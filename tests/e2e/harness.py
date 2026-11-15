# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Launches a TorFox test build (TORFOX_TEST_HOOKS=ON) for end-to-end tests.

Every run gets a fresh user-data folder. The DevTools port is chosen by the
browser (--remote-debugging-port=0) and read back from DevToolsActivePort,
and the browser's JSON-lines test log is parsed for assertions.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import http.server
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from typing import Callable

from cdp import Browser, CdpError, Session

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_EXE = REPO / "out" / "test" / "src" / "browser" / "Release" / "TorFox.exe"


def torfox_exe() -> pathlib.Path:
    exe = pathlib.Path(os.environ.get("TORFOX_TEST_EXE", DEFAULT_EXE))
    if not exe.is_file():
        raise RuntimeError(f"TorFox test build not found at {exe}; build out/test (TORFOX_TEST_HOOKS=ON)")
    return exe


def wait_for(predicate: Callable[[], object], timeout: float, what: str, interval: float = 0.05):
    deadline = time.monotonic() + timeout
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() > deadline:
            raise TimeoutError(f"timed out waiting for {what}")
        time.sleep(interval)


# ----- Windows helpers --------------------------------------------------------

_user32 = ctypes.WinDLL("user32", use_last_error=True) if sys.platform == "win32" else None
WM_CLOSE = 0x0010


def _top_level_windows(pid: int) -> list[int]:
    found: list[int] = []
    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def callback(hwnd, _):
        owner = wt.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and _user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    _user32.EnumWindows(proc(callback), 0)
    return found


# ----- TorFox process ----------------------------------------------------------

class TorFox:
    def __init__(self, user_data: pathlib.Path | None = None, extra_args: list[str] | None = None,
                 start_url: str | None = None, host_rules: str | None = None,
                 exe: pathlib.Path | None = None) -> None:
        self._own_dir = user_data is None
        self.user_data = user_data or pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-"))
        self.log_path = self.user_data / "test-log.jsonl"
        self.exe = exe or torfox_exe()
        args = [
            str(self.exe),
            "--remote-debugging-port=0",
            f"--torfox-test-user-data={self.user_data}",
            f"--torfox-test-log={self.log_path}",
            # Covered by other windows, a test window would count as hidden:
            # no frames, and input that waits for them stalls.
            "--disable-backgrounding-occluded-windows",
        ]
        if start_url:
            args.append(f"--torfox-test-start-url={start_url}")
        if host_rules:
            args.append(f"--torfox-test-host-rules={host_rules}")
        args += extra_args or []
        self.args = args
        self.proc: subprocess.Popen | None = None
        self.browser: Browser | None = None

    # Lifecycle ---------------------------------------------------------------
    def start(self, timeout: float = 30.0) -> "TorFox":
        try:
            return self._start(timeout)
        except BaseException:
            self.kill()  # Never leak a half-started browser.
            raise

    def _start(self, timeout: float) -> "TorFox":
        self.user_data.mkdir(parents=True, exist_ok=True)
        if self.log_path.exists():
            self.log_path.unlink()
        port_file = self.user_data / "User Data" / "DevToolsActivePort"
        if port_file.exists():
            port_file.unlink()
        self.proc = subprocess.Popen(self.args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        def port():
            if self.proc.poll() is not None:
                raise RuntimeError(f"TorFox exited during startup (code {self.proc.returncode})")
            try:
                first = port_file.read_text().splitlines()[0]
                return int(first)
            except (OSError, IndexError, ValueError):
                return None

        self.port = wait_for(port, timeout, "DevToolsActivePort")
        self.browser = Browser(self.port)
        self.wait_log("window_created", timeout=timeout)
        return self

    def close(self, timeout: float = 20.0, check: bool = True) -> int:
        """Closes the window like a user would (WM_CLOSE) and waits for exit.

        With |check|, anything but a clean exit (code 0, no crash trace) raises."""
        if self.browser:
            self.browser.close()
            self.browser = None
        if not self.proc:
            return 0
        if self.proc.poll() is None:
            for hwnd in _top_level_windows(self.proc.pid):
                _user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            try:
                self.proc.wait(timeout)
            except subprocess.TimeoutExpired:
                self.kill()
                raise RuntimeError("TorFox did not exit after its window was closed")
        code = self.proc.returncode
        if check:
            self.assert_clean_exit()
        return code

    def assert_clean_exit(self) -> None:
        code = self.proc.returncode if self.proc else 0
        traces = self.events("crash_trace")
        if code != 0 or traces:
            raise RuntimeError(f"TorFox did not exit cleanly: code {code & 0xFFFFFFFF:#x}, "
                               f"crash traces {traces[:1]}")

    def kill(self) -> None:
        if self.proc and self.proc.poll() is None:
            subprocess.run(["taskkill", "/PID", str(self.proc.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.proc.wait(10)

    def cleanup(self) -> None:
        self.kill()
        if self._own_dir:
            # Child processes may hold files briefly after exit.
            for _ in range(50):
                try:
                    shutil.rmtree(self.user_data)
                    return
                except FileNotFoundError:
                    return
                except OSError:
                    time.sleep(0.1)

    def __enter__(self) -> "TorFox":
        return self.start()

    def __exit__(self, exc_type, exc, tb) -> None:
        try:
            if self.proc and self.proc.poll() is None:
                try:
                    self.close()
                except Exception:
                    self.kill()
                    if exc_type is None:
                        raise  # A dirty shutdown is a test failure.
            elif self.proc and exc_type is None:
                self.assert_clean_exit()
        finally:
            self.cleanup()

    # Test log ------------------------------------------------------------------
    def log(self) -> list[dict]:
        try:
            text = self.log_path.read_text(encoding="utf-8")
        except FileNotFoundError:
            return []
        events = []
        for line in text.splitlines():
            line = line.strip()
            if line:
                try:
                    events.append(json.loads(line))
                except json.JSONDecodeError:
                    pass  # A line still being written.
        return events

    def events(self, name: str, **match) -> list[dict]:
        return [e for e in self.log() if e.get("ev") == name and
                all(e.get(k) == v for k, v in match.items())]

    def wait_log(self, name: str, timeout: float = 15.0, **match) -> dict:
        return wait_for(lambda: (self.events(name, **match) or [None])[-1], timeout,
                        f"test-log event {name} {match}")

    # Targets -------------------------------------------------------------------
    def page(self, predicate: Callable[[dict], bool], timeout: float = 15.0) -> Session:
        target = self.browser.wait_target(predicate, timeout)
        session = self.browser.attach(target["targetId"])
        session.send("Runtime.enable")
        def loaded():
            # A new browser first holds an empty about:blank document, which is
            # already "complete"; wait for the real page to commit and load.
            try:
                state = session.evaluate("[location.href, document.readyState]")
            except CdpError:
                return False  # Context replaced mid-navigation.
            return state[0] != "about:blank" and predicate({"url": state[0], "type": "page"}) and                 state[1] == "complete"
        wait_for(loaded, timeout, f"{target['url']} to finish loading")
        return session

    def shell(self) -> Session:
        return self.page(lambda t: t["url"] == "torfox://shell/")

    def new_shell(self, known: set[str], timeout: float = 15.0) -> Session:
        """The shell of a window opened after the targets in |known| existed."""
        # page() also calls the predicate without a targetId once attached.
        return self.page(lambda t: t["url"] == "torfox://shell/" and t.get("targetId") not in known,
                         timeout)

    def shell_ids(self) -> set[str]:
        return {t["targetId"] for t in self.browser.targets() if t["url"] == "torfox://shell/"}

    def page_at(self, url_prefix: str, timeout: float = 15.0) -> Session:
        return self.page(lambda t: t["url"].startswith(url_prefix), timeout)


# ----- Input and shell state (Stage 2) ------------------------------------------

# CDP modifier bits.
ALT, CTRL, META, SHIFT = 1, 2, 4, 8

# Windows virtual-key codes used by the shortcut tests.
VK = {"tab": 0x09, "pageup": 0x21, "pagedown": 0x22, "home": 0x24, "left": 0x25, "right": 0x27,
      "f3": 0x72, "f5": 0x74, "equals": 0xBB, "minus": 0xBD, "0": 0x30}


def press(session: Session, key: str | int, modifiers: int = 0) -> None:
    """Sends a key press to |session|'s widget, as the keyboard would.

    TorFox handles its shortcuts in the browser process before the page sees
    them (CefKeyboardHandler::OnPreKeyEvent), which CDP key events reach."""
    code = key if isinstance(key, int) else VK.get(key.lower(), ord(key.upper()) if len(key) == 1 else 0)
    for kind in ("rawKeyDown", "keyUp"):
        session.send("Input.dispatchKeyEvent", {
            "type": kind, "windowsVirtualKeyCode": code, "nativeVirtualKeyCode": code,
            "modifiers": modifiers,
        })


def element_center(session: Session, selector: str) -> tuple[float, float]:
    rect = session.evaluate(
        "(() => { const r = document.querySelector(%s).getBoundingClientRect();"
        " return [r.left + r.width / 2, r.top + r.height / 2]; })()" % json.dumps(selector))
    return rect[0], rect[1]


def click(session: Session, x: float, y: float, button: str = "left", modifiers: int = 0) -> None:
    """A real (trusted, user-gesture) mouse click inside |session|'s page."""
    session.send("Input.dispatchMouseEvent", {"type": "mouseMoved", "x": x, "y": y})
    for kind in ("mousePressed", "mouseReleased"):
        session.send("Input.dispatchMouseEvent", {
            "type": kind, "x": x, "y": y, "button": button, "clickCount": 1,
            "modifiers": modifiers,
        })


def click_element(session: Session, selector: str, button: str = "left", modifiers: int = 0) -> None:
    x, y = element_center(session, selector)
    click(session, x, y, button, modifiers)


SHELL_STATE = """(() => {
  const s = document.querySelector('tf-shell');
  return JSON.parse(JSON.stringify({
    tabs: s.tabs, activeId: s.activeId, nav: s.nav, private: s.isPrivate,
    findOpen: s.findOpen, findCount: s.findCount, findActive: s.findActive,
    barVisible: s.barVisible, barItems: s.barItems, notice: s.notice, downloads: s.downloads,
    update: s.readyUpdate,
  }));
})()"""


def shell_state(shell: Session) -> dict:
    return shell.evaluate(SHELL_STATE)


def wait_state(shell: Session, predicate: Callable[[dict], object], what: str,
               timeout: float = 15.0) -> dict:
    last: dict = {}

    def check():
        nonlocal last
        last = shell_state(shell)
        return last if predicate(last) else None

    try:
        return wait_for(check, timeout, what)
    except TimeoutError as e:
        raise TimeoutError(f"{e}; last shell state: {json.dumps(last)[:2000]}") from None


def bridge_call(session: Session, cmd: str, args: dict | None = None, *,
                persistent: bool = False, raw: str | None = None) -> dict:
    """Sends one bridge message from an internal page and reports the outcome."""
    request = raw if raw is not None else json.dumps({"cmd": cmd, **({"args": args} if args is not None else {})})
    expr = """
      new Promise((resolve) => {
        if (typeof window.torfoxQuery !== "function") {
          resolve({ok: false, code: -2, message: "no bridge"});
          return;
        }
        let settled = false;
        const id = window.torfoxQuery({
          request: %s,
          persistent: %s,
          onSuccess: (r) => { if (!settled) { settled = true; resolve({ok: true, response: r}); } },
          onFailure: (code, message) => { if (!settled) { settled = true; resolve({ok: false, code, message}); } },
        });
        if (%s) { setTimeout(() => { if (!settled) { settled = true; window.torfoxQueryCancel(id); resolve({ok: true, response: null, pending: true}); } }, 1500); }
      })
    """ % (json.dumps(request), "true" if persistent else "false", "true" if persistent else "false")
    return session.evaluate(expr)


# ----- Local fixture web server ----------------------------------------------------

class Fixture:
    """A loopback HTTP server whose routes are set per test."""

    def __init__(self) -> None:
        self.routes: dict[str, tuple[int, dict[str, str], bytes]] = {}
        # path -> (headers, chunk, count, delay): a body sent slowly, chunk by chunk.
        self.slow_routes: dict[str, tuple] = {}
        self.range_requests: list[str] = []
        self.requests: list[str] = []
        self.stopping = threading.Event()
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def _serve_slow(self, headers, chunk, count, delay, stall_after, cut_after,
                            resumable) -> None:
                if resumable:
                    self._serve_resumable(headers, chunk, count, cut_after)
                    return
                self.send_response(200)
                for k, v in headers.items():
                    self.send_header(k, v)
                self.send_header("Content-Length", str(len(chunk) * count))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                try:
                    for i in range(count):
                        if stall_after is not None and i == stall_after:
                            fixture.stopping.wait()
                            return
                        if cut_after is not None and i == cut_after:
                            self.close_connection = True
                            return
                        if fixture.stopping.wait(delay):
                            return
                        self.wfile.write(chunk)
                        self.wfile.flush()
                except OSError:
                    pass  # The browser cancelled the download.

            def _serve_resumable(self, headers, chunk, count, cut_after) -> None:
                # Chunked, with validators and range support, so a browser can
                # resume it; every response stops after |cut_after| chunks
                # without the final empty chunk (a network failure).
                total = len(chunk) * count
                start = 0
                rng = self.headers.get("Range", "")
                if rng.startswith("bytes="):
                    start = min(int(rng[6:].split("-", 1)[0] or 0), total)
                    fixture.range_requests.append(self.path.split("?", 1)[0])
                self.protocol_version = "HTTP/1.1"
                self.send_response(206 if start else 200)
                for k, v in headers.items():
                    self.send_header(k, v)
                self.send_header("ETag", '"torfox-test"')
                self.send_header("Last-Modified", "Wed, 21 Oct 2015 07:28:00 GMT")
                self.send_header("Accept-Ranges", "bytes")
                if start:
                    self.send_header("Content-Range", f"bytes {start}-{total - 1}/{total}")
                self.send_header("Transfer-Encoding", "chunked")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.close_connection = True
                data = (chunk * count)[start:]
                try:
                    for i in range(0, min(len(data), len(chunk) * cut_after), len(chunk)):
                        piece = data[i:i + len(chunk)]
                        self.wfile.write(f"{len(piece):x}\r\n".encode() + piece + b"\r\n")
                        self.wfile.flush()
                        if fixture.stopping.wait(0.02):
                            return
                except OSError:
                    pass

            def _serve(self) -> None:
                fixture.requests.append(f"{self.command} {self.path}")
                path = self.path.split("?", 1)[0]
                if path in fixture.slow_routes and self.command == "GET":
                    self._serve_slow(*fixture.slow_routes[path])
                    return
                status, headers, body = fixture.routes.get(path, (404, {}, b"not found"))
                self.send_response(status)
                for k, v in headers.items():
                    self.send_header(k, v)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(body)

            do_GET = do_POST = do_HEAD = _serve

            def log_message(self, *args) -> None:
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.origin = f"http://127.0.0.1:{self.port}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def html(self, path: str, body: str, headers: dict[str, str] | None = None) -> str:
        self.routes[path] = (200, {"Content-Type": "text/html; charset=utf-8", **(headers or {})},
                             body.encode("utf-8"))
        return self.origin + path

    def raw(self, path: str, status: int, headers: dict[str, str], body: bytes = b"") -> str:
        self.routes[path] = (status, headers, body)
        return self.origin + path

    def __enter__(self) -> "Fixture":
        self.thread.start()
        return self

    def slow(self, path: str, headers: dict[str, str], chunk: bytes, count: int,
             delay: float, stall_after: int | None = None, cut_after: int | None = None,
             resumable: bool = False) -> str:
        """A route that sends |count| chunks, |delay| seconds apart. After
        |stall_after| chunks it sends nothing more until the fixture stops;
        after |cut_after| chunks it closes the connection. |resumable| (with
        |cut_after|) makes it a chunked response a browser can resume, cut
        again after every |cut_after| chunks."""
        self.slow_routes[path] = (headers, chunk, count, delay, stall_after, cut_after, resumable)
        return self.origin + path

    def __exit__(self, *exc) -> None:
        self.stopping.set()
        self.server.shutdown()
        self.server.server_close()
