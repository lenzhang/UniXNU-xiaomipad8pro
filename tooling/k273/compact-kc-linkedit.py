#!/usr/bin/env python3
"""A kernel collection's shared __LINKEDIT reduced to what its entries use.

  compact-kc-linkedit.py IN.bin OUT.bin

The KC builder keeps the base collection's whole __LINKEDIT and appends the kernel's symbols and the chained fixups. 27.3's mac17j
collection keeps full symbol tables for all of its 370 kexts (one 17.5 MB string pool), of which ours keeps 48: about 25 MB of the
built collection is the symbols of kexts it dropped, and the RAM image has to stay under the UEFI's 256 MiB kernel region
(tooling/bootimg/build-ram-images.py IMAGE_MAX_BYTES; docs/k273-line.md §4).

The new __LINKEDIT starts where the entries' __LINKEDIT starts (after the __LINKINFO segments, which stay where they are) and holds,
per entry: its symbol table (nlists), the strings those name (one new pool for the Apple kexts, which share one; the kernel keeps
its own table), its indirect symbol table and its function starts; then the top-level chained fixups. Every LC_SYMTAB,
LC_DYSYMTAB, LC_FUNCTION_STARTS, LC_DYLD_CHAINED_FIXUPS and __LINKEDIT segment command is rewritten for the new offsets; symbols
keep their values, types, sections and order (indirect tables and dysymtab ranges index them). Nothing outside __LINKEDIT moves.
The file is a flat image (file offset = vmaddr - base), so it simply ends earlier. Checked after writing: every entry's symbols read
back with the same names and values as in IN.
"""
import struct
import sys
from pathlib import Path

LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB, LC_FUNCTION_STARTS, LC_DATA_IN_CODE = 0x19, 0x2, 0xb, 0x26, 0x29
LC_FILESET_ENTRY, LC_CHAINED_FIXUPS = 0x80000035, 0x80000034
PAGE = 0x4000


def up(v, a=PAGE):
    return (v + a - 1) // a * a


def commands(data, at):
    count, = struct.unpack_from('<I', data, at + 16)
    off = at + 32
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, off)
        yield cmd, off, size
        off += size


def segment_name(data, off):
    return data[off + 8:off + 24].split(b'\0')[0].decode()


def symbols(data, symoff, nsyms, stroff):
    out = []
    for i in range(nsyms):
        strx, kind, sect, desc, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
        end = data.index(b'\0', stroff + strx)
        out.append((bytes(data[stroff + strx:end]), kind, sect, desc, value))
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src = Path(sys.argv[1]).read_bytes()
    data = bytearray(src)
    top = {segment_name(data, off): off for cmd, off, size in commands(data, 0) if cmd == LC_SEGMENT_64}
    base, = struct.unpack_from('<Q', data, top['__TEXT'] + 24)
    le_va, le_vs, le_fo, le_fs = struct.unpack_from('<4Q', data, top['__LINKEDIT'] + 24)
    entries = []
    for cmd, off, size in commands(data, 0):
        if cmd == LC_FILESET_ENTRY:
            va, fo, so = struct.unpack_from('<QQI', data, off + 8)
            entries.append((data[off + so:off + size].split(b'\0')[0].decode(), fo))
    # where the entries' __LINKEDIT begins (one shared range): the new data goes there
    starts = set()
    for name, fo in entries:
        for cmd, off, size in commands(data, fo):
            if cmd == LC_SEGMENT_64 and segment_name(data, off) == '__LINKEDIT':
                starts.add(struct.unpack_from('<Q', data, off + 40)[0])
    if len(starts) != 1:
        sys.exit(f'the entries do not share one __LINKEDIT start: {sorted(hex(s) for s in starts)}')
    start = starts.pop()
    if not le_fo <= start < le_fo + le_fs:
        sys.exit('the entries\' __LINKEDIT is not inside the collection\'s')
    out = bytearray(data[:start])
    pools = {}          # (stroff, strsize) of a shared pool -> (new pool bytearray, {string: offset}, [patch sites])
    patches = []        # (command offset, field offset, value) applied after the layout
    before = {}

    def append(blob, align=8):
        out.extend(bytes(up(len(out), align) - len(out)))
        at = len(out)
        out.extend(blob)
        return at

    shared = {}
    for name, fo in entries:
        for cmd, off, size in commands(data, fo):
            if cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, off + 8)
                shared.setdefault((stroff, strsize), []).append(name)
    for name, fo in entries:
        for cmd, off, size in commands(data, fo):
            if cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, off + 8)
                if nsyms == 0:
                    patches.append((off, 8, 'start'))
                    patches.append((off, 16, 'start'))
                    continue
                syms = symbols(data, symoff, nsyms, stroff)
                before[name] = [(s[0], s[4]) for s in syms]
                key = (stroff, strsize)
                if len(shared[key]) == 1:     # its own string table (the kernel's): copied whole
                    new_str = append(data[stroff:stroff + strsize])
                    table = bytearray(data[symoff:symoff + 16 * nsyms])
                    new_sym = append(table)
                    patches.append((off, 8, new_sym))
                    patches.append((off, 16, new_str))
                    continue
                pool, index, users = pools.setdefault(key, (bytearray(b'\0'), {b'': 0}, []))
                table = bytearray(16 * nsyms)
                for i, (string, kind, sect, desc, value) in enumerate(syms):
                    if string not in index:
                        index[string] = len(pool)
                        pool.extend(string + b'\0')
                    struct.pack_into('<IBBHQ', table, 16 * i, index[string], kind, sect, desc, value)
                new_sym = append(table)
                patches.append((off, 8, new_sym))
                users.append(off)
            elif cmd == LC_DYSYMTAB:
                fields = list(struct.unpack_from('<18I', data, off + 8))
                # tocoff/ntoc, modtaboff/nmodtab, extrefsymoff/nextrefsyms, extreloff/nextrel, locreloff/nlocrel must be empty
                if any(fields[i] for i in (7, 9, 11, 15, 17)):
                    sys.exit(f'{name}: LC_DYSYMTAB has tables this tool does not carry: {fields}')
                indirectoff, nindirect = fields[12], fields[13]
                if nindirect:
                    patches.append((off, 8 + 12 * 4, append(data[indirectoff:indirectoff + 4 * nindirect], 4)))
                else:
                    patches.append((off, 8 + 12 * 4, 0))
            elif cmd in (LC_FUNCTION_STARTS, LC_DATA_IN_CODE):
                dataoff, datasize = struct.unpack_from('<II', data, off + 8)
                patches.append((off, 8, append(data[dataoff:dataoff + datasize]) if datasize else 0))
    for key, (pool, index, users) in pools.items():
        at = append(pool)
        for off in users:
            patches.append((off, 16, at))
            patches.append((off, 20, len(pool)))
    for cmd, off, size in commands(data, 0):
        if cmd == LC_CHAINED_FIXUPS:
            dataoff, datasize = struct.unpack_from('<II', data, off + 8)
            patches.append((off, 8, append(data[dataoff:dataoff + datasize])))
    end = up(len(out))
    out.extend(bytes(end - len(out)))
    # offsets and extents
    for off, field, value in patches:
        struct.pack_into('<I', out, off + field, start if value == 'start' else value)
    for name, fo in entries:
        for cmd, off, size in commands(out, fo):
            if cmd == LC_SEGMENT_64 and segment_name(out, off) == '__LINKEDIT':
                struct.pack_into('<4Q', out, off + 24, base + start, end - start, start, end - start)
    struct.pack_into('<4Q', out, top['__LINKEDIT'] + 24, le_va, end - le_fo, le_fo, end - le_fo)
    Path(sys.argv[2]).write_bytes(out)
    # check: the same symbols, names and values, per entry
    for name, fo in entries:
        for cmd, off, size in commands(out, fo):
            if cmd == LC_SYMTAB and name in before:
                symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', out, off + 8)
                after = [(s[0], s[4]) for s in symbols(out, symoff, nsyms, stroff)]
                if after != before[name]:
                    sys.exit(f'{name}: symbols differ after compaction')
    print(f'{sys.argv[2]}: {len(out):#x} bytes (was {len(src):#x}); __LINKEDIT {le_fs:#x} -> {end - le_fo:#x}; '
          f'{len(before)} symbol tables checked')


if __name__ == '__main__':
    main()
