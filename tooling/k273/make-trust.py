#!/usr/bin/env python3
"""The 27.3 line's trust bundle (DarwinBoot's kernel.macho.trustcache sidecar: u32 count, u32 offsets[count], modules).

  make-trust.py BASE_BUNDLE STATIC.trustcache SYSTEM.trustcache APP.trustcache OUTPUT

BASE_BUNDLE is the 27.0.1 line's bundle (out/inputs/kernel.macho.trustcache): its modules before the last three (the 27.0.1 IPSW's
static, System and App trust caches, as the k27 tree's scripts/make-trust-bundle.py put them last) are this repository's own
staged code and stay first; the 27.3 IPSW's three trust caches (IM4P members, tooling/k273/ipsw-members.py) take the place of
27.0.1's. Each module is checked to be a complete version-2 trust cache. The staging tools (tooling/k27/stage-soft-metal.py) put
their own module in front of these, as for the 27.0.1 line.
"""
import hashlib
import struct
import sys
from pathlib import Path


def der(data, at):
    tag, size = data[at], data[at + 1]
    at += 2
    if size & 128:
        count = size & 127
        size = int.from_bytes(data[at:at + count], 'big')
        at += count
    return tag, data[at:at + size], at + size


def im4p_module(path):
    data = Path(path).read_bytes()
    tag, sequence, end = der(data, 0)
    fields, cursor = [], 0
    while cursor < len(sequence):
        kind, value, cursor = der(sequence, cursor)
        fields.append((kind, value))
    if tag != 0x30 or fields[0][1] != b'IM4P' or len(fields) < 4:
        sys.exit(f'{path}: not an IM4P')
    return check(fields[3][1], path)


def check(module, what):
    if struct.unpack_from('<I', module)[0] != 2 or len(module) != 24 + struct.unpack_from("<I", module, 20)[0] * 24:
        sys.exit(f'{what}: not a complete version-2 trust cache')
    return module


def modules_of(bundle):
    count, = struct.unpack_from('<I', bundle)
    offsets = list(struct.unpack_from(f'<{count}I', bundle, 4)) + [len(bundle)]
    return [check(bundle[offsets[i]:offsets[i + 1]], f'module {i}') for i in range(count)]


def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    base = modules_of(Path(sys.argv[1]).read_bytes())
    if len(base) < 3:
        sys.exit('the base bundle has fewer than three modules')
    own = base[:-3]
    ipsw = [im4p_module(p) for p in sys.argv[2:5]]
    modules = own + ipsw
    offsets, cursor = [], 4 + 4 * len(modules)
    for module in modules:
        offsets.append(cursor)
        cursor += len(module)
    bundle = struct.pack(f'<{len(modules) + 1}I', len(modules), *offsets) + b''.join(modules)
    Path(sys.argv[5]).write_bytes(bundle)
    entries = [struct.unpack_from('<I', m, 20)[0] for m in modules]
    print(f'{sys.argv[5]}: {len(modules)} modules ({len(own)} own, entries {entries}), sha256 {hashlib.sha256(bundle).hexdigest()}')


if __name__ == '__main__':
    main()
