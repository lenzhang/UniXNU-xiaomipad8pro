#!/usr/bin/env python3
"""Build the SHNGROOT root manifest (UniXNU userdata.py layout, read by NativeUfs findRootLayout) for raw disk images that
will be written into a partition at 1 MiB-aligned windows. Runs anywhere; hashing 31 GB takes a few minutes.
   make-root-manifest.py OUT_DIR NAME=IMAGE [NAME=IMAGE ...]
Writes OUT_DIR/manifest.bin (4096 bytes) and OUT_DIR/layout.json {name, offset, size, sha256}; the writer then puts the
manifest at byte 0 of the partition and each image at its offset (dd ... seek=offset/1MiB bs=1M conv=notrunc)."""
import hashlib, json, os, struct, sys
MIB = 1 << 20
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
entries, cursor = [], MIB
for spec in sys.argv[2:]:
    name, _, path = spec.partition('=')
    size = os.path.getsize(path)
    if size % 512 or len(name.encode()) > 31:
        sys.exit(f'{spec}: size must be a multiple of 512 and the name under 32 bytes')
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(8 * MIB), b''):
            h.update(chunk)
    entries.append({'name': name, 'image': path, 'offset': cursor, 'size': size, 'sha256': h.hexdigest()})
    print(f'{name}: {size} bytes at {cursor:#x} sha256 {h.hexdigest()}', flush=True)
    cursor += (size + MIB - 1) // MIB * MIB
header = bytearray(4096)
header[0:8] = b'SHNGROOT'
struct.pack_into('<IIQ', header, 8, 1, len(entries), cursor)
for i, e in enumerate(entries):
    struct.pack_into('<32sQQII32s40s', header, 24 + 128 * i, e['name'].encode(), e['offset'], e['size'], 512, 1,
                     bytes.fromhex(e['sha256']), bytes(40))
header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
open(os.path.join(out, 'manifest.bin'), 'wb').write(header)
json.dump({'image_bytes': cursor, 'windows': entries}, open(os.path.join(out, 'layout.json'), 'w'), indent=2)
print('image_bytes', cursor, 'manifest', hashlib.sha256(bytes(header)).hexdigest())
