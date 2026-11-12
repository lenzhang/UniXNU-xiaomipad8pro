#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""The test images' still wallpaper (docs/drivers/gpu-userspace.md, "Allocation limits and the video wallpaper, 2026-11-12"): the files
tooling/k27/still-wallpaper.sh installs at boot, written here once, the same bytes every time.

    still-wallpaper.py OUTDIR      -> OUTDIR/still.png, OUTDIR/Metadata.plist, OUTDIR/Index.plist; prints the --file arguments

- still.png: the picture, one colour (#5A4A2E, a dark ochre: neither the grey of an undrawn wallpaper nor black nor the boot screen's
  blue), 3200x2136 (the panel), about 20 KB. The login window shows it from /var/db/Wallpapers/<GeneratedUID>/Wallpaper.png, the file
  macOS's login window (SecurityAgent's loginwindow plugin) reads for the account before it falls back to the default aerial video
  (macOS 26's code; macOS 27's console of run 20261112-104004: SecurityAgent "Failed to load wallpaper image for user with UUID ...",
  ImageIO "can't open '/var/db/Wallpapers/...'", then the video's decoder).
- Metadata.plist: what wallpaperexportd writes beside that picture, the choice it was exported from (an image file choice of
  /Library/Desktop Pictures/UniXNU Still.png): the wallpaper agent compares it with the account's choice at login and exports again
  when they differ.
- Index.plist: the account's wallpaper store (~/Library/Application Support/com.apple.wallpaper/Store/Index.plist) with the same choice
  for every space and display, for the desktop after the login; written only where the account has none.
The formats are those of macOS 26's own files (binary property lists); macOS 27's were not read (no system image here).
"""
import datetime
import plistlib
import struct
import sys
import zlib
from pathlib import Path

WIDTH, HEIGHT = 3200, 2136
COLOR = (0x5A, 0x4A, 0x2E)
PICTURE = 'file:///Library/Desktop%20Pictures/UniXNU%20Still.png'
# A fixed time for the store's LastSet/LastUse: the staged seed keeps its identity across rebuilds.
WHEN = datetime.datetime(2026, 11, 12, 0, 0, 0)


def png(width, height, rgb):
    """A one-colour RGB PNG (8 bits a channel), compressed whole."""
    row = b'\x00' + bytes(rgb) * width
    chunk = lambda kind, data: struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(row * height, 9)) + chunk(b'IEND', b''))


def binary(value):
    return plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=True)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    choice = {'Configuration': binary({'type': 'imageFile', 'url': {'relative': PICTURE}}), 'Files': [{'relative': PICTURE}],
              'Provider': 'com.apple.wallpaper.choice.image'}
    files = {
        'still.png': png(WIDTH, HEIGHT, COLOR),
        'Metadata.plist': binary({'Content': {'Choices': [choice], 'OptionValues': '$null', 'Shuffle': '$null',
                                              'UseAsDesktopWallpaperAndScreenSaverWallpaper': False}}),
        'Index.plist': binary({
            'AllSpacesAndDisplays': {'Linked': {'Content': {'Choices': [choice], 'EncodedOptionValues': '$null', 'Shuffle': '$null'},
                                                'LastSet': WHEN, 'LastUse': WHEN}, 'Type': 'linked'},
            'Displays': {}, 'Spaces': {},
            'SystemDefault': {'Linked': {'Content': {'Choices': [{'Configuration': b'', 'Files': [], 'Provider': 'default'}],
                                                     'EncodedOptionValues': '$null', 'Shuffle': '$null'},
                                         'LastSet': WHEN, 'LastUse': WHEN}, 'Type': 'linked'}}),
    }
    for name, data in files.items():
        path = out / name
        if not path.is_file() or path.read_bytes() != data:   # unchanged files keep their time: the stage stamps stay
            path.write_bytes(data)
        print(f'--file {path}:private/var/piano/wallpaper/{name}')


if __name__ == '__main__':
    main()
