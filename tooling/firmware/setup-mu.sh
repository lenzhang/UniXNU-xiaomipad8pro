#!/bin/bash
# Make the UEFI build checkouts of unixnu.lock under UNIXNU_RESEARCH (default ~/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research),
# as piano-uefi's tools/setup-deps.sh does, plus this repository's deltas. Shallow and sparse; safe to rerun.
#   Mu-Silicium-piano/                 Mu-Silicium e51cd877 + Common/Mu, Mu_OEM_Sample, Silicium-ACPI, openssl (no Mu_Basecore:
#                                      UniXNU's edk2 feed is the same commit)
#   Mu-Silicium-piano/Binaries/        Device-Binaries a1501e0c, sparse: dodge/MdeModulePkg + the dodge QcomPkg references
#   piano-uefi-UniXNU-piano/           worktree of piano-uefi 31545f9 (branch unixnu) + firmware/patches/*.patch
#   links: Platforms/Xiaomi/pianoPkg, Resources/Configs/piano.toml, Binaries/piano, Resources/MuPatches/piano-mu-basecore.patch
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
pin() { python3 -c "import tomllib,sys;print(tomllib.load(open('$D/unixnu.lock','rb'))['pin']['$1']['$2'])"; }
M=$R/Mu-Silicium-piano
if [ ! -d "$M/.git" ]; then
  git init -q "$M" && git -C "$M" remote add origin "$(pin Mu-Silicium git)"
  git -C "$M" fetch -q --depth 1 origin "$(pin Mu-Silicium commit)" && git -C "$M" checkout -q FETCH_HEAD
fi
git -C "$M" submodule update -q --init --depth 1 -- Common/Mu Common/Mu_OEM_Sample Silicium-ACPI Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl
B=$M/Binaries
if [ ! -d "$B/.git" ]; then
  mkdir -p "$B" && git -C "$B" init -q && git -C "$B" remote add origin "$(pin Device-Binaries git)"
  git -C "$B" sparse-checkout set --no-cone /dodge/MdeModulePkg/ /dodge/QcomPkg/Drivers/SPMIDxe/ /dodge/QcomPkg/Drivers/ButtonsDxe/ \
    /dodge/QcomPkg/Drivers/UsbConfigDxe/ /dodge/QcomPkg/Drivers/UsbMsdDxe/ /dodge/QcomPkg/Drivers/PmicDxe/ /dodge/QcomPkg/Drivers/QcomChargerDxe/
  git -C "$B" fetch -q --depth 1 --filter=blob:none origin "$(pin Device-Binaries commit)" && git -C "$B" checkout -q FETCH_HEAD
fi
P=$R/piano-uefi-UniXNU-piano
if [ ! -d "$P" ]; then
  [ -d "$R/piano-uefi/.git" ] || git clone -q -b unixnu "$(pin piano-uefi git)" "$R/piano-uefi"
  git -C "$R/piano-uefi" worktree add -q --detach "$P" "$(pin piano-uefi commit)"
  for p in "$D"/firmware/patches/*.patch; do git -C "$P" apply "$p"; done
fi
for p in "$D"/firmware/patches/*.patch; do git -C "$P" apply --reverse --check "$p" 2>/dev/null || { echo "$P lacks $(basename "$p")" >&2; exit 1; }; done
mkdir -p "$M/Resources/MuPatches"
ln -sfn "$P/uefi/pianoPkg" "$M/Platforms/Xiaomi/pianoPkg"
ln -sfn "$P/uefi/piano.toml" "$M/Resources/Configs/piano.toml"
ln -sfn "$P/uefi/patches/mu-basecore.patch" "$M/Resources/MuPatches/piano-mu-basecore.patch"
[ -e "$B/piano" ] || ln -sfn "$P/uefi/Binaries/piano" "$B/piano"
[ -d "$R/uefi-sheng/BootShim" ] || { echo "need uefi-sheng $(pin uefi-sheng commit) for UniXNU's BootShim.S at $R/uefi-sheng" >&2; exit 1; }
echo "UEFI checkouts ready under $R"
