#!/usr/bin/env bash
# Flash one OnePage (ESP32-C61) e-reader with the custom firmware.
#
#   ./flash-device.sh /dev/cu.usbmodemXXXX [backup-dir]
#   ./flash-device.sh --no-backup /dev/cu.usbmodemXXXX
#
# Default: back up the full 16 MB flash AND the internal-storage partition
# (app1) before overwriting anything, so device-side data (Wi-Fi credentials,
# books, reading state) is never lost.
#
# --no-backup: skip both backups and flash straight away (faster).
set -euo pipefail

BACKUP=1
if [ "${1:-}" = "--no-backup" ]; then
  BACKUP=0
  shift
fi

PORT="${1:?usage: flash-device.sh [--no-backup] <serial-port> [backup-dir]}"
BACKUP_DIR="${2:-backups}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REL="$HERE/release"

for f in bootloader.bin partitions.bin firmware.bin internal-storage.img; do
  [ -f "$REL/$f" ] || { echo "missing release/$f"; exit 1; }
done

STAMP="$(date +%Y%m%d-%H%M%S)"

if [ "$BACKUP" = 1 ]; then
  mkdir -p "$BACKUP_DIR"
  MAC="$(uvx esptool --port "$PORT" chip-id 2>&1 | awk '/^MAC:/{print $2}' | tail -1)"
  SLUG="$(echo "$MAC" | tr ':' '-')"
  DST="$BACKUP_DIR/${SLUG:-unknown}"
  mkdir -p "$DST"

  echo "== backing up full 16MB flash -> $DST/flash-$STAMP.bin"
  uvx esptool --port "$PORT" read-flash 0 0x1000000 "$DST/flash-$STAMP.bin"
  echo "== backing up internal storage (app1) -> $DST/app1-$STAMP.bin"
  uvx esptool --port "$PORT" read-flash 0x650000 0x640000 "$DST/app1-$STAMP.bin" >/dev/null
fi

echo "== flashing firmware + internal storage"
uvx esptool --port "$PORT" write-flash \
  0x0      "$REL/bootloader.bin" \
  0x8000   "$REL/partitions.bin" \
  0x10000  "$REL/firmware.bin" \
  0x650000 "$REL/internal-storage.img"

echo "done — device should boot to Home with no microSD."
