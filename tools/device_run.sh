#!/usr/bin/env bash
# Install the patched APK on a device, forward the server port, and launch.
#   tools/device_run.sh -s <serial> [--fresh] [--apk build/....apk]
#     --fresh   clear app data first (the client caches hosts and login state)
# Logcat for the game process is written to logs/logcat-<serial>.txt.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADB="$HOME/Library/Android/sdk/platform-tools/adb"
PKG="sg.gumi.bravefrontier"
APK="$ROOT/build/BraveFrontier-local-127.0.0.1.apk"
SERIAL=""
FRESH=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -s) SERIAL="$2"; shift 2 ;;
        --fresh) FRESH=1; shift ;;
        --apk) APK="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$SERIAL" ]] || { echo "usage: $0 -s <serial> [--fresh] [--apk path]" >&2; exit 2; }
A=("$ADB" -s "$SERIAL")

# A re-signed APK cannot update an install signed with gumi's key.
"${A[@]}" install -r "$APK" || {
    echo "install failed; if the installed copy is the original Play build, uninstall it first (this deletes its local data)" >&2
    exit 1
}
"${A[@]}" reverse tcp:9960 tcp:9960
if [[ "$FRESH" == "1" ]]; then
    "${A[@]}" shell pm clear "$PKG" >/dev/null
fi

mkdir -p "$ROOT/logs"
"${A[@]}" logcat -c
"${A[@]}" shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep 3
PID=$("${A[@]}" shell pidof "$PKG" | tr -d '\r' || true)
echo "launched $PKG (pid ${PID:-?}); logcat -> logs/logcat-$SERIAL.txt"
if [[ -n "$PID" ]]; then
    "${A[@]}" logcat --pid "$PID" > "$ROOT/logs/logcat-$SERIAL.txt" 2>&1 &
    echo "logcat writer pid $!"
fi
