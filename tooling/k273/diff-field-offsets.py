#!/usr/bin/env python3
"""Field offsets of a kext's classes in one collection, mapped to another, from the kext's own code.

  diff-field-offsets.py REFERENCE_KC TARGET_KC KEXT [--offsets N,N,...] [--functions REGEX]

A native kext that reads a family's instance fields by offset (NativeGopDisplay: IOMobileFramebufferLegacy, IOMFBSwapIORequest) was
written against the offsets in one collection's code. This pairs every function of KEXT that both collections define under the same
symbol, instruction by instruction (only functions with the same instruction count), and where a word differs and both are the
same load/store/add-immediate form with the same registers, records the reference immediate -> the target immediate. The table
says, for each reference offset, which target offsets the same instructions use (and how often), so offsets read from the reference
code can be taken over: an offset seen with one target value everywhere is unambiguous. --offsets prints only those.
Behaviour/offsets only: no Apple code is copied or kept. Reads the k27 tree's fileset_kc (PIANO_K27_TREE).
"""
import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'k27'))
from k27_tree import k27_tree  # noqa: E402

sys.path.insert(0, str(k27_tree() / 'scripts'))
import fileset_kc  # noqa: E402


def decode(word):
    """(form, registers, immediate) of the immediate load/store/add forms, else None."""
    if word & 0x3b000000 == 0x39000000:                       # LDR/STR (unsigned offset), GPR and SIMD
        size = word >> 30
        if word & (1 << 26) and (word >> 23) & 1:             # 128-bit SIMD (opc bit 1 with V)
            size = 4
        return ('ldst', word & 0xffc003ff, ((word >> 10) & 0xfff) << size)
    if word & 0x3b200c00 == 0x38000000:                       # LDUR/STUR (unscaled)
        imm = (word >> 12) & 0x1ff
        return ('ldur', word & 0xffe00fff, imm - 512 if imm & 256 else imm)
    if word & 0x3a000000 == 0x28000000 and (word >> 23) & 3 == 2:   # LDP/STP (signed offset)
        opc = word >> 30
        scale = 2 + (opc >> 1) if not word & (1 << 26) else 2 + opc
        imm = (word >> 15) & 0x7f
        return ('ldp', word & 0xffc07fff, (imm - 128 if imm & 64 else imm) << scale)
    if word & 0x1f000000 == 0x11000000 and not word & (1 << 23):    # ADD/SUB (immediate)
        imm = ((word >> 10) & 0xfff) << (12 if word & (1 << 22) else 0)
        return ('add', word & 0xff8003ff, imm)
    return None


def functions(kc, kext):
    macho = kc.macho(kext)
    text = next(s for s in macho['segments'] if s['name'] == '__TEXT_EXEC')
    syms = sorted((v, n) for n, k, s, d, v in kc.symbols(macho) if v and s and text['va'] <= v < text['va'] + text['size'])
    out = {}
    for (va, name), nxt in zip(syms, syms[1:] + [(text['va'] + text['size'], None)]):
        end = nxt[0]
        if end > va and name not in out:
            at = kc.file_offset(va)
            out[name] = [int.from_bytes(kc.data[at + i:at + i + 4], 'little') for i in range(0, end - va, 4)]
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('reference', type=Path)
    p.add_argument('target', type=Path)
    p.add_argument('kext')
    p.add_argument('--offsets')
    p.add_argument('--functions', default='.')
    a = p.parse_args()
    ref = functions(fileset_kc.Kernelcache(a.reference.read_bytes()), a.kext)
    tgt = functions(fileset_kc.Kernelcache(a.target.read_bytes()), a.kext)
    pick = re.compile(a.functions)
    pairs = defaultdict(Counter)       # (form, reference imm) -> Counter(target imm)
    where = defaultdict(set)
    same_len = skipped = 0
    for name, words in ref.items():
        other = tgt.get(name)
        if other is None or not pick.search(name):
            continue
        if len(other) != len(words):
            skipped += 1
            continue
        same_len += 1
        for x, y in zip(words, other):
            if x == y:
                dx = decode(x)
                if dx:
                    pairs[dx[2]][dx[2]] += 1
                continue
            dx, dy = decode(x), decode(y)
            if dx and dy and dx[0] == dy[0] and dx[1] == dy[1]:
                pairs[dx[2]][dy[2]] += 1
                where[(dx[2], dy[2])].add(name)
    print(f'{same_len} functions paired, {skipped} with another length left out')
    wanted = [int(x, 0) for x in a.offsets.split(',')] if a.offsets else sorted(k for k, c in pairs.items() if set(c) != {k})
    for off in wanted:
        c = pairs.get(off)
        if not c:
            print(f'{off:6d}: not seen in paired code')
            continue
        moved = {k: v for k, v in c.items() if k != off}
        verdict = 'same' if not moved else ('moved' if off not in c and len(c) == 1 else 'AMBIGUOUS')
        detail = ', '.join(f'{k}x{v}' for k, v in c.most_common(6))
        print(f'{off:6d} -> {detail}  [{verdict}]' + (f'  e.g. {sorted(where[(off, next(iter(moved)))])[:2]}' if moved else ''))


if __name__ == '__main__':
    main()
