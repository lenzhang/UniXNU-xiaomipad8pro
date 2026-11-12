#!/bin/bash
# Build package/NativeGopDisplay (the display: CPU composition into the GOP framebuffer, and its AppleParavirtGPU nub) as a k27 native
# kext: the host tests first (tests/gop_host.cpp: GopCompose.h's geometry, pixels, layers, jobs, panel plan, refresh copies and the
# simulation of WindowServer, the family's swaps, the GPU fences and the copy thread), then the objects in
# out/native-kexts/NativeGopDisplay for tooling/k27/kext/NativeGopDisplay.json.
#   environment: PIANO_K27_TREE (the k27 tree, read only), K27_OBJ (default k27-native-obj), KC_BASE (default $PIANO_K27_TREE/out/kc/kc27.bin),
#                LLVM_BIN (where llvm-cxxfilt is), CXX (host compiler for the tests, default c++)
# The IOMobileFramebuffer class declarations are generated from the collection the KC is built from (the k27 tree's
# scripts/gen-iomfb-abi.py, which reads the 100 MB KC: about 2 minutes), so the subclass's layout and vtable diversities are that
# collection's. The header is cached in out/k27/iomfb-abi/, keyed by the KC's SHA-256.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
KC_BASE=${KC_BASE:-$K/out/kc/kc27.bin}
export PATH="${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}:$PATH"
P=$D/package/NativeGopDisplay; O=$D/out/native-kexts/NativeGopDisplay
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/gop_host.cpp" -o "$O/host-tests/gop_host"
"$O/host-tests/gop_host" || { echo "Error: NativeGopDisplay host tests failed" >&2; exit 1; }
CACHE=$D/out/k27/iomfb-abi/$(shasum -a 256 "$KC_BASE" | cut -c1-16)
# The family layout the driver's offsets are for (NativeGopDisplay.cpp NATIVE_GOP_IOMFB_ABI): 1, the 27.0.1 vma2 collection's;
# 2, the 27.3 mac17j collection's (IOMobileFramebufferLegacy 9120 bytes, IOMFBSwapIORequest 4056; tooling/k273/diff-field-offsets.py).
# A collection without AppleParavirtDisplay (a Mac's: no IOMobileFramebufferLegacy subclass names the pure virtual slots) takes the
# declarations generated from IOMFB_ABI_REFERENCE_KC (default the k27 tree's kc27.bin) once tooling/k273/check-family-abi.py has
# found the classes' vtables the same there, with the target's instance size.
if [ ! -f "$CACHE/IOMobileFramebufferABI.h" ] || [ ! -f "$CACHE/abi" ]; then
  mkdir -p "$CACHE"
  if python3 "$K/scripts/kc-vtable.py" "$KC_BASE" AppleParavirtDisplay > /dev/null 2>&1; then
    python3 "$K/scripts/gen-iomfb-abi.py" "$KC_BASE" "$CACHE/IOMobileFramebufferABI.h.tmp" && mv "$CACHE/IOMobileFramebufferABI.h.tmp" "$CACHE/IOMobileFramebufferABI.h"
    echo 1 > "$CACHE/abi"
  else
    REF=${IOMFB_ABI_REFERENCE_KC:-$K/out/kc/kc27.bin}
    python3 "$D/tooling/k273/check-family-abi.py" --vtables-only "$REF" "$KC_BASE" IOMobileFramebuffer@com.apple.iokit.IOMobileGraphicsFamily \
      IOMobileFramebufferLegacy@com.apple.iokit.IOMobileGraphicsFamily IOMFBSwapIORequest@com.apple.iokit.IOMobileGraphicsFamily > "$CACHE/abi-check.txt" \
      || { cat "$CACHE/abi-check.txt" >&2; echo "Error: the IOMobileFramebuffer vtables of $KC_BASE are not $REF's" >&2; exit 1; }
    SIZES=$(awk '$1 == "size" {printf "%s:%s:%s ", $2, $3, $4}' "$CACHE/abi-check.txt")
    case " $SIZES " in
      *" IOMobileFramebuffer:144:144 IOMobileFramebufferLegacy:7640:9120 IOMFBSwapIORequest:3400:4056 "*) ABI=2 ;;
      *) echo "Error: no NativeGopDisplay offsets for this IOMobileGraphicsFamily layout ($SIZES)" >&2; exit 1 ;;
    esac
    python3 "$K/scripts/gen-iomfb-abi.py" "$REF" "$CACHE/IOMobileFramebufferABI.h.ref"
    sed -e 's/0x1dd8/0x23a0/g' "$CACHE/IOMobileFramebufferABI.h.ref" > "$CACHE/IOMobileFramebufferABI.h"
    grep -q 'sizeof(IOMobileFramebufferLegacy) == 0x23a0' "$CACHE/IOMobileFramebufferABI.h" || { echo "Error: instance size not replaced" >&2; exit 1; }
    echo $ABI > "$CACHE/abi"
  fi
fi
python3 "$D/tooling/k27/build-native-kext.py" NativeGopDisplay "$D/package/NativeGopDisplay/NativeGopDisplay.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$CACHE" -D "NATIVE_GOP_IOMFB_ABI=$(cat "$CACHE/abi")" --obj "${K27_OBJ:-k27-native-obj}"
