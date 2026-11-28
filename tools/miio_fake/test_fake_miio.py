#!/usr/bin/env python3
"""Host smoke for fake_miio HELLO (+ optional encrypt if PyCryptodome present)."""

from __future__ import annotations

import socket
import subprocess
import sys
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fake_miio  # noqa: E402


class FakeMiioHelloTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = 35432
        cls.proc = subprocess.Popen(
            [sys.executable, str(HERE / "fake_miio.py"), "--port", str(cls.port),
             "--device-id", "aabbccdd"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        time.sleep(0.3)
        if cls.proc.poll() is not None:
            out = cls.proc.stdout.read() if cls.proc.stdout else ""
            raise RuntimeError(f"fake_miio failed to start: {out}")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        try:
            cls.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            cls.proc.kill()

    def test_hello_reply(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(2.0)
        hello = bytearray(32)
        hello[0:2] = b"\x21\x31"
        hello[2:4] = b"\x00\x20"
        sock.sendto(bytes(hello), ("127.0.0.1", self.port))
        data, _ = sock.recvfrom(64)
        sock.close()
        self.assertTrue(fake_miio.is_hello(data))
        self.assertEqual(data[8:12], bytes.fromhex("aabbccdd"))


class CodecHelpersTest(unittest.TestCase):
    def test_hello_builder(self):
        pkt = fake_miio.build_hello_reply(bytes.fromhex("01020304"), 99)
        self.assertTrue(fake_miio.is_hello(pkt))
        self.assertEqual(pkt[8:12], b"\x01\x02\x03\x04")


if __name__ == "__main__":
    unittest.main(verbosity=2)
