#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
"""Is a built Metal plugin bundle independent of the checkout it was built in? (tooling/metal/build-turnip-a830.sh step 7,
docs/data-store.md section 9.)

The plugin's CDHashes go into the installed trust cache and its files into the system layer, so main and every worktree must build
the same bytes. Mesa embeds __FILE__ (vk_error, mesa_log) as the compile command names the source: relative to the build directory,
../mesa/src/... and src/..., the same from every checkout. Refused (exit status 1), with the strings:
  - a path of the checkout (--checkout, its real path, and for a worktree under .claude/worktrees/ the main checkout it belongs to);
  - .claude/worktrees/ anywhere;
  - a string that starts by climbing two directories or more (../../...): a build directory configured for another checkout's tree
    (2026-11-12: ../../../../../../out/turnip-a830/mesa/... in 51 strings, another libvulkan_freedreno.dylib).
Reported, not refused: other absolute paths under the home directory (machine-dependent, the same from every checkout of this Mac),
such as the prebuilt bridge archive's place in the k27 tree, which ld writes into the plugin's debug map (N_OSO) because the bridge's
Rust objects carry DWARF.

  check-bundle-paths.py BUNDLE|FILE... --checkout DIR [--checkout DIR]
  check-bundle-paths.py --self-test
"""
import argparse
import os
import re
import sys
import tempfile
from pathlib import Path

PRINTABLE = re.compile(rb'[\x20-\x7e]{4,}')
CLIMB = re.compile(rb'^(?:\.\./){2,}')
WORKTREES = b'/.claude/worktrees/'


def checkout_paths(checkouts):
    """The byte strings that name a checkout: each one as given and real, and a worktree's main checkout."""
    out = set()
    for c in checkouts:
        for p in {str(c), os.path.realpath(c)}:
            p = p.rstrip('/')
            out.add(p.encode())
            if '/.claude/worktrees/' in p:
                out.add(p.split('/.claude/worktrees/')[0].encode())
    return sorted(out, key=len, reverse=True)


def scan(data, checkouts, home):
    """(refusals, machine-dependent) for one file's bytes: lists of (offset, string)."""
    named = checkout_paths(checkouts)
    refused, machine = [], []
    for m in PRINTABLE.finditer(data):
        s = m.group()
        if any(n in s for n in named) or WORKTREES in s or CLIMB.match(s):
            refused.append((m.start(), s))
        elif home and home in s:
            machine.append((m.start(), s))
    return refused, machine


def files_of(paths):
    out = []
    for p in paths:
        p = Path(p)
        out += sorted(f for f in p.rglob('*') if f.is_file()) if p.is_dir() else [p]
    return out


def show(s, home):
    text = s.decode(errors='replace')
    return (text.replace(home.decode(), '~') if home else text)[:160]


def check(paths, checkouts, home=None):
    home = (home if home is not None else os.path.expanduser('~')).encode()
    bad = 0
    for f in files_of(paths):
        refused, machine = scan(f.read_bytes(), checkouts, home)
        for offset, s in refused[:8]:
            print(f'{f.name}: +{offset:#x}: {show(s, home)}', file=sys.stderr)
        if refused:
            print(f'{f}: {len(refused)} string(s) that depend on the checkout: this bundle is not the same from every checkout '
                  '(a build directory copied from another checkout? remove out/turnip-a830/build and build again)', file=sys.stderr)
            bad += 1
        if machine:
            unique = sorted({show(s, home).split('(')[0] for _, s in machine})
            print(f'{f.name}: {len(machine)} machine-dependent string(s) outside the checkout (the same from every checkout of this Mac): '
                  + '; '.join(unique[:3]))
    if not bad:
        print(f'paths: no checkout path in {", ".join(str(p) for p in paths)}')
    return bad == 0


def self_test():
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        main = tmp / 'UniXNU-piano'
        wt = main / '.claude/worktrees/agent-x'
        wt.mkdir(parents=True)
        home = str(tmp)

        def run(name, data, checkout, want):
            f = tmp / name
            f.write_bytes(data)
            with open(os.devnull, 'w') as null:
                saved, sys.stdout, sys.stderr = (sys.stdout, sys.stderr), null, null
                try:
                    got = check([f], [checkout], home=home)
                finally:
                    sys.stdout, sys.stderr = saved
            print(('  ok    ' if got == want else '  FAIL  ') + name)
            if got != want:
                failures.append(name)

        clean = b'\0../mesa/src/freedreno/vulkan/tu_device.cc\0src/vulkan/runtime/vk_cmd_queue.c\0'
        run('clean (relative to the build directory)', clean, wt, True)
        run('a climbing source path', b'\0../../../../../../out/turnip-a830/mesa/src/util/u_debug.c\0', wt, False)
        run('the worktree path', b'\0' + str(wt).encode() + b'/out/turnip-a830/plugin/frontend.mm\0', wt, False)
        run('the main checkout from a worktree', b'\0' + str(main).encode() + b'/out/turnip-a830/abi/x.h\0', wt, False)
        run('a worktree marker alone', b'\0/somewhere/.claude/worktrees/agent-y/x.c\0', main, False)
        run('a climb inside a string (Rust std) is not one', b'\0/build/rust/library/std/src/../../backtrace/src/mod.rs\0', wt, True)
        run('machine-dependent paths are reported, not refused', b'\0' + home.encode() + b'/k27/out/metal/bridge/lib.a(x.o)\0', wt, True)
    print(f'{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    return not failures


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('paths', nargs='*', type=Path)
    p.add_argument('--checkout', action='append', default=[], type=Path)
    p.add_argument('--self-test', action='store_true')
    a = p.parse_args()
    if a.self_test:
        sys.exit(0 if self_test() else 1)
    if not a.paths or not a.checkout:
        p.error('BUNDLE and --checkout DIR')
    sys.exit(0 if check(a.paths, a.checkout) else 1)


if __name__ == '__main__':
    main()
