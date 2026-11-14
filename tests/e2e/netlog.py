# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Reads Chromium NetLog files (--log-net-log) and lists the network
destinations they mention: URLs requested, hosts resolved, sockets opened."""

from __future__ import annotations

import ipaddress
import json
import pathlib
import re
import urllib.parse

LOCAL_SCHEMES = {"torfox", "data", "blob", "about", "chrome-error", "devtools"}


def load(path: pathlib.Path) -> dict:
    text = path.read_text(encoding="utf-8", errors="replace").rstrip()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        # A log cut short: close the events array and the object.
        text = text.rstrip(",\n ")
        return json.loads(text + "]}")


def _host_of(value: str) -> str | None:
    value = value.strip()
    if "://" in value:
        parsed = urllib.parse.urlsplit(value)
        if parsed.scheme in LOCAL_SCHEMES:
            return None
        return parsed.hostname
    # "host:port" or "[v6]:port" or a bare address.
    m = re.match(r"^\[([0-9a-fA-F:.]+)\](?::\d+)?$", value)
    if m:
        return m.group(1)
    if value.count(":") == 1:
        return value.split(":")[0]
    return value or None


def is_local(host: str) -> bool:
    host = host.strip("[]").lower().rstrip(".")
    if host in ("localhost",) or host.endswith(".localhost"):
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def _names(log: dict) -> tuple[dict[int, str], dict[int, str]]:
    constants = log.get("constants", {})
    types = {v: k for k, v in constants.get("logEventTypes", {}).items()}
    sources = {v: k for k, v in constants.get("logSourceType", {}).items()}
    return types, sources


def _dependents(log: dict, roots: set[int]) -> set[int]:
    """|roots| plus every NetLog source they started, transitively (via the
    "source_dependency" parameter)."""
    children: dict[int, set[int]] = {}
    for ev in log.get("events", []):
        dep = (ev.get("params") or {}).get("source_dependency")
        if isinstance(dep, dict) and "id" in dep:
            children.setdefault(ev["source"]["id"], set()).add(dep["id"])
    seen, todo = set(), list(roots)
    while todo:
        sid = todo.pop()
        if sid in seen:
            continue
        seen.add(sid)
        todo.extend(children.get(sid, ()))
    return seen


def proxy_autodiscovery_sources(log: dict) -> set[int]:
    """NetLog sources that belong to proxy auto-discovery (WPAD over DHCP and
    DNS, and everything those lookups started)."""
    types, sources = _names(log)
    roots = set()
    for ev in log.get("events", []):
        if sources.get(ev["source"]["type"]) == "PAC_FILE_DECIDER":
            roots.add(ev["source"]["id"])
        host = (ev.get("params") or {}).get("host")
        if (types.get(ev.get("type"), "").startswith("HOST_RESOLVER") and isinstance(host, str)
                and host.split(":")[0].lower() == "wpad"):
            roots.add(ev["source"]["id"])
    return _dependents(log, roots) if roots else set()


def system_autodetect_contexts(log: dict) -> int:
    """How many network contexts were configured from system proxy settings
    that ask for auto-detection (one PROXY_CONFIG_CHANGED source each)."""
    types, _ = _names(log)
    sources = set()
    for ev in log.get("events", []):
        if types.get(ev.get("type")) == "PROXY_CONFIG_CHANGED":
            cfg = (ev.get("params") or {}).get("new_config") or {}
            if cfg.get("auto_detect") and cfg.get("from_system"):
                sources.add(ev["source"]["id"])
    return len(sources)


# Event types that name an address without sending anything to it.
_NOT_TRAFFIC = {"UDP_LOCAL_ADDRESS", "TCP_ACCEPT"}


def destinations(log: dict, exclude_sources: set[int] | None = None) -> dict[str, set[str]]:
    """Maps each non-loopback destination to the NetLog event types naming it.

    Ignored: addresses that are our own (UDP_LOCAL_ADDRESS), and UDP sockets
    that never sent a byte: a UDP "connect" only picks a route (Chromium's
    IPv6 reachability probe does this) and puts nothing on the wire."""
    types, sources = _names(log)
    exclude = exclude_sources or set()
    udp_sent = {ev["source"]["id"] for ev in log.get("events", [])
                if types.get(ev.get("type")) == "UDP_BYTES_SENT"}
    found: dict[str, set[str]] = {}
    keys = ("url", "host", "address", "remote_address", "original_url", "location", "endpoint",
            "server", "proxy_server", "hostname")
    for ev in log.get("events", []):
        sid = ev["source"]["id"]
        name = types.get(ev.get("type"), str(ev.get("type")))
        if sid in exclude or name in _NOT_TRAFFIC:
            continue
        if sources.get(ev["source"]["type"]) == "UDP_CLIENT_SOCKET" and sid not in udp_sent:
            continue
        params = ev.get("params") or {}
        for key in keys:
            value = params.get(key)
            if isinstance(value, dict):
                value = value.get("host") or value.get("url")
            if not isinstance(value, str) or not value:
                continue
            host = _host_of(value)
            if host and not is_local(host):
                found.setdefault(host.lower(), set()).add(name)
        addresses = params.get("address_list")
        if isinstance(addresses, list):
            for a in addresses:
                host = _host_of(str(a))
                if host and not is_local(host):
                    found.setdefault(host.lower(), set()).add(name)
    return found


def has_event(log: dict, name: str) -> bool:
    types, _ = _names(log)
    return any(types.get(ev.get("type")) == name for ev in log.get("events", []))


def requested_urls(log: dict) -> list[str]:
    """URLs the network stack started a request for (URL_REQUEST_START_JOB)."""
    types, _ = _names(log)
    return [ev["params"]["url"] for ev in log.get("events", [])
            if types.get(ev.get("type")) == "URL_REQUEST_START_JOB" and (ev.get("params") or {}).get("url")]


def connected_addresses(log: dict, host: str) -> list[str]:
    """Socket connect attempts for requests to |host| (by NetLog source chain)."""
    types, _ = _names(log)
    roots = {ev["source"]["id"] for ev in log.get("events", [])
             if host in str((ev.get("params") or {}).get("url", ""))}
    related = _dependents(log, roots)
    out = []
    for ev in log.get("events", []):
        name = types.get(ev.get("type"), "")
        if ev["source"]["id"] in related and name in ("TCP_CONNECT_ATTEMPT", "UDP_CONNECT", "SOCKET_CONNECT"):
            out.append(str((ev.get("params") or {}).get("address")))
    return out


def resolved_hosts(log: dict) -> set[str]:
    """Hostnames that went through the host resolver."""
    types = {v: k for k, v in log.get("constants", {}).get("logEventTypes", {}).items()}
    hosts = set()
    for ev in log.get("events", []):
        name = types.get(ev.get("type"), "")
        if not name.startswith("HOST_RESOLVER"):
            continue
        params = ev.get("params") or {}
        for key in ("host", "hostname"):
            value = params.get(key)
            if isinstance(value, dict):
                value = value.get("host")
            if isinstance(value, str) and value:
                hosts.add((_host_of(value) or value).lower())
    return hosts
