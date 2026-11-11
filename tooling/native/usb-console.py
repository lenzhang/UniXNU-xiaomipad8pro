#!/usr/bin/env python3
"""The Mac's end of PianoDwc3 (docs/drivers/usb-device-debug.md): the piano's USB serial ports, interactive or captured.

  usb-console.py                 wait for the tablet, open port 0 (kernel console + monitor), type commands; Ctrl-] quits
  usb-console.py --port 1        the user pipe (/dev/pianousb on the tablet: pianousb-shell, log stream)
  usb-console.py --capture 300   no keyboard: save what arrives for 300 s (reconnects across reboots), then exit
  usb-console.py --list          what is attached
  usb-console.py --send 'usb'    send one monitor command after opening (repeatable), e.g. with --capture

Every byte received is also written to logs/usb-console/<time>-port<N>.txt (gitignored), and every complete line once more to
<time>-port<N>.times.txt behind the Mac's clock when it arrived (UTC seconds, 3 decimals, a tab): the reference for the PMIC
RTC's offset (tooling/native/rtc-offset.py, docs/drivers/rtc.md). Only the Python standard library;
no root needed (the cu.* nodes are world-accessible). The device is found by its USB product string ("piano macOS debug",
via ioreg) and the interface names "piano kernel console" / "piano user pipe"; --device overrides.
"""
import argparse
import datetime
import fcntl
import os
import plistlib
import select
import struct
import subprocess
import sys
import termios
import time
import tty
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PRODUCT = 'piano macOS debug'
INTERFACES = {0: 'piano kernel console', 1: 'piano user pipe', 2: 'piano input'}   # 2: piano-usb-hid=1 (tooling/native/piano-input.py)
TIOCMBIS = getattr(termios, 'TIOCMBIS', 0x8004746c)
TIOCM_DTR = getattr(termios, 'TIOCM_DTR', 0x2)
TIOCM_RTS = getattr(termios, 'TIOCM_RTS', 0x4)


def walk(node, parents=()):
    yield node, parents
    for child in node.get('IORegistryEntryChildren', []):
        yield from walk(child, parents + (node,))


def find_ports():
    """{port: /dev/cu.*} of the attached piano gadget, from the IOService plane (ioreg -a)."""
    try:
        out = subprocess.run(['ioreg', '-a', '-l', '-r', '-c', 'IOUSBHostDevice'], capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return {}
    if not out.strip():
        return {}
    # The gadget may sit behind hubs: every IOUSBHostDevice in the tree, not only the roots ioreg -r returns.
    gadgets = [node for root in plistlib.loads(out) for node, _ in walk(root)
               if node.get('IOObjectClass') == 'IOUSBHostDevice' and node.get('kUSBProductString', node.get('USB Product Name')) == PRODUCT]
    found, nodes = {}, []
    for gadget in gadgets:
        for node, parents in walk(gadget):
            callout = node.get('IOCalloutDevice')
            if not callout:
                continue
            nodes.append(callout)
            names = [p.get('kUSBString', p.get('IORegistryEntryName', '')) for p in parents + (node,)]
            for port, name in INTERFACES.items():
                if name in names:
                    found[port] = callout
    if not found and nodes:   # interface names not visible: the order of the nodes
        found = dict(enumerate(sorted(nodes)))
    return found


def describe_gadgets():
    """What macOS made of the gadget: one line per IOUSBHostDevice "piano macOS debug" (configuration, drivers, interfaces)."""
    try:
        out = subprocess.run(['ioreg', '-a', '-l', '-r', '-c', 'IOUSBHostDevice'], capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return []
    lines = []
    for root in (plistlib.loads(out) if out.strip() else []):
        for node, _ in walk(root):
            if node.get('IOObjectClass') != 'IOUSBHostDevice' or node.get('kUSBProductString', node.get('USB Product Name')) != PRODUCT:
                continue
            kids = node.get('IORegistryEntryChildren', [])
            drivers = sorted({k.get('IOObjectClass', '?') for k in kids if k.get('IOObjectClass') != 'IOUSBHostInterface'})
            interfaces = [k.get('kUSBString', '-') + '/' + ','.join(sorted({c.get('IOObjectClass', '?') for c in k.get('IORegistryEntryChildren', [])}) or 'no driver')
                          for k in kids if k.get('IOObjectClass') == 'IOUSBHostInterface']
            lines.append(f'gadget {node.get("idVendor", 0):04x}:{node.get("idProduct", 0):04x} at {node.get("locationID", 0):#x}, speed '
                         f'{node.get("USBSpeed", node.get("Device Speed", "?"))}, configuration '
                         f'{node.get("kUSBCurrentConfiguration", "none")}; drivers {drivers or "none"}; interfaces {interfaces or "none"}')
    return lines


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~getattr(termios, 'CRTSCTS', 0)
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    try:
        fcntl.ioctl(fd, TIOCMBIS, struct.pack('I', TIOCM_DTR | TIOCM_RTS))   # DTR: PianoDwc3 starts streaming
    except OSError:
        pass   # a tty without modem lines (a pty in tests); macOS raises DTR on open anyway
    return fd


class Stamper:
    """Complete lines, each behind the Mac's time (time.time(), UTC seconds) at which the chunk that ended it arrived."""

    def __init__(self, out):
        self.out, self.partial = out, b''

    def feed(self, data, now):
        lines = (self.partial + data).split(b'\n')
        self.partial = lines.pop()
        for line in lines:
            text = line.rstrip(b'\r').decode('utf-8', 'replace')
            self.out.write(f'{now:.3f}\t{text}\n')


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('--port', type=int, default=0, choices=(0, 1, 2))
    a.add_argument('--device', help='a /dev/cu.* node instead of the ioreg lookup')
    a.add_argument('--capture', type=float, metavar='SECONDS', help='save for this long, no keyboard')
    a.add_argument('--send', action='append', default=[], help='a line to send once the port is open')
    a.add_argument('--list', action='store_true')
    a.add_argument('--log-dir', type=Path, default=REPO / 'logs/usb-console')
    args = a.parse_args()
    if args.list:
        ports = find_ports()
        print('\n'.join(describe_gadgets()) or 'no piano gadget in the USB tree')
        print('\n'.join(f'port {p}: {d}' for p, d in sorted(ports.items())) or 'no serial ports of the gadget')
        others = sorted(str(p) for p in Path('/dev').glob('cu.usbmodem*'))
        if others:
            print('all /dev/cu.usbmodem*:', ' '.join(others))
        return 0
    args.log_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    log_path = args.log_dir / f'{stamp}-port{args.port}.txt'
    log = open(log_path, 'ab', buffering=0)
    times = Stamper(open(log_path.with_suffix('.times.txt'), 'a', buffering=1))
    interactive = args.capture is None and sys.stdin.isatty()
    deadline = time.monotonic() + args.capture if args.capture is not None else None
    print(f'[usb-console] port {args.port}, log {log_path}' + ('; Ctrl-] quits' if interactive else ''), file=sys.stderr)
    saved = termios.tcgetattr(sys.stdin) if interactive else None
    try:
        if interactive:
            tty.setcbreak(sys.stdin.fileno())
            attrs = termios.tcgetattr(sys.stdin)
            attrs[3] &= ~(termios.ISIG | termios.ECHO)
            termios.tcsetattr(sys.stdin, termios.TCSANOW, attrs)
        waiting = False
        while deadline is None or time.monotonic() < deadline:
            path = args.device or find_ports().get(args.port)
            if not path or not os.path.exists(path):
                if not waiting:
                    print('[usb-console] waiting for the tablet ...', file=sys.stderr)
                    waiting = True
                time.sleep(0.5)
                continue
            waiting = False
            try:
                fd = open_port(path)
            except OSError as e:
                print(f'[usb-console] {path}: {e}', file=sys.stderr)
                time.sleep(1)
                continue
            print(f'[usb-console] connected: {path}', file=sys.stderr)
            for line in args.send:
                os.write(fd, line.encode() + b'\r')
            try:
                while deadline is None or time.monotonic() < deadline:
                    fds = [fd] + ([sys.stdin] if interactive else [])
                    ready, _, _ = select.select(fds, [], [], 0.5)
                    if fd in ready:
                        data = os.read(fd, 65536)
                        if not data:
                            raise OSError('end of file')
                        log.write(data)
                        times.feed(data, time.time())
                        sys.stdout.buffer.write(data)
                        sys.stdout.buffer.flush()
                    if interactive and sys.stdin in ready:
                        key = os.read(sys.stdin.fileno(), 1024)
                        if b'\x1d' in key:
                            return 0
                        os.write(fd, key.replace(b'\n', b'\r'))
            except OSError as e:
                print(f'\n[usb-console] disconnected ({e}); waiting again', file=sys.stderr)
            finally:
                os.close(fd)
        return 0
    finally:
        if saved is not None:
            termios.tcsetattr(sys.stdin, termios.TCSADRAIN, saved)
        log.close()
        times.out.close()


if __name__ == '__main__':
    sys.exit(main())
