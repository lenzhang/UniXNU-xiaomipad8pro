#!/bin/bash
# Build package/PianoWifi/PianoWifiEthernet.cpp (the macOS side of the Wi-Fi path: an IOEthernetController that publishes en1
# with its link down and the scan list of PianoWifi's level 6, docs/drivers/wifi.md section 13.1) as a k27 native kext: the
# objects in out/native-kexts/PianoWifiEthernet for tooling/k27/kext/PianoWifiEthernet.json. It has no hardware of its own and
# matches the resource PianoWifiLink, which PianoWifi publishes only when level 6 came up, so it is inert without piano-wifi=6.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes and the networking headers), K27_OBJ
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
python3 "$D/tooling/k27/build-native-kext.py" PianoWifiEthernet "$D/package/PianoWifi/PianoWifiEthernet.cpp" \
  "$D/tooling/k27/NativeKextGlue.cpp" -I "$K/out/x12377/inc-net-piano" --obj "${K27_OBJ:-k27-native-obj}"
