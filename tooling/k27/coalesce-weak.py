#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Coalesce weak definitions across a native kext's objects, as a static link with ld would, before the k27 KC links them.

    coalesce-weak.py IN/objects.json OUT_DIR [--report]

The KC's native linker (macOS-DroidVM scripts/kc_native.py, read only here) knows no weak definitions: every object that defines a
symbol resolves its own references to its own copy, and other objects get whichever definition came last. A driver written for a
static link with weak defaults (Adreno830: adreno830_profile_value in ten translation units, the publish shell's weak
adreno830_dcvs_submit next to A830Dcvs's strong one, the status hooks, the firmware images) would then run with split state and
dead hooks. This pass keeps one definition per symbol, as ld does: the strong one (two strong definitions are an error), else the
first weak one in objects.json order, and turns every other definition into an undefined external reference (n_type N_EXT, no
section, no value, no N_WEAK_DEF), so all references go to the kept one. The bytes of the dropped copies stay in their sections,
unused. Only the symbol table entries change; the objects are written to OUT_DIR with a new objects.json (same order).
--report lists what was coalesced (also written to OUT_DIR/coalesce-report.json).
"""
import argparse
import json
import struct
import sys
from pathlib import Path

N_STAB, N_PEXT, N_TYPE, N_EXT, N_SECT, N_WEAK_DEF = 0xe0, 0x10, 0x0e, 0x01, 0x0e, 0x0080


def symtab(data):
    magic, cpu, _, kind, count, _, _, _ = struct.unpack_from('<8I', data)
    if (magic, cpu, kind) != (0xfeedfacf, 0x100000c, 1):
        raise SystemExit('coalesce-weak: not an arm64 MH_OBJECT')
    offset = 32
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 2:
            symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, offset + 8)
            return symoff, nsyms, stroff, strsize
        offset += size
    return None


def entries(data):
    table = symtab(data)
    if not table:
        return
    symoff, nsyms, stroff, strsize = table
    strings = bytes(data[stroff:stroff + strsize])
    for i in range(nsyms):
        at = symoff + 16 * i
        strx, n_type, n_sect, n_desc, n_value = struct.unpack_from('<IBBHQ', data, at)
        yield at, strings[strx:strings.index(b'\0', strx)].decode(), n_type, n_sect, n_desc, n_value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('objects', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--report', action='store_true')
    args = parser.parse_args()
    paths = [Path(p) for p in json.loads(args.objects.read_text())]
    datas = [bytearray(p.read_bytes()) for p in paths]
    definitions = {}   # name -> [(object index, nlist offset, weak)]
    for index, data in enumerate(datas):
        for at, name, n_type, _, n_desc, _ in entries(data):
            if n_type & N_STAB or not n_type & N_EXT or n_type & N_TYPE != N_SECT:
                continue
            definitions.setdefault(name, []).append((index, at, bool(n_desc & N_WEAK_DEF)))
    report, problems = {}, []
    for name, where in definitions.items():
        if len(where) < 2:
            continue
        strong = [w for w in where if not w[2]]
        if len(strong) > 1:
            problems.append(f'{name}: strong in {", ".join(paths[w[0]].name for w in strong)}')
            continue
        keep = strong[0] if strong else where[0]
        dropped = []
        for index, at, _ in where:
            if (index, at) == keep[:2]:
                continue
            strx = struct.unpack_from('<I', datas[index], at)[0]
            struct.pack_into('<IBBHQ', datas[index], at, strx, N_EXT, 0, 0, 0)
            dropped.append(paths[index].name)
        report[name] = {'kept': paths[keep[0]].name, 'kind': 'weak' if keep[2] else 'strong', 'dropped': dropped}
    if problems:
        raise SystemExit('coalesce-weak: duplicate strong definitions:\n  ' + '\n  '.join(problems))
    args.out.mkdir(parents=True, exist_ok=True)
    written = []
    for path, data in zip(paths, datas):
        target = args.out / path.name
        if target.resolve() == path.resolve():
            raise SystemExit(f'coalesce-weak: {path} would be overwritten in place')
        target.write_bytes(data)
        written.append(str(target.resolve()))
    if len(set(written)) != len(written):
        raise SystemExit('coalesce-weak: two objects share a file name')
    (args.out / 'objects.json').write_text(json.dumps(written, indent=1) + '\n')
    (args.out / 'coalesce-report.json').write_text(json.dumps(report, indent=1, sort_keys=True) + '\n')
    if args.report:
        for name, entry in sorted(report.items()):
            print(f'  {name}: kept {entry["kind"]} in {entry["kept"]}, dropped {len(entry["dropped"])}')
    print(f'coalesce-weak: {len(report)} symbols coalesced over {len(paths)} objects -> {args.out}/objects.json')


if __name__ == '__main__':
    sys.exit(main())
