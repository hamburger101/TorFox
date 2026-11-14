# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""No telemetry, no background traffic: an idle TorFox contacts nobody.

TorFox starts with a NetLog recording every network event, sits idle on the
new tab page, opens Settings, and closes cleanly. The log must not name a
single non-loopback destination, and no .onion name may reach the resolver.
"""

from __future__ import annotations

import os
import sys
import time
import unittest

import netlog
from harness import Fixture, TorFox, bridge_call

IDLE_SECONDS = float(os.environ.get("TORFOX_QUIET_SECONDS", "20"))
ONION = "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion"


def windows_proxy_autodetect() -> bool:
    """True if Windows' "Automatically detect settings" proxy option is on."""
    if sys.platform != "win32":
        return False
    import winreg
    key = r"Software\Microsoft\Windows\CurrentVersion\Internet Settings\Connections"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as k:
            data, _ = winreg.QueryValueEx(k, "DefaultConnectionSettings")
    except OSError:
        return False
    return len(data) > 8 and bool(data[8] & 0x08)


def known_gap_if(condition: bool):
    """Marks a test as an expected failure: a known gap, listed in
    docs/FEATURE_STATUS.md, that applies only when |condition| holds. If the
    gap is ever closed, unittest reports an "unexpected success" and the run
    fails, so the marker cannot outlive the gap."""
    def wrap(fn):
        return unittest.expectedFailure(fn) if condition else fn
    return wrap


def describe(found: dict[str, set[str]]) -> str:
    return ", ".join(f"{h} ({', '.join(sorted(t))})" for h, t in sorted(found.items()))


class QuietStartupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        tf = TorFox()
        log_path = tf.user_data / "netlog.json"
        tf.args += [f"--log-net-log={log_path}", "--net-log-capture-mode=Everything"]
        with tf:
            shell = tf.shell()
            time.sleep(IDLE_SECONDS / 2)
            assert bridge_call(shell, "app.openSettings")["ok"]
            tf.page_at("torfox://settings/")
            time.sleep(IDLE_SECONDS / 2)
            assert tf.close() == 0
            cls.log = netlog.load(log_path)

    def test_netlog_recorded_the_session(self) -> None:
        # The network service started and logged its configuration, so an
        # empty result below means "no traffic", not "no log".
        self.assertTrue(netlog.has_event(self.log, "DNS_CONFIG_CHANGED"))
        self.assertTrue(netlog.has_event(self.log, "PROXY_CONFIG_CHANGED"))

    def test_idle_browser_makes_no_network_connections(self) -> None:
        # Proxy auto-discovery is checked on its own below.
        found = netlog.destinations(self.log, netlog.proxy_autodiscovery_sources(self.log))
        self.assertEqual(found, {}, "unexpected network destinations: " + describe(found))

    def test_service_contexts_never_autodiscover_proxies(self) -> None:
        # TorFox's own contexts (internal pages, the global profile and the
        # system network context) are configured for direct connections, so
        # at most one network context, the web one, ever uses the system's
        # auto-detect setting (see the known gap below).
        self.assertLessEqual(netlog.system_autodetect_contexts(self.log), 1)

    # Web tabs connect directly unless Settings > Privacy & Security > Use
    # the system's proxy settings is on, so no context runs WPAD (DHCP, then a
    # "wpad" name lookup), even with Windows' "Automatically detect
    # settings" on (STAGE1_FINDINGS.md §4, STAGE4_FINDINGS.md).
    def test_idle_browser_does_not_autodiscover_proxies(self) -> None:
        self.assertEqual(netlog.proxy_autodiscovery_sources(self.log), set())
        self.assertEqual(netlog.system_autodetect_contexts(self.log), 0)


class SystemProxyTest(unittest.TestCase):
    @unittest.skipUnless(windows_proxy_autodetect(), "needs Windows' proxy auto-detect on")
    def test_system_proxy_setting_follows_windows(self) -> None:
        # The control for the test above: with the setting on, TorFox uses
        # Windows' settings, and so (with auto-detect on) looks for a proxy.
        tf = TorFox()
        (tf.user_data / "settings.json").write_text(
            '{"format": 1, "settings": {"privacy.useSystemProxy": true}}', encoding="utf-8")
        log_path = tf.user_data / "netlog.json"
        tf.args += [f"--log-net-log={log_path}", "--net-log-capture-mode=Everything"]
        with tf:
            tf.shell()
            time.sleep(IDLE_SECONDS / 2)
            self.assertEqual(tf.wait_log("proxy_mode_written")["mode"], "system")
            self.assertEqual(tf.close(), 0)
            log = netlog.load(log_path)  # Before the profile is deleted.
        self.assertNotEqual(netlog.proxy_autodiscovery_sources(log), set())


class OnionResolverTest(unittest.TestCase):
    def onion_netlog(self, extra_args: list[str]) -> dict:
        """Loads a page that requests .onion hosts every way it can and
        returns the NetLog of the whole session."""
        with Fixture() as fx:
            page = fx.html("/onion", f"""<!doctype html><title>onion</title>
              <img src="http://{ONION}/a.png"><script>fetch("https://{ONION}/b").catch(() => {{}});
              try {{ new WebSocket("wss://{ONION}/c"); }} catch (e) {{}}</script>""")
            tf = TorFox(start_url=page, extra_args=extra_args)
            log_path = tf.user_data / "netlog.json"
            tf.args += [f"--log-net-log={log_path}", "--net-log-capture-mode=Everything"]
            with tf:
                tf.shell()
                web = tf.page_at(page)
                bridge_call(tf.shell(), "nav.go", {"input": ONION})
                web.send("Page.navigate", {"url": f"http://{ONION}/"})
                time.sleep(2.0)
                self.assertEqual(tf.close(), 0)
                return netlog.load(log_path)

    def test_onion_names_never_reach_the_resolver(self) -> None:
        log = self.onion_netlog([])
        resolved = netlog.resolved_hosts(log)
        self.assertFalse([h for h in resolved if "onion" in h], resolved)
        # TorFox's request guards cancel navigations and subresources before
        # the network stack sees them. WebSocket handshakes are the exception:
        # CEF does not pass them through OnBeforeResourceLoad, so they reach
        # the network stack and only the host-resolver rule stops them.
        urls = netlog.requested_urls(log)
        self.assertFalse([u for u in urls if ONION in u and not u.startswith(("ws:", "wss:"))], urls)
        self.assertTrue([u for u in urls if u.startswith(f"wss://{ONION}/")],
                        "the WebSocket attempt was not seen; the test page did not run")
        self.assertFalse(netlog.connected_addresses(log, ONION))

    def test_resolver_rule_alone_keeps_onion_names_local(self) -> None:
        # With TorFox's request guards switched off (test builds only), the
        # requests do reach the network stack; the host-resolver rule TorFox
        # always adds must still keep every .onion name away from the resolver.
        log = self.onion_netlog(["--torfox-test-skip-guards=nav,resource"])
        urls = netlog.requested_urls(log)
        self.assertTrue([u for u in urls if u.startswith(f"http://{ONION}/")],
                        "the requests never reached the network stack")
        resolved = netlog.resolved_hosts(log)
        self.assertFalse([h for h in resolved if "onion" in h], resolved)
        self.assertFalse(netlog.connected_addresses(log, ONION))


if __name__ == "__main__":
    unittest.main()
