#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Release tooling for .github/workflows/release.yml (see docs/RELEASING.md).

  version
      Prints the CMake project version.
  preflight --tag vX.Y.Z
      Fails unless the tag is v<the CMake project version>, config/release.json
      names the repository and holds the release public key, the release notes
      file docs/release-notes/X.Y.Z.txt exists. Nothing is built or published
      when it fails. (The signing secret is checked by the sign job, the only
      job that can read it.)
  manifest --version X.Y.Z --notes <file> --out <manifest.json> --comment-out <file> <files...>
      Writes the update manifest (one file per platform: size and SHA-256) and
      the trusted comment the minisign signature must carry.
  check --dir <folder> --public-key <key>
      Checks <folder>/manifest.json and manifest.json.minisig as TorFox will:
      a prehashed minisign signature by that key, the trusted comment, and
      every listed file's size and SHA-256. Run after signing, before publishing.

The manifest format is the one src/core/update/release.cc reads; the
updater's end-to-end tests build their releases with build_manifest() too.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import minisign  # noqa: E402

PLATFORM_FILES = {
    "windows-x64": "TorFox-{v}-win-x64-setup.exe",
    "macos-arm64": "TorFox-{v}-mac-arm64.dmg",
    "macos-x64": "TorFox-{v}-mac-x64.dmg",
}
MAX_MANIFEST = 64 * 1024
MAX_NOTES = 16 * 1024
MAX_ASSET = 1 << 30
VERSION = re.compile(r"^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$")


class ReleaseError(Exception):
    pass


def trusted_comment(version: str, timestamp: int) -> str:
    return f"torfox-release version:{version} timestamp:{timestamp}"


def build_manifest(version: str, timestamp: int, notes: str, assets: dict[str, dict]) -> bytes:
    """|assets|: platform -> {"name", "size", "sha256"}."""
    if not VERSION.match(version):
        raise ReleaseError(f"invalid version {version!r}")
    if timestamp <= 0:
        raise ReleaseError("invalid timestamp")
    if len(notes.encode("utf-8")) > MAX_NOTES:
        raise ReleaseError(f"the release notes are longer than {MAX_NOTES} bytes")
    if not assets:
        raise ReleaseError("no files")
    for platform, a in assets.items():
        if platform not in PLATFORM_FILES:
            raise ReleaseError(f"unknown platform {platform}")
        if a["name"] != PLATFORM_FILES[platform].format(v=version):
            raise ReleaseError(f"{a['name']} is not the {platform} file of TorFox {version}")
        if not (0 < a["size"] <= MAX_ASSET) or not re.fullmatch(r"[0-9a-f]{64}", a["sha256"]):
            raise ReleaseError(f"invalid size or SHA-256 for {a['name']}")
    data = json.dumps({"format": 1, "product": "TorFox", "version": version, "timestamp": timestamp,
                       "notes": notes, "assets": assets}, separators=(",", ":")).encode("utf-8")
    if len(data) > MAX_MANIFEST:
        raise ReleaseError("the manifest is too large")
    return data


def file_digest(path: Path) -> tuple[int, str]:
    h = hashlib.sha256()
    size = 0
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            size += len(chunk)
            h.update(chunk)
    return size, h.hexdigest()


def project_version() -> str:
    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    found = set(re.findall(r"project\(TorFox VERSION ([0-9]+\.[0-9]+\.[0-9]+)", text))
    if len(found) != 1:
        raise ReleaseError(f"could not read one project version from CMakeLists.txt: {found}")
    return found.pop()


def release_config() -> dict:
    return json.loads((REPO / "config" / "release.json").read_text(encoding="utf-8"))


# ----- commands --------------------------------------------------------------------------

def preflight(tag: str) -> None:
    version = project_version()
    if tag != f"v{version}":
        raise ReleaseError(f"the tag {tag!r} does not match the project version {version} (expected v{version})")
    config = release_config()
    if not config.get("github_owner") or not config.get("github_repo"):
        raise ReleaseError("config/release.json does not name the repository")
    key = config.get("update_public_key", "")
    if not key:
        raise ReleaseError("config/release.json has no update_public_key: TorFox could not check this "
                           "release. Make the key first (docs/RELEASING.md).")
    minisign.parse_public_key(key)
    notes = REPO / "docs" / "release-notes" / f"{version}.txt"
    if not notes.is_file():
        raise ReleaseError(f"{notes.relative_to(REPO)} is missing: write the release notes first")
    if len(notes.read_bytes()) > MAX_NOTES:
        raise ReleaseError(f"{notes.relative_to(REPO)} is longer than {MAX_NOTES} bytes")
    # The signing secret is not checked here: only the workflow's sign job,
    # in the "release" environment, can read it, and that job fails without it.
    print(f"Release {version}: tag, version, public key and notes present")


def make(version: str, notes_file: Path, out: Path, comment_out: Path, files: list[Path],
         timestamp: int | None) -> None:
    by_name = {f.name: f for f in files}
    if len(by_name) != len(files):
        raise ReleaseError("two files have the same name")
    assets = {}
    for platform, pattern in PLATFORM_FILES.items():
        name = pattern.format(v=version)
        if name not in by_name:
            raise ReleaseError(f"{name} ({platform}) is missing: a release needs every platform")
        size, sha256 = file_digest(by_name.pop(name))
        assets[platform] = {"name": name, "size": size, "sha256": sha256}
    if by_name:
        raise ReleaseError(f"unexpected files: {', '.join(sorted(by_name))}")
    ts = timestamp if timestamp is not None else int(time.time())
    notes = notes_file.read_text(encoding="utf-8").strip()
    out.write_bytes(build_manifest(version, ts, notes, assets))
    comment_out.write_text(trusted_comment(version, ts), encoding="utf-8")
    print(f"{out}: TorFox {version}, timestamp {ts}")


def check(folder: Path, public_key: str) -> None:
    manifest = (folder / "manifest.json").read_bytes()
    signature = (folder / "manifest.json.minisig").read_text(encoding="utf-8")
    try:
        comment = minisign.verify(public_key, signature, manifest, require_prehashed=True)
    except minisign.MinisignError as e:
        raise ReleaseError(f"manifest.json: the signature does not verify: {e}") from e
    m = json.loads(manifest)
    if comment != trusted_comment(m["version"], m["timestamp"]):
        raise ReleaseError(f"the signed comment {comment!r} does not match the manifest")
    rebuilt = build_manifest(m["version"], m["timestamp"], m["notes"], m["assets"])
    if json.loads(rebuilt) != m:
        raise ReleaseError("manifest.json does not follow the manifest rules")
    for platform in PLATFORM_FILES:
        a = m["assets"].get(platform)
        if not a:
            raise ReleaseError(f"no {platform} file in the manifest")
        if file_digest(folder / a["name"]) != (a["size"], a["sha256"]):
            raise ReleaseError(f"{a['name']} does not match the manifest")
    print(f"manifest.json: signed by the release key, TorFox {m['version']}, every file matches")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("version")
    p = sub.add_parser("preflight")
    p.add_argument("--tag", required=True)
    p = sub.add_parser("manifest")
    p.add_argument("--version", required=True)
    p.add_argument("--notes", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--comment-out", type=Path, required=True)
    p.add_argument("--timestamp", type=int)
    p.add_argument("files", type=Path, nargs="+")
    p = sub.add_parser("check")
    p.add_argument("--dir", type=Path, required=True)
    p.add_argument("--public-key", required=True)
    args = ap.parse_args()
    try:
        if args.cmd == "version":
            print(project_version())
        elif args.cmd == "preflight":
            preflight(args.tag)
        elif args.cmd == "manifest":
            make(args.version, args.notes, args.out, args.comment_out, args.files, args.timestamp)
        else:
            check(args.dir, args.public_key)
    except (ReleaseError, minisign.MinisignError, OSError, ValueError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
