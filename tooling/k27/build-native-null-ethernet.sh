#!/bin/bash
# Build package/PianoNullEthernet (a built-in en0 without hardware, which TimeSync and so WindowServer need) as a k27 native kext:
# objects in out/native-kexts/PianoNullEthernet for tooling/k27/kext/PianoNullEthernet.json.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
python3 "$D/tooling/k27/build-native-kext.py" PianoNullEthernet "$D/package/PianoNullEthernet/PianoNullEthernet.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$K/out/x12377/inc-net-piano" --obj "${K27_OBJ:-k27-native-obj}"
