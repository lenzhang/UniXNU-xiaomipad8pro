#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The charging test images of docs/drivers/charging-policy.md section 15 (2026-11-12): unattended RAM images of the daily profile's
# drivers (the GPU at the login window, the USB console, PianoThermal's ADC for the connector thermistors) on the Mac's USB-C cable,
# each a 600 s run, that decide the daily profile's charging level by the battery's sign at the login window:
#
#   ramp   the candidate daily (build-esp.py --charging 2): level 2, ChargePolicy, the stepped Type-C current (piano-power-rp=1 with
#          the default piano-power-ramp=1, up to piano-power-rp-max=3000): the CDP row 900 mA, then 1500, 2000, 2500, 3000 mA while
#          the bus holds 4.65 V and the input is used up; a sag, a quick detach or a refusal: one step back for the boot
#   l1     the same drivers at level 1 (the DSP not started: the PMIC on its own, as the installed daily): the A/B partner of ramp
#   late   level 2 with the Rp limit at once (piano-power-ramp=0: 1600 mA, P2 of 2026-11-12 again, now the input limit first) and the
#          old 1 s write window (piano-power-write-ms=1000): whether the firmware's answers to the first limit writes come late
#          ("mca: late answer to the write of ...") or never
#
#   build-charge-test-images.sh [NAME ...]   default: ramp l1 late; out/ram-chg-<NAME>/ and out/ram-chg-dump/
#
# Inputs (this repository's out/ unless set): CHG_KC (out/k27/kc-k27-native.bin with this tree's PianoPower, PianoThermal and
# PianoA830: K27_KERNEL=<kernel-k27-0063-native.macho> PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh), CHG_ADT
# (out/adt/piano-k27.adt), CHG_FD, CHG_SEED / CHG_TRUST (the Turnip stage, out/turnip-a830/stage: the login window as the daily
# shows it), CHG_EXTRA (added to every image). Data is volatile (the staged seed is not the installed one).
#
# The verdict lines (grep -E 'PianoPower: (policy: input steps|\[[0-9]+s\] (battery|input|policy)|mca: (write|late))'): `input: bus V mV
# I mA, limit L mA asked, 0x2029 N; steps S mA (...)` and `battery: V mV, I mA (charging|discharging)` every 30 s; pass for ramp: the
# packs at or above 0 mA at the login window from about 120 s on, the bus current above l1's inferred 1.4-1.5 A, no `back from` line
# (or one with its reason), no `FATAL`.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${CHG_KC:-out/k27/kc-k27-native.bin}
ADT=${CHG_ADT:-out/adt/piano-k27.adt}
FD=${CHG_FD:-out/fd/piano-uefi-A-bundled.fd}
SEED=${CHG_SEED:-out/turnip-a830/stage/scratch-data.img}
TRUST=${CHG_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
for f in "$KC" "$ADT" "$FD" "$SEED" "$TRUST"; do [ -f "$f" ] || { echo "build-charge-test-images: $f is missing" >&2; exit 1; }; done
for s in com.unixnu.driver.PianoPower com.unixnu.driver.PianoThermal com.unixnu.driver.PianoA830 piano-power-rp-max; do
  grep -a -q "$s" "$KC" || { echo "build-charge-test-images: $KC has no $s (this tree's PianoPower, PianoThermal, PianoA830)" >&2; exit 1; }
done
# The daily profile's drivers (build-esp.py PROFILES['daily'] without the Mac's keyboard/pointer, which an unattended run does not
# use) and PianoThermal's ADC, which the Rp steps above 1.5 A need for a connector temperature. No CPU frequency argument: the clocks
# UEFI left, as the daily runs.
BASE="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2 piano-thermal=1 piano-thermal-adc=1 ${CHG_EXTRA:-}"
ORDER="ramp l1 late"
image() {
  case $1 in
    ramp) PW="piano-power=2 piano-power-policy=1 piano-power-rp=1" ;;
    l1)   PW="piano-power=1" ;;
    late) PW="piano-power=2 piano-power-policy=1 piano-power-rp=1 piano-power-ramp=0 piano-power-write-ms=1000" ;;
    *) return 1 ;;
  esac
}
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
for n in ${*:-$ORDER}; do
  image "$n" || { echo "build-charge-test-images: no image $n (one of: $ORDER)" >&2; exit 2; }
  PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=600 PIANO_EXTRA_ARGS="$PW $BASE" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-chg-$n" | tail -1
done
PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-chg-dump | tail -1
shasum -a 256 out/ram-chg-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
