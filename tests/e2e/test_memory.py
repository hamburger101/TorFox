# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Settings > Memory: what TorFox's processes use, by tab.

The report (memory.query, Settings only) is checked against Windows' own
view of the processes: every process it counts is TorFox or one of its
descendants, tab rows are renderer processes, and the browser figure is
plausible next to the process's committed private memory. Private and Tor
tabs are never named. Tor's own processes: test_tor.TorProcessTest."""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

from harness import Fixture, TorFox, bridge_call, shell_state, wait_for, wait_state


class _Counters(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t), ("PrivateUsage", ctypes.c_size_t)]


def committed_private(pid: int) -> int:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32.OpenProcess.restype = wt.HANDLE
    handle = kernel32.OpenProcess(0x1000 | 0x0010, False, pid)
    if not handle:
        raise OSError(ctypes.get_last_error(), "OpenProcess")
    try:
        c = _Counters()
        c.cb = ctypes.sizeof(c)
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb):
            raise OSError(ctypes.get_last_error(), "GetProcessMemoryInfo")
        return c.PrivateUsage
    finally:
        kernel32.CloseHandle(handle)


def processes() -> dict[int, dict]:
    """Windows' list of processes: pid -> {parent, command line}."""
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,CommandLine"
         " | ConvertTo-Json -Compress"], capture_output=True, text=True, timeout=60, check=True).stdout
    return {p["ProcessId"]: {"parent": p["ParentProcessId"], "cmd": p["CommandLine"] or ""}
            for p in json.loads(out)}


def descendants(root: int, table: dict[int, dict]) -> set[int]:
    found, frontier = {root}, [root]
    while frontier:
        parent = frontier.pop()
        for pid, p in table.items():
            if p["parent"] == parent and pid not in found:
                found.add(pid)
                frontier.append(pid)
    return found


SECTION = ("[...document.querySelector('tf-settings').shadowRoot.querySelectorAll('.nav-item')]"
           ".find(b => b.textContent.trim() === 'Memory').click()")
SHOWN = """(() => {
  const r = document.querySelector('tf-settings').shadowRoot;
  const total = r.querySelector('#memory-total-value');
  if (!total) return null;
  return {total: total.textContent,
          tabs: [...r.querySelectorAll('#memory-tabs .tab-name')].map(e => e.textContent),
          rows: [...r.querySelectorAll('#memory-torfox .memory-row')].map(e => e.dataset.kind)};
})()"""


@unittest.skipUnless(sys.platform == "win32", "Windows process checks")
class MemoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture().__enter__()
        cls.urls = [cls.fixture.html(f"/m{i}", f"<!doctype html><title>Memory page {i}</title><p>{i}")
                    for i in range(3)]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.fixture.__exit__(None, None, None)

    def setUp(self) -> None:
        self.user_data = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-memory-"))
        self.addCleanup(shutil.rmtree, self.user_data, True)
        self.tf = TorFox(user_data=self.user_data).start()
        self.addCleanup(self.tf.cleanup)
        self.shell = self.tf.shell()

    def open(self, shell, url: str, new_tab: bool = True) -> None:
        if new_tab:
            self.assertTrue(bridge_call(shell, "tab.new")["ok"])
            wait_state(shell, lambda s: s["nav"].get("url", "").startswith("torfox://newtab"), "new tab")
        self.assertTrue(bridge_call(shell, "nav.go", {"input": url})["ok"])
        wait_state(shell, lambda s: s["nav"].get("url") == url and not any(t["loading"] for t in s["tabs"]),
                   f"{url} loaded")

    def settings(self):
        self.assertTrue(bridge_call(self.shell, "app.openSettings")["ok"])
        return self.tf.page_at("torfox://settings/")

    def report(self, page) -> dict:
        r = bridge_call(page, "memory.query")
        self.assertTrue(r["ok"], r)
        return json.loads(r["response"])

    def test_the_report_matches_torfoxs_processes(self) -> None:
        self.open(self.shell, self.urls[0], new_tab=False)
        self.open(self.shell, self.urls[1])
        page = self.settings()

        def with_tabs():
            rep = self.report(page)
            titles = {t["title"] for row in rep["tabs"] for t in row["tabs"]}
            return rep if {"Memory page 0", "Memory page 1", "Settings"} <= titles else None
        rep = wait_for(with_tabs, 15, "both pages and Settings in the report")
        self.assertTrue(rep["supported"])
        table = processes()
        ours = descendants(self.tf.proc.pid, table)
        counted = set()
        for group in [*rep["tabs"], rep["interface"], rep["helpers"], rep["tor"]]:
            self.assertEqual(len(group["pids"]), group["processes"], group)
            counted |= set(group["pids"])
        # Everything counted is TorFox's; nothing is counted twice.
        self.assertTrue(counted <= ours, (counted - ours, ours))
        all_pids = [p for g in [*rep["tabs"], rep["interface"], rep["helpers"], rep["tor"]] for p in g["pids"]]
        self.assertEqual(len(all_pids), len(set(all_pids)))
        self.assertNotIn(self.tf.proc.pid, counted)
        # Tab rows are renderers.
        for row in rep["tabs"]:
            for pid in row["pids"]:
                self.assertIn("--type=renderer", table[pid]["cmd"], (row, table[pid]))
        # The GPU and network processes are helpers.
        kinds = {table[p]["cmd"].split("--type=")[1].split()[0] for p in rep["helpers"]["pids"]
                 if "--type=" in table[p]["cmd"]}
        self.assertIn("gpu-process", kinds)
        self.assertIn("utility", kinds)
        self.assertEqual(rep["tor"]["processes"], 0)  # No Tor window yet.
        # Every child TorFox has is counted (none is left out).
        children = {p for p in ours if p != self.tf.proc.pid and "--type=" in table[p]["cmd"]}
        self.assertEqual(children - counted, set())
        # Figures.
        total = rep["browser"] + rep["interface"]["bytes"] + rep["helpers"]["bytes"] + \
            rep["tor"]["bytes"] + sum(r["bytes"] for r in rep["tabs"])
        self.assertEqual(rep["total"], total)
        self.assertEqual(rep["processes"], len(counted) + 1)
        private = committed_private(self.tf.proc.pid)
        self.assertGreater(rep["browser"], 5 * 1024 * 1024)
        # A private working set is part of the committed private memory.
        self.assertLess(rep["browser"], private * 1.25 + 32 * 1024 * 1024, (rep["browser"], private))
        # Largest first.
        self.assertEqual([r["bytes"] for r in rep["tabs"]], sorted((r["bytes"] for r in rep["tabs"]), reverse=True))

    def test_private_tabs_are_not_named(self) -> None:
        known = self.tf.shell_ids()
        self.assertTrue(bridge_call(self.shell, "window.newPrivate")["ok"])
        wait_for(lambda: self.tf.events("window_created", private=True), 15, "private window")
        private = self.tf.new_shell(known)
        wait_state(private, lambda s: s["private"], "private shell")
        self.open(private, self.urls[2], new_tab=False)
        page = self.settings()

        def with_private():
            rep = self.report(page)
            return rep if any(t["window"] == "private" for row in rep["tabs"] for t in row["tabs"]) else None
        rep = wait_for(with_private, 15, "the private tab in the report")
        text = json.dumps(rep)
        self.assertNotIn("Memory page 2", text)
        self.assertNotIn(self.urls[2], text)
        self.assertNotIn(str(self.fixture.port), text)
        for row in rep["tabs"]:
            for t in row["tabs"]:
                if t["window"] == "private":
                    self.assertEqual(t["title"], "")
        # Shown as "Private tab".
        page.evaluate(SECTION)
        shown = wait_for(lambda: (lambda s: s if s and "Private tab" in s["tabs"] else None)(page.evaluate(SHOWN)),
                         10, "the Memory section")
        self.assertNotIn("Memory page 2", json.dumps(shown))

    def test_the_section_shows_the_report_and_keeps_it_current(self) -> None:
        self.open(self.shell, self.urls[0], new_tab=False)
        page = self.settings()
        page.evaluate(SECTION)
        shown = wait_for(lambda: (lambda s: s if s and "Memory page 0" in s["tabs"] else None)(page.evaluate(SHOWN)),
                         10, "the Memory section")
        self.assertRegex(shown["total"], r"^\d+(\.\d+)? (MB|GB)$")
        self.assertEqual(shown["rows"][:3], ["browser", "interface", "helpers"])
        self.assertNotIn("tor", shown["rows"])
        settings_id = shell_state(self.shell)["activeId"]
        # A tab opened meanwhile appears without reloading the page. While
        # Settings is hidden behind it, nothing is measured.
        self.open(self.shell, self.urls[1])
        self.assertTrue(page.evaluate("document.hidden"))
        time.sleep(2.5)  # Longer than a poll.
        self.assertNotIn("Memory page 1", page.evaluate(SHOWN)["tabs"])
        self.assertTrue(bridge_call(self.shell, "tab.select", {"id": settings_id})["ok"])
        wait_for(lambda: (lambda s: s if s and "Memory page 1" in s["tabs"] else None)(page.evaluate(SHOWN)),
                 1.5, "the new tab in the section, as soon as Settings is shown")

    def test_only_settings_can_ask(self) -> None:
        r = bridge_call(self.shell, "memory.query")
        self.assertFalse(r["ok"], r)
        self.assertEqual([e["reason"] for e in self.tf.events("bridge_reject")], ["unknown_command"])


if __name__ == "__main__":
    unittest.main()
