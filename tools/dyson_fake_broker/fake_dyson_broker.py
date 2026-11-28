#!/usr/bin/env python3
"""Minimal fake Dyson MQTT broker for Homewind P0 (dyson_probe).

Behaves enough like a local Dyson fan broker for CONNECT / PUBLISH QoS1 /
PINGREQ / DISCONNECT. Optional mDNS (_dyson_mqtt._tcp) if zeroconf is installed.

Examples:
  python3 fake_dyson_broker.py
  python3 fake_dyson_broker.py --serial NK6-EU-TEST1234 --password secret --pool 3
  python3 fake_dyson_broker.py --auth-fail
"""

from __future__ import annotations

import argparse
import hashlib
import base64
import json
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple


def encode_remaining_length(n: int) -> bytes:
    out = bytearray()
    while True:
        digit = n % 128
        n //= 128
        if n > 0:
            digit |= 0x80
        out.append(digit)
        if n == 0:
            break
    return bytes(out)


def decode_remaining_length(buf: bytes, offset: int) -> Tuple[int, int]:
    multiplier = 1
    value = 0
    pos = offset
    while True:
        if pos >= len(buf):
            raise ValueError("short remaining length")
        encoded = buf[pos]
        pos += 1
        value += (encoded & 127) * multiplier
        if (encoded & 128) == 0:
            break
        multiplier *= 128
        if multiplier > 128 * 128 * 128:
            raise ValueError("remaining length too long")
    return value, pos


def mqtt_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("!H", len(b)) + b


def sticker_to_mqtt_password(sticker: str) -> str:
    digest = hashlib.sha512(sticker.encode("utf-8")).digest()
    return base64.b64encode(digest).decode("ascii")


@dataclass
class BrokerConfig:
    host: str = "0.0.0.0"
    port: int = 1883
    serial: str = "NK6-EU-P0PROBE01"
    product: str = "438"
    password: str = "probe"
    password_is_sticker: bool = True
    auth_fail: bool = False
    pool_limit: int = 0  # 0 = unlimited; else max concurrent after dead leftovers
    keepalive_default: int = 30


@dataclass
class ClientSession:
    sock: socket.socket
    addr: Tuple[str, int]
    client_id: str = ""
    alive: bool = True
    connected_ok: bool = False
    last_rx: float = field(default_factory=time.time)
    recv_buf: bytearray = field(default_factory=bytearray)


class FakeDysonBroker:
    def __init__(self, cfg: BrokerConfig):
        self.cfg = cfg
        if cfg.password_is_sticker and not cfg.auth_fail:
            self.mqtt_password = sticker_to_mqtt_password(cfg.password)
        else:
            self.mqtt_password = cfg.password
        self.sessions: Dict[str, ClientSession] = {}
        self.lock = threading.Lock()
        self.commands: List[dict] = []
        self._mdns = None

    def expected_topic_prefix(self) -> str:
        return f"{self.cfg.product}/{self.cfg.serial}/"

    def start_mdns(self) -> None:
        try:
            from zeroconf import ServiceInfo, Zeroconf  # type: ignore
        except ImportError:
            print("mDNS: zeroconf not installed (pip install zeroconf) — skip")
            return
        # Advertise like Dyson's instance name product_serial
        name = f"{self.cfg.product}_{self.cfg.serial}._dyson_mqtt._tcp.local."
        info = ServiceInfo(
            "_dyson_mqtt._tcp.local.",
            name,
            addresses=[socket.inet_aton(_local_ip())],
            port=self.cfg.port,
            properties={},
        )
        zc = Zeroconf()
        zc.register_service(info)
        self._mdns = (zc, info)
        print(f"mDNS: registered {name} -> :{self.cfg.port}")

    def stop_mdns(self) -> None:
        if not self._mdns:
            return
        zc, info = self._mdns
        zc.unregister_service(info)
        zc.close()
        self._mdns = None

    def serve_forever(self) -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.cfg.host, self.cfg.port))
        srv.listen(8)
        print(
            f"Fake Dyson MQTT on {self.cfg.host}:{self.cfg.port}  "
            f"serial={self.cfg.serial} product={self.cfg.product}"
        )
        print(f"MQTT password (88 chars if sticker): {self.mqtt_password[:16]}…")
        try:
            while True:
                sock, addr = srv.accept()
                threading.Thread(
                    target=self._client_thread, args=(sock, addr), daemon=True
                ).start()
        finally:
            srv.close()
            self.stop_mdns()

    def _client_thread(self, sock: socket.socket, addr: Tuple[str, int]) -> None:
        sess = ClientSession(sock=sock, addr=addr)
        print(f"+ accept {addr}")
        try:
            sock.settimeout(1.0)
            while sess.alive:
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                sess.recv_buf.extend(chunk)
                sess.last_rx = time.time()
                while True:
                    if not self._try_handle_packet(sess):
                        break
        except OSError as e:
            print(f"! {addr} {e}")
        finally:
            self._drop(sess, reason="closed")

    def _drop(self, sess: ClientSession, reason: str) -> None:
        with self.lock:
            if sess.client_id and self.sessions.get(sess.client_id) is sess:
                # Keep dead entry if simulating pool fill (do not delete)
                if self.cfg.pool_limit <= 0:
                    self.sessions.pop(sess.client_id, None)
                else:
                    # Mark dead but count toward pool
                    sess.alive = False
                    sess.connected_ok = False
            elif sess.client_id:
                pass
        try:
            sess.sock.close()
        except OSError:
            pass
        print(f"- drop {sess.addr} id={sess.client_id!r} ({reason})")

    def _pool_full(self) -> bool:
        if self.cfg.pool_limit <= 0:
            return False
        with self.lock:
            return len(self.sessions) >= self.cfg.pool_limit

    def _try_handle_packet(self, sess: ClientSession) -> bool:
        buf = sess.recv_buf
        if len(buf) < 2:
            return False
        try:
            rem, pos = decode_remaining_length(buf, 1)
        except ValueError:
            return False
        total = pos + rem
        if len(buf) < total:
            return False
        packet = bytes(buf[:total])
        del buf[:total]
        self._dispatch(sess, packet)
        return True

    def _dispatch(self, sess: ClientSession, packet: bytes) -> None:
        ptype = packet[0] >> 4
        rem, pos = decode_remaining_length(packet, 1)
        payload = packet[pos : pos + rem]

        if ptype == 1:  # CONNECT
            self._on_connect(sess, payload)
        elif ptype == 3:  # PUBLISH
            self._on_publish(sess, packet[0], payload)
        elif ptype == 12:  # PINGREQ
            sess.sock.sendall(bytes([0xD0, 0x00]))
            print(f"  PINGREQ from {sess.client_id!r} -> PINGRESP")
        elif ptype == 14:  # DISCONNECT
            print(f"  DISCONNECT from {sess.client_id!r}")
            sess.alive = False
        else:
            print(f"  ignore packet type={ptype} len={len(packet)}")

    def _on_connect(self, sess: ClientSession, payload: bytes) -> None:
        # MQTT 3.1 / 3.1.1 variable header
        if len(payload) < 10:
            return
        proto_len = struct.unpack("!H", payload[0:2])[0]
        proto = payload[2 : 2 + proto_len].decode("utf-8", "replace")
        i = 2 + proto_len
        level = payload[i]
        flags = payload[i + 1]
        keepalive = struct.unpack("!H", payload[i + 2 : i + 4])[0]
        i += 4

        def read_str(at: int) -> Tuple[str, int]:
            n = struct.unpack("!H", payload[at : at + 2])[0]
            return payload[at + 2 : at + 2 + n].decode("utf-8", "replace"), at + 2 + n

        client_id, i = read_str(i)
        username = password = ""
        if flags & 0x80:
            username, i = read_str(i)
        if flags & 0x40:
            password, i = read_str(i)

        print(
            f"  CONNECT proto={proto!r} lvl={level} id={client_id!r} "
            f"user={username!r} ka={keepalive} from {sess.addr}"
        )

        # Pool / replace same client id
        with self.lock:
            if client_id in self.sessions:
                old = self.sessions[client_id]
                if old is not sess:
                    try:
                        old.sock.close()
                    except OSError:
                        pass
                    print(f"  replace previous session id={client_id!r}")
            if self.cfg.pool_limit > 0 and client_id not in self.sessions:
                if len(self.sessions) >= self.cfg.pool_limit:
                    # Refuse: CONNACK not authorized / server unavailable
                    sess.sock.sendall(bytes([0x20, 0x02, 0x00, 0x03]))
                    print("  CONNACK 3 (pool full)")
                    sess.alive = False
                    return
            self.sessions[client_id] = sess

        sess.client_id = client_id
        ok = True
        code = 0
        if self.cfg.auth_fail:
            ok = False
            code = 4
        elif username and username != self.cfg.serial:
            ok = False
            code = 4
        elif password and password != self.mqtt_password:
            # Also accept raw sticker password for convenience in P0
            if not (
                self.cfg.password_is_sticker and password == self.cfg.password
            ):
                ok = False
                code = 5

        # CONNACK
        sess.sock.sendall(bytes([0x20, 0x02, 0x00, code]))
        sess.connected_ok = ok
        print(f"  CONNACK {code} ({'ok' if ok else 'fail'})")
        if not ok:
            sess.alive = False

    def _on_publish(self, sess: ClientSession, header: int, payload: bytes) -> None:
        qos = (header >> 1) & 0x03
        if len(payload) < 2:
            return
        tlen = struct.unpack("!H", payload[0:2])[0]
        topic = payload[2 : 2 + tlen].decode("utf-8", "replace")
        i = 2 + tlen
        pkt_id = None
        if qos > 0:
            pkt_id = struct.unpack("!H", payload[i : i + 2])[0]
            i += 2
        body = payload[i:]
        text = body.decode("utf-8", "replace")
        stage = _parse_stage(text)
        entry = {
            "t": time.time(),
            "client_id": sess.client_id,
            "topic": topic,
            "qos": qos,
            "stage": stage,
            "body": text[:200],
        }
        with self.lock:
            self.commands.append(entry)
        print(f"  PUBLISH {topic} qos={qos} stage={stage} body={text[:120]!r}")
        if qos == 1 and pkt_id is not None:
            sess.sock.sendall(struct.pack("!BBH", 0x40, 0x02, pkt_id))
            print(f"  PUBACK id={pkt_id}")


def _parse_stage(body: str) -> Optional[int]:
    try:
        data = json.loads(body)
    except json.JSONDecodeError:
        return None
    d = data.get("data") or {}
    if isinstance(d.get("fnsp"), str) and d["fnsp"].isdigit():
        return int(d["fnsp"])
    if d.get("fpwr") == "OFF" or d.get("fmod") == "OFF":
        return 0
    return None


def _local_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--serial", default="NK6-EU-P0PROBE01")
    ap.add_argument("--product", default="438")
    ap.add_argument("--password", default="probe", help="sticker password or raw MQTT password")
    ap.add_argument(
        "--raw-password",
        action="store_true",
        help="treat --password as the final MQTT password (no SHA-512)",
    )
    ap.add_argument("--auth-fail", action="store_true")
    ap.add_argument("--pool", type=int, default=0, help="max sessions (0=unlimited); dead keep slots")
    ap.add_argument("--no-mdns", action="store_true")
    args = ap.parse_args()

    cfg = BrokerConfig(
        host=args.host,
        port=args.port,
        serial=args.serial,
        product=args.product,
        password=args.password,
        password_is_sticker=not args.raw_password,
        auth_fail=args.auth_fail,
        pool_limit=args.pool,
    )
    broker = FakeDysonBroker(cfg)
    if not args.no_mdns:
        broker.start_mdns()
    try:
        broker.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
        broker.stop_mdns()


if __name__ == "__main__":
    main()
