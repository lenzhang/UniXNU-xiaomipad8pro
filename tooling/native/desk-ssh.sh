#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# ssh into the tablet over the USB network (piano-usb-net=1: the desk-dev image, tooling/k27/desk-dev.sh; docs/drivers/usb-device-debug.md
# section 14.8), on the Mac. No root needed.
#
#   tooling/native/desk-ssh.sh [-a ADDRESS] [-u ACCOUNT] [-n] [COMMAND ...]
#
# ADDRESS  the tablet's: default 169.254.80.2 (desk-dev.sh's fixed link-local; its console line "desk-dev: usb net: en1 inet ..."
#          says which it took: a DHCP lease with Internet Sharing, or fe80::...: the interface is added as its scope)
# ACCOUNT  the tablet's account: DESK_SSH_USER, else this Mac's user name
# -n       print the ssh command instead of running it (DESK_SSH_MAC=<another interface's MAC> with it: a test without the tablet)
# The key is out/desk-dev/id_ed25519 (made once by tooling/k27/build-desktop-images.sh desk-dev, the public half in the image). The
# Mac's end of the link is the interface with MAC 02:50:49:41:4e:03 (usb-console.py --list names it); for a 169.254 address ssh is
# bound to it (-B): macOS routes 169.254/16 to its primary interface (Wi-Fi), not to the cable, and a socket bound to the cable's
# address takes the route scoped to it. The tablet's host keys are new each boot (its Data is volatile), so none is kept or checked:
# the link is the cable to a development image, never a network.
set -eu
D=$(cd "$(dirname "$0")/../.." && pwd)
addr=169.254.80.2 user=${DESK_SSH_USER:-$(id -un)} dry=
while getopts a:u:n o; do
  case $o in a) addr=$OPTARG ;; u) user=$OPTARG ;; n) dry=1 ;; *) sed -n '6,7p' "$0" >&2; exit 2 ;; esac
done
shift $((OPTIND - 1))
key=$D/out/desk-dev/id_ed25519
[ -f "$key" ] || { echo "desk-ssh: no $key (tooling/k27/build-desktop-images.sh desk-dev makes it)" >&2; exit 1; }
ether=${DESK_SSH_MAC:-02:50:49:41:4e:03} mac=
for i in $(ifconfig -l); do
  ifconfig "$i" 2> /dev/null | grep -q "ether $ether" && { mac=$i; break; }
done
[ -n "$mac" ] || { echo "desk-ssh: no interface with MAC $ether: the tablet's USB network is not up (piano-usb-net=1? usb-console.py --list)" >&2; exit 1; }
bind=
case $addr in
  169.254.*)
    ifconfig "$mac" | grep -q 'inet 169\.254\.' ||
      { echo "desk-ssh: $mac has no 169.254 address (leave it at \"Using DHCP\": without a server it self-assigns one in seconds)" >&2; exit 1; }
    bind=$mac ;;
  fe80:*%*) ;;
  fe80:*) addr=$addr%$mac ;;
esac
# ssh [options] destination [command]: "$@" is the command; the destination, then the options, go in front of it.
set -- "$user@$addr" "$@"
set -- -i "$key" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
  -o ConnectTimeout=10 "$@"
[ -z "$bind" ] || set -- -B "$bind" "$@"
if [ -n "$dry" ]; then echo ssh "$@"; exit 0; fi
exec ssh "$@"
