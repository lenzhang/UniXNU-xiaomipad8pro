#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The 27.3 line's k27 tree: an APFS clone of PIANO_K27_TREE (never changed itself) with patches/k273-kc applied, the segment
# addresses of 27.3's mac17j collection (k27/kc27-layout.py --write) and the native kernel relinked at them.
#
#   prepare-k27-tree.sh [KERNEL_TAG]     -> out/k273/k27-tree, its out/k27/kernel-k273-<TAG>-native.macho (TAG default 0065)
#
# The kernel is the k27 tree's source as it is (xnu-12377 + 0001-0052 + patches/k27 0053-...); only its link addresses differ from
# the 27.0.1 line's. The clone's build recompiles everything once (its objects' recipes name the other tree's paths), a few minutes.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
. "$D/tooling/k27/k27-tree.sh"
SRC=$PIANO_K27_TREE
T=${K273_TREE:-$D/out/k273/k27-tree}
KC=${K273_INPUTS:-$D/out/inputs/k273}/kc/kc273-mac17j.bin
TAG=${1:-0065}
[ -f "$KC" ] || { echo "no $KC: tooling/k273/decompress-kernelcache.py" >&2; exit 1; }
if [ ! -d "$T" ]; then
  mkdir -p "$(dirname "$T")"
  cp -c -R "$SRC" "$T"
fi
# The tree's links to its sibling checkouts (sources/UniXNU -> ../../UniXNU: DarwinBoot's packed_ramdisk, ...) are relative: in
# the clone they would point next to the clone. Point them where the original's resolve.
for l in "$T"/sources/*; do
  if [ -L "$l" ] && [ "$(readlink "$l")" != "$(cd "$SRC/sources" && cd "$(readlink "$l")" 2> /dev/null && pwd -P)" ]; then
    target=$(cd "$SRC/sources" && cd "$(readlink "$l")" && pwd -P)
    rm "$l"; ln -s "$target" "$l"
  fi
done
for p in "$D"/patches/k273-kc/*.patch; do
  if git -C "$T" apply --check "$p" 2> /dev/null; then git -C "$T" apply "$p"; echo "applied $(basename "$p")"
  elif git -C "$T" apply --reverse --check "$p" 2> /dev/null; then echo "$(basename "$p"): applied already"
  else echo "$(basename "$p") does not apply to $T" >&2; exit 1; fi
done
cd "$T"
python3 k27/kc27-layout.py "$KC" "out/k27/kernel-k27-$TAG-native.macho" --natives "$D/tooling/k27/kext" --write k27/segaddrs.sh
OBJ=out/x12377/build/xnu/k27-native-obj/DEVELOPMENT_ARM64_BCM2837
rm -rf "$OBJ"/mach.development.bcm2837 "$OBJ"/mach.development.bcm2837.ctf "$OBJ"/mach.development.bcm2837.unstripped \
  "$OBJ"/mach.development.bcm2837.unstripped.noctf "$OBJ"/mach.development.bcm2837.dSYM
K27_VARIANT=native ./build-k27-piano.sh > "$D/out/k273/kernel-relink.log" 2>&1 || { tail -20 "$D/out/k273/kernel-relink.log"; exit 1; }
cp "$OBJ/mach.development.bcm2837.unstripped.noctf" "out/k27/kernel-k273-$TAG-native.macho"
python3 k27/kc27-layout.py "$KC" "out/k27/kernel-k273-$TAG-native.macho" --natives "$D/tooling/k27/kext" | tail -1
echo "$T/out/k27/kernel-k273-$TAG-native.macho"
