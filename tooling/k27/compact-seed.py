#!/usr/bin/env python3
"""A Data seed without the pages of deleted files: the volume's free space is overwritten with zeros, which the packed format
(DarwinBoot's DBRDISK1: nonzero pages only) leaves out.

  compact-seed.py IN.img OUT.img [--work DIR] [--grow-gib N]

--grow-gib N: the disk and its APFS container grown to N GiB first (diskutil apfs resizeContainer on the whole-disk container; the
Data volume keeps its UUID and files): the 27.3 line's store seed (docs/k273-line.md), whose window on linux_data has room for more
than the 27.0.1 line's 12 GiB. A seed of another size is another store layout: only a store bound to its window (data-store-lba)
may use one (tooling/persist/common.py check_store_size).

Why: a staging step that replaces files (tooling/k27/stage-soft-metal.py replaces the base seed's Metal plugin bundle, 40 MB)
frees the old file's blocks, but they keep their bytes, so the packed seed carries both the old and the new copy: the 27.0.1
line's Turnip seed is 124.7 MB for 81 MB of files. The RAM image's payload has to fit the UEFI's 256 MiB kernel region
(docs/k273-line.md §4), and the 27.3 line's kernel collection is 43 MB larger than the 27.0.1 line's.

The seed is unpacked to a sparse raw file in DIR (default: beside OUT), attached with hdiutil (no mount), its APFS volume mounted
(no sudo), a file of zeros written until the volume is full, synced and deleted, the volume unmounted and the disk packed again.
The files, their owners and modes are not touched; the volume's free blocks become zeros. A new seed is a new seed identity
(docs/data-store.md): images with it run Data volatile unless they say otherwise.
"""
import argparse
import os
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k27_tree import k27_tree  # noqa: E402
sys.path.insert(0, str(k27_tree() / 'sources/UniXNU/drivers/DarwinBoot/Test'))
import packed_ramdisk  # noqa: E402  (read from the k27 tree, not changed)


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('seed', type=Path)
    p.add_argument('out', type=Path)
    p.add_argument('--work', type=Path)
    p.add_argument('--grow-gib', type=int)
    p.add_argument('--disk-only', action='store_true',
                   help='with --grow-gib: only the disk grows (no hdiutil/diskutil: no zero fill, the APFS container keeps its size); '
                        'grow the container later on the running system (diskutil apfs resizeContainer DISK 0)')
    a = p.parse_args()
    a.out.parent.mkdir(parents=True, exist_ok=True)
    before = packed_ramdisk.inspect(a.seed)[0]
    logical, extents = packed_ramdisk.regions(a.seed.read_bytes())
    if a.disk_only:
        if not a.grow_gib or a.grow_gib << 30 <= logical:
            raise SystemExit('--disk-only needs --grow-gib larger than the seed')
        with tempfile.TemporaryDirectory(prefix='seed-grow-', dir=str(a.work or a.out.parent)) as scratch:
            raw = Path(scratch) / 'seed.raw'
            with raw.open('wb') as handle:
                handle.truncate(a.grow_gib << 30)
                for offset, data in extents:
                    handle.seek(offset)
                    handle.write(data)
            a.out.unlink(missing_ok=True)
            meta = packed_ramdisk.pack(raw, a.out)
        print(f'{a.out}: the disk grown to {a.grow_gib} GiB (the container unchanged: resize it on the system), {meta["encoded_bytes"]} '
              f'bytes packed, logical {meta["bytes"]}, identity {meta["sha256"][:16]} (input {before["sha256"][:16]})')
        return
    with tempfile.TemporaryDirectory(prefix='seed-compact-', dir=str(a.work or a.out.parent)) as scratch:
        raw = Path(scratch) / 'seed.raw'
        with raw.open('wb') as handle:
            handle.truncate(logical)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
            if a.grow_gib and a.grow_gib << 30 <= logical:
                raise SystemExit(f'--grow-gib {a.grow_gib}: the seed is {logical} bytes already')
        # hdiutil attaches and mounts in one step (stage.py's pattern since fdf426d): no diskutil, whose mount hangs some days here
        mount = Path(scratch) / 'mnt'
        mount.mkdir()
        attached = plistlib.loads(subprocess.run(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse',
                                                  '-mountpoint', str(mount), '-plist', str(raw)], check=True, capture_output=True,
                                                 timeout=300).stdout)['system-entities']
        whole = min((e['dev-entry'] for e in attached), key=lambda d: (len(d), d))
        disk_of = [sys.executable, str(D / 'tooling/k273/disk-of.py')]
        try:
            volume = next((e['dev-entry'] for e in attached if e.get('mount-point') == str(mount.resolve())), None)
            if volume is None:
                raise SystemExit(f'{raw.name}: hdiutil attached the seed but mounted no volume at {mount}; nothing filled')
            # a directory this user may write without sudo (the volume root is root's 755)
            where = next((mount / d for d in ('private/tmp', 'private/var/tmp', 'Library/GPUBundles')
                          if (mount / d).is_dir() and os.access(mount / d, os.W_OK)), None)
            if where is None:
                raise SystemExit(f'{volume}: no writable directory for the zero fill')
            fill = where / '.piano-zero-fill'
            zero = bytes(8 << 20)
            written = 0
            with fill.open('wb', buffering=0) as handle:
                try:
                    while True:
                        written += handle.write(zero)
                except OSError:
                    pass
                os.fsync(handle.fileno())
            fill.unlink()
            run('sync')
            print(f'{volume}: {written >> 20} MiB of free space overwritten with zeros')
        finally:
            run('hdiutil', 'detach', whole, stdout=subprocess.DEVNULL)
        # The growth comes after the zero fill: the new space is never written (a sparse file keeps it as holes), so a large grown
        # seed costs no disk space here (2026-11-14: filling a 176 GiB container ran the build disk full).
        if a.grow_gib:
            with raw.open('r+b') as handle:
                handle.truncate(a.grow_gib << 30)
            run('hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nomount', '-nobrowse', str(raw),
                stdout=subprocess.DEVNULL)
            whole = subprocess.run([*disk_of, 'whole', str(raw)], check=True, capture_output=True, text=True).stdout.strip()
            try:
                volume = subprocess.run([*disk_of, 'volume', whole], check=True, capture_output=True, text=True).stdout.strip()
                container = volume.rsplit('s', 1)[0]   # /dev/diskNs1 -> /dev/diskN, the container on the image's disk
                run('diskutil', 'apfs', 'resizeContainer', container, '0', stdout=subprocess.DEVNULL)
                print(f'{container}: grown to {a.grow_gib} GiB')
            finally:
                run('hdiutil', 'detach', whole, stdout=subprocess.DEVNULL)
        a.out.unlink(missing_ok=True)
        meta = packed_ramdisk.pack(raw, a.out)
    print(f'{a.out}: {meta["encoded_bytes"]} bytes packed (was {a.seed.stat().st_size}), logical {meta["bytes"]}, '
          f'identity {meta["sha256"][:16]} (input {before["sha256"][:16]})')


if __name__ == '__main__':
    main()
