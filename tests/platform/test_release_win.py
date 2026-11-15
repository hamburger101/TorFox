# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Release-build checks on Windows: every process runs sandboxed, and no
command-line switch can weaken TorFox.

These run the release build (no test hooks: no DevTools, no test log) the
way a user would, so they use the per-user data folder,
%LOCALAPPDATA%\\TorFox. The user's own folder is set aside while they run and
put back afterwards, checked byte for byte (real_profile.py); a test removes
the data it created.
They are skipped while another TorFox is running, because a second launch
only opens a window in the running one.

Usage:  python -m unittest discover -v -s tests/platform -p "test_*_win.py"
Needs:  the release build in out/dev, or TORFOX_RELEASE_EXE.
"""

from __future__ import annotations

import json
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

    import real_profile


def setUpModule() -> None:
    # The user's own data folder is set aside while these tests run.
    if sys.platform == "win32":
        real_profile.set_aside()


def tearDownModule() -> None:
    if sys.platform == "win32":
        real_profile.restore()


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
        net_log = pathlib.Path(os.environ.get("TEMP", str(REPO / "out"))) / "torfox-decoy-netlog.json"
        hostile = [
            "--no-sandbox", "--disable-gpu-sandbox", "--single-process", "--in-process-gpu",
            f"--remote-debugging-port={DEBUG_PORT}", "--remote-allow-origins=*",
            "--proxy-server=http://127.0.0.1:9", "--host-resolver-rules=MAP * 127.0.0.1:9",
            "--disable-features=NetworkServiceSandbox", "--disable-site-isolation-trials",
            "--disable-web-security", "--load-extension=C:\\nonexistent",
            "--component-updater=url-source=https://evil.test/",
            "--user-data-dir=C:\\nonexistent-torfox", "--enable-logging",
            # Absolute: a relative NetLog path can end up in TorFox's own
            # folder rather than the test's (one once appeared next to
            # TorFox.exe), where the decoy check would not look.
            f"--log-net-log={net_log}",
        ]
        decoys = [pathlib.Path("C:\\nonexistent-torfox"), net_log]
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

    def test_release_builds_write_no_logs(self) -> None:
        # Chromium's and CEF's logs could record the pages a user opens. A
        # release build writes none, even when asked to on the command line,
        # and TorFox's own test log does not exist in it.
        names = {"debug.log", "cef.log", "chrome_debug.log", "test-log.jsonl"}
        install = release_exe().parent
        before = {p.relative_to(install) for p in install.rglob("*")}
        page = self.fixture.html("/no-log-marker-7f3a", "<!doctype html><title>log probe</title>"
                                 "<script>console.error('no-log-marker-7f3a')</script>")
        with ReleaseTorFox(["--enable-logging", "--v=2", "--log-level=0", page]) as tf:
            wait_for(lambda: "GET /no-log-marker-7f3a" in self.fixture.requests, 30, "the page")
            time.sleep(3)
            self.assertEqual(tf.close(), 0)
            found = [p for p in user_data_dir().rglob("*")
                     if p.is_file() and (p.name.lower() in names or p.suffix.lower() == ".jsonl")]
            self.assertEqual(found, [])
        after = {p.relative_to(install) for p in install.rglob("*")}
        new = sorted(map(str, after - before))
        # A known exception (docs/STAGE11_FINDINGS.md §9): TorFox.exe is CEF's
        # bootstrap, which reads --enable-logging itself, before any TorFox
        # code runs, and writes its own start-up lines to debug.log next to
        # it. They name no page: every line must be the bootstrap's.
        bootstrap_log = install / "debug.log"
        if bootstrap_log.exists():
            text = bootstrap_log.read_text(encoding="utf-8", errors="replace")
            self.assertNotIn("no-log-marker-7f3a", text)
            self.assertNotIn(self.fixture.origin.split("//", 1)[1], text)
            for line in text.splitlines():
                if line.strip():
                    self.assertIn("libcef_dll\\bootstrap\\", line, "a log line not from CEF's bootstrap")
            new = [n for n in new if n != "debug.log"]
        self.assertEqual(new, [], "files appeared next to TorFox.exe")

    def test_release_builds_ignore_the_test_update_source(self) -> None:
        # Test builds can be pointed at a fake GitHub with their own key
        # (tests/e2e/test_updates.py). In a release build that switch must do
        # nothing: with it honoured, this config would make TorFox ask the
        # fixture for a release half a second after start.
        config = REPO / "out" / "dev" / "test-update-config.json"
        config.write_text(json.dumps({
            "origin": self.fixture.origin, "repository": "test/TorFox", "firstCheckDelayMs": 500,
            "publicKey": "RWTdwXdA4w5c2DzsSnRKBJHsiyiWz5kbFwLS3nGYo+4HVGYNmS3/E90J"}), encoding="utf-8")
        self.addCleanup(config.unlink, True)
        with ReleaseTorFox([f"--torfox-test-update-config={config}", self.page_url]) as tf:
            wait_for(lambda: "GET /page" in self.fixture.requests, 30, "the start page request")
            time.sleep(8)
            self.assertEqual([r for r in self.fixture.requests if "/repos/" in r or "/releases" in r], [])
            self.assertEqual(tf.close(), 0)


if __name__ == "__main__":
    unittest.main()
