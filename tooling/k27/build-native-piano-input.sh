#!/bin/bash
# Build package/PianoInput (native input: the power and volume keys as a HID consumer control) as a k27 native kext: the host
# tests first (package/common's; tests/buttons_host.cpp: SPMI observer reads, key decoding, debounce, the adaptive polling, the
# report descriptor), then the objects in out/native-kexts/PianoInput for tooling/k27/kext/PianoInput.json. Inert without the
# boot argument piano-input.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes and out/x12377/inc-hid, the IOKit/hid headers
#                the VM-era HID kext was built with), K27_OBJ (default k27-native-obj), CXX (host compiler for the tests, default c++)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
HID=$K/out/x12377/inc-hid
[ -f "$HID/IOKit/hid/IOHIDDevice.h" ] || { echo "Error: no IOKit/hid headers at $HID" >&2; exit 1; }
T=$D/out/native-kexts/PianoInput/host-tests; mkdir -p "$T"
"$D/package/common/tests/run.sh"   # PianoIrq.h: the adaptive-poll state machine PianoInput's loop uses
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$D/package/PianoInput" "$D/package/PianoInput/tests/buttons_host.cpp" -o "$T/buttons_host"
"$T/buttons_host" || { echo "Error: PianoInput host tests failed" >&2; exit 1; }
python3 "$D/tooling/k27/build-native-kext.py" PianoInput "$D/package/PianoInput/PianoInput.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$D/package/PianoInput" -I "$HID" -I "$K/out/x12377/inc-gfx" -I "$K/out/x12377/storage-abi/include" -D U:TARGET_OS_OSX -D TARGET_OS_OSX=1 \
  --obj "${K27_OBJ:-k27-native-obj}"
