# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Stage 7: Settings > Updates against a fake GitHub on loopback.

UpdateTest checks each step with automatic downloads off; AutoUpdateTest
the default, Chrome-like flow: a verified update downloads by itself, the
toolbar offers "Relaunch to update", and an update still waiting when
TorFox closes is installed without starting TorFox again.

The test build is pointed at the fake (--torfox-test-update-config, test
builds only) with a key made for each run; releases are signed with it by
scripts/minisign.py, whose output the official minisign verifies (and whose
verifier agrees with TorFox's on the official tool's vectors, see
tests/unit/data/update). Only a release that verifies, is newer, and whose
file matches the signed size and SHA-256 may be installed; the "installer"
is tests/e2e/helpers/fake_installer.c, which records how TorFox started it.
"""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

from harness import REPO, Fixture, TorFox, bridge_call, click, shell_state, wait_for, wait_state

sys.path.insert(0, str(REPO / "scripts"))
import minisign  # noqa: E402
import release as release_tool  # noqa: E402

FAKE_INSTALLER = (REPO / "out" / "test" / "tests" / "e2e" / "helpers" / "Release" /
                  "torfox_fake_installer.exe")
REPOSITORY = "test/TorFox"
PLATFORM = "windows-x64"
INSTALLER_ARGS = "/SILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE"
# TorFox closed with an update waiting: no window, and no relaunch.
INSTALL_ON_EXIT_ARGS = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE /TORFOXNORELAUNCH"


class Key:
    def __init__(self) -> None:
        self.seed = os.urandom(32)
        self.key_id = os.urandom(8)
        self.public = minisign.public_key_text(self.seed, self.key_id)

    def sign(self, data: bytes, comment: str) -> str:
        return minisign.sign(self.seed, self.key_id, data, comment)


class UpdateHelpers:
    # Settings written before the first start.
    PRESET: dict = {}

    def setUp(self) -> None:
        if not FAKE_INSTALLER.is_file():
            self.fail(f"{FAKE_INSTALLER} is missing: build out/test (TORFOX_TEST_HOOKS=ON)")
        self.key = Key()
        self.github = Fixture().__enter__()
        self.addCleanup(self.github.__exit__)
        self.user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-updates-"))
        self.addCleanup(shutil.rmtree, self.user_data, True)
        self.marker = self.user_data.parent / (self.user_data.name + "-installer.txt")
        self.addCleanup(lambda: self.marker.unlink(missing_ok=True))
        self.installer = FAKE_INSTALLER.read_bytes()
        self.choice = self.user_data.parent / (self.user_data.name + "-dialog.txt")
        self.addCleanup(lambda: self.choice.unlink(missing_ok=True))
        if self.PRESET:
            (self.user_data / "settings.json").write_text(
                json.dumps({"format": 1, "settings": self.PRESET}), encoding="utf-8")

    # ----- The fake GitHub -------------------------------------------------------------

    def download_path(self, tag: str, name: str) -> str:
        return f"/{REPOSITORY}/releases/download/{tag}/{name}"

    def publish(self, version: str, timestamp: int, *, body: bytes | None = None,
                listed: bytes | None = None, key: Key | None = None, comment: str | None = None,
                manifest_edit=None, signature: bool = True, notes: str = "Test release.") -> dict:
        """Publishes a release. |body| is what the server sends for the file,
        |listed| what the signed manifest says it is (both default to the
        fake installer)."""
        tag = f"v{version}"
        name = f"TorFox-{version}-win-x64-setup.exe"
        body = self.installer if body is None else body
        listed = body if listed is None else listed
        # Made by the release workflow's own code (scripts/release.py).
        manifest_bytes = release_tool.build_manifest(
            version, timestamp, notes,
            {PLATFORM: {"name": name, "size": len(listed), "sha256": hashlib.sha256(listed).hexdigest()}})
        sig = (key or self.key).sign(
            manifest_bytes, comment or f"torfox-release version:{version} timestamp:{timestamp}")
        if manifest_edit:
            edited = manifest_edit(manifest_bytes)
            assert edited != manifest_bytes, "the manifest edit changed nothing"
            manifest_bytes = edited
        files = {"manifest.json": manifest_bytes, name: body}
        if signature:
            files["manifest.json.minisig"] = sig.encode()
        assets = []
        for n, data in files.items():
            path = self.download_path(tag, n)
            self.github.raw(path, 200, {"Content-Type": "application/octet-stream"}, data)
            assets.append({"name": n, "size": len(data),
                           "browser_download_url": self.github.origin + path})
        release = {"tag_name": tag, "draft": False, "prerelease": False, "assets": assets}
        self.github.raw(f"/repos/{REPOSITORY}/releases/latest", 200,
                        {"Content-Type": "application/json"}, json.dumps(release).encode())
        return {"name": name, "tag": tag, "sha256": hashlib.sha256(listed).hexdigest(),
                "path": self.download_path(tag, name)}

    def requests_for(self, prefix: str, fixture: Fixture | None = None) -> list[str]:
        return [r for r in (fixture or self.github).requests if r.split(" ", 1)[1].startswith(prefix)]

    # ----- TorFox ------------------------------------------------------------------------

    def torfox(self, delay_ms: int = 600000, extra_origins: list[str] | None = None,
               configured: bool = True, extra_args: list[str] | None = None) -> TorFox:
        args = [f"--torfox-test-dialog-choice={self.choice}", *(extra_args or [])]
        if configured:
            config = self.user_data.parent / (self.user_data.name + "-update.json")
            config.write_text(json.dumps({"origin": self.github.origin,
                                          "extraOrigins": extra_origins or [],
                                          "publicKey": self.key.public,
                                          "repository": REPOSITORY,
                                          "firstCheckDelayMs": delay_ms}), encoding="utf-8")
            self.addCleanup(lambda: config.unlink(missing_ok=True))
            args.append(f"--torfox-test-update-config={config}")
        os.environ["TORFOX_FAKE_INSTALLER_MARKER"] = str(self.marker)
        self.addCleanup(os.environ.pop, "TORFOX_FAKE_INSTALLER_MARKER", None)
        return TorFox(user_data=self.user_data, extra_args=args)

    def settings(self, tf: TorFox):
        shell = tf.shell()
        self.assertTrue(bridge_call(shell, "app.openSettings")["ok"])
        return tf.page_at("torfox://settings/")

    def status(self, page) -> dict:
        r = bridge_call(page, "update.status")
        self.assertTrue(r["ok"], r)
        return json.loads(r["response"])

    def act(self, page, cmd: str) -> dict:
        r = bridge_call(page, cmd)
        self.assertTrue(r["ok"], r)
        return json.loads(r["response"])

    def wait_status(self, page, states: set[str], timeout: float = 20) -> dict:
        last = {}

        def done():
            nonlocal last
            last = self.status(page)
            return last if last["state"] in states else None
        try:
            return wait_for(done, timeout, f"update state in {states}")
        except TimeoutError as e:
            raise TimeoutError(f"{e}; last status {last}") from None

    def check(self, page) -> dict:
        self.act(page, "update.check")
        return self.wait_status(page, {"available", "up_to_date", "error"})

    def assert_nothing_in_updates(self) -> None:
        folder = self.user_data / "Updates"
        self.assertEqual(sorted(p.name for p in folder.iterdir()) if folder.exists() else [], [])

    def installer_started(self) -> list[str]:
        return wait_for(lambda: self.marker.exists() and
                        self.marker.read_text(encoding="utf-8").splitlines(), 20,
                        "the installer to start")

    def preset(self, settings: dict) -> None:
        (self.user_data / "settings.json").write_text(
            json.dumps({"format": 1, "settings": {**self.PRESET, **settings}}), encoding="utf-8")

    def wait_ready_by_itself(self, tf: TorFox, release: dict) -> dict:
        """The automatic check finds |release| and downloads it, unasked."""
        check = tf.wait_log("update_check", timeout=20, automatic=True)
        self.assertEqual(check["result"], "available")
        done = tf.wait_log("update_download", timeout=30)
        self.assertEqual((done["result"], done["sha256"]), ("ok", release["sha256"]))
        return wait_state(tf.shell(), lambda s: s["update"], "the toolbar's relaunch button", 15)

    def relaunch_from(self, shell) -> None:
        """Asks for the relaunch from the toolbar, without waiting for the
        reply (TorFox may close before it arrives)."""
        shell.evaluate("window.torfoxQuery({request: JSON.stringify({cmd: 'update.install'}),"
                       " persistent: false, onSuccess: () => {}, onFailure: () => {}}), true")


@unittest.skipUnless(sys.platform == "win32", "the update E2E tests run on Windows")
class UpdateTest(UpdateHelpers, unittest.TestCase):
    # Each step on its own: nothing is downloaded until the user asks.
    PRESET = {"updates.autoInstall": False}

    # ----- Tests -------------------------------------------------------------------------

    def test_a_signed_newer_release_is_checked_downloaded_and_installed_after_exit(self) -> None:
        release = self.publish("9.1.0", 2000000000, notes="Line one.\n<b>not markup</b>")
        with self.torfox() as tf:
            page = self.settings(tf)
            s = self.check(page)
            self.assertEqual(s["state"], "available", s)
            self.assertEqual(s["version"], "9.1.0")
            self.assertEqual(s["notes"], "Line one.\n<b>not markup</b>")
            self.assertEqual(s["size"], len(self.installer))
            # Settings > Updates shows the notes as text, never as markup.
            page.evaluate("[...document.querySelector('tf-settings').shadowRoot"
                          ".querySelectorAll('.nav-item')].find(b => b.textContent.trim() === "
                          "'Updates').click()")
            shown = wait_for(lambda: page.evaluate(
                "(() => { const r = document.querySelector('tf-settings').shadowRoot;"
                " const n = r.querySelector('.notes-text');"
                " return n && [n.textContent, n.children.length, r.querySelector('#update-status')"
                ".dataset.state]; })()"), 5, "release notes shown")
            self.assertEqual(shown, ["Line one.\n<b>not markup</b>", 0, "available"])
            # Nothing is downloaded until the user asks.
            self.assertEqual(self.requests_for(release["path"]), [])
            self.act(page, "update.download")
            s = self.wait_status(page, {"ready", "error"})
            self.assertEqual(s["state"], "ready", s)
            done = tf.events("update_download")
            self.assertEqual([(e["result"], e["sha256"], e["size"]) for e in done],
                             [("ok", release["sha256"], len(self.installer))])
            downloaded = self.user_data / "Updates" / release["name"]
            self.assertEqual(downloaded.read_bytes(), self.installer)
            # Restart to update: TorFox closes by itself, then starts the file.
            # (Not waiting for the reply: the page closes with TorFox.)
            page.evaluate("window.torfoxQuery({request: JSON.stringify({cmd: 'update.install'}),"
                          " persistent: false, onSuccess: () => {}, onFailure: () => {}}), true")
            tf.proc.wait(30)
            self.assertEqual(tf.proc.returncode, 0)
            self.assertEqual([e["ok"] for e in tf.events("update_install")], [True])
            self.assertEqual([e["sha256"] for e in tf.events("update_handoff")], [release["sha256"]])
            lines = wait_for(lambda: self.marker.exists() and
                             self.marker.read_text(encoding="utf-8").splitlines(), 20,
                             "the installer to start")
            self.assertTrue(lines[0].endswith(INSTALLER_ARGS), lines)
            self.assertEqual(pathlib.Path(lines[1]).resolve(), downloaded.resolve())
            # Only GitHub's API and this release's files were asked for.
            self.assertEqual(sorted(self.github.requests), sorted([
                f"GET /repos/{REPOSITORY}/releases/latest",
                f"GET {self.download_path(release['tag'], 'manifest.json.minisig')}",
                f"GET {self.download_path(release['tag'], 'manifest.json')}",
                f"GET {release['path']}"]))

    def test_with_automatic_updates_off_closing_torfox_installs_nothing(self) -> None:
        release = self.publish("9.1.0", 2000000000)
        with self.torfox() as tf:
            page = self.settings(tf)
            self.assertEqual(self.check(page)["state"], "available")
            self.act(page, "update.download")
            self.assertEqual(self.wait_status(page, {"ready", "error"})["state"], "ready")
            # The user downloaded it: the toolbar offers the relaunch too, but
            # closing TorFox does not install it.
            self.assertEqual(shell_state(tf.shell())["update"], {"version": "9.1.0", "restarts": True})
            self.assertEqual(tf.close(), 0)
            self.assertEqual(tf.events("update_install"), [])
            self.assertEqual(tf.events("update_handoff"), [])
        time.sleep(1)
        self.assertFalse(self.marker.exists())
        # Kept for later: the next start finds it again.
        self.assertTrue((self.user_data / "Updates" / release["name"]).exists())

    def test_turning_automatic_updates_on_downloads_the_update_found(self) -> None:
        release = self.publish("9.1.0", 2000000000)
        with self.torfox() as tf:
            page = self.settings(tf)
            self.assertEqual(self.check(page)["state"], "available")
            time.sleep(1)
            self.assertEqual(self.requests_for(release["path"]), [])
            r = bridge_call(page, "settings.set", {"key": "updates.autoInstall", "value": True})
            self.assertTrue(r["ok"], r)
            s = self.wait_status(page, {"ready", "error"}, timeout=30)
            self.assertEqual(s["state"], "ready", s)
            self.assertEqual(len(self.requests_for(release["path"])), 1)
            wait_state(tf.shell(), lambda st: st["update"] == {"version": "9.1.0", "restarts": True}, "relaunch button")

    def test_a_release_signed_with_another_key_is_ignored(self) -> None:
        release = self.publish("9.1.0", 2000000000, key=Key())
        with self.torfox() as tf:
            page = self.settings(tf)
            s = self.check(page)
            self.assertEqual(s["state"], "error", s)
            self.assertIn("could not be verified", s["message"])
            self.assertEqual(s.get("version"), "")
            self.assertEqual(self.requests_for(release["path"]), [])
            # And nothing can be downloaded or installed.
            self.act(page, "update.download")
            self.assertEqual(self.status(page)["state"], "error")
            self.assertFalse(bridge_call(page, "update.install")["ok"])
            self.assertEqual(self.requests_for(release["path"]), [])
        self.assertFalse(self.marker.exists())
        self.assert_nothing_in_updates()

    def test_a_changed_manifest_or_signed_comment_is_ignored(self) -> None:
        cases = {
            "manifest changed after signing": dict(
                manifest_edit=lambda b: b.replace(b'"notes":"', b'"notes":"x')),
            "comment names another release": dict(
                comment="torfox-release version:9.9.9 timestamp:2000000000"),
            "comment names another time": dict(
                comment="torfox-release version:9.1.0 timestamp:1"),
            "no signature published": dict(signature=False),
        }
        with self.torfox() as tf:
            page = self.settings(tf)
            for what, kwargs in cases.items():
                with self.subTest(what):
                    release = self.publish("9.1.0", 2000000000, **kwargs)
                    s = self.check(page)
                    self.assertEqual(s["state"], "error", (what, s))
                    self.assertEqual(self.requests_for(release["path"]), [])
        self.assert_nothing_in_updates()

    def test_a_file_that_does_not_match_the_signed_release_is_deleted(self) -> None:
        other = bytes(len(self.installer))  # Same size, other content.
        cases = {
            "different content": dict(body=other, listed=self.installer),
            "truncated": dict(body=self.installer[:-1000], listed=self.installer),
            "longer": dict(body=self.installer + b"x" * 1000, listed=self.installer),
        }
        with self.torfox() as tf:
            page = self.settings(tf)
            for what, kwargs in cases.items():
                with self.subTest(what):
                    self.publish("9.1.0", 2000000000, **kwargs)
                    self.assertEqual(self.check(page)["state"], "available")
                    self.act(page, "update.download")
                    s = self.wait_status(page, {"ready", "error"})
                    self.assertEqual(s["state"], "error", (what, s))
                    self.assert_nothing_in_updates()
                    r = bridge_call(page, "update.install")
                    self.assertFalse(r["ok"], r)
            self.assertEqual([e["result"] for e in tf.events("update_download")], ["failed"] * 3)
            self.assertEqual(tf.events("update_install"), [])
        self.assertFalse(self.marker.exists())

    def test_a_file_changed_after_its_download_is_not_installed(self) -> None:
        release = self.publish("9.1.0", 2000000000)
        with self.torfox() as tf:
            page = self.settings(tf)
            self.assertEqual(self.check(page)["state"], "available")
            self.act(page, "update.download")
            self.assertEqual(self.wait_status(page, {"ready", "error"})["state"], "ready")
            # Something else on this computer replaces the checked file.
            downloaded = self.user_data / "Updates" / release["name"]
            downloaded.write_bytes(bytes(len(self.installer)))
            r = bridge_call(page, "update.install")
            self.assertFalse(r["ok"], r)
            self.assertIn("changed on this computer", r["message"])
            self.assertEqual(self.status(page)["state"], "error")
            self.assertEqual([e["ok"] for e in tf.events("update_install")], [False])
            self.assertFalse(downloaded.exists())
            time.sleep(1)
            self.assertIsNone(tf.proc.poll())  # TorFox stays open.
        self.assertEqual(tf.events("update_handoff"), [])
        self.assertFalse(self.marker.exists())

    def test_older_and_same_versions_are_never_offered(self) -> None:
        with self.torfox() as tf:
            page = self.settings(tf)
            installed = self.status(page)["installed"]
            major, minor, patch = (int(p) for p in installed.split("."))
            older = f"{major}.{minor}.{patch - 1}" if patch else f"{major}.{minor - 1}.9" if minor else None
            for version in [installed] + ([older] if older else []):
                with self.subTest(version):
                    release = self.publish(version, 2000000000)
                    s = self.check(page)
                    self.assertEqual(s["state"], "up_to_date", s)
                    self.assertEqual(self.requests_for(release["path"]), [])

    def test_an_older_signed_release_cannot_be_replayed(self) -> None:
        with self.torfox() as tf:
            page = self.settings(tf)
            self.publish("9.3.0", 2000000000)
            self.assertEqual(self.check(page)["state"], "available")
            # A real, correctly signed, newer-than-installed but older release.
            old = self.publish("9.2.0", 1900000000)
            s = self.check(page)
            # Reported as a refused release, never as "up to date".
            self.assertEqual(s["state"], "error", s)
            self.assertIn("older than one TorFox has already seen", s["message"])
            self.assertEqual(self.requests_for(old["path"]), [])
            # Also when its version is not even newer than this one.
            installed = self.status(page)["installed"]
            self.publish(installed, 1900000000)
            s = self.check(page)
            self.assertEqual(s["state"], "error", s)
            self.assertIn("older than one TorFox has already seen", s["message"])
        # Remembered across restarts.
        saved = json.loads((self.user_data / "update.json").read_text(encoding="utf-8"))
        self.assertEqual(saved["newestSeen"], 2000000000)
        with self.torfox() as tf:
            page = self.settings(tf)
            self.assertEqual(self.check(page)["state"], "error")
            # The newest one is still offered.
            self.publish("9.3.0", 2000000000)
            self.assertEqual(self.check(page)["state"], "available")

    def test_the_installer_is_started_by_its_real_path(self) -> None:
        # Another program makes the Updates folder a junction to a folder of
        # its own after TorFox started. TorFox must start the file it checked
        # by the path Windows resolved for it, never through the junction,
        # which could be pointed elsewhere between the check and the start.
        elsewhere = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-updates-elsewhere-"))
        self.addCleanup(shutil.rmtree, elsewhere, True)
        release = self.publish("9.1.0", 2000000000)
        with self.torfox() as tf:
            page = self.settings(tf)
            self.assertEqual(self.check(page)["state"], "available")
            link = self.user_data / "Updates"
            shutil.rmtree(link, ignore_errors=True)
            made = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(elsewhere)],
                                  capture_output=True, text=True)
            self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
            self.addCleanup(lambda: os.path.isdir(link) and os.rmdir(link))
            self.act(page, "update.download")
            self.assertEqual(self.wait_status(page, {"ready", "error"})["state"], "ready")
            self.assertEqual((elsewhere / release["name"]).read_bytes(), self.installer)
            page.evaluate("window.torfoxQuery({request: JSON.stringify({cmd: 'update.install'}),"
                          " persistent: false, onSuccess: () => {}, onFailure: () => {}}), true")
            tf.proc.wait(30)
        lines = wait_for(lambda: self.marker.exists() and
                         self.marker.read_text(encoding="utf-8").splitlines(), 20,
                         "the installer to start")
        started = os.path.normcase(lines[1])
        # realpath: the temporary folder may be named by a short (8.3) name,
        # as on GitHub's runners; TorFox starts the installer by its real one.
        self.assertEqual(started, os.path.normcase(os.path.realpath(elsewhere / release["name"])), lines)
        self.assertNotIn(os.path.normcase(str(link)), started)

    def test_no_update_is_offered_while_the_record_cannot_be_saved(self) -> None:
        # update.json cannot be written (a folder is in its place): the replay
        # record would not survive a restart, so nothing is offered.
        (self.user_data / "update.json").mkdir(parents=True)
        release = self.publish("9.1.0", 2000000000)
        with self.torfox() as tf:
            page = self.settings(tf)
            s = self.check(page)
            self.assertEqual(s["state"], "error", s)
            self.assertIn("could not save its record", s["message"])
            self.act(page, "update.download")
            time.sleep(1)
            self.assertEqual(self.requests_for(release["path"]), [])
            self.assertEqual(tf.events("update_download"), [])

    def test_downloads_follow_redirects_only_to_allowed_hosts(self) -> None:
        with Fixture() as cdn, Fixture() as elsewhere:
            with self.torfox(extra_origins=[cdn.origin]) as tf:
                page = self.settings(tf)
                # GitHub sends release files on to its download host.
                release = self.publish("9.1.0", 2000000000)
                cdn.raw("/file", 200, {}, self.installer)
                self.github.raw(release["path"], 302, {"Location": cdn.origin + "/file"})
                self.assertEqual(self.check(page)["state"], "available")
                self.act(page, "update.download")
                s = self.wait_status(page, {"ready", "error"})
                self.assertEqual(s["state"], "ready", s)
                self.assertEqual(cdn.requests, ["GET /file"])
            # Anywhere else: TorFox stops before connecting.
            with self.torfox(extra_origins=[cdn.origin]) as tf:
                page = self.settings(tf)
                release = self.publish("9.2.0", 2000000001)
                elsewhere.raw("/file", 200, {}, self.installer)
                self.github.raw(release["path"], 302, {"Location": elsewhere.origin + "/file"})
                self.assertEqual(self.check(page)["state"], "available")
                self.act(page, "update.download")
                s = self.wait_status(page, {"ready", "error"})
                self.assertEqual(s["state"], "error", s)
                self.assertIn("not one of GitHub's servers", s["message"])
                self.assertEqual(elsewhere.requests, [])
                self.assertEqual([e["allowed"] for e in tf.events("update_redirect")], [False])
                self.assert_nothing_in_updates()
            # The API's answer too: a redirect there is followed only to an
            # allowed host.
            with self.torfox() as tf:
                page = self.settings(tf)
                self.github.raw(f"/repos/{REPOSITORY}/releases/latest", 302,
                                {"Location": elsewhere.origin + "/latest"})
                s = self.check(page)
                self.assertEqual(s["state"], "error", s)
                self.assertEqual(elsewhere.requests, [])

    def test_release_files_of_another_repository_are_not_used(self) -> None:
        release = self.publish("9.1.0", 2000000000)
        latest = json.loads(self.github.routes[f"/repos/{REPOSITORY}/releases/latest"][2])
        for a in latest["assets"]:
            a["browser_download_url"] = a["browser_download_url"].replace(
                f"/{REPOSITORY}/", "/someone/TorFox/")
        self.github.raw(f"/repos/{REPOSITORY}/releases/latest", 200, {},
                        json.dumps(latest).encode())
        with self.torfox() as tf:
            page = self.settings(tf)
            s = self.check(page)
            self.assertEqual(s["state"], "error", s)
            self.assertIn("no signed manifest", s["message"])
            self.assertEqual(self.requests_for("/someone/"), [])
            self.assertEqual(self.requests_for(release["path"]), [])

    def test_automatic_checks_happen_once_a_day_and_tell_the_user(self) -> None:
        release = self.publish("9.1.0", 2000000000)
        with self.torfox(delay_ms=300) as tf:
            check = tf.wait_log("update_check", timeout=20, automatic=True)
            self.assertEqual(check["result"], "available")
            notice = tf.wait_log("update_notice", timeout=10)
            self.assertEqual(notice["version"], "9.1.0")
            # Shown in the window.
            wait_for(lambda: "TorFox 9.1.0 is available" in tf.shell().evaluate(
                "document.body.innerText + [...document.querySelectorAll('*')].map("
                "e => e.shadowRoot ? e.shadowRoot.textContent : '').join(' ')"), 10, "notice")
            # Found, not fetched.
            self.assertEqual(self.requests_for(release["path"]), [])
        self.assertEqual(len(self.requests_for("/repos/")), 1)
        # A restart within the day does not check again.
        with self.torfox(delay_ms=300) as tf:
            tf.wait_log("update_service", timeout=10)
            time.sleep(3)
            self.assertEqual(tf.events("update_check"), [])
        self.assertEqual(len(self.requests_for("/repos/")), 1)

    def test_notices_can_be_turned_off(self) -> None:
        self.publish("9.1.0", 2000000000)
        with self.torfox() as tf:
            page = self.settings(tf)
            r = bridge_call(page, "settings.set", {"key": "updates.notify", "value": False})
            self.assertTrue(r["ok"], r)
        with self.torfox(delay_ms=300) as tf:
            check = tf.wait_log("update_check", timeout=20, automatic=True)
            self.assertEqual(check["result"], "available")
            time.sleep(1)
            self.assertEqual(tf.events("update_notice"), [])

    def test_automatic_checks_can_be_turned_off(self) -> None:
        self.publish("9.1.0", 2000000000)
        with self.torfox() as tf:
            page = self.settings(tf)
            r = bridge_call(page, "settings.set", {"key": "updates.autoCheck", "value": False})
            self.assertTrue(r["ok"], r)
        with self.torfox(delay_ms=300) as tf:
            tf.wait_log("update_service", timeout=10)
            time.sleep(3)
            self.assertEqual(tf.events("update_check"), [])
            # A check the user asks for still works.
            page = self.settings(tf)
            self.assertEqual(self.check(page)["state"], "available")
        self.assertEqual(len(self.requests_for("/repos/")), 1)

    def test_with_updates_off_torfox_never_contacts_github(self) -> None:
        self.publish("9.1.0", 2000000000)
        with self.torfox() as tf:
            page = self.settings(tf)
            r = bridge_call(page, "settings.set", {"key": "updates.enabled", "value": False})
            self.assertTrue(r["ok"], r)
            self.assertEqual(self.status(page)["state"], "off")
        with self.torfox(delay_ms=300) as tf:
            page = self.settings(tf)
            time.sleep(3)
            self.assertEqual(self.status(page)["state"], "off")
            self.act(page, "update.check")
            time.sleep(1)
            self.assertEqual(self.status(page)["state"], "off")
            self.assertEqual(tf.events("update_check"), [])
        self.assertEqual(self.github.requests, [])

    def test_only_the_settings_page_can_use_the_updater(self) -> None:
        release = self.publish("9.1.0", 2000000000)
        with self.torfox() as tf:
            shell = tf.shell()
            commands = ("update.status", "update.check", "update.download")
            for cmd in commands:
                r = bridge_call(shell, cmd)
                self.assertFalse(r["ok"], (cmd, r))
            self.assertEqual([e["reason"] for e in tf.events("bridge_reject")],
                             ["unknown_command"] * len(commands))
            # The toolbar may only ask for a relaunch, which needs a ready update.
            r = bridge_call(shell, "update.install")
            self.assertEqual((r["ok"], r.get("message")), (False, "No update is ready to install."))
            self.assertEqual(tf.events("update_install"), [])
            # Web pages have no bridge at all (test_isolation.py).
        self.assertEqual(self.github.requests, [])
        self.assertEqual(self.requests_for(release["path"]), [])

    def test_a_build_without_a_key_never_looks_for_updates(self) -> None:
        # Test builds never compile in the release key (src/browser/
        # CMakeLists.txt): with no fake GitHub configured, no updates.
        self.publish("9.1.0", 2000000000)
        with self.torfox(configured=False, delay_ms=300) as tf:
            page = self.settings(tf)
            s = self.status(page)
            self.assertEqual(s["state"], "not_configured", s)
            self.assertIn("without an update signing key", s["message"])
            self.act(page, "update.check")
            time.sleep(2)
            self.assertEqual(self.status(page)["state"], "not_configured")
            self.assertEqual(tf.events("update_fetch"), [])
        self.assertEqual(self.github.requests, [])


@unittest.skipUnless(sys.platform == "win32", "the update E2E tests run on Windows")
class AutoUpdateTest(UpdateHelpers, unittest.TestCase):
    """The default: updates download by themselves, like Chrome's."""

    def test_an_update_downloads_by_itself_and_the_toolbar_offers_a_relaunch(self) -> None:
        site = Fixture().__enter__()
        self.addCleanup(site.__exit__, None, None, None)
        url = site.html("/kept", "<!doctype html><title>Kept page</title><p>kept</p>")
        release = self.publish("9.1.0", 2000000000)
        with self.torfox(delay_ms=300) as tf:
            shell = tf.shell()
            self.assertTrue(bridge_call(shell, "nav.go", {"input": url})["ok"])
            wait_state(shell, lambda s: s["tabs"][0]["url"] == url and not s["tabs"][0]["loading"],
                       "the page")
            state = self.wait_ready_by_itself(tf, release)
            self.assertEqual(state["update"], {"version": "9.1.0", "restarts": True})
            # Downloaded, not merely announced.
            self.assertEqual(tf.events("update_notice"), [])
            self.assertEqual((self.user_data / "Updates" / release["name"]).read_bytes(), self.installer)
            # The button, clicked like a user would.
            rect = wait_for(lambda: shell.evaluate(
                "(() => { const b = document.querySelector('tf-shell').shadowRoot"
                ".querySelector('#relaunch'); if (!b) return null; const r = b.getBoundingClientRect();"
                " return r.width > 0 ? [r.left + r.width / 2, r.top + r.height / 2, b.textContent.trim()]"
                " : null; })()"), 10, "the relaunch button")
            self.assertEqual(rect[2], "Relaunch to update")
            click(shell, rect[0], rect[1])
            tf.proc.wait(30)
            self.assertEqual(tf.proc.returncode, 0)
            self.assertEqual([(e["ok"], e["relaunch"]) for e in tf.events("update_install")], [(True, True)])
            self.assertEqual([(e["sha256"], e["relaunch"]) for e in tf.events("update_handoff")],
                             [(release["sha256"], True)])
            # No question: there was no private or Tor window to lose.
            self.assertEqual(tf.events("dialog_shown"), [])
        lines = self.installer_started()
        self.assertTrue(lines[0].endswith(INSTALLER_ARGS), lines)
        # TorFox held the setup's mutex from before it shut down, and waited
        # until the setup said it holds the mutex too (fake_installer.c).
        self.assertEqual(lines[2:6], ["installing-mutex=1", "parent-waited=1", "started-event=1",
                                      "parent-exited=1"], lines)
        self.assertEqual((self.user_data / "update-handoff").read_text(encoding="utf-8"), "9.1.0\n")
        # The tabs are kept for the relaunch, although "On startup" is the
        # default "Open a new tab".
        self.assertTrue((self.user_data / "session-relaunch").exists())
        saved = json.loads((self.user_data / "session.json").read_text(encoding="utf-8"))
        self.assertEqual(saved["windows"], [{"tabs": [url], "active": 0}])

        # The installer starts TorFox again (here the test does): the tabs
        # come back once, then What's new opens in front.
        (self.user_data / "last-version").write_text("0.0.1\n", encoding="utf-8")
        with self.torfox() as tf:
            shell = tf.shell()
            relaunch = tf.wait_log("session_relaunch", timeout=10)
            self.assertEqual((relaunch["windows"], relaunch["kept"]), (1, False))
            state = wait_state(shell, lambda s: [t["url"] for t in s["tabs"]] == [url, "torfox://whatsnew/"],
                               "the kept tab, then What's new")
            self.assertEqual(state["nav"]["url"], "torfox://whatsnew/")
            self.assertEqual(tf.events("whats_new_opened")[0]["from"], "0.0.1")
            # The fake installer installed nothing: TorFox says so, and What's
            # new does not say this version is the update TorFox checked.
            self.assertEqual(tf.events("update_handoff_record")[0]["outcome"], "not_installed")
            wait_state(shell, lambda s: "could not install the update to version 9.1.0" in (s["notice"] or ""),
                       "the notice that the update did not install")
            self.assertFalse((self.user_data / "update-handoff").exists())
            page = tf.page_at("torfox://whatsnew/")
            shown = wait_for(lambda: page.evaluate(
                "document.querySelector('tf-whats-new')?.shadowRoot?.querySelector('#from')?.textContent"),
                10, "What's new")
            self.assertIn("updated from version 0.0.1", shown)
            self.assertNotIn("signing key", shown)
            self.assertFalse((self.user_data / "session-relaunch").exists())
            self.assertFalse((self.user_data / "session.json").exists())
            # Nothing waits any more.
            self.assertIsNone(state["update"])
        # Only once: the next start opens a new tab, as it is set to.
        with self.torfox() as tf:
            wait_state(tf.shell(), lambda s: [t["url"] for t in s["tabs"]] == ["torfox://newtab/"],
                       "one new tab")
            self.assertEqual(tf.events("session_relaunch"), [])
            self.assertEqual(tf.events("whats_new_opened"), [])

    def test_an_update_waiting_when_torfox_closes_installs_without_a_relaunch(self) -> None:
        release = self.publish("9.1.0", 2000000000)
        with self.torfox(delay_ms=300) as tf:
            self.wait_ready_by_itself(tf, release)
            self.assertEqual(tf.close(), 0)
            self.assertEqual([(e["ok"], e["relaunch"]) for e in tf.events("update_install")], [(True, False)])
            self.assertEqual([e["relaunch"] for e in tf.events("update_handoff")], [False])
        lines = self.installer_started()
        self.assertTrue(lines[0].endswith(INSTALL_ON_EXIT_ARGS), lines)
        # TorFox held the setup's mutex from before it shut down, and waited
        # until the setup said it holds the mutex too (fake_installer.c).
        self.assertEqual(lines[2:6], ["installing-mutex=1", "parent-waited=1", "started-event=1",
                                      "parent-exited=1"], lines)
        self.assertEqual(pathlib.Path(lines[1]).resolve(),
                         (self.user_data / "Updates" / release["name"]).resolve())
        # Nothing is kept for a start that will not happen.
        self.assertFalse((self.user_data / "session-relaunch").exists())
        self.assertFalse((self.user_data / "session.json").exists())

    def test_an_update_is_not_installed_while_windows_shuts_down(self) -> None:
        # Windows is ending the session: an installer started now would be
        # cut off. The update waits for the next start.
        release = self.publish("9.1.0", 2000000000)
        with self.torfox(delay_ms=300, extra_args=["--torfox-test-session-ending"]) as tf:
            self.wait_ready_by_itself(tf, release)
            self.assertEqual(tf.close(), 0)
            self.assertEqual([e["why"] for e in tf.events("update_install_skipped")], ["session_ending"])
            self.assertEqual(tf.events("update_install"), [])
            self.assertEqual(tf.events("update_handoff"), [])
        time.sleep(1)
        self.assertFalse(self.marker.exists())
        self.assertTrue((self.user_data / "Updates" / release["name"]).exists())

    def test_private_windows_are_asked_about_before_a_relaunch(self) -> None:
        release = self.publish("9.1.0", 2000000000)
        with self.torfox(delay_ms=300) as tf:
            shell = tf.shell()
            self.wait_ready_by_itself(tf, release)
            known = tf.shell_ids()
            self.assertTrue(bridge_call(shell, "window.newPrivate")["ok"])
            wait_for(lambda: tf.events("window_created", private=True), 15, "private window")
            private = tf.new_shell(known)
            wait_state(private, lambda s: s["private"], "private shell")
            # Not now: nothing closes and nothing is installed.
            self.choice.write_text("cancel", encoding="utf-8")
            r = bridge_call(shell, "update.install")
            self.assertEqual((r["ok"], r.get("message")), (False, "cancelled"))
            shown = tf.events("dialog_shown")
            self.assertEqual([(d["kind"], d["heading"], d["accept"], d["cancel"]) for d in shown],
                             [("update_relaunch", "Your private or Tor window will close",
                               "Relaunch", "Not now")])
            self.assertIn("9.1.0", shown[0]["details"])
            self.assertIn("Your normal windows open again", shown[0]["details"])
            time.sleep(1)
            self.assertIsNone(tf.proc.poll())
            self.assertEqual(tf.events("update_install"), [])
            self.assertFalse((self.user_data / "session-relaunch").exists())
            # Relaunch: the normal window is kept, the private one is not.
            self.choice.write_text("accept", encoding="utf-8")
            self.relaunch_from(shell)
            tf.proc.wait(30)
            self.assertEqual(tf.proc.returncode, 0)
            self.assertEqual(len(tf.events("dialog_shown")), 2)
            self.assertEqual([e["relaunch"] for e in tf.events("update_handoff")], [True])
        self.assertTrue(self.installer_started()[0].endswith(INSTALLER_ARGS))
        saved = json.loads((self.user_data / "session.json").read_text(encoding="utf-8"))
        self.assertEqual(saved["windows"], [{"tabs": ["torfox://newtab/"], "active": 0}])

    def test_nothing_is_kept_for_a_relaunch_while_history_is_cleared_on_exit(self) -> None:
        site = Fixture().__enter__()
        self.addCleanup(site.__exit__, None, None, None)
        url = site.html("/gone", "<!doctype html><title>Gone page</title><p>gone</p>")
        self.preset({"privacy.clearOnExit.history": True})
        release = self.publish("9.1.0", 2000000000)
        with self.torfox(delay_ms=300) as tf:
            shell = tf.shell()
            self.assertTrue(bridge_call(shell, "nav.go", {"input": url})["ok"])
            self.wait_ready_by_itself(tf, release)
            # A private window, so that TorFox asks: it must not promise the
            # normal windows back.
            known = tf.shell_ids()
            self.assertTrue(bridge_call(shell, "window.newPrivate")["ok"])
            wait_for(lambda: tf.events("window_created", private=True), 15, "private window")
            wait_state(tf.new_shell(known), lambda s: s["private"], "private shell")
            self.choice.write_text("accept", encoding="utf-8")
            self.relaunch_from(shell)
            tf.proc.wait(30)
            details = [d["details"] for d in tf.events("dialog_shown")]
            self.assertEqual(len(details), 1)
            self.assertIn("No tabs open again", details[0])
            self.assertNotIn("open again;", details[0])
            self.assertEqual([e["relaunch"] for e in tf.events("update_handoff")], [True])
            self.assertEqual(tf.events("session_saved"), [])
        self.installer_started()
        self.assertFalse((self.user_data / "session-relaunch").exists())
        self.assertFalse((self.user_data / "session.json").exists())
        with self.torfox() as tf:
            wait_state(tf.shell(), lambda s: [t["url"] for t in s["tabs"]] == ["torfox://newtab/"],
                       "one new tab")
            self.assertEqual(tf.events("session_relaunch"), [])
        self.assertNotIn(b"Gone page", b"".join(p.read_bytes() for p in self.user_data.rglob("*")
                                                if p.is_file() and p.suffix in (".json", "")))

    def test_a_relaunch_marker_without_an_update_restores_only_once(self) -> None:
        # The marker is a one-shot hint, removed at the next start whatever
        # it finds; it never turns "Restore previous tabs" on.
        site = Fixture().__enter__()
        self.addCleanup(site.__exit__, None, None, None)
        url = site.html("/once", "<!doctype html><title>Once</title>")
        (self.user_data / "session.json").write_text(json.dumps(
            {"format": 1, "windows": [{"tabs": [url], "active": 0}]}), encoding="utf-8")
        (self.user_data / "session-relaunch").write_text("1\n", encoding="utf-8")
        with self.torfox() as tf:
            wait_state(tf.shell(), lambda s: [t["url"] for t in s["tabs"]] == [url], "the kept tab")
        self.assertFalse((self.user_data / "session-relaunch").exists())
        with self.torfox() as tf:
            wait_state(tf.shell(), lambda s: [t["url"] for t in s["tabs"]] == ["torfox://newtab/"],
                       "one new tab")


@unittest.skipUnless(sys.platform == "win32", "the update E2E tests run on Windows")
class UpdateBeingInstalledTest(unittest.TestCase):
    """While the setup replaces TorFox's files (it holds a named mutex),
    starting TorFox does not run the old copy: it asks the setup to start
    TorFox when it is done, and exits."""

    MUTEX = "TorFoxUpdateInstalling"
    KEY = r"Software\TorFox"
    VALUE = "RelaunchAfterUpdate"

    def setUp(self) -> None:
        import ctypes
        import winreg
        self.ctypes, self.winreg = ctypes, winreg
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel32.CreateMutexW.restype = ctypes.c_void_p
        self.kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
        self.kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        self.user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-installing-"))
        self.addCleanup(shutil.rmtree, self.user_data, True)
        # The value lives in the real HKCU\Software\TorFox: put back what
        # was there.
        self.before = self.read_value()
        self.addCleanup(self.restore_value)

    def read_value(self):
        try:
            with self.winreg.OpenKey(self.winreg.HKEY_CURRENT_USER, self.KEY) as k:
                return self.winreg.QueryValueEx(k, self.VALUE)
        except FileNotFoundError:
            return None

    def restore_value(self) -> None:
        if self.before is not None:
            with self.winreg.CreateKey(self.winreg.HKEY_CURRENT_USER, self.KEY) as k:
                self.winreg.SetValueEx(k, self.VALUE, 0, self.before[1], self.before[0])
            return
        try:
            with self.winreg.OpenKey(self.winreg.HKEY_CURRENT_USER, self.KEY, 0,
                                     self.winreg.KEY_SET_VALUE) as k:
                self.winreg.DeleteValue(k, self.VALUE)
        except FileNotFoundError:
            pass

    def run_once(self) -> tuple[int, list[dict]]:
        tf = TorFox(user_data=self.user_data)
        proc = subprocess.run(tf.args, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return proc.returncode, tf.log()

    def test_a_start_during_an_update_asks_the_setup_to_start_torfox(self) -> None:
        handle = self.kernel32.CreateMutexW(None, True, self.MUTEX)
        self.assertTrue(handle, self.ctypes.get_last_error())
        try:
            code, log = self.run_once()
        finally:
            self.kernel32.CloseHandle(handle)
        self.assertEqual(code, 0)
        events = [e for e in log if e.get("ev") == "update_being_installed"]
        self.assertEqual([e["start_requested"] for e in events], [True])
        # No window, no browser: it stopped before CEF started.
        self.assertFalse([e for e in log if e.get("ev") in ("startup", "window_created")])
        self.assertEqual(self.read_value(), (1, self.winreg.REG_DWORD))
        self.assertFalse((self.user_data / "User Data").exists())

    def test_a_normal_start_clears_an_old_request(self) -> None:
        with self.winreg.CreateKey(self.winreg.HKEY_CURRENT_USER, self.KEY) as k:
            self.winreg.SetValueEx(k, self.VALUE, 0, self.winreg.REG_DWORD, 1)
        with TorFox(user_data=self.user_data) as tf:
            self.assertEqual(tf.events("update_being_installed"), [])
        self.assertIsNone(self.read_value())


if __name__ == "__main__":
    unittest.main()
