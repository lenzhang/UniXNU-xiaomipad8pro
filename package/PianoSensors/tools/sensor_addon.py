#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
"""The sensor pack as a RAM test image carries it (docs/drivers/sensors.md section 5b): one entry, sensors/piano-sensors.pack, in
UniXNU's boot addon container (scripts/make-gpu-addon.py's layout, which DarwinBoot hands the kernel as /chosen/memory-map "GpuAddon"
when the payload has Payloads/kernel.macho.gpuaddon and the ADT has that slot). PianoSensors (SensorsAddon.h) takes the pack from it
before it looks at the ESP. tooling/bootimg/build-ram-images.py imports addon_bytes() and check_pack() by path (PIANO_SENSOR_PACK).

  sensor_addon.py PACK OUT     write the addon to OUT (only under this repository's ignored out/ or private/, mode 0600)

Layout, little endian: "NGPUADD2", u32 count (1), u32 0, u64 total bytes; the entry: u32 path bytes (with its NUL), u32 mode (0600),
u64 size, u64 compressed size, the path padded to 8 bytes, the zlib stream padded to 8 bytes. The pack holds this unit's calibration:
the addon and every image carrying it are as private as the pack. Nothing of its contents is printed."""
import hashlib
import struct
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]     # package/PianoSensors/tools/ -> the checkout
MAGIC = b'NGPUADD2'
PACK_PATH = 'sensors/piano-sensors.pack'       # SensorsAddon.h SA_PACK_PATH
MAX_PACK = 4 << 20                             # SensorsLoad.h SL_MAX_BYTES


def check_pack(data):
    """A sensor pack as PianoSensors' loader takes it (SensorsPack.h): "SSCPACK1", its own size at +16, at most 4 MiB. ValueError if not."""
    if len(data) < 32 or data[:8] != b'SSCPACK1' or int.from_bytes(data[16:24], 'little') != len(data):
        raise ValueError('not a sensor pack (package/PianoSensors/tools/make-sensor-pack.py)')
    if len(data) > MAX_PACK:
        raise ValueError(f'{len(data)} bytes; PianoSensors takes at most 4 MiB')


def pad(data):
    return data + bytes(-len(data) % 8)


def addon_bytes(pack, path=PACK_PATH):
    """The addon carrying pack (checked first) as its one entry."""
    check_pack(pack)
    name = path.encode() + b'\0'
    stream = zlib.compress(pack, 9)
    entry = struct.pack('<IIQQ', len(name), 0o600, len(pack), len(stream)) + pad(name) + pad(stream)
    return MAGIC + struct.pack('<IIQ', 1, 0, 24 + len(entry)) + entry


def check_out(out):
    """Only this repository's ignored out/ or private/ may receive it (it is as private as the pack)."""
    target = out.resolve()
    for allowed in (REPO / 'out', REPO / 'private'):
        if target.is_relative_to(allowed.resolve()):
            return
    raise SystemExit(f'{out}: the addon goes only under {REPO / "out"} or {REPO / "private"} (ignored by git), never elsewhere')


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    pack, out = Path(sys.argv[1]), Path(sys.argv[2])
    check_out(out)
    data = pack.read_bytes()
    try:
        blob = addon_bytes(data)
    except ValueError as error:
        raise SystemExit(f'{pack}: {error}')
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    out.chmod(0o600)
    print(f'{out}: {len(blob)} bytes, the pack ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()[:16]}) as {PACK_PATH}')


if __name__ == '__main__':
    main()
