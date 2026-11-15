# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Fail-closed behaviour this build promises in its UI: downloads blocked,
permissions denied, onion addresses refused, pop-ups only on a click,
searches to DuckDuckGo, and no browsing data in Chrome's default profile."""

from __future__ import annotations

import contextlib
import ctypes
import json
import os
import pathlib
import sqlite3
import shutil
import tempfile
import time
import unittest
import uuid

from harness import Fixture, TorFox, bridge_call, wait_for

ONION = "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion"


def downloads_folder() -> pathlib.Path:
    # FOLDERID_Downloads
    guid = (ctypes.c_byte * 16)(*uuid.UUID("374DE290-123F-4565-9164-39C4925E467B").bytes_le)
    out = ctypes.c_wchar_p()
    if ctypes.windll.shell32.SHGetKnownFolderPath(guid, 0, None, ctypes.byref(out)) != 0:
        return pathlib.Path.home() / "Downloads"
    path = out.value
    ctypes.windll.ole32.CoTaskMemFree(out)
    return pathlib.Path(path)


class Stage1DefaultsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()
        cls.page_url = cls.fixture.html("/page", """<!doctype html><meta charset="utf-8"><title>page</title>
            <body><a id="dl" href="/file">download</a>
            <a id="dl2" href="/file2" download>download attribute</a></body>""")
        cls.name = f"torfox-e2e-{uuid.uuid4().hex}.bin"
        cls.fixture.raw("/file", 200, {"Content-Type": "application/octet-stream",
                                       "Content-Disposition": f'attachment; filename="{cls.name}"'},
                        b"x" * 4096)
        cls.fixture.raw("/file2", 200, {"Content-Type": "application/octet-stream"}, b"y" * 4096)
        # Searches must never leave the machine during tests.
        cls.tf = TorFox(start_url=cls.page_url, host_rules="MAP duckduckgo.com ~NOTFOUND").start()
        cls.shell = cls.tf.shell()
        cls.web = cls.tf.page_at(cls.page_url)

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            cls.tf.__exit__(None, None, None)
        finally:
            cls.fixture.__exit__(None, None, None)
            for folder in (downloads_folder(),):
                for leftover in folder.glob("torfox-e2e-*"):
                    leftover.unlink(missing_ok=True)

    def setUp(self) -> None:
        # Each test starts on the fixture page in the active tab.
        if self.web.evaluate("location.href") != self.page_url:
            self.web.evaluate(f"location.href = {json.dumps(self.page_url)}; 1")
            wait_for(lambda: self.web.evaluate("location.href") == self.page_url and
                     self.web.evaluate("document.readyState") == "complete", 10, "fixture page")

    # Downloads: Stage 3 replaced the blanket block with Settings > Downloads;
    # see test_downloads.py (a fresh profile asks before every download).

    # Permissions -------------------------------------------------------------------------
    def test_permission_requests_are_denied(self) -> None:
        result = self.web.evaluate("""
          (async () => {
            const out = {};
            out.notifications = await Notification.requestPermission();
            out.geolocation = await new Promise((resolve) => navigator.geolocation.getCurrentPosition(
              () => resolve("granted"), (e) => resolve("error:" + e.code), {timeout: 5000}));
            for (const [name, c] of [["camera", {video: true}], ["microphone", {audio: true}]]) {
              try { const s = await navigator.mediaDevices.getUserMedia(c);
                    s.getTracks().forEach(t => t.stop()); out[name] = "granted"; }
              catch (e) { out[name] = "error:" + e.name; }
            }
            try { await navigator.clipboard.readText(); out.clipboard = "granted"; }
            catch (e) { out.clipboard = "error:" + e.name; }
            return out;
          })()""", user_gesture=True, timeout=30)
        self.assertEqual(result["notifications"], "denied", result)
        self.assertEqual(result["geolocation"], "error:1", result)  # PERMISSION_DENIED
        for name in ("camera", "microphone", "clipboard"):
            self.assertNotEqual(result[name], "granted", (name, result))
            self.assertTrue(result[name].startswith("error:"), (name, result))
        self.assertTrue(self.tf.events("permission_denied"))

    def test_a_window_opened_on_an_address_shows_it_as_visible(self) -> None:
        # TorFox opened this window on the fixture page (its start URL). Its
        # first tab used to be created before the window was shown, and
        # Chromium then treated the page as hidden.
        self.assertEqual(self.web.evaluate("document.visibilityState"), "visible")

    def test_media_permission_refused_by_torfox(self) -> None:
        # NotFoundError would mean "no device" rather than a refusal; require
        # TorFox's handler to have refused the request.
        before = len(self.tf.events("permission_denied", kind="media"))
        result = self.web.evaluate("""
          navigator.mediaDevices.getUserMedia({video: true, audio: true})
            .then(s => { s.getTracks().forEach(t => t.stop()); return "granted"; }, e => "error:" + e.name)""",
                                   user_gesture=True, timeout=20)
        self.assertNotEqual(result, "granted")
        if result != "error:NotFoundError":
            self.assertEqual(result, "error:NotAllowedError")
            wait_for(lambda: len(self.tf.events("permission_denied", kind="media")) > before, 5,
                     "permission_denied media")

    # Onion addresses -----------------------------------------------------------------------
    def test_onion_input_is_refused_and_never_searched(self) -> None:
        # Valid onion addresses open in a Tor window (tests/e2e/test_tor.py,
        # with a scripted tor). Everything else that names one is refused:
        # it is never searched for and never loaded here.
        typo = ONION[:10] + ("b" if ONION[10] == "a" else "a") + ONION[11:]
        for text in ("search " + ONION, typo, "ftp://" + ONION + "/", "expyuzz4wqqyqhjn.onion"):
            r = bridge_call(self.shell, "nav.go", {"input": text})
            self.assertFalse(r["ok"], (text, r))
            self.assertTrue(r["message"], (text, r))
        self.assertFalse(self.tf.events("window_created", tor=True))
        time.sleep(0.5)
        urls = [t["url"] for t in self.tf.browser.targets()]
        self.assertFalse([u for u in urls if "onion" in u.lower() or "duckduckgo" in u], urls)

    def test_onion_subresources_and_navigation_are_blocked(self) -> None:
        result = self.web.evaluate("""
          new Promise((resolve) => { const i = new Image();
            i.onload = () => resolve("loaded"); i.onerror = () => resolve("error");
            i.src = "http://%s/pixel.png"; setTimeout(() => resolve("timeout"), 5000); })""" % ONION)
        self.assertEqual(result, "error")
        wait_for(lambda: [e for e in self.tf.events("resource_blocked") if ".onion" in e["url"]], 5,
                 "resource_blocked onion")
        self.web.evaluate(f"location.href = 'http://{ONION}/'; 1", user_gesture=True)
        wait_for(lambda: [e for e in self.tf.events("nav_blocked") if ".onion" in e["url"]], 5,
                 "nav_blocked onion")
        time.sleep(0.3)
        self.assertEqual(self.web.evaluate("location.href"), self.page_url)

    # Pop-ups ---------------------------------------------------------------------------------
    def test_popup_without_gesture_is_blocked(self) -> None:
        other = self.fixture.html("/other", "<!doctype html><title>other</title>")
        # A fresh document has no transient user activation left over from
        # earlier tests (Chromium counts a click within the last few seconds).
        fresh = self.fixture.html("/fresh", "<!doctype html><title>fresh</title>")
        self.web.evaluate(f"location.href = {json.dumps(fresh)}; 1")
        wait_for(lambda: self.web.evaluate("location.href") == fresh and
                 self.web.evaluate("document.readyState") == "complete", 10, "fresh page")
        self.assertFalse(self.web.evaluate("navigator.userActivation.isActive"))
        # Chromium's own blocker refuses gesture-less pop-ups before TorFox's
        # OnBeforePopup runs; either layer refusing is a pass.
        self.assertEqual(self.web.evaluate(f"String(window.open({json.dumps(other)}))"), "null")
        time.sleep(1.0)
        self.assertFalse([t for t in self.tf.browser.targets() if t["url"] == other])
        self.assertNotIn("GET /other", self.fixture.requests)

    def test_popup_with_gesture_opens_a_tab_without_opener(self) -> None:
        other = self.fixture.html("/other2", "<!doctype html><title>other2</title>")
        self.web.evaluate(f"window.open({json.dumps(other)}); 1", user_gesture=True)
        page = self.tf.page_at(other)
        self.assertIsNone(page.evaluate("window.opener"))
        bridge_call(self.shell, "tab.select", {"id": self.tf.events("browser_created", role="web_tab")[0]["tab"]})

    # Search ------------------------------------------------------------------------------------
    def test_search_goes_to_duckduckgo(self) -> None:
        r = bridge_call(self.shell, "nav.go", {"input": "privacy browser test"})
        self.assertTrue(r["ok"], r)
        # duckduckgo.com is mapped to NOTFOUND for this test; the attempted URL
        # is what matters.
        target = self.tf.browser.wait_target(lambda t: "duckduckgo.com" in t["url"], 10)
        self.assertTrue(target["url"].startswith("https://duckduckgo.com/?q=privacy"), target["url"])


class DefaultProfileStaysEmptyTest(unittest.TestCase):
    """Chrome keeps an on-disk "Default" profile beside TorFox's own. TorFox
    never browses in it, so it must hold no browsing data."""

    def test_no_history_in_default_profile(self) -> None:
        user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-default-"))
        try:
            with Fixture() as fx:
                url = fx.html("/visited", "<!doctype html><title>visited</title>")
                with TorFox(user_data=user_data, start_url=url) as tf:
                    tf.page_at(url)
                    time.sleep(1.0)
            profile = user_data / "User Data" / "Default"
            for name in ("History", "Favicons", "Top Sites"):
                db = profile / name
                if not db.exists():
                    continue
                copy = user_data / (name + ".copy")
                shutil.copy(db, copy)
                with contextlib.closing(sqlite3.connect(copy)) as conn:
                    tables = {r[0] for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                    for table in ("urls", "visits", "favicon_bitmaps", "top_sites"):
                        if table in tables:
                            count = conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
                            self.assertEqual(count, 0, f"{name}.{table}")
            blob = b"".join(p.read_bytes() for p in profile.rglob("*") if p.is_file() and p.stat().st_size < 5_000_000)
            self.assertNotIn(b"/visited", blob)
        finally:
            shutil.rmtree(user_data, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
