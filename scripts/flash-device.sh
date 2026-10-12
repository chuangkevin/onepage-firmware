#!/usr/bin/env bash
# Flash one OnePage (ESP32-C61) e-reader with the custom firmware.
#
#   ./flash-device.sh /dev/cu.usbmodemXXXX [backup-dir]
#
# Steps:
#   1. Back up the device's full 16 MB flash (restorable with esptool write-flash 0x0).
#   2. Flash bootloader + partition table + app + the internal-storage image
#      (which carries the CJK font used when no microSD is inserted).
#
# The internal-storage image goes to the app1 slot (0x650000) and is mounted as
# a FAT volume by the firmware when no microSD is present. Nothing here touches
# the microSD, so a card in the slot is left alone.
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

echo "== 1/2  backing up full 16MB flash -> $BACKUP_DIR/flash-$STAMP.bin"
uvx esptool --port "$PORT" read-flash 0 0x1000000 "$BACKUP_DIR/flash-$STAMP.bin"
shasum -a 256 "$BACKUP_DIR/flash-$STAMP.bin"

echo "== 2/2  flashing firmware + internal storage"
uvx esptool --port "$PORT" write-flash \
  0x0      "$REL/bootloader.bin" \
  0x8000   "$REL/partitions.bin" \
  0x10000  "$REL/firmware.bin" \
  0x650000 "$REL/internal-storage.img"

echo "done — device should boot to Home with no microSD."
