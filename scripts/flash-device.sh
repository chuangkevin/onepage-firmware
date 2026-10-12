#!/usr/bin/env bash
# Flash one OnePage (ESP32-C61) e-reader with the custom firmware.
#
#   ./flash-device.sh /dev/cu.usbmodemXXXX [backup-dir]
#
# Steps:
#   1. Back up the device's full 16 MB flash.
#   2. Back up the internal-storage partition (app1) as it stands on the device,
#      so device-side data (Wi-Fi credentials, books, reading state) is never
#      lost when step 3 overwrites it.
#   3. Flash bootloader + partition table + app + the internal-storage image.
set -euo pipefail

PORT="${1:?usage: flash-device.sh <serial-port> [backup-dir]}"
BACKUP_DIR="${2:-backups}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REL="$HERE/release"

for f in bootloader.bin partitions.bin firmware.bin internal-storage.img; do
  [ -f "$REL/$f" ] || { echo "missing release/$f"; exit 1; }
done

mkdir -p "$BACKUP_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"

MAC="$(uvx esptool --port "$PORT" chip-id 2>&1 | awk '/^MAC:/{print $2}' | tail -1)"
SLUG="$(echo "$MAC" | tr ':' '-')"
DST="$BACKUP_DIR/${SLUG:-unknown}"
mkdir -p "$DST"

echo "== 1/3  backing up full 16MB flash -> $DST/flash-$STAMP.bin"
uvx esptool --port "$PORT" read-flash 0 0x1000000 "$DST/flash-$STAMP.bin"
shasum -a 256 "$DST/flash-$STAMP.bin"

echo "== 2/3  backing up internal storage (app1) -> $DST/app1-$STAMP.bin"
uvx esptool --port "$PORT" read-flash 0x650000 0x640000 "$DST/app1-$STAMP.bin" >/dev/null
shasum -a 256 "$DST/app1-$STAMP.bin"

echo "== 3/3  flashing firmware + internal storage"
uvx esptool --port "$PORT" write-flash \
  0x0      "$REL/bootloader.bin" \
  0x8000   "$REL/partitions.bin" \
  0x10000  "$REL/firmware.bin" \
  0x650000 "$REL/internal-storage.img"

echo "done — device should boot to Home with no microSD."
echo "device-side data saved to: $DST/app1-$STAMP.bin"
