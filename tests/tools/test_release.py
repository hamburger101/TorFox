# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""scripts/release.py: the release workflow's manifest, signature check and
preflight. Signatures are made by the official minisign tool when it has
been fetched (scripts/fetch_deps.py minisign <platform>), and by
scripts/minisign.py's test signer otherwise; both with throw-away keys.

Usage:  python -m unittest discover -v -s tests/tools
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import minisign  # noqa: E402
import release  # noqa: E402

VERSION = "1.2.3"
NAMES = [f"TorFox-{VERSION}-win-x64-setup.exe", f"TorFox-{VERSION}-mac-arm64.dmg",
         f"TorFox-{VERSION}-mac-x64.dmg"]


def official_minisign() -> pathlib.Path | None:
    root = REPO / ".deps" / "minisign"
    name = "minisign.exe" if sys.platform == "win32" else "minisign"
    found = sorted(p for p in root.rglob(name) if "x86_64" in str(p)) if root.is_dir() else []
    return found[0] if found else None


class ReleaseToolTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="torfox-release-test-"))
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.dist = self.dir / "dist"
        self.dist.mkdir()
        for i, n in enumerate(NAMES):
            (self.dist / n).write_bytes(os.urandom(1000 + i))
        self.notes = self.dir / "notes.txt"
        self.notes.write_text("Fixes.\n", encoding="utf-8")
        self.minisign = official_minisign()
        if self.minisign:
            pub, sec = self.dir / "k.pub", self.dir / "k.key"
            subprocess.run([str(self.minisign), "-G", "-W", "-p", str(pub), "-s", str(sec)], check=True,
                           stdin=subprocess.DEVNULL, capture_output=True)
            self.public = pub.read_text(encoding="utf-8").splitlines()[1].strip()
            self.secret = sec
        else:
            self.seed, self.key_id = os.urandom(32), os.urandom(8)
            self.public = minisign.public_key_text(self.seed, self.key_id)

    def run_tool(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-I", str(REPO / "scripts" / "release.py"), *args],
                              capture_output=True, text=True)

    def make_manifest(self, files: list[pathlib.Path] | None = None) -> subprocess.CompletedProcess:
        files = files if files is not None else [self.dist / n for n in NAMES]
        return self.run_tool("manifest", "--version", VERSION, "--notes", str(self.notes),
                             "--out", str(self.dist / "manifest.json"),
                             "--comment-out", str(self.dir / "comment.txt"),
                             "--timestamp", "1790000000", *map(str, files))

    def sign(self, comment: str | None = None) -> None:
        comment = comment or (self.dir / "comment.txt").read_text(encoding="utf-8")
        manifest = self.dist / "manifest.json"
        if self.minisign:
            subprocess.run([str(self.minisign), "-S", "-s", str(self.secret), "-m", str(manifest),
                            "-x", str(self.dist / "manifest.json.minisig"), "-t", comment,
                            "-c", "TorFox release manifest"], check=True, stdin=subprocess.DEVNULL,
                           capture_output=True)
        else:
            sig = minisign.sign(self.seed, self.key_id, manifest.read_bytes(), comment)
            (self.dist / "manifest.json.minisig").write_text(sig, encoding="utf-8")

    def check(self, public: str | None = None) -> subprocess.CompletedProcess:
        return self.run_tool("check", "--dir", str(self.dist), "--public-key", public or self.public)

    # ----- tests ---------------------------------------------------------------------

    def test_a_signed_release_passes_the_check(self) -> None:
        r = self.make_manifest()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.dir / "comment.txt").read_text(encoding="utf-8"),
                         f"torfox-release version:{VERSION} timestamp:1790000000")
        m = json.loads((self.dist / "manifest.json").read_bytes())
        self.assertEqual(m["format"], 1)
        self.assertEqual(m["product"], "TorFox")
        self.assertEqual(m["notes"], "Fixes.")
        self.assertEqual(sorted(m["assets"]), ["macos-arm64", "macos-x64", "windows-x64"])
        self.assertEqual(m["assets"]["windows-x64"]["size"], 1000)
        self.sign()
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_a_release_needs_every_platform_and_no_other_files(self) -> None:
        r = self.make_manifest([self.dist / n for n in NAMES[:2]])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("missing", r.stderr)
        extra = self.dist / "notes.exe"
        extra.write_bytes(b"x")
        r = self.make_manifest([*(self.dist / n for n in NAMES), extra])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unexpected", r.stderr)

    def test_the_check_refuses_changed_files_comments_and_other_keys(self) -> None:
        self.assertEqual(self.make_manifest().returncode, 0)
        self.sign()
        # Another key.
        other = minisign.public_key_text(os.urandom(32), os.urandom(8))
        self.assertNotEqual(self.check(other).returncode, 0)
        # A changed file.
        f = self.dist / NAMES[1]
        data = f.read_bytes()
        f.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        r = self.check()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("does not match", r.stderr)
        f.write_bytes(data)
        self.assertEqual(self.check().returncode, 0)
        # A signed comment that is not the manifest's.
        self.sign(f"torfox-release version:{VERSION} timestamp:1790000001")
        r = self.check()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("comment", r.stderr)
        # A changed manifest.
        self.sign()
        m = self.dist / "manifest.json"
        m.write_bytes(m.read_bytes().replace(b'"Fixes."', b'"Fixed."'))
        self.assertNotEqual(self.check().returncode, 0)

    def test_manifest_rules(self) -> None:
        good = {"name": NAMES[0], "size": 1, "sha256": "0" * 64}
        release.build_manifest(VERSION, 1, "", {"windows-x64": good})
        for version, ts, notes, assets in [
            ("1.2", 1, "", {"windows-x64": good}),
            ("01.2.3", 1, "", {"windows-x64": good}),
            (VERSION, 0, "", {"windows-x64": good}),
            (VERSION, 1, "x" * (16 * 1024 + 1), {"windows-x64": good}),
            (VERSION, 1, "", {}),
            (VERSION, 1, "", {"linux-x64": good}),
            (VERSION, 1, "", {"macos-x64": good}),
            (VERSION, 1, "", {"windows-x64": {**good, "name": "TorFox-1.2.4-win-x64-setup.exe"}}),
            (VERSION, 1, "", {"windows-x64": {**good, "size": 0}}),
            (VERSION, 1, "", {"windows-x64": {**good, "sha256": "A" * 64}}),
        ]:
            with self.assertRaises(release.ReleaseError, msg=(version, ts, notes[:5], assets)):
                release.build_manifest(version, ts, notes, assets)

    def test_preflight_fails_closed(self) -> None:
        env = {k: v for k, v in os.environ.items() if k != "MINISIGN_SECRET_KEY"}
        version = release.project_version()

        def preflight(tag: str, secret: str | None) -> subprocess.CompletedProcess:
            e = dict(env)
            if secret is not None:
                e["MINISIGN_SECRET_KEY"] = secret
            return subprocess.run([sys.executable, "-I", str(REPO / "scripts" / "release.py"),
                                   "preflight", "--tag", tag], capture_output=True, text=True, env=e)
        # Preflight runs without the secret (only the sign job can read it),
        # so a secret in the environment changes nothing.
        for secret in (None, "x"):
            r = preflight("v99.0.0", secret)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("does not match", r.stderr)
            r = preflight(f"v{version}", secret)
            if not json.loads((REPO / "config" / "release.json").read_text())["update_public_key"]:
                # This repository has no release key yet: no release can be made.
                self.assertNotEqual(r.returncode, 0)
                self.assertIn("update_public_key", r.stderr)

    def test_only_the_sign_job_can_read_the_release_key(self) -> None:
        text = (REPO / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        jobs = re.split(r"(?m)^  (?=[a-z]+:\n)", text.split("\njobs:\n", 1)[1])
        by_name = {j.split(":", 1)[0]: j for j in jobs if j.strip()}
        self.assertIn("sign", by_name)
        for name, body in by_name.items():
            if name == "sign":
                self.assertIn("environment: release", body)
                self.assertIn("secrets.MINISIGN_SECRET_KEY", body)
                # It gets the manifest, not the builds.
                self.assertIn("name: manifest", body)
                self.assertNotIn("merge-multiple", body)
            else:
                self.assertNotIn("secrets.", body, name)
        self.assertNotIn("secrets.", text.split("\njobs:\n", 1)[0])
        # Every action is pinned by commit.
        for use in re.findall(r"uses:\s*(\S+)", text):
            self.assertRegex(use, r"@[0-9a-f]{40}$", use)
        self.assertIn('git merge-base --is-ancestor "$GITHUB_SHA" origin/main', by_name["preflight"])


if __name__ == "__main__":
    unittest.main()
