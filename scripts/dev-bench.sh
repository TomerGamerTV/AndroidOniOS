#!/bin/bash
# Rough guest performance check on the iPhone (needs debug.json debugShell + monitor).
# Relaunches the app, reports boot time, code-cache flushes and Settings cold-start times.
# usage: scripts/dev-bench.sh [label]
DEV=${DEV:?set DEV to your iPhone UDID (xcrun devicectl list devices)}
HOST=${MONITOR_HOST:-169.254.116.178}
sh() { echo "$1" | nc -w 120 "$HOST" 4445; }
t0=$(date +%s)
xcrun devicectl device process launch --device "$DEV" --terminate-existing com.tomergamertv.oni >/dev/null 2>&1
sleep 15
until [ "$(sh 'getprop sys.boot_completed' 2>/dev/null | tr -d '\r\n ')" = "1" ]; do
    sleep 2; [ $(( $(date +%s) - t0 )) -gt 400 ] && { echo "boot timeout"; exit 1; }
done
echo "${1:-run}: boot $(( $(date +%s) - t0 ))s"
sleep 45
for i in 1 2 3; do
    sh 'am force-stop com.android.settings; am start -W -n com.android.settings/.Settings | grep -E "TotalTime"; input keyevent KEYCODE_HOME; sleep 3'
done
printf 'info jit\n' | nc -w 3 "$HOST" 4444 | tr -d '\r' | grep -aE 'TB flush count|TLB partial'
