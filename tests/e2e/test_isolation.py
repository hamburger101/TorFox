# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Scheme isolation: web content can never reach torfox:// or the bridge.

A hostile page in a web tab tries every route into TorFox's privileged pages.
Each attempt must fail as seen by the page, and the browser's own log must
show that torfox:// never served a single request to a web browser.
"""

from __future__ import annotations

import json
import time
import unittest

from harness import Fixture, TorFox, bridge_call, wait_for

INTERNAL_ROLES = {"shell", "internal_tab"}

ATTACK_PAGE = """<!doctype html>
<meta charset="utf-8"><title>attack</title>
<body>
<a id="link" href="torfox://settings/">settings</a>
<a id="blank" href="torfox://settings/" target="_blank">settings in new tab</a>
<form id="form" method="POST" action="torfox://settings/"><input name="a" value="b"></form>
</body>
"""


def load_probe(kind: str, url: str) -> str:
    """JS that loads |url| as |kind| and resolves to "loaded" or "error"."""
    return """
      new Promise((resolve) => {
        const kind = %s, url = %s;
        let el;
        if (kind === "img") { el = new Image(); }
        else if (kind === "script") { el = document.createElement("script"); }
        else if (kind === "style") { el = document.createElement("link"); el.rel = "stylesheet"; }
        el.onload = () => resolve("loaded");
        el.onerror = () => resolve("error");
        setTimeout(() => resolve("timeout"), 4000);
        if (kind === "style") { el.href = url; } else { el.src = url; }
        document.body.appendChild(el);
      })
    """ % (json.dumps(kind), json.dumps(url))


class SchemeIsolationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()
        cls.attack_url = cls.fixture.html("/attack", ATTACK_PAGE)
        cls.fixture.raw("/redirect-settings", 302, {"Location": "torfox://settings/"})
        cls.tf = TorFox(start_url=cls.attack_url).start()
        cls.shell = cls.tf.shell()
        cls.web = cls.tf.page_at(cls.attack_url)
        cls.web.send("Log.enable")
        # Real bundled asset URLs: if web content could reach torfox:// at all,
        # loading these would succeed.
        cls.shell_script = cls.shell.evaluate("document.querySelector('script[type=module]').src")
        cls.shell_style = cls.shell.evaluate("document.querySelector('link[rel=stylesheet]').src || document.querySelector('link[rel=stylesheet]').href")
        cls.fixture.raw("/redirect-asset", 302, {"Location": cls.shell_script})

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            cls.tf.__exit__(None, None, None)
        finally:
            cls.fixture.__exit__(None, None, None)

    # Helpers -------------------------------------------------------------------
    def web_scheme_requests(self) -> list[dict]:
        return [e for e in self.tf.events("scheme_request") if e.get("role") not in INTERNAL_ROLES]

    def assert_still_on_attack_page(self) -> None:
        time.sleep(0.3)
        self.assertEqual(self.web.evaluate("location.href"), self.attack_url)

    def assert_logged(self, event: str, url_part: str = "torfox:", role: str = "web_tab",
                      timeout: float = 5.0) -> None:
        wait_for(lambda: [e for e in self.tf.events(event, role=role) if url_part in e.get("url", "")],
                 timeout, f"{event} for {url_part}")

    def assert_refused(self, event: str, url: str, timeout: float = 5.0) -> str:
        """Requires positive evidence that |url| was refused, by either layer:
        TorFox's handlers (|event| in the test log) or Chromium itself, which
        will not display a DISPLAY_ISOLATED scheme from a web origin and says so
        on the console. Returns which layer refused it."""
        def refused():
            if [e for e in self.tf.events(event, role="web_tab") if e.get("url") == url]:
                return "torfox"
            self.tf.browser.pump(0.1)
            for ev in self.tf.browser.take_events("Log.entryAdded", self.web.session_id):
                text = ev["params"]["entry"].get("text", "")
                if "Not allowed to load local resource" in text and url in text:
                    return "chromium"
            return None
        return wait_for(refused, timeout, f"refusal of {url}")

    def tearDown(self) -> None:
        # The invariant behind every test: torfox:// served nothing to the web.
        self.assertEqual(self.web_scheme_requests(), [])

    # Sanity: the probe can tell success from failure --------------------------------
    def test_probe_detects_successful_loads(self) -> None:
        self.fixture.raw("/ok.js", 200, {"Content-Type": "text/javascript"}, b"window.okLoaded = 1;")
        self.assertEqual(self.web.evaluate(load_probe("script", self.fixture.origin + "/ok.js")), "loaded")

    def test_assets_are_served_to_internal_pages(self) -> None:
        self.assertTrue(self.shell_script.startswith("torfox://shell/assets/"), self.shell_script)
        ok = [e for e in self.tf.events("scheme_request", role="shell", status=200)
              if e["url"] == self.shell_script]
        self.assertTrue(ok)

    # Subresources ------------------------------------------------------------------
    def test_script_tag(self) -> None:
        self.assertEqual(self.web.evaluate(load_probe("script", self.shell_script)), "error")

    def test_stylesheet(self) -> None:
        self.assertEqual(self.web.evaluate(load_probe("style", self.shell_style)), "error")

    def test_image(self) -> None:
        self.assertEqual(self.web.evaluate(load_probe("img", "torfox://settings/")), "error")

    def test_fetch(self) -> None:
        result = self.web.evaluate(
            f"fetch({json.dumps(self.shell_script)}).then(r => 'loaded:' + r.status, e => 'error:' + e.name)")
        self.assertTrue(result.startswith("error:"), result)

    def test_fetch_no_cors(self) -> None:
        result = self.web.evaluate(
            f"fetch({json.dumps(self.shell_script)}, {{mode: 'no-cors'}}).then(r => 'loaded:' + r.type, e => 'error:' + e.name)")
        self.assertTrue(result.startswith("error:"), result)

    def test_xhr(self) -> None:
        result = self.web.evaluate("""
          new Promise((resolve) => {
            const x = new XMLHttpRequest();
            try { x.open("GET", %s); } catch (e) { resolve("error:" + e.name); return; }
            x.onload = () => resolve("loaded:" + x.status);
            x.onerror = () => resolve("error");
            x.send();
          })""" % json.dumps(self.shell_script))
        self.assertTrue(result.startswith("error"), result)

    def test_redirected_subresource(self) -> None:
        self.assertEqual(self.web.evaluate(load_probe("script", self.fixture.origin + "/redirect-asset")),
                         "error")

    def test_worker_fetch(self) -> None:
        result = self.web.evaluate("""
          new Promise((resolve) => {
            const src = "fetch(" + JSON.stringify(%s) + ").then(() => postMessage('loaded'), (e) => postMessage('error:' + e.name));";
            const w = new Worker(URL.createObjectURL(new Blob([src], {type: "text/javascript"})));
            w.onmessage = (m) => resolve(m.data);
            w.onerror = () => resolve("worker error");
            setTimeout(() => resolve("timeout"), 4000);
          })""" % json.dumps(self.shell_script))
        self.assertTrue(result.startswith("error"), result)

    def test_worker_script_url(self) -> None:
        result = self.web.evaluate("""
          new Promise((resolve) => {
            try {
              const w = new Worker(%s);
              w.onerror = () => resolve("error");
              w.onmessage = () => resolve("loaded");
              setTimeout(() => resolve("timeout"), 3000);
            } catch (e) { resolve("error:" + e.name); }
          })""" % json.dumps(self.shell_script))
        self.assertTrue(result.startswith("error"), result)

    def test_send_beacon(self) -> None:
        result = self.web.evaluate("""
          (() => { try { return String(navigator.sendBeacon("torfox://settings/", "x")); }
                   catch (e) { return "error:" + e.name; } })()""")
        self.assertIn(result, ("false", "error:TypeError"))

    def test_css_url(self) -> None:
        self.web.evaluate("""
          (() => { const d = document.createElement("div");
                   d.style.backgroundImage = "url(torfox://settings/bg.png)";
                   d.style.width = d.style.height = "10px";
                   document.body.appendChild(d); getComputedStyle(d).backgroundImage; })()""")
        time.sleep(0.5)

    # Frames ------------------------------------------------------------------------
    def test_iframe(self) -> None:
        self.web.evaluate("""
          (() => { const f = document.createElement("iframe"); f.id = "f1";
                   f.src = "torfox://settings/"; document.body.appendChild(f); })()""")
        self.assert_refused("nav_blocked", "torfox://settings/")
        frame_url = self.web.evaluate("""
          (() => { try { return document.getElementById("f1").contentWindow.location.href; }
                   catch (e) { return "inaccessible"; } })()""")
        self.assertNotIn("torfox:", frame_url)

    def test_object_and_embed(self) -> None:
        self.web.evaluate("""
          (() => { const o = document.createElement("object"); o.data = "torfox://settings/";
                   document.body.appendChild(o);
                   const e = document.createElement("embed"); e.src = "torfox://newtab/";
                   document.body.appendChild(e); })()""")
        time.sleep(1.0)

    # Navigations ------------------------------------------------------------------------
    def test_location_assignment(self) -> None:
        self.web.evaluate("location.href = 'torfox://settings/'; 1", user_gesture=True)
        self.assert_refused("nav_blocked", "torfox://settings/")
        self.assert_still_on_attack_page()

    def test_location_variants(self) -> None:
        for url in ("TORFOX://settings/", "torfox:settings", " torfox://newtab/", "torfox:\t//settings/",
                    "torfox://shell/"):
            self.web.evaluate(f"location.href = {json.dumps(url)}; 1", user_gesture=True)
            time.sleep(0.3)
            self.assert_still_on_attack_page()

    def test_link_click(self) -> None:
        self.web.evaluate("document.getElementById('link').click(); 1", user_gesture=True)
        self.assert_refused("nav_blocked", "torfox://settings/")
        self.assert_still_on_attack_page()

    def test_link_target_blank(self) -> None:
        before = len(self.tf.browser.targets())
        self.web.evaluate("document.getElementById('blank').click(); 1", user_gesture=True)
        self.assert_refused("popup_blocked", "torfox://settings/")
        time.sleep(0.3)
        self.assertEqual(len(self.tf.browser.targets()), before)

    def test_window_open(self) -> None:
        before = len(self.tf.browser.targets())
        result = self.web.evaluate("String(window.open('torfox://settings/'))", user_gesture=True)
        self.assertEqual(result, "null")
        self.assert_refused("popup_blocked", "torfox://settings/")
        self.assertEqual(len(self.tf.browser.targets()), before)

    def test_form_post(self) -> None:
        self.web.evaluate("document.getElementById('form').submit(); 1", user_gesture=True)
        self.assert_refused("nav_blocked", "torfox://settings/")
        self.assert_still_on_attack_page()

    def test_server_redirect(self) -> None:
        self.web.evaluate(f"location.href = {json.dumps(self.fixture.origin + '/redirect-settings')}; 1",
                          user_gesture=True)
        self.assert_logged("nav_blocked", "torfox://settings/")
        wait_for(lambda: "redirect" not in self.web.evaluate("location.href"), 5, "redirect cancelled")
        self.assertNotIn("torfox:", self.web.evaluate("location.href"))
        # Return to the attack page for the remaining tests.
        self.web.evaluate(f"location.href = {json.dumps(self.attack_url)}; 1", user_gesture=True)
        wait_for(lambda: self.web.evaluate("location.href") == self.attack_url and
                 self.web.evaluate("document.readyState") == "complete", 5, "attack page reload")

    # The bridge -----------------------------------------------------------------------
    def test_no_bridge_in_web_pages(self) -> None:
        names = self.web.evaluate(
            "Object.getOwnPropertyNames(window).filter(n => /torfox|cefquery/i.test(n))")
        self.assertEqual(names, [])
        self.assertEqual(self.web.evaluate("typeof window.cefQuery"), "undefined")
        self.assertEqual(self.web.evaluate("typeof window.torfoxQuery"), "undefined")

    def test_no_bridge_in_web_subframes(self) -> None:
        self.fixture.html("/child", "<!doctype html><title>child</title>")
        self.web.evaluate("""
          new Promise((resolve) => { const f = document.createElement("iframe");
            f.onload = resolve; f.src = "/child"; document.body.appendChild(f); })""")
        names = self.web.evaluate("""
          Object.getOwnPropertyNames(document.querySelector('iframe[src="/child"]').contentWindow)
            .filter(n => /torfox|cefquery/i.test(n))""")
        self.assertEqual(names, [])

    def test_internal_pages_have_the_bridge(self) -> None:
        self.assertEqual(self.shell.evaluate("typeof window.torfoxQuery"), "function")


class TypedInternalUrlTest(unittest.TestCase):
    """torfox:// typed into the address bar never loads in a web browser."""

    def setUp(self) -> None:
        self.fixture = Fixture().__enter__()
        self.page_url = self.fixture.html("/page", "<!doctype html><title>page</title>")
        self.tf = TorFox(start_url=self.page_url).start()
        self.shell = self.tf.shell()

    def tearDown(self) -> None:
        try:
            self.tf.__exit__(None, None, None)
        finally:
            self.fixture.__exit__(None, None, None)

    def test_settings_opens_in_an_internal_browser(self) -> None:
        self.tf.page_at(self.page_url)
        result = bridge_call(self.shell, "nav.go", {"input": "torfox://settings/"})
        self.assertTrue(result["ok"], result)
        self.tf.page_at("torfox://settings/")
        served = [e for e in self.tf.events("scheme_request") if e["url"] == "torfox://settings/"]
        self.assertTrue(served)
        self.assertEqual({e["role"] for e in served}, {"internal_tab"})
        # The web page's browser was replaced, not navigated.
        self.assertFalse([t for t in self.tf.browser.targets() if t["url"] == self.page_url])

    def test_shell_and_unknown_pages_are_refused(self) -> None:
        for text in ("torfox://shell/", "torfox://evil/", "torfox://settings/../shell/"):
            result = bridge_call(self.shell, "nav.go", {"input": text})
            time.sleep(0.3)
            urls = [t["url"] for t in self.tf.browser.targets()]
            self.assertEqual(urls.count("torfox://shell/"), 1, (text, result, urls))
            self.assertFalse([u for u in urls if u.startswith("torfox://evil")], (text, urls))
        self.assertFalse([e for e in self.tf.events("scheme_request")
                          if e["role"] != "shell" and "shell" in e["url"]])


class InternalPagesStayInternalTest(unittest.TestCase):
    """Internal pages cannot navigate to, open or fetch from the web."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()
        cls.web_url = cls.fixture.html("/web", "<!doctype html><title>web</title>")
        cls.tf = TorFox().start()
        cls.newtab = cls.tf.page_at("torfox://newtab/")

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            cls.tf.__exit__(None, None, None)
        finally:
            cls.fixture.__exit__(None, None, None)

    def test_navigation_to_web_is_blocked(self) -> None:
        self.newtab.evaluate(f"location.href = {json.dumps(self.web_url)}; 1", user_gesture=True)
        wait_for(lambda: self.tf.events("nav_blocked", role="internal_tab"), 5, "nav_blocked")
        time.sleep(0.3)
        self.assertEqual(self.newtab.evaluate("location.href"), "torfox://newtab/")
        self.assertNotIn("GET /web", self.fixture.requests)

    def test_window_open_is_blocked(self) -> None:
        before = len(self.tf.browser.targets())
        self.newtab.evaluate(f"String(window.open({json.dumps(self.web_url)}))", user_gesture=True)
        time.sleep(0.5)
        self.assertEqual(len(self.tf.browser.targets()), before)
        self.assertNotIn("GET /web", self.fixture.requests)

    def test_fetch_is_blocked(self) -> None:
        result = self.newtab.evaluate(
            f"fetch({json.dumps(self.web_url)}).then(() => 'loaded', e => 'error:' + e.name)")
        self.assertTrue(result.startswith("error:"), result)
        self.assertNotIn("GET /web", self.fixture.requests)

    def test_image_from_web_is_blocked(self) -> None:
        result = self.newtab.evaluate(load_probe("img", self.fixture.origin + "/x.png"))
        self.assertEqual(result, "error")
        self.assertNotIn("GET /x.png", self.fixture.requests)

    def test_other_internal_page_in_same_tab_is_blocked_for_shell(self) -> None:
        self.newtab.evaluate("location.href = 'torfox://shell/'; 1", user_gesture=True)
        time.sleep(0.5)
        self.assertEqual(self.newtab.evaluate("location.href"), "torfox://newtab/")


def navigate(session, url: str) -> dict:
    """A browser-initiated navigation (DevTools Page.navigate), like a typed
    URL: Chromium's renderer-side checks, which stop web pages from opening
    torfox://, do not apply, so only TorFox's own layers stand in the way."""
    return session.send("Page.navigate", {"url": url})


PAGE = "<!doctype html><title>page</title><body>web page</body>"


class BrowserInitiatedNavigationTest(unittest.TestCase):
    """TorFox's navigation guard refuses what Chromium's renderer checks never see."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()
        cls.page_url = cls.fixture.html("/page", PAGE)
        cls.web_url = cls.fixture.html("/web", PAGE)
        cls.tf = TorFox(start_url=cls.page_url).start()
        cls.shell = cls.tf.shell()
        cls.web = cls.tf.page_at(cls.page_url)

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            cls.tf.__exit__(None, None, None)
        finally:
            cls.fixture.__exit__(None, None, None)

    def tearDown(self) -> None:
        self.assertEqual([e for e in self.tf.events("scheme_request")
                          if e.get("role") not in INTERNAL_ROLES], [])

    def assert_blocked(self, role: str, url_part: str, before: int) -> None:
        def blocked():
            return [e for e in self.tf.events("nav_blocked", role=role)[before:]
                    if url_part in e.get("url", "")]
        wait_for(blocked, 5, f"nav_blocked ({role}) for {url_part}")

    def test_web_tab_refuses_privileged_urls(self) -> None:
        for url, part in (("torfox://settings/", "torfox://settings"),
                          ("torfox://newtab/", "torfox://newtab"),
                          ("torfox://shell/", "torfox://shell"),
                          ("TORFOX://SETTINGS/", "torfox://settings"),
                          ("torfox://settings/assets/../index.html", "torfox://settings"),
                          ("chrome://version/", "chrome://version"),
                          # A blob: URL carries its creator's origin.
                          ("blob:torfox://settings/0d4c2d3e-6a5b-4c3d-8e2f-1a2b3c4d5e6f",
                           "blob:torfox://settings"),
                          ("file:///C:/Windows/win.ini", "file:///C:/Windows/win.ini")):
            with self.subTest(url=url):
                before = len(self.tf.events("nav_blocked", role="web_tab"))
                navigate(self.web, url)
                self.assert_blocked("web_tab", part, before)
                time.sleep(0.2)
                self.assertEqual(self.web.evaluate("location.href"), self.page_url)
                self.assertEqual(self.web.evaluate("typeof window.cefQuery"), "undefined")

    def test_internal_tab_refuses_web_urls(self) -> None:
        self.assertTrue(bridge_call(self.shell, "app.openSettings")["ok"])
        settings = self.tf.page_at("torfox://settings/")
        before = len(self.tf.events("nav_blocked", role="internal_tab"))
        navigate(settings, self.web_url)
        self.assert_blocked("internal_tab", self.web_url, before)
        time.sleep(0.2)
        self.assertEqual(settings.evaluate("location.href"), "torfox://settings/")
        self.assertNotIn("GET /web", self.fixture.requests)

    def test_shell_refuses_other_pages(self) -> None:
        for url in ("torfox://settings/", self.web_url):
            with self.subTest(url=url):
                before = len(self.tf.events("nav_blocked", role="shell"))
                navigate(self.shell, url)
                self.assert_blocked("shell", url, before)
                time.sleep(0.2)
                self.assertEqual(self.shell.evaluate("location.href"), "torfox://shell/")
        self.assertNotIn("GET /web", self.fixture.requests)


class GuardLayersTest(unittest.TestCase):
    """Each inner layer holds on its own when the layer in front of it is
    switched off (test builds only: --torfox-test-skip-guards)."""

    def setUp(self) -> None:
        self.fixture = Fixture().__enter__()
        self.page_url = self.fixture.html("/page", PAGE)

    def tearDown(self) -> None:
        self.fixture.__exit__(None, None, None)

    def start(self, skip: str) -> TorFox:
        tf = TorFox(start_url=self.page_url, extra_args=[f"--torfox-test-skip-guards={skip}"]).start()
        self.addCleanup(tf.__exit__, None, None, None)
        self.assertEqual(tf.wait_log("fault_injection")["skip_guards"], skip.split(","))
        return tf

    def web_scheme_requests(self, tf: TorFox) -> list[dict]:
        return [e for e in tf.events("scheme_request") if e.get("role") not in INTERNAL_ROLES]

    def test_resource_guard_alone_refuses_internal_urls(self) -> None:
        tf = self.start("nav")
        web = tf.page_at(self.page_url)
        navigate(web, "torfox://settings/")
        wait_for(lambda: [e for e in tf.events("resource_blocked", role="web_tab")
                          if e.get("url") == "torfox://settings/"], 5, "resource_blocked")
        time.sleep(0.3)
        self.assertNotEqual(web.evaluate("location.href"), "torfox://settings/")
        self.assertEqual(web.evaluate("typeof window.cefQuery"), "undefined")
        self.assertEqual(self.web_scheme_requests(tf), [])

    def test_web_contexts_have_no_internal_scheme_handler(self) -> None:
        # With both TorFox guards off, the request reaches the network stack of
        # the web tab's own request context, which must not know torfox:// at all.
        tf = self.start("nav,resource")
        web = tf.page_at(self.page_url)
        result = navigate(web, "torfox://settings/")
        self.assertEqual(result.get("errorText"), "net::ERR_UNKNOWN_URL_SCHEME", result)
        time.sleep(0.5)
        self.assertNotEqual(web.evaluate("location.href"), "torfox://settings/")
        self.assertEqual(web.evaluate("typeof window.cefQuery"), "undefined")
        self.assertFalse(web.evaluate("!!document.querySelector('tf-settings')"))
        self.assertEqual(self.web_scheme_requests(tf), [])
        self.assertFalse(tf.events("nav_blocked", role="web_tab"))
        self.assertFalse(tf.events("resource_blocked", role="web_tab"))


if __name__ == "__main__":
    unittest.main()
