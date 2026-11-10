#!/bin/bash
# AVB-sign the two boot_a candidates of the persistent boot (docs/persistent-boot.md) on the signing host, exactly as the installed
# Ubuntu boot_a is signed (tooling/bootimg/sign-on-remote.sh: partition "boot", 100663296 bytes, SHA256_RSA4096, rollback
# index and properties of boot-profile.json; the private key never leaves that host), then check them offline and assemble
# out/persistent/macos-set/ (tooling/persist/build-boot.py --verify). Needs ssh to the signing host; no password is asked here.
#   tooling/persist/sign.sh [OUT_DIR]          default out/persistent
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
O=${1:-out/persistent}
for n in boot-piano-macos-persistent-v4 boot-piano-esp-test-v2; do
  [ -f "$O/$n.img" ] || { echo "$O/$n.img is missing: run tooling/persist/build-all.sh" >&2; exit 1; }
done
tooling/bootimg/sign-on-remote.sh "$O/boot-piano-macos-persistent-v4.img" "$O/boot-piano-esp-test-v2.img"
python3 tooling/persist/build-boot.py --verify --out "$O"
(cd "$O" && shasum -a 256 boot-piano-*-signed.img macos-set/*.img 2>/dev/null)
