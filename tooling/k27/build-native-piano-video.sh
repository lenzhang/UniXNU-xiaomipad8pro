#!/bin/bash
# Build package/PianoVideo (the video core, SM8750 Iris; level 1 only: a read-only survey of its power and clock state, its
# controller's registers when it is powered, and the apps SMMU's entries for its streams) as a k27 native kext: the host tests first
# (tests/video_host.cpp: the gate and what is and is never loaded, the clock and status decoders, the SMMU scan), then the objects
# in out/native-kexts/PianoVideo for tooling/k27/kext/PianoVideo.json. Inert without the boot argument piano-video
# (docs/drivers/video.md).
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoVideo; O=$D/out/native-kexts/PianoVideo
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/video_host.cpp" -o "$O/host-tests/video_host"
"$O/host-tests/video_host" || { echo "Error: PianoVideo host tests failed" >&2; exit 1; }
python3 "$D/tooling/k27/build-native-kext.py" PianoVideo "$P/PianoVideo.cpp" "$D/tooling/k27/NativeKextGlue.cpp" -I "$P" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" | cut -c1-160 >&2
