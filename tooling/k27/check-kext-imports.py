#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""List every import of a native kext that the k27 kernel does not define, before the KC link (which stops at the first one).

    check-kext-imports.py OBJECTS.json KERNEL.macho [--library-prefix PREFIX ...]

A symbol is resolved by the KC builder (macOS-DroidVM scripts/kc-build.py resolve()) from the kernel's symbol table (any section
symbol, local or external, if its name is unique), the exports of the kept kexts the descriptor lists as libraries, and the natives
linked before. This check covers the kernel and the objects themselves; imports of IOGraphicsFamily's classes (IOAccelerator, the
default --library-prefix) are listed as library imports for the KC link to resolve. Exit 1 when anything else is missing.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

N_STAB, N_TYPE, N_EXT, N_SECT, N_UNDF = 0xe0, 0x0e, 0x01, 0x0e, 0x00


def symbols(data):
    magic, cpu, _, _, count, _, _, _ = struct.unpack_from('<8I', data)
    if magic != 0xfeedfacf:
        raise SystemExit('check-kext-imports: not a 64-bit Mach-O')
    offset = 32
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 2:
            symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, offset + 8)
            strings = data[stroff:stroff + strsize]
            for i in range(nsyms):
                strx, n_type, n_sect, n_desc, n_value = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
                yield strings[strx:strings.index(b'\0', strx)].decode(errors='replace'), n_type, n_value
            return
        offset += size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('objects', type=Path)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('--library-prefix', action='append', default=['__ZN13IOAccelerator', '__ZTV13IOAccelerator',
                                                                      '__ZNK13IOAccelerator', '__ZN13IOAccelerator10gMetaClassE'])
    args = parser.parse_args()
    kernel = set()
    for name, n_type, _ in symbols(args.kernel.read_bytes()):
        if not n_type & N_STAB and n_type & N_TYPE == N_SECT:
            kernel.add(name)
    defined, undefined = set(), {}
    for path in json.loads(args.objects.read_text()):
        for name, n_type, n_value in symbols(Path(path).read_bytes()):
            if n_type & N_STAB:
                continue
            if n_type & N_TYPE == N_SECT and n_type & N_EXT:
                defined.add(name)
            elif n_type & N_TYPE == N_UNDF and n_type & N_EXT and not n_value:
                undefined.setdefault(name, set()).add(Path(path).name)
    missing = sorted(n for n in undefined if n not in defined and n not in kernel)
    library = [n for n in missing if any(n.startswith(p) for p in args.library_prefix)]
    missing = [n for n in missing if n not in library]
    from_kernel = sum(1 for n in undefined if n not in defined and n in kernel)
    print(f'check-kext-imports: {from_kernel} imports from the kernel, {len(library)} from the libraries '
          f'({", ".join(library) if library else "none"}), {len(missing)} unresolved')
    for name in missing:
        print(f'  unresolved {name} (in {", ".join(sorted(undefined[name]))})')
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
