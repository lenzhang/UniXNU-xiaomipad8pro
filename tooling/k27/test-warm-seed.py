#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host test of tooling/k27/warm-seed.py's pure parts (no seed, no sudo, no tablet): the member selection (a kept member's every
parent directory and its AppleDouble entry, nothing else), the pack filter copying entries with their owners, the owner guard, the
tablet's collector (no single quote: it travels inside sh -c '...'; tar's patterns quoted), and rc.server. The build itself (mount,
root-owned files, the packed seed) runs end to end with `warm-seed.py build --test-pack` on a dummy pack (docs/boot-time.md §10.1)."""
import importlib.util
import io
import sys
import tarfile
import tempfile
from pathlib import Path

D = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('warm_seed', D / 'tooling/k27/warm-seed.py')
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)

names = ['private/', 'private/var/', 'private/var/folders/', 'private/var/folders/zz/', 'private/var/folders/zz/h/',
         'private/var/folders/zz/h/0/', 'private/var/folders/zz/h/0/com.apple.LaunchServices.dv/',
         'private/var/folders/zz/h/0/com.apple.LaunchServices.dv/a.csstore', 'private/var/folders/zz/h/0/com.apple.LaunchServices.dv/._a.csstore',
         'private/var/folders/zz/h/0/other/', 'private/var/folders/zz/h/0/other/x', 'private/var/folders/zz/h/0/._other',
         'private/var/folders/zz/h/C/', 'private/var/folders/zz/h/C/com.apple.FontRegistry/', 'private/var/folders/zz/h/C/com.apple.FontRegistry/f',
         'Library/', 'Library/Caches/', 'Library/Caches/com.apple.iconservices.store/', 'Library/Caches/com.apple.iconservices.store/i',
         'Library/Caches/com.apple.other/', 'private/var/db/', 'private/var/db/lsd/', 'private/var/db/lsd/p']
keep = w.selected(names, w.DEFAULT_INCLUDE, False)
want = {'private/', 'private/var/', 'private/var/folders/', 'private/var/folders/zz/', 'private/var/folders/zz/h/', 'private/var/folders/zz/h/0/',
        'private/var/folders/zz/h/0/com.apple.LaunchServices.dv/', 'private/var/folders/zz/h/0/com.apple.LaunchServices.dv/a.csstore',
        'private/var/folders/zz/h/0/com.apple.LaunchServices.dv/._a.csstore', 'private/var/folders/zz/h/C/',
        'private/var/folders/zz/h/C/com.apple.FontRegistry/', 'private/var/folders/zz/h/C/com.apple.FontRegistry/f', 'Library/',
        'Library/Caches/', 'Library/Caches/com.apple.iconservices.store/', 'Library/Caches/com.apple.iconservices.store/i',
        'private/var/db/', 'private/var/db/lsd/', 'private/var/db/lsd/p'}
assert set(keep) == want, sorted(set(keep) ^ want)
assert keep == [n for n in names if n in want]   # archive order
assert w.selected(names, [], True) == names

with tempfile.TemporaryDirectory() as tmp:
    src, dst = Path(tmp) / 'in.tar.xz', Path(tmp) / 'out.tar.xz'
    with tarfile.open(src, 'w:xz', format=tarfile.PAX_FORMAT) as t:
        for n in names:
            info = tarfile.TarInfo(n.rstrip('/'))
            info.uid, info.gid = (501, 20) if '/h/' in n else (0, 0)
            if n.endswith('/'):
                info.type, info.mode = tarfile.DIRTYPE, 0o700 if n.endswith('/h/') else 0o755
                t.addfile(info)
            else:
                data = n.encode()
                info.size, info.mode = len(data), 0o600
                info.pax_headers = {'LIBARCHIVE.xattr.com.apple.test': 'eA=='}
                t.addfile(info, io.BytesIO(data))
    assert w.filter_pack(src, dst, w.DEFAULT_INCLUDE) == len(want)
    with tarfile.open(dst, 'r:xz') as t:
        got = {m.name: m for m in t.getmembers()}
        assert set(got) == {n.rstrip('/') for n in want}
        h = got['private/var/folders/zz/h']
        assert h.uid == 501 and h.mode == 0o700 and h.isdir()          # the per-user folder keeps its owner and mode
        f = got['private/var/folders/zz/h/0/com.apple.LaunchServices.dv/a.csstore']
        assert f.uid == 501 and t.extractfile(f).read() == f.name.encode() and f.pax_headers.get('LIBARCHIVE.xattr.com.apple.test') == 'eA=='
    w.check_owners(dst)   # private, private/var, ... are root's: accepted
    bad = Path(tmp) / 'bad.tar.xz'
    with tarfile.open(bad, 'w:xz') as t:
        info = tarfile.TarInfo('private/var')
        info.type, info.uid = tarfile.DIRTYPE, 501
        t.addfile(info)
    try:
        w.check_owners(bad)
        raise AssertionError('a user-owned private/var accepted')
    except SystemExit:
        pass

script = w.tablet_script()
assert "'" not in script and '--numeric-owner' in script and '--exclude "private/var/folders/*/*/T"' in script
rc = w.rc_server('0123456789abcdef')
assert rc.startswith('#!/bin/sh\n') and '/private/var/piano/warm/warm.tar.xz' in rc and '--numeric-owner' in rc and '> /dev/console' in rc
print('test-warm-seed: PASS (selection with parents and AppleDouble entries, the filter keeps owners/modes/xattrs, the owner guard, '
      'the collector and rc.server)')
