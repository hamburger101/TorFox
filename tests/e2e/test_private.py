# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Stage 2 private windows: each one is its own off-the-record context,
isolated from normal windows and from other private windows, and nothing a
private window visits is written to disk (history, cookies, storage, cache)."""

from __future__ import annotations

import ctypes
import json
import pathlib
import shutil
import tempfile
import unittest

from harness import CTRL, SHIFT, Fixture, TorFox, _top_level_windows, bridge_call, click_element, \
    press, shell_state, wait_for, wait_state

# Unique strings that can only reach a file through the page that uses them.
PRIVATE_MARK = "pvmark-a8c3q"
NORMAL_MARK = "nmmark-b7d2r"
SKIP = ("test-log.jsonl", "cef.log", "DevToolsActivePort")

STORAGE_PAGE = """<!doctype html><title>Storage {mark}</title><body><script>
document.cookie = "who={mark}; max-age=86400; path=/";
localStorage.setItem("who", "{mark}");
const open = indexedDB.open("db-{mark}", 1);
open.onupgradeneeded = () => open.result.createObjectStore("s");
open.onsuccess = () => {{
  const tx = open.result.transaction("s", "readwrite");
  tx.objectStore("s").put("{mark}", "who");
  tx.oncomplete = () => {{ document.title = "Stored {mark}"; }};
}};
</script></body>"""


def files_containing(root: pathlib.Path, needle: str) -> list[str]:
    patterns = (needle.encode("utf-8"), needle.encode("utf-16-le"))
    found = []
    for path in root.rglob("*"):
        if path.is_file() and path.name not in SKIP:
            try:
                data = path.read_bytes()
            except OSError as e:
                found.append(f"{path.relative_to(root)} (unreadable: {e})")
                continue
            if any(p in data for p in patterns):
                found.append(str(path.relative_to(root)))
    return found


def window_titles(pid: int) -> list[str]:
    titles = []
    for hwnd in _top_level_windows(pid):
        buf = ctypes.create_unicode_buffer(512)
        ctypes.windll.user32.GetWindowTextW(hwnd, buf, 512)
        titles.append(buf.value)
    return titles


class PrivateWindowTest(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = Fixture().__enter__()
        self.addCleanup(self.fixture.__exit__, None, None, None)
        port = self.fixture.port
        self.fixture.html("/private", STORAGE_PAGE.format(mark=PRIVATE_MARK))
        self.fixture.html("/normal", STORAGE_PAGE.format(mark=NORMAL_MARK))
        self.fixture.html("/plain", "<!doctype html><title>Plain</title><p>plain</p>")
        self.fixture.html("/shared", "<!doctype html><title>Shared</title>"
                          "<a id='link' href='/plain' style='display:block;padding:20px'>x</a>")
        self.private_url = f"http://{PRIVATE_MARK}.test:{port}/private"
        self.normal_url = f"http://{NORMAL_MARK}.test:{port}/normal"
        self.shared = f"http://shared-site.test:{port}/shared"
        self.plain = f"http://shared-site.test:{port}/plain"
        self.user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-private-"))
        self.addCleanup(shutil.rmtree, self.user_data, True)
        self.choice = self.user_data.parent / f"{self.user_data.name}-menu.txt"
        self.addCleanup(lambda: self.choice.unlink(missing_ok=True))
        rules = ", ".join(f"MAP {h}.test 127.0.0.1" for h in (PRIVATE_MARK, NORMAL_MARK, "shared-site"))
        self.tf = TorFox(user_data=self.user_data, host_rules=rules,
                         extra_args=[f"--torfox-test-menu-choice={self.choice}"]).start()
        self.addCleanup(self.tf.cleanup)
        self.normal = self.tf.shell()

    def open_private(self, via_shell=None):
        known = self.tf.shell_ids()
        created = len(self.tf.events("window_created", private=True))
        press(via_shell or self.normal, "n", CTRL | SHIFT)
        wait_for(lambda: len(self.tf.events("window_created", private=True)) > created, 15,
                 "private window")
        shell = self.tf.new_shell(known)
        wait_state(shell, lambda s: s["private"], "private shell state")
        return shell

    def go(self, shell, url: str) -> None:
        self.assertTrue(bridge_call(shell, "nav.go", {"input": url})["ok"])
        wait_state(shell, lambda s: s["nav"]["url"] == url and not s["nav"]["loading"], url)

    def web_page(self, url: str, exclude: set[str] = frozenset()):
        # page() waits past the browser's initial about:blank document.
        session = self.tf.page(lambda t: t["url"] == url and t.get("targetId") not in exclude)
        return session, session.target_id

    def stored(self, session, mark: str) -> None:
        wait_for(lambda: session.evaluate("document.title") == f"Stored {mark}", 15, "storage written")

    # -----

    def test_private_window_is_marked_and_records_nothing(self) -> None:
        shell = self.open_private()
        self.assertEqual(self.tf.events("context_ready", kind="private")[-1]["kind"], "private")
        state = shell_state(shell)
        self.assertTrue(state["private"])
        self.assertFalse(shell_state(self.normal)["private"])
        badge = shell.evaluate("!!document.querySelector('tf-shell').shadowRoot.querySelector('.private-badge')")
        self.assertTrue(badge)
        self.go(shell, self.private_url)
        loaded = self.tf.wait_log("page_loaded", private=True)
        self.assertFalse(loaded["recorded"])
        wait_for(lambda: any("TorFox Private" in t for t in window_titles(self.tf.proc.pid)), 10,
                 "private window title")
        # The normal window's History page shows nothing from it.
        self.assertTrue(bridge_call(self.normal, "app.openHistory")["ok"])
        history = self.tf.page_at("torfox://history/")
        reply = json.loads(bridge_call(history, "history.list", {})["response"])
        self.assertEqual(reply["entries"], [])

    def test_storage_is_isolated_between_windows(self) -> None:
        # Same origin in three windows: normal, private A, private B.
        script_set = "document.cookie = 'k=' + %s + '; path=/'; localStorage.setItem('k', %s); 1"
        script_get = "[document.cookie, localStorage.getItem('k')]"
        self.go(self.normal, self.shared)
        normal_page, normal_id = self.web_page(self.shared)
        normal_page.evaluate(script_set % ('"normal"', '"normal"'))

        private_a = self.open_private()
        self.go(private_a, self.shared)
        page_a, id_a = self.web_page(self.shared, {normal_id})
        self.assertEqual(page_a.evaluate(script_get), ["", None])
        page_a.evaluate(script_set % ('"a"', '"a"'))

        private_b = self.open_private()
        self.go(private_b, self.shared)
        page_b, _ = self.web_page(self.shared, {normal_id, id_a})
        self.assertEqual(page_b.evaluate(script_get), ["", None])
        page_b.evaluate(script_set % ('"b"', '"b"'))

        self.assertEqual(normal_page.evaluate(script_get), ["k=normal", "normal"])
        self.assertEqual(page_a.evaluate(script_get), ["k=a", "a"])
        self.assertEqual(page_b.evaluate(script_get), ["k=b", "b"])

    def test_private_tabs_and_links_stay_private(self) -> None:
        shell = self.open_private()
        bridge_call(shell, "tab.new")
        state = wait_state(shell, lambda s: len(s["tabs"]) == 2, "second private tab")
        self.go(shell, self.shared)
        page, _ = self.web_page(self.shared)
        page.evaluate("document.cookie = 'k=private; path=/'; 1")
        click_element(page, "#link", modifiers=CTRL)  # Ctrl+click: a new tab in the same window.
        state = wait_state(shell, lambda s: len(s["tabs"]) == 3, "link tab")
        self.assertEqual(len(shell_state(self.normal)["tabs"]), 1)
        link_url = self.plain
        link_page, _ = self.web_page(link_url)
        self.assertEqual(link_page.evaluate("document.cookie"), "k=private")  # Same private context.

    def test_open_link_in_private_window_from_a_normal_window(self) -> None:
        self.go(self.normal, self.shared)
        page, _ = self.web_page(self.shared)
        page.evaluate("document.cookie = 'k=normal; path=/'; 1")
        known = self.tf.shell_ids()
        self.choice.write_text("Open link in private window", encoding="utf-8")
        click_element(page, "#link", button="right")
        shell = self.tf.new_shell(known)
        link_url = self.plain
        wait_state(shell, lambda s: s["private"] and s["nav"]["url"] == link_url, "private link window")
        link_page, _ = self.web_page(link_url)
        self.assertEqual(link_page.evaluate("document.cookie"), "")

    def test_nothing_from_a_private_window_reaches_the_disk(self) -> None:
        # Control: the same page in a normal window does reach the disk, so the
        # search below would find a private leak.
        self.go(self.normal, self.normal_url)
        normal_page, normal_id = self.web_page(self.normal_url)
        self.stored(normal_page, NORMAL_MARK)

        shell = self.open_private()
        self.go(shell, self.private_url)
        private_page, _ = self.web_page(self.private_url, {normal_id})
        self.stored(private_page, PRIVATE_MARK)
        # Close the private window (its shell goes with it, so no bridge reply
        # can come back), then TorFox, so everything is flushed.
        shell.send("Input.dispatchKeyEvent", {"type": "rawKeyDown", "windowsVirtualKeyCode": 0x57,
                                              "nativeVirtualKeyCode": 0x57,
                                              "modifiers": CTRL | SHIFT})  # Ctrl+Shift+W.
        wait_for(lambda: self.tf.events("window_destroyed", private=True), 15, "private window closed")
        self.assertEqual(self.tf.close(), 0)

        normal_hits = files_containing(self.user_data, NORMAL_MARK)
        self.assertTrue(normal_hits, "control failed: the normal window left no trace to find")
        self.assertEqual(files_containing(self.user_data, PRIVATE_MARK), [])


if __name__ == "__main__":
    unittest.main()
