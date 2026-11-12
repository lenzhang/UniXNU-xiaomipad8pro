#!/bin/bash
# AVB-sign the boot_a images of the persistent boot (docs/persistent-boot.md) on the signing host, exactly as the installed
# Ubuntu boot_a is signed (tooling/bootimg/sign-on-remote.sh: partition "boot", 100663296 bytes, SHA256_RSA4096, rollback
# index and properties of boot-profile.json; the private key never leaves that host), then check them offline and assemble
# out/persistent/macos-set*/ (tooling/persist/build-boot.py --verify). Needs ssh to the signing host; no password is asked here.
#   tooling/persist/sign.sh [OUT_DIR] [SERIES]     default out/persistent, series 4 (the v4 image and its v2 test twin);
#                                                  series 5 and later: boot-piano-macos-persistent-vN.img only (build-boot.py --series)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
O=${1:-out/persistent}
S=${2:-4}
if [ "$S" = 4 ]; then NAMES="boot-piano-macos-persistent-v4 boot-piano-esp-test-v2"; else NAMES="boot-piano-macos-persistent-v$S"; fi
IMAGES=()
for n in $NAMES; do
  [ -f "$O/$n.img" ] || { echo "$O/$n.img is missing: run tooling/persist/build-boot.py --series $S (or build-all.sh)" >&2; exit 1; }
  IMAGES+=("$O/$n.img")
done
tooling/bootimg/sign-on-remote.sh "${IMAGES[@]}"
python3 tooling/persist/build-boot.py --verify --series "$S" --out "$O"
(cd "$O" && for n in $NAMES; do shasum -a 256 "$n-signed.img"; done)
