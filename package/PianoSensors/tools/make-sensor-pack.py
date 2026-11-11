#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
"""Packs what the ADSP's sensor protection domain reads into the one read-only file PianoSensors serves it from (SensorsPack.h), out of
this tablet's own files (docs/drivers/sensors.md section 5b). PaganiSensors' scripts/make-sensor-pack.py (UPSTREAM) with piano's inputs:
the owner's registry backup, the stock ROM's sensor configuration with its manifest, the ODM tree, a soc0 allowlist, and an output that
must stay in this repository's ignored out/ or private/ (the pack holds this unit's calibration: local only, never committed, never
another unit's).

  make-sensor-pack.py [--registry-tar TAR] [--rom-config DIR] [--dump DIR] [--soc0 FILE] --out out/sensors/pack.bin
                      [--libs] [--library-path 'a;b'] [--add /device/path=FILE ...] [--list]

Sources, all optional but at least one must give something (later ones replace a path an earlier one gave):
  --registry-tar TAR   a backup of this unit's persist registry: the archive holds one outer registry directory (the one with registry/,
                       sns_reg_version and parsed_file_list.csv) under any prefix (piano's backup: usr/share/piano/sensors/registry-private/,
                       Ubuntu's private seed); served as /mnt/vendor/persist/sensors/registry/. Its file names and contents are never
                       printed, only their count.
  --rom-config DIR     the stock ROM's sensor configuration with its manifest DIR/source.json ({"stock_build", "files": [{"path",
                       "source", "sha256"}]}, as xiaomipad-8pro-mainline's firmware/sensors): every file is checked against its sha256 and
                       served at /<source> (only under /vendor/etc/sensors/ and /odm/etc/sensors/).
  --dump DIR           archives whose member names are the DSP's paths (made on a tablet that runs Linux; docs/drivers/sensors.md 5b):
                       vendor-etc-sensors.tar, odm-etc-sensors.tar, persist-sensors.tar, dsp-libs.tar (only with --libs), soc0.txt
  --soc0 FILE          name=value lines of /sys/devices/soc0 (also DIR/soc0.txt of --dump), each served as /sys/devices/soc0/NAME; only
                       the names Ubuntu's sensors-prepare copies (SOC0_NAMES): a serial number or anything else is refused
--library-path is the sensors daemon's ADSP_LIBRARY_PATH, kept as /.pagani/library_path (the name SensorsRpc.h reads).

Archives are only read, never extracted; a member with an unsafe name is refused, a link leaving its archive skipped."""
import argparse
import hashlib
import json
import struct
import sys
import tarfile
from pathlib import Path, PurePosixPath

SOC0_NAMES = ('soc_id', 'revision', 'machine', 'family', 'platform_subtype', 'platform_subtype_id', 'platform_version', 'hw_platform')
ARCHIVES = ('vendor-etc-sensors.tar', 'odm-etc-sensors.tar', 'persist-sensors.tar')
REGISTRY = '/mnt/vendor/persist/sensors/registry/'
ROM_PREFIXES = ('vendor/etc/sensors/', 'odm/etc/sensors/')
REPO = Path(__file__).resolve().parents[3]     # package/PianoSensors/tools/ -> the checkout

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


def add_registry_tar(files, tar_path):
    """The outer registry directory of a backup, wherever it sits in the archive, as REGISTRY. Returns (files, bytes)."""
    with tarfile.open(tar_path) as tar:
        members = [m for m in tar.getmembers()]
        names = [clean(m.name) for m in members]   # refuses unsafe names before anything is read
        outer = sorted({str(PurePosixPath(n).parent) for n, m in zip(names, members)
                        if m.isfile() and PurePosixPath(n).name == 'sns_reg_version'
                        and any(o.startswith(str(PurePosixPath(n).parent) + '/registry/') for o in names)})
        if len(outer) != 1:
            fail(f'{tar_path}: {len(outer)} registry directories (one with registry/ and sns_reg_version is wanted)')
        prefix = outer[0].rstrip('/') + '/'
        count = size = 0
        for name, member in zip(names, members):
            if not name.startswith(prefix):
                continue
            if member.issym() or member.islnk() or not (member.isfile() or member.isdir()):
                fail(f'{tar_path}: the registry holds a link or a special file (refused)')
            if member.isfile():
                data = tar.extractfile(member).read()
                files[REGISTRY + name[len(prefix):]] = data
                count += 1
                size += len(data)
        if not any(name.startswith(prefix + 'registry/') for name in names):
            fail(f'{tar_path}: no registry/ in the registry directory')
        return count, size


def add_rom_config(files, directory):
    """The stock ROM's sensor configuration: each file of the manifest checked against its sha256, served at /<source>."""
    try:
        manifest = json.loads((directory / 'source.json').read_text())
        entries = manifest['files']
    except (OSError, ValueError, KeyError, TypeError) as error:
        fail(f'{directory}/source.json: not a manifest ({error})')
    for entry in entries:
        source, path, digest = entry.get('source', ''), entry.get('path', ''), entry.get('sha256', '')
        if not source.startswith(ROM_PREFIXES) or '..' in PurePosixPath(source).parts or '..' in PurePosixPath(path).parts:
            fail(f'{directory}/source.json: {source!r} is not under {" or ".join(ROM_PREFIXES)}')
        data = (directory / path).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            fail(f'{directory}/{path}: does not match its sha256 in source.json')
        files[clean(source)] = data
    return manifest.get('stock_build', 'unknown build'), len(entries)


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
    """The pack holds a unit's calibration: only this repository's ignored out/ or private/ may receive it."""
    target = out.resolve()
    for allowed in (REPO / 'out', REPO / 'private'):
        try:
            target.relative_to(allowed.resolve())
            return
        except ValueError:
            continue
    fail(f'{out}: a pack goes only under {REPO / "out"} or {REPO / "private"} (ignored by git), never elsewhere')


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
    parser.add_argument('--registry-tar', type=Path, help="a backup of this unit's persist registry (private)")
    parser.add_argument('--rom-config', type=Path, help="the stock ROM's sensor configuration with its source.json")
    parser.add_argument('--dump', type=Path, help="a directory of archives made on this tablet")
    parser.add_argument('--soc0', type=Path, help='name=value lines of /sys/devices/soc0')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--libs', action='store_true', help='also dsp-libs.tar of --dump')
    parser.add_argument('--library-path', help="the sensors daemon's ADSP_LIBRARY_PATH")
    parser.add_argument('--add', action='append', default=[], metavar='DEVICE=FILE', help='one more file, by its device path')
    parser.add_argument('--list', action='store_true', help='print what went in (the registry only as a count)')
    args = parser.parse_args()
    check_out(args.out)
    files, skipped, notes = {}, [], []
    if args.dump:
        wanted = list(ARCHIVES) + (['dsp-libs.tar'] if args.libs else [])
        for name in [n for n in wanted if (args.dump / n).is_file()]:
            add_tar(files, args.dump / name, skipped)
        if (args.dump / 'soc0.txt').is_file():
            add_soc0(files, args.dump / 'soc0.txt')
    if args.rom_config:
        build, count = add_rom_config(files, args.rom_config)
        notes.append(f'ROM configuration: {count} files of {build}, each matching its sha256')
    if args.registry_tar:
        count, size = add_registry_tar(files, args.registry_tar)
        notes.append(f'persist registry: {count} files, {size} bytes (private: not listed)')
    if args.soc0:
        add_soc0(files, args.soc0)
    for item in args.add:
        device, separator, source = item.partition('=')
        if not separator or not device.startswith('/'):
            fail(f'--add {item}: DEVICE=FILE with an absolute device path')
        files[device] = Path(source).read_bytes()
    if args.library_path:
        files['/.pagani/library_path'] = args.library_path.encode()
    if not files:
        fail('nothing to pack (give --registry-tar, --rom-config or --dump)')
    for needed in ('/vendor/etc/sensors/sns_reg_config', '/odm/etc/sensors/config/json.lst'):
        if needed not in files:
            print(f'make-sensor-pack: warning: no {needed}: the sensor PD will not find its registry configuration', file=sys.stderr)
    if not any(name.startswith(REGISTRY) for name in files):
        print('make-sensor-pack: warning: no persist registry: the DSP falls back to the stock JSON defaults (generic calibration)',
              file=sys.stderr)
    if not any(name.startswith('/sys/devices/soc0/') for name in files):
        print('make-sensor-pack: note: no soc0 values: the PD\'s reads of /sys/devices/soc0 fail (level 2 logs whether it asks)',
              file=sys.stderr)
    blob = pack(files)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    args.out.chmod(0o600)
    for line in notes:
        print(line)
    if args.list:
        for name in sorted(n for n in files if not n.startswith(REGISTRY)):
            print(f'{len(files[name]):9} {name}')
    for line in skipped:
        print('make-sensor-pack: skipped ' + line, file=sys.stderr)
    if len(blob) > WARN_BYTES:
        print(f'make-sensor-pack: warning: {len(blob)} bytes: the service loads the pack into memory (at most 4 MiB)', file=sys.stderr)
    print(f'{args.out}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
