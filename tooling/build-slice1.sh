#!/bin/bash
# Slice 1, one command: both pianoPkg FDs (A: piano-uefi's bundled DXEs, B: this tablet's own OS3.0.303 DXEs), the k27 native
# kernel collection, the piano ADT, the two RAM-boot images per FD, offline verification; SIGN=1 also AVB-signs them on
# the private build host (sign-on-remote.sh). Inputs are the pinned checkouts of unixnu.lock and the files of out/inputs/
# (ARTIFACTS.md). Nothing is flashed or sent to the tablet.
#   tooling/build-slice1.sh            environment: SIGN=1, UTC (droidvm-utc of the macOS image, default 1794312000),
#                                      UNIQUE_CHIP_ID (default: the VM's made-up platform seed), K27_KERNEL
set -euo pipefail
D=$(cd "$(dirname "$0")/.." && pwd); cd "$D"
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
M=$R/Mu-Silicium-piano
tooling/firmware/setup-mu.sh
K27=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
KERNEL=${K27_KERNEL:-$K27/out/k27/kernel-k27-0054-native.macho}
UTC=${UTC:-1794312000}
SEED=${UNIQUE_CHIP_ID:-47554e5941480001}
mkdir -p out/fd out/k27 out/adt out/ram logs/fd logs/bootimg
fd() {   # $1 variant name, $2 Binaries/piano directory
  ln -sfn "$2" "$M/Binaries/piano"
  tooling/unixnu.sh build firmware --skip-preflight -j "${JOBS:-10}" > "logs/fd/build-$1.log" 2>&1 || { tail -20 "logs/fd/build-$1.log"; exit 1; }
  cp "$M/Build/pianoPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd" "out/fd/piano-uefi-$1.fd"
  grep -E '^\+ build ' "logs/fd/build-$1.log" | tail -1 > "logs/fd/command-$1.txt"
}
fd A-bundled "$R/piano-uefi-UniXNU-piano/uefi/Binaries/piano"
[ -d "$R/piano-binaries-own303/QcomPkg" ] || tooling/firmware/make-own-binaries.sh "$D/../droidvm-build/piano-backup-20261110/sde/1-uefi_a.img.zst" "$R/piano-binaries-own303"
fd B-own303 "$R/piano-binaries-own303"
ln -sfn "$R/piano-uefi-UniXNU-piano/uefi/Binaries/piano" "$M/Binaries/piano"
tooling/kc/build-kc-k27.sh "$KERNEL" out/k27/kc-k27-native.bin
python3 tooling/adt/make-piano-adt.py out/adt/piano-k27.adt --unique-chip-id "$SEED" --data --trust
for v in A-bundled B-own303; do
  python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-$v.fd --fd-name $v --out out/ram --kc out/k27/kc-k27-native.bin \
    --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt --utc "$UTC"
  python3 tooling/bootimg/verify-ram-images.py out/ram/build-$v.json > logs/bootimg/verify-$v-unsigned.log 2>&1 || { tail -5 logs/bootimg/verify-$v-unsigned.log; exit 1; }
done
if [ "${SIGN:-0}" = 1 ]; then
  tooling/bootimg/sign-on-remote.sh out/ram/boot-piano-{uefi,macos}-ram-{A-bundled,B-own303}.img
  AVB=$D/../v1.1.0/upstream/xiaomipad-8pro-mainline-1.1.0/tools/local/aosp-avb/avbtool.py
  for v in A-bundled B-own303; do
    python3 tooling/bootimg/verify-ram-images.py out/ram/build-$v.json out/ram/boot-piano-{uefi,macos}-ram-$v-signed.img \
      --avbtool "$AVB" --pubkey out/keys/piano-boot-signing.pub.pem > logs/bootimg/verify-$v-signed.log 2>&1 || { tail -5 logs/bootimg/verify-$v-signed.log; exit 1; }
  done
fi
(cd out && shasum -a 256 fd/*.fd k27/kc-k27-native.bin adt/piano-k27.adt ram/*.img keys/* 2>/dev/null) > logs/SHA256SUMS
cat logs/SHA256SUMS
