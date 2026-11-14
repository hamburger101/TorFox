# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""A scripted stand-in for tor, for TorFox's end-to-end tests.

TorFox (test builds) runs it instead of the bundled tor when started with
--torfox-test-tor-command. It takes tor's own arguments (-f <torrc>
--defaults-torrc <file>) after its own, reads the torrc TorFox wrote, and
plays tor's part:

  * the control port: PROTOCOLINFO, SAFECOOKIE authentication (the real
    HMAC-SHA256 exchange, with the cookie file it writes), TAKEOWNERSHIP,
    SETEVENTS, GETINFO, SIGNAL NEWNYM, and STATUS_CLIENT bootstrap events;
  * the SOCKS port: SOCKS5 without authentication, as Chromium uses it.
    Every request is recorded (address type, host, port) and relayed to the
    test's "Tor side" web server, whatever host it names;
  * exits when its owning controller connection closes or the owning process
    dies, as tor does with TAKEOWNERSHIP and __OwningControllerProcess.

Everything it sees is appended to the --record file as JSON lines. The --plan
file (JSON) scripts the bootstrap: {"steps": [[delay, progress, tag,
summary], ...], "hold_at": n, "release_file": path, "die_file": path,
"exit_at_start": code, "relay_port": port, "ignore_owner": bool}. The plan
is read each time it starts, so a test can change it for the next start.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import hmac
import json
import os
import pathlib
import secrets
import socket
import sys
import threading
import time

SERVER_KEY = b"Tor safe cookie authentication server-to-controller hash"
CLIENT_KEY = b"Tor safe cookie authentication controller-to-server hash"

record_lock = threading.Lock()
record_path: pathlib.Path | None = None


def record(event: str, **fields) -> None:
    if not record_path:
        return
    line = json.dumps({"ev": event, "t": time.time(), "pid": os.getpid(), **fields})
    with record_lock:
        with record_path.open("a", encoding="utf-8") as f:
            f.write(line + "\n")


def quit_now(code: int, why: str) -> None:
    record("exit", code=code, why=why)
    os._exit(code)


def parse_torrc(text: str) -> dict[str, list[str]]:
    options: dict[str, list[str]] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, _, value = line.partition(" ")
        options.setdefault(key, []).append(value.strip())
    return options


def unquote(value: str) -> str:
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        out, i = [], 1
        while i < len(value) - 1:
            c = value[i]
            if c == "\\" and i + 1 < len(value) - 1:
                i += 1
                c = value[i]
            out.append(c)
            i += 1
        return "".join(out)
    return value


def process_alive(pid: int) -> bool:
    if sys.platform == "win32":
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        handle = kernel32.OpenProcess(0x00100000 | 0x1000, False, pid)  # SYNCHRONIZE | QUERY_LIMITED
        if not handle:
            return False
        try:
            return kernel32.WaitForSingleObject(handle, 0) == 0x102  # WAIT_TIMEOUT: still running
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


class FakeTor:
    def __init__(self, plan: dict, options: dict[str, list[str]]) -> None:
        self.plan = plan
        self.options = options
        self.cookie = secrets.token_bytes(32)
        self.progress = 0
        self.tag = "starting"
        self.summary = "Starting"
        self.event_conns: list[socket.socket] = []
        self.lock = threading.Lock()
        self.owner_conn: socket.socket | None = None
        self.socks = socket.create_server(("127.0.0.1", 0))
        self.control = socket.create_server(("127.0.0.1", 0))
        self.socks_port = self.socks.getsockname()[1]
        self.relay_port = int(plan.get("relay_port", 0))

    # ----- Startup -----
    def start(self) -> None:
        data_dir = pathlib.Path(unquote(self.options["DataDirectory"][0]))
        data_dir.mkdir(parents=True, exist_ok=True)
        cookie_file = pathlib.Path(unquote(self.options["CookieAuthFile"][0]))
        cookie_file.write_bytes(self.cookie)
        port_file = pathlib.Path(unquote(self.options["ControlPortWriteToFile"][0]))
        port_file.write_text(f"PORT=127.0.0.1:{self.control.getsockname()[1]}\n")
        record("listening", socks=self.socks_port, control=self.control.getsockname()[1])
        owner = self.options.get("__OwningControllerProcess")
        # "ignore_owner": neither the owning process nor the owning control
        # connection ends this tor, so only TorFox's job object can.
        if owner and not self.plan.get("ignore_owner"):
            threading.Thread(target=self.watch_owner, args=(int(owner[0]),), daemon=True).start()
        threading.Thread(target=self.accept_socks, daemon=True).start()
        threading.Thread(target=self.watch_die_file, daemon=True).start()
        self.accept_control()

    def watch_owner(self, pid: int) -> None:
        while True:
            if not process_alive(pid):
                quit_now(0, "owning process exited")
            time.sleep(0.3)

    def watch_die_file(self) -> None:
        die = self.plan.get("die_file")
        if not die:
            return
        path = pathlib.Path(die)
        while True:
            if path.exists():
                try:
                    path.unlink()
                except OSError:
                    pass
                quit_now(1, "die_file")
            time.sleep(0.05)

    # ----- Control port -----
    def accept_control(self) -> None:
        while True:
            conn, _ = self.control.accept()
            threading.Thread(target=self.serve_control, args=(conn,), daemon=True).start()

    def send(self, conn: socket.socket, text: str) -> None:
        with self.lock:
            conn.sendall(text.encode("utf-8"))

    def serve_control(self, conn: socket.socket) -> None:
        client_nonce = server_nonce = b""
        authenticated = False
        buffer = b""
        try:
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buffer += chunk
                while b"\n" in buffer:
                    raw, buffer = buffer.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    word = line.split(" ", 1)[0].upper()
                    # Never record secrets: command names and their public parts only.
                    record("control", command=word if word in ("AUTHENTICATE", "AUTHCHALLENGE") else line)
                    if word == "PROTOCOLINFO":
                        cookie_file = unquote(self.options["CookieAuthFile"][0]).replace("\\", "\\\\")
                        self.send(conn, "250-PROTOCOLINFO 1\r\n"
                                        f"250-AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE=\"{cookie_file}\"\r\n"
                                        "250-VERSION Tor=\"0.4.9.99-fake\"\r\n250 OK\r\n")
                    elif word == "AUTHCHALLENGE":
                        parts = line.split(" ")
                        if len(parts) != 3 or parts[1] != "SAFECOOKIE":
                            self.send(conn, "513 Invalid argument\r\n")
                            continue
                        client_nonce = bytes.fromhex(parts[2])
                        server_nonce = secrets.token_bytes(32)
                        server_hash = hmac.new(SERVER_KEY, self.cookie + client_nonce + server_nonce,
                                               hashlib.sha256).hexdigest().upper()
                        self.send(conn, f"250 AUTHCHALLENGE SERVERHASH={server_hash} "
                                        f"SERVERNONCE={server_nonce.hex().upper()}\r\n")
                    elif word == "AUTHENTICATE":
                        expected = hmac.new(CLIENT_KEY, self.cookie + client_nonce + server_nonce,
                                            hashlib.sha256).hexdigest().upper()
                        given = line.split(" ", 1)[1].strip().upper() if " " in line else ""
                        if client_nonce and hmac.compare_digest(given, expected):
                            authenticated = True
                            record("authenticated")
                            self.send(conn, "250 OK\r\n")
                        else:
                            record("auth_failed")
                            self.send(conn, "515 Authentication failed\r\n")
                            conn.close()
                            return
                    elif not authenticated:
                        self.send(conn, "514 Authentication required.\r\n")
                        conn.close()
                        return
                    elif word == "TAKEOWNERSHIP":
                        self.owner_conn = conn
                        self.send(conn, "250 OK\r\n")
                    elif line == "SETEVENTS STATUS_CLIENT":
                        with self.lock:
                            self.event_conns.append(conn)
                        self.send(conn, "250 OK\r\n")
                        threading.Thread(target=self.bootstrap, daemon=True).start()
                    elif line == "GETINFO net/listeners/socks":
                        self.send(conn, f"250-net/listeners/socks=\"127.0.0.1:{self.socks_port}\"\r\n250 OK\r\n")
                    elif line == "GETINFO status/bootstrap-phase":
                        self.send(conn, "250-status/bootstrap-phase=" + self.phase() + "\r\n250 OK\r\n")
                    elif line == "SIGNAL NEWNYM":
                        record("newnym")
                        self.send(conn, "250 OK\r\n")
                    else:
                        self.send(conn, "510 Unrecognized command\r\n")
        except OSError:
            pass
        if conn is self.owner_conn and not self.plan.get("ignore_owner"):
            quit_now(0, "owning controller closed")

    def phase(self) -> str:
        return (f"NOTICE BOOTSTRAP PROGRESS={self.progress} TAG={self.tag} "
                f"SUMMARY=\"{self.summary}\"")

    def bootstrap(self) -> None:
        steps = self.plan.get("steps", [[0.2, 10, "conn_done", "Connected to a relay"],
                                        [0.2, 50, "loading_descriptors", "Loading relay descriptors"],
                                        [0.2, 100, "done", "Done"]])
        hold_at = self.plan.get("hold_at")
        release = self.plan.get("release_file")
        for delay, progress, tag, summary in steps:
            if hold_at is not None and progress > hold_at and release:
                while not pathlib.Path(release).exists():
                    time.sleep(0.05)
                record("released")
            time.sleep(delay)
            self.progress, self.tag, self.summary = progress, tag, summary
            event = f"650 STATUS_CLIENT NOTICE BOOTSTRAP PROGRESS={progress} TAG={tag} SUMMARY=\"{summary}\"\r\n"
            with self.lock:
                conns = list(self.event_conns)
            for c in conns:
                try:
                    self.send(c, event)
                except OSError:
                    pass
            record("bootstrap", progress=progress)

    # ----- SOCKS port -----
    def accept_socks(self) -> None:
        while True:
            conn, _ = self.socks.accept()
            threading.Thread(target=self.serve_socks, args=(conn,), daemon=True).start()

    @staticmethod
    def read_exact(conn: socket.socket, n: int) -> bytes:
        data = b""
        while len(data) < n:
            chunk = conn.recv(n - len(data))
            if not chunk:
                raise OSError("closed")
            data += chunk
        return data

    def serve_socks(self, conn: socket.socket) -> None:
        try:
            ver, nmethods = self.read_exact(conn, 2)
            methods = self.read_exact(conn, nmethods)
            if ver != 5 or 0 not in methods:
                record("socks_refused", why="greeting", version=ver)
                conn.sendall(b"\x05\xff")
                return
            conn.sendall(b"\x05\x00")
            ver, cmd, _, atyp = self.read_exact(conn, 4)
            if atyp == 1:
                host = socket.inet_ntoa(self.read_exact(conn, 4))
            elif atyp == 3:
                host = self.read_exact(conn, self.read_exact(conn, 1)[0]).decode("ascii", "replace")
            elif atyp == 4:
                host = socket.inet_ntop(socket.AF_INET6, self.read_exact(conn, 16))
            else:
                host = "?"
            port = int.from_bytes(self.read_exact(conn, 2), "big")
            record("socks", atyp=atyp, host=host, port=port, cmd=cmd, progress=self.progress)
            if cmd != 1 or not self.relay_port or self.progress < 100:
                # Not bootstrapped (or no relay): tor's "general failure".
                conn.sendall(b"\x05\x01\x00\x01\x00\x00\x00\x00\x00\x00")
                return
            upstream = socket.create_connection(("127.0.0.1", self.relay_port), timeout=10)
            conn.sendall(b"\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00")
            threading.Thread(target=self.pipe, args=(upstream, conn), daemon=True).start()
            self.pipe(conn, upstream)
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass

    @staticmethod
    def pipe(src: socket.socket, dst: socket.socket) -> None:
        try:
            while True:
                data = src.recv(65536)
                if not data:
                    break
                dst.sendall(data)
        except OSError:
            pass
        finally:
            for s in (src, dst):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass


def main() -> int:
    global record_path
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", required=True)
    ap.add_argument("--record", required=True)
    ap.add_argument("-f", dest="torrc", required=True)
    ap.add_argument("--defaults-torrc", dest="defaults", required=True)
    args = ap.parse_args()
    record_path = pathlib.Path(args.record)
    plan = json.loads(pathlib.Path(args.plan).read_text(encoding="utf-8"))
    torrc = pathlib.Path(args.torrc).read_text(encoding="utf-8")
    defaults = pathlib.Path(args.defaults).read_text(encoding="utf-8")
    record("started", torrc=torrc, defaults=defaults, cwd=os.getcwd(), argv=sys.argv[1:])
    if "exit_at_start" in plan:
        quit_now(int(plan["exit_at_start"]), "exit_at_start")
    FakeTor(plan, parse_torrc(torrc)).start()
    return 0


if __name__ == "__main__":
    sys.exit(main())
