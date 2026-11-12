#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The test images of docs/drivers/camera.md section 8.1 (PianoCamera, B11): c1 the read-only survey, c1x the survey without the
# clock controllers (only if c1 hangs), c1b c1's re-test (level 2 stopped after its checks: GCC's camera branches enabled and
# CAMCC's domains voted, so the step-202 survey reads both clock controllers, then all as found; c1 found them unreadable),
# c2f/c2r/c2rg/c2r24 level 2 (one sensor powered, its chip ID read, powered down again) for the front, the rear over CCI0, the rear
# over software I2C, the rear with the 24 MHz MCLK (c2fx, the front on the minimal path, answered on 2026-11-13: MCLK4 needs
# TITAN_TOP and CAMCC's domains; level 2 takes them for either sensor). All unattended (camera.md 8.1 has why), c2* only after c1
# passed. Level 3 (camera.md 8.3): c3rp the rear prepared to the sensor's tables and stopped before the stream-on table (the
# power-down's test: the SMMU window released first, the found words this SMMU refuses kept), c3r the rear's colour bars (two
# frames), c3rs the rear's scene (four frames), c3f the front (two frames); each only after the previous one passed (c3rp after
# c2r24; c3f after c2f). Images built before 2026-11-12 evening abort in level 3's power-down (c3rp/c3r): rebuild them. Each is a
# RAM image of the GPU profile (PianoA830, the Turnip stage, ADT vmm-present 0), or CAM_PROFILE=plain, with the USB console and
# piano-reset-after as the backstop; PIANO_FAST_BOOT=1 keeps the command line short.
#
#   build-camera-test-images.sh [NAME ...]    default: c1 c1x c1b c2f c2r c2rg c2r24 c3rp c3r c3rs c3f cplan cae
#                                             out/ram-cam-<NAME>/ and out/ram-cam-dump/
#
# Inputs (this repository's out/ unless set): CAM_KC (out/k27/kc-k27-native.bin, built by
#   PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh), CAM_ADT (out/adt/piano-k27.adt from the same build), CAM_FD,
#   CAM_SEED, CAM_TRUST (the Turnip stage), CAM_EXTRA (added to every image). CAM_PROFILE=plain: no GPU arguments and the plain seed
#   and trust cache (out/inputs), for a KC built with PIANO_A830_METAL=0. The KC is checked for PianoCamera (and PianoA830).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${CAM_KC:-out/k27/kc-k27-native.bin}
ADT=${CAM_ADT:-out/adt/piano-k27.adt}
FD=${CAM_FD:-out/fd/piano-uefi-A-bundled.fd}
if [ "${CAM_PROFILE:-gpu}" = plain ]; then
  SEED=${CAM_SEED:-out/inputs/scratch-data.img}
  TRUST=${CAM_TRUST:-out/inputs/kernel.macho.trustcache}
  PROFILE="panic_on_crash_disabled=1 piano-usb=2"
else
  SEED=${CAM_SEED:-out/turnip-a830/stage/scratch-data.img}
  TRUST=${CAM_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
  PROFILE="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2"
fi
for f in "$KC" "$ADT" "$FD" "$SEED" "$TRUST"; do [ -f "$f" ] || { echo "build-camera-test-images: $f is missing" >&2; exit 1; }; done
for s in com.unixnu.driver.PianoCamera com.unixnu.driver.PianoA830; do
  grep -a -q "$s" "$KC" || { echo "build-camera-test-images: $KC has no $s (build it with tooling/k27/build-native-image.sh)" >&2; exit 1; }
done
ORDER="c1 c1x c1b c2f c2r c2rg c2r24 c3rp c3r c3rs c3f cplan cae"
image() {
  case $1 in
    c1)    A="piano-camera=1 piano-camera-delay=20" ;;                                                       # the survey
    c1x)   A="piano-camera=1 piano-camera-cc=0 piano-camera-mclkcc=0 piano-camera-delay=20" ;;              # without CAMCC/MCLK-CC
    c1b)   A="piano-camera=2 piano-camera-sensor=1 piano-camera-stop=203 piano-camera-delay=20" ;;           # c1's re-test: the controllers
    c2f)   A="piano-camera=2 piano-camera-sensor=2 piano-camera-delay=20" ;;                                 # front, software I2C, TITAN_TOP
    c2r)   A="piano-camera=2 piano-camera-sensor=1 piano-camera-delay=20" ;;                                 # rear, CCI0
    c2rg)  A="piano-camera=2 piano-camera-sensor=1 piano-camera-rear-bus=1 piano-camera-delay=20" ;;         # rear, software I2C
    c2r24) A="piano-camera=2 piano-camera-sensor=1 piano-camera-mclk24=1 piano-camera-delay=20" ;;           # rear, 24 MHz MCLK
    c3rp)  A="piano-camera=3 piano-camera-sensor=1 piano-camera-stop=312 piano-camera-delay=20" ;;           # rear, all but the stream-on table
    c3r)   A="piano-camera=3 piano-camera-sensor=1 piano-camera-delay=20" ;;                                 # rear, colour bars
    c3rs)  A="piano-camera=3 piano-camera-sensor=1 piano-camera-pattern=0 piano-camera-frames=4 piano-camera-delay=20" ;;   # rear scene
    c3f)   A="piano-camera=3 piano-camera-sensor=2 piano-camera-delay=20" ;;                                 # front, its scene
    # Several on one boot (CameraPlan.h): each with its image's arguments, the next only after a complete power-down, the riskier
    # ones last. cplan: the level 3 ladder with the front's level 2; cae: the exposure loop on both sensors' scenes after the rear's
    # colour bars (the stream's regression check).
    cplan) A="piano-camera=3 piano-camera-plan=c3rp,c2f,c3r,c3rs,c3f piano-camera-delay=20" ;;
    cae)   A="piano-camera=3 piano-camera-plan=c3r,c3rs,c3f piano-camera-delay=20" ;;
    *) return 1 ;;
  esac
}
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
for n in ${*:-$ORDER}; do
  image "$n" || { echo "build-camera-test-images: no image $n (one of: $ORDER)" >&2; exit 2; }
  PIANO_FAST_BOOT=1 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="$PROFILE $A ${CAM_EXTRA:-}" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-cam-$n" | tail -1
done
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-cam-dump \
  | tail -1
shasum -a 256 out/ram-cam-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
