#!/bin/bash
# Build package/PianoDwc3 (the USB device-mode debug channel, docs/drivers/usb-device-debug.md) as a k27 native kext: objects in
# out/native-kexts/PianoDwc3/ for tooling/k27/kext/PianoDwc3.json ("@OUT@" = out/). Runs the two host tests first.
#   tooling/k27/build-piano-dwc3.sh
# Environment: K27_TREE (the k27 tree, only read), PIANO_OUT (default out/), PIANO_KEXT_BUILDER (default
# tooling/k27/build-native-kext.py; without it, macOS-DroidVM's scripts/build-native-kext.py and its out/x12377/native-kexts).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
P=$D/package/PianoDwc3
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
for t in usb_gadget_host smmu_stream_host; do
  c++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -I "$P" "$P/tests/$t.cpp" -o "$T/$t"
  "$T/$t"
done
B=${PIANO_KEXT_BUILDER:-$D/tooling/k27/build-native-kext.py}
if [ -f "$B" ]; then
  G=$(dirname "$B")/NativeKextGlue.cpp
  python3 "$B" PianoDwc3 "$P/PianoDwc3.cpp" "$G" -I "$P" --out "${PIANO_OUT:-$D/out}/native-kexts/PianoDwc3" --obj "${K27_OBJ:-k27-native-obj}"
else
  cd "$K"
  python3 scripts/build-native-kext.py PianoDwc3 "$P/PianoDwc3.cpp" overlay/kext/NativeKextGlue.cpp -I "$P" --obj "${K27_OBJ:-k27-native-obj}"
fi
