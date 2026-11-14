#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-usbnet-address (configs/userland.toml; docs/drivers/usb-device-debug.md section 14.9): the USB network's debug address
# (169.254.80.2/16 on PianoUsbNet's interface, MAC 02:50:49:41:4e:02, the address tooling/native/desk-ssh.sh dials) kept through
# configd's own configuration of that interface. desk-dev.sh sets it with `ipconfig set IF MANUAL`, which is IPConfiguration's
# setting, not a SystemConfiguration service: when configd configures the interface itself (the registry's quiet, or the login
# wave on a fast boot) it applies its service for it, DHCP with a self-assigned link-local address, and the fixed address is
# gone; ssh then times out (ufs4a, ufs4b, the v6 RAM test of 2026-11-14: private host-checks SSH-AFTER-LOGIN.md, H1, inferred).
#   1. waits (300 s) for the interface and for desk-dev's fixed address on it; a DHCP lease (Internet Sharing on the Mac) or no
#      address by then: nothing to keep, it exits;
#   2. in the background: the interface's SystemConfiguration service (the one configd made for it, else a new "Piano USB" on its
#      hardware port) set to IPv4 Manual 169.254.80.2/16 with networksetup, so configd applies the address itself whenever it
#      configures the interface; on a persistent Data store (the installed system) the setting stays. networksetup waits until
#      configd's interface list is ready (the registry's quiet or the login wave): its line says after how long;
#   3. for 600 s, every 10 s: the address on the interface, else one console line with what is there instead (its inet addresses and
#      scutil --nwi's IPv4 part: the check of H1) and the address set again (ipconfig MANUAL).
# Without piano-usb-net=N (N not 0) in kern.bootargs: exits at once. Lines "piano-usbnet-address: ..." on the console.
# A test runs it with PIANO_USBNET_PATH (stub commands first), PIANO_USBNET_OUT (a file), PIANO_USBNET_WAIT/_GUARD/_PERIOD.
set -u
PATH=${PIANO_USBNET_PATH:-/usr/sbin:/usr/bin:/sbin:/bin}
out=${PIANO_USBNET_OUT:-/dev/console}
fixed=169.254.80.2 mask=255.255.0.0 mac=02:50:49:41:4e:02 svc="Piano USB"
wait_s=${PIANO_USBNET_WAIT:-300} guard_s=${PIANO_USBNET_GUARD:-600} period=${PIANO_USBNET_PERIOD:-10}
say() { printf 'piano-usbnet-address: %.230s\n' "$1" >> "$out" 2> /dev/null; }

case " $(sysctl -n kern.bootargs 2> /dev/null) " in
  *" piano-usb-net="[1-9]*) ;;
  *) exit 0 ;;
esac

ifname= w=0
has() { [ -n "$ifname" ] && ifconfig "$ifname" 2> /dev/null | grep -q "inet $fixed "; }
first_inet() { ifconfig "$ifname" 2> /dev/null | awk '$1 == "inet" {print $2; exit}'; }
while [ "$w" -lt "$wait_s" ]; do
  if [ -z "$ifname" ]; then
    for i in $(ifconfig -l 2> /dev/null); do
      ifconfig "$i" 2> /dev/null | grep -q "ether $mac" && { ifname=$i; break; }
    done
  fi
  if [ -n "$ifname" ]; then
    has && break
    a=$(first_inet)
    [ -n "$a" ] && [ "${a#169.254.}" = "$a" ] && break   # a DHCP lease: configd's own configuration is what serves
  fi
  sleep 2
  w=$((w + 2))
done
[ -n "$ifname" ] || { say "no interface with MAC $mac after ${w} s: nothing to keep"; exit 0; }
if ! has; then
  a=$(first_inet)
  say "$ifname has ${a:-no IPv4 address}, not $fixed, after ${w} s (a DHCP lease, or desk-dev did not set it): nothing to keep"
  exit 0
fi
say "$ifname has $fixed (after ${w} s): its SystemConfiguration service next, the address guarded for ${guard_s} s"

(
  t0=$(date +%s)
  cur=$(networksetup -listnetworkserviceorder 2> /dev/null | awk -v d="$ifname" '
    /^\([0-9*]+\) / { s = $0; sub(/^\([0-9*]+\) /, "", s) }
    index($0, "Device: " d ")") { print s; exit }')
  made=
  if [ -z "$cur" ]; then
    port=$(networksetup -listallhardwareports 2> /dev/null | awk -v d="$ifname" '
      /^Hardware Port: / { p = $0; sub(/^Hardware Port: /, "", p) }
      $1 == "Device:" && $2 == d { print p; exit }')
    if [ -n "$port" ] && networksetup -createnetworkservice "$svc" "$port" > /dev/null 2>&1; then cur=$svc made=" (made, on port $port)"; fi
  fi
  if [ -n "$cur" ] && networksetup -setmanual "$cur" "$fixed" "$mask" > /dev/null 2>&1; then
    say "$ifname: service \"$cur\"$made set to IPv4 Manual $fixed/16 after $(( $(date +%s) - t0 )) s: configd applies it from now on"
  else
    say "$ifname: no service set after $(( $(date +%s) - t0 )) s (service ${cur:-none}, port ${port:-not listed}): the guard alone keeps it"
  fi
) &

end=$(( $(date +%s) + guard_s )) lost=0
while [ "$(date +%s)" -lt "$end" ]; do
  sleep "$period"
  has && continue
  lost=$((lost + 1))
  if [ "$lost" -le 3 ]; then
    now=$(ifconfig "$ifname" 2> /dev/null | awk '$1 == "inet" { printf "%s ", $2 }')
    nwi=$(scutil --nwi 2> /dev/null | awk '/^IPv4/ { f = 1 } /^IPv6/ { f = 0 } f && NF { printf "%s; ", $0 }')
    say "$ifname lost $fixed (inet now: ${now:-none}; scutil --nwi: ${nwi:-nothing}): set again (loss $lost)"
  fi
  ipconfig set "$ifname" MANUAL "$fixed" "$mask" 2> /dev/null
done
wait
say "$ifname: guard over after ${guard_s} s, $lost losses, $fixed $(has && echo present || echo ABSENT)"
exit 0
