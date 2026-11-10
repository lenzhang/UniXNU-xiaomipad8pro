#!/bin/bash
# FD variant B ("own303"): the Qualcomm DXEs of this tablet's own uefi_a (OS3.0.303, BOOT.MXF.2.5.1-00265), extracted and
# patched with piano-uefi's own tools (extract-qcom-dxe.py, patch-qcom-dxe.sh), instead of the set piano-uefi bundles
# (another firmware build: 13 of 104 drivers differ). The proprietary binaries stay outside every repository.
#   make-own-binaries.sh UEFI_A_IMG_ZST OUTDIR
# UFSDxe: piano-uefi's link-reset patch is written for fixed offsets of its own build and refuses ours (its own check); our
# UFSDxe stays unpatched, so UEFI may not bring the UFS link up. The RAM payload does not need UFS in UEFI.
set -euo pipefail
[ $# = 2 ] || { echo "usage: $0 UEFI_A_IMG_ZST OUTDIR" >&2; exit 2; }
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
PU=$R/piano-uefi-UniXNU-piano
DODGE=$R/Mu-Silicium-piano/Binaries/dodge
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
W=$(mktemp -d /tmp/piano-own-binaries.XXXXXX)
trap 'rm -rf "$W"' EXIT
zstd -dq "$1" -o "$W/uefi_a.img"
python3 -m venv "$W/venv" >/dev/null && "$W/venv/bin/pip" -q install uefi_firmware >/dev/null 2>&1
mkdir -p "$W/root/deps/Mu-Silicium/Binaries" && ln -s "$PU/tools" "$W/root/tools" && ln -s "$DODGE" "$W/root/deps/Mu-Silicium/Binaries/dodge"
"$W/venv/bin/python" "$PU/tools/extract-qcom-dxe.py" "$W/uefi_a.img" "$W/root/deps/Mu-Silicium/Binaries/piano"
(cd "$W/root" && PATH="$W/venv/bin:$PATH" bash tools/patch-qcom-dxe.sh) || echo "patch-qcom-dxe.sh stopped (expected at UFSDxe for this build)"
rm -rf "$OUT" && cp -R "$W/root/deps/Mu-Silicium/Binaries/piano" "$OUT"
shasum -a 256 "$W/uefi_a.img" | sed 's|  .*|  uefi_a.img|' > "$OUT/SOURCE.sha256"
grep -l 'patched.efi' "$OUT"/QcomPkg/Drivers/*/*.inf | xargs -n1 basename | sed 's/\.inf$//' | tr '\n' ' '; echo
