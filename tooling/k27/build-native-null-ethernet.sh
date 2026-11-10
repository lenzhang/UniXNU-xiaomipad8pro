#!/bin/bash
# Build package/PianoNullEthernet (a built-in en0 without hardware, which TimeSync and so WindowServer need) as a k27 native kext:
# objects in out/native-kexts/PianoNullEthernet for tooling/k27/kext/PianoNullEthernet.json.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
python3 "$D/tooling/k27/build-native-kext.py" PianoNullEthernet "$D/package/PianoNullEthernet/PianoNullEthernet.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$K/out/x12377/inc-net-piano" --obj "${K27_OBJ:-k27-native-obj}"
