#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The desk-dev image's boot-job step (tooling/k27/build-desktop-images.sh desk-dev, docs/attended-desktop.md section 3): the ways in
# from the Mac besides typing into Terminal, each said on the kernel console (/dev/console, "desk-dev: " lines):
#   - the root shell over the cable: pianousb-shell (the userland stage's, piano-usb-shell=1): `tooling/native/usb-console.py --port 1`;
#     here only whether its job runs;
#   - the USB network (piano-usb-net=1, docs/drivers/usb-device-debug.md section 14.8): PianoUsbNet's interface (MAC
#     02:50:49:41:4e:02, en1: the seed's NetworkInterfaces.plist has it, so configd names it at once instead of after its 240 s quiet
#     timeout), its address by the mode (the argument, the builder's DESK_DEV_NET):
#       auto (default)  DHCP for 20 s (Internet Sharing on the Mac: its lease), else the fixed link-local 169.254.80.2/16;
#       static          169.254.80.2/16 at once;      dhcp   DHCP only (a lease, or the self-assigned 169.254 address after it);
#     the Mac's side, left at "Using DHCP" without a server, self-assigns a 169.254 address too; tooling/native/desk-ssh.sh finds it
#     and binds ssh to it (the Mac routes 169.254/16 to its primary interface, not to the cable). The address is printed;
#   - sshd (Remote Login: the seed enables com.openssh.sshd, this makes sure it is loaded) with the key staged by the builder
#     (out/desk-dev/id_ed25519.pub, the private half only on the Mac) as authorized_keys of each account of uid 501 and up whose home
#     is in /Users, owned by it (sshd's StrictModes: the staged copy is uid 99's). Then, on the Mac: tooling/native/desk-ssh.sh;
#   - the IOKit registry's busy leaves, a few lines: what keeps the registry from ever going quiet (InterfaceNamer's wait, 14.8).
# Every wait is bounded; nothing here touches the accounts' passwords.
#
#   sh desk-dev.sh [auto|static|dhcp]     DESK_DEV_OUT (a file instead of /dev/console), DESK_DEV_KEYS (the staged key file) for a test
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
mode=${1:-auto}
case $mode in auto|static|dhcp) ;; *) say "usb net: mode $mode unknown (auto, static, dhcp): auto"; mode=auto ;; esac
fixed=169.254.80.2
# sshd answers on port 22 (launchd's socket; the first connection starts it).
if nc -z -G 3 127.0.0.1 22 > /dev/null 2>&1; then say "ssh: port 22 answers"; else say "ssh: port 22 does NOT answer"; fi
# The IOKit registry's busy leaves: what keeps the registry from going quiet (configd's InterfaceNamer waits for quiet, 240 s at most,
# before naming an interface it does not know, and SCNetworkInterfaceCopyAll's callers wait for the same). A busy entry's ancestors
# are busy too; the deepest busy entries are the ones to look at.
busy() {
  ioreg -w0 -p IOService 2> /dev/null | awk '
    /busy [1-9]/ {
      d = index($0, "+-o"); name = substr($0, d + 4); sub(/  <.*/, "", name)
      cls = $0; sub(/.*<class /, "", cls); sub(/,.*/, "", cls)
      b = $0; sub(/.*busy /, "", b); sub(/\).*/, ")", b)
      if (n && d <= depth[n]) leaf[n] = 1
      n++; depth[n] = d; text[n] = name " (" cls ") busy " b
    }
    END {
      if (n) leaf[n] = 1
      for (i = 1; i <= n; i++) if (leaf[i] && shown < 6) { print text[i]; shown++ }
      if (!n) print "none (the registry is quiet)"
    }'
}
# At configd's 260th second, past InterfaceNamer's 240 s quiet timeout (when it names the interfaces it did not know and the system
# configures new ones, which may replace the fixed address): the fixed address again, and what is still busy then.
late() {
  pid=$(pgrep -x configd 2> /dev/null | head -n 1)
  el=$(ps -o etime= -p "${pid:-1}" 2> /dev/null | awk -F'[-:]' '{s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; print s}')
  if [ -n "$el" ] && [ "$el" -lt 260 ] 2> /dev/null; then sleep $((260 - el)); el=260; fi
  if [ "${how%%,*}" = "fixed link-local" ] && ! ifconfig "$ifname" 2> /dev/null | grep -q "inet $fixed "; then
    ipconfig set "$ifname" MANUAL "$fixed" 255.255.0.0 2> /dev/null
    if wait_addr 15; then say "usb net: $ifname: $fixed was replaced (configd's own configuration of it): set again"
    else say "usb net: $ifname: $fixed was replaced and could NOT be set again: inet $(inet)"; fi
  fi
  busy | while IFS= read -r line; do say "iokit busy (configd ${el:-?}+ s): $line"; done
}
# The USB network: our interface by its MAC (named at once with the seed's NetworkInterfaces.plist; up to 300 s otherwise, which
# covers InterfaceNamer's 240 s quiet timeout), then the address by the mode.
waited=0 ifname= how=
while [ "$waited" -lt 300 ]; do
  for i in $(ifconfig -l 2> /dev/null); do
    ifconfig "$i" 2> /dev/null | grep -q 'ether 02:50:49:41:4e:02' && { ifname=$i; break; }
  done
  [ -n "$ifname" ] && break
  sleep 2
  waited=$((waited + 2))
done
if [ -z "$ifname" ]; then
  say "usb net: no interface with MAC 02:50:49:41:4e:02 after ${waited}s (piano-usb-net=1? PianoDwc3's net: lines)"
  late
  exit 0
fi
say "usb net: $ifname is ours (named after ${waited}s); mode $mode"
ifconfig "$ifname" up 2> /dev/null
inet() { ifconfig "$ifname" 2> /dev/null | awk '$1 == "inet" {print $2; exit}'; }
wait_inet() {   # wait_inet SECONDS [routable]: the first inet address (with routable: not 169.254) within SECONDS
  w=0
  while [ "$w" -lt "$1" ]; do
    a=$(inet)
    if [ -n "$a" ] && { [ -z "${2:-}" ] || [ "${a#169.254.}" = "$a" ]; }; then echo "$a"; return 0; fi
    sleep 1
    w=$((w + 1))
  done
  return 1
}
wait_addr() {   # wait_addr SECONDS: the fixed address on the interface within SECONDS
  w=0
  while [ "$w" -lt "$1" ]; do
    ifconfig "$ifname" 2> /dev/null | grep -q "inet $fixed " && return 0
    sleep 1
    w=$((w + 1))
  done
  return 1
}
addr=
if [ "$mode" != static ]; then
  ipconfig set "$ifname" DHCP 2> /dev/null
  if [ "$mode" = dhcp ]; then
    addr=$(wait_inet 90) && how="DHCP"
  else
    addr=$(wait_inet 20 routable) && how="a DHCP lease: Internet Sharing"
  fi
fi
if [ -z "$addr" ] && [ "$mode" != dhcp ]; then
  # ipconfig's MANUAL (IPConfiguration's own, conflict-checked by ARP); a plain alias only if that gives no address
  ipconfig set "$ifname" MANUAL "$fixed" 255.255.0.0 2> /dev/null
  if wait_addr 15; then addr=$fixed how="fixed link-local"
  elif ifconfig "$ifname" inet "$fixed" netmask 255.255.0.0 alias 2> /dev/null && wait_addr 3; then addr=$fixed how="fixed link-local, an alias"
  fi
fi
v6=$(ifconfig "$ifname" 2> /dev/null | awk '$1 == "inet6" && $2 ~ /^fe80/ {print $2; exit}')
say "usb net: $ifname inet ${addr:-none} (${how:-no address: $(inet)}) ${v6:+inet6 $v6}"
[ -z "$addr" ] || say "usb net: on the Mac: tooling/native/desk-ssh.sh -a $addr (ssh -B <the Mac's end> -i out/desk-dev/id_ed25519)"
late
