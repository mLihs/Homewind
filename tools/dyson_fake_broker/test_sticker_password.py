#!/usr/bin/env python3
"""Host check: sticker password → MQTT password (SHA-512 + Base64, 88 chars)."""

from __future__ import annotations

import hashlib
import base64
import unittest

from fake_dyson_broker import sticker_to_mqtt_password


class StickerPasswordTest(unittest.TestCase):
    def test_length_and_stable(self):
        pw = sticker_to_mqtt_password("probe")
        self.assertEqual(len(pw), 88)
        self.assertEqual(pw, sticker_to_mqtt_password("probe"))

    def test_matches_hashlib(self):
        sticker = "probe"
        expect = base64.b64encode(hashlib.sha512(sticker.encode()).digest()).decode()
        self.assertEqual(sticker_to_mqtt_password(sticker), expect)

    def test_different_stickers_differ(self):
        self.assertNotEqual(
            sticker_to_mqtt_password("probe"),
            sticker_to_mqtt_password("probe2"),
        )


if __name__ == "__main__":
    unittest.main()
