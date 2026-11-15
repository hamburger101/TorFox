# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Stage 5: Tor windows.

Most tests run TorFox with a scripted tor (fake_tor.py) that speaks tor's
control protocol, including SAFECOOKIE authentication, and records every
SOCKS request. Two local web servers stand in for the internet:

  * the "direct" server is where every test URL points (site.test:<port>,
    mapped to 127.0.0.1 by the test resolver rules). A Tor window must never
    reach it: any request there is a connection that bypassed tor.
  * the "Tor side" server is where the fake tor relays every SOCKS
    connection, whatever host it names.

RealTorTest starts the bundled tor itself: it must accept TorFox's torrc and
TorFox's control session. Whether it finishes bootstrapping depends on the
network and is reported, not required. LiveTorTest needs a bootstrapped tor:
it checks that a Tor window's traffic leaves through Tor
(check.torproject.org) and reaches an onion service. Both use the network,
so they run only with TORFOX_LIVE_TESTS=1.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

import netlog
from harness import ALT, SHIFT, Fixture, TorFox, bridge_call, click_element, press, shell_state, \
    wait_for
from sites import Sites

HERE = pathlib.Path(__file__).resolve().parent
FAKE_TOR = HERE / "fake_tor.py"
DDG = "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion"
OBFS4 = ("obfs4 192.0.2.10:443 0123456789ABCDEF0123456789ABCDEF01234567 "
         "cert=AAAAbbbbCCCCddddEEEEffffGGGGhhhhIIIIjjjjKKKKllllMMMMnnnnOOOOpppp iat-mode=0")

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


def process_alive(pid: int) -> bool:
    handle = _kernel32.OpenProcess(0x00100000 | 0x1000, False, pid)
    if not handle:
        return False
    try:
        return _kernel32.WaitForSingleObject(handle, 0) == 0x102
    finally:
        _kernel32.CloseHandle(handle)


def in_kill_on_close_job(pid: int) -> bool:
    """True if |pid| is in some job object. Weak on its own: when the test
    runner is itself in a job (as under some terminals and CI agents), every
    process it starts is. TorProcessTest.test_the_job_alone_ends_tor_if_
    torfox_is_killed is the behaviour test for TorFox's own job."""
    handle = _kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        raise OSError(ctypes.get_last_error(), "OpenProcess")
    try:
        result = wt.BOOL()
        if not _kernel32.IsProcessInJob(handle, None, ctypes.byref(result)):
            raise OSError(ctypes.get_last_error(), "IsProcessInJob")
        return bool(result.value)
    finally:
        _kernel32.CloseHandle(handle)


def short_path(path: pathlib.Path) -> str:
    """|path|'s 8.3 form ("" if the volume keeps no short names)."""
    buf = ctypes.create_unicode_buffer(32768)
    n = _kernel32.GetShortPathNameW(str(path), buf, len(buf))
    return buf.value if 0 < n < len(buf) and buf.value.isascii() else ""


TOR_STATE = "JSON.parse(JSON.stringify(document.querySelector('tf-shell').tor))"


class TorBase(unittest.TestCase):
    """One TorFox, with a fake tor, per test."""

    PLAN: dict = {}
    EXTRA_ARGS: list[str] = []
    FAKE = True
    # Names mapped to the direct server. A Tor window never needs one: tor
    # (the fake) relays every connection to the Tor side, whatever it names.
    HOST_RULES = "MAP site.test 127.0.0.1, MAP other.test 127.0.0.1"
    # With TLS_HOST, the Tor side is an https site with a certificate made
    # for this run (sites.py), trusted by its public key in test builds.
    TLS_HOST: str | None = None
    TLS_OTHER: list[str] = []  # More https names on the same server.
    PROFILE = "profile"  # The profile folder's name.

    def setUp(self) -> None:
        self.root = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-tor-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.direct = Fixture().__enter__()
        self.addCleanup(self.direct.__exit__, None, None, None)
        self.torside = Fixture().__enter__()
        self.addCleanup(self.torside.__exit__, None, None, None)
        self.record = self.root / "fake-tor.jsonl"
        self.release = self.root / "release"
        self.die = self.root / "die"
        self.choice = self.root / "menu-choice.txt"
        self.dialog = self.root / "dialog-choice.txt"
        relay = self.torside.port
        args = []
        host_rules = self.HOST_RULES
        if self.TLS_HOST:
            names = [self.TLS_HOST, *self.TLS_OTHER]
            self.sites = Sites(names).__enter__()
            # Normal windows (the controls) reach the same sites directly.
            host_rules += ", " + ", ".join(f"MAP {n} 127.0.0.1" for n in names)
            self.addCleanup(self.sites.__exit__, None, None, None)
            relay = self.sites.https_port
            args += [f"--ignore-certificate-errors-spki-list={self.sites.spki}",
                     "--use-fake-device-for-media-stream"]
        plan = {"relay_port": relay, "release_file": str(self.release),
                "die_file": str(self.die), **self.PLAN}
        plan_path = self.root / "plan.json"
        plan_path.write_text(json.dumps(plan), encoding="utf-8")
        self.plan, self.plan_path = plan, plan_path
        command = self.root / "tor-command.json"
        command.write_text(json.dumps({"exe": sys.executable, "args": [
            "-I", str(FAKE_TOR), "--plan", str(plan_path), "--record", str(self.record)]}),
            encoding="utf-8")
        self.routes()
        args += [f"--torfox-test-menu-choice={self.choice}", f"--torfox-test-dialog-choice={self.dialog}",
                 *self.EXTRA_ARGS]
        if self.FAKE:
            args.append(f"--torfox-test-tor-command={command}")
        profile = self.root / self.PROFILE
        if not self.PROFILE.isascii():
            profile.mkdir()
            if not short_path(profile):
                self.skipTest("this volume keeps no 8.3 names")
        self.tf = TorFox(user_data=profile,
                         host_rules=host_rules,
                         extra_args=args)
        self.prepare(self.tf)
        self.tf.start()
        self.addCleanup(self.tf.cleanup)
        self.normal = self.tf.shell()

    def prepare(self, tf: TorFox) -> None:
        """Before TorFox starts (extra arguments, settings files)."""

    def routes(self) -> None:
        self.both("/page", "<!doctype html><title>Tor page</title><p>hello</p>")
        self.both("/two", "<!doctype html><title>Page two</title><p>two</p>")
        self.both("/set", "<!doctype html><title>set</title><script>"
                          "document.cookie = 'tor=1; max-age=3600; path=/';</script>")
        self.both("/read", "<!doctype html><title>read</title><script>"
                           "document.title = document.cookie || 'none';</script>")

    def both(self, path: str, body: str) -> None:
        self.direct.html(path, body)
        self.torside.html(path, body)

    def url(self, path: str, host: str = "site.test") -> str:
        return f"http://{host}:{self.direct.port}{path}"

    # ----- helpers -----

    def rec(self, event: str | None = None) -> list[dict]:
        try:
            lines = self.record.read_text(encoding="utf-8").splitlines()
        except FileNotFoundError:
            return []
        out = []
        for line in lines:
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            if event is None or e.get("ev") == event:
                out.append(e)
        return out

    def mark(self) -> int:
        return len(self.tf.log())

    def since(self, mark: int, event: str, **match) -> list[dict]:
        return [e for e in self.tf.log()[mark:] if e.get("ev") == event and
                all(e.get(k) == v for k, v in match.items())]

    def wait_since(self, mark: int, event: str, timeout: float = 20.0, **match) -> dict:
        return wait_for(lambda: (self.since(mark, event, **match) or [None])[-1], timeout,
                        f"{event} {match}")

    def tor_status(self, shell) -> dict | None:
        return shell.evaluate(TOR_STATE)

    def open_tor(self, via=None):
        known = self.tf.shell_ids()
        created = len(self.tf.events("window_created", tor=True))
        self.assertTrue(bridge_call(via or self.normal, "window.newTor")["ok"])
        wait_for(lambda: len(self.tf.events("window_created", tor=True)) > created, 20, "Tor window")
        shell = self.tf.new_shell(known)
        wait_for(lambda: self.tor_status(shell) is not None, 10, "Tor state in the shell")
        return shell

    def replan(self, **changes) -> None:
        """Changes the plan the next (re)started fake tor follows."""
        self.plan = {**self.plan, **changes}
        self.plan_path.write_text(json.dumps(self.plan), encoding="utf-8")

    def wait_connected(self, shell, timeout: float = 30.0) -> dict:
        return wait_for(lambda: (lambda s: s if s and s["connected"] else None)(self.tor_status(shell)),
                        timeout, "Tor connected")

    def go(self, shell, text: str) -> dict:
        return bridge_call(shell, "nav.go", {"input": text})

    def load(self, shell, url: str, title: str | None = None):
        r = self.go(shell, url)
        self.assertTrue(r["ok"], r)
        page = self.tf.page(lambda t: t["url"] == url, 20)
        if title is not None:
            wait_for(lambda: page.evaluate("document.title") == title, 15, f"title {title}")
        return page

    def settings_page(self):
        self.assertTrue(bridge_call(self.normal, "app.openSettings")["ok"])
        return self.tf.page(lambda t: t["url"].startswith("torfox://settings/"))

    def set_setting(self, key: str, value) -> dict:
        return bridge_call(self.settings_page(), "settings.set", {"key": key, "value": value})

    def assert_nothing_direct(self) -> None:
        self.assertEqual(self.direct.requests, [], "a Tor window reached a site without tor")


class TorWindowTest(TorBase):
    # Tor stays at 10% until the test creates the release file.
    PLAN = {"hold_at": 10}

    def test_tor_window_waits_for_tor_then_loads_through_tor_only(self) -> None:
        shell = self.open_tor()
        status = wait_for(lambda: (lambda s: s if s and s["progress"] == 10 else None)(self.tor_status(shell)),
                          20, "bootstrap at 10%")
        self.assertFalse(status["connected"])
        self.assertEqual(status["state"], "connecting")
        mark = self.mark()
        url = self.url("/page")
        self.assertTrue(self.go(shell, url)["ok"])
        self.wait_since(mark, "tor_blocked", why="tor not connected")
        time.sleep(1.0)
        self.assert_nothing_direct()
        self.assertEqual(self.torside.requests, [])
        self.assertEqual(self.rec("socks"), [], "nothing may reach tor's SOCKS port before it is connected")

        self.release.touch()
        self.wait_since(mark, "tor_pending_loaded")
        page = self.tf.page(lambda t: t["url"] == url, 20)
        wait_for(lambda: page.evaluate("document.title") == "Tor page", 15, "page through tor")
        self.assertTrue(self.tor_status(shell)["connected"])
        socks = self.rec("socks")
        self.assertTrue(socks)
        # Chromium hands tor the host name (SOCKS5 address type 3): no local DNS.
        self.assertEqual({s["atyp"] for s in socks}, {3})
        self.assertIn(("site.test", self.direct.port), {(s["host"], s["port"]) for s in socks})
        self.assertIn("GET /page", self.torside.requests)
        self.assert_nothing_direct()
        # A Tor window is a private window: nothing is recorded.
        self.assertFalse([e for e in self.tf.events("page_loaded") if e.get("recorded")])
        self.assertIn("Tor", shell.evaluate(
            "document.querySelector('tf-shell').shadowRoot.querySelector('#tor-status').textContent"))

    def test_torrc_and_control_session(self) -> None:
        shell = self.open_tor()
        self.release.touch()
        self.wait_connected(shell)
        started = self.rec("started")[0]
        torrc = started["torrc"].splitlines()
        for line in ("SocksPort 127.0.0.1:auto ExtendedErrors IsolateDestAddr",
                     "ControlPort 127.0.0.1:auto", "CookieAuthentication 1",
                     f"__OwningControllerProcess {self.tf.proc.pid}", "ClientOnly 1",
                     "AvoidDiskWrites 1", "SafeLogging 1"):
            self.assertIn(line, torrc)
        self.assertFalse([l for l in torrc if l.startswith(("UseBridges", "Bridge ", "ClientTransportPlugin"))])
        self.assertEqual(started["defaults"], "", "tor must not read defaults from anywhere else")
        self.assertEqual(pathlib.Path(started["cwd"]).name, "tor")
        commands = [c["command"] for c in self.rec("control")]
        self.assertEqual(commands[:7], ["PROTOCOLINFO 1", "AUTHCHALLENGE", "AUTHENTICATE", "TAKEOWNERSHIP",
                                        "SETEVENTS STATUS_CLIENT", "GETINFO net/listeners/socks",
                                        "GETINFO status/bootstrap-phase"])
        self.assertTrue(self.rec("authenticated"))
        applied = self.tf.events("tor_port_applied")[-1]
        self.assertTrue(applied["ok"])
        self.assertEqual(applied["port"], self.rec("listening")[-1]["socks"])
        controlled = self.tf.events("tor_controlled")[-1]
        self.assertEqual(controlled["version"], "0.4.9.99-fake")

    def test_onion_addresses_from_a_normal_window_open_in_a_tor_window(self) -> None:
        self.both("/onion", "<!doctype html><title>Onion page</title>")
        self.release.touch()
        known = self.tf.shell_ids()
        before = shell_state(self.normal)
        self.assertTrue(self.go(self.normal, f"{DDG}/onion")["ok"])
        self.tf.wait_log("window_created", tor=True)
        shell = self.tf.new_shell(known)
        page = self.tf.page(lambda t: t["url"] == f"http://{DDG}/onion", 30)
        wait_for(lambda: page.evaluate("document.title") == "Onion page", 15, "onion page")
        self.assertIn((DDG, 80), {(s["host"], s["port"]) for s in self.rec("socks")})
        # The normal window is untouched: same tabs, same addresses.
        tabs = lambda st: [(t["id"], t["url"]) for t in st["tabs"]]  # noqa: E731
        self.assertEqual(tabs(shell_state(self.normal)), tabs(before))
        state = shell.evaluate("document.querySelector('tf-shell').shadowRoot.querySelector('.badge.onion') !== null")
        self.assertTrue(state, "the address bar shows an onion site")
        # Opened on an address, the window's first page is a visible page.
        self.assertEqual(page.evaluate("document.visibilityState"), "visible")

    def test_mistyped_onion_addresses_are_refused_and_never_searched(self) -> None:
        typo = DDG[:20] + ("b" if DDG[20] == "a" else "a") + DDG[21:]
        for text in (typo, f"what is {DDG}", "expyuzz4wqqyqhjn.onion", "ftp://" + DDG):
            created = len(self.tf.events("window_created"))
            r = self.go(self.normal, text)
            self.assertFalse(r["ok"], text)
            self.assertTrue(r["message"], text)
            time.sleep(0.3)
            self.assertEqual(len(self.tf.events("window_created")), created, text)
        self.assertEqual(self.rec("socks"), [])
        self.assertFalse(self.tf.events("nav_input", kind=1))  # Never a search (NavKind::kSearch).

    def test_searches_in_tor_windows_use_duckduckgos_onion_service(self) -> None:
        shell = self.open_tor()
        self.release.touch()
        self.wait_connected(shell)
        self.assertTrue(self.go(shell, "tor search words")["ok"])
        wait_for(lambda: [s for s in self.rec("socks") if s["host"] == DDG], 20, "search through tor")
        self.assertIn((DDG, 443), {(s["host"], s["port"]) for s in self.rec("socks")})
        self.assertFalse([s for s in self.rec("socks") if s["host"] == "duckduckgo.com"])
        # Settings > Search engine picks the page on the onion service too.
        for engine, path in (("duckduckgo-html", "/html/"), ("duckduckgo-lite", "/lite/")):
            self.assertTrue(self.set_setting("search.engine", engine)["ok"])
            self.assertTrue(self.go(shell, f"{engine} words")["ok"])
            wait_for(lambda: shell_state(shell)["nav"].get("url") ==
                     f"https://{DDG}{path}?q={engine}%20words", 20, f"{engine} over tor")
        self.assertFalse([s for s in self.rec("socks") if "duckduckgo.com" in s["host"]])

    def test_tor_windows_refuse_this_computer_and_the_local_network(self) -> None:
        shell = self.open_tor()
        self.release.touch()
        self.wait_connected(shell)
        for text in (f"http://127.0.0.1:{self.direct.port}/page", f"localhost:{self.direct.port}/page",
                     "http://192.168.1.1/", "http://[::1]/"):
            r = self.go(shell, text)
            self.assertFalse(r["ok"], text)
            self.assertIn("local network", r["message"])
        self.both("/embeds", f"""<!doctype html><title>embeds</title>
            <img src="http://127.0.0.1:{self.direct.port}/pic.gif">
            <img src="http://localhost:{self.direct.port}/pic2.gif">
            <img src="http://10.0.0.1/pic3.gif">""")
        mark = self.mark()
        self.load(shell, self.url("/embeds"), "embeds")
        self.wait_since(mark, "resource_blocked", why="local address in a Tor window")
        time.sleep(0.5)
        # Chromium may ask about the same image more than once (preload
        # scanner, then the parser); each of the three must be refused.
        blocked = {e["url"] for e in self.since(mark, "resource_blocked", why="local address in a Tor window")}
        self.assertEqual(blocked, {f"http://127.0.0.1:{self.direct.port}/pic.gif",
                                   f"http://localhost:{self.direct.port}/pic2.gif", "http://10.0.0.1/pic3.gif"})
        self.assert_nothing_direct()
        self.assertFalse([s for s in self.rec("socks") if s["host"] in ("127.0.0.1", "localhost", "10.0.0.1")])

    def test_link_menu_never_leaves_tor(self) -> None:
        self.both("/links", f"""<!doctype html><title>links</title>
            <a id="plain" href="{self.url('/two')}" style="display:block;padding:20px">two</a>""")
        shell = self.open_tor()
        self.release.touch()
        self.wait_connected(shell)
        page = self.load(shell, self.url("/links"), "links")
        seen = len(self.tf.events("menu_shown", kind="web_context"))
        self.choice.write_text("", encoding="utf-8")
        click_element(page, "#plain", button="right")
        menu = wait_for(lambda: (self.tf.events("menu_shown", kind="web_context")[seen:] or [None])[0], 10, "menu")
        labels = [i["label"] for i in menu["items"]]
        self.assertNotIn("Open link in private window", labels)
        self.assertIn("Open link in new Tor window", labels)

    def test_new_tor_window_shortcut(self) -> None:
        created = len(self.tf.events("window_created", tor=True))
        press(self.normal, "n", ALT | SHIFT)
        wait_for(lambda: len(self.tf.events("window_created", tor=True)) > created, 20, "Tor window")


class TorFailsClosedTest(TorBase):
    def test_when_tor_dies_nothing_loads_until_it_is_back(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        self.load(shell, self.url("/page"), "Tor page")
        first_pid = self.rec("started")[0]["pid"]
        mark = self.mark()
        self.die.touch()
        self.wait_since(mark, "tor_stopped")
        wait_for(lambda: (lambda s: s if s and s["state"] == "error" else None)(self.tor_status(shell)),
                 10, "Tor error in the shell")
        self.assertFalse(self.tor_status(shell)["connected"])
        self.assertTrue(self.go(shell, self.url("/two"))["ok"])
        self.wait_since(mark, "tor_blocked", why="tor not connected")
        # The page asked for while tor is down is not served by anything.
        # (Requests the first page made before tor died, such as its icon,
        # may still arrive: what matters is that this one does not.)
        self.assertNotIn("GET /two", self.torside.requests)
        self.assert_nothing_direct()
        # TorFox starts tor again by itself, and the page waiting for it loads.
        self.wait_since(mark, "tor_restart", timeout=15)
        wait_for(lambda: [s for s in self.rec("started") if s["pid"] != first_pid], 15, "tor restarted")
        self.wait_since(mark, "tor_pending_loaded", timeout=30)
        page = self.tf.page(lambda t: t["url"] == self.url("/two"), 20)
        wait_for(lambda: page.evaluate("document.title") == "Page two", 15, "page after restart")
        self.assert_nothing_direct()


class ProxyLayerTest(TorBase):
    """The second layer on its own: with the Tor windows' guard switched off
    (--torfox-test-skip-guards=tor), a tor that is not there still means no
    connection at all, never a direct one."""

    EXTRA_ARGS = ["--torfox-test-skip-guards=tor"]
    PLAN = {"exit_at_start": 3}

    def test_before_tor_ever_reports_a_port_requests_fail(self) -> None:
        shell = self.open_tor()
        mark = self.mark()
        self.assertTrue(self.go(shell, self.url("/page"))["ok"])
        error = self.wait_since(mark, "load_error")
        self.assertNotEqual(error["code"], 0)
        self.assertFalse(self.since(mark, "tor_blocked"), "the guard was meant to be off")
        time.sleep(1.0)
        self.assert_nothing_direct()
        self.assertEqual(self.torside.requests, [])

    def test_the_placeholder_port_is_torfoxs_own(self) -> None:
        # Until tor reports its port, the Tor context points at a placeholder.
        # A fixed port (9, as before) could be taken by any program, which
        # would then be sent the names Tor windows open. TorFox holds its own
        # and resets every connection to it; no other program can take it.
        shell = self.open_tor()
        ready = self.tf.wait_log("context_ready", kind="tor")
        port = ready["placeholder"]
        self.assertGreater(port, 0)
        self.assertNotEqual(port, 9)
        self.assertEqual((ready["socks_port"], ready["proxy_port"]), (0, port))
        for reuse in (False, True):
            with self.subTest("127.0.0.1", reuse=reuse):
                squatter = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                try:
                    if reuse:
                        squatter.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    with self.assertRaises(OSError):
                        squatter.bind(("127.0.0.1", port))
                finally:
                    squatter.close()
        # Windows lets another program listen on every address of the port;
        # it must never be given a connection to 127.0.0.1.
        wild = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.addCleanup(wild.close)
        wild.bind(("0.0.0.0", port))
        wild.listen(8)
        wild.settimeout(0.2)
        # TorFox's listener resets the connection without reading.
        probe = socket.create_connection(("127.0.0.1", port), timeout=10)
        with probe:
            probe.settimeout(5)
            try:
                probe.sendall(b"\x05\x01\x00")
                got = probe.recv(16)
            except (ConnectionResetError, ConnectionAbortedError):
                got = b""
            self.assertEqual(got, b"")
        mark = self.mark()
        self.assertTrue(self.go(shell, self.url("/page"))["ok"])
        self.assertNotEqual(self.wait_since(mark, "load_error")["code"], 0)
        reached = []
        deadline = time.time() + 1.5
        while time.time() < deadline:
            try:
                conn, _ = wild.accept()
            except (socket.timeout, TimeoutError):
                continue
            with conn:
                reached.append(conn.recv(64))
        self.assertEqual(reached, [], "a Tor window's request reached another program")
        self.assert_nothing_direct()
        self.assertEqual(self.torside.requests, [])


class ProxyLayerAfterDeathTest(TorBase):
    EXTRA_ARGS = ["--torfox-test-skip-guards=tor"]

    def test_a_dead_tor_means_connection_refused(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        self.load(shell, self.url("/page"), "Tor page")
        mark = self.mark()
        self.die.touch()
        self.wait_since(mark, "tor_stopped")
        socks_before = len(self.rec("socks"))
        self.assertTrue(self.go(shell, self.url("/two"))["ok"])
        self.wait_since(mark, "load_error")
        self.assertFalse(self.since(mark, "tor_blocked"))
        self.assertEqual(len(self.rec("socks")), socks_before)
        self.assert_nothing_direct()

    def test_a_dead_tors_port_is_never_used_again(self) -> None:
        # Once tor has exited, any program could listen on its old SOCKS port
        # and would be sent the names Tor windows open. The guard is off here,
        # so only the Tor context's proxy stands between the request and it.
        shell = self.open_tor()
        self.wait_connected(shell)
        self.load(shell, self.url("/page"), "Tor page")
        old_port = self.rec("listening")[-1]["socks"]
        mark = self.mark()
        self.die.touch()
        self.wait_since(mark, "tor_stopped")
        wait_for(lambda: (self.tor_status(shell) or {"connected": True})["connected"] is False, 10,
                 "Tor shown as not connected")
        placeholder = self.tf.events("context_ready", kind="tor")[0]["placeholder"]
        moved = self.since(mark, "tor_proxy_port")[-1]
        self.assertEqual((moved["port"], moved["ok"]), (placeholder, True))
        squatter = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.addCleanup(squatter.close)
        squatter.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        squatter.bind(("127.0.0.1", old_port))
        squatter.listen(8)
        squatter.settimeout(0.2)
        self.assertTrue(self.go(shell, self.url("/two"))["ok"])
        self.wait_since(mark, "load_error")
        accepted = []
        deadline = time.time() + 2.0
        while time.time() < deadline:
            try:
                conn, _ = squatter.accept()
            except (socket.timeout, TimeoutError):
                continue
            with conn:
                conn.settimeout(1.0)
                try:
                    accepted.append(conn.recv(64))
                except OSError:
                    accepted.append(b"")
        self.assertEqual(accepted, [], "a Tor window's request went to tor's old port")
        self.assert_nothing_direct()


class NewIdentityTest(TorBase):
    def test_new_identity_closes_tor_windows_and_starts_fresh(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        self.load(shell, self.url("/set"), "set")
        self.load(shell, self.url("/read"), "tor=1")
        old_id = self.tf.events("window_created", tor=True)[-1]["id"]
        known = self.tf.shell_ids()
        mark = self.mark()
        # The shell's New identity button. The window it is in closes within
        # milliseconds, so the click is not awaited.
        shell.evaluate("document.querySelector('tf-shell').shadowRoot"
                       ".querySelector('#tor-identity').click(); 1")
        self.assertTrue(self.wait_since(mark, "tor_newnym")["ok"])
        self.assertTrue(self.rec("newnym"))
        done = self.wait_since(mark, "tor_new_identity")
        self.assertEqual(done["closed"], 1)
        self.assertTrue(done["opened"])
        self.wait_since(mark, "window_destroyed", tor=True, id=old_id)
        self.assertEqual(len(self.since(mark, "context_ready", kind="tor")), 1, "a new Tor context")
        fresh = self.tf.new_shell(known)
        self.wait_connected(fresh)
        self.load(fresh, self.url("/read"), "none")
        self.assert_nothing_direct()


class TorProcessTest(TorBase):
    def test_tor_runs_in_a_job_and_exits_with_torfox(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        pid = self.tf.events("tor_started")[-1]["pid"]
        self.assertEqual(pid, self.rec("started")[-1]["pid"])
        self.assertTrue(in_kill_on_close_job(pid))
        self.tf.close()
        wait_for(lambda: not process_alive(pid), 15, "tor to exit with TorFox")
        exits = self.rec("exit")
        self.assertTrue(exits)
        self.assertEqual(exits[-1]["why"], "owning controller closed")

    def test_the_job_alone_ends_tor_if_torfox_is_killed(self) -> None:
        # This tor ignores its owner (process and control connection), and
        # only TorFox's main process is terminated (not its process tree):
        # the kill-on-close job is all that can end tor.
        self.replan(ignore_owner=True)
        shell = self.open_tor()
        self.wait_connected(shell)
        pid = self.tf.events("tor_started")[-1]["pid"]
        self.assertTrue(process_alive(pid))
        self.tf.proc.kill()  # TerminateProcess on TorFox's main process only.
        self.tf.proc.wait(10)
        wait_for(lambda: not process_alive(pid), 10, "the job to end tor")
        self.assertFalse([e for e in self.rec("exit") if e["pid"] == pid], "tor was killed, it did not exit")

    def test_settings_memory_counts_tor_and_never_names_tor_tabs(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        pid = self.tf.events("tor_started")[-1]["pid"]
        self.load(shell, self.url("/page"), "Tor page")
        page = self.settings_page()

        def with_tor_tab():
            r = bridge_call(page, "memory.query")
            self.assertTrue(r["ok"], r)
            rep = json.loads(r["response"])
            return rep if any(t["window"] == "tor" for row in rep["tabs"] for t in row["tabs"]) else None
        rep = wait_for(with_tor_tab, 15, "the Tor tab in the report")
        # tor itself, as its own row; not as a helper or a tab.
        self.assertIn(pid, rep["tor"]["pids"])
        self.assertGreater(rep["tor"]["bytes"], 0)
        self.assertNotIn(pid, rep["helpers"]["pids"] + rep["interface"]["pids"])
        self.assertFalse([row for row in rep["tabs"] if pid in row["pids"]])
        text = json.dumps(rep)
        self.assertNotIn("Tor page", text)
        self.assertNotIn("site.test", text)
        for row in rep["tabs"]:
            for t in row["tabs"]:
                if t["window"] == "tor":
                    self.assertEqual(t["title"], "")

    def test_tor_is_not_started_until_a_tor_window_opens(self) -> None:
        time.sleep(1.5)
        self.assertEqual(self.rec("started"), [])
        self.assertFalse(self.tf.events("tor_started"))


class NonAsciiProfileTest(TorBase):
    """A profile folder whose name tor cannot read (a Windows user called
    José): tor reads torrc paths in the system code page, so TorFox gives it
    the folder's 8.3 name instead."""

    PROFILE = "profil\u00e9-\u0442\u0435\u0441\u0442"

    def test_tor_gets_a_path_it_can_read(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        torrc = self.rec("started")[-1]["torrc"]
        self.assertTrue(torrc.isascii(), torrc)
        data_dir = next(l for l in torrc.splitlines() if l.startswith("DataDirectory "))
        value = json.loads(data_dir.split(" ", 1)[1])  # A quoted, backslash-escaped path.
        self.assertTrue(pathlib.Path(value).samefile(self.tf.user_data / "Tor"), (value, self.tf.user_data))
        self.load(shell, self.url("/page"), "Tor page")


class TorAtLaunchTest(TorBase):
    def prepare(self, tf: TorFox) -> None:
        tf.user_data.mkdir(parents=True, exist_ok=True)
        (tf.user_data / "settings.json").write_text(json.dumps(
            {"format": 1, "settings": {"tor.startMode": "at-launch"}}), encoding="utf-8")

    def test_tor_starts_with_torfox_when_asked(self) -> None:
        wait_for(lambda: self.rec("started"), 20, "tor started at launch")
        self.tf.wait_log("tor_state", state="connected", timeout=20)


class BridgeSettingsTest(TorBase):
    def torrc(self, index: int = -1) -> list[str]:
        return self.rec("started")[index]["torrc"].splitlines()

    def test_custom_bridges_reach_the_torrc(self) -> None:
        webtunnel = "webtunnel [2001:db8::2]:443 0123456789ABCDEF0123456789ABCDEF01234567 url=https://example.com/x ver=0.0.1"
        r = self.set_setting("tor.bridges.custom", f"# mine\n{OBFS4}\n{webtunnel}\n")
        self.assertTrue(r["ok"], r)
        self.assertTrue(self.set_setting("tor.bridges.mode", "custom")["ok"])
        shell = self.open_tor()
        self.wait_connected(shell)
        torrc = self.torrc()
        self.assertIn("UseBridges 1", torrc)
        self.assertIn(f"Bridge {OBFS4}", torrc)
        self.assertIn(f"Bridge {webtunnel}", torrc)
        self.assertIn("ClientTransportPlugin meek_lite,obfs4,webtunnel exec pluggable_transports\\lyrebird.exe", torrc)

    def test_changing_bridges_restarts_tor_with_them(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        self.assertEqual(len(self.rec("started")), 1)
        mark = self.mark()
        self.assertTrue(self.set_setting("tor.bridges.builtinType", "snowflake")["ok"])
        self.assertTrue(self.set_setting("tor.bridges.mode", "builtin")["ok"])
        self.wait_since(mark, "tor_bridges_changed")
        wait_for(lambda: any("UseBridges 1" in s["torrc"] for s in self.rec("started")), 20, "restart with bridges")
        torrc = [s for s in self.rec("started") if "UseBridges 1" in s["torrc"]][-1]["torrc"].splitlines()
        bridges = [l for l in torrc if l.startswith("Bridge ")]
        self.assertTrue(bridges)
        self.assertTrue(all(l.startswith("Bridge snowflake ") for l in bridges), bridges)
        self.assertIn("ClientTransportPlugin snowflake exec pluggable_transports\\lyrebird.exe", torrc)
        self.wait_connected(shell)

    def test_invalid_custom_bridges_are_refused(self) -> None:
        for text in ("obfs4 192.0.2.1:443 nothex cert=x", "obfs4 192.0.2.1:443\nUseBridges 0",
                     "Bridge obfs4 192.0.2.1:443 " + "A" * 40 + " cert=a\"b"):
            r = self.set_setting("tor.bridges.custom", text)
            self.assertFalse(r["ok"], text)
            self.assertEqual(r["message"], "invalid_value")
        data = json.loads(bridge_call(self.settings_page(), "settings.get")["response"])
        value = next(s["value"] for s in data["settings"] if s["key"] == "tor.bridges.custom")
        self.assertEqual(value, "")

    def test_custom_mode_without_bridges_never_starts_tor(self) -> None:
        self.assertTrue(self.set_setting("tor.bridges.mode", "custom")["ok"])
        shell = self.open_tor()
        status = wait_for(lambda: (lambda s: s if s and s["state"] == "error" else None)(self.tor_status(shell)),
                          15, "Tor error")
        self.assertIn("none are entered", status["message"])
        self.assertEqual(self.rec("started"), [], "tor must not start without the bridges the user asked for")
        mark = self.mark()
        self.assertTrue(self.go(shell, self.url("/page"))["ok"])
        self.wait_since(mark, "tor_blocked")
        self.assert_nothing_direct()


class TorContextIsolationTest(TorBase):
    # RTCDataChannel is not here: Blink installs it after TorFox has seen the
    # context, so it stays, but it cannot be constructed (checked below) and
    # only an RTCPeerConnection makes one.
    WEBRTC_NAMES = ["RTCPeerConnection", "webkitRTCPeerConnection", "RTCRtpTransceiver", "RTCRtpSender"]
    # The WebRTC interfaces a page can reach: in its own window, in an
    # about:blank and a srcdoc frame it makes, and in a frame of another
    # site (another renderer process), which reports by postMessage.
    WEBRTC_PROBE = """new Promise(resolve => {
        const names = %s;
        const seen = w => names.filter(n => { try { return typeof w[n] !== "undefined"; } catch (e) { return false; } });
        const blank = document.createElement("iframe");
        document.body.append(blank);
        const srcdoc = document.createElement("iframe");
        srcdoc.srcdoc = "<p>x</p>";
        const out = {top: seen(window), blank: seen(blank.contentWindow),
                     channel: (() => { try { new RTCDataChannel(); return "constructed"; }
                                       catch (e) { return e.name; } })()};
        let other = null, inner = null;
        const finish = () => { if (other !== null && inner !== null) resolve({...out, srcdoc: inner, other}); };
        srcdoc.onload = () => { inner = seen(srcdoc.contentWindow); finish(); };
        addEventListener("message", e => { other = e.data; finish(); });
        document.body.append(srcdoc);
        const cross = document.createElement("iframe");
        cross.src = %s;
        document.body.append(cross);
        setTimeout(() => resolve({...out, srcdoc: inner, other, timeout: true}), 10000);
    })"""

    def routes(self) -> None:
        super().routes()
        names = json.dumps(self.WEBRTC_NAMES)
        self.both("/rtc-frame", f"""<!doctype html><script>
            parent.postMessage({names}.filter(n => typeof window[n] !== "undefined"), "*");</script>""")

    def test_tor_windows_have_no_webrtc(self) -> None:
        # WebRTC looks up servers and candidates by name itself, outside the
        # proxy (TorDnsTest.test_webrtc_looks_up_no_name_for_tor_windows), so
        # Tor windows have none, as in Tor Browser.
        def probe(page) -> dict:
            frame = self.url("/rtc-frame", host="other.test")
            return page.evaluate(self.WEBRTC_PROBE % (json.dumps(self.WEBRTC_NAMES), json.dumps(frame)),
                                 timeout=20)
        shell = self.open_tor()
        self.wait_connected(shell)
        # Each window its own page: the harness finds tabs by URL.
        tor = probe(self.load(shell, self.url("/page"), "Tor page"))
        self.assertNotIn("timeout", tor)
        self.assertEqual(tor, {"top": [], "blank": [], "srcdoc": [], "other": [], "channel": "TypeError"})
        self.assert_nothing_direct()
        # Control: a normal window has it everywhere.
        normal = probe(self.load(self.normal, self.url("/two"), "Page two"))
        self.assertNotIn("timeout", normal)
        for where in ("top", "blank", "srcdoc", "other"):
            self.assertIn("RTCPeerConnection", normal[where], (where, normal))

    def test_system_proxy_setting_never_changes_the_tor_context(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        mark = self.mark()
        self.assertTrue(self.set_setting("privacy.useSystemProxy", True)["ok"])
        self.assertTrue(self.wait_since(mark, "proxy_mode")["ok"])
        self.assertTrue(self.since(mark, "proxy_mode_refused", context="tor"))
        socks = len(self.rec("socks"))
        self.load(shell, self.url("/two"), "Page two")
        self.assertGreater(len(self.rec("socks")), socks)
        self.assert_nothing_direct()
        self.assertTrue(self.set_setting("privacy.useSystemProxy", False)["ok"])

    GATHER = """new Promise(resolve => {
        const pc = new RTCPeerConnection({iceServers: [{urls: "stun:192.0.2.1:3478"}]});
        const found = [];
        const done = () => resolve({candidates: found, sdp: pc.localDescription ? pc.localDescription.sdp : ""});
        pc.onicecandidate = e => { if (e.candidate) found.push(e.candidate.candidate); else done(); };
        pc.createDataChannel("x");
        pc.createOffer().then(o => pc.setLocalDescription(o));
        setTimeout(done, 8000);
    })"""

    def test_webrtc_in_tor_windows_sends_nothing_and_reveals_no_address(self) -> None:
        # A STUN server the page can reach, on this computer's default-route
        # address (Chromium sends nothing to one on loopback): any UDP a page
        # sends arrives here.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("192.0.2.1", 9))  # Picks the route; sends nothing.
        local = probe.getsockname()[0]
        probe.close()
        stun = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(stun.close)
        stun.bind((local, 0))
        stun.settimeout(0.3)
        script = self.GATHER.replace("stun:192.0.2.1:3478", f"stun:{local}:{stun.getsockname()[1]}")

        def received() -> int:
            count = 0
            while True:
                try:
                    stun.recvfrom(4096)
                    count += 1
                except (socket.timeout, ConnectionResetError):
                    return count

        # Control: whether a normal window's page sends anything. On the
        # development machine it does not (no candidate at all, in any
        # window), so there the policy below is checked as configuration only.
        control = self.load(self.normal, self.url("/page"))
        control.evaluate(script, user_gesture=True, timeout=20)
        sent = received()
        print(f"\n[WebRTC] control: a normal window's page sent {sent} STUN packet(s)", end=" ")

        shell = self.open_tor()
        self.wait_connected(shell)
        # Another page than the control's: the harness finds tabs by URL.
        page = self.load(shell, self.url("/two"), "Page two")
        # Tor windows have no WebRTC at all (test_tor_windows_have_no_webrtc).
        result = page.evaluate("typeof RTCPeerConnection === 'undefined' ? 'absent' : " + script,
                               user_gesture=True, timeout=20)
        self.assertEqual(result, "absent")
        # SOCKS cannot carry UDP: nothing may leave outside Tor.
        self.assertEqual(received(), 0, "a Tor window's page sent UDP")
        # A second layer, should WebRTC ever be reachable: the policy, as
        # Chromium read it back for the Tor context, allows no UDP except
        # through the proxy (which has none).
        policy = [e for e in self.tf.events("pref", context="tor")
                  if e.get("name") == "webrtc.ip_handling_policy"]
        self.assertTrue(policy)
        self.assertEqual({(e["ok"], e["value"]) for e in policy}, {(True, "disable_non_proxied_udp")})


class TorSecureContextTest(TorBase):
    """Camera, microphone and clipboard exist only in secure contexts: these
    tests use an https site behind the (fake) tor, so the refusal tested is
    TorFox's own."""

    TLS_HOST = "secure.tor.test"
    # Another site (a different registrable domain), so its cookie is a
    # third-party cookie on TLS_HOST's pages.
    TLS_OTHER = ["cookies.example.net"]

    def embed(self, name: str) -> str:
        """TLS_HOST embeds TLS_OTHER, which sets a cookie and then reads it back."""
        s, other = self.sites, self.TLS_OTHER[0]
        s.raw(other, f"/set-{name}", 200, {"Content-Type": "text/html",
                                           "Set-Cookie": f"{name}=1; SameSite=None; Secure; Path=/"},
              b"<!doctype html>set")
        s.html(other, f"/read-{name}", "<!doctype html>read")
        s.html(self.TLS_HOST, f"/embed-{name}", f"""<!doctype html><title>embed</title>
            <iframe id="a" src="{s.url('https', other, f'/set-{name}')}"></iframe>
            <script>document.getElementById('a').onload = () => {{
              const b = document.createElement('iframe');
              b.src = {json.dumps(s.url('https', other, f'/read-{name}'))};
              document.body.append(b);
            }};</script>""")
        return s.url("https", self.TLS_HOST, f"/embed-{name}")

    def cookie_sent(self, name: str) -> str:
        reads = wait_for(lambda: self.sites.seen(self.TLS_OTHER[0], f"/read-{name}"), 20, "the embedded read")
        return reads[-1].headers.get("cookie", "")

    def test_third_party_cookies_are_always_blocked(self) -> None:
        # Allowed in Settings: normal windows then keep them (the control)...
        self.assertTrue(self.set_setting("privacy.blockThirdPartyCookies", False)["ok"])
        self.assertTrue(self.go(self.normal, self.embed("normal"))["ok"])
        self.assertIn("normal=1", self.cookie_sent("normal"))
        # ...but Tor windows never do.
        shell = self.open_tor()
        self.wait_connected(shell)
        socks = len(self.rec("socks"))
        self.assertTrue(self.go(shell, self.embed("tor"))["ok"])
        self.assertNotIn("tor=1", self.cookie_sent("tor"))
        self.assertIn(self.TLS_OTHER[0], {r["host"] for r in self.rec("socks")[socks:]}, "the Tor window's request")
        self.assertTrue(self.set_setting("privacy.blockThirdPartyCookies", True)["ok"])

    def test_permissions_and_other_apps_are_refused(self) -> None:
        self.assertTrue(self.set_setting("permissions.camera", "ask")["ok"])
        self.assertTrue(self.set_setting("permissions.clipboard", "ask")["ok"])
        self.sites.html(self.TLS_HOST, "/apps", """<!doctype html><title>apps</title>
            <a id="mail" href="mailto:someone@example.com" style="display:block;padding:20px">mail</a>""")
        shell = self.open_tor()
        self.wait_connected(shell)
        url = self.sites.url("https", self.TLS_HOST, "/apps")
        page = self.load(shell, url, "apps")
        self.assertTrue(page.evaluate("window.isSecureContext"))
        mark = self.mark()
        self.dialog.write_text("accept", encoding="utf-8")  # Would allow, if anything asked.
        camera = page.evaluate("navigator.mediaDevices.getUserMedia({video: true})"
                               ".then(s => { s.getTracks().forEach(t => t.stop()); return 'granted'; }, e => e.name)",
                               user_gesture=True, timeout=15)
        self.assertEqual(camera, "NotAllowedError")
        self.wait_since(mark, "permission_denied", kind="media", why="tor window")
        clipboard = page.evaluate("navigator.clipboard.readText().then(() => 'granted', e => e.name)",
                                  user_gesture=True, timeout=15)
        self.assertEqual(clipboard, "NotAllowedError")
        self.wait_since(mark, "permission_denied", kind="prompt", why="tor window")
        click_element(page, "#mail")
        self.wait_since(mark, "protocol_blocked", why="tor window")
        self.assertFalse(self.since(mark, "dialog_shown"), "nothing may ask in a Tor window")
        self.assertFalse(self.since(mark, "external_opened"))
        self.assertTrue([r for r in self.rec("socks") if r["host"] == self.TLS_HOST])
        self.assert_nothing_direct()


class TorContextGateTest(TorBase):
    """Requests no tab makes (a service worker's own fetches) are checked by
    the Tor context itself: they wait for tor like a Tor tab's, and local
    addresses are refused."""

    TLS_HOST = "worker.tor.test"
    WORKER = b"""
        self.addEventListener('install', () => self.skipWaiting());
        self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
        self.addEventListener('fetch', e => {
          const u = new URL(e.request.url);
          if (u.pathname === '/via-worker') {
            e.respondWith(fetch(u.searchParams.get('to'), {cache: 'no-store'})
              .then(r => r.text(), () => 'failed').then(t => new Response(t)));
          }
        });
        // 'tick': for 20 seconds, whatever its pages do, the worker keeps
        // fetching and opening WebSockets on its own.
        self.addEventListener('message', e => {
          if (e.data !== 'tick') return;
          e.waitUntil((async () => {
            for (let i = 0; i < 130; i++) {
              fetch('/tick?n=' + i, {cache: 'no-store'}).catch(() => {});
              try { new WebSocket('wss://' + location.host + '/ws?n=' + i); } catch (err) {}
              await new Promise(r => setTimeout(r, 150));
            }
          })());
        });"""

    def via_worker(self, page, url: str) -> str:
        return page.evaluate("fetch('/via-worker?to=' + encodeURIComponent(%s)).then(r => r.text())"
                             % json.dumps(url), timeout=20)

    def test_service_worker_requests_wait_for_tor_and_never_reach_local_addresses(self) -> None:
        host = self.TLS_HOST
        self.sites.raw(host, "/worker.js", 200, {"Content-Type": "text/javascript"}, self.WORKER)
        self.sites.html(host, "/worker-page", "<!doctype html><title>worker</title>")
        self.sites.html(host, "/from-worker", "from-worker")
        shell = self.open_tor()
        self.wait_connected(shell)
        page = self.load(shell, self.sites.url("https", host, "/worker-page"), "worker")
        page.evaluate("""navigator.serviceWorker.register('/worker.js')
            .then(() => navigator.serviceWorker.ready)
            .then(() => navigator.serviceWorker.controller ? 1 : new Promise(r =>
              navigator.serviceWorker.addEventListener('controllerchange', () => r(1))))""", timeout=20)
        # Connected: the worker's own request goes through tor.
        mark = self.mark()
        ok = self.sites.url("https", host, "/from-worker")
        self.assertEqual(self.via_worker(page, ok + "?n=1"), "from-worker")
        self.assertFalse(self.since(mark, "tor_blocked"))
        self.assertFalse(self.since(mark, "resource_blocked"), "the worker's request is not a tab's")
        # A local address: refused before it reaches tor.
        local = f"http://127.0.0.1:{self.direct.port}/page"
        self.assertEqual(self.via_worker(page, local), "failed")
        self.wait_since(mark, "tor_blocked", role="context", why="local address in a Tor window")
        self.assertFalse([r for r in self.rec("socks") if r["host"] == "127.0.0.1"])
        # tor stops; the next one stays at 10%: the worker's request waits.
        self.replan(hold_at=10)
        mark = self.mark()
        self.die.touch()
        self.wait_since(mark, "tor_stopped")
        self.wait_since(mark, "tor_restart", timeout=15)
        wait_for(lambda: len(self.rec("started")) >= 2 and self.rec("bootstrap")[-1]["progress"] == 10,
                 15, "the new tor at 10%")
        socks = len(self.rec("socks"))
        self.assertEqual(self.via_worker(page, ok + "?n=2"), "failed")
        self.wait_since(mark, "tor_blocked", role="context", why="tor not connected")
        self.assertEqual(len(self.rec("socks")), socks, "nothing may reach tor before it is connected")
        self.assert_nothing_direct()

    def test_after_new_identity_the_old_context_sends_nothing(self) -> None:
        # A service worker outlives its window: if the old Tor context could
        # still send, its requests would use circuits built after NEWNYM and
        # link the old identity to the new one.
        host = self.TLS_HOST
        self.sites.raw(host, "/worker.js", 200, {"Content-Type": "text/javascript"}, self.WORKER)
        self.sites.html(host, "/worker-page", "<!doctype html><title>worker</title>")
        self.sites.html(host, "/tick", "tick")
        shell = self.open_tor()
        self.wait_connected(shell)
        page = self.load(shell, self.sites.url("https", host, "/worker-page"), "worker")
        page.evaluate("""navigator.serviceWorker.register('/worker.js')
            .then(() => navigator.serviceWorker.ready)
            .then(() => navigator.serviceWorker.controller ? 1 : new Promise(r =>
              navigator.serviceWorker.addEventListener('controllerchange', () => r(1))))""", timeout=20)
        page.evaluate("navigator.serviceWorker.controller.postMessage('tick'); 1")

        def ws_attempts() -> int:
            return len([r for r in self.rec("socks") if r["host"] == host])
        # The probe works: both kinds of request reach tor while the context is current.
        socks_before = ws_attempts()
        wait_for(lambda: len(self.sites.seen(host, "/tick")) >= 3 and ws_attempts() >= socks_before + 3,
                 15, "the worker's requests through tor")
        mark = self.mark()
        shell.evaluate("document.querySelector('tf-shell').shadowRoot"
                       ".querySelector('#tor-identity').click(); 1")
        retired = self.wait_since(mark, "tor_context_retired")
        self.assertTrue(retired["proxy_changed"], retired)
        # Moved to TorFox's own placeholder port, which nothing listens on.
        placeholder = self.tf.events("context_ready", kind="tor")[0]["placeholder"]
        self.assertEqual(retired["port"], placeholder)
        self.assertTrue(self.wait_since(mark, "tor_newnym")["ok"])
        self.wait_since(mark, "tor_new_identity")
        # Requests already on their way when the context was retired may
        # still arrive; after that, nothing.
        time.sleep(1.0)
        ticks, attempts = len(self.sites.seen(host, "/tick")), ws_attempts()
        # The worker is still running, and the old context refuses it.
        blocked = len(self.since(mark, "tor_blocked", role="context", why="tor identity retired"))
        wait_for(lambda: len(self.since(mark, "tor_blocked", role="context",
                                        why="tor identity retired")) >= blocked + 5,
                 15, "the old worker's requests, refused")
        self.assertEqual(len(self.sites.seen(host, "/tick")), ticks, "the old context still sends")
        self.assertEqual(ws_attempts(), attempts, "the old context's WebSockets still reach tor")
        self.assert_nothing_direct()


class TorDnsTest(TorBase):
    """NetLog: nothing a Tor window names is looked up by TorFox in a way
    that could leave this computer; Chromium hands the name to tor.

    Tor windows' host (site.test) is not mapped by the test resolver rules
    here, so a leak would show the real name. The control: a normal window
    opening an unmapped name does start a real lookup, and the check sees it.

    Chromium's network quality estimator asks the resolver about every host
    it sees, restricted to the resolver cache and the hosts file (it wants to
    know whether a host is on the local network). Those requests never leave
    the computer; the test checks that each request for a Tor window's host
    was restricted to them."""

    HOST_RULES = "MAP other.test 127.0.0.1"
    CONTROL = "torfox-dns-control.test"

    def prepare(self, tf: TorFox) -> None:
        self.netlog = tf.user_data / "netlog.json"
        tf.args += [f"--log-net-log={self.netlog}", "--net-log-capture-mode=Everything"]

    def test_no_tor_host_is_looked_up_on_the_network(self) -> None:
        self.both("/onion", "<!doctype html><title>Onion page</title>")
        shell = self.open_tor()
        self.wait_connected(shell)
        self.load(shell, self.url("/page"), "Tor page")
        self.load(shell, f"http://{DDG}/onion", "Onion page")
        self.assertIn(("site.test", self.direct.port), {(s["host"], s["port"]) for s in self.rec("socks")})
        # Control: a normal window's unmapped name is really looked up.
        mark = self.mark()
        self.assertTrue(self.go(self.normal, f"http://{self.CONTROL}:{self.direct.port}/")["ok"])
        self.wait_since(mark, "load_error", timeout=30)
        self.tf.close()
        log = netlog.load(self.netlog)
        network = netlog.network_lookups(log)
        self.assertIn(self.CONTROL, network, "the check must be able to see real lookups")
        self.assertNotIn("site.test", network)
        self.assertFalse([h for h in network if h.endswith(".onion") or h == "~notfound"], network)
        for request in netlog.resolver_requests(log):
            if request["host"] in ("site.test", "~notfound") or request["host"].endswith(".onion"):
                self.assertIsNotNone(request["tasks"], request)
                self.assertTrue(set(request["tasks"]) <= netlog.LOCAL_TASKS, request)
        self.assert_nothing_direct()

    # Names WebRTC could look up itself: STUN and TURN servers and a remote
    # candidate given as a name, all under the domain |n|.
    RTC_NAMES = """(async () => {
        const n = %s;
        if (typeof RTCPeerConnection === "undefined") return "absent";
        const a = new RTCPeerConnection({iceServers: [
            {urls: `stun:stun.${n}:3478`},
            {urls: `turn:turn.${n}:3478?transport=tcp`, username: "u", credential: "p"}]});
        const b = new RTCPeerConnection();
        a.createDataChannel("x");
        await a.setLocalDescription(await a.createOffer());
        await b.setRemoteDescription(a.localDescription);
        await b.setLocalDescription(await b.createAnswer());
        await a.setRemoteDescription(b.localDescription);
        try {
          await a.addIceCandidate({candidate: `candidate:1 1 udp 2122260223 peer.${n} 54400 typ host`,
                                   sdpMid: "0", sdpMLineIndex: 0});
        } catch (e) {}
        await new Promise(r => setTimeout(r, 4000));
        a.close(); b.close();
        return 1;
    })()"""

    def test_webrtc_looks_up_no_name_for_tor_windows(self) -> None:
        shell = self.open_tor()
        self.wait_connected(shell)
        tor_page = self.load(shell, self.url("/page"), "Tor page")
        # Tor windows have no WebRTC (TorContextIsolationTest); the names
        # must not be looked up either way.
        self.assertIn(tor_page.evaluate(self.RTC_NAMES % json.dumps("rtc-tor.test"),
                                        user_gesture=True, timeout=20), (1, "absent"))
        self.assert_nothing_direct()
        # Control: the same script in a normal window (another page: the
        # harness finds tabs by URL).
        control = self.load(self.normal, self.url("/two", host="other.test"), "Page two")
        self.assertEqual(control.evaluate(self.RTC_NAMES % json.dumps("rtc-normal.test"),
                                          user_gesture=True, timeout=20), 1)
        self.tf.close()
        log = netlog.load(self.netlog)
        network = netlog.network_lookups(log)
        named = sorted({r["host"] for r in netlog.resolver_requests(log) if "rtc-" in r["host"]})
        print(f"\n[WebRTC names] resolver requests: {named}; network lookups: "
              f"{sorted(h for h in network if 'rtc-' in h)}", end=" ")
        self.assertTrue([h for h in network if h.endswith("rtc-normal.test")],
                        "the check must be able to see WebRTC's lookups")
        self.assertFalse([h for h in network if h.endswith("rtc-tor.test")], network)


def bundled_tor() -> pathlib.Path:
    from harness import torfox_exe
    return torfox_exe().parent / "tor" / "tor.exe"


LIVE = unittest.skipUnless(os.environ.get("TORFOX_LIVE_TESTS") == "1",
                           "uses the Tor network: set TORFOX_LIVE_TESTS=1")


class TorSuggestTest(TorBase):
    """Address-bar suggestions in Tor windows: bookmarks only, so that what
    was browsed outside Tor never shows up in a Tor window (and nothing is
    ever looked up on the network for a suggestion)."""

    SUGGEST_ROWS = ("(() => { const root = document.querySelector('tf-suggest')?.shadowRoot;"
                    " return root ? [...root.querySelectorAll('li')].length : null; })()")

    def suggest_targets(self) -> set[str]:
        return {t["targetId"] for t in self.tf.browser.targets()
                if t["url"].startswith("torfox://suggest/")}

    def test_tor_windows_suggest_bookmarks_only(self) -> None:
        # The normal window's history and bookmarks.
        self.load(self.normal, self.url("/page"), "Tor page")
        self.assertTrue(bridge_call(self.normal, "app.openBookmarks")["ok"])
        bookmarks = self.tf.page(lambda t: t["url"].startswith("torfox://bookmarks/"))
        r = bridge_call(bookmarks, "bookmarks.add",
                        {"parent": 1, "title": "Bookmarked two", "url": self.url("/two")})
        self.assertTrue(r["ok"], r)
        args = {"text": "site.test", "x": 10, "y": 40, "width": 400}
        normal = json.loads(bridge_call(self.normal, "suggest.query", args)["response"])
        found = {(i["source"], i["url"]) for i in normal["items"]}
        self.assertIn(("history", self.url("/page")), found)  # The control.
        self.assertIn(("bookmark", self.url("/two")), found)
        bridge_call(self.normal, "suggest.hide")

        shell = self.open_tor()
        before = self.suggest_targets()
        mark = self.mark()
        tor = json.loads(bridge_call(shell, "suggest.query", args)["response"])
        self.assertEqual([(i["source"], i["url"]) for i in tor["items"]], [("bookmark", self.url("/two"))])
        self.assertEqual(self.wait_since(mark, "suggest_shown")["count"], 1)
        # The Tor window's own list shows that one row.
        new = wait_for(lambda: self.suggest_targets() - before, 10, "the Tor window's list")
        page = self.tf.page(lambda t: t.get("targetId", next(iter(new))) in new)
        wait_for(lambda: page.evaluate(self.SUGGEST_ROWS) == 1, 10, "one row")
        # A history entry's text alone finds nothing in the Tor window.
        none = json.loads(bridge_call(shell, "suggest.query", {**args, "text": "Tor page"})["response"])
        self.assertEqual(none["items"], [])
        self.wait_since(mark, "suggest_hidden")


@LIVE
class RealTorTest(TorBase):
    """The bundled tor.exe, with TorFox's torrc and control session."""

    FAKE = False

    def test_bundled_tor_starts_and_is_controlled(self) -> None:
        shell = self.open_tor()
        controlled = self.tf.wait_log("tor_controlled", timeout=90)
        self.assertTrue(controlled["version"].startswith("0.4."), controlled)
        started = self.tf.events("tor_started")[-1]
        self.assertFalse(started["fake"])
        pid = started["pid"]
        self.assertTrue(in_kill_on_close_job(pid))
        # Bootstrapping needs the Tor network; report how far it got.
        deadline = time.monotonic() + 45
        best = 0
        while time.monotonic() < deadline:
            status = self.tor_status(shell) or {}
            best = max(best, status.get("progress", 0))
            if status.get("connected"):
                break
            time.sleep(1)
        print(f"\n[RealTorTest] real tor {controlled['version']}: bootstrap reached {best}%",
              file=sys.stderr)
        notices = (self.tf.user_data / "Tor" / "tor-notices.log").read_text(encoding="utf-8", errors="replace")
        self.assertIn("Bootstrapped", notices)
        self.assertNotIn("[err]", notices)
        self.tf.close()
        wait_for(lambda: not process_alive(pid), 15, "tor to exit with TorFox")

        # Release builds write no notices log: the one line that differs
        # ("Log notice stdout") must be accepted by tor as well.
        state = self.tf.user_data / "Tor"
        torrc = (state / "torrc").read_text(encoding="utf-8").splitlines()
        logs = [l for l in torrc if l.startswith("Log ")]
        self.assertEqual(len(logs), 1, logs)
        release = state / "torrc-release"
        release.write_text("\n".join("Log notice stdout" if l.startswith("Log ") else l for l in torrc) + "\n",
                           encoding="utf-8")
        for path in (state / "torrc", release):
            # tor reads its command line in the system code page too.
            args = [short_path(path) or str(path), short_path(state / "torrc-defaults") or str(state / "torrc-defaults")]
            check = subprocess.run([str(bundled_tor()), "--verify-config", "-f", args[0], "--defaults-torrc", args[1]],
                                   capture_output=True, text=True, timeout=60, cwd=bundled_tor().parent)
            self.assertEqual(check.returncode, 0, (path.name, check.stdout[-2000:]))
            self.assertIn("Configuration was valid", check.stdout, path.name)



class RealTorNonAsciiProfileTest(RealTorTest):
    """The real tor, given the 8.3 path of a profile it could not read."""

    PROFILE = "profil\u00e9-\u0442\u0435\u0441\u0442"


@LIVE
class LiveTorTest(TorBase):
    """Live (needs the Tor network): the bundled tor, bootstrapped for real.
    Skipped, not failed, when tor cannot finish bootstrapping here."""

    FAKE = False

    def test_tor_windows_reach_the_web_and_onion_services_through_tor(self) -> None:
        shell = self.open_tor()
        try:
            self.wait_connected(shell, timeout=150)
        except TimeoutError:
            self.skipTest(f"tor did not finish bootstrapping: {self.tor_status(shell)}")
        # The Tor Project's own check: is this request coming from a Tor exit?
        api = "https://check.torproject.org/api/ip"
        self.assertTrue(self.go(shell, api)["ok"])
        page = self.tf.page(lambda t: t["url"].startswith(api), 120)
        text = wait_for(lambda: (lambda b: b if b and "IsTor" in b else None)(
            page.evaluate("document.body ? document.body.innerText : ''")), 120, "check.torproject.org")
        self.assertTrue(json.loads(text)["IsTor"], text)
        # DuckDuckGo's onion service.
        onion = f"https://{DDG}/"
        self.assertTrue(self.go(shell, onion)["ok"])
        # The tab's title, as the shell shows it (attaching to a page that is
        # still loading over a slow circuit can stall the debugging protocol).
        wait_for(lambda: [t for t in shell_state(shell)["tabs"]
                          if t["url"].startswith(onion) and "DuckDuckGo" in t["title"]], 150, "DuckDuckGo onion")
        print("\n[LiveTorTest] check.torproject.org: IsTor true; DuckDuckGo onion service loaded",
              file=sys.stderr)
        self.assert_nothing_direct()


if __name__ == "__main__":
    unittest.main()
