#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# FD A (out/fd/piano-uefi-A-bundled.fd) with every delta of firmware/patches/: tooling/firmware/setup-mu.sh, then
# tooling/unixnu.sh build firmware (DarwinBoot from out/deps/DarwinBoot with firmware/patches/darwinboot/), checked and stamped:
#   <fd>.ram-payload-first   its SHA-256: UniXNU.efi has firmware/patches/unixnu/0001 (a boot image's RAM payload wins over the ESP)
#   <fd>.ram-window          its SHA-256 and DarwinBoot's window bound: DarwinBoot.efi has firmware/patches/darwinboot/0001 (XNU's
#                            RAM window up to 13 GiB); tooling/bootimg/build-ram-images.py refuses an ADT asking for more than
#                            8 GiB with an FD without it (that DarwinBoot rejects the ADT: no boot)
# The previous FD is kept as <fd>.prev (with its stamps): the rollback when the new one does not boot.
#   tooling/firmware/build-fd.sh          JOBS (default 10); the log is out/fd/build-A-bundled.log
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
M=$R/Mu-Silicium-piano
B=$M/Build/pianoPkg/DEBUG_CLANGPDB
FD_FILE=out/fd/piano-uefi-A-bundled.fd
mkdir -p out/fd
tooling/firmware/setup-mu.sh
ln -sfn "$R/piano-uefi-UniXNU-piano/uefi/Binaries/piano" "$M/Binaries/piano"
tooling/unixnu.sh build firmware --skip-preflight -j "${JOBS:-10}" > out/fd/build-A-bundled.log 2>&1 || { tail -20 out/fd/build-A-bundled.log; exit 1; }
strings -a "$B/AARCH64/UniXNU.efi" | grep -q 'config: volume %s' \
  || { echo "UniXNU.efi lacks firmware/patches/unixnu/0001 (config: volume)" >&2; exit 1; }
python3 - "$B/AARCH64/DarwinBoot.efi" <<'EOF' || { echo "DarwinBoot.efi lacks firmware/patches/darwinboot/0001" >&2; exit 1; }
import sys
sys.exit(0 if 'taken (largest run)'.encode('utf-16-le') in open(sys.argv[1], 'rb').read() else 1)
EOF
if [ -f "$FD_FILE" ] && ! cmp -s "$FD_FILE" "$B/FV/SILICIUM_UEFI.fd"; then
  cp "$FD_FILE" "$FD_FILE.prev"
  for s in ram-payload-first ram-window; do
    if [ -f "$FD_FILE.$s" ]; then cp "$FD_FILE.$s" "$FD_FILE.prev.$s"; else rm -f "$FD_FILE.prev.$s"; fi
  done
fi
cp "$B/FV/SILICIUM_UEFI.fd" "$FD_FILE"
SUM=$(shasum -a 256 "$FD_FILE" | cut -c1-64)
echo "$SUM" > "$FD_FILE.ram-payload-first"
echo "$SUM 0x340000000" > "$FD_FILE.ram-window"
echo "$FD_FILE: sha256 $SUM (RAM payload first; RAM window up to 13 GiB)"
