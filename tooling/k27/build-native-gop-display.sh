#!/bin/bash
# Build package/NativeGopDisplay (DroidVMDisplay's CPU copy into the boot framebuffer, see package/NativeGopDisplay/UPSTREAM) as a k27
# native kext: objects for tooling/k27/kext/NativeGopDisplay.json, in $K27_TREE/out/x12377/native-kexts/NativeGopDisplay (its own
# directory: nothing of NativeUfs or of the DroidVM kexts is rebuilt or overwritten).
#   tooling/k27/build-native-gop-display.sh     environment: K27_TREE (macOS-DroidVM), K27_OBJ (default k27-native-obj),
#                                               KC_BASE (default $K27_TREE/out/kc/kc27.bin), LLVM_BIN (where llvm-cxxfilt is)
# The IOMobileFramebuffer class declarations are generated from the collection the KC is built from (scripts/gen-iomfb-abi.py),
# so the subclass's layout and vtable diversities are that collection's (27.0.1 26A434: IOMobileFramebufferLegacy 0x1dd8 bytes).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
KC_BASE=${KC_BASE:-$K/out/kc/kc27.bin}
export PATH="${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}:$PATH"
INC=$(mktemp -d); trap 'rm -rf "$INC"' EXIT
python3 "$K/scripts/gen-iomfb-abi.py" "$KC_BASE" "$INC/IOMobileFramebufferABI.h"
# DroidVMDisplay's copy in the same tree is the header the collection's other display was built with: say when they differ.
cmp -s "$INC/IOMobileFramebufferABI.h" "$K/overlay/kext/DroidVMDisplay/IOMobileFramebufferABI.h" \
  || echo "note: the generated IOMobileFramebufferABI.h differs from $K/overlay/kext/DroidVMDisplay's (another collection?)"
cd "$K"
python3 scripts/build-native-kext.py NativeGopDisplay "$D/package/NativeGopDisplay/NativeGopDisplay.cpp" overlay/kext/NativeKextGlue.cpp \
  -I "$INC" --obj "${K27_OBJ:-k27-native-obj}"
