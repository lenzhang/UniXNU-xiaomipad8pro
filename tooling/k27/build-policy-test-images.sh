#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The test images of docs/policy.md section 9 (PianoPolicy: the cover, the power key, the keyboard's display wake, Low Power Mode),
# in the order of risk, from a KC with the 0062 kernel and PianoKeyboard. Each is a RAM image of the GPU profile with the drivers the
# policy watches (PianoKeyboard level 2 with piano-keyboard-lid=0, PianoPower's battery as macOS's power source, PianoBacklight level
# 2 for the display power, PianoThermal's governor), the USB console, the buttons and piano-reset-after as the backstop.
#
#   build-policy-test-images.sh [NAME ...]    default: all of observe lock sleep lpm suspend; out/ram-pol-<NAME>/ and out/ram-pol-dump/
#                                             (chargesleep only by name: it starts the DSP, PianoPower level 2)
#
# Inputs (this repository's out/ unless set): POL_KC (out/k27/kc-k27-native.bin, built by
#   K27_KERNEL=<kernel-k27-0062-native.macho> PIANO_EXTRA_KEXTS=$(tooling/k27/build-native-piano-keyboard.sh | tail -1) \
#   PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh), POL_ADT (out/adt/piano-k27.adt), POL_FD, POL_SEED, POL_TRUST
# (the Turnip stage), POL_EXTRA (added to every image). The KC is checked for PianoPolicy, PianoKeyboard and the 0062 kernel.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${POL_KC:-out/k27/kc-k27-native.bin}
ADT=${POL_ADT:-out/adt/piano-k27.adt}
FD=${POL_FD:-out/fd/piano-uefi-A-bundled.fd}
SEED=${POL_SEED:-out/turnip-a830/stage/scratch-data.img}
TRUST=${POL_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
for f in "$KC" "$ADT" "$FD" "$SEED" "$TRUST"; do [ -f "$f" ] || { echo "build-policy-test-images: $f is missing" >&2; exit 1; }; done
for s in com.unixnu.driver.PianoPolicy com.unixnu.driver.PianoKeyboard piano_s2idle_enter; do
  grep -a -q "$s" "$KC" || { echo "build-policy-test-images: $KC has no $s (PianoPolicy, PianoKeyboard and the 0062 kernel are needed)" >&2; exit 1; }
done
# The drivers the policy reads, at the levels that publish what it reads. PIANO_FAST_BOOT=1 keeps the command line under 1023 bytes.
# PW, TH: PianoPower's and PianoThermal's levels, per image (the kernel takes the first occurrence of an argument, so an image cannot
# raise a level BASE already gives).
BASE="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2 piano-input=1 piano-power-ps=1"
BASE="$BASE piano-power-period=10 piano-keyboard=2 piano-backlight=2 piano-cpufreq=3 ${POL_EXTRA:-}"
ORDER="observe lock sleep lpm suspend"
image() {
  PW="piano-power=1" TH="piano-thermal=1"
  case $1 in
    observe) A="piano-policy=1 piano-policy-lid=1";                                      R=900 ;;    # decisions only
    lock)    A="piano-policy=2 piano-policy-lid=1 piano-backlight-lid=1";                R=1200 ;;   # no system sleep: closing locks
    sleep)   A="piano-policy=2 piano-policy-lid=1 piano-backlight-lid=1 piano-sleep=1 piano-sleep-state=1"
             A="$A piano-sleep-wake=19 piano-sleep-max=600";                             R=1800 ;;   # closing on battery sleeps
    lpm)     TH="piano-thermal=2"
             A="piano-policy=2 piano-gpu-dcvs-max=607";                                  R=900 ;;    # the profile and the ceiling
    # every driver of power.md 4 that joins the power tree in a suspend: PianoInput, PianoDwc3 (the Mac kept), PianoKeyboard (the
    # MCU blanked, gpio95 latched), PianoBacklight, PianoThermal, NativeGopDisplay's display power; 3 test sleeps of 30 s, then by hand
    suspend) A="piano-policy=2 piano-policy-lid=1 piano-backlight-lid=1 piano-keyboard-blank=1 piano-sleep=1 piano-sleep-state=1"
             A="$A piano-sleep-wake=19 piano-sleep-max=30 piano-sleep-test=150 piano-sleep-test-count=3";    R=900 ;;
    # the same with PianoPower's charger handover (level 2: the DSP started, ChargePolicy; docs/drivers/charging-policy.md 8.5)
    chargesleep) PW="piano-power=2 piano-power-policy=1"
             A="piano-policy=2 piano-keyboard-blank=1 piano-sleep=1 piano-sleep-state=1 piano-sleep-wake=17"
             A="$A piano-sleep-max=30 piano-sleep-test=240 piano-sleep-test-count=2";       R=900 ;;
    *) return 1 ;;
  esac
}
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
for n in ${*:-$ORDER}; do
  image "$n" || { echo "build-policy-test-images: no image $n (one of: $ORDER)" >&2; exit 2; }
  PIANO_FAST_BOOT=1 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=$R PIANO_EXTRA_ARGS="$PW $TH $BASE $A" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-pol-$n" | tail -1
done
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-pol-dump \
  | tail -1
shasum -a 256 out/ram-pol-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
