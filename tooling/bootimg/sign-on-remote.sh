#!/bin/bash
# AVB-sign RAM images the way the installed Ubuntu set is signed (xiaomipad-8pro-mainline build-persistent-boot.py: avbtool
# add_hash_footer, SHA256_RSA4096, partition "boot", rollback index and properties of boot-profile.json), with the key that
# signed this tablet's boot_a (AVB public key sha1 f5151930dc69791637b2a48505cb175f5f78a522). The private key never leaves
# the private signing host: the unsigned image goes there, the signed one and the public key come back, the copies there are removed.
#   sign-on-remote.sh IMG...        -> IMG-signed.img next to each, plus out/keys/piano-boot-signing.{avbpubkey,pub.pem} (public halves)
# Environment: SIGN_HOST (required, user@build-host), SIGN_KEY (path on that host),
# AVBTOOL (path to the pinned avbtool.py).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
H=${SIGN_HOST:?set SIGN_HOST to user@build-host}
K=${SIGN_KEY:?set SIGN_KEY to the signing-key path on SIGN_HOST}
AVB=${AVBTOOL:?set AVBTOOL to the pinned avbtool.py}
[ "$(shasum -a 256 "$AVB" | cut -c1-64)" = e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b ] || { echo "not the pinned avbtool 1.3.0" >&2; exit 1; }
PROFILE=${PIANO_V110:-$D/../v1.1.0}/release/boot/boot-profile.json
RI=$(python3 -c "import json;print(json.load(open('$PROFILE'))['boot']['rollback_index'])")
PROPS=$(python3 -c "import json,shlex;print(' '.join('--prop '+shlex.quote(k+':'+v) for k,v in json.load(open('$PROFILE'))['boot']['avb_properties'].items()))")
R=/tmp/piano-sign.$$
ssh -o BatchMode=yes "$H" "mkdir -p $R"
trap 'ssh -o BatchMode=yes "$H" "rm -rf $R"' EXIT
scp -q "$AVB" "$H:$R/avbtool.py"
mkdir -p "$D/out/keys"
ssh -o BatchMode=yes "$H" "python3 $R/avbtool.py extract_public_key --key $K --output $R/pub.avbpubkey && openssl rsa -in $K -pubout -out $R/pub.pem 2>/dev/null"
scp -q "$H:$R/pub.avbpubkey" "$D/out/keys/piano-boot-signing.avbpubkey"
scp -q "$H:$R/pub.pem" "$D/out/keys/piano-boot-signing.pub.pem"
for img in "$@"; do
  n=$(basename "$img" .img)
  bytes=$(stat -f %z "$img")
  # The boot partition's 96 MiB when the image fits (the Ubuntu set's capacity), else just above the image: RAM boot only.
  part=100663296; [ "$bytes" -gt $((part - 1048576)) ] && part=$(( (bytes + 2097152 + 4095) / 4096 * 4096 ))
  # verify_image finds the partition's image as <partition_name>.img next to it: one directory per image.
  ssh -o BatchMode=yes "$H" "mkdir -p $R/$n"
  scp -q "$img" "$H:$R/$n/boot.img"
  ssh -o BatchMode=yes "$H" "python3 $R/avbtool.py add_hash_footer --image $R/$n/boot.img --partition_name boot --partition_size $part \
    --algorithm SHA256_RSA4096 --key $K --rollback_index $RI $PROPS && python3 $R/avbtool.py verify_image --image $R/$n/boot.img --key $R/pub.pem >/dev/null"
  scp -q "$H:$R/$n/boot.img" "$(dirname "$img")/$n-signed.img"
  echo "$n-signed.img partition_size $part sha256 $(shasum -a 256 "$(dirname "$img")/$n-signed.img" | cut -c1-64)"
done
