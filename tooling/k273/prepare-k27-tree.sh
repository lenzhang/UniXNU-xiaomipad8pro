#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The 27.3 line's k27 tree: an APFS clone of PIANO_K27_TREE (never changed itself) with patches/k273-kc applied, the segment
# addresses of 27.3's mac17j collection (k27/kc27-layout.py --write) and the native kernel relinked at them.
#
#   prepare-k27-tree.sh [KERNEL_TAG]     -> out/k273/k27-tree, its out/k27/kernel-k273-<TAG>-native.macho (TAG default 0065)
#
# The kernel is the k27 tree's source (xnu-12377 + 0001-0052 + patches/k27 0053-...) with patches/k273 on top (the 27.3 line's
# kernel series: patches/k273/README.md), linked at the 27.3 collection's addresses; the kernel file is named after the k27 tag and
# the k273 series' last patch (kernel-k273-0065-s0001-native.macho). The clone's first build recompiles everything once (its objects'
# recipes name the other tree's paths), a few minutes. patches/k27 patches past the configured tree's series (K27_TREE_TOP, 0066)
# are applied to the clone too, after patches/k273, and then name the kernel (0067: kernel-k273-0067-s0001-native.macho).
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
# patches/k273 on the clone's xnu source, each committed there so a rerun sees it applied
SERIES=
for p in "$D"/patches/k273/*.patch; do
  [ -e "$p" ] || continue
  n=$(basename "$p" .patch)
  # (grep without -q reads the whole log: -q's early exit gave git log a SIGPIPE, which pipefail took for "not committed")
  if git -C "$T/out/k27/xnu" log --format=%s | grep -xF "k273 $n" > /dev/null; then echo "$n: applied already"
  elif git -C "$T/out/k27/xnu" apply --reverse --check "$p" 2> /dev/null; then echo "$n: in the source already (not committed)"
  else git -C "$T/out/k27/xnu" apply "$p" || { echo "$n does not apply to $T/out/k27/xnu" >&2; exit 1; }
    git -C "$T/out/k27/xnu" add -A && git -C "$T/out/k27/xnu" commit -q -m "k273 $n" && echo "applied $n"; fi
  SERIES=-s${n%%-*}
done
# patches/k27 newer than the configured k27 tree carries (K27_TREE_TOP, default 0066: its series ends there), each committed in the
# clone as "k27 NAME" (a source that already has it, committed or not, is left as it is): a new patches/k27 patch reaches the 27.3
# kernel without the configured tree being changed. The kernel is then named after the newest (kernel-k273-0067-s0001-native.macho);
# the layout below still measures the k27 kernel of TAG.
KTAG=$TAG
for p in "$D"/patches/k27/0*.patch; do
  [ -e "$p" ] || continue
  n=$(basename "$p" .patch); num=${n%%-*}
  [ "$((10#$num))" -gt "$((10#${K27_TREE_TOP:-0066}))" ] || continue
  if git -C "$T/out/k27/xnu" log --format=%s | grep -xF "k27 $n" > /dev/null; then echo "$num: applied already"
  elif git -C "$T/out/k27/xnu" apply --reverse --check "$p" 2> /dev/null; then echo "$num: in the source already (not committed)"
  else git -C "$T/out/k27/xnu" apply "$p" || { echo "$n does not apply to $T/out/k27/xnu: no kernel without it" >&2; exit 1; }
    git -C "$T/out/k27/xnu" add -A && git -C "$T/out/k27/xnu" commit -q -m "k27 $n" && echo "applied $num"; fi
  KTAG=$num
done
cd "$T"
python3 k27/kc27-layout.py "$KC" "out/k27/kernel-k27-$TAG-native.macho" --natives "$D/tooling/k27/kext" --write k27/segaddrs.sh
OBJ=out/x12377/build/xnu/k27-native-obj/DEVELOPMENT_ARM64_BCM2837
rm -rf "$OBJ"/mach.development.bcm2837 "$OBJ"/mach.development.bcm2837.ctf "$OBJ"/mach.development.bcm2837.unstripped \
  "$OBJ"/mach.development.bcm2837.unstripped.noctf "$OBJ"/mach.development.bcm2837.dSYM
K27_VARIANT=native ./build-k27-piano.sh > "$D/out/k273/kernel-relink.log" 2>&1 || { tail -20 "$D/out/k273/kernel-relink.log"; exit 1; }
cp "$OBJ/mach.development.bcm2837.unstripped.noctf" "out/k27/kernel-k273-$KTAG$SERIES-native.macho"
python3 k27/kc27-layout.py "$KC" "out/k27/kernel-k273-$KTAG$SERIES-native.macho" --natives "$D/tooling/k27/kext" | tail -1
echo "$T/out/k27/kernel-k273-$KTAG$SERIES-native.macho"
