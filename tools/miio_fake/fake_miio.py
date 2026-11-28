#!/usr/bin/env python3
"""Minimal fake Xiaomi miIO UDP endpoint for Homewind P1.

Answers HELLO (32-byte) on UDP 54321. Optional encrypted miIO/JSON for
miIO.info and set_properties when --token is set (AES-128-CBC + MD5 checksum).

Examples:
  python3 fake_miio.py
  python3 fake_miio.py --token 00112233445566778899aabbccddeeff --model zhimi.fan.za5
"""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import struct
import time
from typing import Optional, Tuple


PORT = 54321
MAGIC = b"\x21\x31"


def md5(data: bytes) -> bytes:
    return hashlib.md5(data).digest()


def compute_key_iv(token: bytes) -> Tuple[bytes, bytes]:
    key = md5(token)
    iv = md5(key + token)
    return key, iv


def pkcs7_pad(data: bytes) -> bytes:
    # SMA padPlain: NUL then PKCS#7
    raw = data + b"\x00"
    pad = 16 - (len(raw) % 16)
    return raw + bytes([pad]) * pad


def aes_cbc_encrypt(key: bytes, iv: bytes, plain: bytes) -> bytes:
    try:
        from Crypto.Cipher import AES  # type: ignore
    except ImportError:
        from Cryptodome.Cipher import AES  # type: ignore
    cipher = AES.new(key, AES.MODE_CBC, iv)
    return cipher.encrypt(pkcs7_pad(plain))


def aes_cbc_decrypt(key: bytes, iv: bytes, data: bytes) -> Optional[bytes]:
    if not data or len(data) % 16:
        return None
    try:
        from Crypto.Cipher import AES  # type: ignore
    except ImportError:
        try:
            from Cryptodome.Cipher import AES  # type: ignore
        except ImportError:
            return None
    cipher = AES.new(key, AES.MODE_CBC, iv)
    raw = cipher.decrypt(data)
    pad = raw[-1]
    if pad < 1 or pad > 16 or raw[-pad:] != bytes([pad]) * pad:
        return None
    body = raw[:-pad]
    if body.endswith(b"\x00"):
        body = body[:-1]
    return body


def build_hello_reply(device_id: bytes, ts: int) -> bytes:
    assert len(device_id) == 4
    pkt = bytearray(32)
    pkt[0:2] = MAGIC
    pkt[2:4] = struct.pack(">H", 32)
    pkt[8:12] = device_id
    pkt[12:16] = struct.pack(">I", ts & 0xFFFFFFFF)
    return bytes(pkt)


def build_encrypted(token: bytes, device_id: bytes, ts: int, plain: bytes) -> bytes:
    key, iv = compute_key_iv(token)
    cipher = aes_cbc_encrypt(key, iv, plain)
    total = 32 + len(cipher)
    header = bytearray(32)
    header[0:2] = MAGIC
    header[2:4] = struct.pack(">H", total)
    header[8:12] = device_id
    header[12:16] = struct.pack(">I", ts & 0xFFFFFFFF)
    checksum = md5(bytes(header[:16]) + token + cipher)
    header[16:32] = checksum
    return bytes(header) + cipher


def is_hello(pkt: bytes) -> bool:
    return len(pkt) == 32 and pkt[0:2] == MAGIC and pkt[2:4] == b"\x00\x20"


def parse_encrypted(pkt: bytes, token: bytes) -> Optional[str]:
    if len(pkt) < 48 or pkt[0:2] != MAGIC:
        return None
    total = struct.unpack(">H", pkt[2:4])[0]
    if total != len(pkt):
        return None
    expect = md5(pkt[:16] + token + pkt[32:])
    if expect != pkt[16:32]:
        return None
    key, iv = compute_key_iv(token)
    plain = aes_cbc_decrypt(key, iv, pkt[32:])
    if plain is None:
        return None
    try:
        return plain.decode("utf-8")
    except UnicodeDecodeError:
        return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--device-id", default="01020304", help="8 hex chars")
    ap.add_argument("--token", default="", help="32 hex chars; enables encrypted replies")
    ap.add_argument("--model", default="zhimi.fan.za5")
    ap.add_argument("--fw", default="2.0.0")
    ap.add_argument("--hw", default="ESP32")
    args = ap.parse_args()

    device_id = bytes.fromhex(args.device_id)
    if len(device_id) != 4:
        raise SystemExit("--device-id must be 8 hex chars")
    token = b""
    if args.token:
        token = bytes.fromhex(args.token)
        if len(token) != 16:
            raise SystemExit("--token must be 32 hex chars")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    print(f"fake_miio listening udp/{args.port} model={args.model} enc={'yes' if token else 'hello-only'}")

    ts = int(time.time()) & 0xFFFFFFFF
    while True:
        data, addr = sock.recvfrom(2048)
        if is_hello(data):
            ts = (ts + 1) & 0xFFFFFFFF
            reply = build_hello_reply(device_id, ts)
            sock.sendto(reply, addr)
            print(f"HELLO ← {addr[0]} → did={args.device_id} ts={ts}")
            continue

        if not token or len(data) < 48:
            continue
        plain = parse_encrypted(data, token)
        if plain is None:
            print(f"bad enc ← {addr[0]} len={len(data)}")
            continue
        try:
            req = json.loads(plain)
        except json.JSONDecodeError:
            print(f"bad json ← {addr[0]}: {plain[:80]!r}")
            continue
        method = req.get("method", "")
        req_id = req.get("id", 1)
        ts = (ts + 1) & 0xFFFFFFFF
        if method == "miIO.info":
            result = {
                "id": req_id,
                "result": {
                    "model": args.model,
                    "fw_ver": args.fw,
                    "hw_ver": args.hw,
                    "token": args.token,
                },
            }
        elif method in ("set_properties", "set_property"):
            result = {"id": req_id, "result": ["ok"]}
        else:
            result = {"id": req_id, "error": {"code": -1, "message": f"unknown {method}"}}
        body = json.dumps(result, separators=(",", ":")).encode("utf-8")
        sock.sendto(build_encrypted(token, device_id, ts, body), addr)
        print(f"{method or '?'} ← {addr[0]} id={req_id}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nbye")
