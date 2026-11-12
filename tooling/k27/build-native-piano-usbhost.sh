#!/bin/bash
# Build package/PianoUsbHost (USB host mode and the Type-C port, docs/drivers/usb-host.md) as a k27 native kext: the host tests first
# (tests/usbhost_host.cpp: the level 1 survey, what is loaded and when, the decoders, the role policy; tests/ucsi_host.cpp: the levels
# 3/4 UCSI client, the witnesses and the role switch against a simulated PPM; tests/xhcihost_host.cpp: the
# level 5 host switch and xHCI probe against a register model; tests/smmu_identity_host.cpp: the level 6 identity-mapped SMMU
# context), then the objects in out/native-kexts/PianoUsbHost for tooling/k27/kext/PianoUsbHost.json. Inert without the boot
# argument piano-usbhost; levels 1 and 2 only load, 3 and 4 talk to the DSP through PianoPower's USB-C relay (its functions linked
# by name). PianoDwc3 (device mode) is asked through its platform function only (its UsbHandoff.h); its PianoUsbHw.h and
# SmmuStream.h, PianoPower's SPMI observer headers and package/common/PianoIrq.h are included, not copied.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoUsbHost; O=$D/out/native-kexts/PianoUsbHost
INC=(-I "$P" -I "$D/package/PianoDwc3" -I "$D/package/PianoPower")
mkdir -p "$O/host-tests"
for t in usbhost_host xhcihost_host smmu_identity_host ucsi_host; do
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "${INC[@]}" "$P/tests/$t.cpp" -o "$O/host-tests/$t"
  "$O/host-tests/$t" || { echo "Error: PianoUsbHost host test $t failed" >&2; exit 1; }
done
# The survey headers stay store-free: no str instruction and no assignment through a volatile pointer (levels 1 and 2 store only the
# SPMI observer's read command, in PianoUsbHost.cpp; the stores of levels 5 and 6 are XhciHost.h's and SmmuIdentity.h's, through
# their Hal, host-tested).
if grep -nE '"str|volatile[^;]*\*[^;]*[^=!<>]=[^=]' "$P/UsbHostSurvey.h" "$P/UsbHostHw.h"; then
  echo "Error: the survey headers have a store" >&2; exit 1
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoUsbHost "$P/PianoUsbHost.cpp" "$D/tooling/k27/NativeKextGlue.cpp" "${INC[@]}" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoPower's USB-C relay (package/include/PianoUsbcContract.h) and port-event functions (PianoAdspContract.h) come from PianoPower
# (listed before PianoUsbHost, named as a library by the descriptor): the KC link resolves them by name.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _piano_usbc_ \
  --library-prefix _piano_port_ --library-prefix _piano_spmi_words | cut -c1-200 >&2
