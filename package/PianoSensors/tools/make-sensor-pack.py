#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
"""Packs what the ADSP's sensor protection domain reads into the one read-only file PianoSensors serves it from (SensorsPack.h), out of
this tablet's own files (docs/drivers/sensors.md section 5). PaganiSensors' scripts/make-sensor-pack.py (UPSTREAM) with piano's inputs:
the ODM tree, a soc0 allowlist, and an output that must stay out of any repository (it holds this unit's calibration: local only, under
the ignored out/ or private/, never committed, never another unit's).

  make-sensor-pack.py --dump DIR --out out/sensors/pack.bin [--libs] [--library-path 'a;b'] [--add /device/path=FILE ...] [--list]

From DIR, all optional but at least one must be there (member names are the DSP's paths; make them from the owner's own tablet):
  vendor-etc-sensors.tar   vendor/etc/sensors/...: sns_reg_config, config/*.json, hals.conf, sns_odp_config
  odm-etc-sensors.tar      odm/etc/sensors/...: config/json.lst and the ODM's sensor JSON (piano's PD reads them: Ubuntu's hexagonrpc
                           patch 0003)
  persist-sensors.tar      mnt/vendor/persist/sensors/...: the outer registry directory (registry/, sns_reg_version,
                           parsed_file_list.csv) with this unit's factory calibration (persist mounted read only, ro,noload)
  dsp-libs.tar             vendor/lib/rfsa/adsp, vendor/dsp (only with --libs: large; the service's "rpc missing ..." lines say which
                           the sensor PD really asks for: --add those one by one)
  soc0.txt                 name=value lines of /sys/devices/soc0, each served as /sys/devices/soc0/NAME; only the names Ubuntu's
                           sensors-prepare copies (SOC0_NAMES): a serial number or anything else is refused
--library-path is the sensors daemon's ADSP_LIBRARY_PATH, kept as /.pagani/library_path (the name SensorsRpc.h reads).

Archives are only read, never extracted; a member with an unsafe name is refused."""
import argparse
import struct
import subprocess
import sys
import tarfile
from pathlib import Path, PurePosixPath

SOC0_NAMES = ('soc_id', 'revision', 'machine', 'family', 'platform_subtype', 'platform_subtype_id', 'platform_version', 'hw_platform')
ARCHIVES = ('vendor-etc-sensors.tar', 'odm-etc-sensors.tar', 'persist-sensors.tar')

MAX_ENTRIES = 8192
MAX_NAMES = 0x100000
WARN_BYTES = 6 << 20


def fail(message):
    raise SystemExit('make-sensor-pack: ' + message)


def clean(name):
    """A tar member's name as an absolute device path; refuses what could escape."""
    path = PurePosixPath('/' + name.lstrip('./'))
    if '..' in path.parts or any(c < ' ' or c == '\x7f' for c in str(path)):
        fail(f'unsafe member name {name!r}')
    return str(path)


def add_tar(files, tar_path, skipped):
    with tarfile.open(tar_path) as tar:
        members = {m.name.lstrip('./'): m for m in tar.getmembers()}
        for name, member in members.items():
            target = member
            for _ in range(8):                               # a link inside the archive is served as what it points to
                if target is None or not (target.issym() or target.islnk()):
                    break
                base = PurePosixPath('/' + name).parent if target.issym() else PurePosixPath('/')
                link = target.linkname
                resolved = PurePosixPath(link) if link.startswith('/') else base / link
                parts = []
                for part in resolved.parts[1:]:
                    if part == '..':
                        parts and parts.pop()
                    elif part != '.':
                        parts.append(part)
                target = members.get('/'.join(parts))
            if target is None or target.issym() or target.islnk():
                skipped.append(f'{name} -> {member.linkname} (outside the archive)')
                continue
            if target.isfile():
                files[clean(name)] = tar.extractfile(target).read()


def add_soc0(files, path):
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, separator, value = line.partition('=')
        if not separator or '/' in name or not name:
            fail(f'{path}:{number}: not name=value')
        if name not in SOC0_NAMES:
            fail(f'{path}:{number}: {name} is not served (only {", ".join(SOC0_NAMES)}): no serial numbers in a pack')
        files['/sys/devices/soc0/' + name] = (value + '\n').encode()


def check_out(out):
    """The pack holds a unit's calibration: refuse to write it into a git work tree, except its ignored out/ and private/."""
    target = out.resolve().parent
    try:
        top = subprocess.run(['git', '-C', str(target), 'rev-parse', '--show-toplevel'], capture_output=True, text=True)
    except OSError:
        return
    if top.returncode:
        return
    inside = out.resolve().relative_to(Path(top.stdout.strip()).resolve())
    if inside.parts[0] not in ('out', 'private'):
        fail(f'{out} is inside the repository {top.stdout.strip()}: write it under out/ or private/ (ignored), never elsewhere')


def pack(files):
    """The bytes of a pack (SensorsPack.h) of {absolute path: bytes}; every parent directory listed."""
    directories = {'/'}
    for path in files:
        parent = PurePosixPath(path).parent
        while str(parent) != '/':
            directories.add(str(parent))
            parent = parent.parent
    clash = directories & set(files)
    if clash:
        fail('both a file and a directory: ' + ', '.join(sorted(clash)))
    names = sorted(directories | set(files), key=lambda text: text.encode())
    if len(names) > MAX_ENTRIES:
        fail(f'{len(names)} entries, the kernel takes {MAX_ENTRIES}')
    area, offsets = b'', {}
    for name in names:
        encoded = name.encode()
        if len(encoded) >= 256:
            fail(f'path too long: {name}')
        offsets[name] = len(area)
        area += encoded + b'\0'
    if len(area) > MAX_NAMES:
        fail(f'{len(area)} bytes of names, the kernel takes {MAX_NAMES}')
    at = 32 + 24 * len(names) + len(area)
    table, data = b'', []
    for name in names:
        if name in directories:
            table += struct.pack('<QQII', 0, 0, offsets[name], 1)
        else:
            body = files[name]
            table += struct.pack('<QQII', at, len(body), offsets[name], 0)
            data.append(body)
            at += len(body)
    head = table + area
    header = b'SSCPACK1' + struct.pack('<IIQII', len(names), len(area), at, sum(head) & 0xffffffff, 0)
    return header + head + b''.join(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dump', type=Path, required=True, help="the directory of this tablet's archives")
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--libs', action='store_true', help='also dsp-libs.tar')
    parser.add_argument('--library-path', help="the sensors daemon's ADSP_LIBRARY_PATH")
    parser.add_argument('--add', action='append', default=[], metavar='DEVICE=FILE', help='one more file, by its device path')
    parser.add_argument('--list', action='store_true', help='print what went in')
    args = parser.parse_args()
    check_out(args.out)
    files, skipped = {}, []
    wanted = list(ARCHIVES) + (['dsp-libs.tar'] if args.libs else [])
    found = [name for name in wanted if (args.dump / name).is_file()]
    for name in found:
        add_tar(files, args.dump / name, skipped)
    if (args.dump / 'soc0.txt').is_file():
        add_soc0(files, args.dump / 'soc0.txt')
    for item in args.add:
        device, separator, source = item.partition('=')
        if not separator or not device.startswith('/'):
            fail(f'--add {item}: DEVICE=FILE with an absolute device path')
        files[device] = Path(source).read_bytes()
    if args.library_path:
        files['/.pagani/library_path'] = args.library_path.encode()
    if not files:
        fail(f'nothing to pack in {args.dump}')
    for needed in ('/vendor/etc/sensors/sns_reg_config', '/odm/etc/sensors/config/json.lst'):
        if needed not in files:
            print(f'make-sensor-pack: warning: no {needed}: the sensor PD will not find its registry configuration', file=sys.stderr)
    if not any(name.startswith('/mnt/vendor/persist/sensors/registry/') for name in files):
        print('make-sensor-pack: warning: no persist registry: the DSP falls back to the stock JSON defaults (generic calibration)',
              file=sys.stderr)
    blob = pack(files)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    args.out.chmod(0o600)
    if args.list:
        for name in sorted(files):
            print(f'{len(files[name]):9} {name}')
    for line in skipped:
        print('make-sensor-pack: skipped ' + line, file=sys.stderr)
    if len(blob) > WARN_BYTES:
        print(f'make-sensor-pack: warning: {len(blob)} bytes: the service loads the pack into memory (SensorsRpc reads it through SSR_BOUNCE): keep it small', file=sys.stderr)
    print(f'{args.out}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
