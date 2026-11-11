#!/bin/bash
# Build package/PianoBacklight (the panel backlight: two KTZ8866 over bit-banged I2C on their QUP buses' TLMM pins) as a k27 native
# kext: the host tests first (tests/backlight_host.cpp: the I2C engine on a bit-level KTZ8866 model, the steps and levels, the
# controller's register sequences, display sleep/wake, the lid, failures, the key watch), then the objects in
# out/native-kexts/PianoBacklight for tooling/k27/kext/PianoBacklight.json. Inert without the boot argument piano-backlight
# (docs/drivers/backlight.md). No firmware, clock or vote is needed.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoBacklight; O=$D/out/native-kexts/PianoBacklight
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/backlight_host.cpp" -o "$O/host-tests/backlight_host"
"$O/host-tests/backlight_host" || { echo "Error: PianoBacklight host tests failed" >&2; exit 1; }
python3 "$D/tooling/k27/build-native-kext.py" PianoBacklight "$P/PianoBacklight.cpp" "$D/tooling/k27/NativeKextGlue.cpp" -I "$P" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" | cut -c1-160 >&2
