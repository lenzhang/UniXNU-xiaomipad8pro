#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
"""The userland of native macOS on piano, staged in one reproducible step (docs/userland.md): configs/userland.toml's files built
(xcrun clang, arm64/arm64e, ad hoc signed by the linker) or copied, put into a copy of the Data seed under the manifest's staging
directory with the on-tablet installer (tooling/userland/install.sh), the seed's boot job told to run it, the Mach-Os' CDHashes added
to the trust cache, and a record of every file's sha256.

    stage.py [--seed out/turnip-a830/stage/scratch-data.img] [--trust out/turnip-a830/stage/kernel.macho.trustcache]
             [--out out/userland/stage] [--manifest configs/userland.toml] [--on UNIT]... [--off UNIT]... [--force]
    stage.py --live out/userland/live [--installed-trust FILE]     the same files as a live update's bundle, no seed (docs/userland.md 7)
    stage.py --plan                                                the manifest, checked, as a table; nothing built
    --gpu-bundle BUNDLE (with any mode, repeatable)                a Metal plugin bundle as one more unit, gpu-<name>: every file of
                                                                   BUNDLE at /Library/GPUBundles/<name>.bundle/..., Mach-Os trusted

The system layer (docs/data-store.md section 9): the files that change are installed on the running system, never staged into the
seed of an ESP that keeps the owner's Data (that seed is the store's base, tooling/persist/base-seed.py). `stage.py --live DIR
--gpu-bundle out/turnip-a830/NativeAdrenoMetal.bundle --installed-trust <the base's trust cache>` is that layer: the userland and the
plugin, and the trust cache an ESP takes with --layer DIR. install.sh keeps a copy of every file it replaces that it did not put
there itself (the base's plugin) and puts it back when the unit goes, so taking the layer out restores the base's files.

Outputs (seed mode, nothing outside --out is written; the input seed and trust cache are only read):
  OUT/scratch-data.img          the input seed plus STAGING (default /private/var/piano/userland: install.sh, manifest.tsv,
                                SHA256SUMS, files/<unit>/<name>) and the boot job's extra command `sh STAGING/install.sh --boot`
  OUT/kernel.macho.trustcache   the input trust cache with one more module first (stage-soft-metal.py's trust_bundle) holding the
                                CDHashes it lacks; none added when it has them all
  OUT/userland.json             the record: inputs, every file (unit, dest, owner, mode, sha256, CDHash, on), outputs, identities
  OUT/stamp                     what the outputs were made from

Idempotent: the build of a file is skipped when its stamp (its sources' bytes, the command, the toolchain) is unchanged, and the
staging when its stamp (the input seed's and trust cache's sha256, every staged file's sha256 and mode, the boot command, FORMAT) is:
then the outputs are left byte for byte as they are and the seed keeps its identity. A restage starts from the input seed again
(never from an earlier output): files of the staging directory that the manifest no longer has are removed, the boot command is
added once, CDHashes the input already has are not added again.

The seed mechanics are tooling/k27/stage-soft-metal.py's (imported, not changed: its sha256 is in build-native-image.sh's stamp):
the packed DBRDISK1 disk unpacked to a sparse raw file in OUT, attached and its APFS volume mounted in one hdiutil step (no
diskutil: this Mac's diskutil mount hangs some days; no sudo: ownership is ignored, the copies are stored as uid/gid 99 with their
modes), the files written, the disk detached and packed again. Every external command runs under a timeout (sh). The owners and
modes of configs/userland.toml are applied on the tablet by install.sh, as root,
from the boot job (launchd refuses a job file that is not root's, and this Mac cannot make one on the seed's volume).

Every run that changes the staging makes a new seed identity (APFS writes times and transaction ids): docs/data-store.md applies.
An ESP with this seed meets the installed store as another seed's (the store guard keeps it, Data runs volatile) unless the store is
flattened (then the seed is not read at all and none of these files reach Data: the live update path, docs/userland.md section 7).
"""
import argparse
import contextlib
import glob
import hashlib
import importlib.util
import io
import json
import os
import plistlib
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
from pathlib import Path

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(D / 'tooling/persist'))
import common  # noqa: E402  (tooling/persist/common.py: seed identity, trust cache hashes, the installed seed)

FORMAT = 1   # the staged layout's version: part of the stamp; raise it when the same inputs are to be staged differently
INSTALLER = D / 'tooling/userland/install.sh'
DIRS = {'/Library/LaunchDaemons': 'daemon', '/Library/LaunchAgents': 'agent', '/usr/local/bin': 'file', '/usr/local/libexec': 'file',
        '/usr/local/share/piano/wallpaper': 'file'}   # the last: the still wallpaper's data files (unit piano-still-wallpaper)
# A Metal plugin bundle's files (IOGPU looks for <MetalPluginName>.bundle in /Library/GPUBundles first; docs/data-store.md section 5):
# Contents/Info.plist, Contents/MacOS/<file>, Contents/Resources/<file>.
GPU_BUNDLES = '/Library/GPUBundles'
GPU_BUNDLE_FILE = re.compile(r'/Library/GPUBundles/[A-Za-z0-9_-]+\.bundle/Contents/(?:Info\.plist|(?:MacOS|Resources)/[A-Za-z0-9_+-][A-Za-z0-9._+-]*)')
SYSTEM_PROGRAMS = ('/bin/', '/usr/bin/', '/sbin/', '/usr/sbin/', '/usr/libexec/')
WORD = re.compile(r'[A-Za-z0-9._+@/-]+')
MODE = re.compile(r'0[0-7]{3}')
OWNER = re.compile(r'[a-z_][a-z0-9_]*:[a-z_][a-z0-9_]*')
UNIT = re.compile(r'[a-z0-9][a-z0-9-]*')
_SSM = None


def soft_metal():
    """tooling/k27/stage-soft-metal.py as a module: cdhash, trust_bundle, data_volume, run, BOOT_JOB, SETTIME, packed_ramdisk (from
    the k27 tree: PIANO_K27_TREE)."""
    global _SSM
    if _SSM is None:
        sys.path.insert(0, str(D / 'tooling/k27'))
        spec = importlib.util.spec_from_file_location('stage_soft_metal', D / 'tooling/k27/stage-soft-metal.py')
        _SSM = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_SSM)
    return _SSM


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rel(path):
    """PATH relative to the repository when it is inside it (records and messages carry no home directories)."""
    path = Path(path).resolve()
    return path.relative_to(D).as_posix() if path.is_relative_to(D) else str(path)


def sh(command, *, timeout=600, **kw):
    """subprocess.run for every external command this tool starts, always bounded: one that stops answering (this Mac's disk tools
    have hung on some days) fails the run with a clear message instead of hanging it forever. TIMEOUT is per command."""
    try:
        return subprocess.run(command, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        raise SystemExit(f'{command[0]}: no answer within {timeout} s ({shlex.join(command)}); giving up on it') from None


# ---- the manifest ------------------------------------------------------------------------------------------------------------

def load_manifest(path, on=(), off=()):
    """configs/userland.toml as a checked plan: {'staging', 'units': [{name, about, on, planned, files: [...]}]}. SystemExit with
    every problem found."""
    try:
        doc = tomllib.loads(Path(path).read_text())
    except (OSError, tomllib.TOMLDecodeError) as e:
        raise SystemExit(f'{path}: {e}')
    problems = []
    if doc.get('format') != 1:
        problems.append(f'format {doc.get("format")!r}: this tool reads format 1')
    staging = doc.get('staging', '')
    if not (staging.startswith('/private/var/') and WORD.fullmatch(staging) and '..' not in staging.split('/')):
        problems.append(f'staging {staging!r}: an absolute path under /private/var/ (Data), plain characters')
    units, names, dests = [], set(), {}
    for u in doc.get('unit', []):
        name = u.get('name', '')
        where = f'unit {name or "?"}'
        if not UNIT.fullmatch(name) or name in names:
            problems.append(f'{where}: the name must be lower case letters, digits and dashes, and unique')
        names.add(name)
        unknown = set(u) - {'name', 'about', 'default_on', 'status', 'file'}
        if unknown:
            problems.append(f'{where}: unknown keys {sorted(unknown)}')
        if u.get('status', 'ready') not in ('ready', 'planned'):
            problems.append(f'{where}: status {u.get("status")!r} (ready or planned)')
        if not isinstance(u.get('default_on'), bool):
            problems.append(f'{where}: default_on must be true or false')
        unit = {'name': name, 'about': u.get('about', ''), 'on': bool(u.get('default_on')), 'default_on': bool(u.get('default_on')),
                'planned': u.get('status') == 'planned', 'files': []}
        for f in u.get('file', []):
            unit['files'].append(check_file(f, unit, problems))
        if not unit['files']:
            problems.append(f'{where}: no files')
        jobs = [f for f in unit['files'] if f['kind'] != 'file']
        if len(jobs) > 1:
            problems.append(f'{where}: {len(jobs)} launchd jobs; one unit has at most one')
        if not jobs and not unit['default_on']:
            problems.append(f'{where}: default_on false needs a launchd job to turn off (a command-line tool is always installed)')
        for f in unit['files']:
            if f['dest'] in dests:
                problems.append(f'{where}: {f["dest"]} is also unit {dests[f["dest"]]}\'s')
            dests[f['dest']] = name
        units.append(unit)
    for name, value in [(n, True) for n in on] + [(n, False) for n in off]:
        match = [u for u in units if u['name'] == name]
        if not match:
            problems.append(f'--{"on" if value else "off"} {name}: no such unit')
        elif not [f for f in match[0]['files'] if f['kind'] != 'file'] and not value:
            problems.append(f'--off {name}: a command-line tool has no job to turn off')
        else:
            match[0]['on'] = value
    if problems:
        raise SystemExit(f'{path}:\n  ' + '\n  '.join(problems))
    return {'path': Path(path), 'staging': staging, 'units': units}


def dest_kind(dest):
    """What a destination is: daemon, agent or file (directly in one of DIRS, or a Metal plugin bundle's file); None when refused."""
    parent, _, base = dest.rpartition('/')
    if not WORD.fullmatch(dest) or not base or base.startswith('.') or '..' in dest.split('/'):
        return None
    return DIRS.get(parent) or ('file' if GPU_BUNDLE_FILE.fullmatch(dest) else None)


def check_file(f, unit, problems):
    where = f'unit {unit["name"]}, {f.get("dest", "?")}'
    unknown = set(f) - {'source', 'sources', 'build', 'arch', 'min_os', 'cflags', 'frameworks', 'command', 'output', 'inputs',
                        'dest', 'owner', 'mode', 'trust', 'staged'}
    if unknown:
        problems.append(f'{where}: unknown keys {sorted(unknown)}')
    dest = f.get('dest', '')
    base = dest.rpartition('/')[2]
    kind = dest_kind(dest)
    if not kind:
        problems.append(f'{where}: dest must be a file directly in {", ".join(DIRS)}, or a Metal plugin bundle\'s '
                        f'({GPU_BUNDLES}/<name>.bundle/Contents/Info.plist, MacOS/<file>, Resources/<file>)')
        kind = 'file'
    owner, mode = f.get('owner', 'root:wheel'), f.get('mode', '')
    if not OWNER.fullmatch(owner):
        problems.append(f'{where}: owner {owner!r} (user:group)')
    if not MODE.fullmatch(mode) or int(mode, 8) & 0o022:
        problems.append(f'{where}: mode {mode!r}: four octal digits, not writable by group or others')
    if kind != 'file':
        if not base.endswith('.plist') or mode != '0644' or owner != 'root:wheel':
            problems.append(f'{where}: a launchd job is <Label>.plist, root:wheel 0644 (launchd refuses any other)')
    build = f.get('build', 'copy')
    sources = [f['source']] if 'source' in f else list(f.get('sources', []))
    if build in ('copy', 'cc'):
        if not sources or (build == 'copy' and len(sources) != 1):
            problems.append(f'{where}: build {build} needs source' + ('' if build == 'copy' else ' or sources'))
        if not unit['planned']:
            for s in sources:
                if not (D / s).is_file():
                    problems.append(f'{where}: no source {s}')
        if build == 'cc' and f.get('arch', 'arm64') not in ('arm64', 'arm64e'):
            problems.append(f'{where}: arch {f.get("arch")!r} (arm64 or arm64e)')
    elif build == 'script':
        if not f.get('command') or not isinstance(f.get('command'), list) or not f.get('output') or not f.get('inputs'):
            problems.append(f'{where}: build script needs command (a list), output and inputs')
    else:
        problems.append(f'{where}: build {build!r} (copy, cc or script)')
    return {'unit': unit['name'], 'dest': dest, 'name': base, 'kind': kind, 'owner': owner, 'mode': mode, 'build': build,
            'staged': f.get('staged'),
            'sources': sources, 'arch': f.get('arch', 'arm64'), 'min_os': f.get('min_os', '14.0'), 'cflags': list(f.get('cflags', [])),
            'frameworks': list(f.get('frameworks', [])), 'command': list(f.get('command', [])), 'output': f.get('output'),
            'inputs': list(f.get('inputs', [])), 'trust': f.get('trust', True)}


def add_gpu_bundles(plan, bundles):
    """--gpu-bundle: each Metal plugin bundle as one more unit of PLAN, gpu-<its name in lower case>, without a job (always installed):
    every file of the bundle at /Library/GPUBundles/<name>.bundle/<its path>, root:wheel, 0755 for a Mach-O or an executable, 0644
    otherwise, its Mach-Os' CDHashes into the trust cache. Staged as files/<unit>/<its path in the bundle>."""
    problems = []
    dests = {f['dest']: u['name'] for u in plan['units'] for f in u['files']}
    for bundle in bundles:
        bundle = Path(bundle).resolve()
        if not re.fullmatch(r'[A-Za-z0-9_-]+\.bundle', bundle.name) or not (bundle / 'Contents/Info.plist').is_file():
            raise SystemExit(f'--gpu-bundle {bundle}: not a plugin bundle (<name>.bundle with Contents/Info.plist)')
        name = 'gpu-' + bundle.stem.lower().replace('_', '-')
        if name in {u['name'] for u in plan['units']}:
            raise SystemExit(f'--gpu-bundle {bundle}: unit {name} twice')
        unit = {'name': name, 'about': f'the Metal plugin {bundle.name} in {GPU_BUNDLES} (the system layer, docs/data-store.md section 9)',
                'on': True, 'default_on': True, 'planned': False, 'files': []}
        for path in sorted(bundle.rglob('*')):
            if path.is_symlink() or (not path.is_file() and not path.is_dir()):
                raise SystemExit(f'--gpu-bundle {bundle}: {path.relative_to(bundle)} is neither a file nor a directory')
            if not path.is_file():
                continue
            inside = path.relative_to(bundle).as_posix()
            data = path.read_bytes()
            executable = data[:4] == b'\xcf\xfa\xed\xfe' or os.access(path, os.X_OK)
            f = check_file({'source': str(path), 'build': 'copy', 'dest': f'{GPU_BUNDLES}/{bundle.name}/{inside}',
                            'mode': '0755' if executable else '0644', 'staged': f'files/{name}/{inside}'}, unit, problems)
            if f['dest'] in dests:
                problems.append(f'{f["dest"]}: also unit {dests[f["dest"]]}\'s')
            dests[f['dest']] = name
            unit['files'].append(f)
        if not unit['files']:
            problems.append(f'--gpu-bundle {bundle}: no files')
        plan['units'].append(unit)
    if problems:
        raise SystemExit('--gpu-bundle:\n  ' + '\n  '.join(problems))
    return plan


def check_jobs(plan, built):
    """Each launchd job of the plan against the plan: its Label is its file name, its program is installed by the manifest (or a
    system path), the paths under /usr/local it names are the manifest's, Disabled is not set by hand."""
    problems = []
    dests = {f['dest'] for u in plan['units'] if not u['planned'] for f in u['files']}
    for f, data in built:
        if f['kind'] == 'file':
            continue
        try:
            job = plistlib.loads(data)
        except Exception as e:  # noqa: BLE001 (plistlib raises several types)
            problems.append(f'{f["dest"]}: not a property list ({e})')
            continue
        label = job.get('Label')
        if label != f['name'][:-len('.plist')]:
            problems.append(f'{f["dest"]}: Label {label!r} is not the file name')
        argv = job.get('ProgramArguments') or ([job['Program']] if job.get('Program') else [])
        if not argv:
            problems.append(f'{f["dest"]}: no Program or ProgramArguments')
        elif argv[0] not in dests and not argv[0].startswith(SYSTEM_PROGRAMS):
            problems.append(f'{f["dest"]}: runs {argv[0]}, neither installed by the manifest nor a system program')
        for word in argv:
            if isinstance(word, str) and word.startswith('/usr/local/') and word not in dests:
                problems.append(f'{f["dest"]}: names {word}, which the manifest does not install')
        if 'Disabled' in job:
            problems.append(f'{f["dest"]}: has Disabled; on/off is default_on in the manifest (stage.py writes Disabled for off)')
        f['label'] = label or '-'
    if problems:
        raise SystemExit('launchd jobs:\n  ' + '\n  '.join(problems))


# ---- building ----------------------------------------------------------------------------------------------------------------

_TOOLCHAIN = None


def toolchain():
    """What a build depends on beyond its sources: clang, ld and the SDK, as their versions say."""
    global _TOOLCHAIN
    if _TOOLCHAIN is None:
        parts = []
        for command in (['xcrun', 'clang', '--version'], ['xcrun', '--show-sdk-path'], ['xcrun', '--show-sdk-version'],
                        ['xcrun', 'ld', '-v']):
            r = sh(command, timeout=60, capture_output=True, text=True)
            parts.append((r.stdout + r.stderr).strip())
        _TOOLCHAIN = '\n'.join(parts)
    return _TOOLCHAIN


def build_file(f, build_dir):
    """The bytes FILE installs: its source (copy) or its build output, rebuilt only when its stamp changed. Returns a Path."""
    if f['build'] == 'copy':
        return D / f['sources'][0]
    out = build_dir / f['unit'] / f['name']
    out.parent.mkdir(parents=True, exist_ok=True)
    stamp_file = out.with_name(out.name + '.stamp')
    if f['build'] == 'cc':
        command = ['xcrun', 'clang', '-arch', f['arch'], f'-mmacosx-version-min={f["min_os"]}', '-O2', '-Wall', '-Wextra', '-Werror',
                   *f['cflags'], *f['sources'], *[x for fw in f['frameworks'] for x in ('-framework', fw)], '-o', str(out)]
        inputs = [D / s for s in f['sources']]
    else:
        command = f['command']
        inputs = sorted({Path(p) for pattern in f['inputs'] for p in glob.glob(str(D / pattern)) if Path(p).is_file()})
        if not inputs:
            raise SystemExit(f'{f["dest"]}: no file matches its inputs {f["inputs"]}')
    h = hashlib.sha256(json.dumps([command if f['build'] == 'script' else command[:-1], toolchain()]).encode())
    for path in inputs:
        h.update(rel(path).encode() + b'\0' + path.read_bytes())
    stamp = h.hexdigest()
    try:
        old = json.loads(stamp_file.read_text())
    except (OSError, ValueError):
        old = {}
    if old.get('stamp') == stamp and out.is_file() and sha(out.read_bytes()) == old.get('sha256'):
        return out
    log = out.with_name(out.name + '.log')
    with log.open('w') as handle:
        result = sh(command, timeout=600, cwd=D, stdout=handle, stderr=subprocess.STDOUT)
    if result.returncode:
        tail = log.read_text().strip().splitlines()[-15:]
        raise SystemExit(f'{f["dest"]}: the build failed ({shlex.join(command)}); {rel(log)}:\n  ' + '\n  '.join(tail))
    if f['build'] == 'script':
        produced = D / f['output']
        if not produced.is_file():
            raise SystemExit(f'{f["dest"]}: {shlex.join(command)} made no {f["output"]}')
        shutil.copyfile(produced, out)
        os.chmod(out, 0o755)
    stamp_file.write_text(json.dumps({'stamp': stamp, 'sha256': sha(out.read_bytes())}) + '\n')
    return out


def build_all(plan, build_dir):
    """Every file of the units that are not planned: [(file, bytes)], each Mach-O's CDHash in file['cdhash'] (hex) when trusted."""
    built = []
    for unit in plan['units']:
        if unit['planned']:
            continue
        for f in unit['files']:
            path = build_file(f, build_dir)
            f['on'] = unit['on']
            f['path'] = path
            data = path.read_bytes()
            f['cdhash'] = None
            if data[:4] in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
                if data[:4] != b'\xcf\xfa\xed\xfe':
                    raise SystemExit(f'{f["dest"]}: a fat Mach-O; build one architecture (the trust cache takes a thin file\'s CDHash)')
                digest = soft_metal().cdhash(path)
                if f['trust']:
                    f['cdhash'] = digest.hex()
            elif f['build'] in ('cc', 'script') and f['trust']:
                raise SystemExit(f'{f["dest"]}: built, but not a Mach-O ({rel(path)})')
            built.append((f, data))
    check_jobs(plan, built)
    out = []
    for f, data in built:
        if f['kind'] != 'file' and not f['on']:
            job = plistlib.loads(data)
            job['Disabled'] = True   # off in this profile: launchd loads it only once the owner says launchctl enable
            data = plistlib.dumps(job, fmt=plistlib.FMT_XML)
        f['sha256'] = sha(data)
        out.append((f, data))
    return out


# ---- the staging tree --------------------------------------------------------------------------------------------------------

def staging_tree(plan, built):
    """{path relative to the staging directory: (bytes, mode)}: install.sh, manifest.tsv, SHA256SUMS, files/<unit>/<name>."""
    tree = {'install.sh': (INSTALLER.read_bytes(), 0o755)}
    lines = ['# unit\tkind\ton\tmode\towner\tgroup\tsha256\tstaged\tdest\tlabel (configs/userland.toml, tooling/userland/stage.py)']
    for f, data in built:
        staged = f.get('staged') or f'files/{f["unit"]}/{f["name"]}'
        tree[staged] = (data, 0o755 if int(f['mode'], 8) & 0o111 else 0o644)
        user, group = f['owner'].split(':')
        fields = [f['unit'], f['kind'], '1' if f['on'] else '0', f['mode'], user, group, f['sha256'], staged, f['dest'],
                  f.get('label', '-')]
        assert all(WORD.fullmatch(x) for x in fields), fields
        lines.append('\t'.join(fields))
    tree['manifest.tsv'] = (('\n'.join(lines) + '\n').encode(), 0o644)
    sums = ''.join(f'{sha(data)}  {name}\n' for name, (data, _) in sorted(tree.items()))
    tree['SHA256SUMS'] = (sums.encode(), 0o644)
    return tree


def sync_tree(root, tree):
    """Make ROOT hold exactly TREE: files written only when their bytes differ, modes set, anything else under ROOT removed.
    Returns (written, removed) relative paths."""
    root.mkdir(parents=True, exist_ok=True)
    written, removed = [], []
    for path in sorted(root.rglob('*'), key=lambda p: len(p.parts), reverse=True):
        name = path.relative_to(root).as_posix()
        if path.is_symlink() or path.is_file():
            if name not in tree:
                path.unlink()
                removed.append(name)
        elif path.is_dir() and not any(k.startswith(name + '/') for k in tree):
            shutil.rmtree(path)
            removed.append(name + '/')
    for name, (data, mode) in sorted(tree.items()):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not (path.is_file() and path.read_bytes() == data):
            path.write_bytes(data)
            written.append(name)
        os.chmod(path, mode)
    for path in [root, *[p for p in root.rglob('*') if p.is_dir()]]:
        os.chmod(path, 0o755)
    # the build Mac's com.apple.provenance tag (its sandboxed tools write it) is not carried into the tablet's volume
    sh(['xattr', '-cr', str(root)], timeout=60, check=False)
    return written, removed


def boot_command(plan):
    return f'sh {plan["staging"]}/install.sh --boot'


def add_boot_command(job, command, settime):
    """The seed's boot job, rewritten in place (same file, so the same owner): COMMAND right after settime.sh, once; the commands
    already there (stage-soft-metal.py --boot-run, --log-errors loops, which never end) stay, after it. Returns (script, changed)."""
    if not job.is_file():
        raise SystemExit(f'{job}: the seed has no boot job; a new plist would not be root\'s and launchd would refuse it')
    record = plistlib.loads(job.read_bytes())
    argv = record.get('ProgramArguments', [])
    if len(argv) != 3 or argv[:2] != ['/bin/sh', '-c'] or settime not in argv[2]:
        raise SystemExit(f'{job}: not the expected boot job ({argv}); check it by hand')
    if command in argv[2]:
        return argv[2], False
    script = argv[2].replace(settime, f'{settime}; {command}', 1)
    record['ProgramArguments'] = ['/bin/sh', '-c', script]
    job.write_bytes(plistlib.dumps(record, fmt=plistlib.FMT_XML))
    sh(['xattr', '-c', str(job)], timeout=60, check=False)
    return script, True


@contextlib.contextmanager
def mounted_seed(seed, out, result):
    """SEED's Data volume, mounted read-write from a sparse raw copy under OUT; packed again to OUT/scratch-data.img afterwards
    (RESULT gets packed_ramdisk.pack's metadata). stage-soft-metal.py's unpack/pack steps, in its order; the attach and mount are
    hdiutil's in one step (no diskutil: this Mac's diskutil mount hangs some days, hdiutil does not). Non-sudo like the old
    `diskutil mount`, so the volume ignores ownership the same way: the copies are stored as uid/gid 99 with their modes (inferred
    from the mount running as this uid either way; the staging's checks below would catch a mount of the wrong volume: the seed's
    boot job would be missing)."""
    ssm = soft_metal()
    logical, extents = ssm.packed_ramdisk.regions(seed.read_bytes())
    with tempfile.TemporaryDirectory(prefix='seed-', dir=str(out)) as scratch:
        raw = Path(scratch) / 'seed.raw'
        with raw.open('wb') as handle:   # sparse: only the extents are written
            handle.truncate(logical)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
        mount = Path(scratch) / 'mnt'
        mount.mkdir()
        entities = sh(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-mountpoint', str(mount),
                       '-plist', str(raw)], timeout=300, check=True, capture_output=True).stdout
        attached = plistlib.loads(entities)['system-entities']
        disks = [e['dev-entry'] for e in attached]
        whole = min(disks, key=lambda d: (len(d), d)).removeprefix('/dev/')   # the image's disk, before its synthesized container
        detached = False
        # hdiutil reports the mount point through its realpath (/private/... for a /tmp/... request; confirmed on the host
        # 2026-11-14), so compare the resolved form of MOUNT:
        mount_real = str(mount.resolve())
        try:
            volume = next((e['dev-entry'].removeprefix('/dev/') for e in attached if e.get('mount-point') == mount_real), None)
            if volume is None:   # attached but nothing mounted here: refuse before any write
                raise SystemExit(f'{raw.name}: hdiutil attached the seed but mounted no volume at {mount}; refusing to stage')
            try:
                yield mount, volume
            finally:
                sh(['hdiutil', 'detach', '/dev/' + whole], timeout=120, check=True, stdout=subprocess.DEVNULL)
                detached = True
        finally:
            if not detached:   # the refusal above, or a detach that failed: force the image off so nothing is left attached
                sh(['hdiutil', 'detach', '-force', '/dev/' + whole], timeout=120, check=False, stdout=subprocess.DEVNULL)
        packed = out / 'scratch-data.img.new'
        packed.unlink(missing_ok=True)
        result.update(ssm.packed_ramdisk.pack(raw, packed))
        os.replace(packed, out / 'scratch-data.img')


def add_trust(trust_bytes, cdhashes):
    """The trust cache with the CDHashes it lacks as one more module first (stage-soft-metal.py's trust_bundle); unchanged when it
    has them all. Returns (bytes, added hex list)."""
    ssm = soft_metal()
    with tempfile.NamedTemporaryFile() as handle:
        handle.write(trust_bytes)
        handle.flush()
        have = common.trust_hashes(handle.name)
    need = sorted({bytes.fromhex(h) for h in cdhashes} - have)
    if not need:
        return trust_bytes, []
    return ssm.trust_bundle(need, trust_bytes), [h.hex() for h in need]


def files_record(built):
    return [{'unit': f['unit'], 'dest': f['dest'], 'kind': f['kind'], 'label': f.get('label', '-'), 'on': f['on'],
             'owner': f['owner'], 'mode': f['mode'], 'build': f['build'], 'from': rel(f['path']), 'sha256': f['sha256'],
             'cdhash': f['cdhash']} for f, _ in built]


def store_notes(identity, input_identity, seed):
    """What this seed means for the tablet's Data store (docs/data-store.md), as lines."""
    installed, source = common.installed_seed()
    lines = [f'seed identity {identity[:16]} (input {input_identity[:16]}): a new seed, as every restage makes.']
    if installed and common.same_seed(identity, installed):
        lines.append(f'it is the installed seed ({source}).')
    else:
        lines += [f'the installed seed is {(installed or "unknown")[:16]} ({source}). An ESP or RAM image with this seed:',
                  '  - store of the installed seed, not flattened: kept untouched, Data VOLATILE (these files installed every boot, '
                  'gone at the next); build-esp.py --replace-data would start the store over from this seed (the owner\'s Data '
                  'changes dropped)',
                  '  - flattened (self-contained) store: resumed, the seed NOT read: none of these files reach Data; use the live '
                  'update (docs/userland.md section 7)',
                  '  - blank store, data-store-replace, sheng-data-reset: a new store from this seed: everything installed at its '
                  'first boot and kept']
    if seed['pages'] != common.INSTALLED_STORE_PAGES:
        lines.append(f'note: {seed["pages"]} pages, not the installed store\'s {common.INSTALLED_STORE_PAGES}: build-esp.py and '
                     'build-ram-images.py refuse data-persist=2 for it (docs/data-store.md section 8)')
    return lines


# ---- the modes ---------------------------------------------------------------------------------------------------------------

def stage_seed(a, plan):
    seed_in, trust_in, out = a.seed.resolve(), a.trust.resolve(), a.out.resolve()
    if (out / 'scratch-data.img') in (seed_in,) or (out / 'kernel.macho.trustcache') in (trust_in,):
        raise SystemExit(f'--out {rel(out)} would overwrite the input seed or trust cache: stage into another directory')
    for path in (seed_in, trust_in):
        if not path.is_file():
            raise SystemExit(f'{rel(path)}: no such file')
    out.mkdir(parents=True, exist_ok=True)
    built = build_all(plan, out.parent / 'build')
    tree = staging_tree(plan, built)
    command = boot_command(plan)
    seed_info_in = common.seed_info(seed_in)
    trust_bytes = trust_in.read_bytes()
    stamp = sha(json.dumps({'format': FORMAT, 'seed': seed_info_in['file_sha256'], 'trust': sha(trust_bytes), 'boot': command,
                            'staging': plan['staging'], 'tree': {k: [sha(v), m] for k, (v, m) in sorted(tree.items())}},
                           sort_keys=True).encode())
    record_path, seed_out, trust_out = out / 'userland.json', out / 'scratch-data.img', out / 'kernel.macho.trustcache'
    try:
        record = json.loads(record_path.read_text())
    except (OSError, ValueError):
        record = {}
    if not a.force and (out / 'stamp').is_file() and (out / 'stamp').read_text().strip() == stamp and seed_out.is_file() and \
            trust_out.is_file() and common.sha(seed_out) == record.get('outputs', {}).get('seed', {}).get('file_sha256') and \
            common.sha(trust_out) == record.get('outputs', {}).get('trust', {}).get('sha256'):
        identity = record['outputs']['seed']['identity_sha256']
        print(f'{rel(seed_out)}: up to date (stamp {stamp[:16]}): seed identity {identity[:16]} unchanged; '
              f'{rel(trust_out)} unchanged')
        return record

    trust_new, added = add_trust(trust_bytes, [f['cdhash'] for f, _ in built if f['cdhash']])
    tmp = trust_out.with_name(trust_out.name + '.new')
    tmp.write_bytes(trust_new)
    os.replace(tmp, trust_out)
    print(f'{rel(trust_out)}: ' + (f'+ {len(added)} CDHash(es) {" ".join(added)}' if added else 'the input\'s CDHashes already '
                                     'cover every Mach-O: unchanged'))

    ssm = soft_metal()
    packed, staging = {}, plan['staging'].lstrip('/')
    (out / 'stamp').unlink(missing_ok=True)   # a run cut short below leaves no stamp: the next one restages
    with mounted_seed(seed_in, out, packed) as (mount, volume):
        written, removed = sync_tree(mount / staging, tree)
        script, changed = add_boot_command(mount / ssm.BOOT_JOB, command, ssm.SETTIME)
        listing = sh(['ls', '-lnR', str(mount / staging)], timeout=60, capture_output=True, text=True).stdout
        listing = listing.replace(str(mount), '')
        print(f'{volume}: /{staging}: {len(written)} written, {len(removed)} removed{": " + " ".join(removed) if removed else ""}\n'
              f'{listing.strip()}')
        print(f'the boot job {"now runs" if changed else "already ran"}: {script}')
    seed_info_out = common.seed_info(seed_out)
    record = {
        'format': FORMAT, 'stamp': stamp, 'manifest': rel(plan['path']), 'staging': plan['staging'], 'boot_command': command,
        'inputs': {'seed': {'path': rel(seed_in), **{k: seed_info_in[k] for k in ('file_sha256', 'identity_sha256', 'logical_bytes')}},
                   'trust': {'path': rel(trust_in), 'sha256': sha(trust_bytes)}},
        'files': files_record(built),
        'planned': [u['name'] for u in plan['units'] if u['planned']],
        'outputs': {'seed': {'path': rel(seed_out), **{k: seed_info_out[k] for k in ('file_sha256', 'identity_sha256',
                                                                                     'logical_bytes', 'bytes')}},
                    'trust': {'path': rel(trust_out), 'sha256': common.sha(trust_out), 'added_cdhashes': added}},
    }
    record_path.write_text(json.dumps(record, indent=1) + '\n')
    (out / 'stamp').write_text(stamp + '\n')
    print(f'{rel(seed_out)}: {seed_info_out["bytes"]} bytes packed, logical {seed_info_out["logical_bytes"]}, identity '
          f'{seed_info_out["identity_sha256"][:16]} (input {seed_info_in["identity_sha256"][:16]})')
    print('\n'.join(store_notes(seed_info_out['identity_sha256'], seed_info_in['identity_sha256'], seed_info_out)))
    print(f'{rel(record_path)}: the record; then e.g. PIANO_SEED={rel(seed_out)} PIANO_TRUST={rel(trust_out)} '
          'tooling/k27/build-native-image.sh, or build-esp.py --seed/--trust with them')
    return record


def deterministic_tar(tree, prefix):
    """TREE as a tar archive: sorted, owner root:wheel, time 0, the modes of TREE, directories 0755."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        dirs = sorted({'/'.join(k.split('/')[:i]) for k in tree for i in range(1, k.count('/') + 1)})
        for name in ['', *dirs]:
            info = tarfile.TarInfo(f'{prefix}/{name}'.rstrip('/'))
            info.type, info.mode, info.mtime, info.uname, info.gname = tarfile.DIRTYPE, 0o755, 0, 'root', 'wheel'
            tar.addfile(info)
        for name, (data, mode) in sorted(tree.items()):
            info = tarfile.TarInfo(f'{prefix}/{name}')
            info.size, info.mode, info.mtime, info.uname, info.gname = len(data), mode, 0, 'root', 'wheel'
            tar.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


APPLY = '''#!/bin/sh
# piano-userland apply-update.sh (written by tooling/userland/stage.py --live; docs/userland.md section 7): on the tablet, as root,
# with userland-update.tar next to it. Checks the archive, swaps it in as {staging} (the one before stays as .prev)
# and runs its install.sh --live detached, so a restart of the shell it came through does not stop it.
#   sh apply-update.sh [ARCHIVE]        then: cat /private/var/db/piano-userland/live.log
set -eu
A=${{1:-$(dirname "$0")/userland-update.tar}}
[ "$(id -u)" = 0 ] || {{ echo "apply-update.sh: run as root" >&2; exit 1; }}
[ "$(shasum -a 256 "$A" | cut -d' ' -f1)" = {tar_sha} ] || {{ echo "apply-update.sh: $A is not the archive this script was made for" >&2; exit 1; }}
S={staging}
rm -rf "$S.new" && mkdir -p "$S.new"
tar -xpf "$A" -C "$S.new" --strip-components 1
(cd "$S.new" && shasum -a 256 -c -s SHA256SUMS)
rm -rf "$S.prev"
[ ! -d "$S" ] || mv "$S" "$S.prev"
mv "$S.new" "$S"
mkdir -p /private/var/db/piano-userland
nohup sh {staging}/install.sh --live > /private/var/db/piano-userland/live.log 2>&1 < /dev/null &
echo "apply-update.sh: {staging} replaced; install.sh --live runs (pid $!): /private/var/db/piano-userland/live.log"
'''


def stage_live(a, plan):
    """The live update's bundle (docs/userland.md section 7): the same staging tree, as a deterministic tar and its apply script,
    and the trust check against the trust cache the running kernel has (the installed ESP's)."""
    out = a.live.resolve()
    out.mkdir(parents=True, exist_ok=True)
    built = build_all(plan, out.parent / 'build')
    tree = staging_tree(plan, built)
    sync_tree(out / 'staging', tree)
    archive = deterministic_tar(tree, 'userland')
    (out / 'userland-update.tar').write_bytes(archive)
    apply = APPLY.format(staging=plan['staging'], tar_sha=sha(archive))
    (out / 'apply-update.sh').write_text(apply)
    os.chmod(out / 'apply-update.sh', 0o755)
    installed = a.installed_trust or common.installed_trust()
    report = {'format': FORMAT, 'manifest': rel(plan['path']), 'staging': plan['staging'], 'archive_sha256': sha(archive),
              'files': files_record(built), 'installed_trust': rel(installed) if installed else None,
              'installed_trust_sha256': common.sha(installed) if installed else None}
    print(f'{rel(out / "userland-update.tar")}: {len(archive)} bytes, sha256 {sha(archive)[:16]}; {rel(out / "apply-update.sh")}')
    machos = [f for f, _ in built if f['cdhash']]
    if not installed:
        print('the installed trust cache is unknown (--installed-trust, or PIANO_INSTALLED_SEED=<the ESP record>.json): which '
              'Mach-Os the running kernel trusts cannot be told; ' + ', '.join(f['dest'] for f in machos))
    else:
        have = common.trust_hashes(installed)
        new = [f for f in machos if bytes.fromhex(f['cdhash']) not in have]
        for f in machos:
            print(f'  {f["dest"]}: CDHash {f["cdhash"][:16]} ' + ('in the installed trust cache: live' if f not in new else
                                                                 'NOT in the installed trust cache'))
        report['untrusted'] = [f['dest'] for f in new]
        if new:
            trust_new, added = add_trust(installed.read_bytes(), [f['cdhash'] for f in new])
            (out / 'kernel.macho.trustcache').write_bytes(trust_new)
            report['trust'] = {'path': rel(out / 'kernel.macho.trustcache'), 'sha256': sha(trust_new), 'added_cdhashes': added}
            print(f'{rel(out / "kernel.macho.trustcache")}: the installed trust cache + {len(added)} CDHash(es). An ESP with it '
                  'and the INSTALLED seed (the store\'s base: the same identity, the store resumes) makes them trusted from its first '
                  'boot: tooling/persist/build-esp.py --profile daily --layer ' + rel(out) +
                  ' (the daily profile takes the pinned base; docs/data-store.md section 9, docs/userland.md section 7)')
    (out / 'live.json').write_text(json.dumps(report, indent=1) + '\n')
    return report


def show_plan(plan):
    print(f'{rel(plan["path"])}: staging {plan["staging"]}')
    for u in plan['units']:
        state = 'planned' if u['planned'] else ('on' if u['on'] else 'off')
        print(f'{u["name"]} [{state}]: {u["about"]}')
        for f in u['files']:
            how = {'copy': f'copy of {f["sources"][0] if f["sources"] else "?"}',
                   'cc': f'cc {f["arch"]} {" ".join(f["sources"])}' + (f' -framework {" ".join(f["frameworks"])}' if f['frameworks']
                                                                       else ''),
                   'script': f'{" ".join(f["command"])} -> {f["output"]}'}[f['build']]
            print(f'  {f["dest"]}  {f["owner"]} {f["mode"]}  {f["kind"]}  {how}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--manifest', type=Path, default=D / 'configs/userland.toml')
    p.add_argument('--seed', type=Path, default=D / 'out/turnip-a830/stage/scratch-data.img')
    p.add_argument('--trust', type=Path, default=D / 'out/turnip-a830/stage/kernel.macho.trustcache')
    p.add_argument('--out', type=Path, default=D / 'out/userland/stage')
    p.add_argument('--on', action='append', default=[], metavar='UNIT', help='this unit\'s job on in this build')
    p.add_argument('--off', action='append', default=[], metavar='UNIT', help='this unit\'s job off (Disabled) in this build')
    p.add_argument('--force', action='store_true', help='restage even when the stamp says the outputs are up to date')
    p.add_argument('--live', type=Path, metavar='DIR', help='write a live update\'s bundle to DIR instead of a seed')
    p.add_argument('--installed-trust', type=Path, help='the trust cache of the installed ESP (default: from its record)')
    p.add_argument('--plan', action='store_true', help='check the manifest and print it; build nothing')
    p.add_argument('--gpu-bundle', action='append', default=[], type=Path, metavar='BUNDLE',
                   help='a Metal plugin bundle as one more unit (/Library/GPUBundles/<name>.bundle), its Mach-Os trusted')
    a = p.parse_args()
    plan = add_gpu_bundles(load_manifest(a.manifest, a.on, a.off), a.gpu_bundle)
    if a.plan:
        show_plan(plan)
    elif a.live:
        stage_live(a, plan)
    else:
        stage_seed(a, plan)


if __name__ == '__main__':
    main()
