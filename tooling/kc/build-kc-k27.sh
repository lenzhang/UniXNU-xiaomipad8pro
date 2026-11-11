#!/bin/bash
# Build a k27 kernel collection from a linked k27 kernel, without touching anything else.
# The KC half of the k27 tree's iterate-k27-piano.sh --no-boot, without its side effects (it overwrote out/k27/kc-k27.bin,
# rsynced to the signing host and copied kernel-k27.nm into the tablet's rig/).
#
#   build-kc-k27.sh KERNEL OUTPUT.bin
#
# Environment:
#   PIANO_K27_TREE  the k27 tree (environment or ~/.config/piano/env, tooling/k27/k27-tree.sh): its KC builder, KC base and
#              layout are read, nothing is written there
#   NATIVE     the native kext JSONs linked into the KC (required; the native set is configs/k27-native-kexts.txt)
#   KC_BASE    the Apple 27.0.1 (26A434) vma2 kernelcache the KC is built from (default $PIANO_K27_TREE/out/kc/kc27.bin)
#   KC_NATIVE_PLACEMENT  best-fit (the default here): the native kexts' segments largest first into the smallest gaps that hold
#              them, with patches/k27-kc/0001 applied to the tree's builder (without it: first fit in link order, and a note);
#              first-fit: as the unpatched builder. KC_NATIVE_CONST_TO_TEXT=1 (patches/k27-kc/0002): relocation-free constant
#              data of a native kext in its __TEXT instead of __DATA_CONST. docs/kc-space.md.
# Output: OUTPUT.bin, OUTPUT.natives.json, OUTPUT.log, OUTPUT.nm (llvm-nm -n of KERNEL), OUTPUT.sha256; the headroom left per
# segment kind (tooling/k27/kc-space.py --brief) on stdout.
# No password, no ssh: copying the KC anywhere is the caller's business.
set -euo pipefail
PIANO_REPO=$(cd "$(dirname "$0")/../.." && pwd)   # before any cd
[ $# = 2 ] || { echo "usage: $0 KERNEL OUTPUT.bin" >&2; exit 2; }
KERNEL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
OUT=$(mkdir -p "$(dirname "$2")" && cd "$(dirname "$2")" && pwd)/$(basename "$2")
. "$PIANO_REPO/tooling/k27/k27-tree.sh"; T=$PIANO_K27_TREE
KC_BASE=${KC_BASE:-$T/out/kc/kc27.bin}
NM=${LLVM_NM:-/opt/homebrew/opt/llvm/bin/llvm-nm}
cd "$T"
# The kernel was linked at the segment addresses of k27/segaddrs.sh; the KC layout must agree (else relink after --write).
if python3 k27/kc27-layout.py "$KC_BASE" "$KERNEL" | grep -q CONFLICT; then
  echo "layout CONFLICT: run k27/kc27-layout.py $KC_BASE $KERNEL --write k27/segaddrs.sh and link again" >&2; exit 1
fi
DI="com.apple.driver.DiskImages com.apple.driver.DiskImages.FileBackingStore com.apple.driver.DiskImages.KernelBacked com.apple.driver.DiskImages.RAMBackingStore com.apple.driver.DiskImages.ReadWriteDiskImage com.apple.driver.DiskImages.UDIFDiskImage"
[ -n "${NATIVE:-}" ] || { echo "NATIVE (the kext descriptors) is required: tooling/k27/build-native-image.sh sets it from configs/k27-native-kexts.txt" >&2; exit 2; }
# The native kext rule: only this repository's kexts in a native collection (tooling/k27/check-native-kexts.py, which first
# proves on synthetic descriptors that it still refuses the VM-era kexts; docs/bringup-debugging.md §5).
python3 "$PIANO_REPO/tooling/k27/check-native-kexts.py" --self-test >&2 || exit 2
# shellcheck disable=SC2086
python3 "$PIANO_REPO/tooling/k27/check-native-kexts.py" "$PIANO_REPO" $NATIVE >&2 || exit 2
export KC_NATIVE_PLACEMENT=${KC_NATIVE_PLACEMENT:-best-fit}
# shellcheck disable=SC2046,SC2086
python3 k27/kc-build27-piano.py "$KC_BASE" "$KERNEL" "$OUT" --keep $(cat configs/kc-keep.txt) $DI --native $NATIVE > "${OUT%.bin}.log" 2>&1 \
  || { tail -20 "${OUT%.bin}.log"; exit 1; }
grep -E "problem|wrote" "${OUT%.bin}.log" | cut -c1-120
grep -q "^native placement ($KC_NATIVE_PLACEMENT)" "${OUT%.bin}.log" \
  || echo "note: the k27 tree's KC builder lacks patches/k27-kc/0001: native kexts placed first fit in link order (patches/k27-kc/README.md)"
python3 "$PIANO_REPO/tooling/k27/kc-space.py" "$OUT" --brief || true
python3 "$PIANO_REPO/tooling/k27/check-native-kexts.py" "$PIANO_REPO" --kc-log "${OUT%.bin}.log" || { rm -f "$OUT"; exit 2; }
"$NM" -n "$KERNEL" > "${OUT%.bin}.nm"
shasum -a 256 "$KERNEL" "$OUT" | tee "${OUT%.bin}.sha256"
