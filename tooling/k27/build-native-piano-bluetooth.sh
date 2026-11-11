#!/bin/bash
# Build package/PianoBluetooth (the WCN7860's Bluetooth side over QUP2 SE6 "UART14"; level 1 only: a read-only survey of the SE6
# clocks and clock source, the UART pins, SE6's registers when clocked, the SMMU stream 0x423 and BT_EN over the SPMI observer) as a
# k27 native kext: the host tests first (tests/bt_host.cpp: the survey's gates and decoders, the clock and UART plans, H4 framing,
# the QCA vendor codec and the whole controller set-up against a simulated controller with faults), then a store check of the
# survey headers, then the objects in out/native-kexts/PianoBluetooth for tooling/k27/kext/PianoBluetooth.json. Inert without the
# boot argument piano-bt (docs/drivers/bluetooth.md). No firmware is in the build: the later levels read it at run time from the
# tablet's own bluetooth_a partition. PianoPower's SPMI observer header is included, not copied.
#   environment: K27_TREE (the k27 build tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
P=$D/package/PianoBluetooth; O=$D/out/native-kexts/PianoBluetooth
INC=(-I "$P" -I "$D/package/PianoPower")
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "${INC[@]}" "$P/tests/bt_host.cpp" -o "$O/host-tests/bt_host"
"$O/host-tests/bt_host" || { echo "Error: PianoBluetooth host tests failed" >&2; exit 1; }
# The level 1 survey header stays store-free: no str instruction and no assignment through a volatile pointer (the kext's only store
# is the SPMI observer's read command, in PianoBluetooth.cpp).
if grep -nE '"str|volatile[^;]*\*[^;]*[^=!<>]=[^=]' "$P/GeniUart.h" "$P/BtHw.h"; then
  echo "Error: the survey headers have a store" >&2; exit 1
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoBluetooth "$P/PianoBluetooth.cpp" "$D/tooling/k27/NativeKextGlue.cpp" "${INC[@]}" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" | cut -c1-160 >&2
