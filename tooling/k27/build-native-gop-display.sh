#!/bin/bash
# Build package/NativeGopDisplay (the display: CPU copy into the GOP framebuffer, and its AppleParavirtGPU nub) as a k27 native kext:
# objects in out/native-kexts/NativeGopDisplay for tooling/k27/kext/NativeGopDisplay.json.
#   environment: PIANO_K27_TREE (the k27 tree, read only), K27_OBJ (default k27-native-obj), KC_BASE (default $PIANO_K27_TREE/out/kc/kc27.bin),
#                LLVM_BIN (where llvm-cxxfilt is)
# The IOMobileFramebuffer class declarations are generated from the collection the KC is built from (the k27 tree's
# scripts/gen-iomfb-abi.py, which reads the 100 MB KC: about 2 minutes), so the subclass's layout and vtable diversities are that
# collection's. The header is cached in out/k27/iomfb-abi/, keyed by the KC's SHA-256.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
KC_BASE=${KC_BASE:-$K/out/kc/kc27.bin}
export PATH="${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}:$PATH"
CACHE=$D/out/k27/iomfb-abi/$(shasum -a 256 "$KC_BASE" | cut -c1-16)
if [ ! -f "$CACHE/IOMobileFramebufferABI.h" ]; then
  mkdir -p "$CACHE"
  python3 "$K/scripts/gen-iomfb-abi.py" "$KC_BASE" "$CACHE/IOMobileFramebufferABI.h.tmp" && mv "$CACHE/IOMobileFramebufferABI.h.tmp" "$CACHE/IOMobileFramebufferABI.h"
fi
python3 "$D/tooling/k27/build-native-kext.py" NativeGopDisplay "$D/package/NativeGopDisplay/NativeGopDisplay.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$CACHE" --obj "${K27_OBJ:-k27-native-obj}"
