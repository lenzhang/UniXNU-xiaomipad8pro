#!/bin/bash
# One unattended native boot of piano, from the Mac, with the tablet on USB-C and Ubuntu running:
#   tooling/native/native-run.sh IMAGE [WAIT_SECONDS]
# Ubuntu -> `sudo piano-reboot-bootloader` -> ABL fastboot -> `fastboot boot IMAGE` (RAM only, never flash) -> XNU runs until
# piano-reset-after (or a panic) resets the tablet -> ABL boots Ubuntu -> the XNU console is read back from
# the cvp@9cb00000 mailbox (k27 0055, tooling/native/xnu-log.py; Ubuntu's ramoops zone is overwritten by the firmware)
# into logs/native-runs/<timestamp>/. Nothing is written to any partition.
# The tablet's sudo password is read from ~/.config/piano/tablet-sudo (mode 600, outside every repository; PROJECT-MAP
# rule 4) and given to sudo on stdin only. WAIT_SECONDS (default 420): how long to wait for Ubuntu to come back.
set -euo pipefail
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); WAIT=${2:-420}
D=$(cd "$(dirname "$0")/../.." && pwd)
# Machine-local settings (the tablet's ssh target, its fastboot serial) live outside the repository:
# ~/.config/piano/env with PIANO_HOST=user@host and PIANO_SERIAL=<serial>.
[ -f "$HOME/.config/piano/env" ] && . "$HOME/.config/piano/env"
T=${PIANO_HOST:?set PIANO_HOST in ~/.config/piano/env}; SERIAL=${PIANO_SERIAL:?set PIANO_SERIAL in ~/.config/piano/env}
PW=${PIANO_SUDO_FILE:-$HOME/.config/piano/tablet-sudo}
export PATH=/opt/homebrew/share/android-commandlinetools/platform-tools:$PATH
R=$D/logs/native-runs/$(date +%Y%m%d-%H%M%S); mkdir -p "$R"
log() { echo "$(date +%T) $*" | tee -a "$R/run.log"; }
tsudo() { ssh -o BatchMode=yes -o ConnectTimeout=5 "$T" "sudo -S -p '' sh -c $(printf %q "$1")" < "$PW"; }
up() { ssh -o BatchMode=yes -o ConnectTimeout=3 "$T" true 2>/dev/null; }
log "image $IMG sha256 $(shasum -a 256 "$IMG" | cut -c1-64)"
if ! up; then
  log "Ubuntu not reachable; waiting for it (or for ABL fastboot)"
  for i in $(seq 60); do up && break; fastboot devices | grep -q "$SERIAL" && break; sleep 5; done
fi
BOOT_ID=""
if up; then
  BOOT_ID=$(ssh -o BatchMode=yes "$T" cat /proc/sys/kernel/random/boot_id)
  scp -q "$D/tooling/native/xnu-log.py" "$T:.piano-xnu-log.py"
  tsudo 'python3 /home/leo/.piano-xnu-log.py clear' >/dev/null
  log "Ubuntu up (boot $BOOT_ID); mailbox cleared; rebooting to the bootloader"
  tsudo 'nohup piano-reboot-bootloader >/dev/null 2>&1 &' || true
fi
for i in $(seq 90); do fastboot devices | grep -q "$SERIAL" && break; sleep 2; done
fastboot devices | grep -q "$SERIAL" || { log "no ABL fastboot device $SERIAL"; exit 1; }
log "ABL fastboot: fastboot boot (RAM only)"
fastboot -s "$SERIAL" boot "$IMG" 2>&1 | tee -a "$R/run.log"
T0=$(date +%s)
# USB timeline: ABL -> our UEFI (piano-uefi fastboot) -> gone when DarwinBoot exits boot services
( for i in $(seq 60); do echo "$(date +%T) [$(fastboot devices | tr '\t\n' '  ')]"; sleep 2; done ) > "$R/usb.log" 2>&1 &
log "waiting up to $WAIT s for Ubuntu to come back"
for i in $(seq $((WAIT / 2))); do
  sleep 2
  if up; then
    NB=$(ssh -o BatchMode=yes "$T" cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)
    [ -n "$NB" ] && [ "$NB" != "$BOOT_ID" ] && break
  fi
done
up || { log "Ubuntu did not come back within $WAIT s (tablet may be parked: power + volume-down 15-20 s)"; exit 2; }
log "Ubuntu back after $(( $(date +%s) - T0 )) s; reading pstore"
scp -q "$D/tooling/native/xnu-log.py" "$T:.piano-xnu-log.py"
tsudo 'python3 /home/leo/.piano-xnu-log.py read' > "$R/xnu-console.txt" 2> "$R/xnu-log-reader.txt" || true
cat "$R/xnu-log-reader.txt" | tee -a "$R/run.log"
log "xnu-console.txt: $(wc -c < "$R/xnu-console.txt") bytes ($R)"
tail -25 "$R/xnu-console.txt" | tee -a "$R/run.log"
