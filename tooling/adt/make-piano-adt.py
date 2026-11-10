#!/usr/bin/env python3
"""The piano ADT for the k27 kernel collection, made by UniXNU's own generator (scripts/unixnu/devicetree.py generate):
hardware from dts/piano.dts (+ UniXNU-sm8750-common's sm8750.dtsi), identity from configs/k27-adt.toml instead of the
macos-27.3 profile, the rest of board.toml as it is.

  make-piano-adt.py OUTPUT.adt --unique-chip-id HEX [--data] [--trust] [--dtb-out FILE] [--psci-method smc|hvc] [--gop-display]

--data / --trust: the payload carries a Data seed / trust cache (their /chosen/memory-map slots).
--psci-method: override /psci method (default: the DTS's, "smc"; "hvc" only for a test inside the VM).
--gop-display: add the [arm-io.*] nodes of configs/k27-gop-display.toml under /arm-io: /arm-io/gop-display, the display nub of
  DroidVMDisplay / NativeGopDisplay on the boot framebuffer (docs/drivers/native-gop-display.md). Off by default.
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


def main():
    p = argparse.ArgumentParser()
    p.add_argument('output', type=Path)
    p.add_argument('--unique-chip-id', required=True)
    p.add_argument('--data', action='store_true')
    p.add_argument('--trust', action='store_true')
    p.add_argument('--dtb-out', type=Path)
    p.add_argument('--psci-method', choices=('smc', 'hvc'))
    p.add_argument('--gop-display', action='store_true', help='add /arm-io/gop-display (configs/k27-gop-display.toml)')
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
    extra = []
    if a.gop_display:
        nodes = load_toml(DEVICE / 'configs/k27-gop-display.toml').get('arm-io', {})
        extra = [devicetree.platform_node(name, table) for name, table in nodes.items()]
    with arm_io_children(extra):
        tree = generate(dtb, config, unique_chip_id=a.unique_chip_id, data=a.data, trust=a.trust)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(tree)
    print(f'{a.output}: {len(tree)} bytes (hardware {dtb}){", with /arm-io/gop-display" if a.gop_display else ""}')


if __name__ == '__main__':
    main()
