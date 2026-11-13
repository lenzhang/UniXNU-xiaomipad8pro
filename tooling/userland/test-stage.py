#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
"""Host tests of the userland pipeline (docs/userland.md section 8); no tablet, nothing outside a temporary directory is written
(the repository's out/ gets the build outputs of the real units in part 7):

1. manifest   configs/userland.toml loads and checks; every refusal of load_manifest / check_jobs on a broken manifest
2. installer  install.sh (under /bin/sh and /bin/dash) on a scratch root with launchctl and logger stand-ins: the first install,
              a second run that changes nothing, --check and tampering, a torn staged file, a changed program (kickstart) and job
              file (bootout + bootstrap), the owner's launchctl disable, an off job (Disabled), nobody logged in, the receipt's removals;
              the system layer: a Metal plugin bundle already in place (the base's) taken over with a copy kept, given back when its
              unit goes, never taken over when that copy cannot be made
3. pmset      piano-pmset-defaults.sh with a pmset stand-in: both sources, keys this model lacks, a battery that appears later,
              the owner's timers first, the owner's change after a partial run, once and only once
4. trust      add_trust: one module first with the missing CDHashes only; none when all are there (byte for byte the input)
5. cdhash     stage-soft-metal.py's cdhash against codesign's CDHash for a fresh ad hoc (linker-signed) binary
6. seed       stage.py on a 64 MiB APFS image made here with hdiutil (packed as a DBRDISK1 seed): the staged files, modes and
              sha256s on the volume, the boot job's command inserted once after settime.sh and before an endless loop, a stale
              file removed, the Disabled plist, the trust cache and the record; the second run up to date (outputs byte for byte the
              same), --force restages with the command still once, the input seed never changed, --out over the input refused;
              install.sh run from the staged directory of the output seed (end to end)
7. live       stage.py --live on the test manifest (the trust check against an installed trust cache that lacks a CDHash, the
              archive's determinism and its SHA256SUMS) and on configs/userland.toml itself (every real unit builds and checks)

Parts 4-7 read stage-soft-metal.py, which needs the k27 tree (PIANO_K27_TREE): skipped without it.
  python3 tooling/userland/test-stage.py [--quick]      (--quick: parts 1-3 only)
"""
import contextlib
import hashlib
import io
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
D = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(D / 'tooling/k27'))
import importlib.util  # noqa: E402

spec = importlib.util.spec_from_file_location('stage', HERE / 'stage.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)
from k27_tree import k27_tree  # noqa: E402

failures = []
SHELLS = [s for s in ('/bin/sh', '/bin/dash') if Path(s).exists()]


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)


def refused(fn, *args, match=''):
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            fn(*args)
    except SystemExit as e:
        return match in str(e)
    return False


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write(path, text, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text) if isinstance(text, str) else path.write_bytes(text)
    os.chmod(path, mode)
    return path


def job_plist(label, argv, **extra):
    return plistlib.dumps({'Label': label, 'ProgramArguments': argv, 'RunAtLoad': True, **extra}, fmt=plistlib.FMT_XML)


# ---- 1. the manifest ---------------------------------------------------------------------------------------------------------

BASE = '''format = 1
staging = "/private/var/piano/userland"
'''


def unit_toml(name, files, default_on=True, extra=''):
    out = f'[[unit]]\nname = "{name}"\nabout = "test"\ndefault_on = {"true" if default_on else "false"}\n{extra}'
    for f in files:
        out += '\n[[unit.file]]\n' + ''.join(f'{k} = {json.dumps(v)}\n' for k, v in f.items())
    return out + '\n'


def manifest_tests(tmp):
    print('1. manifest')
    plan = stage.load_manifest(D / 'configs/userland.toml')
    names = {u['name']: u for u in plan['units']}
    check(set(names) >= {'pianousb-shell', 'piano-policy-agent', 'piano-rotated', 'piano-torch', 'piano-pmset-defaults',
                         'piano-still-wallpaper', 'piano-wifi'}, 'configs/userland.toml: the seven units')
    wall = {f['dest']: f for f in names['piano-still-wallpaper']['files']}
    check(sorted(wall) == ['/Library/LaunchDaemons/com.unixnu.piano-still-wallpaper.plist', '/usr/local/libexec/piano-still-wallpaper',
                           '/usr/local/share/piano/wallpaper/Index.plist', '/usr/local/share/piano/wallpaper/Metadata.plist',
                           '/usr/local/share/piano/wallpaper/still.png'] and
          all(not f['trust'] and f['mode'] == '0644' for d, f in wall.items() if d.startswith('/usr/local/share/')),
          'piano-still-wallpaper: the script, its job and its three data files (0644, no CDHash)')
    check(names['piano-wifi']['planned'] and not any(u['planned'] for n, u in names.items() if n != 'piano-wifi'),
          'piano-wifi is the one planned unit')
    check(all(u['on'] for u in plan['units']), 'every unit is on in the installed profile')
    dests = [f['dest'] for u in plan['units'] for f in u['files']]
    check(len(dests) == len(set(dests)) and all(d.rsplit('/', 1)[0] in stage.DIRS for d in dests), 'destinations unique, in the allowed directories')
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        stage.show_plan(plan)
    check('/Library/LaunchAgents/com.unixnu.piano-rotated.plist  root:wheel 0644  agent' in out.getvalue(), '--plan lists the agent')

    src = write(tmp / 'm/tool.c', 'int main(void) { return 0; }\n')
    plist = write(tmp / 'm/com.test.a.plist', job_plist('com.test.a', ['/usr/local/libexec/tool']).decode())
    good_tool = {'source': str(src), 'build': 'cc', 'dest': '/usr/local/libexec/tool', 'mode': '0755'}
    good_job = {'source': str(plist), 'dest': '/Library/LaunchDaemons/com.test.a.plist', 'mode': '0644'}

    def load(text, *, on=(), off=()):
        path = write(tmp / 'm/userland.toml', BASE + text)
        return stage.load_manifest(path, on, off)

    check(load(unit_toml('a', [good_tool, good_job]))['units'][0]['files'][1]['kind'] == 'daemon', 'a good manifest loads')
    bad = {
        'a dest outside the allowed directories': (unit_toml('a', [{**good_tool, 'dest': '/usr/bin/tool'}]), 'dest must be'),
        'a group-writable mode': (unit_toml('a', [{**good_tool, 'mode': '0775'}]), 'not writable'),
        'a job file not 0644': (unit_toml('a', [good_tool, {**good_job, 'mode': '0755'}]), 'launchd refuses'),
        'a job file not root\'s': (unit_toml('a', [good_tool, {**good_job, 'owner': 'nobody:wheel'}]), 'launchd refuses'),
        'a duplicate destination': (unit_toml('a', [good_tool]) + unit_toml('b', [good_tool]), 'is also unit a'),
        'two jobs in a unit': (unit_toml('a', [good_job, {**good_job, 'dest': '/Library/LaunchDaemons/com.test.b.plist'}]),
                               'at most one'),
        'off without a job': (unit_toml('a', [good_tool], default_on=False), 'needs a launchd job'),
        'an unknown key': (unit_toml('a', [{**good_tool, 'owners': 'x'}]), 'unknown keys'),
        'a missing source': (unit_toml('a', [{**good_tool, 'source': str(tmp / 'm/none.c')}]), 'no source'),
        'an unknown build': (unit_toml('a', [{**good_tool, 'build': 'make'}]), "build 'make'"),
        'an unknown arch': (unit_toml('a', [{**good_tool, 'arch': 'x86_64'}]), 'arch'),
        'a bad unit name': (unit_toml('A_b', [good_tool]), 'the name must be'),
        'a script without inputs': (unit_toml('a', [{'build': 'script', 'command': ['true'], 'output': 'x',
                                                      'dest': '/usr/local/bin/x', 'mode': '0755'}]), 'needs command'),
        'a staging outside /private/var': ('', 'staging'),
    }
    for what, (text, match) in bad.items():
        if what == 'a staging outside /private/var':
            path = write(tmp / 'm/userland.toml', BASE.replace('/private/var/piano/userland', '/usr/local/stage') + unit_toml('a', [good_tool]))
            check(refused(stage.load_manifest, path, match=match), f'refused: {what}')
        else:
            check(refused(load, text, match=match), f'refused: {what}')
    check(refused(lambda: load(unit_toml('a', [good_tool]), off=['a']), match='no job to turn off'), 'refused: --off of a tool')
    check(refused(lambda: load(unit_toml('a', [good_tool]), on=['zz']), match='no such unit'), 'refused: --on of no unit')
    planned = load(unit_toml('a', [{**good_tool, 'source': str(tmp / 'm/later.c')}], extra='status = "planned"\n'))
    check(planned['units'][0]['planned'], 'a planned unit may name a source that is not written yet')

    def jobs(data, dest='/Library/LaunchDaemons/com.test.a.plist'):
        plan = load(unit_toml('a', [good_tool, good_job]))
        f = {**plan['units'][0]['files'][1], 'dest': dest, 'name': dest.rsplit('/', 1)[1]}
        return stage.check_jobs(plan, [(f, data)])
    check(jobs(job_plist('com.test.a', ['/usr/local/libexec/tool'])) is None, 'check_jobs: a good job')
    check(refused(jobs, job_plist('com.test.b', ['/usr/local/libexec/tool']), match='is not the file name'), 'refused: a Label that is not the file name')
    check(refused(jobs, job_plist('com.test.a', ['/opt/x/tool']), match='neither installed'), 'refused: a program nobody installs')
    check(refused(jobs, job_plist('com.test.a', ['/bin/sh', '/usr/local/libexec/other']), match='does not install'),
          'refused: a /usr/local path the manifest does not install')
    check(refused(jobs, job_plist('com.test.a', ['/usr/local/libexec/tool'], Disabled=True), match='has Disabled'),
          'refused: Disabled written by hand')
    check(refused(jobs, b'not a plist', match='not a property list'), 'refused: a job file that is no plist')

    # --gpu-bundle: a Metal plugin bundle as one more unit (the system layer, docs/data-store.md section 9)
    b = tmp / 'm/T_x.bundle'
    write(b / 'Contents/Info.plist', plistlib.dumps({'CFBundleExecutable': 'T'}))
    write(b / 'Contents/MacOS/T', b'\xcf\xfa\xed\xfe' + bytes(60), 0o644)       # a Mach-O, even without its x bit
    write(b / 'Contents/MacOS/libt.dylib', b'\xcf\xfa\xed\xfe' + bytes(60), 0o755)
    plan = stage.add_gpu_bundles(load(unit_toml('a', [good_tool])), [b])
    unit = plan['units'][-1]
    files = {f['dest']: f for f in unit['files']}
    root = '/Library/GPUBundles/T_x.bundle/Contents'
    check(unit['name'] == 'gpu-t-x' and unit['on'] and sorted(files) == [f'{root}/Info.plist', f'{root}/MacOS/T', f'{root}/MacOS/libt.dylib'],
          '--gpu-bundle: one unit, every file of the bundle at /Library/GPUBundles')
    check(files[f'{root}/Info.plist']['mode'] == '0644' and files[f'{root}/MacOS/T']['mode'] == '0755' and
          files[f'{root}/MacOS/T']['staged'] == 'files/gpu-t-x/Contents/MacOS/T' and all(f['trust'] and f['kind'] == 'file' for f in files.values()),
          '--gpu-bundle: modes, staged paths below the unit, trusted, no job')
    check(refused(stage.add_gpu_bundles, load(unit_toml('a', [good_tool])), [tmp / 'm'], match='not a plugin bundle'), 'refused: --gpu-bundle of no bundle')
    check(refused(stage.add_gpu_bundles, load(unit_toml('a', [good_tool])), [b, b], match='twice'), 'refused: the same bundle twice')
    os.symlink('T', b / 'Contents/MacOS/link')
    check(refused(stage.add_gpu_bundles, load(unit_toml('a', [good_tool])), [b], match='neither a file'), 'refused: a symlink in the bundle')
    (b / 'Contents/MacOS/link').unlink()
    ok = ['/Library/GPUBundles/A.bundle/Contents/Info.plist', '/Library/GPUBundles/A.bundle/Contents/MacOS/A',
          '/Library/GPUBundles/A.bundle/Contents/Resources/x.metallib', '/usr/local/bin/x', '/usr/local/share/piano/wallpaper/x.png']
    no = ['/Library/GPUBundles/A.bundle/Info.plist', '/Library/GPUBundles/A.bundle/Contents/MacOS/../../x',
          '/Library/GPUBundles/A.bundle/Contents/MacOS/.hidden', '/Library/GPUBundles/x', '/Library/GPUBundles/A.kext/Contents/Info.plist',
          '/Library/GPUBundles/A.bundle/Contents/MacOS/sub/x', '/System/Library/Extensions/A.bundle/Contents/Info.plist',
          '/usr/local/share/x', '/usr/local/share/piano/x', '/usr/local/share/piano/wallpaper/sub/x']
    check(all(stage.dest_kind(d) == 'file' for d in ok) and not any(stage.dest_kind(d) for d in no),
          'dest_kind: a bundle\'s Contents/Info.plist, MacOS/ and Resources/ files only; /usr/local/share/piano/wallpaper/ only')


# ---- 2. the installer --------------------------------------------------------------------------------------------------------

LAUNCHCTL = r'''#!/bin/sh
# launchctl's stand-in: "domain/label" lines in $LAUNCHD/loaded, $LAUNCHD/disabled, $LAUNCHD/enabled; every call in $LAUNCHD/calls
L=$LAUNCHD
touch "$L/loaded" "$L/disabled" "$L/enabled"
echo "$*" >> "$L/calls"
case "$1" in
  print) grep -qxF "$2" "$L/loaded" ;;
  print-disabled) while read -r x; do case $x in "$2"/*) printf '\t"%s" => disabled\n' "${x#"$2"/}" ;; esac; done < "$L/disabled" ;;
  bootstrap) label=$(basename "$3" .plist)
    if grep -qxF "$2/$label" "$L/disabled"; then echo "Bootstrap failed: 119: Service is disabled" >&2; exit 119; fi
    if [ "$(plutil -extract Disabled raw -o - "$3" 2>/dev/null)" = true ] && ! grep -qxF "$2/$label" "$L/enabled"; then
      echo "Bootstrap failed: 119: Service is disabled" >&2; exit 119; fi
    if grep -qxF "$2/$label" "$L/loaded"; then echo "Bootstrap failed: 5: Input/output error" >&2; exit 5; fi
    echo "$2/$label" >> "$L/loaded" ;;
  bootout) grep -vxF "$2" "$L/loaded" > "$L/loaded.new"; mv "$L/loaded.new" "$L/loaded" ;;
  kickstart) grep -qxF "$3" "$L/loaded" ;;
  *) exit 64 ;;
esac
'''


class Rig:
    """A scratch root for install.sh: stage/ (the staged directory), root/ (standing for /), bin/ (stand-ins), launchd/ (state)."""

    def __init__(self, tmp, shell):
        self.tmp, self.shell = tmp, shell
        for d in ('stage', 'root', 'bin', 'launchd'):
            shutil.rmtree(tmp / d, ignore_errors=True)
            (tmp / d).mkdir(parents=True)
        write(tmp / 'bin/launchctl', LAUNCHCTL, 0o755)
        write(tmp / 'bin/logger', '#!/bin/sh\nexit 0\n', 0o755)
        write(tmp / 'root/dev/console', '')
        self.env = {**os.environ, 'PATH': f'{tmp / "bin"}:{os.environ["PATH"]}', 'LAUNCHD': str(tmp / 'launchd'),
                    'PIANO_USERLAND_ROOT': str(tmp / 'root'), 'PIANO_USERLAND_NOCHOWN': '1'}
        self.uid = os.getuid()

    def files(self, units):
        """Stage UNITS ([(name, on, [(dest, mode, bytes)])]) through stage.staging_tree, as stage.py would."""
        built = []
        for name, on, files in units:
            for dest, mode, data in files:
                kind = stage.dest_kind(dest)
                f = {'unit': name, 'dest': dest, 'name': dest.rsplit('/', 1)[1], 'kind': kind, 'owner': 'root:wheel', 'mode': mode,
                     'on': on, 'sha256': sha(data)}
                if dest.startswith(stage.GPU_BUNDLES + '/'):
                    f['staged'] = f'files/{name}/' + dest.split('.bundle/', 1)[1]
                if kind != 'file':
                    f['label'] = plistlib.loads(data)['Label']
                built.append((f, data))
        tree = stage.staging_tree({'staging': '/private/var/piano/userland'}, built)
        stage.sync_tree(self.tmp / 'stage', tree)
        return tree

    def run(self, mode='--boot'):
        (self.tmp / 'launchd/calls').write_text('')
        r = subprocess.run([self.shell, str(self.tmp / 'stage/install.sh'), mode], env=self.env, capture_output=True, text=True)
        self.calls = (self.tmp / 'launchd/calls').read_text().splitlines()
        return r.returncode, r.stdout + r.stderr

    def loaded(self):
        return set((self.tmp / 'launchd/loaded').read_text().split()) if (self.tmp / 'launchd/loaded').exists() else set()

    def root(self, dest):
        return self.tmp / 'root' / dest.lstrip('/')


def installer_tests(tmp):
    print('2. installer')
    tool = b'#!/bin/sh\necho tool\n'
    daemon = job_plist('com.test.d', ['/usr/local/libexec/tool'], KeepAlive=True)
    off = plistlib.dumps({**plistlib.loads(job_plist('com.test.off', ['/usr/local/libexec/tool'])), 'Disabled': True})
    agent = job_plist('com.test.ag', ['/usr/local/libexec/tool'])
    units = [('d', True, [('/usr/local/libexec/tool', '0755', tool), ('/Library/LaunchDaemons/com.test.d.plist', '0644', daemon)]),
             ('off', False, [('/Library/LaunchDaemons/com.test.off.plist', '0644', off)]),
             ('ag', True, [('/Library/LaunchAgents/com.test.ag.plist', '0644', agent)]),
             ('cli', True, [('/usr/local/bin/cli', '0755', b'#!/bin/sh\necho cli\n')])]
    for shell in SHELLS:
        tag = Path(shell).name
        rig = Rig(tmp / tag, shell)
        rig.files(units)
        code, out = rig.run()
        gui = f'gui/{rig.uid}'
        check(code == 0, f'{tag}: the first install succeeds')
        check(all(rig.root(d).is_file() for _, _, fs in units for d, _, _ in fs), f'{tag}: every file in place')
        check(oct(rig.root('/usr/local/libexec/tool').stat().st_mode & 0o7777) == '0o755' and
              oct(rig.root('/Library/LaunchDaemons/com.test.d.plist').stat().st_mode & 0o7777) == '0o644', f'{tag}: modes as the manifest says')
        check(rig.loaded() == {'system/com.test.d', f'{gui}/com.test.ag'}, f'{tag}: the on daemon and the agent loaded, the off one not')
        check((tmp / tag / 'root/private/var/db/piano-userland/receipt.tsv').read_text().count('\n') == 5, f'{tag}: the receipt has the five files')
        check('--boot done: ok' in (tmp / tag / 'root/private/var/db/piano-userland/last.txt').read_text(), f'{tag}: last.txt')
        code, out = rig.run()
        check(code == 0 and 'changed:)' in out and not [c for c in rig.calls if c.split()[0] in ('bootstrap', 'bootout', 'kickstart')],
              f'{tag}: a second run changes nothing and touches no job')
        code, out = rig.run('--check')
        check(code == 0 and 'com.test.off: off' in out, f'{tag}: --check passes, the off job reported off')
        rig.root('/usr/local/libexec/tool').write_bytes(b'#!/bin/sh\necho changed by hand\n')
        os.chmod(rig.root('/usr/local/bin/cli'), 0o700)
        code, out = rig.run('--check')
        check(code == 1 and '/usr/local/libexec/tool: sha256' in out and '/usr/local/bin/cli: 0700, the manifest\'s 0755' in out,
              f'{tag}: --check finds a changed file and a changed mode')
        code, out = rig.run('--live')
        check(code == 0 and 'kickstart -k system/com.test.d' in rig.calls and rig.root('/usr/local/libexec/tool').read_bytes() == tool and
              oct(rig.root('/usr/local/bin/cli').stat().st_mode & 0o7777) == '0o755', f'{tag}: repaired; the loaded daemon restarted (kickstart -k)')
        units2 = [('d', True, [('/usr/local/libexec/tool', '0755', tool),
                               ('/Library/LaunchDaemons/com.test.d.plist', '0644', job_plist('com.test.d', ['/usr/local/libexec/tool']))]),
                  *units[1:]]
        rig.files(units2)
        code, out = rig.run('--live')
        check(code == 0 and 'bootout system/com.test.d' in rig.calls and
              any(c.startswith('bootstrap system ') and c.endswith('com.test.d.plist') for c in rig.calls) and 'system/com.test.d' in rig.loaded(),
              f'{tag}: a changed job file: bootout and bootstrap')
        flipped = plistlib.dumps({**plistlib.loads(units2[0][2][1][2]), 'Disabled': True}, fmt=plistlib.FMT_XML)
        rig.files([('d', False, [units2[0][2][0], (units2[0][2][1][0], '0644', flipped)]), *units2[1:]])
        code, out = rig.run('--live')
        check(code == 0 and 'com.test.d: off now' in out and 'system/com.test.d' not in rig.loaded(), f'{tag}: a loaded job turned off: booted out')
        rig.files(units2)
        code, out = rig.run('--live')
        check(code == 0 and 'system/com.test.d' in rig.loaded(), f'{tag}: and on again: loaded')
        staged = tmp / tag / 'stage/files/cli/cli'
        staged.write_bytes(b'torn')
        rig.root('/usr/local/bin/cli').unlink()
        code, out = rig.run()
        check(code == 1 and 'a torn copy' in out and not rig.root('/usr/local/bin/cli').exists(), f'{tag}: a torn staged file is not installed')
        rig.files(units2)
        rig.run()
        (tmp / tag / 'launchd/loaded').write_text(f'{gui}/com.test.ag\n')
        (tmp / tag / 'launchd/disabled').write_text('system/com.test.d\n')
        code, out = rig.run()
        check(code == 0 and 'turned off by the owner' in out and 'system/com.test.d' not in rig.loaded(), f'{tag}: the owner\'s launchctl disable is kept')
        code, out = rig.run('--check')
        check(code == 0 and 'turned off by the owner' in out, f'{tag}: --check accepts a job the owner turned off')
        (tmp / tag / 'launchd/disabled').write_text('')
        (tmp / tag / 'launchd/enabled').write_text('system/com.test.off\n')
        rig.root('/Library/LaunchDaemons/com.test.off.plist').unlink()
        rig.run()
        check('system/com.test.off' not in rig.loaded(), f'{tag}: an off job is never loaded by the installer')
        (tmp / tag / 'root/dev/console').unlink()
        (tmp / tag / 'launchd/loaded').write_text('')
        code, out = rig.run()
        check('com.test.ag: an agent; nobody is logged in' in out and f'{gui}/com.test.ag' not in rig.loaded(), f'{tag}: nobody logged in: the agent waits')
        write(tmp / tag / 'root/dev/console', '')
        rig.root('/usr/local/bin/cli').write_bytes(b'#!/bin/sh\necho the owner\'s own\n')
        rig.files([u for u in units2 if u[0] not in ('d', 'cli')])
        (tmp / tag / 'launchd/loaded').write_text('system/com.test.d\n')
        code, out = rig.run()
        check(code == 0 and not rig.root('/usr/local/libexec/tool').exists() and not rig.root('/Library/LaunchDaemons/com.test.d.plist').exists()
              and 'bootout system/com.test.d' in rig.calls, f'{tag}: a unit dropped from the manifest: removed, its job booted out')
        check(rig.root('/usr/local/bin/cli').exists() and 'changed since it was installed: left' in out, f'{tag}: a dropped file changed since is left')
        check(subprocess.run([shell, str(tmp / tag / 'stage/install.sh'), '--bogus'], env=rig.env, capture_output=True).returncode == 2,
              f'{tag}: an unknown mode is refused')
        layer_tests(tmp, shell, tag)


def layer_tests(tmp, shell, tag):
    """The system layer (docs/data-store.md section 9) on the owner's Data: the base seed's plugin is in place, not installed by
    install.sh; the layer replaces one file and adopts two; taking the layer out gives the base's files back."""
    rig = Rig(tmp / f'{tag}-layer', shell)
    c = '/Library/GPUBundles/T.bundle/Contents'
    base = {f'{c}/Info.plist': (b'<plist/>', 0o644), f'{c}/MacOS/T': (b'exe', 0o755), f'{c}/MacOS/libx.dylib': (b'old dylib', 0o755)}
    for dest, (data, mode) in base.items():
        write(rig.root(dest), data, mode)
    cli = ('cli', True, [('/usr/local/bin/cli', '0755', b'#!/bin/sh\necho cli\n')])
    layer = ('gpu-t', True, [(f'{c}/Info.plist', '0644', b'<plist/>'), (f'{c}/MacOS/T', '0755', b'exe'),
                             (f'{c}/MacOS/libx.dylib', '0755', b'new dylib')])
    prev = tmp / f'{tag}-layer/root/private/var/db/piano-userland/prev'
    rig.files([cli, layer])
    code, out = rig.run('--live')
    check(code == 0 and rig.root(f'{c}/MacOS/libx.dylib').read_bytes() == b'new dylib' and
          rig.root(f'{c}/MacOS/T').read_bytes() == b'exe', f'{tag}: layer: the changed file installed, the same ones adopted')
    check(all((prev / d.lstrip('/')).read_bytes() == data for d, (data, _) in base.items()) and out.count('was not installed by this installer') == 3
          and not (prev / 'usr/local/bin/cli').exists(), f'{tag}: layer: a copy of each base file it took over, none of a new file')
    check((tmp / f'{tag}-layer/stage/files/gpu-t/Contents/MacOS/libx.dylib').is_file(),
          f'{tag}: layer: staged below its unit with the bundle\'s paths')
    code, out = rig.run('--live')
    check(code == 0 and 'changed:)' in out and (prev / c.lstrip('/') / 'MacOS/libx.dylib').read_bytes() == b'old dylib',
          f'{tag}: layer: a second run changes nothing; the copy is still the base\'s file')
    layer2 = ('gpu-t', True, [*layer[2][:2], (f'{c}/MacOS/libx.dylib', '0755', b'newer dylib')])
    rig.files([cli, layer2])
    code, out = rig.run('--live')
    check(code == 0 and rig.root(f'{c}/MacOS/libx.dylib').read_bytes() == b'newer dylib' and
          (prev / c.lstrip('/') / 'MacOS/libx.dylib').read_bytes() == b'old dylib', f'{tag}: layer: an update; the copy stays the base\'s')
    rig.files([cli])
    code, out = rig.run('--live')
    check(code == 0 and all(rig.root(d).read_bytes() == data for d, (data, _) in base.items()) and out.count('the file it had replaced is back') == 3,
          f'{tag}: layer taken out: the base\'s files back, byte for byte')
    check(not any(p.is_file() for p in prev.rglob('*')) and oct(rig.root(f'{c}/MacOS/T').stat().st_mode & 0o777) == '0o755',
          f'{tag}: layer taken out: no copies left, the modes of before')
    rig.files([cli, layer])
    rig.run('--live')
    rig.root(f'{c}/MacOS/libx.dylib').write_bytes(b'the owner\'s own')
    rig.files([cli])
    code, out = rig.run('--live')
    check(rig.root(f'{c}/MacOS/libx.dylib').read_bytes() == b'the owner\'s own' and (prev / c.lstrip('/') / 'MacOS/libx.dylib').is_file() and
          'changed since it was installed: left' in out, f'{tag}: layer: a file changed after the take-over is left, its copy kept')
    rig = Rig(tmp / f'{tag}-layer2', shell)
    for dest, (data, mode) in base.items():
        write(rig.root(dest), data, mode)
    write(rig.root('/private/var/db/piano-userland/prev'), 'not a directory')
    rig.files([cli, layer])
    code, out = rig.run('--live')
    check(code == 1 and rig.root(f'{c}/MacOS/libx.dylib').read_bytes() == b'old dylib' and 'not installed (gpu-t)' in out and
          rig.root('/usr/local/bin/cli').is_file(), f'{tag}: layer: no copy possible: the base\'s file is not replaced, the rest goes on')


# ---- 3. pmset defaults -------------------------------------------------------------------------------------------------------

PMSET = r'''#!/bin/sh
# pmset's stand-in: -g custom prints $PMSET_CUSTOM; -b/-c KEY VALUE writes powerd's settings file as powerd would; calls in $PMSET_CALLS
echo "$*" >> "$PMSET_CALLS"
if [ "$1" = -g ]; then cat "$PMSET_CUSTOM"; exit 0; fi
case "$1" in -b) s='Battery Power' ;; -c) s='AC Power' ;; *) exit 1 ;; esac
case "$2" in sleep) k='System Sleep Timer' ;; displaysleep) k='Display Sleep Timer' ;; *) k=$2 ;; esac
f=$PIANO_USERLAND_ROOT/Library/Preferences/com.apple.PowerManagement.plist
mkdir -p "$(dirname "$f")"
[ -f "$f" ] || plutil -create xml1 "$f"
plutil -extract "$s" xml1 -o - "$f" > /dev/null 2>&1 || plutil -insert "$s" -dictionary "$f"
plutil -replace "$s.$k" -integer "$3" "$f"
'''
CUSTOM_BATTERY = 'Battery Power:\n Sleep On Power Button 1\n standby              1\n hibernatemode        3\n powernap             1\n' \
                 ' displaysleep         10\n sleep                1\n tcpkeepalive         1\n'
CUSTOM_AC = 'AC Power:\n Sleep On Power Button 1\n hibernatemode        3\n displaysleep         10\n sleep                1\n'


def pmset_tests(tmp):
    print('3. pmset defaults')
    script = HERE / 'piano-pmset-defaults.sh'
    for shell in SHELLS:
        tag = Path(shell).name

        def rig(custom):
            base = tmp / f'pm-{tag}'
            shutil.rmtree(base, ignore_errors=True)
            write(base / 'bin/pmset', PMSET, 0o755)
            write(base / 'bin/logger', '#!/bin/sh\nexit 0\n', 0o755)
            write(base / 'custom', custom)
            env = {**os.environ, 'PATH': f'{base / "bin"}:{os.environ["PATH"]}', 'PIANO_USERLAND_ROOT': str(base / 'root'),
                   'PMSET_CUSTOM': str(base / 'custom'), 'PMSET_CALLS': str(base / 'calls')}
            return base, env

        def run(base, env):
            (base / 'calls').write_text('')
            r = subprocess.run([shell, str(script)], env=env, capture_output=True, text=True)
            return r.returncode, r.stdout, [c for c in (base / 'calls').read_text().splitlines() if not c.startswith('-g')]

        def prefs(base, source, key):
            r = subprocess.run(['plutil', '-extract', f'{source}.{key}', 'raw', '-o', '-',
                                str(base / 'root/Library/Preferences/com.apple.PowerManagement.plist')], capture_output=True, text=True)
            return r.stdout.strip() if r.returncode == 0 else None

        base, env = rig(CUSTOM_BATTERY + CUSTOM_AC)
        code, out, calls = run(base, env)
        check(code == 0 and calls == ['-b sleep 5', '-b displaysleep 2', '-b hibernatemode 0', '-b standby 0', '-b powernap 0',
                                      '-b tcpkeepalive 0', '-c sleep 0', '-c displaysleep 10'], f'{tag}: both sources set, in order')
        check(prefs(base, 'Battery Power', 'System Sleep Timer') == '5' and prefs(base, 'AC Power', 'System Sleep Timer') == '0',
              f'{tag}: powerd\'s settings hold the timers')
        code, out, calls = run(base, env)
        check(code == 0 and calls == [], f'{tag}: once: a second boot calls nothing')
        base, env = rig(CUSTOM_BATTERY.replace(' standby              1\n', '') + CUSTOM_AC)
        code, out, calls = run(base, env)
        check('-b standby 0' not in calls and 'left out (not offered here): standby' in out, f'{tag}: a key this model lacks is left out and said so')
        base, env = rig(CUSTOM_AC)
        code, out, calls = run(base, env)
        check(calls == ['-c sleep 0', '-c displaysleep 10'] and 'Battery Power: not offered by pmset yet' in out,
              f'{tag}: no battery yet: AC only, battery later')
        write(base / 'custom', CUSTOM_BATTERY + CUSTOM_AC)
        code, out, calls = run(base, env)
        check(calls[:1] == ['-b sleep 5'] and not [c for c in calls if c.startswith('-c')], f'{tag}: the battery once it appears, AC not again')
        base, env = rig(CUSTOM_BATTERY + CUSTOM_AC)
        subprocess.run([str(base / 'bin/pmset'), '-b', 'sleep', '15'], env=env, check=True)
        code, out, calls = run(base, env)
        check(not [c for c in calls if c.startswith('-b')] and calls == ['-c sleep 0', '-c displaysleep 10'] and
              prefs(base, 'Battery Power', 'System Sleep Timer') == '15', f'{tag}: the owner\'s battery timers first: kept, AC set')
        base, env = rig(CUSTOM_AC)
        run(base, env)
        subprocess.run([str(base / 'bin/pmset'), '-b', 'displaysleep', '7'], env=env, check=True)
        write(base / 'custom', CUSTOM_BATTERY + CUSTOM_AC)
        code, out, calls = run(base, env)
        check(calls == [] and 'the owner\'s timers are set' in out and prefs(base, 'Battery Power', 'Display Sleep Timer') == '7',
              f'{tag}: the owner\'s change after a partial run is kept')


# ---- 4-7: trust cache, cdhash, seed, live ------------------------------------------------------------------------------------

def trust_cache(hash_lists):
    """A trust cache bundle of version 2 modules, one per list of 20-byte hashes."""
    modules = [struct.pack('<I16sI', 2, bytes([i + 1]) * 16, len(hs)) + b''.join(h + bytes([2, 0, 0, 0]) for h in sorted(hs))
               for i, hs in enumerate(hash_lists)]
    offsets, cursor = [], 4 + 4 * len(modules)
    for m in modules:
        offsets.append(cursor)
        cursor += len(m)
    return struct.pack(f'<{len(modules) + 1}I', len(modules), *offsets) + b''.join(modules)


def hashes_of(data, tmp):
    p = write(tmp / 'tc.bin', data)
    import common
    return common.trust_hashes(p)


def module_count(data):
    return struct.unpack_from('<I', data)[0]


def trust_tests(tmp):
    print('4. trust cache')
    a, b, c = (bytes([x]) * 20 for x in (0xaa, 0xbb, 0xcc))
    base = trust_cache([[a]])
    out, added = stage.add_trust(base, [c.hex(), b.hex()])
    check(module_count(out) == 2 and added == [b.hex(), c.hex()] and hashes_of(out, tmp) == {a, b, c}, 'the missing CDHashes as one module, sorted')
    first = struct.unpack_from('<I', out, 4)[0]
    check(out[first + 24:first + 44] == b and out.endswith(base[8:]), 'the new module first, the input\'s modules kept as they were')
    again, added = stage.add_trust(out, [b.hex(), c.hex()])
    check(again == out and added == [], 'all there: the input byte for byte, nothing added')
    d = bytes([0xdd]) * 20
    some, added = stage.add_trust(out, [a.hex(), d.hex()])
    check(added == [d.hex()] and module_count(some) == 3, 'only what is missing is added')


def build_c(tmp, name, text):
    src = write(tmp / f'{name}.c', text)
    out = tmp / name
    subprocess.run(['xcrun', 'clang', '-arch', 'arm64', '-mmacosx-version-min=14.0', '-O2', str(src), '-o', str(out)], check=True)
    return out


def cdhash_tests(tmp):
    print('5. cdhash')
    exe = build_c(tmp, 'hello', '#include <stdio.h>\nint main(void) { puts("hello"); return 0; }\n')
    info = subprocess.run(['codesign', '-dvvv', str(exe)], capture_output=True, text=True).stderr
    want = re.search(r'^CDHash=([0-9a-f]+)', info, re.M).group(1)
    check(stage.soft_metal().cdhash(exe).hex() == want, f'cdhash = codesign\'s CDHash ({want[:16]}), linker-signed ad hoc')
    check(stage.soft_metal().cdhash(write(tmp / 'script.sh', '#!/bin/sh\n')) is None, 'a script has no CDHash')


@contextlib.contextmanager
def attached(raw, readonly=False):
    """RAW (a bare APFS container image) attached, its Data volume mounted; yields the mount point."""
    ssm = stage.soft_metal()
    args = ['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nomount', '-nobrowse', '-plist']
    entities = plistlib.loads(subprocess.run(args + (['-readonly'] if readonly else []) + [str(raw)], check=True,
                                             capture_output=True).stdout)['system-entities']
    whole = min((e['dev-entry'] for e in entities), key=lambda d: (len(d), d)).removeprefix('/dev/')
    mount = Path(tempfile.mkdtemp(prefix='mnt-', dir=raw.parent))
    try:
        volume = ssm.data_volume(whole)
        subprocess.run(['diskutil', 'mount', *(['readOnly'] if readonly else []), '-mountPoint', str(mount), volume], check=True,
                       stdout=subprocess.DEVNULL)
        try:
            yield mount
        finally:
            subprocess.run(['diskutil', 'unmount', str(mount)], check=True, stdout=subprocess.DEVNULL)
    finally:
        subprocess.run(['hdiutil', 'detach', '/dev/' + whole], check=True, stdout=subprocess.DEVNULL)


def unpack(seed, raw):
    logical, extents = stage.soft_metal().packed_ramdisk.regions(seed.read_bytes())
    with raw.open('wb') as h:
        h.truncate(logical)
        for offset, data in extents:
            h.seek(offset)
            h.write(data)
    return raw


BOOT_SCRIPT = 'sleep 0; sh /private/var/settime.sh; while :; do log show --last 25s 2>/dev/null; sleep 20; done'


def make_seed(tmp):
    """A 64 MiB APFS volume named Data (hdiutil), with a boot job like the seed's (settime.sh, then an endless loop) and a stale file
    in the staging directory, packed as DBRDISK1."""
    ssm = stage.soft_metal()
    tmp.mkdir(parents=True, exist_ok=True)
    subprocess.run(['hdiutil', 'create', '-size', '64m', '-layout', 'NONE', '-fs', 'APFS', '-volname', 'Data', str(tmp / 'data.dmg')],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['hdiutil', 'convert', str(tmp / 'data.dmg'), '-format', 'UDTO', '-o', str(tmp / 'data')], check=True,
                   stdout=subprocess.DEVNULL)
    raw = tmp / 'data.cdr'
    with attached(raw) as mnt:
        write(mnt / ssm.BOOT_JOB, plistlib.dumps({'Label': Path(ssm.BOOT_JOB).stem, 'ProgramArguments': ['/bin/sh', '-c', BOOT_SCRIPT],
                                                  'RunAtLoad': True}, fmt=plistlib.FMT_XML))
        write(mnt / 'private/var/settime.sh', 'exit 0\n')
        write(mnt / 'private/var/piano/userland/files/old/stale', 'from an earlier userland\n')
    seed = tmp / 'seed/scratch-data.img'
    seed.parent.mkdir()
    ssm.packed_ramdisk.pack(raw, seed)
    raw.unlink()
    (tmp / 'data.dmg').unlink()
    return seed


def test_manifest(tmp):
    tool = write(tmp / 'src/tool.c', '#include <stdio.h>\nint main(void) { puts("tool"); return 0; }\n')
    daemon = write(tmp / 'src/com.test.tool.plist', job_plist('com.test.tool', ['/usr/local/libexec/tool'], KeepAlive=True).decode())
    script = write(tmp / 'src/helper.sh', '#!/bin/sh\necho helper\n', 0o755)
    helper = write(tmp / 'src/com.test.helper.plist', job_plist('com.test.helper', ['/bin/sh', '/usr/local/libexec/helper']).decode())
    text = BASE + unit_toml('tool', [{'source': str(tool), 'build': 'cc', 'dest': '/usr/local/libexec/tool', 'mode': '0755'},
                                     {'source': str(daemon), 'dest': '/Library/LaunchDaemons/com.test.tool.plist', 'mode': '0644'}]) \
        + unit_toml('helper', [{'source': str(script), 'dest': '/usr/local/libexec/helper', 'mode': '0755'},
                               {'source': str(helper), 'dest': '/Library/LaunchDaemons/com.test.helper.plist', 'mode': '0644'}],
                    default_on=False) \
        + unit_toml('cli', [{'source': str(tool), 'build': 'cc', 'dest': '/usr/local/bin/tool2', 'mode': '0755'}])
    return write(tmp / 'userland.toml', text)


def stage_run(*args):
    r = subprocess.run([sys.executable, str(HERE / 'stage.py'), *map(str, args)], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def seed_tests(tmp):
    print('6. seed (hdiutil, a 64 MiB APFS image)')
    ssm = stage.soft_metal()
    seed = make_seed(tmp)
    manifest = test_manifest(tmp)
    trust = write(tmp / 'seed/kernel.macho.trustcache', trust_cache([[bytes([0x11]) * 20]]))
    seed_sha, trust_sha = sha(seed.read_bytes()), sha(trust.read_bytes())
    out = tmp / 'out/stage'
    code, text = stage_run('--manifest', manifest, '--seed', seed, '--trust', trust, '--out', out)
    check(code == 0, 'stage.py stages the test seed' + ('' if code == 0 else ':\n' + text[-2000:]))
    if code:
        return
    record = json.loads((out / 'userland.json').read_text())
    staged = {f['dest']: f for f in record['files']}
    check(set(staged) == {'/usr/local/libexec/tool', '/Library/LaunchDaemons/com.test.tool.plist', '/usr/local/libexec/helper',
                          '/Library/LaunchDaemons/com.test.helper.plist', '/usr/local/bin/tool2'}, 'the record lists the five files')
    cd = {staged['/usr/local/libexec/tool']['cdhash'], staged['/usr/local/bin/tool2']['cdhash']}
    check(None not in cd and len(cd) == 2 and staged['/usr/local/libexec/helper']['cdhash'] is None,
          'two Mach-Os with CDHashes (named as installed: they differ), the script none')
    check({h.hex() for h in hashes_of((out / 'kernel.macho.trustcache').read_bytes(), tmp)} == cd | {'11' * 20} and
          module_count((out / 'kernel.macho.trustcache').read_bytes()) == 2, 'the trust cache: the input module and one with both CDHashes')
    check(sha(seed.read_bytes()) == seed_sha and sha(trust.read_bytes()) == trust_sha, 'the input seed and trust cache are unchanged')
    with attached(unpack(out / 'scratch-data.img', tmp / 'check.raw'), readonly=True) as mnt:
        root = mnt / 'private/var/piano/userland'
        on_volume = {p.relative_to(root).as_posix(): p for p in root.rglob('*') if p.is_file()}
        manifest_tsv = (root / 'manifest.tsv').read_text()
        ok = all(sha(on_volume[f'files/{f["unit"]}/{Path(d).name}'].read_bytes()) == f['sha256'] for d, f in staged.items())
        check(ok, 'every staged file on the volume has the record\'s sha256')
        check(set(on_volume) == {'install.sh', 'manifest.tsv', 'SHA256SUMS', *[f'files/{f["unit"]}/{Path(d).name}' for d, f in staged.items()]},
              'the staging directory holds exactly the plan (the stale file removed)')
        check(oct(on_volume['install.sh'].stat().st_mode & 0o777) == '0o755' and
              oct(on_volume['files/tool/com.test.tool.plist'].stat().st_mode & 0o777) == '0o644', 'modes 755/644 on the volume')
        check(subprocess.run(['shasum', '-a', '256', '-c', '-s', 'SHA256SUMS'], cwd=root).returncode == 0, 'SHA256SUMS verifies on the volume')
        helper = plistlib.loads(on_volume['files/helper/com.test.helper.plist'].read_bytes())
        check(helper.get('Disabled') is True and '\nhelper\tdaemon\t0\t' in manifest_tsv, 'the off job: Disabled in its plist, on 0 in manifest.tsv')
        job = plistlib.loads((mnt / ssm.BOOT_JOB).read_bytes())['ProgramArguments'][2]
        check(job == BOOT_SCRIPT.replace('settime.sh;', 'settime.sh; sh /private/var/piano/userland/install.sh --boot;', 1),
              'the boot job: install.sh right after settime.sh, before the endless loop')
        with tempfile.TemporaryDirectory(dir=tmp) as e2e:
            e2e = Path(e2e)
            shutil.copytree(root, e2e / 'stage')
            rig = Rig(e2e, SHELLS[0])
            shutil.rmtree(e2e / 'stage')
            shutil.copytree(root, e2e / 'stage')
            code2, out2 = rig.run()
            check(code2 == 0 and rig.loaded() == {'system/com.test.tool'} and
                  sha(rig.root('/usr/local/libexec/tool').read_bytes()) == staged['/usr/local/libexec/tool']['sha256'],
                  'end to end: install.sh from the output seed installs it (the on job loaded, the off one not)')
    before = {n: sha((out / n).read_bytes()) for n in ('scratch-data.img', 'kernel.macho.trustcache', 'userland.json')}
    code, text = stage_run('--manifest', manifest, '--seed', seed, '--trust', trust, '--out', out)
    after = {n: sha((out / n).read_bytes()) for n in before}
    check(code == 0 and 'up to date' in text and before == after, 'the second run: up to date, the outputs byte for byte the same')
    code, text = stage_run('--manifest', manifest, '--seed', seed, '--trust', trust, '--out', out, '--force')
    with attached(unpack(out / 'scratch-data.img', tmp / 'check2.raw'), readonly=True) as mnt:
        job = plistlib.loads((mnt / ssm.BOOT_JOB).read_bytes())['ProgramArguments'][2]
        files = {p.relative_to(mnt).as_posix(): sha(p.read_bytes()) for p in (mnt / 'private/var/piano/userland').rglob('*') if p.is_file()}
    record2 = json.loads((out / 'userland.json').read_text())
    check(code == 0 and job.count('install.sh --boot') == 1 and record2['files'] == record['files'] and
          sha((out / 'kernel.macho.trustcache').read_bytes()) == before['kernel.macho.trustcache'],
          f'--force restages: the same files and trust cache, the command once (seed identity {record["outputs"]["seed"]["identity_sha256"][:8]}'
          f' -> {record2["outputs"]["seed"]["identity_sha256"][:8]}: a restage is a new seed)')
    check(len(files) == 8, 'the restaged directory: the same eight files')
    code, text = stage_run('--manifest', manifest, '--seed', seed, '--trust', trust, '--out', seed.parent)
    check(code != 0 and 'would overwrite the input' in text and sha(seed.read_bytes()) == seed_sha, '--out over the input seed is refused')
    code, text = stage_run('--manifest', manifest, '--seed', seed, '--trust', trust, '--out', tmp / 'out/stage-off', '--on', 'helper',
                           '--off', 'tool')
    rec = json.loads((tmp / 'out/stage-off/userland.json').read_text()) if code == 0 else {'files': []}
    onoff = {f['unit']: f['on'] for f in rec['files']}
    check(code == 0 and onoff.get('helper') is True and onoff.get('tool') is False, '--on/--off override the profile for one build')


def live_tests(tmp):
    print('7. live update bundle')
    manifest = test_manifest(tmp / 'l')
    installed = write(tmp / 'l/installed.trustcache', trust_cache([[bytes([0x22]) * 20]]))
    code, text = stage_run('--manifest', manifest, '--live', tmp / 'l/live', '--installed-trust', installed)
    check(code == 0, 'stage.py --live' + ('' if code == 0 else ':\n' + text[-1500:]))
    if code:
        return
    report = json.loads((tmp / 'l/live/live.json').read_text())
    check(sorted(report['untrusted']) == ['/usr/local/bin/tool2', '/usr/local/libexec/tool'] and 'NOT in the installed trust cache' in text,
          'the two new Mach-Os are not in the installed trust cache: said so')
    tc = (tmp / 'l/live/kernel.macho.trustcache').read_bytes()
    check(module_count(tc) == 2 and len(hashes_of(tc, tmp)) == 3, 'the ESP trust cache: the installed one plus a module of the new CDHashes')
    first = (tmp / 'l/live/userland-update.tar').read_bytes()
    stage_run('--manifest', manifest, '--live', tmp / 'l/live', '--installed-trust', installed)
    check((tmp / 'l/live/userland-update.tar').read_bytes() == first, 'the archive is the same byte for byte on a second run')
    with tarfile.open(tmp / 'l/live/userland-update.tar') as tar:
        members = tar.getmembers()
        check(all(m.uid == 0 and m.gid == 0 and m.mtime == 0 for m in members), 'archive entries: owner 0:0, time 0')
        tar.extractall(tmp / 'l/x', filter='tar')
    check(subprocess.run(['shasum', '-a', '256', '-c', '-s', 'SHA256SUMS'], cwd=tmp / 'l/x/userland').returncode == 0,
          'the extracted archive verifies against its SHA256SUMS')
    apply = (tmp / 'l/live/apply-update.sh').read_text()
    check(sha(first) in apply and 'install.sh --live' in apply and 'nohup' in apply, 'apply-update.sh names the archive and runs install.sh --live detached')
    check(all(subprocess.run([s, '-n', str(tmp / 'l/live/apply-update.sh')]).returncode == 0 for s in SHELLS), 'apply-update.sh parses (sh -n)')
    trusted = write(tmp / 'l/all.trustcache', tc)
    code, text = stage_run('--manifest', manifest, '--live', tmp / 'l/live2', '--installed-trust', trusted)
    check(code == 0 and json.loads((tmp / 'l/live2/live.json').read_text())['untrusted'] == [] and
          not (tmp / 'l/live2/kernel.macho.trustcache').exists(), 'every CDHash trusted: live only, no ESP trust cache')
    print('   (configs/userland.toml: every real unit; piano-rotated\'s build runs PianoAuto\'s host tests)')
    code, text = stage_run('--live', tmp / 'real/live', '--installed-trust', installed)
    check(code == 0, 'configs/userland.toml --live: every real unit builds and checks' + ('' if code == 0 else ':\n' + text[-1500:]))
    if code == 0:
        real = json.loads((tmp / 'real/live/live.json').read_text())
        machos = {f['dest']: f['cdhash'] for f in real['files'] if f['cdhash']}
        check(set(machos) == {'/usr/local/libexec/pianousb-shell', '/usr/local/libexec/piano-rotated', '/usr/local/bin/piano-torch',
                              '/usr/local/libexec/piano-wifi-join'},
              'four Mach-Os with CDHashes: ' + ', '.join(f'{Path(d).name} {h[:12]}' for d, h in sorted(machos.items())))
        archs = {d: subprocess.run(['lipo', '-archs', str(tmp / 'real/live/staging/files' / f['unit'] / Path(d).name)],
                                   capture_output=True, text=True).stdout.strip() for d in machos for f in real['files'] if f['dest'] == d}
        check(set(archs.values()) == {'arm64'}, 'each one thin arm64')
        code, text = stage_run('--live', tmp / 'real/live', '--installed-trust', installed)
        again = json.loads((tmp / 'real/live/live.json').read_text())
        check(code == 0 and again['archive_sha256'] == real['archive_sha256'], 'the real bundle is reproducible (the build stamps hold)')
        rig = Rig(tmp / 'real/rig', SHELLS[0])
        shutil.rmtree(tmp / 'real/rig/stage')
        shutil.copytree(tmp / 'real/live/staging', tmp / 'real/rig/stage')
        code, out = rig.run()
        want = {'system/com.unixnu.pianousb-shell', 'system/com.unixnu.piano-policy-agent', 'system/com.unixnu.piano-pmset-defaults',
                'system/com.unixnu.piano-still-wallpaper', 'system/com.unixnu.piano-wifi-join',
                f'gui/{rig.uid}/com.unixnu.piano-rotated', f'gui/{rig.uid}/com.unixnu.piano-login-front'}
        check(code == 0 and rig.loaded() == want and rig.root('/usr/local/bin/piano-torch').is_file() and
              rig.root('/usr/local/share/piano/wallpaper/still.png').read_bytes()[:8] == b'\x89PNG\r\n\x1a\n',
              f'the real staging installed on a scratch root ({sorted(rig.loaded() ^ want) or "as wanted"}): five daemons and the two agents loaded, piano-torch in /usr/local/bin, '
              'the still picture in /usr/local/share/piano/wallpaper')
        code, out = rig.run('--check')
        check(code == 0, 'and --check passes on it')
        # The owner's Data: the base's Metal plugin in place; the real layer with --gpu-bundle over it, then the empty layer
        # (configs/userland-none.toml) takes it all out again (docs/persistent-boot.md section 11.6, the layer's rollback).
        b = tmp / 'real/T.bundle'
        write(b / 'Contents/Info.plist', plistlib.dumps({'CFBundleExecutable': 'T'}))
        write(b / 'Contents/MacOS/T', b'#!/bin/sh\necho the layer\'s plugin\n', 0o755)   # no Mach-O: no CDHash to compute
        code, text = stage_run('--live', tmp / 'real/live-gpu', '--installed-trust', installed, '--gpu-bundle', b)
        rig = Rig(tmp / 'real/rig2', SHELLS[0])
        base = {'/Library/GPUBundles/T.bundle/Contents/Info.plist': b'<plist>the base\'s</plist>',
                '/Library/GPUBundles/T.bundle/Contents/MacOS/T': b'the base\'s plugin'}
        for dest, data in base.items():
            write(rig.root(dest), data, 0o755)
        shutil.rmtree(tmp / 'real/rig2/stage')
        shutil.copytree(tmp / 'real/live-gpu/staging', tmp / 'real/rig2/stage')
        code1, _ = rig.run('--live')
        installed_files = [f['dest'] for f in json.loads((tmp / 'real/live-gpu/live.json').read_text())['files']]
        code2, text2 = stage_run('--manifest', D / 'configs/userland-none.toml', '--live', tmp / 'real/none', '--installed-trust', installed)
        shutil.rmtree(tmp / 'real/rig2/stage')
        shutil.copytree(tmp / 'real/none/staging', tmp / 'real/rig2/stage')
        code3, out = rig.run('--live')
        left = [d for d in installed_files if d not in base and rig.root(d).exists()]
        check(code1 == 0 and code2 == 0 and code3 == 0 and not left and rig.loaded() == set() and
              all(rig.root(d).read_bytes() == data for d, data in base.items()),
              'the empty layer (configs/userland-none.toml) after the real one: every unit out, every job booted out, the base\'s '
              'plugin back byte for byte' + (f' (left: {left})' if left else ''))


def main():
    quick = '--quick' in sys.argv
    with tempfile.TemporaryDirectory(prefix='userland-test-') as tmp:
        tmp = Path(tmp)
        manifest_tests(tmp / 'm1')
        installer_tests(tmp / 'i')
        pmset_tests(tmp / 'p')
        if quick:
            print('(--quick: parts 4-7 skipped)')
        elif k27_tree(required=False) is None:
            print('SKIP 4-7: PIANO_K27_TREE is not set (stage-soft-metal.py reads packed_ramdisk from the k27 tree)')
        else:
            trust_tests(tmp / 't')
            cdhash_tests(tmp / 'c')
            seed_tests(tmp / 's')
            live_tests(tmp / 'v')
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
