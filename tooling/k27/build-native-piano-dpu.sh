#!/bin/bash
# Build package/PianoDpu (the display processor; level 1: a read-only survey of what the boot stages left driving the panel: pipes,
# CTL paths, layer mixers, DSPP colour blocks, DSC, INTF and DSI timing, the DPU's SMMU route, the refresh measured from the INTF frame
# counters; B1b's zero-copy flips in DpuFlip.cpp at piano-dpu=2; and the opt-in writing slices B1c refresh, B1d Night Light, B1e panel
# sleep in DpuSlices.cpp) as a k27 native kext: the host tests first (tests/dpu_host.cpp: the clock gate and what is and is never
# loaded, the decoders, the path and flip verdicts on a modelled boot-loader state, the SMMU scan and the page-table walk;
# tests/dpu_flip_host.cpp: B1b; tests/dpu_refresh_host.cpp, dpu_pcc_host.cpp, dpu_panel_host.cpp: the slices' sequences on a timed
# register model), then the objects in out/native-kexts/PianoDpu for tooling/k27/kext/PianoDpu.json.
# Inert without the boot argument piano-dpu (docs/drivers/dpu.md).
#   environment: K27_TREE (the k27 build tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
P=$D/package/PianoDpu; O=$D/out/native-kexts/PianoDpu
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/dpu_host.cpp" -o "$O/host-tests/dpu_host"
"$O/host-tests/dpu_host" || { echo "Error: PianoDpu host tests failed" >&2; exit 1; }
# B1b (zero-copy flips): the SMMU bank in place, the windows, the flip engine and its safety, against DPU/SMMU/RAM models.
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/dpu_flip_host.cpp" -o "$O/host-tests/dpu_flip_host"
"$O/host-tests/dpu_flip_host" || { echo "Error: PianoDpu B1b host tests failed" >&2; exit 1; }
# The writing slices B1c/B1d/B1e (DpuRefresh.h, DpuPcc.h, DpuPanel.h on the timed register model tests/dpu_model.h).
for t in refresh pcc panel; do
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/dpu_${t}_host.cpp" -o "$O/host-tests/dpu_${t}_host"
  "$O/host-tests/dpu_${t}_host" || { echo "Error: PianoDpu $t host tests failed" >&2; exit 1; }
done
python3 "$D/tooling/k27/build-native-kext.py" PianoDpu "$P/PianoDpu.cpp" "$P/DpuFlip.cpp" "$P/DpuSlices.cpp" "$D/tooling/k27/NativeKextGlue.cpp" -I "$P" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" | cut -c1-160 >&2
