# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Settings > General > On startup, across real restarts of one profile.

"Restore previous tabs" reopens the tabs of TorFox's normal windows, after a
normal exit and after a crash. Private windows are never kept, and nothing is
kept while Clear when TorFox closes > History is on: then, and with the
default "Open a new tab", <user data>/session.json does not exist."""

from __future__ import annotations

import json
import pathlib
import shutil
import tempfile
import time
import unittest

from harness import Fixture, TorFox, bridge_call, shell_state, wait_for, wait_state


def page(title: str) -> str:
    return f"<!doctype html><title>{title}</title><p>{title}</p>"


class StartupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()
        cls.urls = [cls.fixture.html(f"/p{i}", page(f"Page {i}")) for i in range(4)]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.fixture.__exit__(None, None, None)

    def setUp(self) -> None:
        self.user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-startup-"))
        self.addCleanup(shutil.rmtree, self.user_data, True)
        self.session = self.user_data / "session.json"

    # ----- helpers -----

    def start(self, start_url: str | None = None) -> TorFox:
        tf = TorFox(user_data=self.user_data, start_url=start_url).start()
        self.addCleanup(tf.cleanup)
        return tf

    def set(self, tf: TorFox, key: str, value) -> None:
        shell = tf.shell()
        self.assertTrue(bridge_call(shell, "app.openSettings")["ok"])
        settings = tf.page_at("torfox://settings/")
        r = bridge_call(settings, "settings.set", {"key": key, "value": value})
        self.assertTrue(r["ok"], r)
        # Close the settings tab again.
        state = wait_state(shell, lambda s: s["nav"].get("url", "").startswith("torfox://settings"),
                           "settings tab")
        bridge_call(shell, "tab.close", {"id": state["activeId"]})
        wait_state(shell, lambda s: all(not t["url"].startswith("torfox://settings") for t in s["tabs"]),
                   "settings tab closed")

    def open_tabs(self, shell, urls: list[str]) -> None:
        """The window's tabs become |urls| (its first tab loads the first)."""
        first = shell_state(shell)["activeId"]
        self.assertTrue(bridge_call(shell, "nav.go", {"input": urls[0]})["ok"])
        for url in urls[1:]:
            self.assertTrue(bridge_call(shell, "tab.new")["ok"])
            wait_state(shell, lambda s: s["activeId"] != first and s["nav"].get("url", "").startswith("torfox://newtab"),
                       "new tab")
            self.assertTrue(bridge_call(shell, "nav.go", {"input": url})["ok"])
            first = shell_state(shell)["activeId"]
        wait_state(shell, lambda s: [t["url"] for t in s["tabs"]] == urls and
                   not any(t["loading"] for t in s["tabs"]), "tabs loaded")

    def saved(self) -> dict:
        return json.loads(self.session.read_text(encoding="utf-8"))

    def wait_saved(self, tf: TorFox, windows: list[list[str]], active: list[int]) -> None:
        expected = {"format": 1, "windows": [{"tabs": w, "active": a} for w, a in zip(windows, active)]}

        def matches():
            try:
                return self.saved() == expected
            except (OSError, ValueError):
                return False
        wait_for(matches, 10, f"session.json to hold {expected}")

    def tabs_of(self, shell) -> tuple[list[str], int]:
        state = shell_state(shell)
        urls = [t["url"] for t in state["tabs"]]
        active = next(i for i, t in enumerate(state["tabs"]) if t["id"] == state["activeId"])
        return urls, active

    def restored_shells(self, tf: TorFox, count: int) -> list:
        wait_for(lambda: len(tf.events("session_window_restored")) == count, 15, f"{count} restored windows")
        wait_for(lambda: len([t for t in tf.browser.targets() if t["url"] == "torfox://shell/"]) == count,
                 15, "the shells")
        return [tf.page(lambda t, i=i: t["url"] == "torfox://shell/" and t.get("targetId", i) == i)
                for i in sorted(tf.shell_ids())]

    # ----- tests -----

    def test_by_default_a_new_tab_opens_and_nothing_is_kept(self) -> None:
        tf = self.start()
        shell = tf.shell()
        self.open_tabs(shell, self.urls[:2])
        time.sleep(1.5)  # Longer than a session save would wait.
        self.assertFalse(self.session.exists())
        self.assertEqual(tf.close(), 0)
        self.assertFalse(self.session.exists())

        tf = self.start()
        shell = tf.shell()
        wait_state(shell, lambda s: len(s["tabs"]) == 1 and s["tabs"][0]["url"] == "torfox://newtab/",
                   "one new tab")
        self.assertFalse(tf.events("session_restored"))

    def test_restore_reopens_the_tabs_after_a_normal_exit(self) -> None:
        tf = self.start()
        self.set(tf, "general.startupPage", "restore")
        shell = tf.shell()
        self.open_tabs(shell, self.urls[:3])
        self.assertTrue(bridge_call(shell, "tab.select", {"id": shell_state(shell)["tabs"][1]["id"]})["ok"])
        self.wait_saved(tf, [self.urls[:3]], [1])
        self.assertEqual(tf.close(), 0)
        self.assertEqual(self.saved()["windows"][0]["tabs"], self.urls[:3])

        tf = self.start()
        (shell,) = self.restored_shells(tf, 1)
        wait_state(shell, lambda s: [t["url"] for t in s["tabs"]] == self.urls[:3], "restored tabs")
        self.assertEqual(self.tabs_of(shell), (self.urls[:3], 1))
        self.assertEqual(tf.events("session_restored")[0]["tabs"], 3)
        # The restored pages are real, visible pages.
        page1 = tf.page_at(self.urls[1])
        wait_for(lambda: page1.evaluate("document.title") == "Page 1", 10, "page 1")
        self.assertEqual(page1.evaluate("document.visibilityState"), "visible")

    def test_restore_after_a_crash_reopens_every_normal_window(self) -> None:
        tf = self.start()
        self.set(tf, "general.startupPage", "restore")
        first = tf.shell()
        self.open_tabs(first, self.urls[:2])
        known = tf.shell_ids()
        self.assertTrue(bridge_call(first, "window.new")["ok"])
        second = tf.new_shell(known)
        self.open_tabs(second, self.urls[2:4])
        # A private window is never part of the session.
        known = tf.shell_ids()
        self.assertTrue(bridge_call(first, "window.newPrivate")["ok"])
        private = tf.new_shell(known)
        self.assertTrue(bridge_call(private, "nav.go", {"input": self.urls[0] + "?private"})["ok"])
        wait_state(private, lambda s: s["nav"].get("url", "").endswith("?private"), "private page")
        self.wait_saved(tf, [self.urls[:2], self.urls[2:4]], [1, 1])
        time.sleep(1.5)
        self.assertNotIn("?private", self.session.read_text(encoding="utf-8"))
        tf.kill()

        # Started with an address: it opens after the first window's tabs.
        extra = self.fixture.html("/extra", page("Extra"))
        tf = self.start(start_url=extra)
        shells = self.restored_shells(tf, 2)
        wins = []
        for shell in shells:
            wait_state(shell, lambda s: not any(t["loading"] for t in s["tabs"]) and len(s["tabs"]) >= 2,
                       "restored window")
            wins.append(self.tabs_of(shell))
        self.assertCountEqual(wins, [(self.urls[:2] + [extra], 2), (self.urls[2:4], 1)])
        self.assertFalse(tf.events("window_created", private=True))

    def test_nothing_is_kept_while_history_is_cleared_on_exit(self) -> None:
        tf = self.start()
        self.set(tf, "general.startupPage", "restore")
        shell = tf.shell()
        self.open_tabs(shell, self.urls[:2])
        self.wait_saved(tf, [self.urls[:2]], [1])
        # Turning on Clear when TorFox closes > History removes the session
        # at once and keeps none from then on.
        self.set(tf, "privacy.clearOnExit.history", True)
        wait_for(lambda: not self.session.exists(), 5, "session removed")
        self.assertTrue(bridge_call(shell, "nav.go", {"input": self.urls[3]})["ok"])
        time.sleep(1.5)
        self.assertFalse(self.session.exists())
        tf.kill()  # Even after a crash, nothing is reopened.

        tf = self.start()
        shell = tf.shell()
        wait_state(shell, lambda s: len(s["tabs"]) == 1 and s["tabs"][0]["url"] == "torfox://newtab/",
                   "one new tab")
        self.assertFalse(tf.events("session_restored"))

    def test_choosing_a_new_tab_again_removes_the_session(self) -> None:
        tf = self.start()
        self.set(tf, "general.startupPage", "restore")
        shell = tf.shell()
        self.open_tabs(shell, self.urls[:1])
        self.wait_saved(tf, [self.urls[:1]], [0])
        self.set(tf, "general.startupPage", "newtab")
        wait_for(lambda: not self.session.exists(), 5, "session removed")
        self.assertTrue(tf.events("session_removed", why="setting"))

    def test_a_damaged_session_file_opens_a_new_tab(self) -> None:
        self.user_data.mkdir(parents=True, exist_ok=True)
        (self.user_data / "settings.json").write_text(json.dumps(
            {"format": 1, "settings": {"general.startupPage": "restore"}}), encoding="utf-8")
        for text in ("{ not json", json.dumps({"format": 2, "windows": [{"tabs": [self.urls[0]]}]}),
                     json.dumps({"format": 1, "windows": [{"tabs": ["javascript:alert(1)", "torfox://shell/",
                                                                    "file:///C:/Windows/win.ini"]}]})):
            self.session.write_text(text, encoding="utf-8")
            tf = self.start()
            shell = tf.shell()
            wait_state(shell, lambda s: len(s["tabs"]) == 1 and s["tabs"][0]["url"] == "torfox://newtab/",
                       "one new tab")
            self.assertFalse(tf.events("session_restored"), text)
            self.assertEqual(tf.close(), 0)


if __name__ == "__main__":
    unittest.main()
