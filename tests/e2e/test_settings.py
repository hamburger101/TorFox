# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Settings persist across real restarts, apply live, and survive damage."""

from __future__ import annotations

import json
import pathlib
import shutil
import sys
import tempfile
import unittest

from harness import TorFox, bridge_call, wait_for

HERE = pathlib.Path(__file__).resolve().parent
FAKE_TOR = HERE / "fake_tor.py"
OBFS4 = ("obfs4 192.0.2.10:443 0123456789ABCDEF0123456789ABCDEF01234567 "
         "cert=AAAAbbbbCCCCddddEEEEffffGGGGhhhhIIIIjjjjKKKKllllMMMMnnnnOOOOpppp iat-mode=0")


def settings_file(user_data: pathlib.Path) -> pathlib.Path:
    return user_data / "settings.json"


class SettingsPersistenceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-settings-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.user_data, ignore_errors=True)

    def open_settings(self, tf: TorFox):
        shell = tf.shell()
        self.assertTrue(bridge_call(shell, "app.openSettings")["ok"])
        return shell, tf.page_at("torfox://settings/")

    def theme_of(self, session) -> str:
        return session.evaluate("document.documentElement.dataset.theme")

    def test_theme_persists_across_restart(self) -> None:
        with TorFox(user_data=self.user_data) as tf:
            shell, page = self.open_settings(tf)
            self.assertEqual(self.theme_of(shell), "dark")  # Default.
            r = bridge_call(page, "settings.set", {"key": "appearance.theme", "value": "light"})
            self.assertTrue(r["ok"], r)
            # Applied live to every open internal page.
            wait_for(lambda: self.theme_of(shell) == "light", 5, "shell theme")
            wait_for(lambda: self.theme_of(page) == "light", 5, "settings theme")
            saved = json.loads(settings_file(self.user_data).read_text(encoding="utf-8"))
            self.assertEqual(saved, {"format": 1, "settings": {"appearance.theme": "light"}})
            self.assertEqual(tf.close(), 0)

        with TorFox(user_data=self.user_data) as tf:
            shell, page = self.open_settings(tf)
            data = json.loads(bridge_call(page, "settings.get")["response"])
            theme = next(s for s in data["settings"] if s["key"] == "appearance.theme")
            self.assertEqual(theme["value"], "light")
            wait_for(lambda: self.theme_of(shell) == "light", 5, "theme after restart")

    def test_the_ui_control_changes_the_setting(self) -> None:
        with TorFox(user_data=self.user_data) as tf:
            shell, page = self.open_settings(tf)
            page.evaluate("document.querySelector('tf-settings').section = 'appearance'; "
                          "document.querySelector('tf-settings').updateComplete")
            page.evaluate("""
              (() => { const r = document.querySelector('tf-settings').shadowRoot;
                       const radio = [...r.querySelectorAll('input[type=radio]')].find(i => i.value === 'system');
                       radio.click(); })()""", user_gesture=True)
            wait_for(lambda: self.theme_of(shell) == "system", 5, "system theme")
            saved = json.loads(settings_file(self.user_data).read_text(encoding="utf-8"))
            self.assertEqual(saved["settings"].get("appearance.theme"), "system")

    def test_default_is_not_written(self) -> None:
        with TorFox(user_data=self.user_data) as tf:
            shell, page = self.open_settings(tf)
            self.assertTrue(bridge_call(page, "settings.set", {"key": "appearance.theme", "value": "light"})["ok"])
            self.assertTrue(bridge_call(page, "settings.set", {"key": "appearance.theme", "value": "dark"})["ok"])
            saved = json.loads(settings_file(self.user_data).read_text(encoding="utf-8"))
            self.assertEqual(saved, {"format": 1, "settings": {}})

    def test_corrupt_file_is_backed_up_and_reset(self) -> None:
        self.user_data.mkdir(parents=True, exist_ok=True)
        settings_file(self.user_data).write_text("{ this is not json", encoding="utf-8")
        with TorFox(user_data=self.user_data) as tf:
            loaded = tf.wait_log("settings_loaded")
            self.assertEqual(loaded["outcome"], 3)  # kCorruptReset
            shell, page = self.open_settings(tf)
            self.assertEqual(self.theme_of(shell), "dark")
        self.assertEqual((self.user_data / "settings.json.corrupt").read_text(encoding="utf-8"),
                         "{ this is not json")

    def test_invalid_entries_fall_back_to_defaults(self) -> None:
        self.user_data.mkdir(parents=True, exist_ok=True)
        settings_file(self.user_data).write_text(json.dumps({"format": 1, "settings": {
            "appearance.theme": "neon",
            "downloads.disableDownloads": "yes",
            "unknown.key": True,
        }}), encoding="utf-8")
        with TorFox(user_data=self.user_data) as tf:
            loaded = tf.wait_log("settings_loaded")
            self.assertEqual(loaded["outcome"], 2)  # kRepaired
            shell, page = self.open_settings(tf)
            data = json.loads(bridge_call(page, "settings.get")["response"])
            values = {s["key"]: s["value"] for s in data["settings"]}
            self.assertEqual(values["appearance.theme"], "dark")
            self.assertIs(values["downloads.disableDownloads"], False)
        repaired = json.loads(settings_file(self.user_data).read_text(encoding="utf-8"))
        self.assertEqual(repaired, {"format": 1, "settings": {}})

    def test_every_active_setting_persists_across_restart(self) -> None:
        # Every setting that is active gets a value other than its default
        # through the settings page, as a user would set it; after a restart
        # each one reads back. Tor starts at launch after the restart (a
        # scripted tor here, so nothing reaches the network).
        command = self.user_data.parent / f"{self.user_data.name}-tor.json"
        plan = self.user_data.parent / f"{self.user_data.name}-plan.json"
        plan.write_text(json.dumps({"relay_port": 9}), encoding="utf-8")
        command.write_text(json.dumps({"exe": sys.executable, "args": [
            "-I", str(FAKE_TOR), "--plan", str(plan),
            "--record", str(self.user_data.parent / f"{self.user_data.name}-tor.jsonl")]}),
            encoding="utf-8")
        self.addCleanup(command.unlink, True)
        self.addCleanup(plan.unlink, True)
        args = [f"--torfox-test-tor-command={command}"]
        chosen: dict[str, object] = {}
        with TorFox(user_data=self.user_data, extra_args=args) as tf:
            _, page = self.open_settings(tf)
            data = json.loads(bridge_call(page, "settings.get")["response"])
            active = [s for s in data["settings"] if s["status"] == "active"]
            self.assertGreaterEqual(len(active), 20)
            # Custom bridge lines before the custom bridge mode.
            active.sort(key=lambda s: s["key"] != "tor.bridges.custom")
            for s in active:
                if s["type"] == "bool":
                    value = not s["default"]
                elif s["type"] == "enum":
                    value = next(v for v in s["values"] if v != s["default"])
                else:
                    self.assertEqual(s["key"], "tor.bridges.custom", "a string setting this test does not know")
                    value = OBFS4
                r = bridge_call(page, "settings.set", {"key": s["key"], "value": value})
                self.assertTrue(r["ok"], (s["key"], r))
                chosen[s["key"]] = value
            self.assertEqual(tf.close(), 0)

        with TorFox(user_data=self.user_data, extra_args=args) as tf:
            _, page = self.open_settings(tf)
            data = json.loads(bridge_call(page, "settings.get")["response"])
            values = {s["key"]: s["value"] for s in data["settings"]}
            for key, value in chosen.items():
                self.assertEqual(values[key], value, key)
            # And they act: tor started at launch, with the custom bridge.
            tf.wait_log("tor_controlled", timeout=30)


if __name__ == "__main__":
    unittest.main()
