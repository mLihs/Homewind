#!/usr/bin/env bash
# Build and flash the Homewind Touch and Basic firmware variants.
# See tools/README.md.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE="$(cd "$ROOT/.." && pwd)"
CACHE_ROOT="$ROOT/build/.cache"

# Board option ids match the ESP32 Arduino core 3.3.12 menus.
# Touch: ARDUINO_SETUP.md (PSRAM on, 16MB, USB CDC on boot).
# Basic: XIAO ESP32-S3. CDCOnBoot=default means Enabled on this board.
TOUCH_FQBN="esp32:esp32:waveshare_esp32_s3_touch_amoled_164:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=enabled,LoopCore=1,EventsCore=1,EraseFlash=none"
TOUCH_DIR="esp32.esp32.waveshare_esp32_s3_touch_amoled_164"
BASIC_FQBN="esp32:esp32:XIAO_ESP32S3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=8M,PartitionScheme=default_8MB,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none"
BASIC_DIR="esp32.esp32.XIAO_ESP32S3"

WORKSPACE_LIBS=(
  HomeWindWSAmoled
  BluetoothBikeSensorServer
  SmartMiFanAsync
  wifiMangerLite
  GitFirmwareUpdate
  NVSUtilityLibrary
  DebugLog
)

usage() {
  cat <<EOF
Usage: tools/build.sh [build|flash|ports] [touch|basic|all] [options]

  build (default)   compile, write local manifest.json; by default also copy to the website tree
  flash             upload an existing build (no compile)
  ports             list serial ports

Variants:
  touch             Waveshare ESP32-S3-Touch-AMOLED-1.64, display on
  basic             XIAO ESP32-S3, display off
  all               both (default). Cannot be combined with flashing.

Options:
  --flash           flash after a successful build (single variant only)
  --port PATH       serial port, e.g. /dev/cu.usbmodem1101
  --no-deploy       keep the local manifest; do not copy firmware into the website tree
  --clean           delete the object cache before compiling
  -h, --help        show this help

Examples:
  tools/build.sh
  tools/build.sh touch --flash
  tools/build.sh flash basic --port /dev/cu.usbmodem1101
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

require_tools() {
  command -v arduino-cli >/dev/null 2>&1 || die "arduino-cli not found"
  command -v python3 >/dev/null 2>&1 || die "python3 not found"
  [[ ! -f "$ROOT/build_opt.h" ]] || die "$ROOT/build_opt.h would override the variant flags. Remove it."
  for name in "${WORKSPACE_LIBS[@]}"; do
    [[ -f "$WORKSPACE/$name/library.properties" ]] || die "missing workspace library: $WORKSPACE/$name"
  done
}

read_version() {
  VERSION="$(grep '^version=' "$ROOT/library.properties" | head -n 1 | cut -d= -f2- | tr -d '[:space:]')"
  [[ -n "$VERSION" ]] || die "no version= in $ROOT/library.properties"
  BUILD_ID="$(date +%Y%m%d-%H%M%S)"
}

list_ports() {
  arduino-cli board list --json | python3 -c '
import json, sys
data = json.load(sys.stdin)
skip = ("debug-console", "wlan-debug", "Bluetooth-Incoming-Port")
found = False
for item in data.get("detected_ports", []):
    port = item.get("port") or {}
    addr = port.get("address") or ""
    if port.get("protocol") != "serial":
        continue
    if any(part in addr for part in skip):
        continue
    found = True
    boards = item.get("matching_boards") or []
    names = ", ".join(b.get("name", "") for b in boards if b.get("name"))
    print(f"{addr}\t{names}" if names else addr)
if not found:
    sys.exit(2)
'
}

detect_port() {
  if [[ -n "$PORT" ]]; then
    echo "$PORT"
    return
  fi
  local ports
  if ! ports="$(list_ports)"; then
    die "no board port found. Connect the ESP32 and pass --port."
  fi
  local count
  count="$(printf '%s\n' "$ports" | sed '/^$/d' | wc -l | tr -d ' ')"
  if [[ "$count" -ne 1 ]]; then
    echo "Several serial ports found:" >&2
    printf '%s\n' "$ports" >&2
    die "pass --port to choose one."
  fi
  printf '%s\n' "$ports" | head -n 1 | cut -f1
}

write_build_stamp() {
  local cache="$1"
  local display="$2"
  local include_dir="$cache/include"
  mkdir -p "$include_dir"
  python3 - "$include_dir/homewind_build_stamp.h" "$cache.build_opt.h" "$display" "$VERSION" "$BUILD_ID" "$include_dir" <<'PY'
import sys
header, opt, display, version, build_id, include_dir = sys.argv[1:]
with open(header, "w", encoding="utf-8") as fh:
    fh.write("#pragma once\n")
    fh.write(f"#define HW_ENABLE_DISPLAY {display}\n")
    fh.write(f'#define FW_VERSION "{version}"\n')
    fh.write(f'#define FW_BUILD_ID "{build_id}"\n')
# Response-file quoting so the space in "ESP-Code 1_1" stays one argument.
with open(opt, "w", encoding="utf-8") as fh:
    fh.write(f'"-I{include_dir}"\n')
    # Discovery snapshot size = BLE_DISCOVERY_MAX in BLERelayManager.h (15 fit
    # into one DISCOVERY_RESULTS frame). Applies to every translation unit.
    fh.write("-DBBS_MAX_DISCOVERED=15\n")
PY
  # arduino-cli does not recompile library objects when build_opt.h changes.
  if [[ -f "$cache/build_opt.h" ]] && ! cmp -s "$cache.build_opt.h" "$cache/build_opt.h"; then
    echo "    build_opt.h changed: discarding $cache"
    find "$cache" -mindepth 1 -maxdepth 1 ! -name include -exec rm -rf {} +
  fi
  mv -f "$cache.build_opt.h" "$cache/build_opt.h"
}

library_args() {
  LIB_ARGS=()
  local name
  for name in "${WORKSPACE_LIBS[@]}"; do
    LIB_ARGS+=(--library "$WORKSPACE/$name")
  done
}

variant_settings() {
  case "$1" in
    touch)
      FQBN="$TOUCH_FQBN"
      BOARD_DIR="$TOUCH_DIR"
      DISPLAY=1
      EXPECT_URL="firmware/homewind-touch/latest.json"
      REJECT_URL="firmware/homewind-basic/latest.json"
      ;;
    basic)
      FQBN="$BASIC_FQBN"
      BOARD_DIR="$BASIC_DIR"
      DISPLAY=0
      EXPECT_URL="firmware/homewind-basic/latest.json"
      REJECT_URL="firmware/homewind-touch/latest.json"
      ;;
    *)
      die "unknown variant: $1"
      ;;
  esac
}

assert_workspace_libs() {
  local log="$1"
  local variant="$2"
  local name
  for name in "${WORKSPACE_LIBS[@]}"; do
    if grep -F "/Arduino/libraries/${name}/" "$log" >/dev/null; then
      die "$variant build compiled the Arduino libraries copy of $name"
    fi
    if [[ "$variant" == "basic" && "$name" == "HomeWindWSAmoled" ]]; then
      continue
    fi
    if ! grep -F "$WORKSPACE/$name/" "$log" >/dev/null; then
      die "$variant build did not compile workspace library $name"
    fi
  done
}

assert_variant_binary() {
  local bin="$1"
  strings "$bin" | grep -F "$VERSION" >/dev/null || die "binary is missing version $VERSION: $bin"
  strings "$bin" | grep -F "$BUILD_ID" >/dev/null || die "binary is missing build id $BUILD_ID: $bin"
  strings "$bin" | grep -F "$EXPECT_URL" >/dev/null || die "binary is missing $EXPECT_URL: $bin"
  if strings "$bin" | grep -F "$REJECT_URL" >/dev/null; then
    die "binary contains the other variant URL ($REJECT_URL): $bin"
  fi
}

publish_bins() {
  local cache="$1"
  local dest="$2"
  mkdir -p "$dest"
  cp -f "$cache/flash_args" "$dest/flash_args"
  local offset name
  while read -r offset name; do
    [[ "$offset" == 0x* ]] || continue
    [[ -n "$name" ]] || continue
    [[ -f "$cache/$name" ]] || die "missing $cache/$name"
    cp -f "$cache/$name" "$dest/$name"
    echo "  $name"
  done < "$cache/flash_args"
}

build_variant() {
  local variant="$1"
  variant_settings "$variant"
  local cache="$CACHE_ROOT/$variant"
  local published="$ROOT/build/$BOARD_DIR"
  local log="$cache.log"

  if [[ "$CLEAN" -eq 1 ]]; then
    rm -rf "$cache"
  fi
  mkdir -p "$cache"
  write_build_stamp "$cache" "$DISPLAY"

  echo
  echo "==> Build $variant  version $VERSION  build $BUILD_ID  display=$DISPLAY"
  echo "    FQBN: $FQBN"
  echo "    log: $log"
  library_args
  if ! arduino-cli compile \
    --fqbn "$FQBN" \
    --build-path "$cache" \
    --verbose \
    --warnings none \
    "${LIB_ARGS[@]}" \
    "$ROOT" >"$log" 2>&1; then
    echo "compile failed. Last lines of $log:" >&2
    tail -n 40 "$log" >&2
    exit 1
  fi
  grep -E "Sketch uses|Global variables" "$log" || true

  [[ -f "$cache/Homewind.ino.bin" ]] || die "compile did not produce $cache/Homewind.ino.bin"
  [[ -f "$cache/flash_args" ]] || die "compile did not produce $cache/flash_args"
  assert_workspace_libs "$log" "$variant"
  assert_variant_binary "$cache/Homewind.ino.bin"

  echo "==> Artifacts -> $published"
  publish_bins "$cache" "$published"
  rm -f "$published"/*.elf "$published"/*.map "$published"/*.merged.bin
}

flash_variant() {
  local variant="$1"
  variant_settings "$variant"
  local published="$ROOT/build/$BOARD_DIR"
  [[ -f "$published/Homewind.ino.bin" ]] || die "no build for $variant. Run: tools/build.sh $variant"
  local port
  port="$(detect_port)"
  echo
  echo "==> Flash $variant on $port"
  arduino-cli upload \
    --port "$port" \
    --fqbn "$FQBN" \
    --input-dir "$published" \
    "$ROOT"
}

write_manifest_variant() {
  local variant="$1"
  local -a args=(--board "$variant")
  echo
  if [[ "$DEPLOY" -eq 1 ]]; then
    echo "==> ESP Web Tools manifest + website deploy ($variant)"
    args+=(--deploy)
  else
    echo "==> ESP Web Tools manifest ($variant)"
  fi
  python3 "$ROOT/tools/update_manifest.py" "${args[@]}"
}

COMMAND="build"
VARIANT=""
PORT=""
DEPLOY=1
CLEAN=0
FLASH_AFTER=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    build|flash|ports) COMMAND="$1"; shift ;;
    touch|basic|all) VARIANT="$1"; shift ;;
    --flash) FLASH_AFTER=1; shift ;;
    --port)
      [[ $# -ge 2 ]] || die "--port needs a path"
      PORT="$2"; shift 2 ;;
    --port=*) PORT="${1#*=}"; shift ;;
    --no-deploy) DEPLOY=0; shift ;;
    --clean) CLEAN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done

if [[ "$COMMAND" == "ports" ]]; then
  list_ports || die "no board port found"
  exit 0
fi

VARIANT="${VARIANT:-all}"
require_tools
read_version

if [[ "$COMMAND" == "flash" || "$FLASH_AFTER" -eq 1 ]]; then
  [[ "$VARIANT" != "all" ]] || die "flashing needs one variant: touch or basic"
fi

variants=()
if [[ "$VARIANT" == "all" ]]; then
  variants=(touch basic)
else
  variant_settings "$VARIANT"
  variants=("$VARIANT")
fi

if [[ "$COMMAND" != "flash" ]]; then
  for variant in "${variants[@]}"; do
    build_variant "$variant"
    write_manifest_variant "$variant"
  done
fi

if [[ "$COMMAND" == "flash" || "$FLASH_AFTER" -eq 1 ]]; then
  flash_variant "$VARIANT"
fi

echo
echo "Done. Version $VERSION, build $BUILD_ID."
