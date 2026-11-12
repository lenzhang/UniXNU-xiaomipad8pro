#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The desk-dev image's boot-job step (tooling/k27/build-desktop-images.sh desk-dev, docs/attended-desktop.md section 3): the ways in
# from the Mac besides typing into Terminal, each said on the kernel console (/dev/console, "desk-dev: " lines):
#   - the root shell over the cable: pianousb-shell (the userland stage's, piano-usb-shell=1): `tooling/native/usb-console.py --port 1`;
#     here only whether its job runs;
#   - the USB network (piano-usb-net=1, docs/drivers/usb-device-debug.md section 14): PianoUsbNet's interface (MAC 02:50:49:41:4e:02)
#     set to DHCP: with Internet Sharing on the Mac an address from it, else a self-assigned 169.254 one (the Mac's side, left at
#     "Using DHCP", self-assigns too); the address printed;
#   - sshd (Remote Login: the seed enables com.openssh.sshd, this makes sure it is loaded) with the key staged by the builder
#     (out/desk-dev/id_ed25519.pub, the private half only on the Mac) as authorized_keys of each account of uid 501 and up whose home
#     is in /Users, owned by it (sshd's StrictModes: the staged copy is uid 99's). Then: ssh -i out/desk-dev/id_ed25519 <account>@<address>.
# Every wait is bounded; nothing here touches the accounts' passwords.
#
#   sh desk-dev.sh            DESK_DEV_OUT (a file instead of /dev/console), DESK_DEV_KEYS (the staged key file) for a test
out=${DESK_DEV_OUT:-/dev/console}
keys=${DESK_DEV_KEYS:-/private/var/piano/dev/authorized_keys}
say() { printf 'desk-dev: %.150s\n' "$1" >> "$out" 2> /dev/null; }

# The key for each account (uid 501 and up, home in /Users).
if [ -s "$keys" ]; then
  for home in /Users/*; do
    [ -d "$home" ] || continue
    uid=$(stat -f %u "$home" 2> /dev/null) gid=$(stat -f %g "$home" 2> /dev/null)
    [ "${uid:-0}" -ge 501 ] 2> /dev/null && [ "$uid" != 99 ] || continue
    mkdir -p "$home/.ssh" && cp "$keys" "$home/.ssh/authorized_keys" && chown -R "$uid:$gid" "$home/.ssh" &&
      chmod 700 "$home/.ssh" && chmod 600 "$home/.ssh/authorized_keys" && say "ssh: the key installed for uid $uid" ||
      say "ssh: the key NOT installed for uid $uid"
  done
fi
# sshd: enabled in the seed's overrides; loaded here if launchd has not.
launchctl enable system/com.openssh.sshd 2> /dev/null
launchctl print system/com.openssh.sshd > /dev/null 2>&1 || launchctl bootstrap system /System/Library/LaunchDaemons/ssh.plist 2> /dev/null
if launchctl print system/com.openssh.sshd > /dev/null 2>&1; then say "ssh: sshd loaded (port 22, socket-activated)"; else say "ssh: sshd NOT loaded"; fi
# The root shell's job (pianousb-shell).
state=$(launchctl print system/com.unixnu.pianousb-shell 2> /dev/null | sed -n 's/^[[:space:]]*state = //p' | head -n 1)
say "usb shell: com.unixnu.pianousb-shell ${state:-not loaded} (tooling/native/usb-console.py --port 1)"
# The USB network: our interface by its MAC, DHCP, the address (up to 120 s for the interface, 90 s for an address).
waited=0 ifname=
while [ "$waited" -lt 120 ]; do
  for i in $(ifconfig -l 2> /dev/null); do
    ifconfig "$i" 2> /dev/null | grep -q 'ether 02:50:49:41:4e:02' && { ifname=$i; break; }
  done
  [ -n "$ifname" ] && break
  sleep 5
  waited=$((waited + 5))
done
if [ -z "$ifname" ]; then
  say "usb net: no interface with MAC 02:50:49:41:4e:02 after ${waited}s (piano-usb-net=1? PianoDwc3's net: lines)"
  exit 0
fi
ifconfig "$ifname" up 2> /dev/null
ipconfig set "$ifname" DHCP 2> /dev/null
addr= waited=0
while [ "$waited" -lt 90 ]; do
  addr=$(ifconfig "$ifname" 2> /dev/null | awk '$1 == "inet" {print $2; exit}')
  [ -n "$addr" ] && break
  sleep 3
  waited=$((waited + 3))
done
v6=$(ifconfig "$ifname" 2> /dev/null | awk '$1 == "inet6" && $2 ~ /^fe80/ {print $2; exit}')
say "usb net: $ifname inet ${addr:-none after 90 s} ${v6:+inet6 $v6} (ssh -i out/desk-dev/id_ed25519 <account>@${addr:-<address>})"
