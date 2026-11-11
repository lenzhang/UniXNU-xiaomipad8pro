#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
"""Host tests of PianoSensors, all with -Wall -Wextra -Werror and ASan/UBSan (cc from the environment's CC, default cc):

  wire     PaganiSensors' protobuf and SNS vectors (unchanged test), vector 4 framed by PianoPower's own QcomQrtr.c
  fixed    the integer float conversions and axis maps (PaganiSensors' test)
  orient   the orientation state machine (PaganiSensors' test)
  rpc      the FastRPC file service against PaganiSensors' simulated DSP, then a pack made by tools/make-sensor-pack.py
  pack     the pack tool: piano's archives, soc0 allowlist, unsafe names, no pack inside a repository's tracked tree
  piano    SensorsGuard, SensorsPolicy, SensorsClient against a simulated hub, SensorsHealth (PianoPower's report), the arena check, a
           declined DSP call, and the independent cross-checks against libqmi, libssc, Linux fastrpc.c and qualcomm/fastrpc
  streams  level 4: SensorsStream over PianoAuto's AutoStream and AutoLight (compiled from their package) against a simulated hub
           with the service's message shapes: the requests byte for byte, the light and the accelerometer, off/on cycles, the
           guard, a strike, a sleep, generations, the level-3 list feeding the streams
  load     SensorsLoad + FatVolume on a FAT32 image made by mtools as build-esp.py makes the ESP (skipped without mtools)
  addon    the RAM test image's carrier: tools/sensor_addon.py's addon (build-ram-images.py's PIANO_SENSOR_PACK) read by SensorsAddon,
           inflated with zlib and checked by sl_check, the container's refusals; the tool's own refusals
  kernel   every portable file compiles freestanding with AArch64 general registers only (no C library, no floating point)

Synthetic inputs only: no vendor file, no registry, no calibration of any unit is read or written. Host tests do not establish that the
sensor hub or the DSP answers."""
import argparse
import atexit
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PKG = HERE.parent
REPO_OUT = PKG.parents[1] / 'out' / 'host-tests'   # ignored; the pack tool writes only under out/ or private/
POWER = PKG.parent / 'PianoPower'
AUTO = PKG.parent / 'PianoAuto'   # AutoStream.c, AutoLight.c: the streams' off/on lifecycle and the light chain (level 4)
PORTABLE = ('SensorsGuard.c', 'SensorsPolicy.c', 'SensorsPb.c', 'SensorsSsc.c', 'SensorsRpc.c', 'SensorsPack.c', 'SensorsFixed.c',
            'SensorsOrient.c', 'SensorsClient.c', 'SensorsLoad.c', 'FatVolume.c', 'SensorsStream.c', 'SensorsAddon.c')
INCLUDE = PKG.parent / 'include'   # PianoAdspContract.h
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


def registry_tar(path, prefix='usr/share/piano/sensors/registry-private/', link=False, twice=False):
    """A registry backup shaped as piano's (the outer registry directory under a prefix), with made-up contents."""
    with tarfile.open(path, 'w') as tar:
        member(tar, prefix + 'registry/group_a', b'made-up group a')
        member(tar, prefix + 'registry/sub/group_b', b'made-up group b')
        member(tar, prefix + 'sns_reg_version', b'1\n')
        member(tar, prefix + 'parsed_file_list.csv', b'x\n')
        if link:
            member(tar, prefix + 'registry/escape', link='/etc/passwd')
        if twice:
            member(tar, 'other/registry/group_c', b'c')
            member(tar, 'other/sns_reg_version', b'1\n')


def rom_config(d, tamper=False, outside=False):
    """A stock-configuration directory with its manifest, as firmware/sensors/source.json describes one, made-up contents."""
    d.mkdir(parents=True, exist_ok=True)
    (d / 'vendor').mkdir(exist_ok=True)
    (d / 'odm/config').mkdir(parents=True, exist_ok=True)
    bodies = {'vendor/sns_reg_config': b'version=1\n', 'odm/config/json.lst': b'b.json\n', 'odm/config/b.json': b'{"b":1}'}
    sources = {'vendor/sns_reg_config': 'vendor/etc/sensors/sns_reg_config', 'odm/config/json.lst': 'odm/etc/sensors/config/json.lst',
               'odm/config/b.json': 'odm/etc/sensors/config/b.json'}
    if outside:
        sources['odm/config/b.json'] = 'odm/etc/init/b.json'
    files = []
    for path, body in bodies.items():
        (d / path).write_bytes(body)
        files.append({'path': path, 'source': sources[path], 'sha256': hashlib.sha256(body).hexdigest()})
    if tamper:
        (d / 'odm/config/b.json').write_bytes(b'{"b":2}')
    (d / 'source.json').write_text(json.dumps({'stock_build': 'TEST.BUILD', 'files': files}))
    return d


def pack_checks(d, scratch):
    """The pack tool on made-up inputs; every pack is written under this repository's out/ (scratch), the only place it may go."""
    sample_dump(d)
    pack = scratch / 'pack.bin'
    done = tool('--dump', d, '--out', pack, '--library-path', '/vendor/dsp/sensors;/x', '--add', f'/vendor/extra/added={d / "added"}',
                '--list')
    if done.returncode or 'skipped vendor/etc/sensors/escape' not in done.stderr or '/odm/etc/sensors/config/json.lst' not in done.stdout:
        sys.exit(f'FAIL: make-sensor-pack: {done.stdout}{done.stderr}')
    if pack.stat().st_mode & 0o077:
        sys.exit('FAIL: make-sensor-pack: the pack is readable by others')
    (d / 'evil').mkdir()
    with tarfile.open(d / 'evil/vendor-etc-sensors.tar', 'w') as tar:
        member(tar, 'vendor/../../etc/passwd', b'x')
    done = tool('--dump', d / 'evil', '--out', scratch / 'evil.bin')
    if not done.returncode or 'unsafe member name' not in done.stderr or (scratch / 'evil.bin').exists():
        sys.exit(f'FAIL: make-sensor-pack took an unsafe name: {done.stdout}{done.stderr}')
    (d / 'serial').mkdir()
    (d / 'serial/soc0.txt').write_text('soc_id=618\nserial_number=12345\n')
    done = tool('--dump', d / 'serial', '--out', scratch / 'serial.bin')
    if not done.returncode or 'serial_number is not served' not in done.stderr or (scratch / 'serial.bin').exists():
        sys.exit(f'FAIL: make-sensor-pack took a serial number: {done.stdout}{done.stderr}')
    # only out/ or private/ of this repository: not the package, not a temporary directory
    for where in (PKG / 'pack-in-the-tree.bin', d / 'outside.bin'):
        done = tool('--dump', d, '--out', where)
        if where.exists():
            where.unlink()
            sys.exit(f'FAIL: make-sensor-pack wrote a pack to {where}')
        if not done.returncode or 'a pack goes only under' not in done.stderr:
            sys.exit(f'FAIL: make-sensor-pack did not refuse {where}: {done.stdout}{done.stderr}')
    # the owner's private sources: a registry backup and the stock configuration with its manifest
    registry_tar(d / 'registry.tar')
    rom = rom_config(d / 'rom')
    local = scratch / 'local.bin'
    done = tool('--registry-tar', d / 'registry.tar', '--rom-config', rom, '--out', local, '--list')
    if done.returncode or 'persist registry: 4 files' not in done.stdout or 'ROM configuration: 3 files of TEST.BUILD' not in done.stdout:
        sys.exit(f'FAIL: make-sensor-pack (local sources): {done.stdout}{done.stderr}')
    if 'group_a' in done.stdout + done.stderr or 'made-up' in done.stdout + done.stderr:
        sys.exit('FAIL: make-sensor-pack printed a registry name or content')
    blob = local.read_bytes()
    for path, body in (('/mnt/vendor/persist/sensors/registry/registry/group_a', b'made-up group a'),
                       ('/mnt/vendor/persist/sensors/registry/registry/sub/group_b', b'made-up group b'),
                       ('/mnt/vendor/persist/sensors/registry/sns_reg_version', b'1\n'),
                       ('/vendor/etc/sensors/sns_reg_config', b'version=1\n'), ('/odm/etc/sensors/config/json.lst', b'b.json\n')):
        if path.encode() + b'\0' not in blob or body not in blob:
            sys.exit(f'FAIL: make-sensor-pack (local sources): {path} missing')
    for label, args, expect in (
            ('a link in the registry', ['--registry-tar', registry_tar(d / 'link.tar', link=True) or d / 'link.tar'], 'link or a special'),
            ('two registry directories', ['--registry-tar', registry_tar(d / 'twice.tar', twice=True) or d / 'twice.tar'], '2 registry'),
            ('a tampered ROM file', ['--rom-config', rom_config(d / 'tampered', tamper=True)], 'does not match its sha256'),
            ('a ROM path outside the sensor trees', ['--rom-config', rom_config(d / 'outside', outside=True)], 'is not under')):
        bad = scratch / 'bad.bin'
        done = tool(*args, '--out', bad)
        if not done.returncode or expect not in done.stderr or bad.exists():
            sys.exit(f'FAIL: make-sensor-pack took {label}: {done.stdout}{done.stderr}')
    print("PASS pack tool: piano's archives, links, soc0 allowlist (no serial), unsafe names, only out/ or private/; the registry "
          "backup (never printed) and the manifest-checked ROM configuration")
    return pack


def esp_image(out, pack):
    """A FAT32 volume as build-esp.py formats the ESP (4096-byte sectors, one per cluster), the pack at SensorsLoad.h's SL_PATH,
    a damaged copy, a short file and one too big. None (and a note) without mtools."""
    tools = [shutil.which(t) for t in ('mformat', 'mmd', 'mcopy')]
    if not all(tools):
        print('SKIP pack loader: no mtools (brew install mtools)')
        return None
    image = out / 'esp.img'
    mtools = lambda *a: subprocess.run(list(a), check=True, capture_output=True)
    mtools('mformat', '-C', '-F', '-S', '5', '-M', '4096', '-T', '70000', '-h', '1', '-s', '1', '-c', '1', '-i', str(image), '::')
    mtools('mmd', '-i', str(image), '::/EFI', '::/EFI/UniXNU')
    data = pack.read_bytes()
    bad = bytearray(data)
    bad[40] ^= 0xff                                   # inside the table: the sum no longer holds
    files = {'piano-sensors.pack': data, 'bad-sensors.pack': bytes(bad), 'short-sensors.pack': data[:16],
             'big-sensors.pack': data[:32] + bytes(5 << 20)}
    for name, body in files.items():
        (out / name).write_bytes(body)
        mtools('mcopy', '-i', str(image), str(out / name), f'::/EFI/UniXNU/{name}')
    return image


def addon_checks(pack, scratch):
    """tools/sensor_addon.py: the addon of a made-up pack (under out/, mode 0600, nothing of the pack printed), its refusals."""
    tool = PKG / 'tools/sensor_addon.py'
    addon = scratch / 'pack.gpuaddon'
    done = subprocess.run([sys.executable, str(tool), str(pack), str(addon)], capture_output=True, text=True)
    if done.returncode or not addon.is_file() or addon.stat().st_mode & 0o077 or 'sensors/piano-sensors.pack' not in done.stdout:
        sys.exit(f'FAIL: sensor_addon.py: {done.stdout}{done.stderr}')
    for where, expect in ((PKG / 'addon-in-the-tree.gpuaddon', 'goes only under'), (scratch / 'x.gpuaddon', 'not a sensor pack')):
        source = pack if expect == 'goes only under' else tool
        done = subprocess.run([sys.executable, str(tool), str(source), str(where)], capture_output=True, text=True)
        if where.exists() and where.parent == PKG:
            where.unlink()
            sys.exit(f'FAIL: sensor_addon.py wrote {where}')
        if not done.returncode or expect not in done.stderr:
            sys.exit(f'FAIL: sensor_addon.py did not refuse {where}: {done.stdout}{done.stderr}')
    print('PASS addon tool: the pack as UniXNU\'s addon container, only under out/ or private/, mode 0600, a non-pack refused')
    return addon


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--only', action='append', default=[],
                        choices=['wire', 'fixed', 'orient', 'rpc', 'pack', 'piano', 'streams', 'load', 'addon', 'kernel'])
    args = parser.parse_args()
    want = set(args.only) or {'wire', 'fixed', 'orient', 'rpc', 'pack', 'piano', 'streams', 'load', 'addon', 'kernel'}
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
        REPO_OUT.mkdir(parents=True, exist_ok=True)
        scratch = Path(tempfile.mkdtemp(prefix='sensor-pack-', dir=REPO_OUT))   # the only place a pack may be written
        atexit.register(shutil.rmtree, scratch, True)                            # made-up packs only, gone also after a FAIL
        if 'pack' in want or 'rpc' in want or 'load' in want or 'addon' in want:
            (out / 'dump').mkdir()
            pack = pack_checks(out / 'dump', scratch)
        if 'rpc' in want:
            run(build(out, 'rpc', [HERE / 'rpc.c', *src('SensorsRpc.c', 'SensorsPack.c')], ['-D_DARWIN_C_SOURCE', '-lpthread']), pack)
        if 'piano' in want:
            run(build(out, 'piano', [HERE / 'piano.c', *src('SensorsGuard.c', 'SensorsPolicy.c', 'SensorsFixed.c', 'SensorsOrient.c',
                                                           'SensorsSsc.c', 'SensorsPb.c', 'SensorsRpc.c', 'SensorsPack.c',
                                                           'SensorsClient.c'),
                                     POWER / 'QcomQrtr.c'], ['-lm', '-I', str(INCLUDE)]))
        if 'streams' in want:
            run(build(out, 'streams', [HERE / 'streams.c', *src('SensorsStream.c', 'SensorsSsc.c', 'SensorsPb.c', 'SensorsPolicy.c',
                                                               'SensorsFixed.c', 'SensorsOrient.c', 'SensorsGuard.c', 'SensorsClient.c'),
                                       AUTO / 'AutoStream.c', AUTO / 'AutoLight.c'],
                      ['-lm', '-I', str(PKG), '-I', str(INCLUDE), '-I', str(AUTO)]))
        if 'load' in want:
            image = esp_image(out, pack)
            if image:
                run(build(out, 'load', [HERE / 'load.c', *src('SensorsLoad.c', 'SensorsPack.c', 'FatVolume.c')]), image)
        if 'addon' in want:
            addon = addon_checks(pack, scratch)
            run(build(out, 'addon', [HERE / 'addon.c', *src('SensorsAddon.c', 'SensorsLoad.c', 'SensorsPack.c', 'FatVolume.c')], ['-lz']),
                addon, pack)
        if 'kernel' in want:
            # the kernel's constraints: no C library, no floating point registers (AArch64 -mgeneral-regs-only)
            for name in PORTABLE:
                subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-target', 'arm64-apple-macos13', '-ffreestanding', '-fno-builtin',
                                '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-I', str(PKG), '-I', str(INCLUDE), '-I', str(AUTO),
                                '-c', str(PKG / name),
                                '-o', str(out / 'k.o')], check=True)
            print(f'PASS kernel constraints: {len(PORTABLE)} portable files freestanding, general registers only')
    print('PASS PianoSensors host tests')


if __name__ == '__main__':
    main()
