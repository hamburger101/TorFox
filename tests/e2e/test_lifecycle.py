# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Window and tab lifecycle: tabs open and close cleanly, and TorFox always
exits cleanly (code 0, no crash trace) however it is closed."""

from __future__ import annotations

import time
import unittest

from cdp import CdpError
from harness import Fixture, TorFox, bridge_call, wait_for


def tab_count(tf: TorFox) -> int:
    return len([t for t in tf.browser.targets() if t["url"] != "torfox://shell/"])


class LifecycleTest(unittest.TestCase):
    def test_window_close_with_several_tabs(self) -> None:
        with Fixture() as fx:
            url = fx.html("/p", "<!doctype html><title>p</title>")
            with TorFox(start_url=url) as tf:
                shell = tf.shell()
                for _ in range(3):
                    self.assertTrue(bridge_call(shell, "tab.new")["ok"])
                self.assertTrue(bridge_call(shell, "app.openSettings")["ok"])
                wait_for(lambda: tab_count(tf) == 5, 10, "five tabs")
                self.assertEqual(tf.close(), 0)
                # Every browser TorFox created was closed: the shell, the five
                # tabs, and the new-tab page the first tab started as (it is
                # replaced when the window's start address loads).
                created = {e["browser"] for e in tf.events("browser_created")}
                closed = [e["browser"] for e in tf.events("browser_closed")]
                self.assertEqual(len(created), 7)
                self.assertEqual(sorted(closed), sorted(created))
                self.assertTrue(tf.events("window_destroyed"))

    def test_opening_and_closing_tabs_repeatedly(self) -> None:
        with TorFox() as tf:
            shell = tf.shell()
            for _ in range(10):
                self.assertTrue(bridge_call(shell, "tab.new")["ok"])
                wait_for(lambda: tab_count(tf) == 2, 10, "second tab")
                state = [e for e in tf.events("browser_created") if e["role"] == "internal_tab"][-1]
                self.assertTrue(bridge_call(shell, "tab.close", {"id": state["tab"]})["ok"])
                wait_for(lambda: tab_count(tf) == 1, 10, "tab closed")
            self.assertEqual(len(tf.events("browser_closed")), 10)

    def test_closing_the_last_tab_closes_the_window(self) -> None:
        tf = TorFox().start()
        try:
            shell = tf.shell()
            first = tf.events("browser_created", role="internal_tab")[0]["tab"]
            try:
                bridge_call(shell, "tab.close", {"id": first})
            except CdpError:
                pass  # TorFox may exit before the reply arrives.
            tf.proc.wait(20)
            tf.assert_clean_exit()
            self.assertTrue(tf.events("window_destroyed"))
        finally:
            tf.cleanup()

    def test_role_change_replaces_the_browser(self) -> None:
        with Fixture() as fx:
            url = fx.html("/p", "<!doctype html><title>p</title>")
            with TorFox() as tf:
                shell = tf.shell()
                for target in (url, "torfox://settings/", url, "torfox://newtab/"):
                    self.assertTrue(bridge_call(shell, "nav.go", {"input": target})["ok"])
                    tf.page_at(target)
                self.assertEqual(tab_count(tf), 1)
                roles = [e["role"] for e in tf.events("browser_created") if e["role"] != "shell"]
                self.assertEqual(roles, ["internal_tab", "web_tab", "internal_tab", "web_tab", "internal_tab"])


if __name__ == "__main__":
    unittest.main()
