#!/bin/bash
# Build package/PianoTorch (piano's rear torch: the PM8550 flash LED module; level 1 a read-only survey, level 2 torch control with a
# read-back after every write, a time limit and a thermal guard) as a k27 native kext: the host tests first (tests/torch_host.cpp:
# the SPMI arbiter model, what is read and written, the allowlist, the sequences, the guard), a store check of the portable
# headers, then the objects in out/native-kexts/PianoTorch for tooling/k27/kext/PianoTorch.json, and on a Mac the user-space tool
# (tooling/native/piano-torch) compiled for the host as a build check. Inert without the boot argument piano-torch
# (docs/drivers/torch.md). PianoPower's SPMI observer header is included, not copied.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), CC (host compiler for the tool, default cc), K27_KERNEL (for the import check; skipped
#                when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoTorch; O=$D/out/native-kexts/PianoTorch
INC=(-I "$P" -I "$D/package/PianoPower")
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "${INC[@]}" "$P/tests/torch_host.cpp" -o "$O/host-tests/torch_host"
"$O/host-tests/torch_host" || { echo "Error: PianoTorch host tests failed" >&2; exit 1; }
# The portable headers hold no store of their own: no str instruction and no assignment through a volatile pointer (the kext's
# stores are the observer's read command and the flash APID's write channel, both through PianoTorch.cpp's wr32).
if grep -nE '"str|volatile[^;]*\*[^;]*[^=!<>]=[^=]' "$P/FlashHw.h" "$P/TorchCore.h" "$P/TorchSpmi.h"; then
  echo "Error: a portable header has a store" >&2; exit 1
fi
if [ "$(uname)" = Darwin ]; then
  "${CC:-cc}" -O2 -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation "$D/tooling/native/piano-torch/piano-torch.c" \
    -o "$O/host-tests/piano-torch" || { echo "Error: tooling/native/piano-torch does not build" >&2; exit 1; }
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoTorch "$P/PianoTorch.cpp" "$D/tooling/k27/NativeKextGlue.cpp" "${INC[@]}" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" | cut -c1-160 >&2
