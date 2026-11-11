#!/bin/bash
# Build package/PianoUsbHost (USB host mode and the Type-C port; level 1 only: a read-only survey of the Type-C state, the orientation
# line, the USB power domains and clocks, the DWC3 role, the xHCI capability words when the core is clocked, the QMP PHY's Type-C mux
# when the PHY is powered) as a k27 native kext: the host tests first (tests/usbhost_host.cpp: what is loaded and when, the decoders,
# the role policy), then the objects in out/native-kexts/PianoUsbHost for tooling/k27/kext/PianoUsbHost.json. Inert without the boot
# argument piano-usbhost (docs/drivers/usb-host.md). PianoDwc3 (device mode) is not touched; its PianoUsbHw.h and PianoPower's SPMI
# observer headers are included, not copied.
#   environment: K27_TREE (the k27 build tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
P=$D/package/PianoUsbHost; O=$D/out/native-kexts/PianoUsbHost
INC=(-I "$P" -I "$D/package/PianoDwc3" -I "$D/package/PianoPower")
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "${INC[@]}" "$P/tests/usbhost_host.cpp" -o "$O/host-tests/usbhost_host"
"$O/host-tests/usbhost_host" || { echo "Error: PianoUsbHost host tests failed" >&2; exit 1; }
# The survey header stays store-free: no str instruction and no assignment through a volatile pointer (the kext's only store is the
# SPMI observer's read command, in PianoUsbHost.cpp).
if grep -nE '"str|volatile[^;]*\*[^;]*[^=!<>]=[^=]' "$P/UsbHostSurvey.h" "$P/UsbHostHw.h"; then
  echo "Error: the survey headers have a store" >&2; exit 1
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoUsbHost "$P/PianoUsbHost.cpp" "$D/tooling/k27/NativeKextGlue.cpp" "${INC[@]}" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" | cut -c1-160 >&2
