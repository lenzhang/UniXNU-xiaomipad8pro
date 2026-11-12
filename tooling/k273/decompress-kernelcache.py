#!/usr/bin/env python3
"""The 27.3 line's KC base: kernelcache.release.mac17j (IM4P, LZFSE) -> the arm64e MH_FILESET Mach-O, checked against the pin.

  decompress-kernelcache.py IN.im4p OUT.bin [--sha256 PIN]

The pin is the UniXNU macOS 27.3 profile's [kernel.prelink] kernelcache_sha256 (the decompressed Mach-O). Same steps as UniXNU's
scripts/unixnu/ipsw.py kernelcache(): the IM4P's payload, macOS compression_tool (LZFSE), the arm64 slice of a fat file.
"""
import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PIN = 'd1003b5143ffd88439b4d45b6017f587171cfe6855660bed05370a2ea5db9fe9'


def der(data, at):
    tag, length = data[at], data[at + 1]
    at += 2
    if length & 0x80:
        count = length & 0x7f
        length = int.from_bytes(data[at:at + count], 'big')
        at += count
    return tag, at, length


def im4p_payload(data):
    tag, start, length = der(data, 0)
    if tag != 0x30:
        sys.exit('not an IM4P')
    at, items = start, []
    while at < start + length:
        _, body, size = der(data, at)
        items.append(data[body:body + size])
        at = body + size
    if len(items) < 4 or items[0] != b'IM4P':
        sys.exit('not an IM4P')
    return items[3]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('input', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--sha256', default=PIN)
    a = p.parse_args()
    payload = im4p_payload(a.input.read_bytes())
    with tempfile.TemporaryDirectory() as tmp:
        packed, raw = Path(tmp) / 'kc.lzfse', Path(tmp) / 'kc.raw'
        packed.write_bytes(payload)
        subprocess.run(['/usr/bin/compression_tool', '-decode', '-a', 'lzfse', '-i', packed, '-o', raw], check=True)
        data = raw.read_bytes()
    if data[:4] == b'\xca\xfe\xba\xbe':
        count = struct.unpack('>I', data[4:8])[0]
        slices = [struct.unpack('>5I', data[8 + 20 * i:28 + 20 * i]) for i in range(count)]
        arm64 = [s for s in slices if s[0] == 0x0100000c]
        if len(arm64) != 1:
            sys.exit('expected one arm64 slice')
        data = data[arm64[0][2]:arm64[0][2] + arm64[0][3]]
    if data[:4] != b'\xcf\xfa\xed\xfe':
        sys.exit('not a 64-bit Mach-O')
    digest = hashlib.sha256(data).hexdigest()
    if a.sha256 and digest != a.sha256:
        sys.exit(f'sha256 {digest} differs from the pin {a.sha256}')
    a.output.write_bytes(data)
    kind, = struct.unpack_from('<I', data, 12)
    print(f'{a.output}: {len(data)} bytes, filetype {kind} ({"MH_FILESET" if kind == 12 else "?"}), sha256 {digest}')


if __name__ == '__main__':
    main()
