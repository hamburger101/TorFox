#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Download, verify and extract pinned third-party dependencies.

Every artifact listed in deps.lock.json is verified by size, SHA-256 and (when
present) the SHA-1 published by the upstream project before it is extracted.
A missing or mismatching hash is a hard failure; nothing is ever extracted
from an unverified file.

Usage:
  python scripts/fetch_deps.py cef windows64-standard
  python scripts/fetch_deps.py cef macosarm64-minimal --file path/to/archive
  python scripts/fetch_deps.py cef windows64-standard --pin-sha256   (maintainers only)

Prints the extracted root directory on success (last line of stdout).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LOCK_PATH = REPO_ROOT / "deps.lock.json"
DEPS_DIR = Path(os.environ.get("TORFOX_DEPS_DIR", REPO_ROOT / ".deps"))
CHUNK = 1024 * 1024


class VerificationError(Exception):
    pass


def load_lock() -> dict:
    with LOCK_PATH.open("r", encoding="utf-8") as f:
        return json.load(f)


def save_lock(lock: dict) -> None:
    tmp = LOCK_PATH.with_suffix(".json.tmp")
    with tmp.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(lock, f, indent=2)
        f.write("\n")
    os.replace(tmp, LOCK_PATH)


def hash_file(path: Path) -> tuple[str, str, int]:
    sha1 = hashlib.sha1()
    sha256 = hashlib.sha256()
    size = 0
    with path.open("rb") as f:
        while True:
            block = f.read(CHUNK)
            if not block:
                break
            sha1.update(block)
            sha256.update(block)
            size += len(block)
    return sha1.hexdigest(), sha256.hexdigest(), size


def download(url: str, dest: Path) -> None:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https":
        raise VerificationError(f"refusing non-https download URL: {url}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "TorFox-build-fetch"})
    print(f"Downloading {url}", file=sys.stderr)
    with urllib.request.urlopen(req, timeout=60) as resp, part.open("wb") as out:
        shutil.copyfileobj(resp, out, CHUNK)
    os.replace(part, dest)


def verify(path: Path, entry: dict, allow_pin: bool) -> str:
    sha1, sha256, size = hash_file(path)
    if entry.get("size") is not None and size != entry["size"]:
        raise VerificationError(f"{path.name}: size {size} != pinned {entry['size']}")
    if entry.get("sha1") and sha1 != entry["sha1"]:
        raise VerificationError(f"{path.name}: SHA-1 {sha1} != upstream {entry['sha1']}")
    pinned = entry.get("sha256")
    if pinned:
        if sha256 != pinned:
            raise VerificationError(f"{path.name}: SHA-256 {sha256} != pinned {pinned}")
    elif not allow_pin:
        raise VerificationError(
            f"{path.name}: no SHA-256 pinned in deps.lock.json (computed {sha256}). "
            "A maintainer must pin it with --pin-sha256 after checking the upstream hash."
        )
    elif not entry.get("sha1"):
        raise VerificationError(f"{path.name}: refusing to pin SHA-256 without an upstream hash to cross-check")
    return sha256


def safe_extract(archive: Path, dest_parent: Path, root_name: str) -> Path:
    """Extract into a temp dir under dest_parent, then atomically move into place.

    Archives with a single top-level directory named root_name (CEF) are moved
    as-is; archives with several top-level entries (Tor Expert Bundle) are
    placed inside a directory named root_name. The 'data' filter rejects
    absolute paths, '..' traversal, device files and links escaping the tree.
    """
    dest_parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix=".extract-", dir=dest_parent))
    try:
        with tarfile.open(archive, "r:*") as tar:
            tar.extractall(tmp / "x", filter="data")
        children = list((tmp / "x").iterdir())
        if len(children) == 1 and children[0].is_dir() and children[0].name == root_name:
            source = children[0]
        else:
            source = tmp / "x"
        final = dest_parent / root_name
        if final.exists():
            shutil.rmtree(final)
        os.replace(source, final)
        return final
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def fetch(kind: str, key: str, file_override: str | None, allow_pin: bool, verify_only: bool = False) -> Path:
    lock = load_lock()
    if kind not in lock:
        raise VerificationError(f"unknown dependency kind '{kind}'")
    section = lock[kind]
    files = section.get("files", {})
    if key not in files:
        raise VerificationError(f"unknown {kind} artifact '{key}' (known: {', '.join(files)})")
    entry = files[key]
    name = entry["name"]

    dest_parent = DEPS_DIR / kind
    marker_name = f"{name}.verified"
    marker = dest_parent / marker_name
    install_as = entry.get("install_as")
    if install_as:
        # Single-file artifact: copied (not extracted) to dest_parent/install_as.
        rel = Path(install_as)
        if rel.is_absolute() or ".." in rel.parts:
            raise VerificationError(f"invalid install_as for {name}: {install_as}")
        root_name = rel.parts[0]
    else:
        root_name = name
        for ext in (".tar.bz2", ".tar.gz", ".tar.xz", ".tgz"):
            if root_name.endswith(ext):
                root_name = root_name[: -len(ext)]
    if entry.get("sha256") and marker.exists() and marker.read_text().strip() == entry["sha256"]:
        root = dest_parent / root_name
        if root.is_dir():
            return root

    if file_override:
        archive = Path(file_override).resolve()
    else:
        archive = DEPS_DIR / "downloads" / name
        if not archive.exists():
            url = section["base_url"] + urllib.parse.quote(name)
            download(url, archive)

    sha256 = verify(archive, entry, allow_pin)
    if not entry.get("sha256"):
        entry["sha256"] = sha256
        save_lock(lock)
        print(f"Pinned SHA-256 for {name}: {sha256}", file=sys.stderr)

    if verify_only:
        print(f"Verified {name}", file=sys.stderr)
        return archive

    if install_as:
        print(f"Verified {name}; installing", file=sys.stderr)
        root = dest_parent / root_name
        if root.exists():
            shutil.rmtree(root)
        target = dest_parent / install_as
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(archive, target)
    else:
        print(f"Verified {name}; extracting", file=sys.stderr)
        root = safe_extract(archive, dest_parent, root_name)
    marker.write_text(sha256 + "\n")
    return root


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", help="dependency kind, e.g. cef or tor")
    ap.add_argument("key", help="artifact key in deps.lock.json, e.g. windows64-standard")
    ap.add_argument("--file", help="use an already-downloaded archive instead of downloading")
    ap.add_argument("--pin-sha256", action="store_true", help="record the SHA-256 if none is pinned (maintainers)")
    ap.add_argument("--verify-only", action="store_true", help="download and verify without extracting")
    args = ap.parse_args()
    try:
        root = fetch(args.kind, args.key, args.file, args.pin_sha256, args.verify_only)
    except (VerificationError, OSError, tarfile.TarError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    print(root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
