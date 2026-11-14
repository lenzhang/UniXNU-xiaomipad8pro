#!/bin/bash
# Build package/PianoXhciHost (path A2: the three-override subclass of this collection's generic Apple xHCI driver,
# docs/drivers/usb-host.md §5.4) as a k27 native kext: objects in out/native-kexts/PianoXhciHost for
# tooling/k27/kext/PianoXhciHost.json. Linked only with PIANO_USBHOST_A2=1 (tooling/k27/build-native-image.sh).
#
# The build fails closed on ABI drift, in this order:
#   1. tooling/k27/xhci-abi.py self-test      the gates themselves still refuse a drifted vtable and a changed collection
#   2. host test reg64_host                   the 64-bit read/write pair against a model register file (the hi-lo order)
#   3. xhci-abi.py check-kc KC_BASE           the collection is the one data/usb-xhci/abi-26C5035r-mac17j.json was made from
#                                             (sha256, all 427 slot targets and diversities, IA + address, instance size)
#   4. xhci-abi.py header                     OriginalUsbXhciABI.h generated into out/native-kexts/PianoXhciHost (never committed)
#   5. build-native-kext.py                   the subclass compiled (the generated header's static_assert checks the size)
#   6. xhci-abi.py thunks + clang             xhci-thunks.S: one `bti c; b <the collection's symbol>` per inherited slot whose
#                                             fabricated declaration name no collection defines; the branches are resolved by
#                                             name by the collection builder, so no address is hard-coded
#   7. xhci-abi.py check-object               the compiled vtable: exactly 427 slots, every entry signed with the collection's
#                                             diversity, only slots 292/419/421 (and the destructors/getMetaClass) ours, and every
#                                             other slot reaching the recorded symbol - directly or through a checked thunk
#
#   environment: KC_BASE (the Apple collection the KC is built from; default the 27.3 mac17j input), PIANO_K27_TREE (read only:
#                the compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler for the test, default c++)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
P=$D/package/PianoXhciHost; O=$D/out/native-kexts/PianoXhciHost
KC=${KC_BASE:-$D/out/inputs/k273/kc/kc273-mac17j.bin}
mkdir -p "$O/host-tests"
python3 "$D/tooling/k27/xhci-abi.py" self-test
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/reg64_host.cpp" -o "$O/host-tests/reg64_host"
"$O/host-tests/reg64_host" || { echo "Error: PianoXhciHost host test reg64_host failed" >&2; exit 1; }
[ -f "$KC" ] || { echo "Error: no Apple collection at $KC (KC_BASE): the A2 ABI gate needs the collection it is pinned to" >&2; exit 1; }
python3 "$D/tooling/k27/xhci-abi.py" check-kc "$KC"
python3 "$D/tooling/k27/xhci-abi.py" header --out "$O"
python3 "$D/tooling/k27/build-native-kext.py" PianoXhciHost "$P/PianoXhciHost.cpp" -I "$P" -I "$O" --obj "${K27_OBJ:-k27-native-obj}"
python3 "$D/tooling/k27/xhci-abi.py" thunks "$O/PianoXhciHost.o" --out "$O"
xcrun clang -arch arm64e -c "$O/xhci-thunks.S" -o "$O/xhci-thunks.o"
python3 "$D/tooling/k27/xhci-abi.py" check-object "$O/PianoXhciHost.o" --thunks "$O/xhci-thunks.o"
# Both objects go into the kext (tooling/k27/kext/PianoXhciHost.json reads this list).
python3 -c 'import json,sys; json.dump([sys.argv[1], sys.argv[2]], open(sys.argv[3], "w"), indent=1)' \
  "$O/PianoXhciHost.o" "$O/xhci-thunks.o" "$O/objects.json"
echo "PianoXhciHost: 2 object(s) in $O (the subclass and its ABI thunks)"
