#!/bin/bash
# Build package/NativeUfs (ShengUfs b11de87 + the SM8750/piano deltas) as a k27 native kext (objects for tooling/k27/kext/NativeUfs.json).
#   tooling/k27/build-native-ufs.sh        environment: K27_TREE (the k27 build tree, read only), UNIXNU_RESEARCH, K27_OBJ
# Objects: out/native-kexts/NativeUfs.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
INC=$(mktemp -d); trap 'rm -rf "$INC"' EXIT
ln -s "$R/UniXNU-sm8750-common/kits/Sm8750Platform" "$INC/Sm8750Platform"
# The development build (NATIVE_UFS_UNVERIFIED_ROOT) needs neither VerifiedRoot nor NativeVirtioBlock's VerifiedBlockPolicy.h.
python3 "$D/tooling/k27/build-native-kext.py" NativeUfs "$D/package/NativeUfs/NativeUfs.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$D/package/NativeUfs" -I "$INC" -I "$K/out/x12377/storage-abi/include" -I "$K/out/x12377/xnu-m/osfmk" \
  -D U:TARGET_OS_OSX -D TARGET_OS_OSX=1 -D NATIVE_UFS_SM8750=1 -D 'UFS_ROOT_PARTITION="linux_rootfs"' -D NATIVE_UFS_POLL_ONLY=1 -D NATIVE_UFS_UNVERIFIED_ROOT=1 \
  --obj "${K27_OBJ:-k27-native-obj}"
