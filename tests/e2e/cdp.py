# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""A minimal Chrome DevTools Protocol client (Python standard library only).

It speaks just enough RFC 6455 to talk to a local DevTools endpoint: a
client-masked handshake, text frames, fragmentation, ping/pong and close.
The browser-level connection attaches to page targets in flattened mode, so
one socket drives every TorFox browser.
"""

from __future__ import annotations

import base64
import collections
import hashlib
import json
import os
import socket
import struct
import time
import urllib.parse
import urllib.request
from typing import Any, Callable

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class CdpError(Exception):
    pass


class WebSocket:
    def __init__(self, url: str, timeout: float = 15.0) -> None:
        parsed = urllib.parse.urlparse(url)
        if parsed.scheme != "ws" or parsed.hostname not in ("127.0.0.1", "localhost"):
            raise CdpError(f"refusing non-loopback DevTools endpoint: {url}")
        self._sock = socket.create_connection((parsed.hostname, parsed.port or 80), timeout)
        self._sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        path = parsed.path + (f"?{parsed.query}" if parsed.query else "")
        request = (
            f"GET {path} HTTP/1.1\r\nHost: {parsed.hostname}:{parsed.port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self._sock.sendall(request.encode())
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise CdpError("DevTools closed the connection during the handshake")
            response += chunk
        head, _, self._buffer = response.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        if " 101 " not in lines[0]:
            raise CdpError(f"websocket handshake failed: {lines[0]}")
        expected = base64.b64encode(hashlib.sha1((key + _GUID).encode()).digest()).decode()
        headers = {k.strip().lower(): v.strip() for k, _, v in (l.partition(":") for l in lines[1:])}
        if headers.get("sec-websocket-accept") != expected:
            raise CdpError("websocket handshake: bad Sec-WebSocket-Accept")

    def _recv_exact(self, n: int) -> bytes:
        while len(self._buffer) < n:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise CdpError("DevTools connection closed")
            self._buffer += chunk
        data, self._buffer = self._buffer[:n], self._buffer[n:]
        return data

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        header = bytearray([0x80 | opcode])
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack("!H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", n)
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self._sock.sendall(bytes(header) + masked)

    def send_text(self, text: str) -> None:
        self._send_frame(0x1, text.encode("utf-8"))

    def recv_text(self) -> str:
        message = b""
        while True:
            b0, b1 = self._recv_exact(2)
            fin, opcode = b0 & 0x80, b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                (n,) = struct.unpack("!H", self._recv_exact(2))
            elif n == 127:
                (n,) = struct.unpack("!Q", self._recv_exact(8))
            if b1 & 0x80:
                raise CdpError("server frames must not be masked")
            payload = self._recv_exact(n)
            if opcode == 0x9:  # ping
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:  # pong
                continue
            if opcode == 0x8:
                raise CdpError("DevTools closed the connection")
            message += payload
            if fin:
                return message.decode("utf-8")

    def settimeout(self, timeout: float | None) -> None:
        self._sock.settimeout(timeout)

    def close(self) -> None:
        try:
            self._send_frame(0x8, b"")
        except OSError:
            pass
        self._sock.close()


class Session:
    """A flattened CDP session attached to one target."""

    def __init__(self, browser: "Browser", session_id: str, target_id: str) -> None:
        self.browser = browser
        self.session_id = session_id
        self.target_id = target_id

    def send(self, method: str, params: dict | None = None, timeout: float = 15.0) -> dict:
        return self.browser.send(method, params, session_id=self.session_id, timeout=timeout)

    def evaluate(self, expression: str, *, await_promise: bool = True,
                 user_gesture: bool = False, timeout: float = 15.0) -> Any:
        result = self.send("Runtime.evaluate", {
            "expression": expression,
            "awaitPromise": await_promise,
            "returnByValue": True,
            "userGesture": user_gesture,
            # DevTools would otherwise exempt the evaluation from the page's
            # CSP eval restrictions; tests must see the page's real policy.
            "allowUnsafeEvalBlockedByCSP": False,
        }, timeout=timeout)
        if "exceptionDetails" in result:
            details = result["exceptionDetails"]
            text = details.get("exception", {}).get("description") or details.get("text")
            raise CdpError(f"page exception: {text}")
        return result.get("result", {}).get("value")


class Browser:
    """A browser-level DevTools connection."""

    def __init__(self, port: int, timeout: float = 15.0) -> None:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/version", timeout=timeout) as r:
            info = json.load(r)
        self.ws = WebSocket(info["webSocketDebuggerUrl"], timeout)
        self._next_id = 1
        self._events: collections.deque[dict] = collections.deque()

    def close(self) -> None:
        self.ws.close()

    def send(self, method: str, params: dict | None = None, *, session_id: str | None = None,
             timeout: float = 15.0) -> dict:
        msg_id = self._next_id
        self._next_id += 1
        msg: dict[str, Any] = {"id": msg_id, "method": method, "params": params or {}}
        if session_id:
            msg["sessionId"] = session_id
        self.ws.send_text(json.dumps(msg))
        deadline = time.monotonic() + timeout
        while True:
            reply = self._read(deadline, f"reply to {method}")
            if reply.get("id") == msg_id:
                if "error" in reply:
                    raise CdpError(f"{method}: {reply['error'].get('message')}")
                return reply.get("result", {})
            if "method" in reply:
                self._events.append(reply)

    def _read(self, deadline: float, what: str) -> dict:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"timed out waiting for {what}")
        self.ws.settimeout(remaining)
        try:
            return json.loads(self.ws.recv_text())
        except socket.timeout as e:
            raise TimeoutError(f"timed out waiting for {what}") from e

    def wait_event(self, method: str, predicate: Callable[[dict], bool] | None = None,
                   timeout: float = 15.0) -> dict:
        for ev in list(self._events):
            if ev.get("method") == method and (predicate is None or predicate(ev)):
                self._events.remove(ev)
                return ev
        deadline = time.monotonic() + timeout
        while True:
            ev = self._read(deadline, method)
            if ev.get("method") == method and (predicate is None or predicate(ev)):
                return ev
            if "method" in ev:
                self._events.append(ev)

    def pump(self, seconds: float) -> None:
        """Reads incoming events for |seconds| without waiting for anything."""
        deadline = time.monotonic() + seconds
        try:
            while True:
                ev = self._read(deadline, "events")
                if "method" in ev:
                    self._events.append(ev)
        except TimeoutError:
            pass

    def take_events(self, method: str, session_id: str | None = None) -> list[dict]:
        taken = [e for e in self._events if e.get("method") == method and
                 (session_id is None or e.get("sessionId") == session_id)]
        for e in taken:
            self._events.remove(e)
        return taken

    def targets(self) -> list[dict]:
        return [t for t in self.send("Target.getTargets")["targetInfos"] if t["type"] == "page"]

    def attach(self, target_id: str) -> Session:
        sid = self.send("Target.attachToTarget", {"targetId": target_id, "flatten": True})
        return Session(self, sid["sessionId"], target_id)

    def wait_target(self, predicate: Callable[[dict], bool], timeout: float = 15.0) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            for t in self.targets():
                if predicate(t):
                    return t
            if time.monotonic() > deadline:
                raise TimeoutError("no matching target: " + json.dumps(self.targets()))
            time.sleep(0.1)
