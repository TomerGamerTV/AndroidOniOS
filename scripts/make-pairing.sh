#!/bin/bash
# Creates the RemotePairing file StikJIT needs (iOS 26+) over USB, without prompts, and copies it
# into the app (Library/Android/pairingFile.plist). The iPhone or iPad must be connected, unlocked and trusted.
# usage: scripts/make-pairing.sh [UDID]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEV=${1:-${DEV:?usage: make-pairing.sh <iPhone UDID>}}
SRC="$ROOT/build/src/idevice-src"
export PATH="$HOME/.cargo/bin:$PATH"
[ -d "$SRC" ] || git clone --depth 1 https://github.com/jkcoxson/idevice.git "$SRC"
[ -x "$SRC/target/release/idevice-tools" ] || (cd "$SRC/tools" && cargo +1.92.0 build --release)
OUT="$ROOT/build/rp_pairing_file-$DEV.plist"   # one per device (private keys; build/ is gitignored)
"$SRC/target/release/idevice-tools" --udid "$DEV" remotepairing_lockdown pair StikJIT "$OUT"
chmod 600 "$OUT"
xcrun devicectl device copy to --device "$DEV" --domain-type appDataContainer \
    --domain-identifier com.tomergamertv.oni --source "$OUT" --destination Library/Android/pairingFile.plist
echo "pairing file installed"
