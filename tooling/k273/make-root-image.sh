#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The 27.3 line's root image for linux_data (docs/k273-line.md), and the seed and trust cache its RAM images take:
#
#   OUT/root.img                 SHNGROOT manifest (tooling/native/make-root-manifest.py, as linux_rootfs's) at byte 0, then the
#                                windows OS (27.3's System volume container, the IPSW's 141-22233-021.dmg decrypted, unchanged)
#                                and SystemOS (tooling/k273/make-systemos.sh), 1 MiB aligned; written from the first block of
#                                linux_data, nothing else of the partition changes
#   OUT/root.json                volumes, windows, sizes, sha256s and the write plan (partition, LBAs)
#   OUT/kernel.macho.trustcache  tooling/k273/make-trust.py: the 27.0.1 bundle's own modules + 27.3's static, System, App
#   OUT/scratch-data.img         the Data seed (default: the 27.0.1 line's out/inputs/scratch-data.img, the installed system's
#                                Data as it was before the update; K273_SEED names another)
#
#   make-root-image.sh [OUT]      (default out/inputs/k273/root; inputs in out/inputs/k273/26C5035r)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
I=${K273_INPUTS:-$D/out/inputs/k273}
O=${1:-$I/root}
M=$I/26C5035r
OS=$M/141-22233-021.dmg
SYS=$O/systemos.img
for f in "$OS" "$SYS" "$SYS.json" "$M/141-22233-021.dmg.aea.trustcache" "$M/141-22145-021.dmg.aea.trustcache" "$M/141-22106-021.dmg.trustcache"; do
  [ -f "$f" ] || { echo "missing $f (tooling/k273/ipsw-members.py, decrypt-aea.py, make-systemos.sh)" >&2; exit 1; }
done
# The OS image as the profile pins it (UniXNU macos-27.3-26C5035r [root] os_image_sha256).
OS_SHA=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["sha256"])' "$OS.json")
[ "$OS_SHA" = 40d8282eb8650da9b4d7437a981764c48d47ee4bdc5b8b362248f25ecbce0e70 ] || { echo "$OS: sha256 $OS_SHA is not the profile's" >&2; exit 1; }
mkdir -p "$O/manifest"
# An image assembled from the same windows (the manifest's layout names their sha256s) is kept: a later run only redoes the rest.
if [ -f "$O/root.img.sha256" ] && [ -f "$O/manifest/layout.json" ] && [ "$O/root.img.sha256" -nt "$SYS" ] && [ "$O/root.img.sha256" -nt "$OS" ] \
   && cmp -s <(dd if="$O/root.img" bs=4096 count=1 2> /dev/null) "$O/manifest/manifest.bin"; then
  IMAGE_BYTES=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["image_bytes"])' "$O/manifest/layout.json")
  ROOT_SHA=$(cat "$O/root.img.sha256")
  echo "root.img: kept (sha256 $ROOT_SHA)"
else
python3 tooling/native/make-root-manifest.py "$O/manifest" "OS=$OS" "SystemOS=$SYS"
IMAGE_BYTES=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["image_bytes"])' "$O/manifest/layout.json")
rm -f "$O/root.img" "$O/root.img.sha256"
mkfile -n "$IMAGE_BYTES" "$O/root.img"
dd if="$O/manifest/manifest.bin" of="$O/root.img" bs=4096 count=1 conv=notrunc 2> /dev/null
python3 - "$O/manifest/layout.json" "$O/root.img" <<'EOF'
import json, sys
layout, image = json.load(open(sys.argv[1])), sys.argv[2]
with open(image, 'r+b') as target:
    for w in layout['windows']:
        target.seek(w['offset'])
        with open(w['image'], 'rb') as source:
            while chunk := source.read(64 << 20):
                target.write(chunk)
        print(f"{w['name']}: {w['size']} bytes at {w['offset']:#x}")
EOF
ROOT_SHA=$(shasum -a 256 "$O/root.img" | cut -d' ' -f1)
echo "$ROOT_SHA" > "$O/root.img.sha256"
fi
python3 tooling/k273/make-trust.py out/inputs/kernel.macho.trustcache "$M/141-22233-021.dmg.aea.trustcache" \
  "$M/141-22145-021.dmg.aea.trustcache" "$M/141-22106-021.dmg.trustcache" "$O/kernel.macho.trustcache"
cp "${K273_SEED:-out/inputs/scratch-data.img}" "$O/scratch-data.img"
python3 - "$O" "$IMAGE_BYTES" "$ROOT_SHA" <<'EOF'
import hashlib, json, sys
from pathlib import Path
o, size, sha = Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
layout = json.loads((o / 'manifest/layout.json').read_text())
sysos = json.loads((o / 'systemos.img.json').read_text())
# linux_data on this unit's LUN 0 (4096-byte LBAs): tooling/persist/common.py DATA_FIRST, its size from the GPT (test-persist.py: sda36
# 574316544+422100952 512-byte sectors).
first, blocks = 71789568, 422100952 // 8
need = (size + 4095) // 4096
assert need <= blocks
record = {
    'os_volume_uuid': '84581E99-85BE-4957-AC53-2947E887B3E4', 'systemos_volume_uuid': sysos['volume_uuid'],
    'image_bytes': size, 'image_sha256': sha, 'manifest_sha256': hashlib.sha256((o / 'manifest/manifest.bin').read_bytes()).hexdigest(),
    'windows': [{k: w[k] for k in ('name', 'offset', 'size', 'sha256')} for w in layout['windows']],
    'write': {'partition': 'linux_data', 'lun': 0, 'lba_bytes': 4096, 'partition_first_lba': first, 'partition_blocks': blocks,
              'first_lba': first, 'last_lba': first + need - 1, 'blocks': need,
              'untouched': f'linux_data blocks {need}..{blocks - 1} (the rest of the partition, the later Data store window at its end)'},
    'seed_sha256': hashlib.sha256((o / 'scratch-data.img').read_bytes()).hexdigest(),
    'trust_sha256': hashlib.sha256((o / 'kernel.macho.trustcache').read_bytes()).hexdigest(),
}
(o / 'root.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
EOF
