#!/bin/bash
# Native runs without Ubuntu in the loop (2026-11-10). The tablet waits in ABL fastboot; each round:
#   fastboot boot IMAGE -> our UEFI comes up -> during its menu window (PIANO_MENU_TIMEOUT, 5 s) the PREVIOUS run's XNU log
#   is pulled from the cvp@9cb00000 mailbox (fastboot -s piano-uefi oem dump: read-only, our in-RAM UEFI, never ABL)
#   -> the menu starts XNU -> piano-reset-after resets the tablet -> k27 0057 set the PMIC restart reason, so ABL stays in
#   fastboot -> next round. RAM boots only; nothing is written to any partition.
#   tooling/native/native-loop.sh IMAGE [ROUNDS]      (start with the tablet in ABL fastboot, or Ubuntu up)
# Logs: logs/native-runs/<ts>-loop/round-N/{xnu-console.txt,dump.bin,usb.log}. ROUNDS (default 1) XNU runs, then one more
# boot of IMAGE only to read the last log, after which the tablet is sent back with `fastboot -s piano-uefi reboot`.
set -euo pipefail
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); ROUNDS=${2:-1}
D=$(cd "$(dirname "$0")/../.." && pwd)
[ -f "$HOME/.config/piano/env" ] && . "$HOME/.config/piano/env"
T=${PIANO_HOST:?set PIANO_HOST in ~/.config/piano/env}; SERIAL=${PIANO_SERIAL:?set PIANO_SERIAL in ~/.config/piano/env}
PW=${PIANO_SUDO_FILE:-$HOME/.config/piano/tablet-sudo}
export PATH=/opt/homebrew/share/android-commandlinetools/platform-tools:$PATH
L=$D/logs/native-runs/$(date +%Y%m%d-%H%M%S)-loop; mkdir -p "$L"
log() { echo "$(date +%T) $*" | tee -a "$L/loop.log"; }
abl() { fastboot devices | grep -q "^$SERIAL"; }
uefi() { fastboot devices | grep -q "^piano-uefi"; }
wait_for() { local what=$1 secs=$2; for i in $(seq $((secs * 2))); do $what && return 0; sleep 0.5; done; return 1; }
log "image $(shasum -a 256 "$IMG" | cut -c1-16) rounds $ROUNDS"
if ! abl; then
  if ssh -o BatchMode=yes -o ConnectTimeout=3 "$T" true 2>/dev/null; then
    log "Ubuntu up: rebooting it to the bootloader"
    ssh -o BatchMode=yes "$T" "sudo -S -p '' sh -c 'nohup piano-reboot-bootloader >/dev/null 2>&1 &'" < "$PW"
  fi
  wait_for abl 120 || { log "no ABL fastboot"; exit 1; }
fi
for r in $(seq 0 "$ROUNDS"); do
  R=$L/round-$r; mkdir -p "$R"; t0=$(date +%s)
  log "round $r: fastboot boot"
  fastboot -s "$SERIAL" boot "$IMG" > "$R/fastboot.log" 2>&1 || { cat "$R/fastboot.log"; exit 1; }
  wait_for uefi 40 || { log "round $r: our UEFI did not appear"; exit 1; }
  log "round $r: UEFI up after $(( $(date +%s) - t0 )) s; dumping the previous log"
  if fastboot -s piano-uefi oem dump 9cb00000 700000 > /dev/null 2>&1 && (cd "$R" && fastboot -s piano-uefi get_staged dump.bin > /dev/null 2>&1); then
    if python3 "$D/tooling/native/xnu-log.py" decode "$R/dump.bin" > "$R/xnu-console.txt" 2> "$R/reader.txt"; then
      log "round $r: previous log $(cat "$R/reader.txt")"
      LC_ALL=C tr -c '\11\12\15\40-\176' '?' < "$R/xnu-console.txt" | tail -4 | sed 's/^/    | /' | tee -a "$L/loop.log"
    else log "round $r: $(cat "$R/reader.txt")"; fi
  else log "round $r: dump failed (menu window missed?)"; fi
  if [ "$r" = "$ROUNDS" ]; then
    log "last round: sending the tablet back (fastboot -s piano-uefi reboot)"
    fastboot -s piano-uefi reboot > /dev/null 2>&1 || true
    break
  fi
  wait_for "eval ! uefi" 30 || true
  log "round $r: XNU started after $(( $(date +%s) - t0 )) s; waiting for the reset into ABL fastboot"
  wait_for abl 240 || { log "round $r: no ABL fastboot within 240 s (parked? power + volume-down 15-20 s)"; exit 2; }
  log "round $r: back in ABL fastboot after $(( $(date +%s) - t0 )) s"
done
