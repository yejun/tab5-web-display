#!/usr/bin/env bash
# Build (and optionally flash) the Tab5 firmware:  tools/build.sh [upload [PORT]]
#
#   CHIP=postv3 tools/build.sh upload    for ESP32-P4 chips of revision v3.0 and later
#
# The default, CHIP=prev3, is for earlier chips: esptool prints "revision v1.x" for them
# (e.g. `esptool.py --port PORT chip_id`). A build for the wrong revision does not boot.
set -euo pipefail
if [[ $# -gt 2 || ( $# -gt 0 && "$1" != "upload" ) ]]; then
  echo "Usage: [CHIP=prev3|postv3] $0 [upload [PORT]]" >&2
  exit 2
fi
cd "$(dirname "$0")/.."
ROOT=$PWD
CHIP=${CHIP:-prev3}
[[ "$CHIP" == prev3 || "$CHIP" == postv3 ]] || { echo "CHIP must be prev3 or postv3" >&2; exit 2; }
FQBN="m5stack:esp32:m5stack_tab5:ChipVariant=$CHIP,PSRAM=enabled,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default"
PORT=${2:-/dev/ttyACM0}
SKETCH="$ROOT/firmware/web_display"
BUILD="$ROOT/build/$CHIP"
FLAGS="-DLV_CONF_INCLUDE_SIMPLE -I$SKETCH"   # LVGL finds firmware/web_display/lv_conf.h
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" \
  --build-property "compiler.c.extra_flags=$FLAGS" --build-property "compiler.cpp.extra_flags=$FLAGS" "$SKETCH"
if [[ "${1:-}" == "upload" ]]; then
  arduino-cli upload --fqbn "$FQBN" --port "$PORT" --input-dir "$BUILD"
fi
