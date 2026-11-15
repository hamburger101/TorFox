# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Hardening of TorFox's own pages: strict CSP, Trusted Types, response
headers, and a working UI under those restrictions."""

from __future__ import annotations

import json
import pathlib
import re
import sys
import unittest

from harness import TorFox, bridge_call, wait_for

REPO = pathlib.Path(__file__).resolve().parents[2]


def expected_csp() -> str:
    header = (REPO / "src" / "browser" / "scheme" / "internal_scheme.h").read_text(encoding="utf-8")
    block = header.split("kInternalCsp =", 1)[1].split(";\n", 1)[0]
    return "".join(re.findall(r'"([^"]*)"', block))


class InternalPageHardeningTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tf = TorFox().start()
        cls.shell = cls.tf.shell()
        cls.newtab = cls.tf.page_at("torfox://newtab/")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tf.__exit__(None, None, None)

    def response_headers(self, session, url: str) -> dict[str, str]:
        session.send("Network.enable")
        session.send("Page.reload")
        ev = self.tf.browser.wait_event(
            "Network.responseReceived",
            lambda e: e.get("sessionId") == session.session_id and e["params"]["response"]["url"] == url)
        session.send("Network.disable")
        wait_for(lambda: session.evaluate("document.readyState") == "complete", 10, "reload")
        return {k.lower(): v for k, v in ev["params"]["response"]["headers"].items()}

    def test_security_headers(self) -> None:
        headers = self.response_headers(self.newtab, "torfox://newtab/")
        self.assertEqual(headers.get("content-security-policy"), expected_csp())
        self.assertEqual(headers.get("x-frame-options"), "DENY")
        self.assertEqual(headers.get("x-content-type-options"), "nosniff")
        self.assertEqual(headers.get("cross-origin-opener-policy"), "same-origin")
        self.assertEqual(headers.get("cross-origin-embedder-policy"), "require-corp")
        self.assertEqual(headers.get("cross-origin-resource-policy"), "same-origin")
        self.assertEqual(headers.get("cache-control"), "no-store")
        self.assertIn("camera=()", headers.get("permissions-policy", ""))

    def test_csp_is_strict(self) -> None:
        csp = expected_csp()
        for directive in ("default-src 'none'", "script-src 'self'", "connect-src 'none'",
                          "frame-ancestors 'none'", "object-src 'none'", "base-uri 'none'",
                          "require-trusted-types-for 'script'"):
            self.assertIn(directive, csp)
        self.assertNotIn("unsafe-inline", csp)
        self.assertNotIn("unsafe-eval", csp)

    def test_eval_is_blocked(self) -> None:
        result = self.newtab.evaluate(
            "(() => { try { return String(eval('1 + 1')); } catch (e) { return 'error:' + e.name; } })()")
        self.assertEqual(result, "error:EvalError")
        result = self.newtab.evaluate(
            "(() => { try { return String(new Function('return 2')()); } catch (e) { return 'error:' + e.name; } })()")
        self.assertEqual(result, "error:EvalError")

    def test_inline_script_is_blocked(self) -> None:
        result = self.newtab.evaluate("""
          (() => {
            try {
              const s = document.createElement("script");
              s.textContent = "window.__inline = 1";
              document.head.appendChild(s);
            } catch (e) { return "error:" + e.name; }
            return String(window.__inline);
          })()""")
        # Trusted Types refuses the string; CSP would block it even if not.
        self.assertIn(result, ("error:TypeError", "undefined"))

    def test_html_sinks_need_trusted_types(self) -> None:
        result = self.newtab.evaluate("""
          (() => { try { document.body.insertAdjacentHTML("beforeend", "<b>x</b>"); return "allowed"; }
                   catch (e) { return "error:" + e.name; } })()""")
        self.assertEqual(result, "error:TypeError")
        result = self.newtab.evaluate("""
          (() => { try { trustedTypes.createPolicy("evil", {createHTML: s => s}); return "allowed"; }
                   catch (e) { return "error:" + e.name; } })()""")
        self.assertEqual(result, "error:TypeError")

    def test_no_subframes(self) -> None:
        result = self.newtab.evaluate("""
          new Promise((resolve) => {
            const f = document.createElement("iframe");
            f.src = "torfox://newtab/";
            f.onload = () => { try { resolve(f.contentWindow.location.href); } catch (e) { resolve("blocked"); } };
            document.body.appendChild(f);
            setTimeout(() => resolve("timeout"), 3000);
          })""")
        self.assertNotEqual(result, "torfox://newtab/")

    def test_ui_renders_under_the_policy(self) -> None:
        self.assertTrue(self.shell.evaluate("!!customElements.get('tf-shell')"))
        self.assertTrue(self.newtab.evaluate("!!customElements.get('tf-newtab')"))
        tabs = self.shell.evaluate(
            "document.querySelector('tf-shell').shadowRoot.querySelectorAll('.tab').length")
        self.assertGreaterEqual(tabs, 1)

    def test_pages_set_no_style_attributes(self) -> None:
        # The CSP refuses style attributes, so an element styled through one
        # stays unstyled. lit's styleMap wrote its first value that way (every
        # tab, and progress bars when first drawn); see Stage 7 findings §3.7.
        for command, url in (("app.openSettings", "torfox://settings/"),
                             ("app.openDownloads", "torfox://downloads/"),
                             ("app.openHistory", "torfox://history/"),
                             ("app.openBookmarks", "torfox://bookmarks/")):
            self.assertTrue(bridge_call(self.shell, command)["ok"], command)
            page = self.tf.page_at(url)
            wait_for(lambda: page.evaluate("document.readyState") == "complete", 10, url)
        self.assertTrue(bridge_call(self.shell, "tab.new")["ok"])
        wait_for(lambda: self.shell.evaluate(
            "document.querySelector('tf-shell').shadowRoot.querySelectorAll('.tab').length") >= 6,
            10, "the new tabs")
        styles = [e["message"] for e in self.tf.events("console") if "inline style" in e.get("message", "")]
        self.assertEqual(styles, [])

    def test_shell_layout(self) -> None:
        layout = self.tf.events("window_layout")[-1]
        self.assertEqual(layout["shell"][3], 82)
        self.assertEqual(layout["tab"][2], layout["content"][2])
        self.assertEqual(layout["tab"][3], layout["content"][3])
        self.assertGreater(layout["tab"][3], 200)
        self.assertEqual(self.shell.evaluate("innerHeight"), 82)
        self.assertEqual(self.shell.evaluate("document.documentElement.scrollHeight"), 82)


class SettingsPageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tf = TorFox().start()
        cls.shell = cls.tf.shell()
        assert bridge_call(cls.shell, "app.openSettings")["ok"]
        cls.page = cls.tf.page_at("torfox://settings/")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tf.__exit__(None, None, None)

    def root(self, expr: str):
        return self.page.evaluate(f"(() => {{ const r = document.querySelector('tf-settings').shadowRoot; return {expr}; }})()")

    def test_all_eight_sections(self) -> None:
        names = self.root("[...r.querySelectorAll('.nav-item span')].map(s => s.textContent)")
        self.assertEqual(names, ["General", "Search Engine", "Privacy & Security", "Downloads",
                                 "Tor Network", "Appearance", "Updates", "About TorFox"])

    def test_planned_settings_are_disabled_and_labelled(self) -> None:
        # Since Stage 7 every setting is active; a setting added before its
        # feature exists must be shown disabled and labelled "Not active yet".
        data = json.loads(bridge_call(self.page, "settings.get")["response"])
        planned = [s["key"] for s in data["settings"] if s["status"] == "planned"]
        self.assertEqual(planned, [])
        for section in ("general", "search", "privacy", "downloads", "tor", "updates"):
            self.page.evaluate(f"document.querySelector('tf-settings').section = '{section}'; 1")
            wait_for(lambda: self.root("r.querySelector('.page-title').textContent") != "", 5, "render")
            self.page.evaluate("document.querySelector('tf-settings').updateComplete")
            enabled = self.root("[...r.querySelectorAll('.row.planned input, .row.planned textarea')].filter(e => !e.disabled).length")
            self.assertEqual(enabled, 0, section)
            unlabelled = self.root("[...r.querySelectorAll('.row.planned')].filter(row => !row.querySelector('.badge')).length")
            self.assertEqual(unlabelled, 0, section)

    def test_only_implemented_settings_are_active(self) -> None:
        # Settings become active only once the behaviour behind them is built
        # and tested (the bookmarks bar toggle: tests/e2e/test_places.py;
        # downloads: tests/e2e/test_downloads.py; privacy and permissions:
        # tests/e2e/test_privacy.py; Tor: tests/e2e/test_tor.py; on startup:
        # tests/e2e/test_startup.py; search engine: tests/e2e/test_browsing.py;
        # updates: tests/e2e/test_updates.py).
        data = json.loads(bridge_call(self.page, "settings.get")["response"])
        self.assertEqual(sorted(s["key"] for s in data["settings"] if s["status"] == "active"),
                         sorted(["general.startupPage", "search.engine",
                                 "updates.enabled", "updates.autoCheck", "updates.notify",
                                 "appearance.theme", "appearance.showBookmarksBar",
                                 "downloads.askBeforeDownload", "downloads.disableDownloads",
                                 "privacy.httpsFirst", "privacy.blockTrackers",
                                 "privacy.blockThirdPartyCookies", "privacy.blockPopups",
                                 "privacy.useSystemProxy",
                                 "privacy.clearOnExit.history", "privacy.clearOnExit.cookies",
                                 "privacy.clearOnExit.cache", "permissions.camera",
                                 "permissions.microphone", "permissions.clipboard",
                                 "tor.startMode", "tor.bridges.mode",
                                 "tor.bridges.builtinType", "tor.bridges.custom"]))

    def test_about_is_honest(self) -> None:
        self.page.evaluate("document.querySelector('tf-settings').section = 'about'; document.querySelector('tf-settings').updateComplete")
        text = self.root("r.querySelector('main').textContent")
        self.assertIn("not Tor Browser", text)
        self.assertIn("test build", text)


if __name__ == "__main__":
    unittest.main()
