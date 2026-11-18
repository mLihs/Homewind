#!/usr/bin/env bash
# Flash trainer_mirror_relay (if needed) and start serial logging for G5.
# Usage: tools/g5_run.sh [/dev/cu.usbmodem…] [log-suffix]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WS="$(cd "$ROOT/.." && pwd)"
PY="$ROOT/tools/.venv-serial/bin/python"
PORT="${1:-}"
SUFFIX="${2:-ipad}"
LOGDIR="$ROOT/logs"
mkdir -p "$LOGDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$LOGDIR/g5_${SUFFIX}_${STAMP}.log"

if [[ -z "$PORT" ]]; then
  PORT="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -n1 || true)"
fi
[[ -n "$PORT" ]] || { echo "error: no serial port; plug in the XIAO ESP32-S3"; exit 1; }
[[ -x "$PY" ]] || { echo "error: missing $PY (create venv + pyserial)"; exit 1; }

echo "==> port $PORT"
echo "==> log  $LOG"
echo "==> flash trainer_mirror_relay (XIAO ESP32-S3)…"
arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32S3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=8M,PartitionScheme=default_8MB,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none" \
  --library "$WS/NimBLE-Arduino" \
  --upload -p "$PORT" \
  "$ROOT/tools/trainer_mirror_relay"

echo
echo "Logger ready. Type commands into this terminal: scan | use N | cps | dump | forget"
echo "iPad: remove homewind-test in Zwift + Bluetooth, then pair again."
echo
exec "$PY" "$ROOT/tools/serial_log.py" "$PORT" "$LOG"
