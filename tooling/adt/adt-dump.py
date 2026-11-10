#!/usr/bin/env python3
"""Print an Apple device tree (ADT, the flattened form DarwinBoot hands to XNU) as sorted text lines, for diffs.

  adt-dump.py FILE [--raw]     one line per property: /path/node  name = value
Values: printable strings as "...", 4/8-byte values as hex integers, the rest as hex bytes (long ones shortened with a hash).
Volatile properties (random-seed, unique-chip-id seed) are masked unless --raw.
"""
import hashlib, struct, sys

VOLATILE = {'random-seed', 'unique-chip-id'}

def parse(data, offset=0):
    nprops, nchildren = struct.unpack_from('<II', data, offset)
    offset += 8
    props = []
    for _ in range(nprops):
        name = data[offset:offset + 32].split(b'\0', 1)[0].decode(errors='replace')
        size = struct.unpack_from('<I', data, offset + 32)[0] & 0x7fffffff
        value = data[offset + 36:offset + 36 + size]
        offset += 36 + ((size + 3) & ~3)
        props.append((name, value))
    children = []
    for _ in range(nchildren):
        child, offset = parse(data, offset)
        children.append(child)
    return (props, children), offset

def fmt(name, value, raw):
    if name in VOLATILE and not raw:
        return f'<{len(value)} bytes, masked>'
    text = value.rstrip(b'\0')
    if text and all(32 <= c < 127 or c == 0 for c in text) and value.endswith(b'\0') and b'\0\0' not in text:
        return '"' + '" "'.join(part.decode() for part in text.split(b'\0')) + '"'
    if len(value) in (4, 8):
        return hex(int.from_bytes(value, 'little'))
    if len(value) > 48:
        return f'<{len(value)} bytes sha256:{hashlib.sha256(value).hexdigest()[:16]}>'
    return value.hex() if value else '<empty>'

def walk(node, path, raw, out):
    """path: None for the root (printed as /), else the parent's path."""
    props, children = node
    name = dict(props).get('name', b'?').rstrip(b'\0').decode()
    here = '/' if path is None else path.rstrip('/') + '/' + name
    for key, value in props:
        if key != 'name':
            out.append(f'{here}  {key} = {fmt(key, value, raw)}')
    for child in children:
        walk(child, here, raw, out)

if __name__ == '__main__':
    raw = '--raw' in sys.argv
    tree, _ = parse(open(sys.argv[1], 'rb').read())
    lines = []
    walk(tree, None, raw, lines)
    print('\n'.join(sorted(lines)))
