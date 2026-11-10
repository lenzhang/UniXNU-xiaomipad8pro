#!/bin/bash
# Build package/NativeSoftGpu (a registry IOAccelerator that only advertises the NativeSoftMetal plugin: WindowServer without a GPU driver)
# as a k27 native kext: objects in out/native-kexts/NativeSoftGpu, and out/k27/kext/NativeSoftGpu.json (the descriptor with @OUT@
# filled in) for a test KC: PIANO_EXTRA_KEXTS=out/k27/kext/NativeSoftGpu.json tooling/k27/build-native-image.sh.
#   environment: K27_TREE (the k27 build tree, read only), K27_OBJ (default k27-native-obj), MACOS_SDK (the macOS SDK whose
#                Kernel.framework has IOKit/graphics/IOAccelerator.h; default: xcrun's macosx SDK)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${MACOS_SDK:-$(xcrun --sdk macosx --show-sdk-path)}
GFX=$D/out/k27/inc-gfx   # IOKit/graphics alone, so no other Kernel.framework header comes before xnu's own
mkdir -p "$GFX/IOKit"
ln -sfn "$SDK/System/Library/Frameworks/Kernel.framework/Headers/IOKit/graphics" "$GFX/IOKit/graphics"
[ -f "$GFX/IOKit/graphics/IOAccelerator.h" ] || { echo "no IOKit/graphics/IOAccelerator.h in $SDK" >&2; exit 1; }
python3 "$D/tooling/k27/build-native-kext.py" NativeSoftGpu "$D/package/NativeSoftGpu/NativeSoftGpu.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$GFX" --obj "${K27_OBJ:-k27-native-obj}"
mkdir -p "$D/out/k27/kext"
sed "s#@OUT@#$D/out#" "$D/tooling/k27/kext/NativeSoftGpu.json" > "$D/out/k27/kext/NativeSoftGpu.json"
echo "$D/out/k27/kext/NativeSoftGpu.json"
