#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The audio test images of docs/drivers/audio.md section 8.8 (PianoAudio levels 5-10), each a RAM image whose addon carries the
# amplifier preset (PIANO_FW_ADDON=1: the stock fs19xx.fsm build-native-piano-audio.sh staged in out/firmware/PianoAudio/ for the
# kext's pin; nothing is written to the ESP), with the USB console and piano-reset-after as the backstop. No other DSP client and no
# other SMMU user that plans stream-match groups (no piano-sensors>=2, no piano-wifi>=3: audio.md 10.13).
#
#   build-audio-test-images.sh [NAME ...]    default: amp graphs (the unattended pair); out/ram-au-<NAME>/ and out/ram-au-dump/
#     amp      level 5 alone, without the DSP: the bit-banged bus, the four FS19xx, the preset from the addon, configured powered
#              down, held in reset (unattended; nothing is powered up)                                             300 s
#     graphs   levels 4-6 on the DSP (piano-power=2, its writes off): the LPASS clocks, the amplifiers configured, the speaker graphs
#              with 2 s of silence (the amplifiers stay down), the microphone graphs opened and closed (unattended)  600 s
#     tone     ATTENDED (someone listens within a metre): levels 4-7 with the tone walk (1 kHz, -20 dBFS, 600 ms on TL, TR, BL, BR
#              in turn, gain 160), then level 8 for the desktop: the macOS Speaker on the DSP at the test level (gain 160, full scale
#              at -20 dBFS); log in, `say hello`, `afplay` a file                                                 1800 s
#     desktop  ATTENDED first: levels 4-6, then level 9 (the daily level: AU_GAIN, default 176, and AU_CEILING dB below full scale,
#              default 6), the Speaker on the DSP for the session                                                  3600 s
#     mic      ATTENDED: as desktop with level 10 (the Mic from DMIC0/DMIC2: Voice Memos, or `rec`-like tools)       3600 s
#
# Inputs (this repository's out/ unless set): AU_KC (out/k27/kc-k27-native.bin: PIANO_A830_METAL=0 tooling/k27/build-native-image.sh),
# AU_ADT (out/adt/piano-k27.adt), AU_FD, AU_SEED / AU_TRUST (the Turnip stage when it is there, for the desktop images' GPU login;
# else out/inputs'), AU_EXTRA (added to every image), AU_PORTS (piano-audio-ports=N when level 2 chose another set), AU_SWAP
# (piano-audio-i2c-swap=1 when level 5 found SDA and SCL the other way round). The KC must carry PianoAudio's preset pin and
# out/firmware/PianoAudio/fs19xx.fsm must be staged (build-native-piano-audio.sh stages it from the owner's copy on this Mac).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${AU_KC:-out/k27/kc-k27-native.bin}
ADT=${AU_ADT:-out/adt/piano-k27.adt}
FD=${AU_FD:-out/fd/piano-uefi-A-bundled.fd}
GPU_SEED=${AU_SEED:+given}   # the desktop images' piano-gpu=2 needs a seed with the GPU's Metal plugin (gpu.md 6): the Turnip stage
if [ -f out/turnip-a830/stage/scratch-data.img ]; then
  SEED=${AU_SEED:-out/turnip-a830/stage/scratch-data.img}; TRUST=${AU_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
  GPU_SEED=yes
else
  SEED=${AU_SEED:-out/inputs/scratch-data.img}; TRUST=${AU_TRUST:-out/inputs/kernel.macho.trustcache}
fi
for f in "$KC" "$ADT" "$FD" "$SEED" "$TRUST"; do [ -f "$f" ] || { echo "build-audio-test-images: $f is missing" >&2; exit 1; }; done
grep -a -q com.unixnu.driver.PianoAudio "$KC" || { echo "build-audio-test-images: $KC has no PianoAudio" >&2; exit 1; }
PINS=$(python3 tooling/persist/firmware.py pins "$KC")
case $PINS in *PianoAudio/fs19xx.fsm*) ;; *)
  echo "build-audio-test-images: $KC has no pin for PianoAudio/fs19xx.fsm (a PianoAudio from before the preset's pin)" >&2; exit 1 ;; esac
STAGED=$(python3 tooling/persist/firmware.py check "$KC" --dir "${PIANO_FW_DIR:-out/firmware}" || true)   # lists the pins it lacks
case $STAGED in *PianoAudio/fs19xx.fsm*)
  echo "build-audio-test-images: the preset is not staged (out/firmware/PianoAudio/fs19xx.fsm: PIANO_AUDIO_PRESET=<the owner's stock" \
       "fs19xx.fsm> tooling/k27/build-native-piano-audio.sh)" >&2; exit 1 ;; esac
# The GPU login for the attended images (the policy images' profile), the console, PianoPower with its writes off for the DSP runs.
BASE="piano-usb=2 panic_on_crash_disabled=1 ${AU_PORTS:+piano-audio-ports=$AU_PORTS} ${AU_SWAP:+piano-audio-i2c-swap=$AU_SWAP} ${AU_EXTRA:-}"
DESK="piano-gpu=2 piano-gpu-delay=20 piano-input=1 piano-keyboard=2 piano-backlight=2 piano-touch=2"
DSP="piano-power=2 piano-power-write=0"
ORDER="amp graphs"
image() {
  case $1 in
    amp)     A="piano-audio=5 piano-audio-dsp=0";                                                        R=300 ;;
    graphs)  A="$DSP piano-audio=6";                                                                     R=600 ;;
    tone)    A="$DSP $DESK piano-audio=8 piano-audio-tone=1";                                            R=1800 ;;
    desktop) A="$DSP $DESK piano-audio=9 piano-audio-gain=${AU_GAIN:-176} piano-audio-ceiling=${AU_CEILING:-6}"; R=3600 ;;
    mic)     A="$DSP $DESK piano-audio=10 piano-audio-gain=${AU_GAIN:-176} piano-audio-ceiling=${AU_CEILING:-6}"; R=3600 ;;
    *) return 1 ;;
  esac
}
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
for n in ${*:-$ORDER}; do
  image "$n" || { echo "build-audio-test-images: no image $n (one of: amp graphs tone desktop mic)" >&2; exit 2; }
  case $A in *piano-gpu=2*) [ -n "$GPU_SEED" ] || {
    echo "build-audio-test-images: $n is a desktop image (piano-gpu=2) and there is no Turnip stage (out/turnip-a830/stage: a build" \
         "without PIANO_A830_METAL=0) nor AU_SEED/AU_TRUST: a seed without the GPU's Metal plugin sends WindowServer into a crash loop" >&2
    exit 1; } ;; esac
  PIANO_FW_ADDON=1 PIANO_FAST_BOOT=1 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=$R PIANO_EXTRA_ARGS="$A $BASE" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-au-$n" | tail -1
  python3 tooling/bootimg/verify-ram-images.py "out/ram-au-$n/build-A-bundled.json" | tail -1
done
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-au-dump \
  | tail -1
shasum -a 256 out/ram-au-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
