#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Run one Stage 0 spike end to end and print its evidence as JSON.

  python spikes/cefspike/run_spikes.py <a|b|c|d> --exe out/spike/spikes/cefspike/Debug/cefspike.exe
"""
from __future__ import annotations

import argparse
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["a", "b", "c", "d"])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--keep", action="store_true", help="keep the temp profile dir")
    ap.add_argument("--extra", action="append", default=[], help="extra Chromium switch (repeatable)")
    args = ap.parse_args()

    work = Path(tempfile.mkdtemp(prefix=f"torfox-spike-{args.mode}-"))
    http_port, socks_port, dead_port = free_port(), free_port(), free_port()
    fixture_log = work / "fixtures.jsonl"
    fixtures = subprocess.Popen(
        [sys.executable, "-I", str(HERE / "fixtures.py"), "--http-port", str(http_port),
         "--socks-port", str(socks_port), "--log", str(fixture_log)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert fixtures.stdout.readline().strip() == "READY"

    out = work / "results.json"
    cmd = [str(Path(args.exe).resolve()), f"--spike={args.mode}", f"--spike-out={out}",
           f"--spike-dir={work}", f"--http-port={http_port}", f"--socks-port={socks_port}",
           f"--dead-port={dead_port}", "--spike-hold=10", *args.extra]
    proc = subprocess.Popen(cmd)
    sandbox = None
    try:
        if args.mode == "a":
            time.sleep(7)
            sandbox = json.loads(subprocess.run(
                [sys.executable, "-I", str(REPO / "tests/platform/win_process_inspect.py"), str(proc.pid)],
                capture_output=True, text=True, check=True).stdout)
        rc = proc.wait(timeout=120)
    except subprocess.TimeoutExpired:
        proc.kill()
        rc = "killed-timeout"
    finally:
        fixtures.stdin.close()
        fixtures.wait(timeout=10)

    results = json.loads(out.read_text()) if out.exists() else {"error": "no results file"}
    log = [json.loads(line) for line in fixture_log.read_text().splitlines() if line.strip()]
    report = {"exit_code": rc, "results": results, "fixture_log": log}
    if sandbox is not None:
        report["sandbox"] = sandbox
    cef_log = work / "cef.log"
    if cef_log.exists():
        report["cef_log_tail"] = cef_log.read_text(errors="replace").splitlines()[-25:]
    print(json.dumps(report, indent=2))
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
