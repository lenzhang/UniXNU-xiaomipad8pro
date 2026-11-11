#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The sensors' batched tablet test (docs/drivers/sensors.md §8.1): one RAM image per step S1..S4, built in one go from one kernel
# collection, run in order in one attended session and stopped at the first failure. Every image has PianoPower level 2 (it starts the
# DSP; the sensors never do), the USB console and root shell, the buttons, the GPU profile, Data volatile (data-persist=0), and a
# timed reset back to ABL fastboot. The sensor pack reaches the images in one of two ways:
#   SENSOR_PACK=out/sensors/pack.bin (default)  carried by each image (build-ram-images.py's PIANO_SENSOR_PACK: DarwinBoot's addon
#                                               slot, read by PianoSensors before the ESP); the images' ADT gets the GpuAddon slot
#                                               (out/adt/piano-k27-addon.adt, made here); the images hold this unit's calibration:
#                                               they stay in the ignored out/, never shared
#   SENSOR_PACK=esp                             none carried: PianoSensors reads the installed ESP's (build-esp.py --sensor-pack,
#                                               written by the owner's maintenance session); the plain ADT (out/adt/piano-k27.adt)
#
#   build-sensor-test-images.sh [NAME ...]     default: all; out/ram-sensors-<NAME>/ and one dump image out/ram-sensors-dump/
#
#   s1     piano-sensors=1                                          the survey (10 min)
#   s2     piano-sensors=2                                          the file service, context bank 3 (15 min)
#   s2b4   piano-sensors=2 piano-sensors-bank=4                     only if s2's attach was refused (15 min)
#   s3     piano-sensors=3                                          the sensor list (10 min)
#   s4     piano-sensors=4 -als=2 -rotate=2                         both streams without their consumers: lux and edge in ioreg; the
#                                                                   display, the lid and the cover still switch them (20 min)
#   s4c    piano-sensors=4 + PianoBacklight's automatic mode, piano-rotate=1 (the consumers decide) (20 min)
#   s4z    s4c + three test sleeps (k27 0062): the streams off before each sleep, fresh after each wake (15 min)
#
# Inputs (this repository's out/ unless set): SENSOR_KC (out/k27/kc-k27-native.bin, built with the cover's kext and the GPU profile:
# `KB=$(tooling/k27/build-native-piano-keyboard.sh | tail -1); PIANO_EXTRA_KEXTS=$KB PIANO_GPU=2 PIANO_VMM_PRESENT=0
# K27_KERNEL=<kernel-k27-0063-native.macho> tooling/k27/build-native-image.sh`; s4z needs 0062 or later), SENSOR_FD, SENSOR_SEED,
# SENSOR_TRUST (the Turnip stage), SENSOR_PACK, SENSOR_EXTRA (more arguments for every image; s4z's line is about 960 of the 1000
# bytes DarwinBoot passes). The KC is checked for PianoSensors with level 4 and for PianoKeyboard.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${SENSOR_KC:-out/k27/kc-k27-native.bin}
FD=${SENSOR_FD:-out/fd/piano-uefi-A-bundled.fd}
SEED=${SENSOR_SEED:-out/turnip-a830/stage/scratch-data.img}
TRUST=${SENSOR_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
PACK=${SENSOR_PACK:-out/sensors/pack.bin}
for f in "$KC" "$FD" "$SEED" "$TRUST"; do [ -f "$f" ] || { echo "build-sensor-test-images: $f is missing" >&2; exit 1; }; done
grep -a -q "streams: joined the power tree" "$KC" || { echo "build-sensor-test-images: $KC has no PianoSensors level 4" >&2; exit 1; }
grep -a -q "com.unixnu.driver.PianoKeyboard" "$KC" \
  || echo "build-sensor-test-images: $KC has no PianoKeyboard: the cover and the lid read as unknown (detached, open)" >&2
if [ "$PACK" = esp ]; then
  ADT=out/adt/piano-k27.adt
  unset PIANO_SENSOR_PACK
  echo "build-sensor-test-images: no pack carried: PianoSensors reads the installed ESP's (sensors.md §5b)" >&2
else
  [ -f "$PACK" ] || { echo "build-sensor-test-images: $PACK is missing (tools/make-sensor-pack.py, sensors.md §5b)" >&2; exit 1; }
  ADT=out/adt/piano-k27-addon.adt
  python3 tooling/adt/make-piano-adt.py "$ADT" --unique-chip-id "${UNIQUE_CHIP_ID:-47554e5941480001}" --data --trust --addon \
    --vmm-present "${PIANO_VMM_PRESENT:-0}" > /dev/null
  export PIANO_SENSOR_PACK=$PACK
fi
[ -f "$ADT" ] || { echo "build-sensor-test-images: $ADT is missing" >&2; exit 1; }
BASE="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-power=2 piano-usb=2 piano-usb-shell=1"
BASE="$BASE piano-input=1 ${SENSOR_EXTRA:-}"
CONSUMERS="piano-backlight=2 piano-backlight-auto=1 piano-keyboard=2 piano-rotate=1"
ORDER="s1 s2 s2b4 s3 s4 s4c s4z"
image() {
  case $1 in
    s1)   A="piano-sensors=1"; R=600 ;;
    s2)   A="piano-sensors=2"; R=900 ;;
    s2b4) A="piano-sensors=2 piano-sensors-bank=4"; R=900 ;;
    s3)   A="piano-sensors=3"; R=600 ;;
    s4)   A="piano-sensors=4 piano-sensors-als=2 piano-sensors-rotate=2 piano-backlight=2 piano-keyboard=2"; R=1200 ;;
    s4c)  A="piano-sensors=4 $CONSUMERS"; R=1200 ;;
    s4z)  A="piano-sensors=4 $CONSUMERS piano-sleep=1 piano-sleep-test=120 piano-sleep-test-count=3"; R=900 ;;   # 30 s each
    *) return 1 ;;
  esac
}
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
for n in ${*:-$ORDER}; do
  image "$n" || { echo "build-sensor-test-images: no image $n (one of: $ORDER)" >&2; exit 2; }
  PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=$R PIANO_DATA_PERSIST=0 PIANO_EXTRA_ARGS="$BASE $A" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-sensors-$n" 2> "out/k27/build-ram-sensors-$n.log" | tail -1
done
PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-sensors-dump \
  2> out/k27/build-ram-sensors-dump.log | tail -1
shasum -a 256 out/ram-sensors-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
