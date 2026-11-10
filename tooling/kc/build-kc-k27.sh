#!/bin/bash
# Build a k27 kernel collection from a linked k27 kernel, without touching anything else.
# This is the KC half of macOS-DroidVM's iterate-k27-piano.sh --no-boot, minus its side effects (it overwrote
# out/k27/kc-k27.bin, rsynced to the private build host and copied kernel-k27.nm into the tablet's rig/).
#
#   build-kc-k27.sh KERNEL OUTPUT.bin
#
# Environment:
#   K27_TREE   the k27 build tree: its KC builder, KC base and layout are read, nothing is written there
#              (default ~/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM)
#   NATIVE     the native kext JSONs linked into the KC (required; the native set is configs/k27-native-kexts.txt)
#   KC_BASE    the Apple 27.0.1 (26A434) vma2 kernelcache the KC is built from (default $K27_TREE/out/kc/kc27.bin)
# Output: OUTPUT.bin, OUTPUT.natives.json, OUTPUT.log, OUTPUT.nm (llvm-nm -n of KERNEL), OUTPUT.sha256.
# No password, no ssh: copying the KC anywhere is the caller's business.
set -euo pipefail
PIANO_REPO=$(cd "$(dirname "$0")/../.." && pwd)   # before any cd
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
[ -n "${NATIVE:-}" ] || { echo "NATIVE (the kext descriptors) is required: tooling/k27/build-native-image.sh sets it from configs/k27-native-kexts.txt" >&2; exit 2; }
# The native line's rule: no DroidVM kext in a native collection (tooling/k27/check-native-kexts.py, docs/bringup-debugging.md).
# shellcheck disable=SC2086
python3 "$PIANO_REPO/tooling/k27/check-native-kexts.py" "$PIANO_REPO" $NATIVE >&2 || exit 2
# shellcheck disable=SC2046,SC2086
python3 k27/kc-build27-piano.py "$KC_BASE" "$KERNEL" "$OUT" --keep $(cat configs/kc-keep.txt) $DI --native $NATIVE > "${OUT%.bin}.log" 2>&1 \
  || { tail -20 "${OUT%.bin}.log"; exit 1; }
grep -E "problem|wrote" "${OUT%.bin}.log" | cut -c1-120
python3 "$PIANO_REPO/tooling/k27/check-native-kexts.py" "$PIANO_REPO" --kc-log "${OUT%.bin}.log" || { rm -f "$OUT"; exit 2; }
"$NM" -n "$KERNEL" > "${OUT%.bin}.nm"
shasum -a 256 "$KERNEL" "$OUT" | tee "${OUT%.bin}.sha256"
