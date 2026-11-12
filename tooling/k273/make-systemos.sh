#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The 27.3 line's SystemOS window: the volume the k27 kernel union-mounts read-only over / (its owner chain, /chosen
# owner-systemos-uuid), made on this Mac with sudo from the 27.3 IPSW's images. The same content as the 27.0.1 line's (the k27
# tree's scripts/make-macos-systemos.sh, which made it on a Linux host with linux-apfs): the SystemOS cryptex's files, the App
# cryptex under /System/Volumes/Preboot/Cryptexes/App, the read-only firmlink targets of the OS volume's Data template (assets,
# CoreTypes, Speech, cups, snmp) and its bulky read-only /Library parts under /System/Library/DroidVMData (the Data seed links
# there), empty directories for the writable firmlinks and the Data mount point, cryptexd's state directories, the k27 tree's
# boot shims (overlay/systemos-shims: fsck, mount; /sbin/mount stays reachable as mount.real), /var /etc /tmp links.
# Files keep their owners, modes, flags and APFS compression (ditto --hfsCompression as root, ownership enabled on every mount).
#
#   sudo is used for: newfs_apfs, diskutil enableOwnership, ditto, mkdir/ln/install inside the new volume. Nothing outside OUT is
#   written; the inputs are attached read-only.
#
#   make-systemos.sh OS.dmg SYSTEMOS_CRYPTEX.dmg APP_CRYPTEX.dmg OUT.img [SIZE_MIB]     (the raw APFS containers of the IPSW)
#
# Output: OUT.img (a raw APFS container, the window as it is written) and OUT.img.json {volume_uuid, size, sha256}.
set -euo pipefail
OS=$1 CX=$2 APP=$3 OUT=$4 SIZE_MIB=${5:-6144}
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"
SHIMS=$PIANO_K27_TREE/overlay/systemos-shims
[ -d "$SHIMS/sbin" ] || { echo "no $SHIMS/sbin (the k27 tree's boot shims)" >&2; exit 1; }
[ -e "$OUT" ] && { echo "$OUT exists" >&2; exit 1; }
WORK=$(mktemp -d /tmp/k273-systemos.XXXXXX)
DISKS=()
cleanup() {
  for m in "$WORK"/mnt "$WORK"/os "$WORK"/cx "$WORK"/app; do
    mount | grep -q " on $(cd "$m" 2>/dev/null && pwd -P) " && diskutil unmount force "$m" > /dev/null 2>&1 || true
  done
  for d in ${DISKS[@]+"${DISKS[@]}"}; do hdiutil detach -force "$d" > /dev/null 2>&1 || true; done
  rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$WORK"/mnt "$WORK"/os "$WORK"/cx "$WORK"/app

DISKOF="python3 $D/tooling/k273/disk-of.py"
attach() {   # attach IMAGE [-readonly]: sets WHOLE to the whole disk hdiutil made for IMAGE (checked by its image path)
  hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -nobrowse "${@:2}" "$1" > /dev/null
  WHOLE=$($DISKOF whole "$1")
  DISKS+=("$WHOLE")
}
mount_ro() {  # mount_ro IMAGE DIR: the image's volume read-only with ownership
  local volume
  attach "$1" -readonly
  volume=$($DISKOF volume "$WHOLE")
  diskutil mount readOnly -mountPoint "$2" "$volume" > /dev/null
  sudo diskutil enableOwnership "$2" > /dev/null
}

mount_ro "$OS" "$WORK/os"
mount_ro "$CX" "$WORK/cx"
mount_ro "$APP" "$WORK/app"
TEMPLATE=$WORK/os/System/Library/Templates/Data
[ -d "$TEMPLATE/private/var" ] || { echo "$OS: no Data template" >&2; exit 1; }

mkfile -n "${SIZE_MIB}m" "$OUT"
attach "$OUT"
# newfs_apfs only on the disk hdiutil attached OUT as, a disk image of exactly OUT's size
[ "$(diskutil info -plist "$WHOLE" | plutil -extract TotalSize raw -o - -)" = "$(stat -f %z "$OUT")" ] \
  && diskutil info "$WHOLE" | grep -q "Disk Image" || { echo "refusing newfs_apfs on $WHOLE: not $OUT's disk image" >&2; exit 1; }
sudo newfs_apfs -v SystemOS "$WHOLE" > /dev/null
sleep 2
VOLUME=$($DISKOF volume "$WHOLE")
diskutil mount -mountPoint "$WORK/mnt" "$VOLUME" > /dev/null
sudo diskutil enableOwnership "$WORK/mnt" > /dev/null
M=$WORK/mnt

sudo ditto --hfsCompression "$WORK/cx" "$M"
sudo mkdir -p "$M/System/Volumes/Preboot/Cryptexes/App"
sudo ln -s / "$M/System/Volumes/Preboot/Cryptexes/OS"
sudo ditto --hfsCompression "$WORK/app" "$M/System/Volumes/Preboot/Cryptexes/App"
for path in System/Library/Assets System/Library/AssetsV2 System/Library/PreinstalledAssets System/Library/PreinstalledAssetsV2 \
            System/Library/Speech System/Library/CoreServices/CoreTypes.bundle/Contents/Library usr/libexec/cups usr/share/snmp; do
  if [ -e "$TEMPLATE/$path" ]; then
    sudo mkdir -p "$M/$(dirname "$path")"
    sudo ditto --hfsCompression "$TEMPLATE/$path" "$M/$path"
  fi
done
for path in Library/Apple "Library/Application Support/Apple" Library/Documentation Library/WebServer; do
  if [ -e "$TEMPLATE/$path" ]; then
    sudo mkdir -p "$M/System/Library/DroidVMData/$(dirname "$path")"
    sudo ditto --hfsCompression "$TEMPLATE/$path" "$M/System/Library/DroidVMData/$path"
  fi
done
for path in Library Users Applications Volumes cores opt pkg usr/local System/Library/Caches System/Volumes/Hardware System/Volumes/Data; do
  sudo mkdir -p "$M/$path"
done
sudo mkdir -p "$M/System/Volumes/Preboot/com.apple.security.cryptexd/Cryptexes"
sudo mkdir -p "$M/sbin"
sudo install -m 755 -o root -g wheel "$WORK/os/sbin/mount" "$M/sbin/mount.real"
for f in "$SHIMS"/sbin/*; do sudo install -m 755 -o root -g wheel "$f" "$M/sbin/$(basename "$f")"; done
sudo mkdir -p "$M/private/var" "$M/private/etc" "$M/private/tmp"
sudo ln -s private/var "$M/var"
sudo ln -s private/etc "$M/etc"
sudo ln -s private/tmp "$M/tmp"
sudo du -sh "$M" | awk '{print "used " $1}'
UUID=$($DISKOF uuid "$VOLUME")
sync
diskutil unmount "$M" > /dev/null
hdiutil detach "$WHOLE" > /dev/null
SHA=$(shasum -a 256 "$OUT" | cut -d' ' -f1)
printf '{"volume_uuid": "%s", "size": %s, "sha256": "%s"}\n' "$UUID" "$(stat -f %z "$OUT")" "$SHA" > "$OUT.json"
cat "$OUT.json"
