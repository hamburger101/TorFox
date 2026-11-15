# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Settings > Privacy & Security > JavaScript optimizer.

Where it is off, Chromium starts the site's renderer process with
--js-flags=--disable-optimizing-compilers, which turns off V8's optimizing compilers
(TurboFan and Maglev) in that process. These tests find each tab's renderer
(Settings > Memory's report) and read its command line from Windows: by
default Tor windows' renderers have the switch and normal and private
windows' do not; "Off everywhere" adds it to every window, "On" to none.
What the switch does inside V8 is Chromium's; it is not measured here."""

from __future__ import annotations

import json
import pathlib
import shutil
import sys
import tempfile
import unittest

from harness import Fixture, TorFox, bridge_call, wait_for, wait_state
from test_memory import processes
from test_tor import TorBase

SWITCH = "--disable-optimizing-compilers"


def tab_renderers(page, title: str | None = None, window: str | None = None) -> list[int]:
    """The renderer pids of the tabs titled |title| (or in |window| windows)."""
    r = bridge_call(page, "memory.query")
    assert r["ok"], r
    rep = json.loads(r["response"])
    return [pid for row in rep["tabs"] for t in row["tabs"]
            if (title is None or t["title"] == title) and (window is None or t["window"] == window)
            for pid in row["pids"]]


def has_switch(pid: int) -> bool:
    """True if V8 in renderer |pid| was given the switch (Chromium passes
    it in --js-flags)."""
    cmd = processes()[pid]["cmd"]
    tokens = cmd.split()
    assert "--type=renderer" in tokens, cmd
    flags = [f for t in tokens if t.startswith("--js-flags=")
             for f in t[len("--js-flags="):].strip('"').replace(",", " ").split()]
    return SWITCH in flags


@unittest.skipUnless(sys.platform == "win32", "reads process command lines")
class JsOptimizerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()
        cls.a = cls.fixture.html("/a", "<!doctype html><title>Page A</title>")
        cls.b = "http://localhost:%d/b" % cls.fixture.port
        cls.fixture.html("/b", "<!doctype html><title>Page B</title>")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.fixture.__exit__(None, None, None)

    def start(self, mode: str | None = None) -> TorFox:
        user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-jsopt-"))
        self.addCleanup(shutil.rmtree, user_data, True)
        if mode:
            (user_data / "settings.json").write_text(json.dumps(
                {"format": 1, "settings": {"privacy.jsOptimizer": mode}}), encoding="utf-8")
        tf = TorFox(user_data=user_data).start()
        self.addCleanup(tf.cleanup)
        return tf

    def load(self, shell, url: str) -> None:
        self.assertTrue(bridge_call(shell, "nav.go", {"input": url})["ok"])
        wait_state(shell, lambda s: s["nav"].get("url") == url and not any(t["loading"] for t in s["tabs"]),
                   f"{url} loaded")

    def settings(self, tf: TorFox, shell):
        self.assertTrue(bridge_call(shell, "app.openSettings")["ok"])
        return tf.page_at("torfox://settings/")

    def private_shell(self, tf: TorFox, shell):
        known = tf.shell_ids()
        self.assertTrue(bridge_call(shell, "window.newPrivate")["ok"])
        wait_for(lambda: tf.events("window_created", private=True), 15, "private window")
        private = tf.new_shell(known)
        wait_state(private, lambda s: s["private"], "private shell")
        return private

    def test_by_default_normal_and_private_windows_keep_the_optimizer(self) -> None:
        tf = self.start()
        shell = tf.shell()
        self.load(shell, self.a)
        private = self.private_shell(tf, shell)
        self.load(private, self.b)
        page = self.settings(tf, shell)
        normal = wait_for(lambda: tab_renderers(page, "Page A"), 10, "page A's renderer")
        hidden = wait_for(lambda: tab_renderers(page, window="private"), 10, "the private renderer")
        self.assertEqual([has_switch(p) for p in normal + hidden], [False] * len(normal + hidden))
        self.assertEqual([(e["context"], e["off"]) for e in tf.events("js_optimizer")], [])

    def test_off_everywhere_reaches_every_window(self) -> None:
        tf = self.start("off")
        shell = tf.shell()
        self.load(shell, self.a)
        private = self.private_shell(tf, shell)
        self.load(private, self.b)
        page = self.settings(tf, shell)
        pids = wait_for(lambda: tab_renderers(page, "Page A"), 10, "page A's renderer") + \
            wait_for(lambda: tab_renderers(page, window="private"), 10, "the private renderer")
        self.assertEqual([has_switch(p) for p in pids], [True] * len(pids))

    def test_a_change_applies_to_pages_opened_after_it(self) -> None:
        tf = self.start()
        shell = tf.shell()
        private = self.private_shell(tf, shell)
        page = self.settings(tf, shell)
        r = bridge_call(page, "settings.set", {"key": "privacy.jsOptimizer", "value": "off"})
        self.assertTrue(r["ok"], r)
        self.assertEqual(sorted((e["context"], e["off"], e["ok"]) for e in tf.events("js_optimizer")),
                         [("normal", True, True), ("private", True, True)])
        self.load(private, self.b)
        pids = wait_for(lambda: tab_renderers(page, window="private"), 10, "the private renderer")
        self.assertEqual([has_switch(p) for p in pids], [True] * len(pids))
        # A normal tab opened now, in a site no process holds yet.
        self.assertTrue(bridge_call(shell, "tab.new")["ok"])
        wait_state(shell, lambda s: s["nav"].get("url", "").startswith("torfox://newtab"), "new tab")
        self.load(shell, self.a)
        pids = wait_for(lambda: tab_renderers(page, "Page A"), 10, "page A's renderer")
        self.assertEqual([has_switch(p) for p in pids], [True] * len(pids))

    def test_settings_shows_the_choice(self) -> None:
        tf = self.start()
        page = self.settings(tf, tf.shell())
        r = bridge_call(page, "settings.get")
        self.assertTrue(r["ok"], r)
        data = json.loads(r["response"])
        entry = next(s for s in data["settings"] if s["key"] == "privacy.jsOptimizer")
        self.assertEqual(entry["value"], "off-in-tor")
        r = bridge_call(page, "settings.set", {"key": "privacy.jsOptimizer", "value": "sometimes"})
        self.assertFalse(r["ok"], r)


class TorJsOptimizerTest(TorBase):
    def tor_tab_renderers(self) -> list[int]:
        page = self.settings_page()
        return wait_for(lambda: tab_renderers(page, window="tor"), 15, "the Tor tab's renderer")

    def test_tor_windows_run_without_the_optimizer(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        self.load(shell, self.url("/page"), "Tor page")
        pids = self.tor_tab_renderers()
        self.assertEqual([has_switch(p) for p in pids], [True] * len(pids))
        # The normal window's own pages keep it.
        self.load(self.normal, self.url("/two"), "Page two")
        page = self.settings_page()
        normal = wait_for(lambda: tab_renderers(page, "Page two"), 10, "the normal page's renderer")
        self.assertEqual([has_switch(p) for p in normal], [False] * len(normal))


class TorJsOptimizerOnTest(TorBase):
    def prepare(self, tf: TorFox) -> None:
        tf.user_data.mkdir(parents=True, exist_ok=True)
        (tf.user_data / "settings.json").write_text(json.dumps(
            {"format": 1, "settings": {"privacy.jsOptimizer": "on"}}), encoding="utf-8")

    def test_on_keeps_the_optimizer_in_tor_windows(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        self.load(shell, self.url("/page"), "Tor page")
        page = self.settings_page()
        pids = wait_for(lambda: tab_renderers(page, window="tor"), 15, "the Tor tab's renderer")
        self.assertEqual([has_switch(p) for p in pids], [False] * len(pids))


if __name__ == "__main__":
    unittest.main()
