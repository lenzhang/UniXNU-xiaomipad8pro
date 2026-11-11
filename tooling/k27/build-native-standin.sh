#!/bin/bash
# Build package/ARMPlatformStandIn (AppleARMPlatform's classes for the vma2 collection's IOMobileGraphicsFamily, see its UPSTREAM) as a
# k27 native kext: objects in out/native-kexts/ARMPlatformStandIn for tooling/k27/kext/ARMPlatformStandIn.json.
# PIANO_USBHOST_STACK=1 (the collection with Apple's USB host stack, docs/drivers/usb-host.md section 5.2) adds UsbCableKeys.cpp,
# AppleARMPlatform's cable-type keys that IOUSBHostFamily and IOAccessoryManager refer to; without it the kext is built as before.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
EXTRA=()
[ "${PIANO_USBHOST_STACK:-0}" != 1 ] || EXTRA=("$D/package/ARMPlatformStandIn/UsbCableKeys.cpp")
python3 "$D/tooling/k27/build-native-kext.py" ARMPlatformStandIn "$D/package/ARMPlatformStandIn/ARMPlatformStandIn.cpp" \
  ${EXTRA[@]+"${EXTRA[@]}"} "$D/tooling/k27/NativeKextGlue.cpp" --obj "${K27_OBJ:-k27-native-obj}"
