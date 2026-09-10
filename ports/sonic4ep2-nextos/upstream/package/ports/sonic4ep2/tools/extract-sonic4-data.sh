#!/bin/bash
# Extract Sonic 4 Episode II Android data into this port.
# Usage:
#   ./extract-sonic4-data.sh <sonic4ep2.apk> [cache.zip|main.22...obb] [port_dir]
set -e

APK="$1"
DATA="$2"
DEST="$3"

if [ -z "$APK" ] || [ ! -f "$APK" ]; then
  echo "usage: $0 <sonic4ep2.apk> [cache.zip|main.22.com.sega.sonic4episode2.obb] [port_dir]"
  exit 1
fi

if [ -z "$DEST" ]; then
  case "$DATA" in
    ""|*.zip|*.ZIP|*.obb|*.OBB) DEST="." ;;
    *) DEST="$DATA"; DATA="" ;;
  esac
fi

mkdir -p "$DEST/lib/armeabi-v7a" "$DEST/data"
echo "APK:  $APK"
echo "DATA: ${DATA:-auto}"
echo "DEST: $DEST"

echo "[1/3] libfox.so"
unzip -o -j "$APK" "lib/armeabi-v7a/libfox.so" -d "$DEST/lib/armeabi-v7a"

if [ -n "$DATA" ]; then
  echo "[2/3] OBB"
  case "$DATA" in
    *.obb|*.OBB)
      cp -f "$DATA" "$DEST/data/main.22.com.sega.sonic4episode2.obb"
      ;;
    *)
      unzip -o -j "$DATA" "com.sega.sonic4episode2/main.22.com.sega.sonic4episode2.obb" -d "$DEST/data"
      ;;
  esac
else
  echo "[2/3] OBB skipped; place cache zip or OBB beside the port if needed."
fi

echo "[3/3] f2f marker"
[ -f "$DEST/Sonic4ep2.f2f" ] || printf '{"MerchandiseTime":1782470230}' > "$DEST/Sonic4ep2.f2f"
echo "OK"
