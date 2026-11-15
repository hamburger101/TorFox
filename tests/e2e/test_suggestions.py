# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Stage 9 address-bar suggestions: typing in the address bar lists matching
bookmarks and history entries (never anything from the network) in a list
over the page, which the keyboard and the mouse can pick from. The list is a
page of its own (torfox://suggest/), which can only say which row was
clicked. Tor windows list bookmarks only (test_tor.TorSuggestTest)."""

from __future__ import annotations

import pathlib
import shutil
import tempfile
import unittest

from harness import CTRL, SHIFT, Fixture, TorFox, bridge_call, click, press, shell_state, \
    wait_for, wait_state

BAR = 1  # The bookmarks bar's folder id (src/core/browsing/places.h).

ROWS = """(() => {
  const root = document.querySelector('tf-suggest')?.shadowRoot;
  if (!root) return null;
  return [...root.querySelectorAll('li')].map(li => ({
    text: li.textContent.replace(/\\s+/g, ' ').trim(),
    highlight: li.classList.contains('highlight'),
  }));
})()"""

ADDRESS = "document.querySelector('tf-shell').shadowRoot.querySelector('#address')"
FORM_RECT = ("(() => { const r = document.querySelector('tf-shell').shadowRoot"
             ".querySelector('form.address').getBoundingClientRect();"
             " return [Math.round(r.left), Math.round(r.bottom), Math.round(r.width)]; })()")


def key(session, name: str, code: int, text: str = "") -> None:
    """A key press the page sees as |name| (e.key)."""
    down = {"type": "keyDown" if text else "rawKeyDown", "key": name, "code": name,
            "windowsVirtualKeyCode": code, "nativeVirtualKeyCode": code}
    if text:
        down["text"] = text
    session.send("Input.dispatchKeyEvent", down)
    session.send("Input.dispatchKeyEvent", {"type": "keyUp", "key": name, "code": name,
                                            "windowsVirtualKeyCode": code, "nativeVirtualKeyCode": code})


class SuggestionsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()
        cls.user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-suggest-"))
        cls.tf = TorFox(user_data=cls.user_data).start()
        cls.shell = cls.tf.shell()
        page = lambda path, title: cls.fixture.html(path, f"<!doctype html><title>{title}</title><p>{title}")
        cls.bookmarked = page("/sg-alpha-bookmarked", "Alpha bookmarked")
        cls.visited = page("/sg-alpha-visited", "Alpha visited")
        cls.other = page("/sg-beta", "Beta page")
        cls.target = page("/sg-target", "Target")
        # A visit (history) and a bookmark, through TorFox's own pages.
        for url in (cls.visited, cls.other):
            loaded = len(cls.tf.events("page_loaded"))
            assert bridge_call(cls.shell, "nav.go", {"input": url})["ok"]
            wait_for(lambda: len(cls.tf.events("page_loaded")) > loaded, 15, f"{url} to load")
        assert bridge_call(cls.shell, "app.openBookmarks")["ok"]
        bookmarks = cls.tf.page_at("torfox://bookmarks/")
        r = bridge_call(bookmarks, "bookmarks.add",
                        {"parent": BAR, "title": "Alpha bookmarked", "url": cls.bookmarked})
        assert r["ok"], r

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tf.cleanup()
        cls.fixture.__exit__(None, None, None)
        shutil.rmtree(cls.user_data, ignore_errors=True)

    def setUp(self) -> None:
        # A fresh tab, so that each test types into an address bar of its own.
        old = [t["id"] for t in shell_state(self.shell)["tabs"]]
        bridge_call(self.shell, "tab.new")
        wait_state(self.shell, lambda s: s["activeId"] not in old, "fresh tab")

    # ----- helpers -----

    def mark(self) -> int:
        return len(self.tf.log())

    def since(self, mark: int, event: str, **match) -> list[dict]:
        return [e for e in self.tf.log()[mark:] if e.get("ev") == event and
                all(e.get(k) == v for k, v in match.items())]

    def wait_since(self, mark: int, event: str, timeout: float = 10.0, **match) -> dict:
        return wait_for(lambda: (self.since(mark, event, **match) or [None])[-1], timeout,
                        f"{event} {match}")

    def type_address(self, text: str) -> None:
        press(self.shell, "l", CTRL)  # Focuses and selects the address bar.
        wait_for(lambda: self.shell.evaluate(
            f"document.querySelector('tf-shell').shadowRoot.activeElement === {ADDRESS}"), 10,
            "the address bar focused")
        self.shell.send("Input.insertText", {"text": text})

    def show(self, text: str) -> dict:
        m = self.mark()
        self.type_address(text)
        return self.wait_since(m, "suggest_shown")

    def suggest_page(self):
        return self.tf.page_at("torfox://suggest/")

    def rows(self, expect_count: int | None = None) -> list[dict]:
        page = self.suggest_page()
        return wait_for(lambda: (lambda r: r if r is not None and
                                 (expect_count is None or len(r) == expect_count) else None)(
            page.evaluate(ROWS)), 10, "the suggestion rows")

    # ----- tests -----

    def test_typing_lists_matching_bookmarks_and_history(self) -> None:
        shown = self.show("Alpha")
        self.assertEqual(shown["count"], 2)
        rows = self.rows(2)
        # Bookmarks first; nothing else matches "Alpha".
        self.assertIn("Alpha bookmarked", rows[0]["text"])
        self.assertIn("Alpha visited", rows[1]["text"])
        self.assertFalse(any(r["highlight"] for r in rows))
        # Under the address bar and as wide as it, in the window.
        left, bottom, width = self.shell.evaluate(FORM_RECT)
        x, y, w, h = shown["bounds"]
        self.assertEqual(w, width)
        self.assertGreaterEqual(x, left)
        self.assertGreaterEqual(y, bottom)
        self.assertEqual(h, 10 + 32 * 2)
        # More text that matches nothing hides it.
        m = self.mark()
        self.shell.send("Input.insertText", {"text": "zzqx"})
        self.wait_since(m, "suggest_hidden")
        # The address prefix matches too ("127.0.0.1:<port>/sg-").
        host = self.fixture.origin.split("//", 1)[1]
        self.show(f"{host}/sg-beta")
        self.assertIn("Beta page", self.rows(1)[0]["text"])

    def test_keyboard_picks_a_suggestion(self) -> None:
        self.show("Alpha")
        self.rows(2)
        page = self.suggest_page()
        key(self.shell, "ArrowDown", 0x28)
        key(self.shell, "ArrowDown", 0x28)
        wait_for(lambda: [r["highlight"] for r in page.evaluate(ROWS)] == [False, True], 10,
                 "the second row highlighted")
        key(self.shell, "ArrowUp", 0x26)
        wait_for(lambda: [r["highlight"] for r in page.evaluate(ROWS)] == [True, False], 10,
                 "the first row highlighted")
        # The address bar keeps what was typed and the focus.
        self.assertEqual(self.shell.evaluate(f"{ADDRESS}.value"), "Alpha")
        m = self.mark()
        key(self.shell, "Enter", 0x0D, "\r")
        self.wait_since(m, "suggest_hidden")
        wait_state(self.shell, lambda s: s["nav"]["url"] == self.bookmarked and not s["nav"]["loading"],
                   "the bookmark open in this tab")

    def test_clicking_a_row_opens_it(self) -> None:
        self.show("Alpha")
        self.rows(2)
        page = self.suggest_page()
        tabs = len(shell_state(self.shell)["tabs"])
        x, y = page.evaluate("(() => { const r = document.querySelector('tf-suggest').shadowRoot"
                             ".querySelectorAll('li')[1].getBoundingClientRect();"
                             " return [r.left + r.width / 2, r.top + r.height / 2]; })()")
        m = self.mark()
        click(page, x, y)
        self.assertTrue(self.wait_since(m, "suggest_opened")["ok"])
        self.wait_since(m, "suggest_hidden")
        state = wait_state(self.shell, lambda s: s["nav"]["url"] == self.visited, "the history entry open")
        self.assertEqual(len(state["tabs"]), tabs)  # In the active tab.

    def test_escape_and_switching_tabs_hide_the_list(self) -> None:
        self.show("Alpha")
        m = self.mark()
        key(self.shell, "Escape", 0x1B)
        self.wait_since(m, "suggest_hidden")
        # The first Escape keeps the text; the address bar is still being edited.
        self.assertEqual(self.shell.evaluate(f"{ADDRESS}.value"), "Alpha")
        self.show("Alpha")
        m = self.mark()
        bridge_call(self.shell, "tab.new")
        self.wait_since(m, "suggest_hidden")
        self.show("Alpha")
        m = self.mark()
        # Leaving the address bar (a click in the page) hides it too.
        self.shell.evaluate(f"{ADDRESS}.blur(); 1")
        self.wait_since(m, "suggest_hidden")

    def test_the_list_cannot_do_anything_else(self) -> None:
        self.show("Alpha")
        page = self.suggest_page()
        # Only "which row": no address of its own, no other command.
        self.assertFalse(bridge_call(page, "suggest.open", {"index": 5})["ok"])
        self.assertFalse(bridge_call(page, "suggest.open", {"index": 0, "url": self.target})["ok"])
        for cmd, args in (("nav.go", {"input": self.target}), ("nav.open", {"url": self.target}),
                          ("tab.new", None), ("history.query", {"text": ""}),
                          ("suggest.query", {"text": "a", "x": 0, "y": 0, "width": 10})):
            self.assertFalse(bridge_call(page, cmd, args)["ok"], cmd)
        # No tab can show the list's page, typed or opened.
        r = bridge_call(self.shell, "nav.go", {"input": "torfox://suggest/"})
        self.assertFalse(r["ok"], r)
        state = shell_state(self.shell)
        self.assertNotIn("torfox://suggest/", [t["url"] for t in state["tabs"]])
        # The only page at that address is the list itself.
        self.assertEqual(len([t for t in self.tf.browser.targets()
                              if t["url"].startswith("torfox://suggest/")]), 1)
        # A web page has no bridge at all.
        loaded = len(self.tf.events("page_loaded"))
        bridge_call(self.shell, "nav.go", {"input": self.target})
        wait_for(lambda: len(self.tf.events("page_loaded")) > loaded, 15, "the target page")
        web = self.tf.page(lambda t: t["url"] == self.target)
        self.assertEqual(web.evaluate("typeof window.torfoxQuery"), "undefined")

    def test_the_list_closes_with_its_window(self) -> None:
        known = self.tf.shell_ids()
        created = len(self.tf.events("window_created"))
        self.assertTrue(bridge_call(self.shell, "window.new")["ok"])
        wait_for(lambda: len(self.tf.events("window_created")) > created, 15, "new window")
        second = self.tf.new_shell(known)
        suggest_before = {t["targetId"] for t in self.tf.browser.targets()
                          if t["url"].startswith("torfox://suggest/")}
        m = self.mark()
        press(second, "l", CTRL)
        wait_for(lambda: second.evaluate(
            f"document.querySelector('tf-shell').shadowRoot.activeElement === {ADDRESS}"), 10,
            "the second window's address bar")
        second.send("Input.insertText", {"text": "Alpha"})
        self.wait_since(m, "suggest_shown")
        mine = wait_for(lambda: [t["targetId"] for t in self.tf.browser.targets()
                                 if t["url"].startswith("torfox://suggest/")
                                 and t["targetId"] not in suggest_before], 10, "its own list")
        destroyed = len(self.tf.events("window_destroyed"))
        second.send("Input.dispatchKeyEvent", {"type": "rawKeyDown", "windowsVirtualKeyCode": 0x57,
                                               "nativeVirtualKeyCode": 0x57,
                                               "modifiers": CTRL | SHIFT})  # Ctrl+Shift+W.
        wait_for(lambda: len(self.tf.events("window_destroyed")) > destroyed, 15, "window closed")
        wait_for(lambda: not [t for t in self.tf.browser.targets() if t["targetId"] in mine], 10,
                 "its list gone")
        # The first window's list still works.
        self.show("Alpha")
        self.assertEqual(len(self.rows(2)), 2)


if __name__ == "__main__":
    unittest.main()
