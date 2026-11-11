#!/bin/bash
# One native test without Ubuntu (2026-11-11). The tablet waits in ABL fastboot:
#   fastboot boot IMAGE -> XNU runs until piano-reset-after resets it (k27 0057: ABL stays in fastboot)
#   -> fastboot boot DUMP (out/ram-dump: the same with piano-reset-after=1) -> while its UEFI runs, the test's XNU log is pulled from
#   the cvp@9cb00000 mailbox (fastboot -s piano-uefi oem dump: read-only, our in-RAM UEFI, never ABL) -> DUMP's XNU resets at once
#   -> the tablet is parked in ABL fastboot again. RAM boots only.
#   tooling/native/native-loop.sh IMAGE [DUMP_IMAGE]
# Logs: logs/native-runs/<ts>-loop/{loop.log,dump.bin,xnu-console.txt,reader.txt}.
set -euo pipefail
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
D=$(cd "$(dirname "$0")/../.." && pwd)
DUMP=${2:-$D/out/ram-dump/$(basename "$1")}
[ -f "$HOME/.config/piano/env" ] && . "$HOME/.config/piano/env"
SERIAL=${PIANO_SERIAL:?set PIANO_SERIAL in ~/.config/piano/env}
export PATH=/opt/homebrew/share/android-commandlinetools/platform-tools:$PATH
L=$D/logs/native-runs/$(date +%Y%m%d-%H%M%S)-loop; mkdir -p "$L"
log() { echo "$(date +%T) $*" | tee -a "$L/loop.log"; }
abl() { fastboot devices | grep -q "^$SERIAL"; }
uefi() { fastboot devices | grep -q "^piano-uefi"; }
wait_for() { local what=$1 secs=$2; for i in $(seq $((secs * 2))); do $what && return 0; sleep 0.5; done; return 1; }
RESET=$(strings "$IMG" | grep -o 'piano-reset-after=[0-9][0-9]*' | head -1 | cut -d= -f2); RESET=${RESET:-45}
log "image $(shasum -a 256 "$IMG" | cut -c1-16), reset after ${RESET} s; dump image $(shasum -a 256 "$DUMP" | cut -c1-16)"
wait_for abl 240 || { log "no ABL fastboot (parked kernel? power + volume-down 15-20 s)"; exit 1; }
t0=$(date +%s)
fastboot -s "$SERIAL" boot "$IMG" > "$L/fastboot.log" 2>&1 || { cat "$L/fastboot.log"; exit 1; }
wait_for uefi 40 || { log "our UEFI did not appear"; exit 1; }
wait_for "eval ! uefi" 40 || true
log "XNU started after $(( $(date +%s) - t0 )) s"
wait_for abl $((RESET + 150)) || { log "no ABL fastboot within $((RESET + 150)) s (parked? power + volume-down 15-20 s)"; exit 2; }
log "back in ABL fastboot after $(( $(date +%s) - t0 )) s; dump boot"
fastboot -s "$SERIAL" boot "$DUMP" >> "$L/fastboot.log" 2>&1 || { cat "$L/fastboot.log"; exit 1; }
wait_for uefi 40 || { log "dump: our UEFI did not appear"; exit 1; }
if fastboot -s piano-uefi oem dump 9cb00000 700000 > /dev/null 2>&1 && (cd "$L" && fastboot -s piano-uefi get_staged dump.bin > /dev/null 2>&1); then
  if python3 "$D/tooling/native/xnu-log.py" decode "$L/dump.bin" > "$L/xnu-console.txt" 2> "$L/reader.txt"; then
    log "log: $(cat "$L/reader.txt")"
    LC_ALL=C tr -c '\11\12\15\40-\176' '?' < "$L/xnu-console.txt" | tail -4 | sed 's/^/    | /' | tee -a "$L/loop.log"
  else log "$(cat "$L/reader.txt")"; fi
else log "dump failed (UEFI gone before the dump?)"; fi
wait_for "eval ! uefi" 40 || true
wait_for abl 120 && log "parked in ABL fastboot after $(( $(date +%s) - t0 )) s" || log "not back in ABL fastboot within 120 s"
