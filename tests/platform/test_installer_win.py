# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""The Windows installer (packaging/windows/TorFox.iss), installed for real.

TorFox's setup installs for the current user only, without administrator
rights. PerUserInstallerTest uses the test build of the setup
(scripts/package_windows.py --test-peruser): the same script under another
name and AppId, installed into a temporary folder, so it never touches a real
installation. ReleaseSetupInstallerTest uses the real setup, installed where
it installs by default (%LOCALAPPDATA%\\Programs\\TorFox); it is skipped when
TorFox is already installed there for this user.

Both check: that the setup runs without administrator rights; what is
installed (and what is not), the shortcut and the uninstall entry; that the
installed TorFox runs sandboxed; that an update
(/TORFOXUPDATE) waits for TorFox to close, installs and starts TorFox again;
that an update after TorFox closed (/TORFOXNORELAUNCH) does not start it, and a
TorFox opened while an update installs exits at once and is started afterwards;
that Setup and Uninstall refuse to run while TorFox is open; and that
uninstalling removes the program but keeps the user's data.

The installed TorFox is a release build, so it uses the real per-user data
folder (%LOCALAPPDATA%\\TorFox). As in test_release_win.py, a folder the test
created is removed afterwards and an existing one is left alone, and the
tests are skipped while another TorFox is running.

Needs the setups made by scripts/package_windows.py, in out/dist:
  TorFox-<v>-win-x64-setup[-peruser-test].exe and, for the update test, the
  same for version <v> with its patch number plus one (--version).
  TORFOX_SETUP_DIR overrides out/dist.
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "e2e"))
sys.path.insert(0, str(pathlib.Path(__file__).parent))

if sys.platform == "win32":
    import winreg

    import win_process_inspect as wpi
    from harness import WM_CLOSE, Fixture, _top_level_windows, _user32, wait_for

SETUP_DIR = pathlib.Path(os.environ.get("TORFOX_SETUP_DIR", REPO / "out" / "dist"))
UPDATE_ARGS = ["/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/TORFOXUPDATE"]
# TorFox closed with an update waiting (TorFox's updater, platform_win.cc LaunchUpdate).
INSTALL_ON_EXIT_ARGS = ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/TORFOXUPDATE", "/TORFOXNORELAUNCH"]
QUIET_ARGS = ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"]
SHIPPED = ["TorFox.exe", "TorFox.dll", "libcef.dll", "chrome_elf.dll", "icudtl.dat", "resources.pak",
           "v8_context_snapshot.bin", "locales/en-US.pak", "tor/tor.exe",
           "tor/pluggable_transports/lyrebird.exe", "tor/pluggable_transports/pt_config.json",
           "THIRD_PARTY_NOTICES.txt", "licenses/TorFox-MPL-2.0.txt", "licenses/CEF-LICENSE.txt",
           "licenses/Chromium-CREDITS.html", "licenses/libsodium-ISC.txt",
           "licenses/lit-BSD-3-Clause.txt", "licenses/nlohmann-json-MIT.txt"]


def project_version() -> str:
    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    return re.search(r"project\(TorFox VERSION ([0-9]+\.[0-9]+\.[0-9]+)", text).group(1)


def next_version(v: str) -> str:
    major, minor, patch = (int(x) for x in v.split("."))
    return f"{major}.{minor}.{patch + 1}"


def user_data_dir() -> pathlib.Path:
    return pathlib.Path(os.environ["LOCALAPPDATA"]) / "TorFox"


def torfox_running() -> bool:
    return any((p.get("Name") or "").lower() == "torfox.exe" for p in wpi.all_processes())


def processes_in(folder: pathlib.Path) -> list[dict]:
    """Processes whose executable is inside |folder|."""
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath } | "
          "Select-Object ProcessId,ParentProcessId,Name,ExecutablePath | ConvertTo-Json -Compress")
    raw = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True,
                         text=True, check=True).stdout
    data = json.loads(raw) if raw.strip() else []
    data = data if isinstance(data, list) else [data]
    prefix = str(folder.resolve()).lower().rstrip("\\") + "\\"
    return [p for p in data if (p.get("ExecutablePath") or "").lower().startswith(prefix)]


class Variant:
    def __init__(self, *, test_build: bool) -> None:
        self.test_build = test_build
        suffix = "-peruser-test" if test_build else ""
        self.version = project_version()
        self.update_version = next_version(self.version)
        self.setup = SETUP_DIR / f"TorFox-{self.version}-win-x64-setup{suffix}.exe"
        self.update_setup = SETUP_DIR / f"TorFox-{self.update_version}-win-x64-setup{suffix}.exe"
        if test_build:
            self.name = "TorFox Installer Test"
            self.app_id = "{a843f419-af80-4bad-b40a-b971e3ea64b1}_is1"
        else:
            self.name = "TorFox"
            self.app_id = "{96a198f9-9688-44af-aa68-192fb7cadebc}_is1"
        # Both install for the current user only.
        self.root = winreg.HKEY_CURRENT_USER
        self.start_menu = pathlib.Path(os.environ["APPDATA"]) / "Microsoft/Windows/Start Menu/Programs"

    def uninstall_entry(self) -> dict | None:
        path = rf"Software\Microsoft\Windows\CurrentVersion\Uninstall\{self.app_id}"
        try:
            with winreg.OpenKey(self.root, path, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
                values = {}
                i = 0
                while True:
                    try:
                        name, value, _ = winreg.EnumValue(k, i)
                    except OSError:
                        return values
                    values[name] = value
                    i += 1
        except FileNotFoundError:
            return None


class InstalledTorFox:
    """The installed TorFox.exe, started like a user would (release build)."""

    def __init__(self, exe: pathlib.Path, args: list[str]) -> None:
        self.exe = exe
        self.args = args
        self.proc: subprocess.Popen | None = None

    def __enter__(self) -> "InstalledTorFox":
        self.proc = subprocess.Popen([str(self.exe), *self.args], stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        wait_for(lambda: _top_level_windows(self.proc.pid), 30, "a TorFox window")
        return self

    def close(self, timeout: float = 30.0) -> int:
        for hwnd in _top_level_windows(self.proc.pid):
            _user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        self.proc.wait(timeout)
        return self.proc.returncode

    def __exit__(self, *exc) -> None:
        if self.proc and self.proc.poll() is None:
            subprocess.run(["taskkill", "/PID", str(self.proc.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.proc.wait(10)


def close_pid(pid: int, timeout: float = 30.0) -> None:
    # A TorFox the setup has just started may not have its window yet: ask
    # again until it has gone.
    def closed() -> bool:
        if not [p for p in wpi.all_processes() if p["ProcessId"] == pid]:
            return True
        for hwnd in _top_level_windows(pid):
            _user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        return False
    wait_for(closed, timeout, f"TorFox ({pid}) to close", interval=0.5)


class InstallerTests:
    """The tests; each subclass names its variant in setUpClass."""

    variant: "Variant"
    app: pathlib.Path

    # ----- set-up --------------------------------------------------------------------

    def setUp(self) -> None:
        if sys.platform != "win32":
            self.skipTest("Windows only")
        v = self.variant
        if not v.setup.is_file():
            self.skipTest(f"{v.setup} not found (build it with scripts/package_windows.py)")
        if torfox_running():
            self.skipTest("another TorFox is running; the installed one would join it")
        if v.uninstall_entry() is not None:
            self.skipTest(f"{v.name} is already installed on this computer; it is left alone")
        self.created_user_data = not user_data_dir().exists()
        self.addCleanup(self.cleanup)
        self.fixture = Fixture().__enter__()
        self.addCleanup(self.fixture.__exit__, None, None, None)
        self.page_url = self.fixture.html("/page", "<!doctype html><title>page</title>")

    def cleanup(self) -> None:
        for p in processes_in(self.app) if self.app.exists() else []:
            subprocess.run(["taskkill", "/PID", str(p["ProcessId"]), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if self.variant.uninstall_entry() is not None:
            self.uninstall(check=False)
            try:
                wait_for(lambda: self.variant.uninstall_entry() is None, 60, "the uninstall")
            except TimeoutError:
                pass
        if self.variant.test_build or self.created_app:
            shutil.rmtree(self.app, ignore_errors=True)
            # A failed uninstall of the test build must not skip the next tests.
            if self.variant.uninstall_entry() is not None:
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER,
                                 rf"Software\Microsoft\Windows\CurrentVersion\Uninstall\{self.variant.app_id}")
            (self.variant.start_menu / f"{self.variant.name}.lnk").unlink(missing_ok=True)
        if self.created_user_data:
            for _ in range(50):
                try:
                    shutil.rmtree(user_data_dir())
                    break
                except FileNotFoundError:
                    break
                except OSError:
                    time.sleep(0.1)

    def run_setup(self, setup: pathlib.Path, args: list[str], timeout: float = 600) -> int:
        self.last_log = self.log_path(setup)
        cmd = [str(setup), *args, f"/LOG={self.last_log}"]
        if self.variant.test_build:
            cmd.append(f"/DIR={self.app}")
        return subprocess.run(cmd, timeout=timeout).returncode

    def last_log_text(self) -> str:
        return self.last_log.read_text(encoding="utf-8-sig", errors="replace")

    def log_path(self, setup: pathlib.Path) -> pathlib.Path:
        return pathlib.Path(tempfile.gettempdir()) / f"{setup.stem}-{int(time.time() * 1000)}.log"

    def install(self) -> None:
        self.assertEqual(self.run_setup(self.variant.setup, QUIET_ARGS), 0)
        # For this user only, never as administrator.
        log = self.last_log_text()
        self.assertIn("Administrative install mode: No", log)
        self.assertIn("Install mode root key: HKEY_CURRENT_USER", log)
        self.assertEqual(self.variant.uninstall_entry()["DisplayVersion"], self.variant.version)

    def uninstall(self, check: bool = True) -> int:
        uninstaller = self.app / "unins000.exe"
        code = subprocess.run([str(uninstaller), *QUIET_ARGS], timeout=300).returncode
        if check:
            # The uninstaller hands over to a copy of itself; wait for it.
            wait_for(lambda: self.variant.uninstall_entry() is None, 120, "the uninstall to finish")
        return code

    def start(self, args: list[str] | None = None) -> InstalledTorFox:
        return InstalledTorFox(self.app / "TorFox.exe", [*(args or []), self.page_url])

    # ----- tests ---------------------------------------------------------------------

    def test_install_contents_shortcut_and_uninstall(self) -> None:
        marker = user_data_dir() / "installer-test-keep.txt"
        if self.created_user_data:
            user_data_dir().mkdir(parents=True)
            marker.write_text("user data", encoding="utf-8")
        self.install()
        missing = [f for f in SHIPPED if not (self.app / f).is_file()]
        self.assertEqual(missing, [])
        build_only = [str(p.relative_to(self.app)) for p in self.app.rglob("*")
                      if p.suffix.lower() in (".pdb", ".lib", ".exp", ".ilk")]
        self.assertEqual(build_only, [])
        stray = [p.name for p in self.app.iterdir() if p.suffix.lower() == ".json"
                 and p.name != "vk_swiftshader_icd.json"]
        self.assertEqual(stray, [])
        shortcut = self.variant.start_menu / f"{self.variant.name}.lnk"
        self.assertTrue(shortcut.is_file(), shortcut)
        entry = self.variant.uninstall_entry()
        self.assertEqual(pathlib.Path(entry["InstallLocation"]).resolve(), self.app.resolve())

        self.uninstall()
        self.assertFalse((self.app / "TorFox.exe").exists())
        self.assertFalse((self.app / "tor").exists())
        self.assertFalse(shortcut.exists())
        if self.created_user_data:
            self.assertTrue(marker.is_file(), "uninstalling removed the user's data")

    def test_installed_torfox_runs_sandboxed(self) -> None:
        self.install()
        with self.start() as tf:
            wait_for(lambda: "GET /page" in self.fixture.requests, 60, "the page request")

            def ready():
                tree = wpi.inspect_tree(tf.proc.pid)
                return tree if any(p["type"] == "renderer" for p in tree) else None
            tree = wait_for(ready, 30, "a renderer", interval=0.5)
            for p in [p for p in tree if p["type"] == "renderer"]:
                self.assertEqual(p.get("integrity"), "untrusted", p)
            self.assertEqual(tf.close(), 0)

    def test_update_waits_for_torfox_then_installs_and_restarts_it(self) -> None:
        if not self.variant.update_setup.is_file():
            self.skipTest(f"{self.variant.update_setup} not found")
        self.install()
        # A file an older version shipped and this one may not.
        stale = self.app / "dxil.dll"
        stale.write_bytes(b"stale")
        with self.start() as tf:
            wait_for(lambda: "GET /page" in self.fixture.requests, 60, "the page request")
            log = self.log_path(self.variant.update_setup)
            setup = subprocess.Popen([str(self.variant.update_setup), *UPDATE_ARGS, f"/LOG={log}"])
            self.addCleanup(lambda: setup.poll() is None and setup.kill())

            def waiting():
                try:
                    return "Update: waiting for TorFox to exit" in log.read_text(encoding="utf-8",
                                                                                errors="replace")
                except FileNotFoundError:
                    return False
            wait_for(waiting, 120, "the update to wait for TorFox")
            time.sleep(5)
            self.assertIsNone(setup.poll(), "the update did not wait for TorFox to close")
            self.assertTrue((self.app / "TorFox.exe").is_file())
            self.assertEqual(self.variant.uninstall_entry()["DisplayVersion"], self.variant.version)
            self.assertEqual(tf.close(), 0)
        self.assertEqual(setup.wait(600), 0)
        self.assertEqual(self.variant.uninstall_entry()["DisplayVersion"], self.variant.update_version)
        # Gone, or replaced by the copy this version ships.
        self.assertFalse(stale.exists() and stale.read_bytes() == b"stale", "a stale file was kept")
        # TorFox is started again, from the program folder.
        browsers = wait_for(lambda: [p for p in processes_in(self.app) if p["Name"] == "TorFox.exe"
                                     and p["ParentProcessId"] not in
                                     {q["ProcessId"] for q in processes_in(self.app)}],
                            60, "TorFox to start again")
        for p in browsers:
            close_pid(p["ProcessId"])

    def torfox_browsers(self) -> list[dict]:
        """TorFox's main processes started from the program folder."""
        inside = processes_in(self.app)
        ids = {q["ProcessId"] for q in inside}
        return [p for p in inside if p["Name"] == "TorFox.exe" and p["ParentProcessId"] not in ids]

    def start_request(self):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\TorFox") as k:
                return winreg.QueryValueEx(k, "RelaunchAfterUpdate")[0]
        except FileNotFoundError:
            return None

    def test_an_update_after_torfox_closed_installs_without_starting_it(self) -> None:
        # TorFox was closed with an update waiting: it installs, and TorFox
        # stays closed.
        if not self.variant.update_setup.is_file():
            self.skipTest(f"{self.variant.update_setup} not found")
        self.install()
        self.assertEqual(self.run_setup(self.variant.update_setup, INSTALL_ON_EXIT_ARGS), 0)
        self.assertEqual(self.variant.uninstall_entry()["DisplayVersion"], self.variant.update_version)
        self.assertIn("Update: holding TorFoxUpdateInstalling", self.last_log_text())
        time.sleep(10)
        self.assertEqual(self.torfox_browsers(), [])

    def test_torfox_opened_during_an_update_starts_once_it_has_finished(self) -> None:
        # The user opens TorFox while the update installs: that TorFox must
        # not run from the files being replaced. It exits at once and asks
        # the setup to start TorFox when it is done.
        if not self.variant.update_setup.is_file():
            self.skipTest(f"{self.variant.update_setup} not found")
        self.install()
        self.addCleanup(lambda: self.start_request() is not None and winreg.DeleteValue(
            winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\TorFox", 0, winreg.KEY_SET_VALUE),
            "RelaunchAfterUpdate"))
        with self.start() as tf:
            wait_for(lambda: "GET /page" in self.fixture.requests, 60, "the page request")
            log = self.log_path(self.variant.update_setup)
            setup = subprocess.Popen([str(self.variant.update_setup), *INSTALL_ON_EXIT_ARGS, f"/LOG={log}"])
            self.addCleanup(lambda: setup.poll() is None and setup.kill())

            def waiting():
                try:
                    return "Update: waiting for TorFox to exit" in log.read_text(encoding="utf-8",
                                                                                errors="replace")
                except FileNotFoundError:
                    return False
            wait_for(waiting, 120, "the update to wait for TorFox")
            started = time.monotonic()
            second = subprocess.run([str(self.app / "TorFox.exe")], timeout=60)
            self.assertEqual(second.returncode, 0)
            self.assertLess(time.monotonic() - started, 20, "the second TorFox did not stop at once")
            self.assertEqual(self.start_request(), 1)
            # It opened no window in the running TorFox either.
            self.assertEqual(len(_top_level_windows(tf.proc.pid)), 1)
            self.assertEqual(tf.close(), 0)
        self.assertEqual(setup.wait(600), 0)
        self.assertEqual(self.variant.uninstall_entry()["DisplayVersion"], self.variant.update_version)
        browsers = wait_for(self.torfox_browsers, 60, "the setup to start TorFox")
        self.assertIsNone(self.start_request())
        for p in browsers:
            close_pid(p["ProcessId"])

    def test_update_gives_up_when_torfox_stays_open(self) -> None:
        if not self.variant.update_setup.is_file():
            self.skipTest(f"{self.variant.update_setup} not found")
        self.install()
        with self.start() as tf:
            wait_for(lambda: "GET /page" in self.fixture.requests, 60, "the page request")
            started = time.monotonic()
            code = self.run_setup(self.variant.update_setup, UPDATE_ARGS, timeout=300)
            self.assertNotEqual(code, 0)
            self.assertGreaterEqual(time.monotonic() - started, 55, "it did not wait")
            log = self.last_log_text()
            self.assertIn("PrepareToInstall failed: TorFox did not close", log)
            self.assertNotIn("Shutting down applications", log)
            self.assertEqual(self.variant.uninstall_entry()["DisplayVersion"], self.variant.version)
            self.assertEqual(tf.close(), 0)

    def test_setup_and_uninstall_refuse_while_torfox_is_open(self) -> None:
        self.install()
        with self.start() as tf:
            wait_for(lambda: "GET /page" in self.fixture.requests, 60, "the page request")
            self.assertNotEqual(self.run_setup(self.variant.setup, QUIET_ARGS, timeout=300), 0)
            # Refused by TorFox's own check, before anything was changed or
            # anything tried to close TorFox. (Inno's Restart Manager would
            # also stop Setup, but only after trying to shut TorFox down,
            # which could close the user's windows: STAGE8_FINDINGS.md §3.6.)
            log = self.last_log_text()
            self.assertIn("PrepareToInstall failed: TorFox is running", log)
            self.assertNotIn("Shutting down applications", log)
            self.assertNotIn("Starting the installation process", log)
            self.uninstall(check=False)
            time.sleep(5)  # The uninstaller's second phase, had it started.
            self.assertTrue((self.app / "TorFox.exe").is_file(), "uninstalled while TorFox was open")
            self.assertIsNotNone(self.variant.uninstall_entry())
            self.assertEqual(tf.close(), 0)
        self.uninstall()
        self.assertFalse((self.app / "TorFox.exe").exists())


@unittest.skipUnless(sys.platform == "win32", "Windows only")
class PerUserInstallerTest(InstallerTests, unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.variant = Variant(test_build=True)

    def setUp(self) -> None:
        parent = pathlib.Path(tempfile.mkdtemp(prefix="torfox-install-test-"))
        self.addCleanup(shutil.rmtree, parent, True)
        self.app = parent / "TorFox"
        self.created_app = True
        super().setUp()


@unittest.skipUnless(sys.platform == "win32", "Windows only")
class ReleaseSetupInstallerTest(InstallerTests, unittest.TestCase):
    """The real setup, where it installs by default."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.variant = Variant(test_build=False)

    def setUp(self) -> None:
        self.app = pathlib.Path(os.environ["LOCALAPPDATA"]) / "Programs" / "TorFox"
        shortcut = self.variant.start_menu / f"{self.variant.name}.lnk"
        if self.app.exists() or shortcut.exists():
            self.skipTest(f"{self.app} or its shortcut exists; a real installation is left alone")
        self.created_app = True
        super().setUp()


if __name__ == "__main__":
    unittest.main()
