#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Inspect the sandbox state of a Chromium/CEF process tree on Windows.

For every descendant of a root PID this reports the Chromium process type
(from --type=), the token integrity level, whether the token is restricted
and whether it is an AppContainer/LPAC token. Used by the sandbox tests.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import re
import subprocess
import sys

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TOKEN_QUERY = 0x0008
TokenIntegrityLevel = 25
TokenIsAppContainer = 29

INTEGRITY_NAMES = {
    0x0000: "untrusted",
    0x1000: "low",
    0x2000: "medium",
    0x2100: "medium_plus",
    0x3000: "high",
    0x4000: "system",
}


class SID_AND_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Sid", ctypes.c_void_p), ("Attributes", wt.DWORD)]


class TOKEN_MANDATORY_LABEL(ctypes.Structure):
    _fields_ = [("Label", SID_AND_ATTRIBUTES)]


advapi32.GetSidSubAuthorityCount.restype = ctypes.POINTER(ctypes.c_ubyte)
advapi32.GetSidSubAuthorityCount.argtypes = [ctypes.c_void_p]
advapi32.GetSidSubAuthority.restype = ctypes.POINTER(wt.DWORD)
advapi32.GetSidSubAuthority.argtypes = [ctypes.c_void_p, wt.DWORD]
advapi32.IsTokenRestricted.restype = wt.BOOL
advapi32.IsTokenRestricted.argtypes = [wt.HANDLE]


def token_info(pid: int) -> dict:
    out: dict = {}
    hproc = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not hproc:
        return {"error": f"OpenProcess failed: {ctypes.get_last_error()}"}
    try:
        htok = wt.HANDLE()
        if not advapi32.OpenProcessToken(hproc, TOKEN_QUERY, ctypes.byref(htok)):
            return {"error": f"OpenProcessToken failed: {ctypes.get_last_error()}"}
        try:
            size = wt.DWORD(0)
            advapi32.GetTokenInformation(htok, TokenIntegrityLevel, None, 0, ctypes.byref(size))
            buf = ctypes.create_string_buffer(size.value)
            if advapi32.GetTokenInformation(htok, TokenIntegrityLevel, buf, size, ctypes.byref(size)):
                label = ctypes.cast(buf, ctypes.POINTER(TOKEN_MANDATORY_LABEL)).contents
                count = advapi32.GetSidSubAuthorityCount(label.Label.Sid).contents.value
                rid = advapi32.GetSidSubAuthority(label.Label.Sid, count - 1).contents.value
                out["integrity_rid"] = rid
                out["integrity"] = INTEGRITY_NAMES.get(rid, hex(rid))
            is_ac = wt.DWORD(0)
            if advapi32.GetTokenInformation(htok, TokenIsAppContainer, ctypes.byref(is_ac),
                                            ctypes.sizeof(is_ac), ctypes.byref(size)):
                out["app_container"] = bool(is_ac.value)
            out["restricted"] = bool(advapi32.IsTokenRestricted(htok))
        finally:
            kernel32.CloseHandle(htok)
    finally:
        kernel32.CloseHandle(hproc)
    return out


def all_processes() -> list[dict]:
    ps = ("Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,Name,CommandLine"
          " | ConvertTo-Json -Compress")
    raw = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True,
                         text=True, check=True).stdout
    data = json.loads(raw)
    return data if isinstance(data, list) else [data]


def inspect_tree(root_pid: int) -> list[dict]:
    procs = all_processes()
    children: dict[int, list[dict]] = {}
    for p in procs:
        children.setdefault(p["ParentProcessId"], []).append(p)
    result = []
    stack = [root_pid]
    seen = set()
    while stack:
        pid = stack.pop()
        if pid in seen:
            continue
        seen.add(pid)
        for c in children.get(pid, []):
            cmd = c.get("CommandLine") or ""
            m = re.search(r"--type=([\w-]+)", cmd)
            utility = re.search(r"--utility-sub-type=([\w.]+)", cmd)
            entry = {"pid": c["ProcessId"], "name": c["Name"],
                     "type": m.group(1) if m else "browser-child",
                     "utility_sub_type": utility.group(1) if utility else None}
            entry.update(token_info(c["ProcessId"]))
            result.append(entry)
            stack.append(c["ProcessId"])
    return result


def main() -> int:
    root = int(sys.argv[1])
    report = {"root": {"pid": root, **token_info(root)}, "children": inspect_tree(root)}
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
