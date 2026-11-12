#!/usr/bin/env python3
"""The piano ADT for the k27 kernel collection, made by UniXNU's own generator (scripts/unixnu/devicetree.py generate):
hardware from dts/piano.dts (+ UniXNU-sm8750-common's sm8750.dtsi), identity from configs/k27-adt.toml instead of the
macos-27.3 profile, the rest of board.toml as it is.

  make-piano-adt.py OUTPUT.adt --unique-chip-id HEX [--data] [--trust] [--addon] [--dtb-out FILE] [--psci-method smc|hvc] [--no-gop-display]
                    [--bt-address-file FILE | --no-bt-address]

--data / --trust: the payload carries a Data seed / trust cache (their /chosen/memory-map slots).
--addon: the payload carries Payloads/kernel.macho.gpuaddon (the /chosen/memory-map GpuAddon slot DarwinBoot fills): a RAM test image
  with the sensor pack (PIANO_SENSOR_PACK, docs/drivers/sensors.md section 5b). DarwinBoot refuses a slot without the file and a file
  without the slot, so an ADT with it is never the installed system's (build-esp.py refuses it).
--psci-method: override /psci method (default: the DTS's, "smc"; "hvc" only for a test inside the VM).
The [arm-io.*] nodes of configs/k27-gop-display.toml go under /arm-io: /arm-io/gop-display, the display nub of NativeGopDisplay on
  the boot framebuffer (docs/drivers/native-gop-display.md). --no-gop-display leaves them out (the ADT without a display).
--bt-address-file FILE: the unit's Bluetooth address (one line, six hex pairs separated by colons, most significant first), private:
  by default private/bluetooth-address when that file exists (never committed: .gitignore), --no-bt-address none. It goes into
  /arm-io/piano-bt as the six-byte property bt-address, which PianoBluetooth's level 4 writes to the controller
  (docs/drivers/bluetooth.md section 8.3). No driver matches that node; the value is never printed.
Environment: UNIXNU_RESEARCH (default ~/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research).
"""
import argparse
import contextlib
import os
import subprocess
import sys
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[2]
RESEARCH = Path(os.environ.get('UNIXNU_RESEARCH', Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research'))
sys.path.insert(0, str(RESEARCH / 'UniXNU/scripts'))
from unixnu.config import load_toml  # noqa: E402
from unixnu import devicetree  # noqa: E402
from unixnu.devicetree import generate  # noqa: E402
from unixnu.profiles import merge  # noqa: E402


@contextlib.contextmanager
def arm_io_children(extra):
    """generate() with more /arm-io children: UniXNU's generator makes /arm-io from the DTS's unixnu,adt-name entries only, through
    its module-level node(), which this wraps for the duration."""
    original = devicetree.node

    def node(name, properties=(), children=()):
        return original(name, properties, [*children, *extra] if name == 'arm-io' else children)
    devicetree.node = node
    try:
        yield
    finally:
        devicetree.node = original


def bt_address(path, required):
    """The six bytes of a private address file (most significant first), or None when there is none and none was asked for. The
    value never appears in a message."""
    if not path.exists():
        if required:
            raise SystemExit(f'{path}: no such file')
        return None
    parts = path.read_text().strip().split(':')
    try:
        raw = bytes(int(x, 16) for x in parts) if len(parts) == 6 and all(len(x) == 2 for x in parts) else b''
    except ValueError:
        raw = b''
    if len(raw) != 6 or raw[0] & 1 or raw in (bytes(6), b'\xff' * 6):
        raise SystemExit(f'{path}: not a unicast Bluetooth address written as six hex pairs separated by colons')
    return raw


def main():
    p = argparse.ArgumentParser()
    p.add_argument('output', type=Path)
    p.add_argument('--unique-chip-id', required=True)
    p.add_argument('--data', action='store_true')
    p.add_argument('--trust', action='store_true')
    p.add_argument('--addon', action='store_true', help='the GpuAddon slot (a RAM test image carrying the sensor pack)')
    p.add_argument('--dtb-out', type=Path)
    p.add_argument('--psci-method', choices=('smc', 'hvc'))
    p.add_argument('--no-gop-display', dest='gop_display', action='store_false', help='leave out /arm-io/gop-display')
    p.add_argument('--gop-display', dest='gop_display', action='store_true', help=argparse.SUPPRESS)   # the default; kept for old commands
    # A test ADT's /defaults vmm-present (configs/k27-adt.toml has 1; see its note). 0 is the truthful value natively: Core Animation then
    # gives WindowServer 8-bit framebuffers instead of the half-float ones of a virtual machine (docs/drivers/soft-metal.md §9).
    p.add_argument('--vmm-present', type=int, choices=(0, 1))
    # XNU's RAM window (/chosen darwinboot-memory-size) instead of configs/k27-adt.toml's owned_memory_size: 0x200000000 for an image
    # or ESP booted by an FD whose DarwinBoot lacks firmware/patches/darwinboot/0001 (it refuses more than 8 GiB).
    p.add_argument('--ram-window', type=lambda v: int(v, 0), help='bytes, 2 MiB steps, 256 MiB to 13 GiB')
    p.add_argument('--bt-address-file', type=Path, help='the unit Bluetooth address (default: private/bluetooth-address if present)')
    p.add_argument('--no-bt-address', action='store_true', help='no /arm-io/piano-bt node')
    a = p.parse_args()
    dtb = a.dtb_out or a.output.with_suffix('.hw.dtb')
    dtb.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-i', str(RESEARCH / 'UniXNU-sm8750-common/dts'),
                    '-o', str(dtb), str(DEVICE / 'dts/piano.dts')], check=True)
    if a.psci_method:
        subprocess.run(['fdtput', '-t', 's', str(dtb), '/psci', 'method', a.psci_method], check=True)
    board = load_toml(DEVICE / 'board.toml')
    board.pop('adt', None)
    board.get('payload', {}).pop('adt', None)
    config = merge(board, load_toml(DEVICE / 'configs/k27-adt.toml'))
    if a.vmm_present is not None:
        config['adt']['defaults']['u32']['vmm-present'] = a.vmm_present
    if a.ram_window is not None:
        config['payload']['adt']['owned_memory_size'] = hex(a.ram_window)
    extra = []
    if a.gop_display:
        nodes = load_toml(DEVICE / 'configs/k27-gop-display.toml').get('arm-io', {})
        extra = [devicetree.platform_node(name, table) for name, table in nodes.items()]
    address = None if a.no_bt_address else bt_address(a.bt_address_file or DEVICE / 'private/bluetooth-address', a.bt_address_file)
    if address:
        extra.append(devicetree.node('piano-bt', [('bt-address', address)]))
    with arm_io_children(extra):
        tree = generate(dtb, config, unique_chip_id=a.unique_chip_id, data=a.data, trust=a.trust, addon=a.addon)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(tree)
    print(f'{a.output}: {len(tree)} bytes (hardware {dtb}){", with /arm-io/gop-display" if a.gop_display else ""}'
          f'{", with the GpuAddon slot" if a.addon else ""}'
          f'{", with /arm-io/piano-bt (a private Bluetooth address)" if address else ""}')


if __name__ == '__main__':
    main()
