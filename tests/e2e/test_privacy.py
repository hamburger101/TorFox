# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Stage 4 privacy and security defaults, in normal windows.

  trackers          third-party requests to EasyPrivacy domains are cancelled
  HTTPS-first       http:// navigations are tried over https first, with a
                    prompt (not a silent downgrade) when that fails
  cookies           third-party cookies are blocked
  pop-ups           pop-ups without a click are blocked
  permissions       camera, microphone and clipboard: denied, or asked;
                    location and notifications: always denied
  external apps     mailto:/tel: links ask first; other schemes never launch

Every site is local (sites.py): real-looking names are mapped to 127.0.0.1
and every other name fails to resolve. The https sites use a certificate made
for this run, trusted by its public key (test builds only)."""

from __future__ import annotations

import json
import pathlib
import shutil
import socket
import tempfile
import time
import unittest

from harness import TorFox, bridge_call, click_element, shell_state, wait_for, wait_state
from sites import Sites

NEWS = "news.example.com"        # The page being read.
SHOP = "shop.example.net"        # Another site, embedded in it.
CDN = "cdn.example.org"          # Not a tracker.
GA = "www.google-analytics.com"  # On the tracker list.
GA_ROOT = "google-analytics.com"
SECURE = "secure.example.com"    # Has https.
PLAIN = "plain.example.com"      # Refuses TLS: an http-only site.
PLAIN2 = "plain2.example.com"
LOOP = "loop.example.com"        # Its https redirects back to http.
LOCAL = "intranet.test"          # Not a public name: never upgraded.
HOSTS = [NEWS, SHOP, CDN, GA, GA_ROOT, SECURE, PLAIN, PLAIN2, LOOP, LOCAL]


class PrivacyBase(unittest.TestCase):
    EXTRA_ARGS: list[str] = []

    @classmethod
    def setUpClass(cls) -> None:
        cls.sites = Sites(HOSTS, refuse_tls=(PLAIN, PLAIN2)).__enter__()
        cls.root = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-privacy-"))
        cls.user_data = cls.root / "profile"
        cls.choice = cls.root / "choice.txt"
        s = cls.sites
        cls.tf = TorFox(user_data=cls.user_data, host_rules=s.host_rules(HOSTS),
                        extra_args=[f"--ignore-certificate-errors-spki-list={s.spki}",
                                    f"--torfox-test-https-port={s.http_port}:{s.https_port}",
                                    f"--torfox-test-dialog-choice={cls.choice}",
                                    "--use-fake-device-for-media-stream",
                                    *cls.EXTRA_ARGS]).start()
        cls.shell = cls.tf.shell()
        wait_state(cls.shell, lambda st: len(st["tabs"]) >= 1, "the first tab")

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            cls.tf.__exit__(None, None, None)
        finally:
            cls.sites.__exit__(None, None, None)
            shutil.rmtree(cls.root, ignore_errors=True)

    def setUp(self) -> None:
        self.choice.unlink(missing_ok=True)
        self.fresh_tab()

    def tearDown(self) -> None:
        self.choice.unlink(missing_ok=True)

    # ----- helpers -----

    def fresh_tab(self) -> None:
        old = [t["id"] for t in shell_state(self.shell)["tabs"]]
        bridge_call(self.shell, "tab.new")
        wait_state(self.shell, lambda st: st["activeId"] not in old and len(st["tabs"]) == len(old) + 1,
                   "fresh tab")
        for tab_id in old:
            bridge_call(self.shell, "tab.close", {"id": tab_id})
        wait_state(self.shell, lambda st: len(st["tabs"]) == 1, "only the fresh tab")

    def setting(self, key: str, value) -> None:
        """Changes a setting for this test only (restored afterwards)."""
        page = self.settings_page()
        before = {s["key"]: s["value"] for s in json.loads(bridge_call(page, "settings.get")["response"])["settings"]}
        r = bridge_call(page, "settings.set", {"key": key, "value": value})
        self.assertTrue(r["ok"], r)
        self.addCleanup(self.restore_setting, key, before[key])
        self.fresh_tab()

    def restore_setting(self, key: str, value) -> None:
        bridge_call(self.settings_page(), "settings.set", {"key": key, "value": value})

    def settings_page(self):
        self.assertTrue(bridge_call(self.shell, "app.openSettings")["ok"])
        return self.tf.page(lambda t: t["url"].startswith("torfox://settings/"))

    def mark(self) -> int:
        return len(self.tf.log())

    def since(self, mark: int, event: str, **match) -> list[dict]:
        return [e for e in self.tf.log()[mark:] if e.get("ev") == event and
                all(e.get(k) == v for k, v in match.items())]

    def wait_since(self, mark: int, event: str, timeout: float = 15.0, **match) -> dict:
        return wait_for(lambda: (self.since(mark, event, **match) or [None])[-1], timeout, f"{event} {match}")

    def go(self, url: str) -> None:
        self.assertTrue(bridge_call(self.shell, "nav.go", {"input": url})["ok"])

    def page_with(self, predicate, timeout: float = 15.0):
        return self.tf.page(lambda t: t["type"] == "page" and predicate(t["url"]), timeout)

    def evaluate(self, page, expression: str, timeout: float = 15.0):
        # As if the user had just clicked: some APIs require it.
        return page.evaluate(expression, user_gesture=True, timeout=timeout)

    def active_url(self) -> str:
        st = shell_state(self.shell)
        return next(t["url"] for t in st["tabs"] if t["id"] == st["activeId"])

    def https(self, host: str, path: str = "/") -> str:
        return self.sites.url("https", host, path)

    def http(self, host: str, path: str = "/") -> str:
        return self.sites.url("http", host, path)


class TrackerTest(PrivacyBase):
    def article(self, name: str) -> str:
        """A page on NEWS loading a tracker script and pixel, and an ordinary image."""
        s = self.sites
        s.raw(GA, f"/{name}.gif", 200, {"Content-Type": "image/gif"}, b"GIF89a")
        s.raw(GA_ROOT, f"/{name}.js", 200, {"Content-Type": "text/javascript"}, b"window.ga=1;")
        s.raw(CDN, f"/{name}.gif", 200, {"Content-Type": "image/gif"}, b"GIF89a")
        s.html(NEWS, f"/{name}", f"""<!doctype html><title>{name}</title>
            <img src="{self.https(GA, f'/{name}.gif')}"><script src="{self.https(GA_ROOT, f'/{name}.js')}"></script>
            <img id="cdn" src="{self.https(CDN, f'/{name}.gif')}">""")
        return self.https(NEWS, f"/{name}")

    def test_third_party_trackers_are_blocked(self) -> None:
        url = self.article("blocked")
        m = self.mark()
        self.go(url)
        wait_for(lambda: self.sites.seen(CDN, "/blocked.gif"), 15, "the ordinary image")
        want = {self.https(GA, "/blocked.gif"), self.https(GA_ROOT, "/blocked.js")}
        # Chromium may ask for a blocked image more than once.
        blocked = wait_for(lambda: {b["url"] for b in self.since(m, "tracker_blocked")} == want and
                           self.since(m, "tracker_blocked"), 10, "both tracker requests blocked")
        self.assertEqual({b["why"] for b in blocked}, {GA_ROOT})
        time.sleep(0.5)
        self.assertEqual(self.sites.seen(GA), [])
        self.assertEqual(self.sites.seen(GA_ROOT), [])

    def test_tracker_sites_work_when_visited_directly(self) -> None:
        s = self.sites
        s.raw(GA, "/own.gif", 200, {"Content-Type": "image/gif"}, b"GIF89a")
        s.html(GA, "/home", f"<!doctype html><img src='{self.https(GA, '/own.gif')}'>")
        m = self.mark()
        self.go(self.https(GA, "/home"))
        wait_for(lambda: s.seen(GA, "/own.gif"), 15, "the site's own image")
        self.assertEqual(self.since(m, "tracker_blocked"), [])

    def test_tracker_blocking_can_be_turned_off(self) -> None:
        self.setting("privacy.blockTrackers", False)
        url = self.article("allowed")
        m = self.mark()
        self.go(url)
        wait_for(lambda: self.sites.seen(GA, "/allowed.gif") and self.sites.seen(GA_ROOT, "/allowed.js"), 15,
                 "the tracker requests")
        self.assertEqual(self.since(m, "tracker_blocked"), [])


class HttpsFirstTest(PrivacyBase):
    def test_http_is_tried_over_https_first(self) -> None:
        self.sites.html(SECURE, "/page", "<!doctype html><title>secure</title>ok")
        m = self.mark()
        self.go(self.http(SECURE, "/page"))
        up = self.wait_since(m, "https_upgraded")
        self.assertEqual(up["to"], self.https(SECURE, "/page"))
        wait_for(lambda: self.sites.seen(SECURE, "/page", scheme="https"), 15, "the https request")
        wait_for(lambda: self.active_url() == self.https(SECURE, "/page"), 10, "the https page shown")
        self.assertEqual(self.sites.seen(SECURE, "/page", scheme="http"), [])

    def test_a_site_without_https_asks_before_using_http(self) -> None:
        self.sites.html(PLAIN, "/page", "<!doctype html><title>plain</title>ok")
        m = self.mark()
        self.choice.write_text("accept")
        self.go(self.http(PLAIN, "/page"))
        shown = self.wait_since(m, "dialog_shown", kind="https")
        self.assertIn(PLAIN, shown["heading"])
        self.assertIn("not encrypted", shown["details"])
        self.assertEqual(shown["accept"], "Continue to the http site")
        wait_for(lambda: self.sites.seen(PLAIN, "/page", scheme="http"), 15, "the http page after accepting")
        # Remembered for the session: no second attempt, no second question.
        self.fresh_tab()
        m2 = self.mark()
        count = len(self.sites.seen(PLAIN, "/page", scheme="http"))
        self.go(self.http(PLAIN, "/page"))
        wait_for(lambda: len(self.sites.seen(PLAIN, "/page", scheme="http")) > count, 15, "the second visit")
        self.assertEqual(self.since(m2, "https_upgraded"), [])
        self.assertEqual(self.since(m2, "dialog_shown"), [])

    def test_declining_never_loads_http(self) -> None:
        self.sites.html(PLAIN2, "/page", "<!doctype html>ok")
        m = self.mark()
        self.choice.write_text("cancel")
        self.go(self.http(PLAIN2, "/page"))
        self.wait_since(m, "dialog_shown", kind="https")
        self.wait_since(m, "https_fallback", accepted=False)
        time.sleep(1)
        self.assertEqual(self.sites.seen(PLAIN2, scheme="http"), [])

    def test_https_redirecting_back_to_http_asks(self) -> None:
        self.sites.raw(LOOP, "/page", 301, {"Location": self.http(LOOP, "/page")})
        m = self.mark()
        self.choice.write_text("cancel")
        self.go(self.http(LOOP, "/page"))
        shown = self.wait_since(m, "dialog_shown", kind="https")
        self.assertIn(LOOP, shown["heading"])
        time.sleep(1)
        self.assertEqual(self.sites.seen(LOOP, scheme="http"), [])
        self.assertEqual(len(self.since(m, "https_upgraded")), 1, "upgraded in a loop")

    def test_local_names_are_not_upgraded(self) -> None:
        self.sites.html(LOCAL, "/page", "<!doctype html>ok")
        m = self.mark()
        self.go(self.http(LOCAL, "/page"))
        wait_for(lambda: self.sites.seen(LOCAL, "/page", scheme="http"), 15, "the local page over http")
        self.assertEqual(self.since(m, "https_upgraded"), [])

    def test_https_first_can_be_turned_off(self) -> None:
        self.setting("privacy.httpsFirst", False)
        self.sites.html(SECURE, "/off", "<!doctype html>ok")
        m = self.mark()
        self.go(self.http(SECURE, "/off"))
        wait_for(lambda: self.sites.seen(SECURE, "/off", scheme="http"), 15, "the http page")
        self.assertEqual(self.since(m, "https_upgraded"), [])


class CookieTest(PrivacyBase):
    def embed(self, name: str) -> str:
        """NEWS embeds SHOP, which sets a cookie and then reads it back."""
        s = self.sites
        s.raw(SHOP, f"/set-{name}", 200, {"Content-Type": "text/html",
                                          "Set-Cookie": f"{name}=1; SameSite=None; Secure; Path=/"},
              b"<!doctype html>set")
        s.html(SHOP, f"/read-{name}", "<!doctype html>read")
        s.html(NEWS, f"/embed-{name}", f"""<!doctype html>
            <iframe id="a" src="{self.https(SHOP, f'/set-{name}')}"></iframe>
            <script>document.getElementById('a').onload = () => {{
              const b = document.createElement('iframe');
              b.src = {json.dumps(self.https(SHOP, f'/read-{name}'))};
              document.body.append(b);
            }};</script>""")
        return self.https(NEWS, f"/embed-{name}")

    def cookie_sent(self, name: str) -> str:
        reads = wait_for(lambda: self.sites.seen(SHOP, f"/read-{name}"), 15, "the embedded read")
        return reads[-1].headers.get("cookie", "")

    def test_third_party_cookies_are_blocked(self) -> None:
        self.go(self.embed("tpblocked"))
        self.assertNotIn("tpblocked=1", self.cookie_sent("tpblocked"))

    def test_first_party_cookies_work(self) -> None:
        s = self.sites
        s.raw(SHOP, "/set-fp", 200, {"Content-Type": "text/html",
                                     "Set-Cookie": "fp=1; SameSite=Lax; Secure; Path=/"}, b"set")
        s.html(SHOP, "/read-fp", "read")
        self.go(self.https(SHOP, "/set-fp"))
        wait_for(lambda: s.seen(SHOP, "/set-fp"), 15, "the cookie page")
        time.sleep(0.5)
        self.go(self.https(SHOP, "/read-fp"))
        self.assertIn("fp=1", self.cookie_sent("fp"))

    def test_third_party_cookies_can_be_allowed(self) -> None:
        self.setting("privacy.blockThirdPartyCookies", False)
        self.go(self.embed("tpallowed"))
        self.assertIn("tpallowed=1", self.cookie_sent("tpallowed"))


class PopupTest(PrivacyBase):
    def opener(self, name: str) -> str:
        self.sites.html(NEWS, f"/popped-{name}", "<!doctype html>popped")
        target = json.dumps(self.https(NEWS, f"/popped-{name}"))
        self.sites.html(NEWS, f"/opener-{name}", f"""<!doctype html>
            <button id="go" style="display:block;width:200px;height:60px"
                    onclick='window.open({target})'>open</button>
            <script>if (location.hash === '#auto') setTimeout(() => window.open({target}), 50);</script>""")
        return self.https(NEWS, f"/opener-{name}")

    def test_popups_without_a_click_are_blocked(self) -> None:
        m = self.mark()
        self.go(self.opener("auto") + "#auto")
        blocked = self.wait_since(m, "popup_blocked")
        self.assertEqual(blocked["why"], "no user gesture")
        wait_state(self.shell, lambda st: "Pop-up blocked" in (st["notice"] or ""), "the notice")
        time.sleep(0.5)
        self.assertEqual(self.sites.seen(NEWS, "/popped-auto"), [])
        self.assertEqual(len(shell_state(self.shell)["tabs"]), 1)

    def test_a_click_opens_a_tab(self) -> None:
        url = self.opener("click")
        self.go(url)
        click_element(self.page_with(lambda u: u == url), "#go")
        wait_state(self.shell, lambda st: len(st["tabs"]) == 2, "the new tab")
        wait_for(lambda: self.sites.seen(NEWS, "/popped-click"), 15, "the pop-up's page")

    def test_popup_blocking_can_be_turned_off(self) -> None:
        self.setting("privacy.blockPopups", False)
        m = self.mark()
        self.go(self.opener("allowed") + "#auto")
        wait_for(lambda: self.sites.seen(NEWS, "/popped-allowed"), 15, "the pop-up's page")
        self.assertEqual(self.since(m, "popup_blocked"), [])


class PermissionTest(PrivacyBase):
    def page(self, name: str):
        self.sites.html(NEWS, f"/perm-{name}", "<!doctype html><title>perm</title><input id=i>")
        url = self.https(NEWS, f"/perm-{name}")
        self.go(url)
        return self.page_with(lambda u: u == url)

    CAMERA = ("navigator.mediaDevices.getUserMedia({video: true})"
              ".then(s => { s.getTracks().forEach(t => t.stop()); return 'granted'; }, e => e.name)")
    MIC = ("navigator.mediaDevices.getUserMedia({audio: true})"
           ".then(s => { s.getTracks().forEach(t => t.stop()); return 'granted'; }, e => e.name)")

    def test_camera_and_microphone_are_denied_by_default(self) -> None:
        page = self.page("default")
        m = self.mark()
        self.assertEqual(self.evaluate(page, self.CAMERA), "NotAllowedError")
        self.assertEqual(self.evaluate(page, self.MIC), "NotAllowedError")
        self.assertEqual(self.since(m, "dialog_shown"), [])
        self.assertGreaterEqual(len(self.since(m, "permission_denied")), 2)

    def test_camera_can_be_asked_for(self) -> None:
        self.setting("permissions.camera", "ask")
        page = self.page("ask")
        m = self.mark()
        self.choice.write_text("accept")
        self.assertEqual(self.evaluate(page, self.CAMERA), "granted")
        shown = self.wait_since(m, "dialog_shown", kind="permission")
        self.assertIn(NEWS, shown["heading"])
        self.assertIn("camera", shown["details"])
        # Each request asks again; declining denies.
        self.choice.write_text("cancel")
        self.assertEqual(self.evaluate(page, self.CAMERA), "NotAllowedError")
        self.assertEqual(len([d for d in self.since(m, "dialog_shown") if d["kind"] == "permission"]), 2)
        # The microphone is still denied without asking.
        m2 = self.mark()
        self.assertEqual(self.evaluate(page, self.MIC), "NotAllowedError")
        self.assertEqual(self.since(m2, "dialog_shown"), [])

    def test_screen_capture_is_always_denied(self) -> None:
        self.setting("permissions.camera", "ask")
        page = self.page("screen")
        m = self.mark()
        result = self.evaluate(page, "navigator.mediaDevices.getDisplayMedia({video: true})"
                                     ".then(() => 'granted', e => e.name)")
        self.assertNotEqual(result, "granted")
        self.assertEqual(self.since(m, "dialog_shown"), [])

    def test_location_and_notifications_are_always_denied(self) -> None:
        page = self.page("geo")
        m = self.mark()
        self.assertEqual(self.evaluate(page, "Notification.requestPermission()"), "denied")
        self.assertEqual(self.evaluate(page, "new Promise(r => navigator.geolocation.getCurrentPosition("
                                             "() => r('granted'), e => r(e.code)))"), 1)
        self.assertEqual(self.since(m, "dialog_shown"), [])

    def test_clipboard_reading_is_denied_or_asked(self) -> None:
        page = self.page("clip")
        read = "navigator.clipboard.readText().then(() => 'granted', e => e.name)"
        page.send("Runtime.evaluate", {"expression": "document.getElementById('i').focus()"})
        m = self.mark()
        self.assertEqual(self.evaluate(page, read), "NotAllowedError")
        self.assertEqual(self.since(m, "dialog_shown"), [])
        # The same site, now set to Ask: Chromium must not have kept the
        # earlier answer.
        self.setting("permissions.clipboard", "ask")
        page = self.page("clip-ask")
        page.send("Runtime.evaluate", {"expression": "document.getElementById('i').focus()"})
        m = self.mark()
        self.choice.write_text("accept")
        self.assertEqual(self.evaluate(page, read), "granted")
        shown = self.wait_since(m, "dialog_shown", kind="permission")
        self.assertIn(NEWS, shown["heading"])
        # Asked again: no answer is kept.
        m = self.mark()
        self.choice.write_text("cancel")
        self.assertEqual(self.evaluate(page, read), "NotAllowedError")
        self.wait_since(m, "dialog_shown", kind="permission")


class ExternalProtocolTest(PrivacyBase):
    def links(self, name: str) -> tuple[str, object]:
        self.sites.html(NEWS, f"/ext-{name}", """<!doctype html>
            <a id="mail" href="mailto:someone@example.com?subject=hi" style="display:block;padding:30px">mail</a>
            <a id="other" href="ms-settings:privacy" style="display:block;padding:30px">settings</a>""")
        url = self.https(NEWS, f"/ext-{name}")
        self.go(url)
        return url, self.page_with(lambda u: u == url)

    def test_mail_links_ask_before_opening_another_app(self) -> None:
        _, page = self.links("accept")
        m = self.mark()
        self.choice.write_text("accept")
        click_element(page, "#mail")
        shown = self.wait_since(m, "dialog_shown", kind="external")
        self.assertIn("mailto:someone@example.com", shown["details"])
        opened = self.wait_since(m, "external_opened")
        self.assertEqual(opened["scheme"], "mailto")

    def test_mail_links_without_a_click_never_ask(self) -> None:
        _, page = self.links("auto")
        m = self.mark()
        self.choice.write_text("accept")  # Must not be consumed.
        page.evaluate("location.href = 'mailto:someone@example.com'; 1")
        blocked = self.wait_since(m, "protocol_blocked", why="no user gesture")
        self.assertIn("mailto:", blocked["url"])
        self.assertEqual(self.since(m, "dialog_shown"), [])
        self.assertTrue(self.choice.exists())

    def test_declined_mail_links_open_nothing(self) -> None:
        _, page = self.links("cancel")
        m = self.mark()
        self.choice.write_text("cancel")
        click_element(page, "#mail")
        self.wait_since(m, "dialog_shown", kind="external")
        time.sleep(1)
        self.assertEqual(self.since(m, "external_opened"), [])

    def test_other_schemes_never_launch(self) -> None:
        _, page = self.links("other")
        m = self.mark()
        self.choice.write_text("accept")  # Must not be consumed.
        click_element(page, "#other")
        # Refused by the navigation guard, before Chromium could look for an app.
        blocked = self.wait_since(m, "nav_blocked", why="scheme not allowed")
        self.assertIn("ms-settings:", blocked["url"])
        time.sleep(0.5)
        self.assertEqual(self.since(m, "dialog_shown"), [])
        self.assertTrue(self.choice.exists())


class WebRtcTest(PrivacyBase):
    GATHER = """new Promise(resolve => {
        const pc = new RTCPeerConnection();
        const found = [];
        const done = () => resolve({candidates: found, sdp: pc.localDescription ? pc.localDescription.sdp : ""});
        pc.onicecandidate = e => { if (e.candidate) found.push(e.candidate.candidate); else done(); };
        pc.createDataChannel("x");
        pc.createOffer().then(o => pc.setLocalDescription(o));
        setTimeout(done, 8000);
    })"""

    def test_webrtc_reveals_no_local_addresses(self) -> None:
        # Without a STUN server the only candidates are the computer's own
        # addresses: they must be hidden behind random mDNS names.
        self.sites.html(NEWS, "/rtc", "<!doctype html><title>rtc</title>")
        url = self.https(NEWS, "/rtc")
        self.go(url)
        result = self.evaluate(self.page_with(lambda u: u == url), self.GATHER, timeout=20)
        addresses = {socket.gethostbyname(socket.gethostname())}
        for info in socket.getaddrinfo(socket.gethostname(), None):
            addresses.add(info[4][0].split("%")[0])
        addresses -= {"127.0.0.1", "::1"}
        self.assertTrue(addresses, "the test found no local address to look for")
        for candidate in result["candidates"]:
            self.assertTrue(candidate.split()[4].endswith(".local"), candidate)
        for address in addresses:
            self.assertNotIn(address, result["sdp"])
            self.assertFalse([c for c in result["candidates"] if address in c], address)


class ClearOnExitTest(unittest.TestCase):
    """Settings > Privacy & Security > Clear when TorFox closes, across real
    restarts of one profile: after a normal exit, after a crash, and (as a
    control showing the checks can see the data) with clearing off."""

    CATEGORIES = ("privacy.clearOnExit.cookies", "privacy.clearOnExit.history", "privacy.clearOnExit.cache")

    @classmethod
    def setUpClass(cls) -> None:
        cls.sites = Sites(HOSTS, refuse_tls=()).__enter__()
        s = cls.sites
        s.raw(SHOP, "/set", 200, {"Content-Type": "text/html",
                                  "Set-Cookie": "kept=1; Secure; Path=/; Max-Age=86400"},
              b"<!doctype html><title>set</title><img src='/pic.png'>")
        s.raw(SHOP, "/pic.png", 200, {"Content-Type": "image/png", "Cache-Control": "max-age=86400"},
              b"\x89PNG\r\n\x1a\n" + b"\0" * 64)
        s.html(SHOP, "/read", "<!doctype html><title>read</title>read")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.sites.__exit__(None, None, None)

    def setUp(self) -> None:
        self.user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-clear-"))
        self.addCleanup(shutil.rmtree, self.user_data, True)

    @property
    def profile(self) -> pathlib.Path:
        return self.user_data / "User Data" / "Normal"

    def start(self) -> TorFox:
        s = self.sites
        tf = TorFox(user_data=self.user_data, host_rules=s.host_rules(HOSTS),
                    extra_args=[f"--ignore-certificate-errors-spki-list={s.spki}",
                                f"--torfox-test-https-port={s.http_port}:{s.https_port}"]).start()
        self.addCleanup(tf.cleanup)
        wait_state(tf.shell(), lambda st: len(st["tabs"]) >= 1, "the first tab")
        return tf

    def set_clearing(self, tf: TorFox, on: bool) -> None:
        self.assertTrue(bridge_call(tf.shell(), "app.openSettings")["ok"])
        page = tf.page(lambda t: t["url"].startswith("torfox://settings/"))
        for key in self.CATEGORIES:
            self.assertTrue(bridge_call(page, "settings.set", {"key": key, "value": on})["ok"], key)

    def visit(self, tf: TorFox, path: str) -> None:
        shell = tf.shell()
        self.assertTrue(bridge_call(shell, "tab.new")["ok"])
        loaded = len(tf.events("page_loaded"))
        self.assertTrue(bridge_call(shell, "nav.go", {"input": self.sites.url("https", SHOP, path)})["ok"])
        wait_for(lambda: len(tf.events("page_loaded")) > loaded, 15, f"{path} to load")

    def history(self, tf: TorFox) -> list[str]:
        self.assertTrue(bridge_call(tf.shell(), "app.openHistory")["ok"])
        page = tf.page_at("torfox://history/")
        r = bridge_call(page, "history.list", {})
        self.assertTrue(r["ok"], r)
        return [e["url"] for e in json.loads(r["response"])["entries"]]

    def browse(self, tf: TorFox) -> None:
        """Leaves a cookie, a cached image and a history entry."""
        self.visit(tf, "/set")
        wait_for(lambda: self.sites.seen(SHOP, "/pic.png"), 15, "the image")
        self.assertIn(self.sites.url("https", SHOP, "/set"), self.history(tf))

    def places_mention(self, text: str) -> bool:
        """True if TorFox's history database files contain |text| anywhere."""
        return any(text.encode() in f.read_bytes() for f in self.user_data.glob("places.db*"))

    def next_session(self) -> tuple[TorFox, str, list[str]]:
        """Starts TorFox again: the cookie it sends, and its history."""
        tf = self.start()
        before = len(self.sites.seen(SHOP, "/read"))
        self.visit(tf, "/read")
        reads = wait_for(lambda: self.sites.seen(SHOP, "/read")[before:], 15, "the read")
        return tf, reads[-1].headers.get("cookie", ""), self.history(tf)

    def test_control_without_clearing_everything_is_kept(self) -> None:
        tf = self.start()
        self.browse(tf)
        self.assertEqual(tf.close(), 0)
        self.assertTrue((self.profile / "Cache").exists())
        self.assertTrue((self.profile / "Network" / "Cookies").exists())
        # Chromium's own per-site scores are erased at every exit, whatever
        # the setting: they would list every site visited.
        self.assertNotIn(SHOP, (self.profile / "Preferences").read_text(encoding="utf-8"))
        self.assertTrue(self.places_mention(SHOP))
        tf, cookie, history = self.next_session()
        self.assertIn("kept=1", cookie)
        self.assertIn(self.sites.url("https", SHOP, "/set"), history)
        tf.close()

    def test_chosen_data_is_cleared_when_torfox_closes(self) -> None:
        tf = self.start()
        self.set_clearing(tf, True)
        self.browse(tf)
        self.assertEqual(tf.close(), 0)
        # Gone once TorFox has exited, before anything else could run.
        for gone in ("Cache", "Network/Cookies", "Network/Network Persistent State"):
            self.assertFalse((self.profile / gone).exists(), gone)
        # History rows are overwritten, not just unlinked (secure_delete).
        self.assertFalse(self.places_mention(SHOP))
        tf, cookie, history = self.next_session()
        self.assertNotIn("kept=1", cookie)
        self.assertNotIn(self.sites.url("https", SHOP, "/set"), history)
        # The exit did it: the next start found nothing left to clear.
        self.assertEqual(tf.wait_log("data_cleared", when="startup")["removed"], 0)
        tf.close()

    def test_data_left_by_a_crash_is_cleared_at_the_next_start(self) -> None:
        tf = self.start()
        self.browse(tf)
        self.assertEqual(tf.close(), 0)  # The cookie is now on disk.
        tf = self.start()
        self.set_clearing(tf, True)
        tf.kill()  # No exit clean-up runs.
        self.assertTrue((self.profile / "Network" / "Cookies").exists())
        tf, cookie, history = self.next_session()
        cleared = tf.wait_log("data_cleared", when="startup")
        self.assertGreater(cleared["removed"], 0)
        self.assertEqual(cleared["failed"], 0)
        tf.wait_log("history_cleared", when="startup", ok=True)
        self.assertNotIn("kept=1", cookie)
        self.assertNotIn(self.sites.url("https", SHOP, "/set"), history)
        tf.close()


if __name__ == "__main__":
    unittest.main()
