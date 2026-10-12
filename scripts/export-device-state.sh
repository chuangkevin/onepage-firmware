#!/usr/bin/env bash
# Pull the Wi-Fi credentials (and other /-.crosspoint state) off a connected
# OnePage device's internal-storage partition, without flashing anything.
#
#   ./export-device-state.sh /dev/cu.usbmodemXXXX [out-dir]
#
# The device's internal storage is a FAT16 volume (superfloppy) on the app1
# partition at 0x650000, size 0x640000. We read it and use mtools to extract
# files. Nothing is written to the device.
set -euo pipefail

PORT="${1:?usage: export-device-state.sh <serial-port> [out-dir]}"
OUT="${2:-device-state}"

command -v mcopy >/dev/null || { echo "mtools not installed (brew install mtools)"; exit 1; }

mkdir -p "$OUT"
IMG="$OUT/app1-dump.bin"

echo "== reading app1 (internal storage) =="
uvx esptool --port "$PORT" read-flash 0x650000 0x640000 "$IMG" >/dev/null
ls -l "$IMG"

echo "== listing /.crosspoint =="
mdir -i "$IMG" -/ ::/.crosspoint 2>&1 | tail -20 || true

for f in wifi.json wifi.bin settings.json recent.json state.json opds.json; do
  if mcopy -i "$IMG" -n "::/.crosspoint/$f" "$OUT/$f" 2>/dev/null; then
    echo "extracted: $OUT/$f"
  fi
done

if [ -f "$OUT/wifi.json" ]; then
  echo "== wifi.json =="
  cat "$OUT/wifi.json"
fi
