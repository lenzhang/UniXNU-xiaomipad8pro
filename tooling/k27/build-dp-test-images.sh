#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The test images of docs/drivers/displayport.md section 8.2 (PianoDp's first slices, B8): D1 the read-only survey, D2 the port events
# with a monitor or dock (attended), D6 the fake second display (no hardware) in four variants. Each is a RAM image of the GPU profile
# (the login screen of 2026-11-11: PianoA830, the Turnip stage, ADT vmm-present 0) with the USB console and piano-reset-after as the
# backstop; PIANO_FAST_BOOT=1 keeps the command line short.
#
#   build-dp-test-images.sh [NAME ...]    default: all of d1 d2 d6a d6b d6c d6d; out/ram-dp-<NAME>/ and out/ram-dp-dump/
#
# Inputs (this repository's out/ unless set): DP_KC (out/k27/kc-k27-native.bin, built by
#   PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh), DP_ADT (out/adt/piano-k27.adt from the same build), DP_FD,
#   DP_SEED, DP_TRUST (the Turnip stage), DP_EXTRA (added to every image). The KC is checked for PianoDp, D6's personality and PianoA830.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${DP_KC:-out/k27/kc-k27-native.bin}
ADT=${DP_ADT:-out/adt/piano-k27.adt}
FD=${DP_FD:-out/fd/piano-uefi-A-bundled.fd}
SEED=${DP_SEED:-out/turnip-a830/stage/scratch-data.img}
TRUST=${DP_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
for f in "$KC" "$ADT" "$FD" "$SEED" "$TRUST"; do [ -f "$f" ] || { echo "build-dp-test-images: $f is missing" >&2; exit 1; }; done
for s in com.unixnu.driver.PianoDp NativeGopDisplayRole com.unixnu.driver.PianoA830; do
  grep -a -q "$s" "$KC" || { echo "build-dp-test-images: $KC has no $s (build it with PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh)" >&2; exit 1; }
done
# The GPU profile with the USB console; PianoPower level 1 (battery readout, the DSP not started) unless the image needs the DSP.
GPU="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2"
# D6's thumbnails: the first frame of each display, then every 30 s, at most 8 per display (the console ring is 7 MiB).
THUMBS="native-gop-display-thumb=30 native-gop-display-thumbs=8"
ORDER="d1 d2 d6a d6b d6c d6d"
image() {
  case $1 in
    d1)  A="piano-power=1 piano-dp=1";                                                      R=300 ;;    # D1: the survey, unattended
    d2)  A="piano-power=2 piano-power-write=0 piano-dp=1";                                  R=1200 ;;   # D2: the DSP's port events, attended
    d6a) A="piano-power=1 piano-dp=1 piano-dp-survey=0 piano-dp-fake=180 $THUMBS";          R=480 ;;    # hot-plug after the login screen
    d6b) A="piano-power=1 piano-dp=1 piano-dp-survey=0 piano-dp-fake-boot=1 $THUMBS";       R=420 ;;    # two displays fixed from boot
    d6c) A="piano-power=1 piano-dp=1 piano-dp-survey=0 piano-dp-fake=180 piano-dp-fake-primary=1 $THUMBS"; R=480 ;;   # primary 1
    d6d) A="piano-power=1 piano-dp=1 piano-dp-survey=0 piano-dp-fake=180 piano-dp-fake-unplug=30 piano-dp-fake-cycles=5 $THUMBS"
         R=660 ;;                                                                                       # 5 plugs, 4 unplugs
    *) return 1 ;;
  esac
}
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
for n in ${*:-$ORDER}; do
  image "$n" || { echo "build-dp-test-images: no image $n (one of: $ORDER)" >&2; exit 2; }
  PIANO_FAST_BOOT=1 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=$R PIANO_EXTRA_ARGS="$GPU $A ${DP_EXTRA:-}" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-dp-$n" | tail -1
done
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-dp-dump \
  | tail -1
shasum -a 256 out/ram-dp-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
