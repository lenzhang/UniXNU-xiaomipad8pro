#!/bin/bash
# Build package/PianoDwc3 (USB device-mode debug channel: two CDC-ACM ports, docs/drivers/usb-device-debug.md) as a k27 native kext:
# objects in out/native-kexts/PianoDwc3 for tooling/k27/kext/PianoDwc3.json. Inert without the boot argument piano-usb.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
"$D/package/PianoDwc3/tests/run.sh"   # host tests of the gadget core and the SMMU planner (a few seconds)
python3 "$D/tooling/k27/build-native-kext.py" PianoDwc3 "$D/package/PianoDwc3/PianoDwc3.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$D/package/PianoDwc3" --obj "${K27_OBJ:-k27-native-obj}"
