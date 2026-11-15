# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""The built-in ad blocker (EasyList), in normal and private windows.
Tor windows are tested in test_tor.py (AdBlockTorTest).

  requests      requests EasyList's rules name are cancelled before they
                are sent: images, scripts, frames and fetches. Not
                WebSockets: CEF does not show them to TorFox's request
                checks (test_ad_websockets_are_blocked, a known gap)
  options       $third-party rules leave a site's own requests alone
  ad boxes      element-hiding rules hide ad boxes: the rules for every
                site, and a site's own rules on that site only
  pop-ups       $popup rules block ad windows even after a click
  workers       a service worker's own requests get tracker and ad
                blocking too (CEF gives them to the request context, not
                the tab)
  shield        the toolbar counts what was blocked; its menu allows ads on
                a site (saved for normal windows, kept in memory for a
                private window) and blocks them again
  settings      Block ads off: nothing is blocked or hidden

Each test page uses names that real EasyList rules list (third_party/
easylist/filters.txt), mapped to local servers (sites.py): nothing leaves
this computer."""

from __future__ import annotations

import json
import pathlib
import shutil
import tempfile
import time
import unittest

from harness import CTRL, SHIFT, TorFox, bridge_call, click_element, press, shell_state, wait_for, \
    wait_state
from sites import Sites

NEWS = "news.example.com"     # The page being read.
CDN = "cdn.example.org"       # Not on any list.
ADS = "adnxs.com"             # ||adnxs.com^
ADS_SUB = "ib.adnxs.com"      # The same rule covers subdomains.
ADSTERRA = "adsterra.com"     # ||adsterra.com^$third-party
POP = "popads.net"            # ||popads.net^$popup
GAMES = "1001games.com"       # 1001games.com##.gc-leaderboard
GA = "www.google-analytics.com"  # On the tracker list (EasyPrivacy).
HOSTS = [NEWS, CDN, ADS, ADS_SUB, ADSTERRA, POP, GAMES, GA]

# Never cached: a test that expects a request must see one.
NO_STORE = {"Cache-Control": "no-store"}
GIF = b"GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff!\xf9\x04\x01\x00\x00\x00\x00," \
      b"\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;"


class AdBlockBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.sites = Sites(HOSTS).__enter__()
        cls.root = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-adblock-"))
        cls.user_data = cls.root / "profile"
        cls.choice = cls.root / "menu-choice.txt"
        s = cls.sites
        cls.tf = TorFox(user_data=cls.user_data, host_rules=s.host_rules(HOSTS),
                        extra_args=[f"--ignore-certificate-errors-spki-list={s.spki}",
                                    f"--torfox-test-https-port={s.http_port}:{s.https_port}",
                                    f"--torfox-test-menu-choice={cls.choice}"]).start()
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
        self.fresh_tab(self.shell)

    def tearDown(self) -> None:
        self.choice.unlink(missing_ok=True)

    # ----- helpers -----

    def fresh_tab(self, shell) -> None:
        old = [t["id"] for t in shell_state(shell)["tabs"]]
        bridge_call(shell, "tab.new")
        wait_state(shell, lambda st: st["activeId"] not in old and len(st["tabs"]) == len(old) + 1,
                   "fresh tab")
        for tab_id in old:
            bridge_call(shell, "tab.close", {"id": tab_id})
        wait_state(shell, lambda st: len(st["tabs"]) == 1, "only the fresh tab")

    def settings_page(self):
        self.assertTrue(bridge_call(self.shell, "app.openSettings")["ok"])
        return self.tf.page(lambda t: t["url"].startswith("torfox://settings/"))

    def get_setting(self, key: str):
        page = self.settings_page()
        settings = json.loads(bridge_call(page, "settings.get")["response"])["settings"]
        return next(s["value"] for s in settings if s["key"] == key)

    def setting(self, key: str, value) -> None:
        """Changes a setting for this test only (restored afterwards)."""
        before = self.get_setting(key)
        r = bridge_call(self.settings_page(), "settings.set", {"key": key, "value": value})
        self.assertTrue(r["ok"], r)
        self.addCleanup(lambda: bridge_call(self.settings_page(), "settings.set", {"key": key, "value": before}))
        self.fresh_tab(self.shell)

    def mark(self) -> int:
        return len(self.tf.log())

    def since(self, mark: int, event: str, **match) -> list[dict]:
        return [e for e in self.tf.log()[mark:] if e.get("ev") == event and
                all(e.get(k) == v for k, v in match.items())]

    def wait_since(self, mark: int, event: str, timeout: float = 15.0, **match) -> dict:
        return wait_for(lambda: (self.since(mark, event, **match) or [None])[-1], timeout, f"{event} {match}")

    def go(self, url: str, shell=None) -> None:
        self.assertTrue(bridge_call(shell or self.shell, "nav.go", {"input": url})["ok"])

    def https(self, host: str, path: str = "/") -> str:
        return self.sites.url("https", host, path)

    def article(self, name: str) -> str:
        """A page on NEWS with ads of every kind, and an ordinary image."""
        s = self.sites
        s.raw(ADS, f"/{name}.gif", 200, {"Content-Type": "image/gif", **NO_STORE}, GIF)
        s.raw(ADS_SUB, f"/{name}.js", 200, {"Content-Type": "text/javascript", **NO_STORE},
              b"window.adLoaded=1;")
        s.html(ADS, f"/{name}-frame", "<!doctype html>ad", NO_STORE)
        s.raw(ADS, f"/{name}-fetch", 200, {"Content-Type": "text/plain", "Access-Control-Allow-Origin": "*",
                                           **NO_STORE}, b"ad")
        s.raw(CDN, f"/{name}.gif", 200, {"Content-Type": "image/gif", **NO_STORE}, GIF)
        s.html(NEWS, f"/{name}", f"""<!doctype html><title>{name}</title>
            <img src="{self.https(ADS, f'/{name}.gif')}">
            <script src="{self.https(ADS_SUB, f'/{name}.js')}"></script>
            <iframe src="{self.https(ADS, f'/{name}-frame')}"></iframe>
            <img id="cdn" src="{self.https(CDN, f'/{name}.gif')}">
            <script>fetch("{self.https(ADS, f'/{name}-fetch')}").catch(() => {{}});</script>""")
        return self.https(NEWS, f"/{name}")

    def ad_urls(self, name: str) -> set[str]:
        return {self.https(ADS, f"/{name}.gif"), self.https(ADS_SUB, f"/{name}.js"),
                self.https(ADS, f"/{name}-frame"), self.https(ADS, f"/{name}-fetch")}

    def blocked_urls(self, mark: int) -> set[str]:
        return {e["url"] for e in self.since(mark, "ad_blocked")}

    def menu(self, choice: str | None, shell=None) -> dict:
        """Opens the shield's menu and picks |choice| (a label), or nothing."""
        if choice is not None:
            self.choice.write_text(choice, encoding="utf-8")
        m = self.mark()
        r = bridge_call(shell or self.shell, "adblock.menu", {"x": 10, "y": 10})
        self.assertTrue(r["ok"], r)
        return self.wait_since(m, "menu_shown", kind="shield")


class AdRequestTest(AdBlockBase):
    def test_ad_requests_are_blocked_before_they_are_sent(self) -> None:
        url = self.article("blocked")
        m = self.mark()
        self.go(url)
        wait_for(lambda: self.sites.seen(CDN, "/blocked.gif"), 15, "the ordinary image")
        want = self.ad_urls("blocked")
        wait_for(lambda: self.blocked_urls(m) >= want, 15, "every ad request blocked")
        self.assertEqual(self.blocked_urls(m), want)
        for e in self.since(m, "ad_blocked"):
            self.assertEqual(e["why"], "||adnxs.com^")
        time.sleep(0.5)
        self.assertEqual(self.sites.seen(ADS), [])
        self.assertEqual(self.sites.seen(ADS_SUB), [])
        page = self.tf.page(lambda t: t["url"] == url)
        self.assertIsNone(page.evaluate("window.adLoaded ?? null"))
        # The shield counts them for this page.
        st = wait_state(self.shell, lambda s: (s["nav"].get("shield") or {}).get("ads", 0) >= len(want),
                        "the shield's count")
        shield = st["nav"]["shield"]
        self.assertEqual((shield["enabled"], shield["allowed"]), (True, False))
        # A new page starts again from nothing.
        self.sites.html(NEWS, "/plain", "<!doctype html><title>plain</title>no ads")
        self.go(self.https(NEWS, "/plain"))
        wait_state(self.shell, lambda s: s["nav"].get("url") == self.https(NEWS, "/plain") and
                   not s["nav"]["loading"] and s["nav"]["shield"]["ads"] == 0, "the count reset")

    def test_third_party_rules_leave_a_sites_own_requests_alone(self) -> None:
        s = self.sites
        s.raw(ADSTERRA, "/x1.gif", 200, {"Content-Type": "image/gif", **NO_STORE}, GIF)
        s.html(NEWS, "/third", f"<!doctype html><img src='{self.https(ADSTERRA, '/x1.gif')}'>")
        s.html(ADSTERRA, "/own", f"<!doctype html><img src='{self.https(ADSTERRA, '/x1.gif')}'>")
        m = self.mark()
        self.go(self.https(NEWS, "/third"))
        blocked = self.wait_since(m, "ad_blocked")
        self.assertEqual((blocked["url"], blocked["why"]),
                         (self.https(ADSTERRA, "/x1.gif"), "||adsterra.com^$third-party"))
        self.assertEqual(s.seen(ADSTERRA, "/x1.gif"), [])
        m = self.mark()
        self.go(self.https(ADSTERRA, "/own"))
        wait_for(lambda: s.seen(ADSTERRA, "/x1.gif"), 15, "the site's own image")
        self.assertEqual(self.since(m, "ad_blocked"), [])

    def test_pages_are_never_blocked(self) -> None:
        # ||adnxs.com^ covers the whole site, but a page the user opens loads
        # (its own requests are still blocked: the rule names every request).
        self.sites.html(ADS, "/home", "<!doctype html><title>adnxs home</title>home")
        m = self.mark()
        self.go(self.https(ADS, "/home"))
        page = self.tf.page(lambda t: t["url"] == self.https(ADS, "/home"))
        self.assertEqual(page.evaluate("document.title"), "adnxs home")
        self.assertTrue(self.sites.seen(ADS, "/home"))
        self.assertNotIn(self.https(ADS, "/home"), self.blocked_urls(m))

    @unittest.expectedFailure
    def test_ad_websockets_are_blocked(self) -> None:
        # A known gap, as for tracker blocking: CEF never passes WebSocket
        # handshakes to OnBeforeResourceLoad (docs/STAGE1_FINDINGS.md §5), so
        # the handshake reaches the ad server. An unexpected success means
        # CEF shows them now: update docs/FEATURE_STATUS.md.
        ws = self.https(ADS, "/socket").replace("https:", "wss:")
        self.sites.html(NEWS, "/ws", f"""<!doctype html><title>ws</title>
            <script>try {{ new WebSocket("{ws}"); }} catch (e) {{}}</script>""")
        m = self.mark()
        self.go(self.https(NEWS, "/ws"))
        wait_for(lambda: self.sites.seen(ADS, "/socket") or ws in self.blocked_urls(m), 15,
                 "the WebSocket attempt")
        time.sleep(0.5)
        self.assertIn(ws, self.blocked_urls(m))
        self.assertEqual(self.sites.seen(ADS, "/socket"), [])


class AdBoxTest(AdBlockBase):
    BOXES = """<div id="generic" class="ad--banner">generic</div>
        <div id="specific" class="gc-leaderboard">specific</div>
        <div id="content" class="article">content</div>"""

    def display(self, page, element_id: str) -> str:
        return page.evaluate(f"getComputedStyle(document.getElementById('{element_id}')).display")

    def test_ad_boxes_are_hidden(self) -> None:
        self.sites.html(NEWS, "/boxes", f"<!doctype html><title>boxes</title>{self.BOXES}")
        m = self.mark()
        self.go(self.https(NEWS, "/boxes"))
        page = self.tf.page(lambda t: t["url"] == self.https(NEWS, "/boxes"))
        wait_for(lambda: self.display(page, "generic") == "none", 10, "the generic ad box hidden")
        # 1001games.com's own rule is for that site only.
        self.assertEqual(self.display(page, "specific"), "block")
        self.assertEqual(self.display(page, "content"), "block")
        hidden = self.since(m, "ads_hidden", main=True)
        self.assertEqual(len(hidden), 1)
        self.assertGreater(hidden[0]["bytes"], 100000)  # Every site's rules.
        # The rules are a constructed style sheet, not an element in the page.
        self.assertEqual(page.evaluate("document.querySelectorAll('style').length"), 0)

    def test_a_sites_own_rules_apply_on_that_site(self) -> None:
        self.sites.html(GAMES, "/play", f"<!doctype html><title>games</title>{self.BOXES}")
        self.go(self.https(GAMES, "/play"))
        page = self.tf.page(lambda t: t["url"] == self.https(GAMES, "/play"))
        wait_for(lambda: self.display(page, "specific") == "none", 10, "the site's ad box hidden")
        self.assertEqual(self.display(page, "generic"), "none")
        self.assertEqual(self.display(page, "content"), "block")

    def test_frames_get_their_own_sites_rules_only(self) -> None:
        self.sites.html(GAMES, "/embed", f"<!doctype html>{self.BOXES}")
        self.sites.html(NEWS, "/framed", f"<!doctype html><iframe src='{self.https(GAMES, '/embed')}'></iframe>")
        m = self.mark()
        self.go(self.https(NEWS, "/framed"))
        frame = self.wait_since(m, "ads_hidden", main=False)
        # 1001games.com's three rules, not every site's.
        self.assertLess(frame["bytes"], 1000)


class AdPopupTest(AdBlockBase):
    def test_ad_popups_are_blocked_even_after_a_click(self) -> None:
        s = self.sites
        s.html(POP, "/p1", "<!doctype html>pop-up")
        s.html(CDN, "/next", "<!doctype html><title>next</title>next")
        s.html(NEWS, "/popups", f"""<!doctype html><title>popups</title>
            <button id="ad" style="width:200px;height:60px"
              onclick="window.open('{self.https(POP, '/p1')}')">one</button>
            <button id="ok" style="width:200px;height:60px"
              onclick="window.open('{self.https(CDN, '/next')}')">ok</button>""")
        self.go(self.https(NEWS, "/popups"))
        page = self.tf.page(lambda t: t["url"] == self.https(NEWS, "/popups"))
        tabs = len(shell_state(self.shell)["tabs"])
        m = self.mark()
        click_element(page, "#ad")
        blocked = self.wait_since(m, "ad_popup_blocked")
        self.assertEqual((blocked["url"], blocked["why"]), (self.https(POP, "/p1"), "||popads.net^$popup"))
        time.sleep(0.5)
        self.assertEqual(len(shell_state(self.shell)["tabs"]), tabs)
        self.assertEqual(s.seen(POP), [])
        # Any other window a click opens still opens.
        click_element(page, "#ok")
        wait_state(self.shell, lambda st: len(st["tabs"]) == tabs + 1, "the ordinary pop-up as a tab")
        wait_for(lambda: s.seen(CDN, "/next"), 15, "the ordinary pop-up's page")


class ShieldMenuTest(AdBlockBase):
    def test_allowing_ads_on_a_site_and_blocking_them_again(self) -> None:
        self.addCleanup(lambda: bridge_call(self.settings_page(), "settings.set",
                                            {"key": "privacy.adsAllowedSites", "value": ""}))
        url = self.article("allow")
        m = self.mark()
        self.go(url)
        wait_for(lambda: self.blocked_urls(m) >= self.ad_urls("allow"), 15, "the ads blocked")

        menu = self.menu(f"Allow ads on {NEWS}")
        labels = [(i["label"], i["enabled"]) for i in menu["items"]]
        self.assertEqual(labels[1:], [(f"Allow ads on {NEWS}", True), ("Ad blocking settings", True)])
        self.assertFalse(labels[0][1])  # What was blocked: not a command.
        self.assertRegex(labels[0][0], r"^\d+ ads blocked on this page$")
        changed = self.wait_since(m, "ads_allowed_changed")
        self.assertEqual((changed["site"], changed["allowed"], changed["ok"]), (NEWS, True, True))
        # The page reloads, with its ads.
        wait_for(lambda: self.sites.seen(ADS, "/allow.gif"), 15, "the ad loaded")
        wait_state(self.shell, lambda s: (s["nav"].get("shield") or {}).get("allowed"), "the shield: allowed")
        self.assertEqual(self.get_setting("privacy.adsAllowedSites"), f"{NEWS}\n")
        # Saved: other tabs of normal windows allow them too.
        self.fresh_tab(self.shell)
        seen = len(self.sites.seen(ADS, "/allow.gif"))
        m = self.mark()
        self.go(url)
        wait_for(lambda: len(self.sites.seen(ADS, "/allow.gif")) > seen, 15, "the ad in a new tab")
        self.assertEqual(self.since(m, "ad_blocked"), [])

        menu = self.menu(f"Block ads on {NEWS} again")
        self.assertEqual([i["label"] for i in menu["items"]][1:],
                         [f"Block ads on {NEWS} again", "Ad blocking settings"])
        changed = self.wait_since(m, "ads_allowed_changed", allowed=False)
        self.assertTrue(changed["ok"])
        wait_for(lambda: self.blocked_urls(m) >= self.ad_urls("allow"), 15, "the ads blocked again")
        self.assertEqual(self.get_setting("privacy.adsAllowedSites"), "")

    def test_a_site_covers_its_subdomains(self) -> None:
        self.addCleanup(lambda: bridge_call(self.settings_page(), "settings.set",
                                            {"key": "privacy.adsAllowedSites", "value": ""}))
        r = bridge_call(self.settings_page(), "settings.set",
                        {"key": "privacy.adsAllowedSites", "value": "example.com\n"})
        self.assertTrue(r["ok"], r)
        self.fresh_tab(self.shell)
        url = self.article("www")
        self.go(url)
        wait_for(lambda: self.sites.seen(ADS, "/www.gif"), 15, "ads on a subdomain of an allowed site")
        # Blocking them again on news.example.com removes example.com.
        menu = self.menu(f"Block ads on {NEWS} again")
        self.assertIn(f"Block ads on {NEWS} again", [i["label"] for i in menu["items"]])
        wait_for(lambda: self.get_setting("privacy.adsAllowedSites") == "", 10, "the entry removed")

    def test_the_menu_only_works_on_web_pages(self) -> None:
        r = bridge_call(self.shell, "adblock.menu", {"x": 10, "y": 10})
        self.assertFalse(r["ok"])
        self.assertEqual(shell_state(self.shell)["nav"].get("shield"), None)

    def test_invalid_allowed_sites_are_refused(self) -> None:
        page = self.settings_page()
        for bad in ("not a site\n", "https://example.com/\n", "*.example.com\n", "a\n" * 1001):
            r = bridge_call(page, "settings.set", {"key": "privacy.adsAllowedSites", "value": bad})
            self.assertFalse(r["ok"], bad[:40])
        self.assertEqual(self.get_setting("privacy.adsAllowedSites"), "")


class WorkerTest(AdBlockBase):
    """Requests a service worker makes itself: no tab sees them."""

    def worker_page(self, name: str) -> str:
        """A page on NEWS whose service worker fetches an ad and a tracker
        while it installs, and an ordinary file."""
        s = self.sites
        cors = {"Access-Control-Allow-Origin": "*", **NO_STORE}
        s.raw(ADS, f"/{name}-sw.gif", 200, {"Content-Type": "image/gif", **cors}, GIF)
        s.raw(GA, f"/{name}-sw.js", 200, {"Content-Type": "text/javascript", **cors}, b"1")
        s.raw(CDN, f"/{name}-sw.txt", 200, {"Content-Type": "text/plain", **cors}, b"ok")
        fetches = ", ".join(f"fetch('{u}').catch(() => 0)" for u in (
            self.https(ADS, f"/{name}-sw.gif"), self.https(GA, f"/{name}-sw.js"),
            self.https(CDN, f"/{name}-sw.txt")))
        s.raw(NEWS, f"/{name}/sw.js", 200, {"Content-Type": "text/javascript", **NO_STORE},
              f"self.addEventListener('install', e => e.waitUntil(Promise.all([{fetches}])));".encode())
        s.html(NEWS, f"/{name}/", """<!doctype html><title>worker</title><script>
            navigator.serviceWorker.register('sw.js').then(
              () => document.title = 'registered', e => document.title = 'failed ' + e);</script>""")
        return self.https(NEWS, f"/{name}/")

    def run_worker(self, name: str, shell=None) -> int:
        url = self.worker_page(name)
        m = self.mark()
        self.go(url, shell)
        page = self.tf.page(lambda t: t["url"] == url)
        wait_for(lambda: page.evaluate("document.title") == "registered", 15, "the worker registered")
        wait_for(lambda: self.sites.seen(CDN, f"/{name}-sw.txt"), 15, "the worker's ordinary request")
        return m

    def test_a_service_workers_requests_are_filtered(self) -> None:
        m = self.run_worker("normal")
        ad = self.wait_since(m, "ad_blocked", role="context")
        self.assertEqual((ad["url"], ad["why"]), (self.https(ADS, "/normal-sw.gif"), "||adnxs.com^"))
        tracker = self.wait_since(m, "tracker_blocked", role="context")
        self.assertEqual(tracker["url"], self.https(GA, "/normal-sw.js"))
        time.sleep(0.5)
        self.assertEqual(self.sites.seen(ADS, "/normal-sw.gif"), [])
        self.assertEqual(self.sites.seen(GA, "/normal-sw.js"), [])

    def test_allowed_sites_apply_to_their_workers(self) -> None:
        self.addCleanup(lambda: bridge_call(self.settings_page(), "settings.set",
                                            {"key": "privacy.adsAllowedSites", "value": ""}))
        r = bridge_call(self.settings_page(), "settings.set",
                        {"key": "privacy.adsAllowedSites", "value": f"{NEWS}\n"})
        self.assertTrue(r["ok"], r)
        self.fresh_tab(self.shell)
        m = self.run_worker("allowed")
        wait_for(lambda: self.sites.seen(ADS, "/allowed-sw.gif"), 15, "the worker's ad")
        self.assertEqual(self.since(m, "ad_blocked"), [])
        # Ads, not trackers.
        self.wait_since(m, "tracker_blocked", role="context")

    def test_private_windows_workers_are_filtered(self) -> None:
        known = self.tf.shell_ids()
        self.assertTrue(bridge_call(self.shell, "window.newPrivate")["ok"])
        shell = self.tf.new_shell(known)
        wait_state(shell, lambda st: st["private"] and st["tabs"], "the private window")
        m = self.run_worker("private", shell)
        ad = self.wait_since(m, "ad_blocked", role="context")
        self.assertEqual(ad["url"], self.https(ADS, "/private-sw.gif"))
        self.wait_since(m, "tracker_blocked", role="context")
        self.assertEqual(self.sites.seen(ADS, "/private-sw.gif"), [])


class AdBlockOffTest(AdBlockBase):
    def test_turning_ad_blocking_off(self) -> None:
        self.setting("privacy.blockAds", False)
        url = self.article("off")
        m = self.mark()
        self.go(url)
        wait_for(lambda: self.sites.seen(ADS, "/off.gif") and self.sites.seen(ADS_SUB, "/off.js"), 15,
                 "the ads loaded")
        self.assertEqual(self.since(m, "ad_blocked"), [])
        self.assertEqual(self.since(m, "ads_hidden"), [])
        st = wait_state(self.shell, lambda s: s["nav"].get("shield") is not None, "the shield")
        self.assertFalse(st["nav"]["shield"]["enabled"])
        menu = self.menu(None)
        self.assertEqual([i["label"] for i in menu["items"]], ["Ad blocking is off", "Ad blocking settings"])


class PrivateWindowAdBlockTest(AdBlockBase):
    def open_private(self):
        known = self.tf.shell_ids()
        created = len(self.tf.events("window_created", private=True))
        press(self.shell, "n", CTRL | SHIFT)
        wait_for(lambda: len(self.tf.events("window_created", private=True)) > created, 15, "private window")
        shell = self.tf.new_shell(known)
        wait_state(shell, lambda s: s["private"], "private shell state")
        return shell

    def test_private_windows_block_ads_and_keep_their_own_list(self) -> None:
        url = self.article("private")
        shell = self.open_private()
        m = self.mark()
        self.go(url, shell)
        wait_for(lambda: self.blocked_urls(m) >= self.ad_urls("private"), 15, "ads blocked in a private window")
        self.assertEqual(self.sites.seen(ADS), [])

        menu = self.menu(f"Allow ads on {NEWS} until this window closes", shell)
        self.assertEqual(menu["picked"] > 0, True)
        changed = self.wait_since(m, "ads_allowed_changed")
        self.assertEqual((changed["site"], changed["allowed"], changed["ok"]), (NEWS, True, True))
        wait_for(lambda: self.sites.seen(ADS, "/private.gif"), 15, "the ad in the private window")
        # Nothing saved, and normal windows still block them.
        self.assertEqual(self.get_setting("privacy.adsAllowedSites"), "")
        m = self.mark()
        self.go(url)
        wait_for(lambda: self.blocked_urls(m) >= self.ad_urls("private"), 15, "ads blocked in the normal window")

        # Another private window has its own list.
        other = self.open_private()
        seen = len(self.sites.seen(ADS))
        m = self.mark()
        self.go(url, other)
        wait_for(lambda: self.blocked_urls(m) >= self.ad_urls("private"), 15, "ads blocked in another private window")
        self.assertEqual(len(self.sites.seen(ADS)), seen)


if __name__ == "__main__":
    unittest.main()
