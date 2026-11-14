#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host test of piano-usbnet-address.sh with stub commands (sysctl, ifconfig, ipconfig, networksetup, scutil): without piano-usb-net it
does nothing; with desk-dev's fixed address it sets the interface's SystemConfiguration service (configd's own, else a new one on the
hardware port) to Manual 169.254.80.2/16, and when "configd" replaces the address during the guard it logs what was there instead
(inet, scutil --nwi) and sets it again; a DHCP lease is left alone."""
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / 'piano-usbnet-address.sh'
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what)
    if not ok:
        failures.append(what)


STUBS = {
    'sysctl': 'cat "$T/bootargs"',
    'ifconfig': '''if [ "$1" = -l ]; then echo "lo0 en0 en1"; exit 0; fi
if [ "$1" = en1 ]; then echo "en1: flags=8863<UP> mtu 1500"; echo "	ether 02:50:49:41:4e:02"; [ -s "$T/inet" ] && echo "	inet $(cat "$T/inet") netmask 0xffff0000"; exit 0; fi
echo "$1: flags=8863<UP> mtu 1500"; echo "	ether 02:50:49:41:4e:01"''',
    'ipconfig': 'echo "ipconfig $*" >> "$T/calls"; [ "$3" = MANUAL ] && echo "$4" > "$T/inet"; exit 0',
    'networksetup': '''echo "networksetup $*" >> "$T/calls"
case "$1" in
  -listnetworkserviceorder) [ -f "$T/service" ] && printf '(1) %s\\n(Hardware Port: USB LAN, Device: en1)\\n\\n(2) Null\\n(Hardware Port: Ethernet, Device: en0)\\n' "$(cat "$T/service")" ;;
  -listallhardwareports) printf 'Hardware Port: Ethernet\\nDevice: en0\\nEthernet Address: 02:50:49:41:4e:01\\n\\nHardware Port: USB LAN\\nDevice: en1\\nEthernet Address: 02:50:49:41:4e:02\\n' ;;
  -createnetworkservice) echo "$2" > "$T/service" ;;
  -setmanual) echo "$2 $3 $4" > "$T/manual" ;;
esac
exit 0''',
    'scutil': 'echo "IPv4 network interface information"; echo "     en1 : flags 0x5 (IPv4,DNS)"; echo "           address 169.254.33.7"; echo "IPv6 network interface information"',
}


def run(bootargs, inet, service=None, replace_after=None, guard=4, wait=4):
    t = Path(tempfile.mkdtemp(prefix='usbnet-'))
    stubs = t / 'bin'
    stubs.mkdir()
    for name, body in STUBS.items():
        (stubs / name).write_text('#!/bin/sh\n' + body + '\n')
        (stubs / name).chmod(0o755)
    (t / 'bootargs').write_text(bootargs + '\n')
    (t / 'inet').write_text(inet + '\n' if inet else '')
    if service:
        (t / 'service').write_text(service + '\n')
    env = dict(os.environ, T=str(t), PIANO_USBNET_PATH=f'{stubs}:/usr/bin:/bin', PIANO_USBNET_OUT=str(t / 'console'),
               PIANO_USBNET_WAIT=str(wait), PIANO_USBNET_GUARD=str(guard), PIANO_USBNET_PERIOD='1')
    p = subprocess.Popen(['/bin/sh', str(SCRIPT)], env=env)
    if replace_after is not None:
        time.sleep(replace_after)
        (t / 'inet').write_text('169.254.33.7\n')   # configd's own configuration: a self-assigned address instead of the fixed one
    p.wait(timeout=60)
    read = lambda name: (t / name).read_text() if (t / name).exists() else ''
    return p.returncode, read('console'), read('calls'), read('manual').strip(), read('inet').strip(), read('service').strip()


rc, console, calls, manual, inet, _ = run('debug=0x14e piano-usb=2', '169.254.80.2')
check(rc == 0 and console == '' and calls == '', 'no piano-usb-net: nothing done, nothing said')
rc, console, calls, manual, inet, _ = run('piano-usb-net=0 piano-usb=2', '169.254.80.2')
check(rc == 0 and console == '', 'piano-usb-net=0: nothing done')

rc, console, calls, manual, inet, service = run('a=1 piano-usb-net=1 b=2', '169.254.80.2', service='Ethernet Adaptor', replace_after=1.5)
check(rc == 0, 'exit 0')
check('en1 has 169.254.80.2' in console, 'the fixed address found on en1 (by its MAC)')
check(manual == 'Ethernet Adaptor 169.254.80.2 255.255.0.0', f'configd\'s own service for en1 set Manual ({manual!r})')
check('-createnetworkservice' not in calls, 'no second service when one exists for en1')
check('lost 169.254.80.2 (inet now: 169.254.33.7' in console and 'scutil --nwi: IPv4 network interface information' in console,
      'the loss logged with what replaced it and scutil --nwi (the check of H1)')
check('ipconfig set en1 MANUAL 169.254.80.2 255.255.0.0' in calls and inet == '169.254.80.2', 'the address set again')
check('guard over after 4 s, 1 losses, 169.254.80.2 present' in console, f'the guard\'s summary ({console.splitlines()[-1]!r})')

rc, console, calls, manual, inet, service = run('piano-usb-net=1', '169.254.80.2')
check(service == 'Piano USB' and 'networksetup -createnetworkservice Piano USB USB LAN' in calls, 'no service for en1: "Piano USB" made '
      'on en1\'s hardware port')
check(manual == 'Piano USB 169.254.80.2 255.255.0.0' and 'made, on port USB LAN' in console, 'and set Manual')
check('0 losses' in console and 'ipconfig' not in calls, 'nothing lost, nothing set again')

rc, console, calls, manual, inet, service = run('piano-usb-net=1', '198.51.100.23')
check('has 198.51.100.23, not 169.254.80.2' in console and 'nothing to keep' in console and manual == '', 'a DHCP lease: left alone')
rc, console, calls, manual, inet, service = run('piano-usb-net=1', '', wait=2)
check('has no IPv4 address' in console and manual == '', 'no address after the wait: nothing to keep')

print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
sys.exit(1 if failures else 0)
