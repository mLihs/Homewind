#!/usr/bin/env bash
# Keep serial_log.py running across ESP resets (USB re-enumeration).
# Commands: echo scan > /tmp/g5_cmd_fifo
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="$ROOT/tools/.venv-serial/bin/python"
LOG="$1"
FIFO=/tmp/g5_cmd_fifo
[[ -p "$FIFO" ]] || mkfifo "$FIFO"
exec 3<>"$FIFO"
while true; do
  PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1)"
  if [[ -n "$PORT" ]]; then
    "$PY" -u "$ROOT/tools/serial_log.py" "$PORT" "$LOG" <&3
  fi
  sleep 1
done
