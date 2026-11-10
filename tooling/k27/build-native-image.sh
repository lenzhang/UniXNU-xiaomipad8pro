#!/bin/bash
# One command after a NativeUfs or k27 change: native kext -> KC (with NativeUfs) -> macOS RAM image (FD A). Prints the image hash.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
tooling/k27/build-native-ufs.sh > out/k27/build-native-ufs.log 2>&1 || { grep error: out/k27/build-native-ufs.log | head; exit 1; }
DEF="overlay/kext/NativeVirtioBlock.json overlay/kext/NativeAES.json overlay/kext/NativeRamBlock.json overlay/kext/ARMPlatformStandIn.json overlay/kext/DroidVMDisplay.json overlay/kext/DroidVMEthernet.json overlay/kext/DroidVMInput.json overlay/kext/DroidVMGpu.json overlay/kext/DroidVMAudio.json overlay/kext/DroidVMVideo.json"
NATIVE="$DEF $D/tooling/k27/kext/NativeUfs.json" tooling/kc/build-kc-k27.sh "${K27_KERNEL:-$K/out/k27/kernel-k27-0058-native.macho}" out/k27/kc-k27-native.bin > out/k27/build-kc.log 2>&1 || { tail -5 out/k27/build-kc.log; exit 1; }
python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --out out/ram --kc out/k27/kc-k27-native.bin \
  --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt --utc "$(date +%s)" | tail -1
