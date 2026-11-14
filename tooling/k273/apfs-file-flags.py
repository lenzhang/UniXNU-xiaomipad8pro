#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Which files of a raw APFS container image are stored compressed, without mounting it (read only; `diskutil mount` can hang on
the build Mac).

    apfs-file-flags.py IMAGE PREFIX [--all]

Scans every 4 KiB block for file-system B-tree leaf nodes (APFS object type btree/btree-node, subtype fstree) and collects, for
directory entries whose name starts with PREFIX: the inode's BSD flags (UF_COMPRESSED 0x20), its com.apple.decmpfs header
(compression type, uncompressed size) and the size of its com.apple.ResourceFork stream (where the compressed bytes of the even
types live). Prints one line per file (--all; else the first 8 and the totals) and the totals: what the kernel reads (compressed)
and what it decompresses. Stale copy-on-write nodes are read too: fine for a freshly made image (make-systemos.sh's output).
decmpfs types: 3/4 zlib, 7/8 LZVN, 9/10 uncompressed, 11/12 LZFSE, 13/14 LZBITMAP (odd: data in the xattr, even: in the resource fork).
"""
import argparse
import mmap
import struct

BS = 4096


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('prefix')
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    prefix = a.prefix.encode()
    with open(a.image, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    names, inodes, decmpfs, rsrc = {}, {}, {}, {}
    for o in range(0, len(m) - BS + 1, BS):
        otype, subtype = struct.unpack_from('<II', m, o + 24)
        if (otype & 0xffff) not in (2, 3) or subtype != 0x0e:
            continue
        flags, _, nkeys = struct.unpack_from('<HHI', m, o + 32)
        if not flags & 2 or flags & 4 or not 0 < nkeys <= 400:   # leaf, variable-size entries
            continue
        toff, tlen = struct.unpack_from('<HH', m, o + 40)
        toc = o + 56 + toff
        keys = toc + tlen
        vend = o + BS - (40 if flags & 1 else 0)                  # a root node ends with its btree_info
        for i in range(nkeys):
            ko, kl, vo, vl = struct.unpack_from('<HHHH', m, toc + 8 * i)
            k, v = keys + ko, vend - vo
            if k + 8 > o + BS or v < o or v + vl > o + BS:
                break
            oid_type = struct.unpack_from('<Q', m, k)[0]
            kind, oid = oid_type >> 60, oid_type & 0x0fffffffffffffff
            if kind == 9 and kl >= 12 and vl >= 8:                 # directory record: name -> file id
                length = struct.unpack_from('<I', m, k + 8)[0] & 0x3ff
                name = bytes(m[k + 12:k + 12 + length]).rstrip(b'\0')
                if name.startswith(prefix):
                    names[struct.unpack_from('<Q', m, v)[0]] = name.decode(errors='replace')
            elif kind == 3 and vl >= 92:                           # inode: bsd_flags at 68, uncompressed_size at 84
                inodes[oid] = struct.unpack_from('<I', m, v + 68)[0]
            elif kind == 4 and kl >= 10:                           # xattr
                length = struct.unpack_from('<H', m, k + 8)[0]
                name = bytes(m[k + 10:k + 10 + length]).rstrip(b'\0')
                xflags, xlen = struct.unpack_from('<HH', m, v)
                if name == b'com.apple.decmpfs' and xflags & 2 and xlen >= 16:
                    magic, ctype, usize = struct.unpack_from('<4sIQ', m, v + 4)
                    if magic == b'fpmc':
                        decmpfs[oid] = (ctype, usize)
                elif name == b'com.apple.ResourceFork' and xflags & 1 and xlen >= 24:
                    rsrc[oid] = struct.unpack_from('<Q', m, v + 4 + 8)[0]
    total_u = total_c = compressed = 0
    rows = sorted(names.items(), key=lambda kv: kv[1])
    for n, (oid, name) in enumerate(rows):
        flags = inodes.get(oid)
        ctype, usize = decmpfs.get(oid, (None, 0))
        if flags is not None and flags & 0x20 and ctype is not None:
            compressed += 1
            total_u += usize
            total_c += rsrc.get(oid, 0)
        if a.all or n < 8:
            print(f'{name}: inode {oid} bsd_flags {flags:#x}' if flags is not None else f'{name}: inode {oid} (no inode record)',
                  f'compressed type {ctype}, {usize} bytes uncompressed, resource fork {rsrc.get(oid, 0)} bytes' if ctype else 'not compressed')
    print(f'{len(rows)} files named {a.prefix}*: {compressed} compressed, {total_u / 2**20:.0f} MiB uncompressed in '
          f'{total_c / 2**20:.0f} MiB of resource forks')


if __name__ == '__main__':
    main()
