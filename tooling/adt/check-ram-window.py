#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Offline check of XNU's RAM window for an image's inputs: what DarwinBoot (firmware/patches/darwinboot/0001) will place, against
the ADT's DRAM range, the FD's bound and the k27 kernel's limits. No device is touched.

    check-ram-window.py --adt ADT --kc KC [--fd FD]

DarwinBoot takes the first free run that holds the request at the kernel collection's offset in its 2 MiB block (AllocateWindow);
with the whole run 0x8c0000000..0xc00000000 of dts/piano.dts free, that is 0x8c0000000 + the offset. A request no run holds whole
is shrunk to the largest run, never below 8 GiB (DarwinWindowFit). Exit 1 when the window cannot be what the ADT says.
"""
import argparse
import importlib.util
import struct
import sys
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[2]
RUN = (0x8c0000000, 0xc00000000)           # dts/piano.dts memory@8c0000000: the hole-free run (docs/el1-hyp-notes.md section 3)
BLOCK = 0x200000
DARWIN_WINDOW_MAX, OLD_MAX = 0x340000000, 0x200000000
BOOTSTRAP_PAGES = 48                        # k27 0051: BOOTSTRAP_TABLE_SIZE, 2 + 2 x (1 GiB regions) used


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def chosen(adt_path):
    adt = load('adt_dump', DEVICE / 'tooling/adt/adt-dump.py')
    (_, children), _ = adt.parse(Path(adt_path).read_bytes())
    for props, _ in children:
        named = dict(props)
        if named.get('name', b'').rstrip(b'\0') == b'chosen':
            return {k: struct.unpack('<Q', v)[0] for k, v in named.items() if len(v) == 8 and k in
                    ('dram-base', 'dram-size', 'darwinboot-memory-size')}
    raise SystemExit(f'{adt_path}: no /chosen')


def kc_span(kc_path):
    data = Path(kc_path).read_bytes()[:1 << 20]
    magic, _, _, _, count = struct.unpack_from('<5I', data)
    if magic != 0xfeedfacf:
        raise SystemExit(f'{kc_path}: not a 64-bit Mach-O')
    offset, segments = 32, []
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x19:
            vmaddr, vmsize = struct.unpack_from('<QQ', data, offset + 24)
            if vmsize:
                segments.append((vmaddr, vmaddr + vmsize))
        offset += size
    return min(s for s, _ in segments), max(e for _, e in segments)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--adt', type=Path, required=True)
    p.add_argument('--kc', type=Path, required=True)
    p.add_argument('--fd', type=Path)
    a = p.parse_args()
    c = chosen(a.adt)
    base, end = kc_span(a.kc)
    request = c.get('darwinboot-memory-size', 0x10000000)
    dram = (c['dram-base'], c['dram-base'] + c['dram-size'])
    start = (RUN[0] + BLOCK - 1) // BLOCK * BLOCK + (base & (BLOCK - 1))
    avail = RUN[1] - start
    if avail >= request:
        window = request
    elif request > OLD_MAX and avail // BLOCK * BLOCK >= OLD_MAX:
        window = avail // BLOCK * BLOCK
    else:
        window = 0
    bound = OLD_MAX
    if a.fd:
        bound = load('persist_common', DEVICE / 'tooling/persist/common.py').fd_ram_window(a.fd)
    regions = (start + window - 1) // (1 << 30) - start // (1 << 30) + 1 if window else 0
    problems = []
    if request % BLOCK or not 0x10000000 <= request <= DARWIN_WINDOW_MAX:
        problems.append(f'request {request:#x} is outside DarwinBoot\'s bound or not in 2 MiB steps')
    if request > bound:
        problems.append(f'request {request:#x} is past the FD\'s DarwinBoot bound {bound:#x}: that DarwinBoot refuses the ADT')
    if not window:
        problems.append('no window: DarwinBoot would draw NO WINDOW')
    if window and not (dram[0] <= start and start + window <= dram[1]):
        problems.append(f'window {start:#x}..{start + window:#x} is outside the ADT\'s DRAM {dram[0]:#x}..{dram[1]:#x}')
    if window and end - base > window - BLOCK:
        problems.append('the kernel collection alone does not fit the window')
    if 2 + 2 * regions > BOOTSTRAP_PAGES:
        problems.append(f'{2 + 2 * regions} bootstrap table pages needed, the kernel has {BOOTSTRAP_PAGES}')
    print(f'ADT: request {request:#x} ({request >> 20} MiB), DRAM {dram[0]:#x}..{dram[1]:#x}; FD bound {bound:#x}; '
          f'KC {base:#x} (block offset {base & (BLOCK - 1):#x}), {(end - base) >> 20} MiB')
    print(f'window with the run whole: {start:#x}..{start + window:#x} ({window >> 20} MiB), {2 + 2 * regions} of {BOOTSTRAP_PAGES} '
          f'bootstrap table pages; the console line to expect: NativeRamBlock: RAM window {start:#x}..{start + window:#x} '
          f'({window >> 20} MiB)')
    for problem in problems:
        print('PROBLEM: ' + problem)
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
