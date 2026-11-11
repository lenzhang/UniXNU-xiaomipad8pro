#!/bin/bash
# Build package/NativeRamBlock (the Data disk: RAM seed + its changed pages in NativeUfs's store window, data-persist=2) as a k27 native
# kext: objects in out/native-kexts/NativeRamBlock for tooling/k27/kext/NativeRamBlock.json. See package/NativeRamBlock/UPSTREAM.
#   environment: PIANO_K27_TREE (the k27 tree, read only), K27_OBJ (default k27-native-obj)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
python3 "$D/tooling/k27/build-native-kext.py" NativeRamBlock "$D/package/NativeRamBlock/NativeRamBlock.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$D/package/NativeRamBlock" -I "$K/out/x12377/storage-abi/include" -I "$K/out/x12377/xnu-m/osfmk" \
  -D U:TARGET_OS_OSX -D TARGET_OS_OSX=1 --obj "${K27_OBJ:-k27-native-obj}"
