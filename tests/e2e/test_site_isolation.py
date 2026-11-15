# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Site isolation: frames of other sites run in other renderer processes.

TorFox forces --site-per-process. Chromium shows a frame that runs in
another renderer process than its parent (an out-of-process iframe) to
DevTools as a target of type "iframe"; a frame in its parent's process has
no target of its own. The tests embed a frame of another site and, as the
control, a frame of the same site (another host under the same registrable
domain, which site isolation keeps together), in normal and private
windows. Tor windows are not covered here.

Usage (from tests/e2e):  python -m unittest -v test_site_isolation
"""

from __future__ import annotations

import pathlib
import shutil
import tempfile
import unittest

from harness import CTRL, SHIFT, TorFox, bridge_call, wait_for, wait_state
from sites import Sites

PAGE = "news.example.com"        # The page.
SAME_SITE = "cdn.example.com"    # Another host of the same site.
OTHER = "shop.example.net"       # Another site.
HOSTS = [PAGE, SAME_SITE, OTHER]


class SiteIsolationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.sites = Sites(HOSTS).__enter__()
        s = cls.sites
        s.html(OTHER, "/frame", "<!doctype html><title>other</title><p>other site</p>")
        s.html(SAME_SITE, "/frame", "<!doctype html><title>same</title><p>same site</p>")
        s.html(PAGE, "/", "<!doctype html><title>page</title>"
               f"<iframe id=other src='{s.url('https', OTHER, '/frame')}'></iframe>"
               f"<iframe id=same src='{s.url('https', SAME_SITE, '/frame')}'></iframe>")
        cls.root = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-isolation-"))
        cls.tf = TorFox(user_data=cls.root / "profile", host_rules=s.host_rules(HOSTS),
                        extra_args=[f"--ignore-certificate-errors-spki-list={s.spki}"]).start()
        cls.shell = cls.tf.shell()
        wait_state(cls.shell, lambda st: len(st["tabs"]) >= 1, "the first tab")

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            cls.tf.__exit__(None, None, None)
        finally:
            cls.sites.__exit__(None, None, None)
            shutil.rmtree(cls.root, ignore_errors=True)

    def frame_targets(self) -> list[dict]:
        infos = self.tf.browser.send("Target.getTargets")["targetInfos"]
        return [t for t in infos if t["type"] == "iframe"]

    def load_page(self, shell) -> None:
        url = self.sites.url("https", PAGE, "/")
        self.assertTrue(bridge_call(shell, "nav.go", {"input": url})["ok"])
        wait_state(shell, lambda st: st["nav"].get("url") == url and not st["nav"].get("loading"),
                   "the page")
        # Both frames were requested.
        wait_for(lambda: self.sites.seen(OTHER, "/frame") and self.sites.seen(SAME_SITE, "/frame"),
                 15, "both frames")

    def check_isolated(self, before: set[str]) -> None:
        # Only frames of the page just loaded (|before|: targets that existed
        # before it), whatever other windows show.
        other = self.sites.url("https", OTHER, "/frame")
        same = self.sites.url("https", SAME_SITE, "/frame")

        def new(url: str) -> list[dict]:
            return [t for t in self.frame_targets() if t["url"] == url and t["targetId"] not in before]
        targets = wait_for(lambda: new(other), 15, "the other site's frame to run in its own process")
        self.assertEqual(len(targets), 1, targets)
        # The control: the same site's frame stays in the page's process, so
        # the check above tells the two cases apart.
        self.assertEqual(new(same), [])

    def existing(self) -> set[str]:
        return {t["targetId"] for t in self.frame_targets()}

    def test_frames_of_other_sites_run_in_other_processes(self) -> None:
        before = self.existing()
        self.load_page(self.shell)
        self.check_isolated(before)

    def test_private_windows_isolate_sites_too(self) -> None:
        known = self.tf.shell_ids()
        self.assertTrue(bridge_call(self.shell, "window.newPrivate")["ok"])
        private = self.tf.new_shell(known)
        try:
            wait_state(private, lambda st: st.get("private") is True and len(st["tabs"]) == 1,
                       "the private window")
            before = self.existing()
            self.load_page(private)
            self.check_isolated(before)
        finally:
            # Ctrl+Shift+W. The shell goes with its window, so nothing waits
            # for a reply.
            created = len(self.tf.events("window_destroyed", private=True))
            private.send("Input.dispatchKeyEvent", {"type": "rawKeyDown", "windowsVirtualKeyCode": 0x57,
                                                    "nativeVirtualKeyCode": 0x57, "modifiers": CTRL | SHIFT})
            wait_for(lambda: len(self.tf.events("window_destroyed", private=True)) > created, 15,
                     "the private window to close")


if __name__ == "__main__":
    unittest.main()
