#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""A copy of an already staged Data seed (default the Turnip stage: out/turnip-a830/stage) with a test's additions, nothing else
changed: no bundle is copied again and the trust cache is copied byte for byte. For test images (docs/power.md 1.2):

    stage-boot-run.py --out DIR [--seed SEED] [--trust TRUST] [--file SOURCE:DEST]... [--boot-run COMMAND]... [--disable-job LABEL]...

--file SOURCE:DEST   a file of this Mac to DEST on the Data volume (relative to its root), mode 644 (stage-soft-metal.py's --file).
--boot-run COMMAND   appended to the seed's one boot job (Library/LaunchDaemons/org.droidvm.run.0.plist, which runs settime.sh),
                     after what it runs already; refused when the job already ends in a loop that never returns (--log-errors).
--disable-job LABEL  LABEL => true in launchd's override database of the volume (private/var/db/com.apple.xpc.launchd/disabled.plist,
                     the file the seed already has, edited in place in its own format): launchd never loads that job, so it
                     neither runs nor answers its Mach services (clients get an invalid connection at once). The VM-era
                     stage-piano.sh's --disable-job did the same for AppSSODaemon, cryptexd, fskitd, mds and msrpc (none of them
                     runs natively: docs/boot-time.md section 4).

The mechanics are stage-soft-metal.py's (imported, not changed: build-native-image.sh's stamp hashes that file): unpack the DBRDISK1
seed to a sparse raw file in DIR, hdiutil attach with its mount point (one step, no diskutil: this Mac's diskutil mount hangs some
days, as stage.py found; no sudo: ownership ignored for the writes; files edited in place keep their owner, NEW files are stored
with this Mac user's uid, 501 here, the tablet account's uid too: confirmed on the host 2026-11-14 for every hdiutil -owners mode,
so the earlier "uid/gid 99" holds at most for diskutil's mount), then
detach and pack. The output is DIR/scratch-data.img and
DIR/kernel.macho.trustcache, and DIR/stage.json (inputs, actions, the output identity). A new seed is a new identity:
build-ram-images.py gives an image with it data-persist=0 (Data volatile, the tablet's store untouched) unless told otherwise.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

D = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('stage_soft_metal', D / 'tooling/k27/stage-soft-metal.py')
ssm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ssm)
packed_ramdisk = ssm.packed_ramdisk

OVERRIDES = 'private/var/db/com.apple.xpc.launchd/disabled.plist'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def append_boot_commands(job, commands):
    """The boot job's script with COMMANDS after what it runs (the file rewritten in place: same inode, same owner)."""
    if not job.is_file():
        raise SystemExit(f'{job}: the seed has no boot job; a new plist would not be root\'s and launchd would refuse it')
    record = plistlib.loads(job.read_bytes())
    arguments = record.get('ProgramArguments', [])
    if len(arguments) != 3 or arguments[0] != '/bin/sh' or ssm.SETTIME not in arguments[2]:
        raise SystemExit(f'{job}: not the expected boot job ({arguments}); check it by hand')
    if 'while :' in arguments[2]:
        raise SystemExit(f'{job}: its script ends in a loop that never returns ({arguments[2][:120]}...): a command after it never runs')
    script = '; '.join([arguments[2], *commands])
    record['ProgramArguments'] = ['/bin/sh', '-c', script]
    with job.open('r+b') as handle:   # in place: the file keeps the owner the VM's staging gave it
        data = plistlib.dumps(record, fmt=plistlib.FMT_XML)
        handle.write(data)
        handle.truncate(len(data))
    subprocess.run(['xattr', '-c', str(job)], check=False)
    return script


def disable_jobs(path, labels):
    """LABELS => true in launchd's override database PATH, in place, in the file's own format; returns the result's labels."""
    if not path.is_file():
        raise SystemExit(f'{path}: the volume has no launchd override database; a new one would not be root\'s')
    raw = path.read_bytes()
    binary = raw.startswith(b'bplist')
    record = plistlib.loads(raw)
    if not isinstance(record, dict):
        raise SystemExit(f'{path}: not a dictionary')
    for label in labels:
        if not label or '/' in label or ' ' in label:
            raise SystemExit(f'--disable-job {label!r}: a launchd label')
        record[label] = True
    data = plistlib.dumps(record, fmt=plistlib.FMT_BINARY if binary else plistlib.FMT_XML)
    with path.open('r+b') as handle:
        handle.write(data)
        handle.truncate(len(data))
    subprocess.run(['xattr', '-c', str(path)], check=False)
    return sorted(k for k, v in record.items() if v is True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--seed', type=Path, default=D / 'out/turnip-a830/stage/scratch-data.img')
    p.add_argument('--trust', type=Path, default=D / 'out/turnip-a830/stage/kernel.macho.trustcache')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--file', metavar='SOURCE:DEST', action='append', default=[])
    p.add_argument('--boot-run', metavar='COMMAND', action='append', default=[])
    p.add_argument('--disable-job', metavar='LABEL', action='append', default=[])
    a = p.parse_args()
    for f in (a.seed, a.trust):
        if not f.is_file():
            raise SystemExit(f'{f}: missing')
    if a.out.resolve() in (a.seed.resolve().parent, a.trust.resolve().parent):
        raise SystemExit('--out is the input\'s directory: the inputs are only read')
    files = []
    for item in a.file:
        source, _, dest = item.partition(':')
        dest = dest.lstrip('/')
        if not source or not dest or '..' in Path(dest).parts or not Path(source).is_file():
            raise SystemExit(f'--file {item}: SOURCE must be a file here, DEST a path on the Data volume')
        files.append((Path(source), dest))
    if not (files or a.boot_run or a.disable_job):
        raise SystemExit('nothing to stage: --file, --boot-run or --disable-job')
    a.out.mkdir(parents=True, exist_ok=True)
    trust = a.out / 'kernel.macho.trustcache'
    shutil.copyfile(a.trust, trust)
    record = {'inputs': {'seed': str(a.seed), 'seed_sha256': sha(a.seed), 'trust': str(a.trust), 'trust_sha256': sha(a.trust)},
              'files': [], 'boot_job': None, 'disabled': None}
    logical, extents = packed_ramdisk.regions(a.seed.read_bytes())
    with tempfile.TemporaryDirectory(prefix='seed-', dir=str(a.out)) as scratch:
        raw = Path(scratch) / 'seed.raw'
        with raw.open('wb') as handle:   # sparse: only the extents are written
            handle.truncate(logical)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
        mount = Path(scratch) / 'mnt'
        mount.mkdir()
        attached = plistlib.loads(subprocess.run(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse',
                                                  '-mountpoint', str(mount), '-plist', str(raw)], check=True, capture_output=True,
                                                 timeout=300).stdout)['system-entities']
        whole = min((e['dev-entry'] for e in attached), key=lambda d: (len(d), d)).removeprefix('/dev/')
        try:
            # hdiutil names the mount point by its realpath (/private/... for /tmp/...)
            volume = next((e['dev-entry'].removeprefix('/dev/') for e in attached if e.get('mount-point') == str(mount.resolve())), None)
            if volume is None:
                raise SystemExit(f'{raw.name}: hdiutil attached the seed but mounted no volume at {mount}; nothing staged')
            for source, dest in files:
                target = mount / dest
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
                os.chmod(target, 0o644)
                subprocess.run(['xattr', '-c', str(target)], check=False)
                record['files'].append({'dest': '/' + dest, 'source': str(source), 'sha256': sha(source)})
                print(f'{volume}: /{dest} <- {source} ({target.stat().st_size} bytes)')
            if a.boot_run:
                record['boot_job'] = append_boot_commands(mount / ssm.BOOT_JOB, a.boot_run)
                print(f'/{ssm.BOOT_JOB} runs: {record["boot_job"]}')
            if a.disable_job:
                record['disabled'] = disable_jobs(mount / OVERRIDES, a.disable_job)
                print(f'/{OVERRIDES}: disabled {", ".join(record["disabled"])}')
        finally:
            subprocess.run(['hdiutil', 'detach', '/dev/' + whole], check=True, stdout=subprocess.DEVNULL, timeout=120)
        seed = a.out / 'scratch-data.img'
        seed.unlink(missing_ok=True)
        meta = packed_ramdisk.pack(raw, seed)
    record['output'] = {'seed_sha256': sha(seed), 'identity': meta['sha256'], 'trust_sha256': sha(trust)}
    (a.out / 'stage.json').write_text(json.dumps(record, indent=1) + '\n')
    print(f'{seed}: {meta["encoded_bytes"]} bytes packed, identity {meta["sha256"][:16]} '
          f'(input {packed_ramdisk.inspect(a.seed)[0]["sha256"][:16]}); trust cache unchanged')


if __name__ == '__main__':
    sys.exit(main())
