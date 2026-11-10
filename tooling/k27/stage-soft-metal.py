#!/usr/bin/env python3
"""Stage a Metal plugin bundle (default out/soft-metal/NativeSoftMetal.bundle) for a native test, on the Mac:
  - a copy of the Data seed with the bundle in /Library/GPUBundles (the first directory IOGPU searches for <MetalPluginName>.bundle;
    /Library is a firmlink into the Data volume), and
  - a trust cache with one more module, the bundle's Mach-O CDHashes, before the input's modules (as macOS-DroidVM's
    stage-gpu-plugin.py does), so AMFI and libsystem_sandbox's gpu_bundle_find_trusted accept the ad hoc signed plugin.

    stage-soft-metal.py [--seed out/inputs/scratch-data.img] [--trust out/inputs/kernel.macho.trustcache] [--out out/soft-metal]
                        [--boot-run COMMAND]... [--log-stream PREDICATE | --log-errors PREDICATE] [BUNDLE]

Then: PIANO_SEED=out/soft-metal/scratch-data.img PIANO_TRUST=out/soft-metal/kernel.macho.trustcache tooling/k27/build-native-image.sh.
The seed is the packed DBRDISK1 disk (NativeRamBlock SeedDisk.h). It is unpacked to a sparse raw file, attached with hdiutil (no mount),
its APFS volume mounted with diskutil (no sudo: the volume then ignores ownership, so the copies are stored as uid/gid 99, mode 755/644),
the bundle copied, the volume unmounted and the disk packed again. Nothing outside --out is written.

A new seed has a new identity, so NativeRamBlock's persistent store (data-persist=2) treats the store window as stale and starts it over,
which drops the Data changes of earlier native boots. Boot staged-seed tests with PIANO_DATA_PERSIST=0 (Data in RAM only) to keep them.

--boot-run adds a command to the seed's one boot job (Library/LaunchDaemons/org.droidvm.run.0.plist, which runs settime.sh: the clock).
The job's file is rewritten in place, so it keeps the owner the VM's staging gave it; launchd refuses a daemon plist that is not root's,
and this Mac cannot set an owner on the seed's volume. The commands run as root after settime.sh, in the order given, and their output
goes to the kernel log with stderr-log=1.
--log-stream PREDICATE puts a process's own os_log messages there. That is where SkyLight's and Metal's messages go, the ones the
console log otherwise never shows (an abort of WindowServer, for instance, logs its reason through os_log). It is a loop of
`log show --last 25s` every 20 s, not `log stream`: on 2026-11-11 a `log stream` behind the job printed its first 72 lines and then
nothing (its stdout is a pipe, so block-buffered, or the stream died), while each `log show` exits and so flushes. Windows overlap by 5 s,
so a line can appear twice; none is lost unless a `log show` takes longer than 5 s more than its sleep. `log show` reads what logd
persisted: errors and faults always, info only when configured. It needs logd; without it the job's loop prints its error every 20 s
and the rest of the boot is unchanged.
Each line is printed by its own write, cut to 150 characters: the kernel's stderr-log logs the first 159 bytes of each write only, so a
buffered `log show` writing many lines at once would lose all but the first.
--log-errors PREDICATE is the same for the error and fault messages only (messageType error or fault): the measured run of 2026-11-11
with every WindowServer and loginwindow message wrapped the 7 MiB console ring, and the crash messages of the last aborts were lost.
"""
import argparse
import hashlib
import os
import plistlib
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

D = Path(__file__).resolve().parents[2]
K27 = Path(os.environ.get('K27_TREE', Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM'))
sys.path.insert(0, str(K27 / 'sources/UniXNU/drivers/DarwinBoot/Test'))
import packed_ramdisk  # noqa: E402  (read from the k27 tree, not changed)

LC_CODE_SIGNATURE = 0x1d
GPU_BUNDLES = 'Library/GPUBundles'
BOOT_JOB = 'Library/LaunchDaemons/org.droidvm.run.0.plist'
SETTIME = 'sh /private/var/settime.sh'


def cdhash(path):
    """The CDHash of a thin Mach-O: SHA-256 (truncated to 20 bytes) of its SHA-256 CodeDirectory, or None when it is not Mach-O.
    As macOS-DroidVM's stage-gpu-plugin.py."""
    data = path.read_bytes()
    if data[:4] != b'\xcf\xfa\xed\xfe':
        return None
    count, = struct.unpack_from('<I', data, 16)
    at = 32
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, at)
        if command == LC_CODE_SIGNATURE:
            offset, length = struct.unpack_from('<II', data, at + 8)
            blob = data[offset:offset + length]
            magic, _, blobs = struct.unpack_from('>III', blob, 0)
            if magic != 0xfade0cc0:
                raise SystemExit(f'{path}: no embedded signature')
            for index in range(blobs):
                _, start = struct.unpack_from('>II', blob, 12 + 8 * index)
                cd_magic, cd_length = struct.unpack_from('>II', blob, start)
                if cd_magic == 0xfade0c02 and blob[start + 37] == 2:
                    return hashlib.sha256(blob[start:start + cd_length]).digest()[:20]
            raise SystemExit(f'{path}: no SHA-256 CodeDirectory')
        at += size
    raise SystemExit(f'{path}: not signed')


def trust_bundle(hashes, original):
    """The input bundle (count, offsets, version-2 modules) with one more module of HASHES first. As stage-gpu-plugin.py."""
    count, = struct.unpack_from('<I', original)
    starts = struct.unpack_from(f'<{count}I', original, 4)
    originals = [original[start:end] for start, end in zip(starts, list(starts[1:]) + [len(original)])]
    hashes = sorted(set(hashes))   # the loader and the kernel binary-search a module's hashes
    identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'unixnu-piano-soft-metal:' + ''.join(h.hex() for h in hashes))
    own = struct.pack('<I16sI', 2, identifier.bytes, len(hashes)) + b''.join(h + bytes([2, 0, 0, 0]) for h in hashes)
    modules = [own, *originals]
    offsets, cursor = [], 4 + 4 * len(modules)
    for module in modules:
        offsets.append(cursor)
        cursor += len(module)
    return struct.pack(f'<{len(modules) + 1}I', len(modules), *offsets) + b''.join(modules)


def run(*command, **kw):
    return subprocess.run(command, check=True, **kw)


def set_boot_commands(job, commands):
    """The seed's boot job, rewritten in place (same file, same owner): settime.sh first, then COMMANDS."""
    if not job.is_file():
        raise SystemExit(f'{job}: the seed has no boot job to extend; a new plist would not be root\'s and launchd would refuse it')
    record = plistlib.loads(job.read_bytes())
    arguments = record.get('ProgramArguments', [])
    if len(arguments) != 3 or arguments[0] != '/bin/sh' or SETTIME not in arguments[2]:
        raise SystemExit(f'{job}: not the expected boot job ({arguments}); check it by hand')
    script = '; '.join([SETTIME, *commands])
    record['ProgramArguments'] = ['/bin/sh', '-c', script]
    job.write_bytes(plistlib.dumps(record, fmt=plistlib.FMT_XML))
    subprocess.run(['xattr', '-c', str(job)], check=False)   # the build Mac's com.apple.provenance tag
    return script


def data_volume(container_disk):
    """The device of the APFS volume named Data (else the first volume) of the container on CONTAINER_DISK."""
    listing = plistlib.loads(run('diskutil', 'apfs', 'list', '-plist', capture_output=True).stdout)
    for container in listing.get('Containers', []):
        stores = [s.get('DeviceIdentifier') for s in container.get('PhysicalStores', [])]
        if container_disk in stores or container.get('ContainerReference') == container_disk:
            volumes = container.get('Volumes', [])
            for volume in volumes:
                if volume.get('Name') == 'Data':
                    return volume['DeviceIdentifier']
            if volumes:
                return volumes[0]['DeviceIdentifier']
    raise SystemExit(f'no APFS container on {container_disk}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--seed', type=Path, default=D / 'out/inputs/scratch-data.img')
    p.add_argument('--trust', type=Path, default=D / 'out/inputs/kernel.macho.trustcache')
    p.add_argument('--out', type=Path, default=D / 'out/soft-metal')
    p.add_argument('--boot-run', metavar='COMMAND', action='append', default=[])
    p.add_argument('--log-stream', metavar='PREDICATE')
    p.add_argument('--log-errors', metavar='PREDICATE')
    p.add_argument('bundle', nargs='?', type=Path, default=D / 'out/soft-metal/NativeSoftMetal.bundle')
    a = p.parse_args()
    commands = list(a.boot_run)
    if a.log_stream and a.log_errors:
        raise SystemExit('--log-stream or --log-errors, not both')
    if a.log_stream or a.log_errors:
        predicate = a.log_stream or f'({a.log_errors}) AND (messageType == error OR messageType == fault)'
        info = ' --info' if a.log_stream else ''
        # one write per line (printf, a shell builtin), the header line left out
        lines = 'while IFS= read -r line; do case $line in Timestamp*) ;; *) printf "%.150s\\n" "$line" ;; esac; done'
        commands += [f'while :; do log show --last 25s{info} --style compact --predicate {shlex.quote(predicate)} 2>&1 | {lines}; '
                     'sleep 20; done']
    bundle = a.bundle.resolve()
    if not (bundle / 'Contents/Info.plist').is_file():
        raise SystemExit(f'{bundle}: not a bundle (Contents/Info.plist)')
    a.out.mkdir(parents=True, exist_ok=True)

    hashes = [h for f in sorted(bundle.rglob('*')) if f.is_file() and (h := cdhash(f))]
    trust = a.out / 'kernel.macho.trustcache'
    trust.write_bytes(trust_bundle(hashes, a.trust.read_bytes()))
    print(f'{trust}: + {len(hashes)} CDHash(es) {" ".join(h.hex() for h in hashes)}')

    logical, extents = packed_ramdisk.regions(a.seed.read_bytes())
    with tempfile.TemporaryDirectory(prefix='seed-', dir=str(a.out)) as scratch:
        raw = Path(scratch) / 'seed.raw'
        with raw.open('wb') as handle:   # sparse: only the extents are written
            handle.truncate(logical)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
        attached = plistlib.loads(run('hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nomount', '-nobrowse',
                                      '-plist', str(raw), capture_output=True).stdout)
        disks = [e['dev-entry'] for e in attached['system-entities']]
        whole = min(disks, key=len).removeprefix('/dev/')
        mount = Path(scratch) / 'mnt'
        mount.mkdir()
        try:
            volume = data_volume(whole)
            run('diskutil', 'mount', '-mountPoint', str(mount), volume, stdout=subprocess.DEVNULL)
            try:
                target = mount / GPU_BUNDLES
                target.mkdir(parents=True, exist_ok=True)
                shutil.rmtree(target / bundle.name, ignore_errors=True)
                shutil.copytree(bundle, target / bundle.name, symlinks=True)
                for path in [target, *(target / bundle.name).rglob('*'), target / bundle.name]:
                    os.chmod(path, 0o755 if path.is_dir() or os.access(path, os.X_OK) else 0o644)
                # The build Mac tags files its sandboxed tools write (com.apple.provenance): not carried into the tablet's volume.
                subprocess.run(['xattr', '-cr', str(target / bundle.name)], check=False)
                left = run('xattr', '-lr', str(target / bundle.name), capture_output=True, text=True).stdout.strip()
                listing = run('ls', '-lnR', str(target / bundle.name), capture_output=True, text=True).stdout
                root = run('ls', '-la', str(mount), capture_output=True, text=True).stdout
                print(f'{volume}: /{GPU_BUNDLES}/{bundle.name}\n{listing}extended attributes left: {left or "none"}\n'
                      f'volume root:\n{root}')
                if commands:
                    print(f'/{BOOT_JOB} runs: ' + set_boot_commands(mount / BOOT_JOB, commands))
            finally:
                run('diskutil', 'unmount', str(mount), stdout=subprocess.DEVNULL)
        finally:
            run('hdiutil', 'detach', '/dev/' + whole, stdout=subprocess.DEVNULL)
        seed = a.out / 'scratch-data.img'
        seed.unlink(missing_ok=True)
        meta = packed_ramdisk.pack(raw, seed)
    print(f'{seed}: {meta["encoded_bytes"]} bytes packed, logical {meta["bytes"]}, identity {meta["sha256"][:16]} '
          f'(input {packed_ramdisk.inspect(a.seed)[0]["sha256"][:16]})')


if __name__ == '__main__':
    main()
