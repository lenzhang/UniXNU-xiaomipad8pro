#!/bin/bash
# Build package/PianoUsbHostStandIn (the stand-in for the seven Apple SoC kexts Apple's USB host stack reaches through its libraries,
# docs/drivers/usb-host.md section 5.2) as a k27 native kext: objects in out/native-kexts/PianoUsbHostStandIn for
# tooling/k27/kext/PianoUsbHostStandIn.json. Linked only with PIANO_USBHOST_STACK=1 (tooling/k27/build-native-image.sh).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
python3 "$D/tooling/k27/build-native-kext.py" PianoUsbHostStandIn "$D/package/PianoUsbHostStandIn/PianoUsbHostStandIn.cpp" \
  "$D/tooling/k27/NativeKextGlue.cpp" --obj "${K27_OBJ:-k27-native-obj}"
