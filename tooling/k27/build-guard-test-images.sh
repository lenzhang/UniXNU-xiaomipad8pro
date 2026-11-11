#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The boot guard's test images (docs/boot-guard.md §5, k27 0063), from a KC whose kernel has 0063. Each is a RAM image of the GPU
# profile (as the installed system boots: PianoA830 and Turnip, vmm-present 0, the USB console and the buttons) WITHOUT
# piano-reset-after and piano-reset-to (PIANO_RESET_AFTER=0): the guard is the only thing that resets the tablet, as on the
# installed system. Data is volatile in all of them (data-persist=0): a deliberate hang never meets an open store.
#
#   build-guard-test-images.sh [NAME ...]     default: all; out/ram-guard-<NAME>/ and one dump image out/ram-guard-dump/
#
#   ping0      piano-boot-guard=60 piano-boot-guard-ping=0         armed, never pinged: the bite -> ABL fastboot
#   hang       piano-boot-guard=60 piano-boot-guard-test=hang-at=40  every CPU masked at 40 s -> ABL fastboot
#   nohealth   piano-boot-guard=60 piano-boot-guard-exec=piano-nosuch  no milestone by 60 s -> PSCI reset -> ABL fastboot
#   healthy    piano-boot-guard=60                                  healthy at 60 s, restart reason normal; stays up
#   latehang   piano-boot-guard=60 piano-boot-guard-test=hang-at=100  healthy, then the hang -> the INSTALLED system boots
#   sleep      piano-boot-guard=60 + three test suspends (k27 0062)  the watchdog stays held across the wake windows
#
# Inputs (this repository's out/ unless set): GUARD_KC (out/k27/kc-k27-native.bin, built by
# `K27_KERNEL=<kernel-k27-0063-native.macho> PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh`), GUARD_ADT
# (out/adt/piano-k27.adt, the ADT that build wrote), GUARD_FD, GUARD_SEED, GUARD_TRUST (the Turnip stage), GUARD_EXTRA (more
# arguments for every image). The KC's kernel is checked for 0063.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${GUARD_KC:-out/k27/kc-k27-native.bin}
ADT=${GUARD_ADT:-out/adt/piano-k27.adt}
FD=${GUARD_FD:-out/fd/piano-uefi-A-bundled.fd}
SEED=${GUARD_SEED:-out/turnip-a830/stage/scratch-data.img}
TRUST=${GUARD_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
for f in "$KC" "$ADT" "$FD" "$SEED" "$TRUST"; do [ -f "$f" ] || { echo "build-guard-test-images: $f is missing" >&2; exit 1; }; done
grep -a -q "piano-boot-guard: healthy after" "$KC" || { echo "build-guard-test-images: $KC's kernel has no k27 0063" >&2; exit 1; }
BASE="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-power=1 piano-usb=2 piano-input=1 ${GUARD_EXTRA:-}"
G="piano-boot-guard=60"
ORDER="ping0 hang nohealth healthy latehang sleep"
image() {
  case $1 in
    ping0)    A="$G piano-boot-guard-ping=0" ;;
    hang)     A="$G piano-boot-guard-test=hang-at=40" ;;
    nohealth) A="$G piano-boot-guard-exec=piano-nosuch" ;;
    healthy)  A="$G" ;;
    latehang) A="$G piano-boot-guard-test=hang-at=100" ;;
    sleep)    A="$G piano-sleep=1 piano-sleep-state=1 piano-sleep-test=90 piano-sleep-test-count=3 piano-sleep-max=30" ;;
    *) return 1 ;;
  esac
}
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
for n in ${*:-$ORDER}; do
  image "$n" || { echo "build-guard-test-images: no image $n (one of: $ORDER)" >&2; exit 2; }
  PIANO_FAST_BOOT=1 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=0 PIANO_DATA_PERSIST=0 PIANO_EXTRA_ARGS="$BASE $A" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-guard-$n" | tail -1
done
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" \
  --out out/ram-guard-dump | tail -1
shasum -a 256 out/ram-guard-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
