#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Vendor the Public Suffix List (https://publicsuffix.org) at one commit.

Downloads public_suffix_list.dat and the official test vectors
(tests/test_psl.txt) from github.com/publicsuffix/list at |commit|, checks
each file against the git blob hash GitHub reports for that commit, and
writes them to third_party/publicsuffix/ with SOURCE.json recording the
commit and SHA-256 of each file. Run by maintainers; the build only reads the
vendored copy (scripts/gen_psl.py checks SOURCE.json).

Usage:  python scripts/update_psl.py <commit-sha>
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEST = REPO / "third_party" / "publicsuffix"
FILES = {"public_suffix_list.dat": "public_suffix_list.dat", "tests/test_psl.txt": "test_psl.txt"}


def get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "TorFox-build-fetch"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read()


def git_blob_sha1(data: bytes) -> str:
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def main(argv: list[str]) -> int:
    if len(argv) != 2 or not re.fullmatch(r"[0-9a-f]{40}", argv[1]):
        print(__doc__, file=sys.stderr)
        return 2
    commit = argv[1]
    info = json.loads(get(f"https://api.github.com/repos/publicsuffix/list/commits/{commit}"))
    source = {"repository": "https://github.com/publicsuffix/list", "commit": commit,
              "date": info["commit"]["committer"]["date"], "files": {}}
    DEST.mkdir(parents=True, exist_ok=True)
    for remote, local in FILES.items():
        meta = json.loads(get(f"https://api.github.com/repos/publicsuffix/list/contents/{remote}?ref={commit}"))
        data = get(f"https://raw.githubusercontent.com/publicsuffix/list/{commit}/{remote}")
        if git_blob_sha1(data) != meta["sha"]:
            print(f"ERROR: {remote}: git blob hash mismatch", file=sys.stderr)
            return 1
        (DEST / local).write_bytes(data)
        source["files"][local] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
                                  "git_blob": meta["sha"]}
        print(f"{local}: {len(data)} bytes, verified against git blob {meta['sha']}")
    (DEST / "SOURCE.json").write_bytes((json.dumps(source, indent=2) + "\n").encode())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
