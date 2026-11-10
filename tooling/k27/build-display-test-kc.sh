#!/bin/bash
# The display test collection: the native set of tooling/k27/build-native-image.sh (the DroidVM defaults + NativeUfs [+ the native
# NativeRamBlock]) plus NativeGopDisplay, linked into a KC of its own. The other kexts are taken as built (their objects in
# $K27_TREE/out/x12377/native-kexts); only NativeGopDisplay is compiled here. The default outputs (out/k27/kc-k27-native.bin,
# out/ram, out/ram-dump) are not touched.
#   tooling/k27/build-display-test-kc.sh [OUTPUT.bin]     default out/k27/kc-k27-display-test.bin
# Environment: K27_TREE (macOS-DroidVM), K27_KERNEL (default $K27_TREE/out/k27/kernel-k27-0058-native.macho),
#   NATIVE_BASE (the kext JSONs before NativeGopDisplay; default: build-native-image.sh's set, see below),
#   PIANO_DROP_KEXTS (names left out of that set, as in build-native-image.sh: e.g. "DroidVMGpu"; DroidVMDisplay is the display's nub,
#   so keep it with NativeGopDisplay).
# Output: OUTPUT.bin and, from tooling/kc/build-kc-k27.sh, OUTPUT.log / .natives.json / .nm / .sha256; OUTPUT.kext.log (the compile).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
KEXTS=$D/tooling/k27/kext
OUT=${1:-$D/out/k27/kc-k27-display-test.bin}
mkdir -p "$(dirname "$OUT")"
# build-native-image.sh's set (copied, not shared: that script's list and outputs stay the native run's). With the native Data disk
# (tooling/k27/kext/NativeRamBlock.json, over NativeUfs's store window) it replaces the VM's NativeRamBlock.
HEAD="overlay/kext/NativeVirtioBlock.json overlay/kext/NativeAES.json"
TAIL="overlay/kext/ARMPlatformStandIn.json overlay/kext/DroidVMDisplay.json overlay/kext/DroidVMEthernet.json overlay/kext/DroidVMInput.json overlay/kext/DroidVMGpu.json overlay/kext/DroidVMAudio.json overlay/kext/DroidVMVideo.json"
if [ -f "$KEXTS/NativeRamBlock.json" ]; then
  BASE="$HEAD $TAIL $KEXTS/NativeUfs.json $KEXTS/NativeRamBlock.json"
else
  BASE="$HEAD overlay/kext/NativeRamBlock.json $TAIL $KEXTS/NativeUfs.json"
fi
BASE=${NATIVE_BASE:-$BASE}
for k in ${PIANO_DROP_KEXTS:-}; do BASE=$(for spec in $BASE; do [ "$(basename "$spec")" = "$k.json" ] || printf '%s ' "$spec"; done); done
case " ${PIANO_DROP_KEXTS:-} " in *" DroidVMDisplay "*) echo "note: without DroidVMDisplay nothing provides AppleParavirtGPU, NativeGopDisplay's nub" >&2 ;; esac
for spec in $BASE; do   # every other kext must already be built: this script compiles none of them
  case $spec in /*) json=$spec ;; *) json=$K/$spec ;; esac
  objects=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("objects", ""))' "$json")
  [ -z "$objects" ] || [ -f "$K/$objects" ] || { echo "$spec: no $K/$objects (build that kext first)" >&2; exit 1; }
done
"$D/tooling/k27/build-native-gop-display.sh" > "${OUT%.bin}.kext.log" 2>&1 || { grep -iE 'error' "${OUT%.bin}.kext.log" | head; exit 1; }
NATIVE="$BASE $KEXTS/NativeGopDisplay.json" "$D/tooling/kc/build-kc-k27.sh" "${K27_KERNEL:-$K/out/k27/kernel-k27-0058-native.macho}" "$OUT"
grep -E 'NativeGopDisplay|vtable check' "${OUT%.bin}.log" | cut -c1-140
