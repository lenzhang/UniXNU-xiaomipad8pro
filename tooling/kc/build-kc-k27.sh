#!/bin/bash
# Build a k27 kernel collection from a linked k27 kernel, without touching anything else.
# This is the KC half of macOS-DroidVM's iterate-k27-piano.sh --no-boot, minus its side effects (it overwrote
# out/k27/kc-k27.bin, rsynced to the private build host and copied kernel-k27.nm into the tablet's rig/).
#
#   build-kc-k27.sh KERNEL OUTPUT.bin
#
# Environment:
#   K27_TREE   the macOS-DroidVM checkout (default ~/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM)
#   NATIVE     the native kext JSONs linked into the KC (default: the piano VM's full set, as iterate-k27-piano.sh)
#   KC_BASE    the Apple 27.0.1 (26A434) vma2 kernelcache the KC is built from (default $K27_TREE/out/kc/kc27.bin)
# Output: OUTPUT.bin, OUTPUT.natives.json, OUTPUT.log, OUTPUT.nm (llvm-nm -n of KERNEL), OUTPUT.sha256.
# No password, no ssh: copying the KC anywhere is the caller's business.
set -euo pipefail
[ $# = 2 ] || { echo "usage: $0 KERNEL OUTPUT.bin" >&2; exit 2; }
KERNEL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
OUT=$(mkdir -p "$(dirname "$2")" && cd "$(dirname "$2")" && pwd)/$(basename "$2")
T=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
KC_BASE=${KC_BASE:-$T/out/kc/kc27.bin}
NM=${LLVM_NM:-/opt/homebrew/opt/llvm/bin/llvm-nm}
cd "$T"
# The kernel was linked at the segment addresses of k27/segaddrs.sh; the KC layout must agree (else relink after --write).
if python3 k27/kc27-layout.py "$KC_BASE" "$KERNEL" | grep -q CONFLICT; then
  echo "layout CONFLICT: run k27/kc27-layout.py $KC_BASE $KERNEL --write k27/segaddrs.sh and link again" >&2; exit 1
fi
DI="com.apple.driver.DiskImages com.apple.driver.DiskImages.FileBackingStore com.apple.driver.DiskImages.KernelBacked com.apple.driver.DiskImages.RAMBackingStore com.apple.driver.DiskImages.ReadWriteDiskImage com.apple.driver.DiskImages.UDIFDiskImage"
NATIVE=${NATIVE:-overlay/kext/NativeVirtioBlock.json overlay/kext/NativeAES.json overlay/kext/NativeRamBlock.json overlay/kext/ARMPlatformStandIn.json overlay/kext/DroidVMDisplay.json overlay/kext/DroidVMEthernet.json overlay/kext/DroidVMInput.json overlay/kext/DroidVMGpu.json overlay/kext/DroidVMAudio.json overlay/kext/DroidVMVideo.json}
# shellcheck disable=SC2046,SC2086
python3 k27/kc-build27-piano.py "$KC_BASE" "$KERNEL" "$OUT" --keep $(cat configs/kc-keep.txt) $DI --native $NATIVE > "${OUT%.bin}.log" 2>&1 \
  || { tail -20 "${OUT%.bin}.log"; exit 1; }
grep -E "problem|wrote" "${OUT%.bin}.log" | cut -c1-120
"$NM" -n "$KERNEL" > "${OUT%.bin}.nm"
shasum -a 256 "$KERNEL" "$OUT" | tee "${OUT%.bin}.sha256"
