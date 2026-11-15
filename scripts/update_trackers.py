# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Regenerates third_party/easyprivacy/domains.txt, TorFox's tracker list.

Source: EasyPrivacy's tracking-server lists (https://github.com/easylist/easylist),
fetched at the commit pinned in third_party/easyprivacy/sources.lock.json. Every
file's SHA-256 is checked against the lock before it is used.

  python -I scripts/update_trackers.py            # verify the lock and regenerate
  python -I scripts/update_trackers.py --bump     # move the lock to the newest commit

Only plain domain rules are kept ("||example.com^", optionally "$third-party"):
TorFox blocks a request to a listed domain (or a subdomain of it) when the
request is third-party for the page. Rules with paths, other options or
exceptions are left out, so the list never blocks more than its authors meant.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
import urllib.request

REPO = pathlib.Path(__file__).resolve().parent.parent
DEST = REPO / "third_party" / "easyprivacy"
LOCK = DEST / "sources.lock.json"
OWNER_REPO = "easylist/easylist"
# Every EasyPrivacy source file at the pinned commit; the allowlists only
# remove domains (exceptions that cover a whole domain).
SOURCE_DIR = "easyprivacy"
# Only these hosts are ever contacted.
ALLOWED_HOSTS = ("api.github.com", "raw.githubusercontent.com")
RULE = re.compile(r"^(@@)?\|\|([a-z0-9][a-z0-9.-]*[a-z0-9])\^(?:\$([a-z,-]+))?$")
# Options a whole-domain rule may carry and still be kept: "third-party" and
# resource types. Rules limited to certain sites (domain=), to first-party
# requests (~third-party) or with other options are left out.
KEPT_OPTIONS = {"third-party", "script", "image", "xmlhttprequest", "ping", "subdocument", "other",
                "media", "font", "stylesheet", "websocket", "important"}
MAX_BYTES = 8 * 1024 * 1024


def fetch(url: str) -> bytes:
    host = url.split("/")[2]
    if host not in ALLOWED_HOSTS:
        raise SystemExit(f"refusing to fetch from {host}")
    req = urllib.request.Request(url, headers={"User-Agent": "torfox-update-trackers"})
    with urllib.request.urlopen(req, timeout=60) as r:
        if r.geturl().split("/")[2] not in ALLOWED_HOSTS:
            raise SystemExit(f"redirected to {r.geturl()}")
        data = r.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise SystemExit(f"{url} is larger than {MAX_BYTES} bytes")
    return data


def latest_commit() -> str:
    data = json.loads(fetch(f"https://api.github.com/repos/{OWNER_REPO}/commits?sha=master&per_page=1"))
    sha = data[0]["sha"]
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise SystemExit(f"unexpected commit id {sha!r}")
    return sha


def rules_from(text: str) -> tuple[set[str], set[str]]:
    """(blocked domains, excepted domains) of one filter file."""
    blocked, excepted = set(), set()
    for line in text.splitlines():
        m = RULE.match(line.strip())
        if not m:
            continue
        exception, domain, options = m.group(1), m.group(2), m.group(3)
        if "." not in domain or ".." in domain or re.fullmatch(r"[0-9.]+", domain):
            continue  # IP addresses and single labels are not domain rules.
        if exception:
            if not options:
                excepted.add(domain)
            continue
        if options and not set(options.split(",")) <= KEPT_OPTIONS:
            continue
        blocked.add(domain)
    return blocked, excepted


def source_files(commit: str) -> list[str]:
    listing = json.loads(fetch(f"https://api.github.com/repos/{OWNER_REPO}/contents/{SOURCE_DIR}?ref={commit}"))
    names = sorted(f"{SOURCE_DIR}/{x['name']}" for x in listing
                   if x["type"] == "file" and x["name"].endswith(".txt"))
    if not names:
        raise SystemExit("no source files listed")
    return names


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bump", action="store_true", help="pin the newest commit")
    args = ap.parse_args()

    lock = json.loads(LOCK.read_text(encoding="utf-8")) if LOCK.exists() else {}
    commit = latest_commit() if args.bump or not lock else lock["commit"]
    hashes = {} if args.bump or not lock else lock["files"]

    names = sorted(hashes) if hashes else source_files(commit)
    blocked: set[str] = set()
    excepted: set[str] = set()
    new_hashes = {}
    for name in names:
        data = fetch(f"https://raw.githubusercontent.com/{OWNER_REPO}/{commit}/{name}")
        digest = hashlib.sha256(data).hexdigest()
        if name in hashes and hashes[name] != digest:
            raise SystemExit(f"{name}: SHA-256 {digest} does not match the lock ({hashes[name]})")
        new_hashes[name] = digest
        b, e = rules_from(data.decode("utf-8"))
        blocked |= b
        excepted |= e
    domains = blocked - excepted

    if not domains:
        raise SystemExit("no domains found; refusing to write an empty list")
    DEST.mkdir(parents=True, exist_ok=True)
    text = "".join(f"{d}\n" for d in sorted(domains)).encode("utf-8")
    (DEST / "domains.txt").write_bytes(text)
    LOCK.write_text(json.dumps({"source": f"https://github.com/{OWNER_REPO}", "commit": commit,
                                "files": new_hashes,
                                "domains_sha256": hashlib.sha256(text).hexdigest(),
                                "domains": len(domains)}, indent=2) + "\n",
                    encoding="utf-8", newline="\n")
    print(f"{len(domains)} domains from {OWNER_REPO}@{commit[:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
