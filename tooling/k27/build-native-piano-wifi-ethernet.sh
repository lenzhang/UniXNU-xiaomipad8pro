#!/bin/bash
# Build package/PianoWifi/PianoWifiEthernet.cpp (the macOS side of the Wi-Fi path: an IOEthernetController that publishes en1
# with its link down and the scan list of PianoWifi's level 6, docs/drivers/wifi.md section 13.1) as a k27 native kext: the
# objects in out/native-kexts/PianoWifiEthernet for tooling/k27/kext/PianoWifiEthernet.json. It has no hardware of its own and
# matches the resource PianoWifiLink, which PianoWifi publishes only when level 6 came up, so it is inert without piano-wifi=6.
# The same translation unit carries PianoWifi80211 (PianoWifi80211.h, docs/drivers/wifi-menu.md section 5: the controller as a
# Wi-Fi interface for airportd, only with piano-wifi-apple80211=1): its host test (tests/apple80211_host.cpp: the apple80211
# requests' layouts and answers) runs first, its kernel entries' stack is bounded after the compile, and the imports are checked
# against the kernel (IONetworkingFamily's and PianoWifi's are the KC link's to resolve).
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes and the networking headers), K27_OBJ, CXX (host
#                compiler for the test, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoWifi; O=$D/out/native-kexts/PianoWifiEthernet
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/apple80211_host.cpp" -o "$O/host-tests/apple80211_host"
"$O/host-tests/apple80211_host" || { echo "Error: PianoWifiEthernet host tests failed (apple80211_host)" >&2; exit 1; }
python3 "$D/tooling/k27/build-native-kext.py" PianoWifiEthernet "$D/package/PianoWifi/PianoWifiEthernet.cpp" \
  "$D/tooling/k27/NativeKextGlue.cpp" -I "$K/out/x12377/inc-net-piano" --obj "${K27_OBJ:-k27-native-obj}"
# PianoWifi80211's entries run on a requesting process's kernel stack (the ioctl, airportd's: 16 KiB with the syscall's and the
# network stack's frames above ours) and on thread calls: at most 3 KiB of our own frames each.
for entry in filterIoctl attachTick scanDoneTick reportTick; do
  python3 "$D/tooling/k27/check-kext-stack.py" "$O/PianoWifiEthernet.o" "$entry" --max 3072 --brief --indirect userIn,userOut >&2
done
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" \
  --library-prefix __ZN18IONetworkInterface --library-prefix __ZN19IONetworkController --library-prefix __ZNK19IONetworkController \
  --library-prefix __ZN20IOEthernetController --library-prefix __ZNK20IOEthernetController --library-prefix __ZN15IONetworkMedium \
  --library-prefix __ZN19IOEthernetInterface --library-prefix __ZTV19IONetworkController \
  --library-prefix __ZTV20IOEthernetController --library-prefix _piano_wifi_net_ | cut -c1-160 >&2
