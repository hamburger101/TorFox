# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Stage 2 browsing: address bar and DuckDuckGo, tabs, navigation controls,
keyboard shortcuts, find, zoom, error pages, and TorFox's own menus.

No test touches the internet: DuckDuckGo and other public names resolve to
nothing (test host rules), so a search shows up as a failed load whose URL
the test checks."""

from __future__ import annotations

import pathlib
import shutil
import struct
import tempfile
import time
import unittest
import urllib.parse
import zlib

from harness import (ALT, CTRL, SHIFT, Fixture, TorFox, bridge_call, click_element, press,
                     shell_state, wait_for, wait_state)

ONION = "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion"
HOST_RULES = "MAP duckduckgo.com ~NOTFOUND, MAP example.com ~NOTFOUND, MAP unreachable.test ~NOTFOUND"


def page_html(title: str, body: str) -> str:
    return f"<!doctype html><title>{title}</title><body>{body}</body>"


def png_pixel() -> bytes:
    """A valid 1x1 opaque PNG."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    header = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    pixels = zlib.compress(b"\x00\x20\x60\xc0")
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", pixels) + chunk(b"IEND", b"")


class SearchEngineTest(unittest.TestCase):
    """Settings > Search engine: each choice is used at once, by the address
    bar and the new tab page."""

    ENGINES = (("duckduckgo-html", "https://html.duckduckgo.com/html/?q="),
               ("duckduckgo-lite", "https://lite.duckduckgo.com/lite/?q="),
               ("duckduckgo", "https://duckduckgo.com/?q="))

    def test_the_chosen_engine_gets_the_search(self) -> None:
        rules = HOST_RULES + ", MAP html.duckduckgo.com ~NOTFOUND, MAP lite.duckduckgo.com ~NOTFOUND"
        with TorFox(host_rules=rules) as tf:
            shell = tf.shell()
            self.assertTrue(bridge_call(shell, "app.openSettings")["ok"])
            settings = tf.page_at("torfox://settings/")
            for engine, prefix in self.ENGINES:
                r = bridge_call(settings, "settings.set", {"key": "search.engine", "value": engine})
                self.assertTrue(r["ok"], r)
                # From the address bar of a new tab...
                self.assertTrue(bridge_call(shell, "tab.new")["ok"])
                self.assertTrue(bridge_call(shell, "nav.go", {"input": "privacy browser"})["ok"])
                wait_state(shell, lambda s: s["nav"].get("url") == prefix + "privacy%20browser",
                           f"{engine} search")
            # ...and from the new tab page's own search box.
            before = {t["targetId"] for t in tf.browser.targets()}
            self.assertTrue(bridge_call(shell, "tab.new")["ok"])
            page = tf.page(lambda t: t["url"] == "torfox://newtab/" and t.get("targetId") not in before, 10)
            self.assertTrue(bridge_call(settings, "settings.set",
                                        {"key": "search.engine", "value": "duckduckgo-lite"})["ok"])
            # Typed and submitted like a user would (the page is replaced by
            # the search, so no reply would come back from a bridge call).
            page.evaluate("""(() => { const r = document.querySelector('tf-newtab').shadowRoot;
                r.querySelector('#q').value = 'lite words';
                r.querySelector('form').requestSubmit(); return true; })()""", user_gesture=True)
            wait_state(shell, lambda s: s["nav"].get("url") == "https://lite.duckduckgo.com/lite/?q=lite%20words",
                       "new tab page search")


class BrowsingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()
        f = cls.fixture
        cls.one = f.html("/one", page_html("Page one", "<p>one</p>"))
        cls.two = f.html("/two", page_html("Page two", "<p>two</p>"))
        cls.three = f.html("/three", page_html("Page three", "<p>three</p>"))
        cls.links = f.html("/links", page_html("Links", f"""
            <a id="blank" href="{cls.two}" target="_blank" style="display:block;padding:20px">blank</a>
            <a id="plain" href="{cls.three}" style="display:block;padding:20px">plain</a>
            <p><span id="sel">privacy browser</span></p>
            <p><span id="onion">see {ONION} now</span></p>
            <p><span id="onion2">abc.ONION</span></p>
            <img id="img" src="/pixel.png" width="40" height="40">
            <div id="empty" style="height:200px"></div>"""))
        f.raw("/pixel.png", 200, {"Content-Type": "image/png"}, png_pixel())
        cls.needles = f.html("/needles", page_html("Needles", "needle hay needle hay NEEDLE"))
        cls.menu_dir = pathlib.Path(tempfile.mkdtemp(prefix="torfox-menu-"))
        cls.choice = cls.menu_dir / "choice.txt"
        cls.tf = TorFox(start_url=cls.one, host_rules=HOST_RULES,
                        extra_args=[f"--torfox-test-menu-choice={cls.choice}"]).start()
        cls.shell = cls.tf.shell()
        wait_state(cls.shell, lambda s: s["nav"].get("url") == cls.one and not s["nav"].get("loading"),
                   "the start page")

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            cls.tf.__exit__(None, None, None)
        finally:
            cls.fixture.__exit__(None, None, None)
            shutil.rmtree(cls.menu_dir, ignore_errors=True)

    # ----- helpers -----

    def setUp(self) -> None:
        # Every test starts from one fresh tab (empty history) showing page one,
        # and with no other web page targets left to attach to by mistake.
        old = [t["id"] for t in shell_state(self.shell)["tabs"]]
        bridge_call(self.shell, "tab.new")
        wait_state(self.shell, lambda s: s["activeId"] not in old and len(s["tabs"]) == len(old) + 1,
                   "fresh tab")
        for tab_id in old:
            bridge_call(self.shell, "tab.close", {"id": tab_id})
        wait_state(self.shell, lambda s: len(s["tabs"]) == 1, "only the fresh tab")
        wait_for(lambda: not self.web_targets(), 15, "old web tabs to close")
        self.go(self.one)
        self.active_url(self.one)
        self.layouts = len(self.tf.events("shell_layout"))

    def web_targets(self) -> list[dict]:
        return [t for t in self.tf.browser.targets()
                if t.get("type") == "page" and not t["url"].startswith("torfox://")]

    def go(self, text: str) -> dict:
        result = bridge_call(self.shell, "nav.go", {"input": text})
        self.assertTrue(result["ok"], result)
        return result

    def active_url(self, url: str, timeout: float = 15.0) -> dict:
        return wait_state(self.shell, lambda s: s["nav"].get("url") == url and not s["nav"].get("loading"),
                          f"active tab at {url}", timeout)

    def tab_urls(self) -> list[str]:
        return [t["url"] for t in shell_state(self.shell)["tabs"]]

    def web_page(self, url: str):
        return self.tf.page_at(url)

    def choose(self, label: str) -> None:
        self.choice.write_text(label, encoding="utf-8")

    def last_menu(self, kind: str, after: int) -> dict:
        def found():
            events = [e for e in self.tf.events("menu_shown", kind=kind)]
            return events[after] if len(events) > after else None
        return wait_for(found, 10, f"{kind} menu")

    # ----- address bar and DuckDuckGo -----

    def test_search_goes_to_duckduckgo(self) -> None:
        self.go("privacy browser")
        self.active_url("https://duckduckgo.com/?q=privacy%20browser")

    def test_search_encodes_reserved_characters(self) -> None:
        self.go("a&b=c #1 ?")
        state = self.active_url("https://duckduckgo.com/?q=" + urllib.parse.quote("a&b=c #1 ?", safe=""))
        self.assertNotIn("&b=", state["nav"]["url"])

    def test_known_tld_becomes_https(self) -> None:
        self.go("example.com/path")
        self.active_url("https://example.com/path")

    def test_loopback_host_uses_http(self) -> None:
        self.go(f"127.0.0.1:{self.fixture.port}/two")
        self.active_url(self.two)
        wait_state(self.shell, lambda s: s["tabs"][0]["title"] == "Page two", "page title")

    def test_onion_input_is_never_searched_or_loaded(self) -> None:
        # A valid onion address opens a Tor window (tests/e2e/test_tor.py);
        # these are refused without loading or searching anything.
        before = len(self.tf.events("load_start"))
        typo = ONION[:30] + ("b" if ONION[30] == "a" else "a") + ONION[31:]
        for text in (typo, f"http://{typo}/", f"look up {ONION}"):
            result = bridge_call(self.shell, "nav.go", {"input": text})
            self.assertFalse(result["ok"], text)
            self.assertTrue(result["message"], text)
        time.sleep(0.5)
        self.assertFalse(self.tf.events("window_created", tor=True))
        self.assertEqual(len(self.tf.events("load_start")), before)
        self.assertEqual(shell_state(self.shell)["nav"]["url"], self.one)

    def test_scripts_cannot_be_typed(self) -> None:
        for text in ("javascript:alert(1)", "data:text/html,<b>x</b>", "file:///C:/Windows/win.ini"):
            self.assertFalse(bridge_call(self.shell, "nav.go", {"input": text})["ok"], text)

    # ----- navigation controls -----

    def test_back_forward_reload_home(self) -> None:
        self.go(self.two)
        self.active_url(self.two)
        state = wait_state(self.shell, lambda s: s["nav"]["canGoBack"], "can go back")
        self.assertFalse(state["nav"]["canGoForward"])
        bridge_call(self.shell, "nav.back")
        self.active_url(self.one)
        wait_state(self.shell, lambda s: s["nav"]["canGoForward"], "can go forward")
        bridge_call(self.shell, "nav.forward")
        self.active_url(self.two)
        requests = self.fixture.requests.count("GET /two")
        bridge_call(self.shell, "nav.reload")
        wait_for(lambda: self.fixture.requests.count("GET /two") > requests, 10, "reload request")
        self.assertTrue(bridge_call(self.shell, "nav.home")["ok"])
        state = self.active_url("torfox://newtab/")
        self.assertTrue(state["nav"]["internal"])

    def test_error_page_is_shown_and_not_searched(self) -> None:
        url = "http://unreachable.test/page"
        errors = len(self.tf.events("load_error"))
        self.go(url)
        wait_for(lambda: len(self.tf.events("load_error")) > errors, 15, "load error")
        state = self.active_url(url)
        self.assertFalse(state["nav"]["internal"])  # Still the web tab, at the typed URL.
        # Chromium's own error page, with no links to the network.
        page = self.tf.page(lambda t: t["url"] in (url, "chrome-error://chromewebdata/"))
        text = page.evaluate("document.body.innerText")
        self.assertIn("unreachable.test", text)
        self.assertEqual(page.evaluate("document.querySelectorAll('a[href^=\"http\"]').length"), 0)

    # ----- tabs -----

    def test_new_close_and_reopen_tabs(self) -> None:
        self.go(self.two)
        self.active_url(self.two)
        bridge_call(self.shell, "tab.new")
        state = wait_state(self.shell, lambda s: len(s["tabs"]) == 2, "two tabs")
        self.assertEqual(state["nav"]["url"], "torfox://newtab/")
        newtab = state["activeId"]
        bridge_call(self.shell, "nav.go", {"input": self.three})
        self.active_url(self.three)
        bridge_call(self.shell, "tab.close", {"id": newtab})
        state = wait_state(self.shell, lambda s: len(s["tabs"]) == 1, "one tab")
        self.assertEqual(state["nav"]["url"], self.two)
        bridge_call(self.shell, "tab.reopen")
        state = wait_state(self.shell, lambda s: len(s["tabs"]) == 2, "reopened")
        self.assertEqual(state["tabs"][1]["url"], self.three)
        self.assertEqual(state["activeId"], state["tabs"][1]["id"])
        self.active_url(self.three)

    def test_reorder_tabs(self) -> None:
        for url in (self.two, self.three):
            bridge_call(self.shell, "tab.new")
            wait_state(self.shell, lambda s: s["nav"].get("url") == "torfox://newtab/", "new tab")
            self.go(url)
            self.active_url(url)
        ids = [t["id"] for t in shell_state(self.shell)["tabs"]]
        self.assertTrue(bridge_call(self.shell, "tab.move", {"id": ids[2], "index": 0})["ok"])
        state = wait_state(self.shell, lambda s: [t["id"] for t in s["tabs"]] == [ids[2], ids[0], ids[1]],
                           "moved to the front")
        self.assertEqual(state["activeId"], ids[2])
        # Past the end clamps to the end.
        bridge_call(self.shell, "tab.move", {"id": ids[2], "index": 99})
        wait_state(self.shell, lambda s: [t["id"] for t in s["tabs"]] == ids, "moved to the end")
        self.assertFalse(bridge_call(self.shell, "tab.move", {"id": 999, "index": 0})["ok"])

    def test_drag_a_tab_to_reorder(self) -> None:
        bridge_call(self.shell, "tab.new")
        wait_state(self.shell, lambda s: len(s["tabs"]) == 2, "two tabs")
        ids = [t["id"] for t in shell_state(self.shell)["tabs"]]
        rects = self.shell.evaluate("""[...document.querySelector('tf-shell').shadowRoot
            .querySelectorAll('.tab')].map(e => { const r = e.getBoundingClientRect();
            return [r.left + 20, r.top + r.height / 2, r.right - 10]; })""")
        (x1, y1, _), (_, _, right2) = rects
        self.shell.send("Input.dispatchMouseEvent", {"type": "mouseMoved", "x": x1, "y": y1})
        self.shell.send("Input.dispatchMouseEvent", {"type": "mousePressed", "x": x1, "y": y1,
                                                     "button": "left", "clickCount": 1})
        for step in range(1, 11):
            x = x1 + (right2 - x1) * step / 10
            self.shell.send("Input.dispatchMouseEvent", {"type": "mouseMoved", "x": x, "y": y1,
                                                         "button": "left", "buttons": 1})
        self.shell.send("Input.dispatchMouseEvent", {"type": "mouseReleased", "x": right2, "y": y1,
                                                     "button": "left", "clickCount": 1})
        wait_state(self.shell, lambda s: [t["id"] for t in s["tabs"]] == [ids[1], ids[0]], "dragged")

    def test_keyboard_shortcuts_for_tabs(self) -> None:
        tab = self.web_page(self.one)
        press(tab, "t", CTRL)
        wait_state(self.shell, lambda s: len(s["tabs"]) == 2, "Ctrl+T")
        press(self.shell, "t", CTRL)
        wait_state(self.shell, lambda s: len(s["tabs"]) == 3, "Ctrl+T in the shell")
        ids = [t["id"] for t in shell_state(self.shell)["tabs"]]
        press(self.shell, "1", CTRL)
        wait_state(self.shell, lambda s: s["activeId"] == ids[0], "Ctrl+1")
        press(self.shell, "9", CTRL)
        wait_state(self.shell, lambda s: s["activeId"] == ids[2], "Ctrl+9 (last)")
        press(self.shell, "tab", CTRL)
        wait_state(self.shell, lambda s: s["activeId"] == ids[0], "Ctrl+Tab wraps")
        press(self.shell, "tab", CTRL | SHIFT)
        wait_state(self.shell, lambda s: s["activeId"] == ids[2], "Ctrl+Shift+Tab")
        press(self.shell, "pageup", CTRL | SHIFT)
        wait_state(self.shell, lambda s: [t["id"] for t in s["tabs"]] == [ids[0], ids[2], ids[1]],
                   "Ctrl+Shift+PageUp moves the tab")
        press(self.shell, "w", CTRL)
        wait_state(self.shell, lambda s: len(s["tabs"]) == 2 and ids[2] not in [t["id"] for t in s["tabs"]],
                   "Ctrl+W")
        press(self.shell, "t", CTRL | SHIFT)
        wait_state(self.shell, lambda s: len(s["tabs"]) == 3, "Ctrl+Shift+T")

    def test_keyboard_shortcuts_for_navigation(self) -> None:
        self.go(self.two)
        self.active_url(self.two)
        tab = self.web_page(self.two)
        press(tab, "left", ALT)
        self.active_url(self.one)
        tab = self.web_page(self.one)
        press(tab, "right", ALT)
        self.active_url(self.two)
        requests = self.fixture.requests.count("GET /two")
        press(self.web_page(self.two), "f5")
        wait_for(lambda: self.fixture.requests.count("GET /two") > requests, 10, "F5 reload")
        press(self.web_page(self.two), "home", ALT)
        self.active_url("torfox://newtab/")

    def test_link_with_target_blank_opens_next_to_its_tab(self) -> None:
        bridge_call(self.shell, "tab.new")  # A tab to the right of the opener.
        state = wait_state(self.shell, lambda s: len(s["tabs"]) == 2, "two tabs")
        bridge_call(self.shell, "tab.select", {"id": state["tabs"][0]["id"]})
        self.go(self.links)
        self.active_url(self.links)
        page = self.web_page(self.links)
        click_element(page, "#blank")
        state = wait_state(self.shell, lambda s: len(s["tabs"]) == 3, "link tab")
        self.assertEqual(state["tabs"][1]["url"], self.two)  # Next to its opener, not at the end.
        self.assertEqual(state["activeId"], state["tabs"][1]["id"])

    # ----- find -----

    def test_find_in_page(self) -> None:
        self.go(self.needles)
        self.active_url(self.needles)
        tab = self.web_page(self.needles)
        press(tab, "f", CTRL)
        wait_state(self.shell, lambda s: s["findOpen"], "find bar open")
        layout = self.new_layout()
        self.assertEqual(layout["height"], 122)
        self.assertEqual(layout["shell"][1], 122)
        self.assertEqual(layout["content"][0], 122)  # The page moved down; nothing covers it.
        results = len(self.tf.events("find_result"))
        self.assertTrue(bridge_call(self.shell, "find.query", {"text": "needle"})["ok"])
        # Chromium may report the count before it picks the active match.
        wait_for(lambda: [e for e in self.tf.events("find_result")[results:]
                          if (e["count"], e["active"]) == (3, 1)], 10,
                 "3 matches, the first active (case-insensitive by default)")
        state = wait_state(self.shell, lambda s: s["findCount"] == 3, "count shown")
        self.assertEqual(state["findActive"], 1)
        press(self.shell, "f3")
        wait_for(lambda: self.tf.events("find_result")[-1]["active"] == 2, 10, "F3 finds the next match")
        press(self.shell, "f3", SHIFT)
        wait_for(lambda: self.tf.events("find_result")[-1]["active"] == 1, 10, "Shift+F3 goes back")
        bridge_call(self.shell, "find.query", {"text": "needle", "matchCase": True})
        wait_for(lambda: self.tf.events("find_result")[-1]["count"] == 2, 10, "match case")
        bridge_call(self.shell, "find.query", {"text": "nothing-like-this"})
        wait_for(lambda: self.tf.events("find_result")[-1]["count"] == 0, 10, "no match")
        bridge_call(self.shell, "find.stop")
        wait_state(self.shell, lambda s: not s["findOpen"], "find bar closed")
        layout = self.new_layout()
        self.assertEqual((layout["height"], layout["content"][0]), (82, 82))

    def new_layout(self) -> dict:
        def found():
            events = self.tf.events("shell_layout")
            return events[self.layouts] if len(events) > self.layouts else None
        layout = wait_for(found, 10, "shell layout")
        self.layouts += 1
        return layout

    def test_find_bar_closes_when_switching_tabs(self) -> None:
        press(self.web_page(self.one), "f", CTRL)
        wait_state(self.shell, lambda s: s["findOpen"], "find bar open")
        bridge_call(self.shell, "tab.new")
        wait_state(self.shell, lambda s: not s["findOpen"] and len(s["tabs"]) == 2, "closed on switch")

    # ----- zoom -----

    def test_zoom(self) -> None:
        tab = self.web_page(self.one)
        base = tab.evaluate("window.devicePixelRatio")
        press(tab, "equals", CTRL)
        state = wait_state(self.shell, lambda s: s["nav"]["zoom"] == 110, "110%")
        wait_for(lambda: abs(tab.evaluate("window.devicePixelRatio") - base * 1.1) < 0.01, 10,
                 "page zoomed")
        press(tab, "equals", CTRL)
        wait_state(self.shell, lambda s: s["nav"]["zoom"] == 125, "125%")
        press(tab, "minus", CTRL)
        press(tab, "minus", CTRL)
        press(tab, "minus", CTRL)
        wait_state(self.shell, lambda s: s["nav"]["zoom"] == 90, "90%")
        bridge_call(self.shell, "zoom.change", {"step": 0})
        wait_state(self.shell, lambda s: s["nav"]["zoom"] == 100, "reset")
        wait_for(lambda: abs(tab.evaluate("window.devicePixelRatio") - base) < 0.01, 10, "unzoomed")
        self.assertFalse(bridge_call(self.shell, "zoom.change", {"step": 5})["ok"])

    # ----- menus -----

    def test_web_context_menu_has_no_save_or_inspect(self) -> None:
        self.go(self.links)
        self.active_url(self.links)
        page = self.web_page(self.links)
        menus = {}
        for selector in ("#plain", "#img", "#empty"):
            seen = len(self.tf.events("menu_shown", kind="web_context"))
            self.choose("")  # Dismiss.
            click_element(page, selector, button="right")
            menu = self.last_menu("web_context", seen)
            labels = [i["label"] for i in menu["items"]]
            menus[selector] = labels
            for banned in ("save", "print", "source", "inspect", "download"):
                self.assertFalse([l for l in labels if banned in l.lower()], (selector, labels))
        self.assertEqual(menus["#plain"][:4], ["Open link in new tab", "Open link in private window",
                                               "Open link in Tor window", "Copy link address"])
        self.assertEqual(menus["#img"][:2], ["Open image in new tab", "Copy image address"])
        self.assertEqual(menus["#empty"][:3], ["Back", "Forward", "Reload"])

    def test_context_menu_opens_link_in_new_tab(self) -> None:
        self.go(self.links)
        self.active_url(self.links)
        page = self.web_page(self.links)
        seen = len(self.tf.events("menu_shown", kind="web_context"))
        self.choose("Open link in new tab")
        click_element(page, "#plain", button="right")
        self.last_menu("web_context", seen)
        state = wait_state(self.shell, lambda s: len(s["tabs"]) == 2, "link opened")
        self.assertEqual(state["tabs"][1]["url"], self.three)
        self.assertEqual(state["nav"]["url"], self.links)  # Opened in the background.

    def select_and_open_menu(self, page, selector: str, choice: str) -> dict:
        page.evaluate(f"getSelection().selectAllChildren(document.querySelector({selector!r})); 1")
        seen = len(self.tf.events("menu_shown", kind="web_context"))
        self.choose(choice)
        click_element(page, selector, button="right")
        return self.last_menu("web_context", seen)

    def test_selection_search_uses_duckduckgo(self) -> None:
        self.go(self.links)
        self.active_url(self.links)
        page = self.web_page(self.links)
        menu = self.select_and_open_menu(page, "#sel", "Search DuckDuckGo for \u201cprivacy browser\u201d")
        self.assertGreaterEqual(menu["picked"], 0, menu)
        state = wait_state(self.shell, lambda s: len(s["tabs"]) == 2, "search tab")
        self.assertEqual(state["tabs"][1]["url"], "https://duckduckgo.com/?q=privacy%20browser")

    def test_onion_selections_are_never_searched(self) -> None:
        self.go(self.links)
        self.active_url(self.links)
        page = self.web_page(self.links)
        for selector in ("#onion", "#onion2"):
            menu = self.select_and_open_menu(page, selector, "")
            labels = [i["label"] for i in menu["items"]]
            self.assertIn("Copy", labels)
            self.assertFalse([l for l in labels if "DuckDuckGo" in l], labels)

    def test_app_menu(self) -> None:
        seen = len(self.tf.events("menu_shown", kind="app"))
        self.choose("New tab")
        self.assertTrue(bridge_call(self.shell, "app.menu", {"x": 1200, "y": 80})["ok"])
        menu = self.last_menu("app", seen)
        labels = [i["label"] for i in menu["items"]]
        for expected in ("New tab", "New window", "New private window", "History", "Bookmarks",
                         "Show bookmarks bar", "Zoom in", "Zoom out", "Find in page", "Settings"):
            self.assertIn(expected, labels)
        wait_state(self.shell, lambda s: len(s["tabs"]) == 2, "New tab from the menu")


if __name__ == "__main__":
    unittest.main()
