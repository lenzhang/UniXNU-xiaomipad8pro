#!/bin/bash
# Everything, one command: the pianoPkg FD (A: piano-uefi's bundled DXEs; FD_B=1 also B, this tablet's own OS3.0.303 DXEs, which
# reset after 10-15 s), then tooling/k27/build-native-image.sh (native kexts, KC, ADT, the macOS RAM image + the loop's dump image) and
# the offline verification. SIGN=1 also AVB-signs the images on the private build host (sign-on-remote.sh). Inputs are the pinned
# checkouts of unixnu.lock and the files of out/inputs/ (ARTIFACTS.md). Nothing is flashed or sent to the tablet.
#   tooling/build-slice1.sh            environment: SIGN=1, FD_B=1 (needs PIANO_PARTITION_BACKUP), K27_KERNEL, PIANO_RESET_AFTER
#                                      (see build-native-image.sh); PIANO_K27_TREE (tooling/k27/k27-tree.sh)
set -euo pipefail
D=$(cd "$(dirname "$0")/.." && pwd); cd "$D"
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
M=$R/Mu-Silicium-piano
tooling/firmware/setup-mu.sh
mkdir -p out/fd out/k27 out/adt out/ram logs/fd logs/bootimg
fd() {   # $1 variant name, $2 Binaries/piano directory
  ln -sfn "$2" "$M/Binaries/piano"
  tooling/unixnu.sh build firmware --skip-preflight -j "${JOBS:-10}" > "logs/fd/build-$1.log" 2>&1 || { tail -20 "logs/fd/build-$1.log"; exit 1; }
  cp "$M/Build/pianoPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd" "out/fd/piano-uefi-$1.fd"
  grep -E '^\+ build ' "logs/fd/build-$1.log" | tail -1 > "logs/fd/command-$1.txt"
}
if [ "${FD_B:-0}" = 1 ]; then
  if [ ! -d "$R/piano-binaries-own303/QcomPkg" ]; then   # from the 2026-11-10 partition backup (PIANO_PARTITION_BACKUP)
    PB=$(python3 -c 'import sys; sys.path.insert(0, "tooling/persist"); import common; print(common.partition_backup())')
    tooling/firmware/make-own-binaries.sh "$PB/sde/1-uefi_a.img.zst" "$R/piano-binaries-own303"
  fi
  fd B-own303 "$R/piano-binaries-own303"
fi
fd A-bundled "$R/piano-uefi-UniXNU-piano/uefi/Binaries/piano"
tooling/k27/build-native-image.sh
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json > logs/bootimg/verify-A-bundled-unsigned.log 2>&1 || { tail -5 logs/bootimg/verify-A-bundled-unsigned.log; exit 1; }
if [ "${SIGN:-0}" = 1 ]; then
  tooling/bootimg/sign-on-remote.sh out/ram/boot-piano-{uefi,macos}-ram-A-bundled.img
  AVB=$D/../v1.1.0/upstream/xiaomipad-8pro-mainline-1.1.0/tools/local/aosp-avb/avbtool.py
  python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json out/ram/boot-piano-{uefi,macos}-ram-A-bundled-signed.img \
    --avbtool "$AVB" --pubkey out/keys/piano-boot-signing.pub.pem > logs/bootimg/verify-A-bundled-signed.log 2>&1 || { tail -5 logs/bootimg/verify-A-bundled-signed.log; exit 1; }
fi
(cd out && shasum -a 256 fd/*.fd k27/kc-k27-native.bin adt/piano-k27.adt ram/*.img keys/* 2>/dev/null) > logs/SHA256SUMS
cat logs/SHA256SUMS
