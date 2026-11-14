#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""A warm Data seed for RAM test images: the caches one desktop boot builds (LaunchServices' databases, the font registry, the
icon store, ...) carried into a copy of the seed, so later RAM boots skip rebuilding them (docs/boot-time.md section 10: lsd
rebuilds its database on every RAM boot, ~20-25 s at one core before Installer Progress / loginwindow, and the first login's
caches add ~16 s). The installed system keeps these in its store and does not need this.

    warm-seed.py inspect [--seed SEED]                         what the seed has of them now (read only)
    warm-seed.py collect --out DIR [--ssh-address A]           from a running RAM desktop boot, over the USB network (desk-ssh.sh)
    warm-seed.py build --pack DIR/warm.tar.xz --out OUTDIR [--seed SEED] [--trust TRUST] [--include GLOB]... [--all]

collect: after the login (the Finder up, a minute idle so lsd has written its database), as root on the tablet (sudo with the
debug account's password from private/login.secret on stdin, never in argv): tar (xz, numeric owners, modes, xattrs) of
private/var/folders/*/*/0 and C (LaunchServices, FontRegistry, Metal, iconservices' per-user parts), private/var/db/lsd,
Library/Caches and every home's Library/Caches, streamed to DIR/warm.tar.xz; DIR/collect.json says what and how big.
build: a copy of SEED with DIR's pack (filtered to --include globs, default the LaunchServices databases, lsd's, the font registry
and the icon store; --all: everything collected) at private/var/piano/warm/warm.tar.xz and private/etc/rc.server, which launchd runs
as a boot task before any daemon (its log: "(rc.server) Doing boot task"; without the file "optional boot task not present") and
which extracts the pack as root, owners and modes as recorded, then writes one console line. inspect and build need sudo -n on
this Mac: the seed's volume is mounted with mount_apfs (owners honoured; diskutil mount hangs on these images here) and the two
files are made root's.
The output is OUTDIR/scratch-data.img (a new seed identity: build-ram-images.py gives its images data-persist=0, Data volatile, the
tablet's store untouched; NativeRamBlock's seed guard keeps the store of another seed in any case), OUTDIR/kernel.macho.trustcache
(the input's, byte for byte) and OUTDIR/stage.json. The RAM image must stay under build-ram-images.py's IMAGE_MAX_BYTES (250 MB): the
pack is what grows it, so the build prints the seed's growth.
Regenerate after a new seed, a new userland layer or a system update: collect again on a desktop boot of the new image, build again.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(D / 'tooling/k27'))
import importlib.util  # noqa: E402

DEFAULT_SEED = Path('/Volumes/ExtSSD/k273/v2/seed-200.img')
DEFAULT_TRUST = Path('/Volumes/ExtSSD/k273/v4/layer/kernel.macho.trustcache')
PACK_DEST = 'private/var/piano/warm/warm.tar.xz'
RC_SERVER = 'private/etc/rc.server'
# What collect takes (relative to the Data volume's root, as the tablet's shell globs them). The 27.3 RAM seed (seed-200, checked
# 2026-11-14 with `inspect`) has an empty private/var/folders, no private/var/db/lsd, an empty Library/Caches and a home whose
# Library holds only Compositions, Favorites, FontCollections, Input Methods and Preferences: every RAM boot builds all of it, and
# every first login the home's. Temporary folders, keychains and logs stay out.
COLLECT = ['private/var/folders', 'private/var/db/lsd', 'Library/Caches', 'Users/*/Library']
COLLECT_EXCLUDE = ['private/var/folders/*/*/T', 'private/var/folders/*/*/T/*', 'Users/*/Library/Keychains', 'Users/*/Library/Logs',
                   '*.sock']
# what build keeps by default: the rebuilds measured on RAM boots
DEFAULT_INCLUDE = ['*com.apple.LaunchServices*', 'private/var/db/lsd*', '*com.apple.FontRegistry*', '*com.apple.iconservices*']


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def tablet_script():
    """The collector, run by /bin/sh as root on the tablet; no single quotes (it travels inside sh -c '...')."""
    excludes = ' '.join('--exclude "' + e + '"' for e in COLLECT_EXCLUDE)   # quoted: tar matches them, not the shell
    return ('cd / || exit 1; L=; for d in ' + ' '.join(COLLECT) + '; do [ -d "$d" ] && L="$L $d"; done; '
            '[ -n "$L" ] || exit 3; exec /usr/bin/tar -c -J --numeric-owner ' + excludes + ' -f - $L 2>/dev/null')


def rc_server(pack_id):
    """private/etc/rc.server: launchd's rc.server boot task, as root, before the daemons."""
    return ('#!/bin/sh\n'
            '# UniXNU-piano tooling/k27/warm-seed.py (a RAM test seed only): the caches of one desktop boot, before any daemon\n'
            f'p=/{PACK_DEST}\n'
            '[ -f "$p" ] || exit 0\n'
            '/usr/bin/tar -x -p --numeric-owner -f "$p" -C / 2>/dev/null\n'
            'rc=$?\n'
            f'echo "warm-seed: the caches of pack {pack_id} extracted before the daemons (tar rc $rc)" > /dev/console\n'
            'exit 0\n')


def selected(names, include, everything):
    """The archive members kept, in archive order: every member when EVERYTHING, else each member that matches a glob of INCLUDE or
    lies under a directory that does, every directory above a kept member (so tar -x finds each parent with the owner and mode the
    tablet gave it: a per-user folder made by tar as root would lock its user out), and the AppleDouble "._" entry of a kept member
    (bsdtar's Mac metadata: its extended attributes)."""
    if everything:
        return list(names)
    clean = [n.rstrip('/') for n in names]
    keep = set()
    for n in clean:
        parts = n.split('/')
        if parts[-1].startswith('._'):
            continue
        prefixes = ['/'.join(parts[:i]) for i in range(1, len(parts) + 1)]
        if any(fnmatch.fnmatch(pre, g) for pre in prefixes for g in include):
            keep.update(prefixes)
    for n in clean:
        parts = n.split('/')
        if parts[-1].startswith('._') and '/'.join(parts[:-1] + [parts[-1][2:]]) in keep:
            keep.add(n)
    return [name for name, n in zip(names, clean) if n in keep]


# Directories a pack may carry that must stay root's: tar -x gives each its recorded owner and mode.
ROOT_DIRS = {'private', 'private/var', 'private/var/folders', 'private/var/db', 'private/var/db/lsd', 'Library', 'Library/Caches', 'Users'}


def check_owners(pack):
    """A pack made on the tablet as root (collect) records root:wheel for the system's directories; one made anywhere else (a test,
    a copy taken as a user) would hand them to someone else at the tablet's boot. Refused unless every such entry is uid 0."""
    import tarfile
    wrong = []
    with tarfile.open(pack, 'r:xz') as t:
        for m in t.getmembers():
            if m.name.rstrip('/') in ROOT_DIRS and m.uid != 0:
                wrong.append(f'{m.name} uid {m.uid}')
    if wrong:
        raise SystemExit(f'{pack}: not a pack collect made as root on the tablet ({", ".join(wrong[:4])}): extracting it at boot '
                         'would change those directories\' owners (--test-pack only for a dry run on this Mac)')


def filter_pack(source, dest, include):
    """DEST: SOURCE's members that selected() keeps, copied entry by entry (Python's tarfile, xz): owners, modes, times and pax
    headers as recorded; nothing extracted on this Mac. Returns the number of members written."""
    import tarfile
    with tarfile.open(source, 'r:xz') as tin:
        members = tin.getmembers()
        keep = set(selected([m.name for m in members], include, False))
        written = 0
        with tarfile.open(dest, 'w:xz', format=tarfile.PAX_FORMAT) as tout:
            for m in members:
                if m.name not in keep:
                    continue
                tout.addfile(m, tin.extractfile(m) if m.isreg() else None)
                written += 1
    return written


def listing(pack):
    """(name, size) of every member of PACK (bsdtar -tv on this Mac; owners are not needed here)."""
    out = subprocess.run(['tar', '-t', '-v', '-f', str(pack)], check=True, capture_output=True, text=True).stdout
    members = []
    for line in out.splitlines():
        parts = line.split(None, 8)
        if len(parts) < 9:
            continue
        name = parts[8].split(' -> ')[0]
        try:
            size = int(parts[4])
        except ValueError:
            size = 0
        members.append((name, size))
    return members


def summary(members, depth=6):
    sizes = {}
    for name, size in members:
        key = '/'.join(name.split('/')[:depth])
        sizes[key] = sizes.get(key, 0) + size
    return sorted(sizes.items(), key=lambda kv: -kv[1])


def load_stage_tools():
    spec = importlib.util.spec_from_file_location('stage_soft_metal', D / 'tooling/k27/stage-soft-metal.py')
    ssm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ssm)
    return ssm


APFS_VOLUME = '41504653-0000-11AA-AA11-00306543ECAC'


def sudo_ok():
    return subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode == 0


class Mounted:
    """The seed unpacked to a sparse raw file in SCRATCH, attached (-nomount) and its APFS volume mounted with mount_apfs through
    sudo -n, owners honoured (read only unless WRITE). Not diskutil mount: on the build Mac it hangs on these images (2026-11-14,
    twice), while mount_apfs mounts at once."""

    def __init__(self, ssm, seed, scratch, write=False):
        self.ssm, self.seed, self.scratch, self.write = ssm, seed, Path(scratch), write

    def __enter__(self):
        logical, extents = self.ssm.packed_ramdisk.regions(self.seed.read_bytes())
        self.raw = self.scratch / 'seed.raw'
        with self.raw.open('wb') as handle:
            handle.truncate(logical)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
        attached = plistlib.loads(self.ssm.run('hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nomount', '-nobrowse',
                                               '-plist', str(self.raw), capture_output=True).stdout)
        entities = attached['system-entities']
        self.whole = min((e['dev-entry'] for e in entities), key=len).removeprefix('/dev/')
        self.mount = self.scratch / 'mnt'
        self.mount.mkdir()
        try:
            volumes = [e['dev-entry'] for e in entities if e.get('content-hint') == APFS_VOLUME]
            if len(volumes) != 1:
                raise SystemExit(f'{self.seed}: {len(volumes)} APFS volumes in the seed (one, the Data volume, expected)')
            self.volume = volumes[0]
            options = 'nobrowse' if self.write else 'rdonly,nobrowse'
            subprocess.run(['sudo', '-n', '/sbin/mount_apfs', '-o', options, self.volume, str(self.mount)], check=True, timeout=300)
        except BaseException:
            subprocess.run(['hdiutil', 'detach', '/dev/' + self.whole], capture_output=True, timeout=120)
            raise
        return self

    def __exit__(self, *exc):
        try:
            subprocess.run(['sudo', '-n', '/sbin/umount', str(self.mount)], check=True, timeout=300)
        finally:
            subprocess.run(['hdiutil', 'detach', '/dev/' + self.whole], check=True, capture_output=True, timeout=120)


def cmd_inspect(a):
    if not sudo_ok():
        raise SystemExit('inspect needs sudo -n for mount_apfs (diskutil mount hangs on these images on the build Mac)')
    ssm = load_stage_tools()
    with tempfile.TemporaryDirectory(prefix='warm-inspect-', dir=str(a.scratch)) as scratch:
        with Mounted(ssm, a.seed, scratch) as m:
            root = m.mount
            for rel in ['private/etc/rc.server', PACK_DEST, 'private/var/db/lsd']:
                p = root / rel
                print(f'/{rel}: {"present" if p.exists() else "absent"}')
            folders = sorted((root / 'private/var/folders').glob('*/*'))
            print(f'/private/var/folders: {len(folders)} per-user folders')
            for f in folders:
                st = f.stat()
                found = []
                for pattern in ['0/com.apple.LaunchServices*', 'C/com.apple.FontRegistry', 'C/com.apple.metal', 'C/com.apple.iconservices*']:
                    for hit in f.glob(pattern):
                        size = sum(x.stat().st_size for x in hit.rglob('*') if x.is_file()) if hit.is_dir() else hit.stat().st_size
                        found.append(f'{hit.relative_to(f)} {size} bytes')
                print(f'  {f.relative_to(root)} (uid {st.st_uid}): {"; ".join(found) or "none of them"}')
            for p in sorted(root.glob('Library/Caches/com.apple.iconservices*')):
                print(f'/{p.relative_to(root)}: present')
            caches = root / 'Library/Caches'
            print(f'/Library/Caches: {len(os.listdir(caches)) if caches.is_dir() else "absent"} entries')
            for home in sorted(p for p in (root / 'Users').iterdir() if p.is_dir() and p.name != 'Shared'):
                lib = home / 'Library'
                print(f'a home\'s Library: {" ".join(sorted(os.listdir(lib))) if lib.is_dir() else "absent"}')
    return 0


def cmd_collect(a):
    a.out.mkdir(parents=True, exist_ok=True)
    pack = a.out / 'warm.tar.xz'
    secret = Path(a.secret)
    if not secret.is_file():
        raise SystemExit(f'{secret}: missing (the debug account\'s password for sudo on the tablet)')
    remote = "sudo -S -p '' /bin/sh -c '" + tablet_script() + "'"
    ssh = [str(D / 'tooling/native/desk-ssh.sh')] + (['-a', a.ssh_address] if a.ssh_address else []) + [remote]
    with secret.open('rb') as stdin, pack.open('wb') as stdout:
        r = subprocess.run(ssh, stdin=stdin, stdout=stdout)
    # bsdtar answers 1 for warnings (a socket skipped, a file that changed while read): the archive is still whole
    if r.returncode not in (0, 1) or pack.stat().st_size < 1024:
        raise SystemExit(f'collect: ssh/tar answered {r.returncode}, {pack.stat().st_size} bytes: nothing usable (USB network up? '
                         'logged in? tooling/native/desk-ssh.sh true)')
    members = listing(pack)
    record = {'pack': pack.name, 'pack_sha256': sha(pack), 'pack_bytes': pack.stat().st_size, 'members': len(members),
              'content_bytes': sum(s for _, s in members), 'collected': COLLECT,
              'largest': [{'path': k, 'bytes': v} for k, v in summary(members)[:40]]}
    (a.out / 'collect.json').write_text(json.dumps(record, indent=1) + '\n')
    print(f'{pack}: {record["pack_bytes"]} bytes, {len(members)} members, {record["content_bytes"]} bytes of content')
    for k, v in summary(members)[:15]:
        print(f'  {v:>12}  {k}')
    return 0


def cmd_build(a):
    for f in (a.seed, a.trust, a.pack):
        if not f.is_file():
            raise SystemExit(f'{f}: missing')
    if a.out.resolve() in (a.seed.resolve().parent, a.trust.resolve().parent):
        raise SystemExit('--out is an input\'s directory: the inputs are only read')
    a.out.mkdir(parents=True, exist_ok=True)
    if not a.test_pack:
        check_owners(a.pack)
    filtered = a.out / 'warm.tar.xz'
    if a.all:
        shutil.copyfile(a.pack, filtered)
    elif not filter_pack(a.pack, filtered, a.include or DEFAULT_INCLUDE):
        raise SystemExit(f'{a.pack}: nothing matches {a.include or DEFAULT_INCLUDE}')
    pack_id = sha(filtered)[:16]
    content = sum(s for n, s in listing(filtered))
    print(f'{filtered}: {filtered.stat().st_size} bytes ({content} bytes of content), pack {pack_id}')
    ssm = load_stage_tools()
    trust = a.out / 'kernel.macho.trustcache'
    shutil.copyfile(a.trust, trust)
    if not sudo_ok():
        raise SystemExit('build needs sudo -n: mount_apfs (diskutil mount hangs on the build Mac) and the two files made root\'s')
    record = {'inputs': {'seed': str(a.seed), 'seed_sha256': sha(a.seed), 'trust': str(a.trust), 'trust_sha256': sha(a.trust),
                         'pack': str(a.pack), 'pack_sha256': sha(a.pack)},
              'warm': {'pack_sha256': sha(filtered), 'pack_bytes': filtered.stat().st_size, 'content_bytes': content,
                       'members': len(listing(filtered)), 'include': None if a.all else (a.include or DEFAULT_INCLUDE),
                       'dest': '/' + PACK_DEST, 'rc_server': '/' + RC_SERVER}}
    with tempfile.TemporaryDirectory(prefix='warm-build-', dir=str(a.out)) as scratch:
        rc = Path(scratch) / 'rc.server'
        rc.write_text(rc_server(pack_id))
        with Mounted(ssm, a.seed, scratch, write=True) as m:
            if (m.mount / RC_SERVER).exists():
                raise SystemExit(f'{a.seed}: has /{RC_SERVER} already (not a seed this tool expects)')
            pack_target = m.mount / PACK_DEST
            # root's, as launchd expects of what it runs at boot (owners honoured on this mount)
            subprocess.run(['sudo', '-n', 'install', '-d', '-o', '0', '-g', '0', '-m', '755', str(pack_target.parent)], check=True)
            subprocess.run(['sudo', '-n', 'install', '-o', '0', '-g', '0', '-m', '600', str(filtered), str(pack_target)], check=True)
            subprocess.run(['sudo', '-n', 'install', '-o', '0', '-g', '0', '-m', '755', str(rc), str(m.mount / RC_SERVER)], check=True)
            subprocess.run(['sudo', '-n', 'xattr', '-c', str(pack_target), str(m.mount / RC_SERVER)], check=False)
            st = (m.mount / RC_SERVER).stat()
            record['warm']['rc_server_owner'] = f'{st.st_uid}:{st.st_gid} {oct(st.st_mode & 0o7777)}'
            if st.st_uid != 0:
                raise SystemExit(f'/{RC_SERVER} is uid {st.st_uid}\'s on this mount, not root\'s: launchd may refuse it')
            print(f'/{PACK_DEST} and /{RC_SERVER} on {m.volume} (root:wheel)')
        seed = a.out / 'scratch-data.img'
        seed.unlink(missing_ok=True)
        meta = ssm.packed_ramdisk.pack(m.raw, seed)
    before = a.seed.stat().st_size
    record['output'] = {'seed_sha256': sha(seed), 'identity': meta['sha256'], 'trust_sha256': sha(trust),
                        'seed_bytes': seed.stat().st_size, 'grew_bytes': seed.stat().st_size - before}
    (a.out / 'stage.json').write_text(json.dumps(record, indent=1) + '\n')
    # the registry verify-ram-images.py reads: an image with one of these seeds must run data-persist=0
    registry = D / 'out/warm-seeds.json'
    known = json.loads(registry.read_text()) if registry.is_file() else {}
    known[record['output']['seed_sha256']] = {'identity': meta['sha256'], 'pack_sha256': record['warm']['pack_sha256'],
                                              'pack_bytes': record['warm']['pack_bytes'], 'out': str(a.out),
                                              'base_seed_sha256': record['inputs']['seed_sha256']}
    registry.parent.mkdir(parents=True, exist_ok=True)
    registry.write_text(json.dumps(known, indent=1) + '\n')
    print(f'{seed}: {seed.stat().st_size} bytes packed (+{seed.stat().st_size - before} over the input), identity {meta["sha256"][:16]}: '
          'a new identity, so build-ram-images.py gives its images data-persist=0; the RAM image must stay under 250 MB')
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('inspect')
    i.add_argument('--seed', type=Path, default=DEFAULT_SEED)
    i.add_argument('--scratch', type=Path, default=Path(tempfile.gettempdir()))
    c = sub.add_parser('collect')
    c.add_argument('--out', type=Path, required=True)
    c.add_argument('--ssh-address')
    c.add_argument('--secret', default=str(D / 'private/login.secret'))
    b = sub.add_parser('build')
    b.add_argument('--pack', type=Path, required=True)
    b.add_argument('--out', type=Path, required=True)
    b.add_argument('--seed', type=Path, default=DEFAULT_SEED)
    b.add_argument('--trust', type=Path, default=DEFAULT_TRUST)
    b.add_argument('--include', action='append', default=[])
    b.add_argument('--all', action='store_true')
    b.add_argument('--test-pack', action='store_true', help='skip the root-owner check (a dummy pack, a dry run of the build here)')
    a = p.parse_args()
    return {'inspect': cmd_inspect, 'collect': cmd_collect, 'build': cmd_build}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
