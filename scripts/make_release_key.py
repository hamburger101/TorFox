# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Makes TorFox's release key, once (docs/RELEASING.md, "Once: the release key").

  python -I scripts/make_release_key.py

Runs on Windows, macOS or Linux, from a checkout of the repository, with the
GitHub CLI (gh) signed in as a maintainer. It:
- fetches the official minisign 0.12 if needed (scripts/fetch_deps.py checks
  it against its upstream signature);
- makes a key without a password, in a temporary folder;
- stores the secret key as the secret MINISIGN_SECRET_KEY of the repository's
  "release" environment, and nowhere else: it is never printed;
- overwrites and deletes the secret key file;
- writes the public key into config/release.json, for you to commit.

Refuses to run if config/release.json already has a public key: a new key
would make every installed TorFox refuse the next update (see RELEASING.md
for replacing a key).
"""

from __future__ import annotations

import json
import os
import pathlib
import platform
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
CONFIG = REPO / "config" / "release.json"
PUBLIC_KEY = re.compile(r"RW[A-Za-z0-9+/]{54}")


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def deps_platform() -> str:
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def find_minisign() -> pathlib.Path | None:
    root = REPO / ".deps" / "minisign"
    name = "minisign.exe" if sys.platform == "win32" else "minisign"
    found = sorted(p for p in root.rglob(name) if p.is_file()) if root.is_dir() else []
    arch = "aarch64" if platform.machine().lower() in ("arm64", "aarch64") else "x86_64"
    for p in found:
        if arch in p.parts:
            return p
    return found[0] if len(found) == 1 else None


def run(args: list[str], **kwargs) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(args, **kwargs)
    except FileNotFoundError:
        fail(f"{args[0]} was not found")
        raise


def main() -> None:
    raw = CONFIG.read_bytes().decode("utf-8")
    config = json.loads(raw)
    if config.get("update_public_key"):
        fail("config/release.json already has a public key; no new key was made.")
    owner, repo = config.get("github_owner", ""), config.get("github_repo", "")
    if not owner or not repo:
        fail("config/release.json does not name the repository.")
    slug = f"{owner}/{repo}"

    if run(["gh", "auth", "status"], capture_output=True).returncode != 0:
        fail("the GitHub CLI is not signed in; run: gh auth login")
    if run(["gh", "api", f"repos/{slug}/environments/release"], capture_output=True).returncode != 0:
        fail(f"{slug} has no 'release' environment yet (RELEASING.md, step 4).")

    minisign = find_minisign()
    if minisign is None:
        if run([sys.executable, "-I", str(REPO / "scripts" / "fetch_deps.py"), "minisign",
                deps_platform()]).returncode != 0:
            fail("minisign could not be fetched and checked.")
        minisign = find_minisign()
        if minisign is None:
            fail("minisign was fetched, but no binary for this computer was found in .deps/minisign.")

    with tempfile.TemporaryDirectory(prefix="torfox-release-key-") as d:
        pub = pathlib.Path(d) / "torfox-release.pub"
        key = pathlib.Path(d) / "torfox-release.key"
        if run([str(minisign), "-G", "-W", "-p", str(pub), "-s", str(key)],
               stdout=subprocess.DEVNULL).returncode != 0 or not key.is_file():
            fail("minisign could not make the key.")
        try:
            stored = run(["gh", "secret", "set", "MINISIGN_SECRET_KEY", "--env", "release", "-R", slug],
                         input=key.read_bytes()).returncode == 0
        finally:
            key.write_bytes(os.urandom(key.stat().st_size))
            key.unlink()
        if not stored:
            fail("the secret could not be stored in GitHub; the key was deleted and nothing was changed.")
        lines = pub.read_text(encoding="utf-8").splitlines()
        public = lines[1].strip() if len(lines) > 1 else ""

    if not PUBLIC_KEY.fullmatch(public):
        fail("minisign wrote an unexpected public key; config/release.json was not changed. "
             "Run this again: it replaces the secret stored in GitHub.")
    old = '"update_public_key": ""'
    if raw.count(old) != 1:
        fail("config/release.json is not in the expected form; it was not changed.")
    CONFIG.write_bytes(raw.replace(old, f'"update_public_key": "{public}"').encode("utf-8"))
    print(f"Done. The secret key is stored only in GitHub ({slug}, environment 'release').")
    print(f"Public key, now in config/release.json (commit it): {public}")


if __name__ == "__main__":
    main()
