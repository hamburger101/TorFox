# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Release-build checks on Windows: every process runs sandboxed, and no
command-line switch can weaken TorFox.

These run the release build (no test hooks: no DevTools, no test log) the
way a user would, so they use the real per-user data folder,
%LOCALAPPDATA%\\TorFox. If that folder did not exist before a test, the test
removes it afterwards; an existing folder (a real profile) is left alone.
They are skipped while another TorFox is running, because a second launch
only opens a window in the running one.

Usage:  python -m unittest discover -v -s tests/platform -p "test_*_win.py"
Needs:  the release build in out/dev, or TORFOX_RELEASE_EXE.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import socket
import subprocess
import sys
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "e2e"))
sys.path.insert(0, str(pathlib.Path(__file__).parent))

if sys.platform == "win32":
    import win_process_inspect as wpi
    from harness import WM_CLOSE, Fixture, _top_level_windows, _user32, wait_for

DEFAULT_EXE = REPO / "out" / "dev" / "src" / "browser" / "Release" / "TorFox.exe"
DEBUG_PORT = 9333


def release_exe() -> pathlib.Path:
    return pathlib.Path(os.environ.get("TORFOX_RELEASE_EXE", DEFAULT_EXE))


def user_data_dir() -> pathlib.Path:
    return pathlib.Path(os.environ["LOCALAPPDATA"]) / "TorFox"


def torfox_running() -> bool:
    return any((p.get("Name") or "").lower() == "torfox.exe" for p in wpi.all_processes())


def port_open(port: int) -> bool:
    with socket.socket() as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


class ReleaseTorFox:
    """One release-build TorFox process, started with |args|."""

    def __init__(self, args: list[str]) -> None:
        self.args = [str(release_exe()), *args]
        self.proc: subprocess.Popen | None = None
        self.created_dir = not user_data_dir().exists()

    def __enter__(self) -> "ReleaseTorFox":
        self.proc = subprocess.Popen(self.args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return self

    def close(self, timeout: float = 20.0) -> int:
        assert self.proc
        for hwnd in _top_level_windows(self.proc.pid):
            _user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        self.proc.wait(timeout)
        return self.proc.returncode

    def __exit__(self, *exc) -> None:
        if self.proc and self.proc.poll() is None:
            subprocess.run(["taskkill", "/PID", str(self.proc.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.proc.wait(10)
        if self.created_dir:
            for _ in range(50):  # Child processes may hold files briefly.
                try:
                    shutil.rmtree(user_data_dir())
                    break
                except FileNotFoundError:
                    break
                except OSError:
                    time.sleep(0.1)


@unittest.skipUnless(sys.platform == "win32", "Windows only")
class ReleaseBuildTest(unittest.TestCase):
    def setUp(self) -> None:
        if not release_exe().is_file():
            self.skipTest(f"release build not found at {release_exe()}")
        if torfox_running():
            self.skipTest("another TorFox is running; a second launch would join it")
        self.fixture = Fixture().__enter__()
        self.addCleanup(self.fixture.__exit__, None, None, None)
        self.page_url = self.fixture.html("/page", "<!doctype html><title>page</title>")

    def processes(self, tf: ReleaseTorFox) -> list[dict]:
        """The process tree once a web page has loaded in a renderer."""
        wait_for(lambda: "GET /page" in self.fixture.requests, 30, "the start page request")
        def ready():
            tree = wpi.inspect_tree(tf.proc.pid)
            types = {p["type"] for p in tree}
            return tree if {"renderer", "gpu-process", "utility"} <= types else None
        return wait_for(ready, 30, "renderer, GPU and utility processes", interval=0.5)

    def assert_sandboxed(self, tree: list[dict]) -> None:
        renderers = [p for p in tree if p["type"] == "renderer"]
        self.assertTrue(renderers)
        for p in renderers:
            self.assertEqual(p.get("integrity"), "untrusted", p)
            self.assertTrue(p.get("restricted"), p)
        network = [p for p in tree if p.get("utility_sub_type") == "network.mojom.NetworkService"]
        self.assertEqual(len(network), 1, tree)
        self.assertTrue(network[0].get("app_container"), network[0])
        for p in tree:
            if p["type"] == "gpu-process":
                self.assertIn(p.get("integrity"), ("low", "untrusted"), p)
        commands = [p.get("CommandLine") or "" for p in wpi.all_processes()
                    if p["ProcessId"] in {q["pid"] for q in tree}]
        self.assertFalse([c for c in commands if "--no-sandbox" in c or "--single-process" in c])

    def test_every_process_type_is_sandboxed(self) -> None:
        with ReleaseTorFox([self.page_url]) as tf:
            tree = self.processes(tf)
            self.assert_sandboxed(tree)
            self.assertEqual(tf.close(), 0)

    def test_command_line_cannot_weaken_torfox(self) -> None:
        hostile = [
            "--no-sandbox", "--disable-gpu-sandbox", "--single-process", "--in-process-gpu",
            f"--remote-debugging-port={DEBUG_PORT}", "--remote-allow-origins=*",
            "--proxy-server=http://127.0.0.1:9", "--host-resolver-rules=MAP * 127.0.0.1:9",
            "--disable-features=NetworkServiceSandbox", "--disable-site-isolation-trials",
            "--disable-web-security", "--load-extension=C:\\nonexistent",
            "--component-updater=url-source=https://evil.test/",
            "--user-data-dir=C:\\nonexistent-torfox", "--enable-logging", "--log-net-log=x.json",
        ]
        decoys = [pathlib.Path("C:\\nonexistent-torfox"), pathlib.Path("x.json").resolve()]
        self.assertFalse([d for d in decoys if d.exists()], "decoy paths exist before the test")
        self.addCleanup(lambda: [shutil.rmtree(d, ignore_errors=True) if d.is_dir() else d.unlink(missing_ok=True)
                                 for d in decoys])
        self.assertFalse(port_open(DEBUG_PORT), f"port {DEBUG_PORT} is already in use")
        with ReleaseTorFox([*hostile, self.page_url]) as tf:
            # The page loaded directly: the proxy and resolver rules were ignored.
            tree = self.processes(tf)
            self.assert_sandboxed(tree)
            self.assertFalse(port_open(DEBUG_PORT), "the DevTools port was opened")
            self.assertFalse((user_data_dir() / "User Data" / "DevToolsActivePort").exists())
            self.assertFalse([d for d in decoys if d.exists()], "a switch wrote outside the profile")
            self.assertEqual(tf.close(), 0)


if __name__ == "__main__":
    unittest.main()
