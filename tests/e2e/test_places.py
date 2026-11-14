# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Stage 2 history and bookmarks: what is recorded (and where), the History
and Bookmarks pages, the star button and bookmarks bar, persistence across
restarts, and that cleared history leaves no trace in TorFox's files."""

from __future__ import annotations

import json
import pathlib
import shutil
import sqlite3
import tempfile
import unittest

from harness import CTRL, SHIFT, Fixture, TorFox, bridge_call, element_center, press, shell_state, \
    wait_for, wait_state

BAR, OTHER = 1, 2  # Root folder ids (src/core/browsing/places.h).


def response(result: dict):
    assert result["ok"], result
    return json.loads(result["response"])


def files_containing(root: pathlib.Path, needle: str, skip: tuple[str, ...] = ()) -> list[str]:
    """Every file under |root| holding |needle| as UTF-8 or UTF-16LE."""
    patterns = (needle.encode("utf-8"), needle.encode("utf-16-le"))
    found = []
    for path in root.rglob("*"):
        if not path.is_file() or path.name in skip:
            continue
        try:
            data = path.read_bytes()
        except OSError:
            continue  # Locked by the running browser; reported by the caller's own checks.
        if any(p in data for p in patterns):
            found.append(str(path.relative_to(root)))
    return found


class PlacesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.fixture.__exit__(None, None, None)

    def setUp(self) -> None:
        self.user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-places-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.user_data, ignore_errors=True)

    def start(self, **kwargs) -> TorFox:
        tf = TorFox(user_data=self.user_data, **kwargs).start()
        self.addCleanup(tf.cleanup)
        return tf

    def page(self, marker: str) -> str:
        return self.fixture.html(f"/visit-{marker}",
                                 f"<!doctype html><title>Title {marker}</title><p>{marker}</p>")

    def visit(self, tf: TorFox, shell, url: str) -> dict:
        loaded = len(tf.events("page_loaded"))
        self.assertTrue(bridge_call(shell, "nav.go", {"input": url})["ok"])
        wait_for(lambda: len(tf.events("page_loaded")) > loaded, 15, f"{url} to load")
        wait_state(shell, lambda s: s["nav"]["url"] == url and not s["nav"]["loading"], "loaded")
        return tf.events("page_loaded")[-1]

    def open_page(self, tf: TorFox, shell, name: str):
        self.assertTrue(bridge_call(shell, f"app.open{name.capitalize()}")["ok"])
        return tf.page_at(f"torfox://{name}/")

    # ----- history -----

    def test_visits_are_recorded_by_torfox_not_chromium(self) -> None:
        url = self.page("hq7m3k")
        tf = self.start()
        shell = tf.shell()
        loaded = self.visit(tf, shell, url)
        self.assertEqual((loaded["private"], loaded["recorded"]), (False, True))
        history = self.open_page(tf, shell, "history")
        entries = response(bridge_call(history, "history.list", {"limit": 10}))["entries"]
        self.assertEqual([(e["url"], e["visits"]) for e in entries], [(url, 1)])
        wait_for(lambda: response(bridge_call(history, "history.list", {}))["entries"][0]["title"]
                 == "Title hq7m3k", 10, "title recorded")
        self.assertEqual(tf.close(), 0)
        # Chromium's own History database never sees the visit.
        chromium_history = self.user_data / "User Data" / "Normal" / "History"
        if chromium_history.exists():
            db = sqlite3.connect(f"file:{chromium_history}?mode=ro&immutable=1", uri=True)
            try:
                self.assertEqual(db.execute("SELECT COUNT(*) FROM urls").fetchone()[0], 0)
                self.assertEqual(db.execute("SELECT COUNT(*) FROM visits").fetchone()[0], 0)
            finally:
                db.close()
        leaks = files_containing(self.user_data / "User Data", "hq7m3k", skip=("cef.log",))
        self.assertEqual(leaks, [], "the page's path was found in Chromium's profile files")

    def test_internal_pages_and_failed_loads_are_not_recorded(self) -> None:
        tf = self.start(host_rules="MAP unreachable.test ~NOTFOUND")
        shell = tf.shell()
        errors = len(tf.events("load_error"))
        self.assertTrue(bridge_call(shell, "nav.go", {"input": "http://unreachable.test/x"})["ok"])
        wait_for(lambda: len(tf.events("load_error")) > errors, 15, "load error")
        wait_for(lambda: tf.events("load_end"), 15, "error page")
        self.assertTrue(bridge_call(shell, "nav.home")["ok"])
        self.assertTrue(bridge_call(shell, "app.openSettings")["ok"])
        history = self.open_page(tf, shell, "history")
        self.assertEqual(response(bridge_call(history, "history.list", {}))["entries"], [])
        self.assertFalse([e for e in tf.events("page_loaded") if e["recorded"]])

    def test_history_page_search_remove_and_clear(self) -> None:
        tf = self.start()
        shell = tf.shell()
        urls = [self.page(m) for m in ("ha1x9", "hb2y8", "hc3z7")]
        for url in urls:
            self.visit(tf, shell, url)
        self.visit(tf, shell, urls[0])  # A second visit to the first page.
        history = self.open_page(tf, shell, "history")
        entries = response(bridge_call(history, "history.list", {}))["entries"]
        self.assertEqual([e["url"] for e in entries], [urls[0], urls[2], urls[1]])  # Most recent first.
        self.assertEqual(entries[0]["visits"], 2)
        found = response(bridge_call(history, "history.list", {"query": "hb2y8"}))["entries"]
        self.assertEqual([e["url"] for e in found], [urls[1]])
        page = response(bridge_call(history, "history.list", {"limit": 1}))
        self.assertTrue(page["more"])
        older = response(bridge_call(history, "history.list",
                                     {"limit": 5, "before": page["entries"][0]["lastVisit"]}))
        self.assertEqual([e["url"] for e in older["entries"]], [urls[2], urls[1]])

        # The page shows what the bridge returns.
        wait_for(lambda: history.evaluate(
            "document.querySelector('tf-history').shadowRoot.querySelectorAll('.row').length") == 3,
            10, "three rows")

        self.assertTrue(bridge_call(history, "history.removeUrl", {"url": urls[1]})["ok"])
        wait_for(lambda: history.evaluate(
            "document.querySelector('tf-history').shadowRoot.querySelectorAll('.row').length") == 2,
            10, "row removed live")
        self.assertEqual(files_containing(self.user_data, "hb2y8", skip=("test-log.jsonl", "cef.log")),
                         [])  # secure_delete + WAL checkpoint: the bytes are gone.

        # Clearing asks first, in the page.
        root = "document.querySelector('tf-history').shadowRoot"
        history.evaluate(f"{root}.querySelector('#clear').click()", user_gesture=True)
        wait_for(lambda: history.evaluate(f"!!{root}.querySelector('#confirm-clear')"), 5, "confirm")
        self.assertEqual(len(response(bridge_call(history, "history.list", {}))["entries"]), 2)
        history.evaluate(f"{root}.querySelector('#confirm-clear').click()", user_gesture=True)
        wait_for(lambda: tf.events("history_removed", what="all"), 10, "history cleared")
        self.assertEqual(response(bridge_call(history, "history.list", {}))["entries"], [])
        for marker in ("ha1x9", "hc3z7"):
            self.assertEqual(files_containing(self.user_data, marker,
                                              skip=("test-log.jsonl", "cef.log")), [], marker)

    def test_clearing_history_forgets_zoomed_sites(self) -> None:
        # Chromium keeps zoom levels and per-site scores in Preferences
        # (STAGE2_FINDINGS.md §5, STAGE4_FINDINGS.md).
        url = self.fixture.html("/zoom", "<!doctype html><title>Zoom</title><p>z</p>").replace(
            "127.0.0.1", "zoomsite-k2p9.test")
        tf = self.start(host_rules="MAP zoomsite-k2p9.test 127.0.0.1")
        shell = tf.shell()
        self.visit(tf, shell, url)
        press(tf.page_at(url), "equals", CTRL)
        wait_state(shell, lambda s: s["nav"]["zoom"] == 110, "zoomed")
        history = self.open_page(tf, shell, "history")
        self.assertTrue(bridge_call(history, "history.clear")["ok"])
        self.assertEqual(tf.close(), 0)
        self.assertEqual(files_containing(self.user_data, "zoomsite-k2p9",
                                          skip=("test-log.jsonl", "cef.log")), [])

    def test_history_page_cannot_run_shell_commands(self) -> None:
        tf = self.start()
        history = self.open_page(tf, tf.shell(), "history")
        for cmd in ("tab.new", "nav.go", "bookmarks.add", "settings.set"):
            result = bridge_call(history, cmd, {})
            self.assertFalse(result["ok"], cmd)

    # ----- bookmarks -----

    def test_star_bookmarks_the_page_and_the_bar_shows_it(self) -> None:
        url = self.page("bs4w6")
        tf = self.start()
        shell = tf.shell()
        self.visit(tf, shell, url)
        state = wait_state(shell, lambda s: s["nav"]["bookmarkable"], "bookmarkable")
        self.assertFalse(state["nav"]["bookmarked"])
        x, y = element_center(shell, "tf-shell")  # Any point; the menu is only for removal.
        self.assertTrue(bridge_call(shell, "bookmarks.star", {"x": int(x), "y": int(y)})["ok"])
        tf.wait_log("bookmark_added", via="star")
        state = wait_state(shell, lambda s: s["nav"]["bookmarked"], "star filled")
        self.assertFalse(state["barVisible"])  # The bar is off by default.

        layouts = len(tf.events("shell_layout"))
        press(shell, "b", CTRL | SHIFT)
        state = wait_state(shell, lambda s: s["barVisible"], "bookmarks bar shown")
        self.assertEqual([(i["title"], i["url"]) for i in state["barItems"]], [("Title bs4w6", url)])
        layout = wait_for(lambda: tf.events("shell_layout")[layouts:], 10, "shell layout")[0]
        self.assertEqual((layout["height"], layout["content"][0]), (114, 114))

        # Starring again offers removal through TorFox's own menu.
        choice = self.user_data / "menu-choice.txt"
        tf.close()
        tf = self.start(extra_args=[f"--torfox-test-menu-choice={choice}"])
        shell = tf.shell()
        state = wait_state(shell, lambda s: s["barVisible"] and s["barItems"], "bar after restart")
        self.assertEqual(state["barItems"][0]["url"], url)
        self.visit(tf, shell, url)
        wait_state(shell, lambda s: s["nav"]["bookmarked"], "still bookmarked")
        choice.write_text("Remove bookmark", encoding="utf-8")
        self.assertTrue(bridge_call(shell, "bookmarks.star", {"x": 10, "y": 10})["ok"])
        menu = tf.wait_log("menu_shown", kind="star")
        self.assertEqual([i["label"] for i in menu["items"]], ["Remove bookmark", "Edit in Bookmarks"])
        tf.wait_log("bookmark_removed", via="star")
        wait_state(shell, lambda s: not s["nav"]["bookmarked"] and not s["barItems"], "removed")

    def test_bookmarks_page_crud_and_persistence(self) -> None:
        tf = self.start()
        shell = tf.shell()
        page = self.open_page(tf, shell, "bookmarks")
        add = lambda **a: bridge_call(page, "bookmarks.add", a)
        folder = response(bridge_call(page, "bookmarks.addFolder", {"parent": BAR, "title": "Reading"}))["id"]
        first = response(add(parent=folder, title="First", url="https://first.example/"))["id"]
        second = response(add(parent=folder, title="Second", url="http://second.example/a?b=c"))["id"]
        response(add(parent=OTHER, title="Elsewhere", url="https://other.example/"))
        for bad in ("javascript:alert(1)", "file:///C:/Windows/win.ini", "torfox://settings/",
                    "data:text/html,x", "https://", "https://a b.example/", "ftp://x.example/"):
            self.assertFalse(add(parent=BAR, title="bad", url=bad)["ok"], bad)
        self.assertFalse(add(parent=first, title="x", url="https://x.example/")["ok"])  # Not a folder.
        self.assertFalse(bridge_call(page, "bookmarks.update",
                                     {"id": first, "title": "x", "url": "javascript:1"})["ok"])

        self.assertTrue(bridge_call(page, "bookmarks.update",
                                    {"id": second, "title": "Second (edited)"})["ok"])
        self.assertTrue(bridge_call(page, "bookmarks.move",
                                    {"id": second, "parent": folder, "index": 0})["ok"])
        self.assertFalse(bridge_call(page, "bookmarks.move",
                                     {"id": folder, "parent": folder, "index": 0})["ok"])  # Into itself.
        children = response(bridge_call(page, "bookmarks.children", {"id": folder}))["items"]
        self.assertEqual([(c["title"], c["url"]) for c in children],
                         [("Second (edited)", "http://second.example/a?b=c"),
                          ("First", "https://first.example/")])
        found = response(bridge_call(page, "bookmarks.search", {"query": "first"}))["items"]
        self.assertEqual([f["id"] for f in found], [first])
        folders = response(bridge_call(page, "bookmarks.folders"))["folders"]
        self.assertEqual([(f["id"], f["depth"]) for f in folders], [(BAR, 0), (folder, 1), (OTHER, 0)])
        self.assertFalse(bridge_call(page, "bookmarks.remove", {"id": BAR})["ok"])  # Roots stay.

        self.assertEqual(tf.close(), 0)

        tf = self.start()
        page = self.open_page(tf, tf.shell(), "bookmarks")
        children = response(bridge_call(page, "bookmarks.children", {"id": folder}))["items"]
        self.assertEqual([c["id"] for c in children], [second, first])
        self.assertTrue(bridge_call(page, "bookmarks.remove", {"id": folder})["ok"])
        self.assertFalse(bridge_call(page, "bookmarks.children", {"id": folder})["ok"])
        bar = response(bridge_call(page, "bookmarks.children", {"id": BAR}))["items"]
        self.assertEqual(bar, [])  # The folder's bookmarks went with it.

    def test_bookmark_opens_from_the_bar_and_page(self) -> None:
        url = self.page("bo5v4")
        tf = self.start()
        shell = tf.shell()
        page = self.open_page(tf, shell, "bookmarks")
        response(bridge_call(page, "bookmarks.add", {"parent": BAR, "title": "Open me", "url": url}))
        tabs = len(shell_state(shell)["tabs"])
        self.assertTrue(bridge_call(page, "nav.open", {"url": url, "newTab": True})["ok"])
        state = wait_state(shell, lambda s: len(s["tabs"]) == tabs + 1, "opened in a new tab")
        self.assertEqual(state["tabs"][-1]["url"], url)
        # In place, the page's own tab becomes the web page; the bridge reply
        # never arrives because that page is gone, so don't wait for it.
        page.evaluate("window.torfoxQuery({request: %s}); 1" % json.dumps(json.dumps(
            {"cmd": "nav.open", "args": {"url": url}})))
        wait_state(shell, lambda s: [t["url"] for t in s["tabs"]].count(url) == 2, "opened in place")
        # nav.open from the shell (the bookmarks bar) opens in the active tab or a new one.
        tabs = len(shell_state(shell)["tabs"])
        self.assertTrue(bridge_call(shell, "nav.open", {"url": url, "newTab": True})["ok"])
        wait_state(shell, lambda s: len(s["tabs"]) == tabs + 1, "new tab")
        for bad in ("javascript:alert(1)", "torfox://settings/", "file:///C:/"):
            self.assertFalse(bridge_call(shell, "nav.open", {"url": bad})["ok"], bad)


if __name__ == "__main__":
    unittest.main()
