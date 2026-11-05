#!/bin/bash
# Grabs the Android framebuffer from the app on the iPhone (QEMU monitor over USB link-local).
# usage: scripts/dev-screen.sh [out.png]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEV=${DEV:?set DEV to your iPhone UDID (xcrun devicectl list devices)}
HOST=${MONITOR_HOST:-169.254.116.178}
OUT=${1:-$ROOT/build/screen.png}
TMP=$(mktemp -d)
dc() { xcrun devicectl device copy from --device "$DEV" --domain-type appDataContainer --domain-identifier com.tomer.androidonios "$@" >/dev/null 2>&1; }
dc --source Library/Android/logs/app.log --destination "$TMP/app.log"
C=$(grep -o "/var/mobile/Containers/Data/Application/[A-F0-9-]*" "$TMP/app.log" | tail -1)
printf 'screendump %s/Library/Android/logs/screen.ppm\n' "$C" | nc -w 3 "$HOST" 4444 >/dev/null
dc --source Library/Android/logs/screen.ppm --destination "$TMP/screen.ppm"
sips -s format png "$TMP/screen.ppm" --out "$OUT" >/dev/null && sips -Z 900 "$OUT" >/dev/null && echo "$OUT"
rm -rf "$TMP"
