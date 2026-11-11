#!/bin/bash
# Build package/PianoDp (USB-C DisplayPort Alt Mode, backlog B8; level 1, read only: D1 the survey of the DP controller, the QMP PHY's
# DP half and the DPU's DP side, each block behind its clock gate; D2 the DSP's USB-C port notifications through PianoPower's client
# contract, decoded and logged per port; D6's shared configuration and plug schedule, which NativeGopDisplay's second display uses) as a
# k27 native kext: the host tests first (tests/dp_host.cpp: the gates on a register model, the decoders, the DPU path verdict, the
# notification decoder, the pin-assignment table and link budget, the HPD state machine, D6's arguments and schedule), a store check of
# the portable headers, then the objects in out/native-kexts/PianoDp for tooling/k27/kext/PianoDp.json and the import check (PianoPower's
# client functions come from PianoPower, listed before PianoDp). Inert without the boot argument piano-dp (docs/drivers/displayport.md).
# The DPU's and the USB side's facts and survey pieces are included, not copied (package/PianoDpu, package/PianoUsbHost, package/PianoDwc3).
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoDp; O=$D/out/native-kexts/PianoDp
INC=(-I "$P" -I "$D/package/PianoDpu" -I "$D/package/PianoUsbHost" -I "$D/package/PianoDwc3")
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "${INC[@]}" "$P/tests/dp_host.cpp" -o "$O/host-tests/dp_host"
"$O/host-tests/dp_host" || { echo "Error: PianoDp host tests failed" >&2; exit 1; }
# The portable headers hold no store: no str instruction and no assignment through a volatile pointer (the kext's only MMIO access is
# PianoDp.cpp's ldr; it has no store helper at all).
if grep -nE '"str|volatile[^;]*\*[^;]*[^=!<>]=[^=]' "$P/DpHw.h" "$P/DpSurvey.h" "$P/DpPort.h" "$P/DpFake.h"; then
  echo "Error: a portable header has a store" >&2; exit 1
fi
if grep -nE '"str|wr32|volatile[^;]*\*[^;]*[^=!<>]=[^=]' "$P/PianoDp.cpp"; then
  echo "Error: PianoDp.cpp has a store helper" >&2; exit 1
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoDp "$P/PianoDp.cpp" "$D/tooling/k27/NativeKextGlue.cpp" "${INC[@]}" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" \
  --library-prefix _piano_port_ --library-prefix _piano_adsp_ | cut -c1-200 >&2
