#!/bin/bash
# Build package/PianoPolicy (piano's laptop-class policy: the cover, the keyboard's display wake, the power key, the power profile
# from macOS Low Power Mode; docs/policy.md) as a k27 native kext: the host tests first (tests/policy_host.cpp: every cover x power
# source x display x Low Power Mode x sleep combination, the notification sequences, the GPU ceiling slot shared with PianoThermal),
# then the objects in out/native-kexts/PianoPolicy for tooling/k27/kext/PianoPolicy.json. Inert without the boot argument
# piano-policy. No hardware is touched.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoPolicy; O=$D/out/native-kexts/PianoPolicy
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/policy_host.cpp" -o "$O/host-tests/policy_host"
"$O/host-tests/policy_host" || { echo "Error: PianoPolicy host tests failed" >&2; exit 1; }
python3 "$D/tooling/k27/build-native-kext.py" PianoPolicy "$P/PianoPolicy.cpp" "$D/tooling/k27/NativeKextGlue.cpp" -I "$P" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoThermal's breadcrumb (piano_thermal_state) comes from PianoThermal, a library of the descriptor, resolved by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _piano_thermal_state \
  | cut -c1-160 >&2
