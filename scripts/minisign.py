#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""minisign signatures in pure Python (no third-party packages).

Used by scripts/fetch_deps.py to check upstream signatures (libsodium and
minisign releases) before a SHA-256 is pinned, and by the updater tests to
sign test releases with throwaway keys. TorFox itself verifies with
libsodium (src/core/update/minisign.cc); the unit tests check both against
signatures made by the real minisign tool.

Format (https://jedisct1.github.io/minisign/):
  public key  base64("Ed" | key id (8) | Ed25519 public key (32))
  signature   untrusted comment: <text>
              base64("ED" | key id (8) | Ed25519 signature (64))   "ED": over BLAKE2b-512(file)
              trusted comment: <text>                              ("Ed": over the file itself)
              base64(Ed25519 signature (64) over signature | trusted comment)

Ed25519 is the reference algorithm of RFC 8032, section 6 (strict: S < L, and
points must decode). It is not constant-time: never sign with a real key here.

Usage:
  python scripts/minisign.py verify -P <public key> -m <file> [-x <file>.minisig]
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import sys
from pathlib import Path

# ----- Ed25519 (RFC 8032, section 6) ---------------------------------------------

_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_I = pow(2, (_P - 1) // 4, _P)


def _inv(x: int) -> int:
    return pow(x, _P - 2, _P)


def _recover_x(y: int, sign: int) -> int | None:
    if y >= _P:
        return None
    x2 = (y * y - 1) * _inv(_D * y * y + 1) % _P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P != 0:
        x = x * _I % _P
    if (x * x - x2) % _P != 0:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


_GY = 4 * _inv(5) % _P
_GX = _recover_x(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % _P)


def _add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % _P
    b = (p[1] + p[0]) * (q[1] + q[0]) % _P
    c = 2 * p[3] * q[3] * _D % _P
    d = 2 * p[2] * q[2] % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(s: int, p):
    q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _equal(p, q) -> bool:
    return (p[0] * q[2] - q[0] * p[2]) % _P == 0 and (p[1] * q[2] - q[1] * p[2]) % _P == 0


def _compress(p) -> bytes:
    zinv = _inv(p[2])
    x, y = p[0] * zinv % _P, p[1] * zinv % _P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(s: bytes):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % _P)


def _hash_mod_l(data: bytes) -> int:
    return int.from_bytes(hashlib.sha512(data).digest(), "little") % _L


def _expand(seed: bytes) -> tuple[int, bytes]:
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def ed25519_public_key(seed: bytes) -> bytes:
    a, _ = _expand(seed)
    return _compress(_mul(a, _G))


def ed25519_sign(seed: bytes, message: bytes) -> bytes:
    a, prefix = _expand(seed)
    public = _compress(_mul(a, _G))
    r = _hash_mod_l(prefix + message)
    rs = _compress(_mul(r, _G))
    h = _hash_mod_l(rs + public + message)
    return rs + int.to_bytes((r + h * a) % _L, 32, "little")


def ed25519_verify(public: bytes, message: bytes, signature: bytes) -> bool:
    if len(public) != 32 or len(signature) != 64:
        return False
    a = _decompress(public)
    r = _decompress(signature[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= _L:
        return False
    h = _hash_mod_l(signature[:32] + public + message)
    return _equal(_mul(s, _G), _add(r, _mul(h, a)))


# ----- minisign --------------------------------------------------------------------

class MinisignError(Exception):
    pass


def _b64(text: str, length: int, what: str) -> bytes:
    try:
        raw = base64.b64decode(text.strip(), validate=True)
    except ValueError as e:
        raise MinisignError(f"{what} is not base64") from e
    if len(raw) != length:
        raise MinisignError(f"{what} has {len(raw)} bytes, not {length}")
    return raw


def parse_public_key(text: str) -> tuple[bytes, bytes]:
    """Returns (key id, Ed25519 public key) from "RW..." or a .pub file's text."""
    lines = [line for line in text.strip().splitlines() if line.strip()]
    raw = _b64(lines[-1], 42, "public key")
    if raw[:2] != b"Ed":
        raise MinisignError("not an Ed25519 minisign public key")
    return raw[2:10], raw[10:]


def verify(public_key: str, signature_text: str, data: bytes, require_prehashed: bool = False) -> str:
    """Checks |signature_text| (a .minisig file) over |data|. Returns the
    trusted comment; raises MinisignError if anything does not verify."""
    key_id, public = parse_public_key(public_key)
    lines = signature_text.replace("\r\n", "\n").split("\n")
    if len(lines) < 4 or not lines[0].startswith("untrusted comment: "):
        raise MinisignError("not a minisign signature")
    sig = _b64(lines[1], 74, "signature")
    algorithm, sig_key_id, signature = sig[:2], sig[2:10], sig[10:]
    if algorithm not in (b"Ed", b"ED") or (require_prehashed and algorithm != b"ED"):
        raise MinisignError(f"unsupported signature algorithm {algorithm!r}")
    if sig_key_id != key_id:
        raise MinisignError("signed with a different key")
    message = hashlib.blake2b(data, digest_size=64).digest() if algorithm == b"ED" else data
    if not ed25519_verify(public, message, signature):
        raise MinisignError("signature does not verify")
    if not lines[2].startswith("trusted comment: "):
        raise MinisignError("no trusted comment")
    trusted = lines[2][len("trusted comment: "):]
    global_sig = _b64(lines[3], 64, "global signature")
    if not ed25519_verify(public, signature + trusted.encode("utf-8"), global_sig):
        raise MinisignError("trusted comment signature does not verify")
    return trusted


def public_key_text(seed: bytes, key_id: bytes) -> str:
    return base64.b64encode(b"Ed" + key_id + ed25519_public_key(seed)).decode("ascii")


def sign(seed: bytes, key_id: bytes, data: bytes, trusted_comment: str,
         untrusted_comment: str = "signature from a TorFox test key") -> str:
    """A prehashed ("ED") minisign signature. Test keys only (not constant-time)."""
    signature = ed25519_sign(seed, hashlib.blake2b(data, digest_size=64).digest())
    global_sig = ed25519_sign(seed, signature + trusted_comment.encode("utf-8"))
    return (f"untrusted comment: {untrusted_comment}\n"
            + base64.b64encode(b"ED" + key_id + signature).decode("ascii") + "\n"
            + f"trusted comment: {trusted_comment}\n"
            + base64.b64encode(global_sig).decode("ascii") + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("verify")
    v.add_argument("-P", required=True, help="public key (RW...)")
    v.add_argument("-m", required=True, help="signed file")
    v.add_argument("-x", help="signature file (default: <file>.minisig)")
    args = parser.parse_args()
    data = Path(args.m).read_bytes()
    sig = Path(args.x or args.m + ".minisig").read_text(encoding="utf-8")
    try:
        trusted = verify(args.P, sig, data)
    except MinisignError as e:
        print(f"FAILED: {e}", file=sys.stderr)
        return 1
    print(f"Signature and comment signature verified\nTrusted comment: {trusted}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
