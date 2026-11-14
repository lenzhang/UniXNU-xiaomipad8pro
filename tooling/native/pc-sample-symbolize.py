#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Name the addresses in a boot log's `pc-sample:` and `sample:` lines (the k27 kernel's bring-up profilers, osfmk/kern/exception.c:
pc-sample=S [pc-sample-hz=N] and sample-name=NAME[,NAME] [sample-period=S]).

    pc-sample-symbolize.py LOG --kc KC [--kernel KERNEL.macho] [--natives KC.natives.json] [--period N]

KC is the image's kernel collection (out/k27/kc-k27-native.bin, or an image's Payloads/kernel.macho): its fileset entries give each
kext's segments, so an address becomes `kext SEGMENT+offset`. KERNEL is the kernel the collection was linked with (its symbols name
kernel addresses), --natives the build's natives.json (this repository's kexts' symbols). A kext address in code that uses the ARMv8
SHA-256 instructions within 256 bytes is marked [sha256 insns] (corecrypto's hardware block function). Prints every pc-sample period
(its task line and the hottest pcs, merged per function) and every sample: frame with names; --period N only the Nth period.
"""
import argparse
import bisect
import re
import struct
import subprocess
import sys
from pathlib import Path

LC_SEGMENT_64, LC_FILESET_ENTRY = 0x19, 0x80000035


def fileset(path):
    data = path.read_bytes()
    _, _, _, _, ncmds, _, _, _ = struct.unpack_from('<8I', data, 0)
    p, segs = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, p)
        if cmd == LC_FILESET_ENTRY:
            vmaddr, fileoff, stroff = struct.unpack_from('<QQI', data, p + 8)
            name = data[p + stroff:p + size].split(b'\0')[0].decode()
            magic, _, _, _, n2, _, _, _ = struct.unpack_from('<8I', data, fileoff)
            if magic == 0xfeedfacf:
                q = fileoff + 32
                for _ in range(n2):
                    c2, s2 = struct.unpack_from('<II', data, q)
                    if c2 == LC_SEGMENT_64:
                        seg = data[q + 8:q + 24].split(b'\0')[0].decode()
                        vm, vs, fo, fs = struct.unpack_from('<4Q', data, q + 24)
                        if vs:
                            segs.append((vm, vm + vs, name, seg, fo, fs))
                    q += s2
        p += size
    segs.sort()
    return data, segs


def sha_insn(word):
    return ((word & 0xFFE0FC00) in (0x5E004000, 0x5E005000, 0x5E006000)) or (word & 0xFFFFFC00) == 0x5E282800


class Names:
    def __init__(self, kc, kernel, natives):
        self.data, self.segs = fileset(kc)
        self.starts = [s[0] for s in self.segs]
        self.syms = []
        if kernel:
            out = subprocess.run(['xcrun', 'nm', '-n', str(kernel)], capture_output=True, text=True).stdout
            for line in out.split('\n'):
                p = line.split()
                if len(p) >= 3 and p[1] in 'tTsS':
                    try:
                        self.syms.append((int(p[0], 16), p[2]))
                    except ValueError:
                        pass
        if natives:
            import json
            for name, value in json.loads(natives.read_text()).items():
                self.syms.append((int(value), name))
        self.syms.sort()
        self.keys = [a for a, _ in self.syms]

    shift = 0   # --apple-kc: Apple's own collection (0xfffffe00... addresses) for a k27 KC that kept its kexts' places

    def segment(self, address):
        address -= self.shift
        i = bisect.bisect_right(self.starts, address) - 1
        if i >= 0 and self.segs[i][0] <= address < self.segs[i][1]:
            return self.segs[i]
        return None

    def name(self, address):
        seg = self.segment(address)
        where = f'{seg[2]} {seg[3]}+0x{address - self.shift - seg[0]:x}' if seg else '?'
        symbol = ''
        i = bisect.bisect_right(self.keys, address) - 1
        if i >= 0 and seg and self.syms[i][0] >= seg[0] + self.shift and address - self.syms[i][0] < 0x10000:
            symbol = f'{self.syms[i][1]}+0x{address - self.syms[i][0]:x}'
        mark = ''
        if seg and seg[3] == '__TEXT_EXEC':
            off = seg[4] + (address - self.shift - seg[0])
            lo, hi = max(seg[4], off - 256) & ~3, min(seg[4] + seg[5], off + 256)
            if any(sha_insn(struct.unpack_from('<I', self.data, o)[0]) for o in range(lo, hi - 3, 4)):
                mark = ' [sha256 insns]'
        return (symbol or where) + (f' ({where})' if symbol and seg and seg[2] != 'com.apple.kernel' else '') + mark

    def function(self, address):
        i = bisect.bisect_right(self.keys, address) - 1
        seg = self.segment(address)
        if i >= 0 and seg and self.syms[i][0] >= seg[0] + self.shift and address - self.syms[i][0] < 0x10000:
            return self.syms[i][1]
        return f'{seg[2]} {seg[3]}+0x{(address - self.shift - seg[0]) & ~0xff:x}' if seg else '?'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', type=Path)
    ap.add_argument('--kc', type=Path, required=True)
    ap.add_argument('--kernel', type=Path)
    ap.add_argument('--natives', type=Path)
    ap.add_argument('--period', type=int)
    ap.add_argument('--apple-kc', action='store_true', help="KC is Apple's collection: map 0xfffffff0... onto its 0xfffffe00...")
    a = ap.parse_args()
    names = Names(a.kc, a.kernel, a.natives)
    if a.apple_kc:
        names.shift = 0xfffffff000000000 - 0xfffffe0000000000
    period, hot = 0, {}

    def flush():
        if hot and (a.period is None or a.period == period):
            total = sum(hot.values())
            print(f'  hottest kernel pcs merged per function ({total} samples in the top 30):')
            for func, count in sorted(hot.items(), key=lambda kv: -kv[1])[:15]:
                print(f'    {count:6d}  {func}')
        hot.clear()

    for line in a.log.read_text(errors='replace').split('\n'):
        m = re.search(r'pc-sample: (\d+) idle, (\d+) user, (\d+) kernel', line)
        if m:
            flush()
            period += 1
            if a.period is None or a.period == period:
                print(f'period {period}: {line[line.index("pc-sample:"):]}')
            continue
        if a.period is not None and a.period != period:
            continue
        m = re.search(r'pc-sample: pc (0x[0-9a-f]+) (\d+)', line)
        if m:
            address, count = int(m.group(1), 16), int(m.group(2))
            func = names.function(address)
            hot[func] = hot.get(func, 0) + count
            print(f'  pc {m.group(1)} {count:6d}  {names.name(address)}')
            continue
        m = re.search(r'pc-sample: (task .*|\d+ waits armed.*)', line)
        if m:
            print(f'  {m.group(1)}')
            continue
        m = re.search(r'pc-sample: wait (\S+) (\d+):((?: 0x[0-9a-f]+)+)', line)
        if m:
            frames = [int(x, 16) for x in m.group(3).split()]
            print(f'  wait {m.group(1)} x{m.group(2)}: ' + ' <- '.join(names.function(f) for f in frames if f))
            continue
        m = re.search(r'(sample: pid .*)', line)
        if m:
            print(f'  {m.group(1)}')
            continue
        m = re.search(r'sample:\s+(k\d+) (0x[0-9a-f]+)', line)
        if m:
            print(f'    {m.group(1)} {names.name(int(m.group(2), 16))}')
            continue
        m = re.search(r'sample:\s+((?:pc|lr) in .*|u\d+ 0x[0-9a-f]+)', line)
        if m:
            print(f'    {m.group(1)}')
    flush()


if __name__ == '__main__':
    main()
