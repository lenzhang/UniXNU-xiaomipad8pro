#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The 27.3 line's kernel collection, ADT and RAM images (docs/k273-line.md): tooling/k27/build-native-image.sh with the 27.3 line's
# inputs instead of the 27.0.1 line's. Same native kexts (configs/k27-native-kexts.txt, check-native-kexts.py), same k27 kernel
# (xnu-12377 + 0001-0065) relinked at the 27.3 collection's addresses, Apple's kexts from 27.3's kernelcache.release.mac17j, the
# Mac17,11 identity (configs/k273-adt.toml).
#
#   tooling/k273/build-k273-image.sh            -> out/k27/kc-k27-native.bin, out/adt/piano-k27.adt, out/ram, out/ram-dump
#
# Environment (defaults in this checkout's out/):
#   K273_TREE    the 27.3 line's k27 tree (tooling/k273/prepare-k27-tree.sh: a clone of PIANO_K27_TREE with patches/k273-kc and the
#                27.3 segment addresses, its kernel relinked): out/k273/k27-tree
#   K273_KERNEL  the relinked kernel (default the newest $K273_TREE/out/k27/kernel-k273-*-native.macho)
#   K273_INPUTS  the IPSW-derived inputs (tooling/k273/ipsw-members.py, decompress-kernelcache.py, make-*.sh): out/inputs/k273
#   K273_ROOT    k273 (default): the 27.3 root image on linux_data (native-ufs-root-part=linux_data), its seed and trust cache from
#                $K273_INPUTS/root; k27: the installed 27.0.1 root on linux_rootfs with its seed and trust cache (out/inputs), to
#                test the 27.3 collection and identity without the 27.3 root
# The rest is build-native-image.sh's (PIANO_GPU, PIANO_EXTRA_ARGS, PIANO_RESET_AFTER, ...). The Data disk is volatile
# (PIANO_DATA_PERSIST=0) unless set: no 27.3 image opens the owner's store.
# This writes the same out/ paths as the 27.0.1 line's builds: run it in its own checkout (worktree), or copy the results away
# (out/ram-NAME) before the next build of the other line.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
T=${K273_TREE:-$D/out/k273/k27-tree}
I=${K273_INPUTS:-$D/out/inputs/k273}
[ -f "$T/k27/kc-build273-piano.py" ] || { echo "no 27.3 k27 tree at $T: tooling/k273/prepare-k27-tree.sh" >&2; exit 1; }
[ -f "$I/kc/kc273-mac17j.bin" ] || { echo "no $I/kc/kc273-mac17j.bin: tooling/k273/decompress-kernelcache.py" >&2; exit 1; }
# Turnip's Mesa tree sits next to the configured k27 tree (tooling/metal/build-turnip-a830.sh's default), not next to the clone.
export TURNIP_K27_TREE=${TURNIP_K27_TREE:-$(. "$D/tooling/k27/k27-tree.sh"; echo "$PIANO_K27_TREE")}
export TURNIP_TREE=${TURNIP_TREE:-$(dirname "$TURNIP_K27_TREE")/UniXNU-turnip}
export PIANO_K27_TREE=$T
export K27_KERNEL=${K273_KERNEL:-$(ls -t "$T"/out/k27/kernel-k273-*-native.macho | head -1)}
export KC_BASE=$I/kc/kc273-mac17j.bin
export KC_BUILDER=k27/kc-build273-piano.py
export KC_COMPACT_LINKEDIT=${KC_COMPACT_LINKEDIT:-1}
export PIANO_ADT_IDENTITY=configs/k273-adt.toml
export PIANO_VMM_PRESENT=${PIANO_VMM_PRESENT:-0}
export PIANO_DATA_PERSIST=${PIANO_DATA_PERSIST:-0}
case ${K273_ROOT:-k273} in
  k273)
    R=$I/root
    for f in root.json scratch-data.img kernel.macho.trustcache; do
      [ -f "$R/$f" ] || { echo "no $R/$f: tooling/k273/make-root-image.sh (the root image, its seed and trust cache)" >&2; exit 1; }
    done
    export PIANO_ROOT_VOLUME_UUID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["os_volume_uuid"])' "$R/root.json")
    export PIANO_SYSTEMOS_UUID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["systemos_volume_uuid"])' "$R/root.json")
    export PIANO_BASE_SEED=$R/scratch-data.img PIANO_BASE_TRUST=$R/kernel.macho.trustcache
    export PIANO_EXTRA_ARGS="native-ufs-root-part=linux_data ${PIANO_EXTRA_ARGS:-}"
    ;;
  k27)
    # The installed 27.0.1 root's volumes (configs/k27-adt.toml).
    export PIANO_ROOT_VOLUME_UUID=C73696D4-CAC4-40A0-90CB-7D0463E4E3E6 PIANO_SYSTEMOS_UUID=8846FE78-2910-4297-AF97-D59BB6F8FC16
    ;;
  *) echo "K273_ROOT must be k273 or k27" >&2; exit 2 ;;
esac
echo "27.3 line: tree $T, kernel $(basename "$K27_KERNEL"), root ${K273_ROOT:-k273} (OS $PIANO_ROOT_VOLUME_UUID, SystemOS $PIANO_SYSTEMOS_UUID)" >&2
mkdir -p out/k27
exec bash tooling/k27/build-native-image.sh
