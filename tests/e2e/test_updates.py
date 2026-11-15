# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Stage 7: Settings > Updates against a fake GitHub on loopback.

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
import sys
import tempfile
import time
import unittest

from harness import REPO, Fixture, TorFox, bridge_call, wait_for

sys.path.insert(0, str(REPO / "scripts"))
import minisign  # noqa: E402
import release as release_tool  # noqa: E402

FAKE_INSTALLER = (REPO / "out" / "test" / "tests" / "e2e" / "helpers" / "Release" /
                  "torfox_fake_installer.exe")
REPOSITORY = "test/TorFox"
PLATFORM = "windows-x64"
INSTALLER_ARGS = "/SILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE"


class Key:
    def __init__(self) -> None:
        self.seed = os.urandom(32)
        self.key_id = os.urandom(8)
        self.public = minisign.public_key_text(self.seed, self.key_id)

    def sign(self, data: bytes, comment: str) -> str:
        return minisign.sign(self.seed, self.key_id, data, comment)


@unittest.skipUnless(sys.platform == "win32", "the update E2E tests run on Windows")
class UpdateTest(unittest.TestCase):
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
               configured: bool = True) -> TorFox:
        args = []
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
            self.assertEqual(s["state"], "up_to_date", s)
            self.assertIn("older than one TorFox has already seen", s["message"])
            self.assertEqual(self.requests_for(old["path"]), [])
        # Remembered across restarts.
        saved = json.loads((self.user_data / "update.json").read_text(encoding="utf-8"))
        self.assertEqual(saved["newestSeen"], 2000000000)
        with self.torfox() as tf:
            page = self.settings(tf)
            self.assertEqual(self.check(page)["state"], "up_to_date")
            # The newest one is still offered.
            self.publish("9.3.0", 2000000000)
            self.assertEqual(self.check(page)["state"], "available")

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
            commands = ("update.status", "update.check", "update.download", "update.install")
            for cmd in commands:
                r = bridge_call(shell, cmd)
                self.assertFalse(r["ok"], (cmd, r))
            self.assertEqual([e["reason"] for e in tf.events("bridge_reject")],
                             ["unknown_command"] * len(commands))
            # Web pages have no bridge at all (test_isolation.py).
        self.assertEqual(self.github.requests, [])
        self.assertEqual(self.requests_for(release["path"]), [])

    def test_a_build_without_a_key_never_looks_for_updates(self) -> None:
        # The test build's own config/release.json has no key (like this
        # repository's): no fake GitHub configured.
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


if __name__ == "__main__":
    unittest.main()
