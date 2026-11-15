# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Regenerates third_party/easylist/filters.txt, TorFox's ad-blocking list.

Source: EasyList (https://github.com/easylist/easylist), the files its own
build template (easylist.template) puts into easylist.txt, fetched at the
commit pinned in third_party/easylist/sources.lock.json. Every file's
SHA-256 is checked against the lock before it is used.

  python -I scripts/update_adblock.py            # verify the lock and regenerate
  python -I scripts/update_adblock.py --bump     # move the lock to the newest commit

Comments and rules in syntax TorFox does not implement are left out here
(regular expressions, uBlock Origin's and Adblock Plus's procedural element
hiding, scriptlets, and options that rewrite rather than block). The engine
(src/core/adblock) parses what remains and still ignores anything it does not
understand, so a rule is never applied more broadly than its authors meant.
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
DEST = REPO / "third_party" / "easylist"
LOCK = DEST / "sources.lock.json"
OWNER_REPO = "easylist/easylist"
TEMPLATE = "easylist.template"
# Only these hosts are ever contacted.
ALLOWED_HOSTS = ("api.github.com", "raw.githubusercontent.com")
MAX_BYTES = 8 * 1024 * 1024
INCLUDE = re.compile(r"^%include easylist:([a-z0-9_]+/[a-z0-9_]+\.txt)%$")

# Element hiding that is not a plain CSS selector.
COSMETIC_UNSUPPORTED = ("#?#", "#@?#", "#$#", "#@$#", "#%#", "##+js", "#@#+js", "##^", "#@#^")
PROCEDURAL = re.compile(r":(-abp-|has-text\(|matches-css|upward\(|xpath\(|remove\(|style\(|"
                        r"min-text-length\(|watch-attr\(|matches-path\(|matches-attr\(|"
                        r"matches-media\(|matches-prop\(|others\()")
# Options that change a request instead of blocking it.
REWRITING = {"rewrite", "csp", "redirect", "redirect-rule", "removeparam", "replace", "header",
             "permissions", "urltransform", "empty", "mp4", "uritransform", "urlskip"}


def fetch(url: str) -> bytes:
    host = url.split("/")[2]
    if host not in ALLOWED_HOSTS:
        raise SystemExit(f"refusing to fetch from {host}")
    req = urllib.request.Request(url, headers={"User-Agent": "torfox-update-adblock"})
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


def kept(rule: str) -> bool:
    """Whether |rule| is worth giving to TorFox's engine."""
    if not rule or rule.startswith(("!", "[")):
        return False
    if any(sep in rule for sep in COSMETIC_UNSUPPORTED):
        return False
    cosmetic = re.search(r"#@?#", rule)
    if cosmetic:
        return not PROCEDURAL.search(rule[cosmetic.end():])
    body = rule[2:] if rule.startswith("@@") else rule
    if body.startswith("/") and re.search(r"/(\$[^/]*)?$", body) and len(body) > 2:
        return False  # A regular expression.
    if "$" in body:
        options = body.rsplit("$", 1)[1].split(",")
        if any(o.split("=")[0].lstrip("~") in REWRITING for o in options):
            return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bump", action="store_true", help="pin the newest commit")
    args = ap.parse_args()

    lock = json.loads(LOCK.read_text(encoding="utf-8")) if LOCK.exists() else {}
    if args.bump or not lock:
        # The same commit as the tracker list, unless bumping.
        trackers = json.loads((REPO / "third_party" / "easyprivacy" / "sources.lock.json")
                              .read_text(encoding="utf-8"))
        commit = latest_commit() if args.bump else trackers["commit"]
        hashes: dict[str, str] = {}
    else:
        commit, hashes = lock["commit"], lock["files"]

    def get(name: str) -> bytes:
        data = fetch(f"https://raw.githubusercontent.com/{OWNER_REPO}/{commit}/{name}")
        digest = hashlib.sha256(data).hexdigest()
        if name in hashes and hashes[name] != digest:
            raise SystemExit(f"{name}: SHA-256 {digest} does not match the lock ({hashes[name]})")
        new_hashes[name] = digest
        return data

    new_hashes: dict[str, str] = {}
    template = get(TEMPLATE).decode("utf-8")
    names = [m.group(1) for m in (INCLUDE.match(l.strip()) for l in template.splitlines()) if m]
    if not names:
        raise SystemExit("the template includes no files")
    rules: set[str] = set()
    total = 0
    for name in names:
        for line in get(name).decode("utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith(("!", "[")):
                total += 1
            if kept(line):
                rules.add(line)
    if hashes and set(hashes) != set(new_hashes):
        raise SystemExit("the template's files differ from the lock; use --bump")
    if len(rules) < 1000:
        raise SystemExit("too few rules found; refusing to write the list")

    DEST.mkdir(parents=True, exist_ok=True)
    text = "".join(f"{r}\n" for r in sorted(rules)).encode("utf-8")
    (DEST / "filters.txt").write_bytes(text)
    LOCK.write_text(json.dumps({"source": f"https://github.com/{OWNER_REPO}", "commit": commit,
                                "files": new_hashes,
                                "filters_sha256": hashlib.sha256(text).hexdigest(),
                                "filters": len(rules)}, indent=2) + "\n",
                    encoding="utf-8", newline="\n")
    print(f"{len(rules)} of {total} rules from {OWNER_REPO}@{commit[:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
