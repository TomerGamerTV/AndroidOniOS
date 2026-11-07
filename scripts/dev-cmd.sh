#!/bin/bash
# Runs Android shell commands in the Droid app on the iPhone through files (no networking):
# needs "debugShell": true in Library/Android/debug.json. A line "monitor <cmd>" goes to QEMU's
# monitor instead (debug.json extraArgs -monitor tcp:127.0.0.1:4444,server=on,wait=off).
#   scripts/dev-cmd.sh 'dumpsys window | grep mCurrentFocus' 'monitor info cpus'
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/build/local.env"
T=$(mktemp -d)
{ echo "echo ==== $RANDOM$RANDOM"; printf '%s\n' "$@"; } > "$T/cmd"   # the marker makes every answer new
cp() { xcrun devicectl device copy "$@" --device "$DEV" --domain-type appDataContainer --domain-identifier com.tomergamertv.oni >/dev/null 2>&1; }
cp from --source Library/Android/debug-cmd.out --destination "$T/old" || true
cp to --source "$T/cmd" --destination Library/Android/debug-cmd
for _ in $(seq 1 60); do
    sleep 2
    if cp from --source Library/Android/debug-cmd.out --destination "$T/out" && ! cmp -s "$T/out" "$T/old" 2>/dev/null; then
        cat "$T/out"; rm -rf "$T"; exit 0
    fi
done
echo "dev-cmd: no answer" >&2; rm -rf "$T"; exit 1
