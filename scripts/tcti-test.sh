#!/bin/bash
# Rebuilds the macOS TCTI QEMU and checks whether Android's init links (TCTI bisection helper).
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
REBUILD=1 bash scripts/build-qemu.sh macos-tcti > build/qemu-macos-tcti.log 2>&1 || { echo BUILD-FAILED; grep -E "error:" build/qemu-macos-tcti.log | head -5; exit 1; }
pkill -9 -f vm-macos/qemu; sleep 1
(QBIN=$ROOT/build/src/qemu-10.0.12-utm/build-macos-tcti/qemu-system-aarch64-unsigned ACCEL="tcg,thread=multi,tb-size=512" \
  DISPLAY_OPTS="-display none" bash scripts/run-macos.sh > build/run-macos-tcti.out 2>&1 &)
for i in $(seq 1 120); do
    sleep 3
    if grep -aq "CANNOT LINK" build/vm-macos/kernel.log 2>/dev/null; then echo "RESULT: FAIL (init cannot link) after $((i*3))s"; break; fi
    if grep -aq "init second stage started" build/vm-macos/kernel.log 2>/dev/null; then echo "RESULT: PASS (second stage reached) after $((i*3))s"; break; fi
done
pkill -9 -f vm-macos/qemu
