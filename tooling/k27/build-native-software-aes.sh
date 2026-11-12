#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Build package/SoftwareAES (UniXNU's software IOAESAccelerator, for AppleImage4's finalization: package/SoftwareAES/UPSTREAM) as a k27
# native kext: objects in out/native-kexts/SoftwareAES for tooling/k27/kext/SoftwareAES.json.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
python3 "$D/tooling/k27/build-native-kext.py" SoftwareAES "$D/package/SoftwareAES/NativeAES.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  --obj "${K27_OBJ:-k27-native-obj}"
