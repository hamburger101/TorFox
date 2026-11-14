#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Local fixtures for the Stage 0 spikes.

* HTTP server on 127.0.0.1:<http-port>; every request is logged.
* Fake SOCKS5 server on 127.0.0.1:<socks-port>; every CONNECT is logged with
  its address type (domain vs IP) and then relayed to the HTTP server,
  whatever host was requested. Nothing here ever touches the real network.

Log lines are JSON objects appended to --log. Prints READY when listening.
"""
from __future__ import annotations

import argparse
import json
import select
import socket
import socketserver
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG_LOCK = threading.Lock()
LOG_PATH = None

WEBRTC_PAGE = b"""<!doctype html><meta charset=utf-8><title>webrtc</title>
<script>
(async () => {
  const cands = [];
  let done = false;
  const report = (tag) => { if (done) return; done = true;
    console.log(tag + ' ' + JSON.stringify(cands)); };
  try {
    const pc = new RTCPeerConnection({iceServers: []});
    pc.createDataChannel('x');
    pc.onicecandidate = (e) => {
      if (e.candidate) { cands.push(e.candidate.candidate); }
      else { report('ICE_RESULT'); }
    };
    await pc.setLocalDescription(await pc.createOffer());
    setTimeout(() => report('ICE_RESULT_TIMEOUT'), 5000);
  } catch (e) { console.log('ICE_ERROR ' + e); }
})();
</script><body>webrtc probe</body>"""


def log(entry: dict) -> None:
    with LOG_LOCK:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\n")


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        log({"kind": "http", "path": self.path, "host": self.headers.get("Host"),
             "client": f"{self.client_address[0]}:{self.client_address[1]}"})
        if self.path.startswith("/webrtc"):
            body = WEBRTC_PAGE
        else:
            body = f"<!doctype html><title>ok</title><body>ok {self.path}</body>".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


class SocksHandler(socketserver.BaseRequestHandler):
    http_port = 0

    def recv_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self.request.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("short read")
            buf += chunk
        return buf

    def handle(self):
        try:
            ver, nmethods = self.recv_exact(2)
            methods = self.recv_exact(nmethods)
            if ver != 5:
                log({"kind": "socks", "error": f"bad version {ver}"})
                return
            self.request.sendall(b"\x05\x00")  # no auth
            ver, cmd, _rsv, atyp = self.recv_exact(4)
            if atyp == 1:
                host = socket.inet_ntoa(self.recv_exact(4))
                atyp_name = "ipv4"
            elif atyp == 3:
                (ln,) = self.recv_exact(1)
                host = self.recv_exact(ln).decode("ascii", "replace")
                atyp_name = "domain"
            elif atyp == 4:
                host = socket.inet_ntop(socket.AF_INET6, self.recv_exact(16))
                atyp_name = "ipv6"
            else:
                log({"kind": "socks", "error": f"bad atyp {atyp}"})
                return
            (port,) = struct.unpack(">H", self.recv_exact(2))
            log({"kind": "socks", "cmd": cmd, "atyp": atyp_name, "host": host, "port": port,
                 "methods": list(methods)})
            upstream = socket.create_connection(("127.0.0.1", self.http_port), timeout=10)
            self.request.sendall(b"\x05\x00\x00\x01" + socket.inet_aton("0.0.0.0") + b"\x00\x00")
            self.relay(upstream)
        except Exception as e:  # noqa: BLE001 - fixture must log, never die silently
            log({"kind": "socks", "error": f"{type(e).__name__}: {e}"})

    def relay(self, upstream: socket.socket) -> None:
        sockets = [self.request, upstream]
        try:
            while True:
                r, _, _ = select.select(sockets, [], [], 30)
                if not r:
                    return
                for s in r:
                    data = s.recv(65536)
                    if not data:
                        return
                    (upstream if s is self.request else self.request).sendall(data)
        finally:
            upstream.close()


class ThreadingTCP(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> int:
    global LOG_PATH
    ap = argparse.ArgumentParser()
    ap.add_argument("--http-port", type=int, required=True)
    ap.add_argument("--socks-port", type=int, required=True)
    ap.add_argument("--log", required=True)
    args = ap.parse_args()
    LOG_PATH = args.log
    open(LOG_PATH, "w").close()

    httpd = ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler)
    SocksHandler.http_port = args.http_port
    socksd = ThreadingTCP(("127.0.0.1", args.socks_port), SocksHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    threading.Thread(target=socksd.serve_forever, daemon=True).start()
    print("READY", flush=True)
    try:
        sys.stdin.read()  # run until stdin closes
    finally:
        httpd.shutdown()
        socksd.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
