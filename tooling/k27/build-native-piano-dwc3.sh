#!/bin/bash
# Build package/PianoDwc3 (USB device-mode debug channel: CDC-ACM console, user pipe, and with piano-usb-hid=1 the input port and its
# keyboard/pointer HID devices; docs/drivers/usb-device-debug.md) as a k27 native kext: the host tests first (package/PianoDwc3/tests/
# run.sh), then the objects in out/native-kexts/PianoDwc3 for tooling/k27/kext/PianoDwc3.json. Inert without the boot argument piano-usb.
#   environment: K27_TREE (the k27 build tree, read only: compile recipes and out/x12377/inc-hid, the IOKit/hid headers PianoInput and
#                PianoKeyboard build with), K27_OBJ (default k27-native-obj), K27_KERNEL (for the import check)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
HID=$K/out/x12377/inc-hid
[ -f "$HID/IOKit/hid/IOHIDDevice.h" ] || { echo "Error: no IOKit/hid headers at $HID" >&2; exit 1; }
"$D/package/PianoDwc3/tests/run.sh"   # host tests: gadget core, SMMU planner, input command parser (a few seconds)
python3 "$D/tooling/k27/build-native-kext.py" PianoDwc3 "$D/package/PianoDwc3/PianoDwc3.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$D/package/PianoDwc3" -I "$D/package/PianoKeyboard" -I "$HID" -I "$K/out/x12377/inc-gfx" -I "$K/out/x12377/storage-abi/include" \
  -D U:TARGET_OS_OSX -D TARGET_OS_OSX=1 --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# IOHIDDevice's symbols come from IOHIDFamily (a library of the descriptor), resolved by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$D/out/native-kexts/PianoDwc3/objects.json" "$KERNEL" \
  --library-prefix __ZN11IOHIDDevice --library-prefix __ZNK11IOHIDDevice --library-prefix __ZTV11IOHIDDevice | cut -c1-160
