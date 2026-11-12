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
# and those of section 17 (the 0 mA of 2026-11-12: the charger stops charging at about 40 s of every native boot and the firmware,
# once it runs, keeps the input on and the battery at 0 mA), 900 s each:
#
#   kick       ramp's arguments with this tree's PianoPower: the charging restart (piano-power-kick=1, the policy's default: 0x2003 =
#              0 then 1 when both packs stay at 0 mA with input to spare), the charge current of a trusted Rp (1500 mA), the steps
#              measured against the firmware's own 0x2029, and the `charging:` balance line every report
#   kick-idle  kick without PerfPowerServices and audiomxd (PIANO_DENY_EXEC, k27-deny-exec: power.md 1.2, the p9 images' fix; in
#              p9-off every CPU idled 98.7-99.9 %): the idle login window's load, so more of the input is left for the battery
#
#   build-charge-test-images.sh [NAME ...]   default: kick kick-idle; out/ram-chg-<NAME>/ and out/ram-chg-dump/
#
# Inputs (this repository's out/ unless set): CHG_KC (out/k27/kc-k27-native.bin with this tree's PianoPower, PianoThermal and
# PianoA830: K27_KERNEL=<kernel-k27-0063-native.macho> PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh), CHG_ADT
# (out/adt/piano-k27.adt), CHG_FD, CHG_SEED / CHG_TRUST (the Turnip stage, out/turnip-a830/stage: the login window as the daily
# shows it), CHG_EXTRA (added to every image). Data is volatile (the staged seed is not the installed one).
#
# The verdict lines (grep -E 'PianoPower: (policy: (input steps|charging restart)|\[[0-9]+s\] (battery|input|policy|charging)|mca: (write|late))'):
# `input: bus V mV I mA, limit L mA asked, 0x2029 N; steps S mA (...)`, `battery: V mV, I mA (charging|discharging)` and (this tree)
# `charging: in I mA at V mV (W), battery +I mA at V mV (W), system ~W; ...; <verdict>` every 30 s; pass for ramp: the packs at or
# above 0 mA at the login window from about 120 s on, the bus current above l1's inferred 1.4-1.5 A, no `back from` line (or one with
# its reason), no `FATAL`. Pass for kick and kick-idle: `charging restart 1 of 3: ...` once the packs read 0 mA (about 150 s), then
# `charging restart 1: the packs take +N mA now: CHARGING again` and `charging: ... charging` lines to the end, the steps on past
# 2000 mA; `no change` three times means 0x2003 does not restart this charger (then the next suspect is the input's own suspend and
# replug, attended: charging-policy.md §17.6).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${CHG_KC:-out/k27/kc-k27-native.bin}
ADT=${CHG_ADT:-out/adt/piano-k27.adt}
FD=${CHG_FD:-out/fd/piano-uefi-A-bundled.fd}
SEED=${CHG_SEED:-out/turnip-a830/stage/scratch-data.img}
TRUST=${CHG_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
for f in "$KC" "$ADT" "$FD" "$SEED" "$TRUST"; do [ -f "$f" ] || { echo "build-charge-test-images: $f is missing" >&2; exit 1; }; done
NEED="com.unixnu.driver.PianoPower com.unixnu.driver.PianoThermal com.unixnu.driver.PianoA830 piano-power-rp-max"
case " ${*:-kick} " in *" kick"*) NEED="$NEED piano-power-kick" ;; esac   # the restart and the balance line: this tree's PianoPower
for s in $NEED; do
  grep -a -q "$s" "$KC" || { echo "build-charge-test-images: $KC has no $s (this tree's PianoPower, PianoThermal, PianoA830)" >&2; exit 1; }
done
# The daily profile's drivers (build-esp.py PROFILES['daily'] without the Mac's keyboard/pointer, which an unattended run does not
# use) and PianoThermal's ADC, which the Rp steps above 1.5 A need for a connector temperature. No CPU frequency argument: the clocks
# UEFI left, as the daily runs.
BASE="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2 piano-thermal=1 piano-thermal-adc=1 ${CHG_EXTRA:-}"
ORDER="kick kick-idle"
ALL="ramp l1 late kick kick-idle"
# the daemons that spun a core at the idle login window in every boot of 2026-11-12 (power.md 1.2)
IDLE_DENY=/usr/libexec/PerfPowerServices,/usr/libexec/audiomxd
image() {
  R=600 DENY=
  case $1 in
    ramp) PW="piano-power=2 piano-power-policy=1 piano-power-rp=1" ;;
    l1)   PW="piano-power=1" ;;
    late) PW="piano-power=2 piano-power-policy=1 piano-power-rp=1 piano-power-ramp=0 piano-power-write-ms=1000" ;;
    kick) PW="piano-power=2 piano-power-policy=1 piano-power-rp=1"; R=900 ;;
    kick-idle) PW="piano-power=2 piano-power-policy=1 piano-power-rp=1"; R=900 DENY=$IDLE_DENY ;;
    *) return 1 ;;
  esac
}
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
for n in ${*:-$ORDER}; do
  image "$n" || { echo "build-charge-test-images: no image $n (one of: $ALL)" >&2; exit 2; }
  PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=$R PIANO_DENY_EXEC=$DENY PIANO_EXTRA_ARGS="$PW $BASE" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-chg-$n" | tail -1
done
PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-chg-dump | tail -1
shasum -a 256 out/ram-chg-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
