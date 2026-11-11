#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
"""Host tests of PianoSensors, all with -Wall -Wextra -Werror and ASan/UBSan (cc from the environment's CC, default cc):

  wire     PaganiSensors' protobuf and SNS vectors (unchanged test), vector 4 framed by PianoPower's own QcomQrtr.c
  fixed    the integer float conversions and axis maps (PaganiSensors' test)
  orient   the orientation state machine (PaganiSensors' test)
  rpc      the FastRPC file service against PaganiSensors' simulated DSP, then a pack made by tools/make-sensor-pack.py
  pack     the pack tool: piano's archives, soc0 allowlist, unsafe names, no pack inside a repository's tracked tree
  piano    SensorsGuard, SensorsPolicy and the independent cross-checks against libqmi, libssc, Linux fastrpc.c and qualcomm/fastrpc
  kernel   every portable file compiles freestanding with AArch64 general registers only (no C library, no floating point)

Synthetic inputs only: no vendor file, no registry, no calibration of any unit is read or written. Host tests do not establish that the
sensor hub or the DSP answers."""
import argparse
import io
import os
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PKG = HERE.parent
POWER = PKG.parent / 'PianoPower'
PORTABLE = ('SensorsGuard.c', 'SensorsPolicy.c', 'SensorsPb.c', 'SensorsSsc.c', 'SensorsRpc.c', 'SensorsPack.c', 'SensorsFixed.c',
            'SensorsOrient.c')
CFLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']


def build(out, name, sources, extra=()):
    exe = out / name
    subprocess.run([os.environ.get('CC', 'cc'), *CFLAGS, *map(str, sources), *extra, '-o', str(exe)], check=True)
    return exe


def run(exe, *args):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
    text = (result.stdout + result.stderr).strip()
    print(text)
    if result.returncode or not text.splitlines()[-1].startswith('PASS'):
        sys.exit(f'FAIL: {exe.name}')


def member(tar, name, data=None, link=None):
    info = tarfile.TarInfo(name)
    if link is not None:
        info.type = tarfile.SYMTYPE
        info.linkname = link
        tar.addfile(info)
    else:
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))


def sample_dump(d):
    """The archives as section 5 of sensors.md makes them, with made-up contents (a link inside, a link that leaves)."""
    with tarfile.open(d / 'vendor-etc-sensors.tar', 'w') as tar:
        member(tar, 'vendor/etc/sensors/sns_reg_config', b'version=1\n')
        member(tar, 'vendor/etc/sensors/config/a.json', b'{}')
        member(tar, 'vendor/etc/sensors/alias.json', link='config/a.json')
        member(tar, 'vendor/etc/sensors/escape', link='/etc/passwd')
    with tarfile.open(d / 'odm-etc-sensors.tar', 'w') as tar:
        member(tar, 'odm/etc/sensors/config/json.lst', b'b.json\n')
        member(tar, 'odm/etc/sensors/config/b.json', b'{"b":1}')
    with tarfile.open(d / 'persist-sensors.tar', 'w') as tar:
        member(tar, 'mnt/vendor/persist/sensors/registry/registry/sensors_registry', b'calib')
    (d / 'soc0.txt').write_text('soc_id=618\nhw_platform=MTP\n')
    (d / 'added').write_bytes(b'add')


def tool(*args):
    return subprocess.run([sys.executable, str(PKG / 'tools/make-sensor-pack.py'), *map(str, args)], capture_output=True, text=True)


def pack_checks(d):
    sample_dump(d)
    pack = d / 'pack.bin'
    done = tool('--dump', d, '--out', pack, '--library-path', '/vendor/dsp/sensors;/x', '--add', f'/vendor/extra/added={d / "added"}',
                '--list')
    if done.returncode or 'skipped vendor/etc/sensors/escape' not in done.stderr or '/odm/etc/sensors/config/json.lst' not in done.stdout:
        sys.exit(f'FAIL: make-sensor-pack: {done.stdout}{done.stderr}')
    if pack.stat().st_mode & 0o077:
        sys.exit('FAIL: make-sensor-pack: the pack is readable by others')
    (d / 'evil').mkdir()
    with tarfile.open(d / 'evil/vendor-etc-sensors.tar', 'w') as tar:
        member(tar, 'vendor/../../etc/passwd', b'x')
    done = tool('--dump', d / 'evil', '--out', d / 'evil.bin')
    if not done.returncode or 'unsafe member name' not in done.stderr or (d / 'evil.bin').exists():
        sys.exit(f'FAIL: make-sensor-pack took an unsafe name: {done.stdout}{done.stderr}')
    (d / 'serial').mkdir()
    (d / 'serial/soc0.txt').write_text('soc_id=618\nserial_number=12345\n')
    done = tool('--dump', d / 'serial', '--out', d / 'serial.bin')
    if not done.returncode or 'serial_number is not served' not in done.stderr or (d / 'serial.bin').exists():
        sys.exit(f'FAIL: make-sensor-pack took a serial number: {done.stdout}{done.stderr}')
    done = tool('--dump', d, '--out', PKG / 'pack-in-the-tree.bin')
    if (PKG / 'pack-in-the-tree.bin').exists():
        (PKG / 'pack-in-the-tree.bin').unlink()
        sys.exit('FAIL: make-sensor-pack wrote a pack into the repository')
    if subprocess.run(['git', '-C', str(PKG), 'rev-parse'], capture_output=True).returncode == 0 and \
            (not done.returncode or 'inside the repository' not in done.stderr):
        sys.exit(f'FAIL: make-sensor-pack did not refuse the repository: {done.stdout}{done.stderr}')
    print('PASS pack tool: piano\'s archives, links, soc0 allowlist (no serial), unsafe names, never inside the repository')
    return pack


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--only', action='append', default=[], choices=['wire', 'fixed', 'orient', 'rpc', 'pack', 'piano', 'kernel'])
    args = parser.parse_args()
    want = set(args.only) or {'wire', 'fixed', 'orient', 'rpc', 'pack', 'piano', 'kernel'}
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        src = lambda *names: [PKG / n for n in names]
        if 'wire' in want:
            run(build(out, 'wire', [HERE / 'wire.c', *src('SensorsPb.c', 'SensorsSsc.c'), POWER / 'QcomQrtr.c']))
        if 'fixed' in want:
            run(build(out, 'fixed', [HERE / 'fixed.c', *src('SensorsFixed.c')], ['-lm']))
        if 'orient' in want:
            run(build(out, 'orient', [HERE / 'orient.c', *src('SensorsOrient.c')], ['-lm']))
        pack = None
        if 'pack' in want or 'rpc' in want:
            (out / 'dump').mkdir()
            pack = pack_checks(out / 'dump')
        if 'rpc' in want:
            run(build(out, 'rpc', [HERE / 'rpc.c', *src('SensorsRpc.c', 'SensorsPack.c')], ['-D_DARWIN_C_SOURCE', '-lpthread']), pack)
        if 'piano' in want:
            run(build(out, 'piano', [HERE / 'piano.c', *src('SensorsGuard.c', 'SensorsPolicy.c', 'SensorsFixed.c', 'SensorsOrient.c',
                                                           'SensorsSsc.c', 'SensorsPb.c', 'SensorsRpc.c', 'SensorsPack.c'),
                                     POWER / 'QcomQrtr.c'], ['-lm']))
        if 'kernel' in want:
            # the kernel's constraints: no C library, no floating point registers (AArch64 -mgeneral-regs-only)
            for name in PORTABLE:
                subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-target', 'arm64-apple-macos13', '-ffreestanding', '-fno-builtin',
                                '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-c', str(PKG / name), '-o', str(out / 'k.o')],
                               check=True)
            print(f'PASS kernel constraints: {len(PORTABLE)} portable files freestanding, general registers only')
    print('PASS PianoSensors host tests')


if __name__ == '__main__':
    main()
