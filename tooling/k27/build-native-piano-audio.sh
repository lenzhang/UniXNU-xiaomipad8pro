#!/bin/bash
# Build package/PianoAudio (the speakers and microphones; level 1 only: a read-only survey of the amplifiers' bus, reset and interrupt
# pins, the TDM pins and the SMMU entries of the DSP's PCM streams) as a k27 native kext: the host tests first (tests/audio_host.cpp:
# what the survey loads and what it never loads, the facts tables, the decoders and verdicts, the SMMU scan), then the objects in
# out/native-kexts/PianoAudio for tooling/k27/kext/PianoAudio.json. Inert without the boot argument piano-audio (docs/drivers/audio.md).
# No firmware, amplifier preset or topology is needed or read.
#   environment: K27_TREE (the k27 build tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
P=$D/package/PianoAudio; O=$D/out/native-kexts/PianoAudio
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/audio_host.cpp" -o "$O/host-tests/audio_host"
"$O/host-tests/audio_host" || { echo "Error: PianoAudio host tests failed" >&2; exit 1; }
python3 "$D/tooling/k27/build-native-kext.py" PianoAudio "$P/PianoAudio.cpp" "$D/tooling/k27/NativeKextGlue.cpp" -I "$P" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" | cut -c1-160 >&2
