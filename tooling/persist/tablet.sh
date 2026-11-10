#!/bin/sh
# Tablet side of tooling/persist/maint-session.py, run in the Ubuntu release's maintenance RAM image (BusyBox sh, dd, sha256sum,
# wget, nc, od; Ubuntu's sgdisk, partx, blockdev). The host downloads it to /run/pp/tablet.sh (sha256 checked) and calls one
# subcommand per telnet line. Every subcommand prints key=value lines; nothing writes unless its name says so (copy, write, job).
# Devices are found from sysfs every time (LUN 0 = the disk on UFS target 0, partitions by PARTNAME), never assumed.
#   map                                   LUN 0 disk, block size, size; boot_a/boot_b/vendor_boot_a/init_boot_a; linux_rootfs/esp
#   hash DEV SKIP COUNT [BS]              sha256 of COUNT blocks of BS (default 4096) bytes at block SKIP
#   zero-sha COUNT [BS]                   sha256 of COUNT zero blocks (to tell an all-zero region)
#   peek DEV LBA OFFSET LENGTH            hex of LENGTH bytes at OFFSET in 4096-byte block LBA
#   send DEV SKIP COUNT BS PORT           stream the blocks to the host's TCP PORT (maint-session.py receives and checks)
#   fetch NAME SHA256 PORT                wget http://HOST:PORT/NAME to /run/pp/NAME and check its sha256
#   file-sha FILE                         sha256 of a file in /run/pp
#   copy DEV SKIP SEEK COUNT              WRITE: dd within DEV, 4096-byte blocks, forward (SEEK < SKIP is safe), conv=notrunc,fsync
#   write FILE DEV SEEK BS                WRITE: dd FILE onto DEV at block SEEK of BS bytes, conv=notrunc,fsync
#   restore NAME DEV SEEK BS PORT         WRITE: stream http://HOST:PORT/NAME (a verified backup) onto DEV at block SEEK
#   job NAME SUBCOMMAND...                run a subcommand detached (setsid): /run/pp/NAME.{log,rc}; survives the telnet session
#   job-status NAME                       rc=<exit status> when done, else running=1
#   drop-caches                           sync; echo 3 > /proc/sys/vm/drop_caches (read-backs then come from the medium)
#   rereadpt DEV                          blockdev --rereadpt, else partx -u
set -u
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
P=${PIANO_PP_DIR:-/run/pp}   # (PIANO_PP_DIR and PIANO_HOST_IP only for a host-side test of this script)
mkdir -p "$P"
host_ip() { [ -n "${PIANO_HOST_IP:-}" ] || . /etc/piano-network; echo "$PIANO_HOST_IP"; }
die() { echo "error=$*"; exit 1; }
lun0() {
	for b in /sys/block/sd*; do
		[ -e "$b" ] || continue
		case "$(readlink -f "$b/device")" in *1d84000*:0) echo "${b##*/}" ;; esac
	done
}
part_by_name() {   # print the node, start and size (512-byte units) of the partition named $1 (all disks)
	for d in /sys/class/block/sd*[0-9]; do
		[ -e "$d/partition" ] || continue
		n=$(sed -n 's/^PARTNAME=//p' "$d/uevent")
		[ "$n" = "$1" ] && echo "${d##*/} $(cat "$d/start") $(cat "$d/size")"
	done
}
cmd=${1:-}; [ $# -gt 0 ] && shift
case $cmd in
map)
	l=$(lun0)
	case "$l" in ''|*' '*) die "LUN 0 not found or ambiguous: '$l'" ;; esac
	echo "lun0=$l"
	echo "lun0_block_size=$(cat /sys/block/$l/queue/logical_block_size)"
	echo "lun0_bytes=$(blockdev --getsize64 /dev/$l)"
	echo "mounted=$(grep -c '^/dev/sd' /proc/mounts)"
	for n in boot_a boot_b vendor_boot_a init_boot_a linux_rootfs linux_data esp; do
		set -- $(part_by_name "$n")
		[ $# -eq 0 ] && { echo "$n=none"; continue; }
		[ $# -ne 3 ] && die "partition name $n is not unique"
		echo "$n=$1 $2 $3"
	done
	;;
hash)
	dd if="/dev/$1" bs="${4:-4096}" skip="$2" count="$3" 2>/dev/null | sha256sum | cut -d' ' -f1 | sed 's/^/sha256=/'
	;;
zero-sha)
	dd if=/dev/zero bs="${2:-4096}" count="$1" 2>/dev/null | sha256sum | cut -d' ' -f1 | sed 's/^/sha256=/'
	;;
peek)
	dd if="/dev/$1" bs=4096 skip="$2" count=1 2>/dev/null | od -A n -t x1 -v -j "$3" -N "$4" | tr -d ' \n' | sed 's/^/hex=/'
	echo
	;;
send)
	dd if="/dev/$1" bs="$4" skip="$2" count="$3" 2>/dev/null | nc "$(host_ip)" "$5"
	echo "sent=$?"
	;;
fetch)
	rm -f "$P/$1"
	wget -q -O "$P/$1" "http://$(host_ip):$3/$1" || die "wget $1"
	got=$(sha256sum "$P/$1" | cut -d' ' -f1)
	[ "$got" = "$2" ] || die "$1 sha256 $got, expected $2"
	echo "fetched=$1 sha256=$got bytes=$(wc -c < "$P/$1")"
	;;
file-sha)
	echo "sha256=$(sha256sum "$P/$1" | cut -d' ' -f1)"
	;;
copy)
	dd if="/dev/$1" of="/dev/$1" bs=4096 skip="$2" seek="$3" count="$4" conv=notrunc,fsync 2>"$P/dd.err" || { cat "$P/dd.err"; die "dd copy"; }
	tail -1 "$P/dd.err"; echo "copied=$4"
	;;
write)
	dd if="$P/$1" of="/dev/$2" bs="$4" seek="$3" conv=notrunc,fsync 2>"$P/dd.err" || { cat "$P/dd.err"; die "dd write"; }
	tail -1 "$P/dd.err"; echo "written=$1"
	;;
restore)
	wget -q -O - "http://$(host_ip):$5/$1" | dd of="/dev/$2" bs="$4" seek="$3" conv=notrunc,fsync 2>"$P/dd.err" || { cat "$P/dd.err"; die "restore $1"; }
	tail -1 "$P/dd.err"; echo "restored=$1"
	;;
job)
	name=$1; shift
	rm -f "$P/$name.rc" "$P/$name.log"
	setsid sh -c "sh $P/tablet.sh $*; echo \$? > $P/$name.rc" > "$P/$name.log" 2>&1 < /dev/null &
	echo "started=$name"
	;;
job-status)
	if [ -f "$P/$1.rc" ]; then echo "rc=$(cat "$P/$1.rc")"; cat "$P/$1.log"; else echo "running=1"; fi
	;;
drop-caches)
	sync; echo 3 > /proc/sys/vm/drop_caches; echo "dropped=1"
	;;
rereadpt)
	blockdev --rereadpt "/dev/$1" 2>/dev/null && echo "reread=blockdev" || { partx -u "/dev/$1" && echo "reread=partx"; }
	;;
*)
	die "usage: tablet.sh map|hash|zero-sha|peek|send|fetch|file-sha|copy|write|restore|job|job-status|drop-caches|rereadpt"
	;;
esac
