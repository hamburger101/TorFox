# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""The validated bridge: only well-formed, allowlisted messages from the
right internal page are acted on; everything else is rejected and logged."""

from __future__ import annotations

import json
import unittest

from harness import TorFox, bridge_call


class BridgeValidationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tf = TorFox().start()
        cls.shell = cls.tf.shell()
        cls.newtab = cls.tf.page_at("torfox://newtab/")
        result = bridge_call(cls.shell, "app.openSettings")
        assert result["ok"], result
        cls.settings = cls.tf.page_at("torfox://settings/")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tf.__exit__(None, None, None)

    def assert_rejected(self, session, cmd: str = "", args=None, *, raw: str | None = None,
                        persistent: bool = False, reason: str | None = None) -> None:
        before = len(self.tf.events("bridge_reject"))
        result = bridge_call(session, cmd, args, raw=raw, persistent=persistent)
        self.assertFalse(result["ok"], result)
        self.assertEqual(result["code"], 403, result)
        self.assertEqual(result["message"], "rejected", result)
        rejects = self.tf.events("bridge_reject")[before:]
        self.assertTrue(rejects, "rejection was not logged")
        if reason:
            self.assertEqual(rejects[-1]["reason"], reason, rejects[-1])

    # Accepted -------------------------------------------------------------------
    def test_valid_calls_succeed(self) -> None:
        r = bridge_call(self.settings, "app.info")
        self.assertTrue(r["ok"], r)
        info = json.loads(r["response"])
        self.assertEqual(info["name"], "TorFox")
        self.assertTrue(info["testBuild"])
        r = bridge_call(self.settings, "settings.get")
        self.assertTrue(r["ok"], r)
        self.assertIn("appearance.theme", [s["key"] for s in json.loads(r["response"])["settings"]])

    # Malformed ------------------------------------------------------------------
    def test_malformed_json(self) -> None:
        self.assert_rejected(self.settings, raw="{not json", reason="malformed")
        self.assert_rejected(self.settings, raw="", reason="malformed")
        self.assert_rejected(self.settings, raw="[]", reason="malformed")
        self.assert_rejected(self.settings, raw='"app.info"', reason="malformed")

    def test_unexpected_members(self) -> None:
        self.assert_rejected(self.settings, raw='{"cmd":"app.info","extra":1}')
        self.assert_rejected(self.settings, raw='{"cmd":"app.info","args":{},"__proto__":{}}')

    def test_duplicate_keys_are_not_a_bypass(self) -> None:
        # Whatever the parser keeps, the result must still be a valid command
        # for this page or be rejected; it must never reach another page's command.
        r = bridge_call(self.settings, "", raw='{"cmd":"tab.close","cmd":"app.info"}')
        if r["ok"]:
            self.assertIn("TorFox", r["response"])
        else:
            self.assertEqual(r["code"], 403)

    def test_oversized_message(self) -> None:
        self.assert_rejected(self.settings, "settings.set",
                             {"key": "appearance.theme", "value": "x" * (70 * 1024)},
                             reason="too_large")

    def test_deep_nesting(self) -> None:
        deep = "1"
        for _ in range(20):
            deep = "[" + deep + "]"
        self.assert_rejected(self.settings, raw='{"cmd":"settings.set","args":{"key":"k","value":%s}}' % deep)

    # Commands and arguments ------------------------------------------------------------
    def test_unknown_command(self) -> None:
        self.assert_rejected(self.settings, "app.quit", reason="unknown_command")
        self.assert_rejected(self.settings, "settings.get ", reason="unknown_command")
        self.assert_rejected(self.settings, "SETTINGS.GET", reason="unknown_command")

    def test_commands_of_other_pages(self) -> None:
        # Shell-only commands from internal tabs.
        self.assert_rejected(self.settings, "tab.close", {"id": 1}, reason="unknown_command")
        self.assert_rejected(self.newtab, "tab.new", reason="unknown_command")
        self.assert_rejected(self.newtab, "settings.set", {"key": "appearance.theme", "value": "light"},
                             reason="unknown_command")
        # Settings commands from the shell.
        self.assert_rejected(self.shell, "settings.get", reason="unknown_command")
        self.assert_rejected(self.shell, "settings.set", {"key": "appearance.theme", "value": "light"},
                             reason="unknown_command")

    def test_persistence_must_match(self) -> None:
        self.assert_rejected(self.settings, "settings.get", persistent=True, reason="persistence_mismatch")
        self.assert_rejected(self.shell, "state.subscribe", persistent=False, reason="persistence_mismatch")

    def test_argument_schema(self) -> None:
        self.assert_rejected(self.shell, "tab.close", {}, reason="missing_argument")
        self.assert_rejected(self.shell, "tab.close", {"id": "1"}, reason="bad_argument_type")
        self.assert_rejected(self.shell, "tab.close", {"id": 1.5}, reason="bad_argument_type")
        self.assert_rejected(self.shell, "tab.close", {"id": 0}, reason="argument_out_of_range")
        self.assert_rejected(self.shell, "tab.close", {"id": -3}, reason="argument_out_of_range")
        self.assert_rejected(self.shell, "tab.close", {"id": 1, "force": True}, reason="unknown_argument")
        self.assert_rejected(self.shell, "nav.go", {"input": "x" * 9000}, reason="argument_out_of_range")
        self.assert_rejected(self.shell, "nav.go", {"input": ["https://example.com"]},
                             reason="bad_argument_type")
        self.assert_rejected(self.shell, "nav.back", {"steps": 2}, reason="unknown_argument")
        self.assert_rejected(self.settings, "settings.set", {"key": "k" * 200, "value": 1},
                             reason="argument_out_of_range")
        self.assert_rejected(self.shell, "", raw='{"cmd":"tab.close","args":[1]}')

    # Semantic checks after validation ------------------------------------------------
    def test_settings_service_refusals(self) -> None:
        for key, value, expected in (
            ("tor.startMode", "at-launch", "not_available"),
            ("permissions.location", "ask", "not_available"),
            ("appearance.theme", "purple", "invalid_value"),
            ("appearance.theme", 1, "invalid_value"),
            ("no.such.setting", True, "unknown_key"),
        ):
            r = bridge_call(self.settings, "settings.set", {"key": key, "value": value})
            self.assertFalse(r["ok"], (key, r))
            self.assertEqual(r["message"], expected, (key, r))

    def test_window_commands_validate_ids(self) -> None:
        r = bridge_call(self.shell, "tab.select", {"id": 999999})
        self.assertFalse(r["ok"])
        self.assertEqual(r["code"], 400)

    def test_rejections_change_nothing(self) -> None:
        tabs_before = sorted(t["url"] for t in self.tf.browser.targets())
        self.assert_rejected(self.newtab, "tab.new")
        self.assert_rejected(self.settings, "tab.close", {"id": 1})
        self.assertEqual(sorted(t["url"] for t in self.tf.browser.targets()), tabs_before)


if __name__ == "__main__":
    unittest.main()
