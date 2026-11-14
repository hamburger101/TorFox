# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Local test sites over http and https, routed by host name.

Tests map real-looking host names (site-a.example.com, www.google-analytics.com)
to 127.0.0.1 with TorFox's test resolver rules, which end in "MAP * ~NOTFOUND":
nothing reaches the internet. One plain HTTP server and one TLS server answer
for every host; routes are keyed by (host, path), and every request is
recorded with its host.

The TLS certificate is generated per run with the openssl command-line tool,
for the host names given. TorFox (test builds) trusts it by its public key
(--ignore-certificate-errors-spki-list); hosts listed in |refuse_tls| fail
the TLS handshake, like a site with no https at all.
"""

from __future__ import annotations

import base64
import hashlib
import http.server
import pathlib
import shutil
import ssl
import subprocess
import tempfile
import threading
from dataclasses import dataclass, field


def _openssl() -> str:
    exe = shutil.which("openssl")
    if not exe:
        raise RuntimeError("the openssl command-line tool is needed for the https test sites")
    return exe


@dataclass
class Request:
    scheme: str
    host: str
    method: str
    path: str
    headers: dict[str, str] = field(default_factory=dict)


def make_certificate(directory: pathlib.Path, hosts: list[str]) -> tuple[pathlib.Path, pathlib.Path, str]:
    """A self-signed certificate for |hosts|: (cert, key, base64 SHA-256 of its public key)."""
    cert, key = directory / "cert.pem", directory / "key.pem"
    san = ",".join(f"DNS:{h}" for h in hosts)
    subprocess.run([_openssl(), "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                    "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "2", "-subj", "/CN=torfox-test",
                    "-addext", f"subjectAltName={san}"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pub = subprocess.run([_openssl(), "x509", "-in", str(cert), "-pubkey", "-noout"],
                         check=True, capture_output=True).stdout
    der = subprocess.run([_openssl(), "pkey", "-pubin", "-outform", "der"], input=pub,
                         check=True, capture_output=True).stdout
    return cert, key, base64.b64encode(hashlib.sha256(der).digest()).decode()


class Sites:
    def __init__(self, hosts: list[str], refuse_tls: tuple[str, ...] = ()) -> None:
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="torfox-e2e-sites-"))
        self.cert, self.key, self.spki = make_certificate(self.dir, hosts)
        self.routes: dict[tuple[str, str], tuple[int, dict[str, str], bytes]] = {}
        self.requests: list[Request] = []
        self.refuse_tls = set(refuse_tls)
        self.lock = threading.Lock()
        sites = self

        class Handler(http.server.BaseHTTPRequestHandler):
            scheme = "http"

            def _serve(self) -> None:
                host = (self.headers.get("Host") or "").split(":")[0].lower()
                path = self.path.split("?", 1)[0]
                with sites.lock:
                    sites.requests.append(Request(self.scheme, host, self.command, self.path,
                                                  {k.lower(): v for k, v in self.headers.items()}))
                status, headers, body = sites.routes.get((host, path)) or \
                    sites.routes.get(("*", path)) or (404, {"Content-Type": "text/plain"}, b"not found")
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

        class TlsHandler(Handler):
            scheme = "https"

        self.http = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.https = http.server.ThreadingHTTPServer(("127.0.0.1", 0), TlsHandler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.cert, self.key)

        def sni(sock, name, ctx):
            if name and name.lower() in sites.refuse_tls:
                return ssl.ALERT_DESCRIPTION_HANDSHAKE_FAILURE
            return None
        context.sni_callback = sni
        self.https.socket = context.wrap_socket(self.https.socket, server_side=True)
        self.http_port = self.http.server_address[1]
        self.https_port = self.https.server_address[1]
        self.threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in (self.http, self.https)]

    def __enter__(self) -> "Sites":
        for t in self.threads:
            t.start()
        return self

    def __exit__(self, *exc) -> None:
        for s in (self.http, self.https):
            s.shutdown()
            s.server_close()
        shutil.rmtree(self.dir, ignore_errors=True)

    # Routes ----------------------------------------------------------------------
    def html(self, host: str, path: str, body: str, headers: dict[str, str] | None = None) -> None:
        self.routes[(host, path)] = (200, {"Content-Type": "text/html; charset=utf-8", **(headers or {})},
                                     body.encode("utf-8"))

    def raw(self, host: str, path: str, status: int, headers: dict[str, str], body: bytes = b"") -> None:
        self.routes[(host, path)] = (status, headers, body)

    def url(self, scheme: str, host: str, path: str = "/") -> str:
        port = self.http_port if scheme == "http" else self.https_port
        return f"{scheme}://{host}:{port}{path}"

    def seen(self, host: str | None = None, path: str | None = None, scheme: str | None = None) -> list[Request]:
        with self.lock:
            return [r for r in self.requests
                    if (host is None or r.host == host) and (path is None or r.path.split("?", 1)[0] == path)
                    and (scheme is None or r.scheme == scheme)]

    def host_rules(self, hosts: list[str]) -> str:
        """Resolver rules: |hosts| to 127.0.0.1, everything else unresolvable."""
        return ", ".join([f"MAP {h} 127.0.0.1" for h in hosts] + ["MAP * ~NOTFOUND"])
