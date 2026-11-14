#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Download and OpenPGP-verify the official Tor Expert Bundles.

Intended to run in CI (where torproject.org is reachable). For each platform
it downloads the bundle and its detached .asc signature from
archive.torproject.org, requires a GOODSIG/VALIDSIG whose *primary* key
fingerprint is the Tor Browser Developers key, and records size + SHA-256.

The output directory receives the verified archives plus tor-lock.json, a
fragment ready to merge into deps.lock.json. Nothing unverified is written
to the output directory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

TOR_BROWSER_DEVELOPERS_PRIMARY_FPR = "EF6E286DDA85EA2A4BA7DE684E2C6E8793298290"
BASE = "https://archive.torproject.org/tor-package-archive/torbrowser/"
PLATFORMS = {
    "windows-x86_64": "tor/tor.exe",
    "macos-x86_64": "tor/tor",
    "macos-aarch64": "tor/tor",
}
VERSION_RE = re.compile(r"^\d+\.\d+(\.\d+)?(a\d+)?$")


def download(url: str, dest: Path) -> None:
    req = urllib.request.Request(url, headers={"User-Agent": "TorFox-build-fetch"})
    with urllib.request.urlopen(req, timeout=120) as resp, dest.open("wb") as out:
        shutil.copyfileobj(resp, out)


def gpg_verify(sig: Path, data: Path) -> dict:
    proc = subprocess.run(["gpg", "--status-fd", "1", "--verify", str(sig), str(data)],
                          capture_output=True, text=True)
    status = proc.stdout.splitlines()
    good = any(line.startswith("[GNUPG:] GOODSIG ") for line in status)
    validsig = [line.split() for line in status if line.startswith("[GNUPG:] VALIDSIG ")]
    if proc.returncode != 0 or not good or not validsig:
        raise SystemExit(f"signature verification FAILED for {data.name}:\n{proc.stdout}\n{proc.stderr}")
    fields = validsig[0]
    signing_fpr, primary_fpr = fields[2], fields[-1]
    if primary_fpr.upper() != TOR_BROWSER_DEVELOPERS_PRIMARY_FPR:
        raise SystemExit(f"{data.name}: signed by unexpected primary key {primary_fpr}")
    return {"signing_subkey": signing_fpr, "primary_key": primary_fpr}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="Tor Browser version, e.g. 16.0.1")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    if not VERSION_RE.match(args.version):
        raise SystemExit(f"refusing suspicious version string: {args.version!r}")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    lock = {"tor_browser_version": args.version, "base_url": f"{BASE}{args.version}/", "files": {}}
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        for platform, tor_member in PLATFORMS.items():
            name = f"tor-expert-bundle-{platform}-{args.version}.tar.gz"
            url = f"{BASE}{args.version}/{name}"
            archive, sig = tmp / name, tmp / (name + ".asc")
            print(f"downloading {url}", flush=True)
            download(url, archive)
            download(url + ".asc", sig)
            verdict = gpg_verify(sig, archive)
            with tarfile.open(archive, "r:gz") as tar:
                members = tar.getnames()
            if tor_member not in members:
                raise SystemExit(f"{name}: expected member {tor_member} not found; archive has: "
                                 + ", ".join(members[:60]))
            data = archive.read_bytes()
            lock["files"][platform] = {
                "name": name,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "pgp": verdict,
                "members": sorted(m for m in members if m.count("/") <= 2),
            }
            shutil.copy2(archive, out / name)
            shutil.copy2(sig, out / (name + ".asc"))
            print(f"verified {name}: sha256={lock['files'][platform]['sha256']}", flush=True)
    (out / "tor-lock.json").write_text(json.dumps(lock, indent=2) + "\n")
    print(json.dumps(lock, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
