#!/bin/bash
# Build package/PianoAudio (the speakers and microphones, docs/drivers/audio.md) as a k27 native kext: the host tests first, then the
# objects in out/native-kexts/PianoAudio for tooling/k27/kext/PianoAudio.json. Inert without the boot argument piano-audio.
#   host tests: tests/audio_host.cpp (level 1: what the survey loads and never loads, the facts tables, decoders, verdicts, the SMMU
#   scan; levels 4-6: the pin values, the amplifier table, the bypass plan of the DSP's PCM streams), tests/gpr_host.c (the GPR codec,
#   sheng's), tests/audioreach_host.c (piano's APM/PRM packets), tests/sequence_host.c (one command at a time, the reverse unwind, the
#   speaker, PRM and microphone plans, the period pump), tests/fs19xx_host.c (the amplifiers against a register model: probe, a
#   synthetic preset container, the safety filter, the power sequences, status; the preset's SHA-256 identity), tests/amps_host.c
#   (the four as a group: reset sequence, all or none, test gain, mute-first shutdown), tests/pcm_host.c (the speaker-safety chain:
#   slots, DC blocker, ceiling, fades, test tone, the walk, guard; the PCM ring), tests/dsplink_host.c (level 2: the DSP guard on
#   PianoPower's health, the inbox, the APM readiness exchange, the GPR port sets), tests/session_host.c (levels 4-7 against a DSP
#   model and the amplifiers' model in simulated time: the order of every command, speaker safety at every write and buffer, every
#   failure path), tests/preset_host.c (the preset loader on an mtools FAT32 image as build-esp.py makes the ESP; skipped without
#   mtools)
# The copies stay byte for byte their originals but for their provenance lines (FatVolume from PianoPower, SmmuStream from PianoDwc3,
# BitI2c from PianoKeyboard). Then the import check (PianoPower's client functions, package/include/PianoAdspContract.h, and NativeUfs's
# partition reader are the library imports) and tooling/k27/check-audio2-family.py: IOAudio2Family's classes in the KC base, which
# the macOS devices are linked under at run time. No firmware, amplifier preset or topology is needed or read: level 5 reads the
# owner's preset from the ESP at run time (tooling/persist/build-esp.py --audio-preset).
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes, its KC base), K27_OBJ (default k27-native-obj), CC/CXX
#                (host compilers for the tests, default cc/c++), K27_KERNEL (for the import check; skipped when absent), KC_BASE
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoAudio; O=$D/out/native-kexts/PianoAudio; T=$O/host-tests
mkdir -p "$T"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/audio_host.cpp" -o "$T/audio_host"
"$T/audio_host" || { echo "Error: PianoAudio host tests failed" >&2; exit 1; }
C=("${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$P")
"${C[@]}" "$P/tests/gpr_host.c" "$P/PianoGpr.c" -o "$T/gpr_host"
"${C[@]}" "$P/tests/audioreach_host.c" "$P/PianoAudioReach.c" "$P/PianoGpr.c" -o "$T/audioreach_host"
"${C[@]}" "$P/tests/sequence_host.c" "$P/AudioSequence.c" "$P/PianoAudioReach.c" "$P/PianoGpr.c" -o "$T/sequence_host"
"${C[@]}" "$P/tests/fs19xx_host.c" "$P/Fs19xx.c" "$P/AudioSha256.c" -o "$T/fs19xx_host"
"${C[@]}" "$P/tests/amps_host.c" "$P/AudioAmps.c" "$P/Fs19xx.c" "$P/AudioSha256.c" -o "$T/amps_host"
"${C[@]}" "$P/tests/pcm_host.c" "$P/AudioSafety.c" -lm -o "$T/pcm_host"
"${C[@]}" "$P/tests/dsplink_host.c" "$P/AudioDspLink.c" "$P/PianoGpr.c" -o "$T/dsplink_host"
"${C[@]}" "$P/tests/session_host.c" "$P/AudioSession.c" "$P/AudioAmps.c" "$P/Fs19xx.c" "$P/AudioSha256.c" "$P/AudioSequence.c" \
  "$P/PianoAudioReach.c" "$P/PianoGpr.c" "$P/AudioSafety.c" "$P/AudioDspLink.c" -o "$T/session_host"
"${C[@]}" "$P/tests/preset_host.c" "$P/AudioPreset.c" "$P/AudioAmps.c" "$P/Fs19xx.c" "$P/AudioSha256.c" "$P/FatVolume.c" -o "$T/preset_host"
for t in gpr_host audioreach_host sequence_host fs19xx_host amps_host pcm_host dsplink_host session_host; do
  "$T/$t" || { echo "Error: PianoAudio $t failed" >&2; exit 1; }
done
# The preset loader on a FAT32 image with 4096-byte sectors, one per cluster, as build-esp.py makes the ESP (mtools).
if command -v mformat >/dev/null && command -v mmd >/dev/null && command -v mcopy >/dev/null; then
  rm -rf "$T/esp" "$T/esp.img"; mkdir -p "$T/esp"
  "$T/preset_host" --write "$T/esp"
  mformat -C -F -S 5 -M 4096 -T 70000 -h 1 -s 1 -c 1 -i "$T/esp.img" ::
  mmd -i "$T/esp.img" ::/EFI ::/EFI/UniXNU
  for f in "$T/esp"/*; do mcopy -i "$T/esp.img" "$f" "::/EFI/UniXNU/$(basename "$f")"; done
  "$T/preset_host" "$T/esp.img" || { echo "Error: PianoAudio preset_host failed" >&2; exit 1; }
  rm -f "$T/esp.img"
else
  "$T/preset_host" || { echo "Error: PianoAudio preset_host failed" >&2; exit 1; }
  echo "SKIP the preset loader on a FAT image: no mtools (brew install mtools)" >&2
fi
# The copies stay byte for byte their originals (but for their provenance lines), which their owners never have to change.
for f in FatVolume.c FatVolume.h; do
  diff <(tail -n +4 "$P/$f") <(tail -n +2 "$D/package/PianoPower/$f") > /dev/null \
    || { echo "Error: package/PianoAudio/$f has drifted from package/PianoPower/$f" >&2; exit 1; }
done
diff <(tail -n +4 "$P/SmmuStream.h" | sed 's/namespace pianoaudio/namespace pianousb/') <(tail -n +2 "$D/package/PianoDwc3/SmmuStream.h") \
  > /dev/null || { echo "Error: package/PianoAudio/SmmuStream.h has drifted from package/PianoDwc3/SmmuStream.h" >&2; exit 1; }
diff <(tail -n +4 "$P/BitI2c.h" | sed 's/namespace pianoaudio/namespace pianokeyboard/; s/PIANO_AUDIO_BIT_I2C_H/PIANO_KEYBOARD_BIT_I2C_H/') \
  <(tail -n +2 "$D/package/PianoKeyboard/BitI2c.h") > /dev/null \
  || { echo "Error: package/PianoAudio/BitI2c.h has drifted from package/PianoKeyboard/BitI2c.h" >&2; exit 1; }
python3 "$D/tooling/k27/build-native-kext.py" PianoAudio "$P/PianoAudio.cpp" "$P/AudioDsp.cpp" "$P/AudioBoard.cpp" "$P/PianoAudioDevice.cpp" \
  "$D/tooling/k27/NativeKextGlue.cpp" "$P/PianoGpr.c" "$P/PianoAudioReach.c" "$P/AudioSequence.c" "$P/Fs19xx.c" "$P/AudioSha256.c" \
  "$P/AudioSafety.c" "$P/AudioDspLink.c" "$P/AudioPcm.c" "$P/AudioAmps.c" "$P/AudioSession.c" "$P/AudioPreset.c" "$P/FatVolume.c" -I "$P" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoPower's client functions and NativeUfs's partition reader come from those kexts (listed before PianoAudio, named as libraries
# by the descriptor): the KC link resolves them by name.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" \
  --library-prefix _piano_adsp_ --library-prefix _piano_qrtr_ --library-prefix _native_ufs_part_ | cut -c1-200 >&2
KC_BASE=${KC_BASE:-$K/out/kc/kc27.bin}
LOG=()
[ ! -f "$D/out/k27/kc-k27-native.log" ] || LOG=(--kc-log "$D/out/k27/kc-k27-native.log")
[ ! -f "$KC_BASE" ] || python3 "$D/tooling/k27/check-audio2-family.py" "$KC_BASE" --tree "$K" ${LOG[@]+"${LOG[@]}"} >&2
