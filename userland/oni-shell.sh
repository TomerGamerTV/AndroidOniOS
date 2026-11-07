#!/bin/bash
# Runs a shell command inside the running onirun Android (as root) and prints its output.
#   userland/oni-shell.sh 'dumpsys input | head'
T="$(cd "$(dirname "$0")/.." && pwd)/build/userland/rootfs/data/local/tmp"
rm -f "$T/oni-cmd.out"
printf '%s\n' "$*" > "$T/oni-cmd.tmp" && mv "$T/oni-cmd.tmp" "$T/oni-cmd"
for _ in $(seq 1 600); do [ -f "$T/oni-cmd.out" ] && { cat "$T/oni-cmd.out"; exit 0; }; sleep 0.5; done
echo "oni-shell: no answer (is Android running?)" >&2; exit 1
