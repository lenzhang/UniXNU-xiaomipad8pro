#!/bin/bash
# Build package/ARMPlatformStandIn (AppleARMPlatform's classes for the vma2 collection's IOMobileGraphicsFamily, see its UPSTREAM) as a
# k27 native kext: objects in out/native-kexts/ARMPlatformStandIn for tooling/k27/kext/ARMPlatformStandIn.json.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
python3 "$D/tooling/k27/build-native-kext.py" ARMPlatformStandIn "$D/package/ARMPlatformStandIn/ARMPlatformStandIn.cpp" \
  "$D/tooling/k27/NativeKextGlue.cpp" --obj "${K27_OBJ:-k27-native-obj}"
