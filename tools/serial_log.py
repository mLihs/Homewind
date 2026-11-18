#!/usr/bin/env python3
"""Serial logger for Homewind test sketches.

Writes every line from the ESP32 with a host timestamp to a log file and to the
terminal; lines typed in the terminal are sent to the device (e.g. "scan", "use 8",
"cps", "s"). Requires pyserial (pip install pyserial).

Usage:
  python3 tools/serial_log.py /dev/cu.usbmodem101 logs/g5_ipad_erg.log
"""
import os
import sys
import threading
import time

import serial


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    port, path = sys.argv[1], sys.argv[2]
    ser = serial.Serial(port, 115200, timeout=0.2)
    with open(path, "a", encoding="utf-8") as log:
        log.write(f"# start {time.strftime('%Y-%m-%d %H:%M:%S')} port {port}\n")

        def reader() -> None:
            buf = b""
            while True:
                try:
                    chunk = ser.read(512)
                except (serial.SerialException, OSError):
                    log.write("# serial lost\n")
                    log.flush()
                    os._exit(2)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").rstrip("\r")
                    stamp = time.strftime("%H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"
                    out = f"{stamp} {text}"
                    print(out, flush=True)
                    log.write(out + "\n")
                    log.flush()

        threading.Thread(target=reader, daemon=True).start()
        try:
            for cmd in sys.stdin:
                cmd = cmd.strip()
                log.write(f"# >>> {cmd}\n")
                try:
                    ser.write((cmd + "\n").encode())
                except (serial.SerialException, OSError):
                    log.write("# serial lost\n")
                    log.flush()
                    os._exit(2)
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
