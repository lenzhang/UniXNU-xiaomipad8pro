#!/bin/bash
# Build package/PianoAudio (the speakers and microphones, docs/drivers/audio.md) as a k27 native kext: the host tests first, then the
# objects in out/native-kexts/PianoAudio for tooling/k27/kext/PianoAudio.json. Inert without the boot argument piano-audio.
#   host tests: tests/audio_host.cpp (level 1: what the survey loads and never loads, the facts tables, decoders, verdicts, the SMMU
#   scan), tests/gpr_host.c (the GPR codec, sheng's), tests/audioreach_host.c (piano's APM/PRM packets), tests/sequence_host.c (one
#   command at a time, the reverse unwind, the speaker plan, the period pump), tests/fs19xx_host.c (the amplifiers against a register
#   model: probe, a synthetic preset container, the safety filter, the power sequences, status; the preset's SHA-256 identity),
#   tests/pcm_host.c (the speaker-safety chain: slots, DC blocker, ceiling, fades, test tone, guard; the PCM ring), tests/dsplink_host.c
#   (level 2: the DSP guard on PianoPower's health, the inbox, the APM readiness exchange)
# Then the import check (PianoPower's client functions, package/include/PianoAdspContract.h, are the library imports) and
# tooling/k27/check-audio2-family.py: IOAudio2Family's classes in the KC base, which the macOS devices are linked under at run time.
# No firmware, amplifier preset or topology is needed or read.
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
"${C[@]}" "$P/tests/pcm_host.c" "$P/AudioSafety.c" -lm -o "$T/pcm_host"
"${C[@]}" "$P/tests/dsplink_host.c" "$P/AudioDspLink.c" "$P/PianoGpr.c" -o "$T/dsplink_host"
for t in gpr_host audioreach_host sequence_host fs19xx_host pcm_host dsplink_host; do
  "$T/$t" || { echo "Error: PianoAudio $t failed" >&2; exit 1; }
done
python3 "$D/tooling/k27/build-native-kext.py" PianoAudio "$P/PianoAudio.cpp" "$P/AudioDsp.cpp" "$P/PianoAudioDevice.cpp" \
  "$D/tooling/k27/NativeKextGlue.cpp" "$P/PianoGpr.c" "$P/PianoAudioReach.c" "$P/AudioSequence.c" "$P/Fs19xx.c" "$P/AudioSha256.c" \
  "$P/AudioSafety.c" "$P/AudioDspLink.c" "$P/AudioPcm.c" -I "$P" --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoPower's client functions come from PianoPower (listed before PianoAudio, named as a library by the descriptor): the KC link
# resolves them by name.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" \
  --library-prefix _piano_adsp_ --library-prefix _piano_qrtr_ | cut -c1-200 >&2
KC_BASE=${KC_BASE:-$K/out/kc/kc27.bin}
LOG=()
[ ! -f "$D/out/k27/kc-k27-native.log" ] || LOG=(--kc-log "$D/out/k27/kc-k27-native.log")
[ ! -f "$KC_BASE" ] || python3 "$D/tooling/k27/check-audio2-family.py" "$KC_BASE" --tree "$K" ${LOG[@]+"${LOG[@]}"} >&2
