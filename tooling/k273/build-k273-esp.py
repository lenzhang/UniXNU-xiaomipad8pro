#!/usr/bin/env python3
"""The installed ESP with macOS 27.3 only (docs/persistent-boot.md section 13, docs/k273-line.md section 7).

  build-k273-esp.py [--base ESP.json] [--entry macos-27.3] [--name esp-k273-only] [--out out/persistent]
                    [--expect-seed HEX] [--expect-store-lba N] [--expect-store-part NAME] [--diag-reset-after SECS]

The base is an ESP build record (default: the installed one, OUT/installed.json's image; normally esp-dual33d-k273d.json of
tooling/k273/build-dual-esp.py) and its image beside it. Its 27.3 entry (--entry) is carried over byte for byte: the entry's four
payload files (its path, path.adt, path.trustcache and its initrd, the 27.3 collection, ADT, trust cache and store seed), the entry's
table (id, title, type, path, initrd, args: the line) and the firmware files its collection pins. Nothing of the 27.0.1 line: no
other payload file, no other OS entry. The new menu:
  macos-27.3        the base's entry, the default (the base's timeout)
  macos-27.3-diag   only when the line + piano-reset-after/-to stays under DarwinBoot's 1000 bytes (the 989-byte line does not)
  fastboot          the same payload, reset into ABL fastboot after 1 s: no UFS root, no Data store (data-persist=0), no piano-* driver
                    and no pstore console (the last run's log stays)
  reset, shutdown, tools
The base files are read twice, by the independent FAT reader (common.fat32_read) and by mtools, and must agree with each other and
with the base's record. The Data store facts are read from the inputs, never fixed here: the partition and the window's first LBA
from the line (native-ufs-root-part, data-store-lba), its size from the seed; common.check_store_size checks the window is the one
NativeUfs puts there for that seed. --expect-* only assert them. The image is read back by the FAT reader and by mtools; every 27.3
payload file and the line must be the base's, byte for byte, and no entry may use linux_rootfs (an entry that mounts a UFS root or
opens a store without native-ufs-root-part names linux_rootfs by default). The record (OUT/NAME.json) is a build-esp.py-style
record whose seed and store are the 27.3 store's: maint-session.py checks the store on that partition at that LBA (bound to it,
of that seed) before it writes the ESP, and rootfs-clear accepts only an ESP of such a record on the tablet.
"""
import argparse
import hashlib
import importlib.util
import json
import shutil
import sys
import tempfile
import tomllib
from pathlib import Path

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(D / 'tooling/persist'))
import common  # noqa: E402
import firmware  # noqa: E402

spec = importlib.util.spec_from_file_location('boot_args', D / 'tooling/adt/boot_args.py')
boot_args = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_args)
spec = importlib.util.spec_from_file_location('build_esp', D / 'tooling/persist/build-esp.py')
build_esp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_esp)

ROOT = 'EFI/UniXNU/'
TOML = ROOT + 'unixnu.toml'
PAYLOADS = ROOT + 'Payloads/'
LINE_MAX = 1000                      # DarwinBoot passes at most 1023 bytes; the tools keep every line under 1000
# The fastboot entry: the line without the root, the store, the console mirror and every piano-* driver, then a reset after 1 s.
FASTBOOT_DROP = ('native-ufs-root', 'native-ufs-root-part', 'native-ufs-diagnostics', 'pstore-console', 'pstore-console-size',
                 'data-persist', 'data-store-lba', 'data-store-replace', 'data-store-flatten', 'sheng-smp')
FASTBOOT_TAIL = ['data-persist=0', 'piano-reset-after=1', 'piano-reset-to=bootloader', 'sheng-smp=1']
ACTIONS = ('[[entry]]\nid = "reset"\ntitle = "Restart"\ntype = "action"\naction = "reset"\n'
           '[[entry]]\nid = "shutdown"\ntitle = "Power off"\ntype = "action"\naction = "shutdown"\n'
           '[[entry]]\nid = "tools"\ntitle = "Tools"\ntype = "action"\naction = "tools"\n')


def h(data):
    return hashlib.sha256(data).hexdigest()


def name_of(arg):
    return arg.split('=', 1)[0]


def entry_text(entry):
    return '[[entry]]\n' + ''.join(f'{k} = "{v}"\n' for k, v in entry.items())


def fastboot_line(line):
    args = [x for x in line.split() if name_of(x) not in FASTBOOT_DROP and not x.startswith('piano-')]
    return ' '.join(args + FASTBOOT_TAIL)


def diag_line(line, secs):
    args = [x for x in line.split() if name_of(x) not in ('piano-reset-after', 'piano-reset-to', 'sheng-smp')]
    return ' '.join(args + [f'piano-reset-after={secs}', 'piano-reset-to=bootloader', 'sheng-smp=1'])


def config_text(base_name, timeout, entries, store, seed):
    text = ('# piano persistent boot (docs/persistent-boot.md): the ESP on UFS LUN 0, partition 37. boot_a holds only BootShim + FD; a\n'
            '# "fastboot boot" image with its own payload still wins (firmware/patches/unixnu/0001).\n'
            f'# macOS 27.3 only (tooling/k273/build-k273-esp.py): the {entries[0]["id"]} entry of {base_name} byte for byte; its Data store\n'
            f'# on {store["partition"]} at LBA {store["first_lba"]} (bound), seed {seed["identity_sha256"][:16]}\n'
            'version = 1\n[boot]\ntimeout = ' + str(timeout) + '\ndefault = "' + entries[0]['id'] + '"\n[kits]\nautoload = false\n')
    return text + ''.join(entry_text(e) for e in entries) + ACTIONS


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--base', type=Path, help='the base ESP\'s build record, its image beside it (default: OUT/installed.json\'s)')
    p.add_argument('--entry', default='macos-27.3', help='the base entry carried over (the 27.3 line)')
    p.add_argument('--name', default='esp-k273-only')
    p.add_argument('--out', type=Path, default=common.DEVICE / 'out/persistent')
    p.add_argument('--expect-seed', metavar='HEX', help='refuse unless the entry\'s seed identity starts with HEX (8+ hex digits)')
    p.add_argument('--expect-store-lba', type=int, metavar='N', help='refuse unless the line binds the store to LBA N')
    p.add_argument('--expect-store-part', metavar='NAME', help='refuse unless the line puts root and store on partition NAME')
    p.add_argument('--kc', type=Path, help='a new kernel collection for the entry (a 27.3 build: tooling/k273/build-k273-image.sh)')
    p.add_argument('--adt', type=Path, help='its ADT (the same build)')
    p.add_argument('--trust', type=Path, help='a new trust cache (it must cover the seed\'s and the system\'s code)')
    p.add_argument('--seed', type=Path, help='a new Data seed: a new store (its window from the seed\'s size, data-store-lba set)')
    p.add_argument('--arg', action='append', default=[], metavar='NAME=VALUE', help='one more argument for the entry (replaces NAME)')
    p.add_argument('--drop-arg', action='append', default=[], metavar='NAME', help='an argument of the base line left out')
    p.add_argument('--split', action='store_true', help='move the piano-* arguments into the ADT (k27 0066) even when the line fits')
    p.add_argument('--diag-reset-after', type=int, default=240, metavar='SECS',
                   help='the -diag entry\'s reset (only built when its line stays under 1000 bytes)')
    a = p.parse_args()

    out = a.out
    if a.base:
        base_json = a.base
    else:
        installed = json.loads((out / 'installed.json').read_text())
        base_json = out / (Path(installed['image']).stem + '.json')
    base = json.loads(base_json.read_text())
    base_img = base_json.with_name(base['image'])
    if common.sha(base_img) != base['sha256']:
        raise SystemExit(f'{base_img}: not the image its record {base_json.name} names')

    # -- the base, read by the independent FAT reader and by mtools ----------------------------------------------------------
    fat = common.fat32_read(base_img)
    if fat['problems']:
        raise SystemExit(f'{base_img}: the FAT reader finds {fat["problems"]}')
    files = fat['files']
    if set(files) != set(base['files']) or any(h(files[n]) != v for n, v in base['files'].items() if v != 'private'):
        raise SystemExit(f'{base_img}: its files differ from its record {base_json.name}')
    text = files[TOML].decode()
    if text != base['unixnu_toml']:
        raise SystemExit(f'{base_img}: unixnu.toml differs from its record')
    config = tomllib.loads(text)
    entry = next((e for e in config['entry'] if e['id'] == a.entry), None)
    if not entry or entry.get('type') != 'xnu':
        raise SystemExit(f'{base_img}: no xnu entry {a.entry} (entries: {[e["id"] for e in config["entry"]]})')
    line = entry['args']
    for where, value in (('cmdlines', (base.get('cmdlines') or {}).get(a.entry)),
                         ('k273.cmdline', (base.get('k273') or {}).get('cmdline') if (base.get('k273') or {}).get('id') == a.entry else None)):
        if value is not None and value != line:
            raise SystemExit(f'{base_json.name}: its {where} is not the line of {a.entry} in the image')
    # DarwinBoot loads PATH, PATH.adt and PATH.trustcache, and the initrd (the seed), all below EFI/UniXNU
    payload = {'kc': ROOT + entry['path'], 'adt': ROOT + entry['path'] + '.adt', 'trust': ROOT + entry['path'] + '.trustcache',
               'seed': ROOT + entry['initrd']}
    for role, path in payload.items():
        if not path.startswith(PAYLOADS) or path not in files:
            raise SystemExit(f'{base_img}: {a.entry}\'s {role} {path} is not on the ESP')
    payload_dir = str(Path(payload['kc']).parent) + '/'

    with tempfile.TemporaryDirectory(prefix='piano-k273-esp-') as scratch:
        scratch = Path(scratch)
        mt = scratch / 'base-mtools'
        mt.mkdir()
        build_esp.mtools('mcopy', '-i', str(base_img), '-s', '-n', '::/EFI', str(mt))
        mfiles = {str(f.relative_to(mt)): h(f.read_bytes()) for f in sorted(mt.rglob('*')) if f.is_file()}
        if mfiles != {n: h(b) for n, b in files.items()}:
            raise SystemExit(f'{base_img}: mtools and the FAT reader read different files')

        # -- the 27.3 payload and its store, from the inputs ---------------------------------------------------------------
        stage = scratch / 'stage'
        for path in payload.values():
            (stage / path).parent.mkdir(parents=True, exist_ok=True)
            (stage / path).write_bytes(files[path])
        kc, adt, seed_file = stage / payload['kc'], stage / payload['adt'], stage / payload['seed']
        overrides = {}
        for role in ('kc', 'adt', 'trust', 'seed'):
            given = getattr(a, role)
            if given:
                (stage / payload[role]).write_bytes(given.read_bytes())
                overrides[role] = {'from': str(given.name), 'sha256': h((stage / payload[role]).read_bytes())}
        if bool(a.kc) != bool(a.adt):
            raise SystemExit('--kc and --adt come together (one build)')
        natives, vm_era, kc_rtc, kc_guard = build_esp.kc_bundles(kc)
        store_guard = common.kc_has_store_guard(kc)
        if vm_era:
            raise SystemExit(f'{a.entry}\'s collection links {vm_era}: the native kext rule allows only this repository\'s kexts')
        window = common.adt_ram_window(adt)
        recorded_window = (base.get('k273') or {}).get('ram_window') or base.get('ram_window')
        if recorded_window and window != recorded_window:
            raise SystemExit(f'{a.entry}\'s ADT asks for {window} bytes of RAM window, its record {recorded_window}')
        if common.adt_addon_slot(adt):
            raise SystemExit(f'{a.entry}\'s ADT has the GpuAddon slot of a RAM test image')
        seed = common.seed_info(seed_file)
        edits = list(a.arg) + ([f'data-store-lba={common.k273_store_first(seed)}'] if a.seed else [])
        if edits or a.drop_arg:
            names = {name_of(x) for x in edits} | set(a.drop_arg)
            line = ' '.join([x for x in line.split() if name_of(x) not in names] + edits)
        args = line.split()
        named = dict(x.split('=', 1) for x in args if '=' in x)
        part, lba = common.store_args_of(args)
        if named.get('native-ufs-root', '0') in ('0', '') or part == 'linux_rootfs':
            raise SystemExit(f'{a.entry}: its line does not mount a root of its own (native-ufs-root-part={part}): not the 27.3 line')
        if lba is None or named.get('data-persist') != '2':
            raise SystemExit(f'{a.entry}: its line has no bound read-write store (data-store-lba, data-persist=2)')
        common.check_store_size(seed, args)
        if a.expect_seed and not seed['identity_sha256'].startswith(a.expect_seed.lower()):
            raise SystemExit(f'{a.entry}\'s seed is {seed["identity_sha256"][:16]}, not {a.expect_seed}')
        if a.expect_store_lba is not None and lba != a.expect_store_lba:
            raise SystemExit(f'{a.entry} binds its store to LBA {lba}, not {a.expect_store_lba}')
        if a.expect_store_part and part != a.expect_store_part:
            raise SystemExit(f'{a.entry} puts its root and store on {part}, not {a.expect_store_part}')
        store = {'partition': part, 'first_lba': lba, 'blocks': seed['store_blocks'], 'last_lba': lba + seed['store_blocks'] - 1,
                 'bound': True, 'reserve_blocks': common.RESERVE_BLOCKS,
                 'from': f'the {a.entry} line (native-ufs-root-part, data-store-lba) and its seed (store_blocks)'}
        old_k = (base.get('k273') or {}).get('store') or {}
        if old_k and (old_k.get('first_lba'), old_k.get('partition')) != (lba, part):
            raise SystemExit(f'{base_json.name}: its k273 store ({old_k.get("partition")} LBA {old_k.get("first_lba")}) is not the line\'s')

        # -- the arguments past DarwinBoot's line: the ADT's /chosen/piano-boot-args-macos (k27 0066) -----------------------------
        split = None
        if len(line) >= LINE_MAX or a.split:
            if b'piano-boot-args: /chosen/' not in kc.read_bytes():
                raise SystemExit(f'{a.entry}: a {len(line)}-byte line needs the ADT tail, and this collection\'s kernel lacks k27 0066')
            kept, tail = boot_args.fit(line.split(), 'macos', force=True)
            adt.write_bytes(boot_args.with_set(adt.read_bytes(), 'macos', tail))
            split = {'set': 'macos', 'property': boot_args.prop_name('macos'), 'tail': ' '.join(tail), 'tail_bytes': len(' '.join(tail)),
                     'line_bytes': len(' '.join(kept)), 'full_bytes': len(line)}
            line = ' '.join(kept)

        # -- firmware the 27.3 collection pins; other non-payload files stay; the 27.0.1 payload goes --------------------------
        carried, dropped, fw = dict(payload.items()), [], []
        pins = firmware.kc_pins(kc)
        pinned = {f'{firmware.ESP_DIR}/{pin["path"]}': pin for pin in pins}
        for path, pin in pinned.items():
            if path not in files:
                raise SystemExit(f'{a.entry}\'s collection pins {path}, which {base_img.name} lacks')
            if not pin['private'] and (len(files[path]) != pin['size'] or h(files[path]) != pin['sha256']):
                raise SystemExit(f'{path}: the base\'s file is not the one {a.entry}\'s collection pins')
            fw.append(pin)
        for path in sorted(files):
            if path in payload.values() or path == TOML:
                continue
            if path in pinned:
                carried[path] = path
            elif path.startswith(PAYLOADS) or path.startswith(firmware.ESP_DIR + '/'):
                dropped.append(path)
            else:
                carried[path] = path          # e.g. the sensor pack or the amplifier preset: unit data the 27.3 kexts read
        for path in carried.values():
            (stage / path).parent.mkdir(parents=True, exist_ok=True)
            (stage / path).write_bytes(files[path])

        # -- the menu ------------------------------------------------------------------------------------------------------
        k_entry = {k: entry[k] for k in ('id', 'title', 'type', 'path', 'initrd', 'args')}
        k_entry['args'] = line
        entries = [k_entry]
        diag = diag_line(line, a.diag_reset_after)
        diag_info = {'bytes': len(diag), 'included': len(diag) < LINE_MAX}
        if diag_info['included']:
            entries.append({**k_entry, 'id': f'{a.entry}-diag', 'title': f'macOS 27.3, reset to fastboot after {a.diag_reset_after} s',
                            'args': diag})
        else:
            diag_info['why'] = f'{len(diag)} bytes with piano-reset-after/-to: DarwinBoot\'s lines stay under {LINE_MAX}'
        fb = fastboot_line(line)
        entries.append({**k_entry, 'id': 'fastboot', 'title': 'Reset into fastboot (ABL)', 'args': fb})
        for e in entries:
            if len(e['args']) >= LINE_MAX or '"' in e['args'] or '\\' in e['args']:
                raise SystemExit(f'{e["id"]}: kernel command line of {len(e["args"])} bytes, or a quote or backslash in it')
        new_text = config_text(base['image'], config['boot']['timeout'], entries, store, seed)
        parsed = tomllib.loads(new_text)
        if parsed['boot']['default'] != a.entry or parsed['entry'][0] != (k_entry if (overrides or edits or a.drop_arg or split) else entry):
            raise SystemExit('the 27.3 entry changed in the new unixnu.toml: refused')
        rootfs = {e['id']: why for e in parsed['entry'] if (why := common.entry_uses_rootfs(e))}
        if rootfs:
            raise SystemExit(f'entries that would use linux_rootfs: {rootfs}')
        (stage / TOML).write_text(new_text)
        for path in [stage, *stage.rglob('*')]:
            build_esp.os.utime(path, (build_esp.EPOCH, build_esp.EPOCH))
        want = {str(f.relative_to(stage)): h(f.read_bytes()) for f in sorted(stage.rglob('*')) if f.is_file()}
        staged = {role: (stage / path).read_bytes() for role, path in payload.items()}
        image = out / f'{a.name}.img'
        out.mkdir(parents=True, exist_ok=True)
        image.unlink(missing_ok=True)
        build_esp.mtools('mformat', '-C', '-F', '-S', '5', '-M', '4096', '-T', str(common.ESP_BLOCKS), '-h', '1', '-s', '1', '-c', '1',
                         '-H', str(common.ESP_FIRST), '-v', build_esp.LABEL, '-N', build_esp.SERIAL, '-i', str(image), '::')
        build_esp.mtools('mcopy', '-i', str(image), '-s', '-Q', '-m', str(stage / 'EFI'), '::/')
        minfo = build_esp.mtools('minfo', '-i', str(image), '::').stdout
        back = scratch / 'back'
        back.mkdir()
        build_esp.mtools('mcopy', '-i', str(image), '-s', '-n', '::/EFI', str(back))
        mback = {str(f.relative_to(back)): h(f.read_bytes()) for f in sorted(back.rglob('*')) if f.is_file()}

    # -- the image read back ----------------------------------------------------------------------------------------------
    new = common.fat32_read(image)
    got = {n: h(b) for n, b in new['files'].items()}
    problems = list(new['problems'])
    if image.stat().st_size != common.ESP_BLOCKS * common.LBA:
        problems.append('image size')
    if new['sector_bytes'] != 4096 or new['sectors_per_cluster'] != 1 or new['total_sectors'] != common.ESP_BLOCKS:
        problems.append('geometry')
    if new['hidden_sectors'] != common.ESP_FIRST or new['label'] != build_esp.LABEL or new['serial'] != build_esp.SERIAL:
        problems.append('hidden sectors, label or serial')
    if got != want or mback != want:
        problems.append('read back differs from the stage')
    if 'sector size: 4096 bytes' not in minfo:
        problems.append('minfo sector size')
    for role, path in payload.items():
        if new['files'].get(path) != staged[role]:
            problems.append(f'{path} is not the staged {role} byte for byte')
    for path in carried.values():
        if new['files'].get(path) != files[path]:
            problems.append(f'{path} changed')
    back_config = tomllib.loads(new['files'].get(TOML, b'').decode())
    back_entry = next((e for e in back_config.get('entry', []) if e['id'] == a.entry), {})
    if back_entry.get('args') != line or back_entry != k_entry or back_config['boot']['default'] != a.entry:
        problems.append(f'the {a.entry} entry read back is not the base\'s (line {len(back_entry.get("args", ""))} bytes)')
    if any(p.startswith(PAYLOADS) and not p.startswith(payload_dir) for p in got):
        problems.append('a payload file outside the 27.3 entry\'s directory')
    if problems:
        raise SystemExit(f'{image}: ' + '; '.join(problems))

    record = {
        'status': 'OFFLINE_BUILD_NOT_DEVICE_TESTED', 'kind': 'k273-only', 'image': image.name, 'bytes': image.stat().st_size,
        'sha256': common.sha(image),
        'partition': {'lun': 0, 'index': common.ESP_INDEX + 1, 'first_lba': common.ESP_FIRST, 'blocks': common.ESP_BLOCKS,
                      'dd': f'bs=1M seek={common.ESP_FIRST * common.LBA >> 20} count={common.ESP_BLOCKS * common.LBA >> 20}'},
        'about': f'macOS 27.3 only: the {a.entry} entry of {base["image"]} (payload and line byte for byte), fastboot, the actions '
                 '(tooling/k273/build-k273-esp.py)',
        'base_esp': {'image': base['image'], 'sha256': base['sha256'], 'record': base_json.name, 'entry': a.entry},
        'profile': base.get('profile'), 'default': a.entry, 'timeout': config['boot']['timeout'],
        'boot_guard': base.get('boot_guard'), 'charging': base.get('charging'), 'fast_boot': base.get('fast_boot'),
        'kc_has_boot_guard': kc_guard, 'kc_has_rtc_calendar': kc_rtc, 'kc_natives': natives,
        'cmdlines': {e['id']: e['args'] for e in entries},
        'line_bytes': len(line), 'diag': diag_info,
        # XNU's RAM window the ADT asks for: maint-session.py writes the ESP only onto a boot_a whose DarwinBoot takes it
        'ram_window': window,
        # the payload as it is on the base ESP (IMAGE:PATH; tooling/persist/base-seed.py and common.installed_trust read real files only)
        'inputs': {role: {'path': f'{a.name}.img:{path}', 'sha256': h(staged[role])} for role, path in payload.items()},
        'overrides': overrides, 'arg_edits': {'add': edits, 'drop': list(a.drop_arg)}, 'boot_args_split': split,
        'firmware': {'files': [{'path': f'{firmware.ESP_DIR}/{pin["path"]}', 'bytes': pin['size'], 'private': pin['private'],
                                'sha256': None if pin['private'] else pin['sha256'], 'placed': True, 'problem': None} for pin in fw]},
        'seed': seed,
        'store': store,
        'data_store': {'seed': seed['identity_sha256'], 'partition': part, 'first_lba': lba, 'bound': True, 'replace': None,
                       'kc_has_store_guard': store_guard},
        'linux_rootfs': 'no entry uses it (checked: no entry names it, every UFS root and store is on ' + part + ')',
        'dropped': dropped,
        'fat': {k: v for k, v in new.items() if k not in ('files', 'problems')},
        'files': {n: ('private' if base['files'].get(n) == 'private' or
                      any(pin['private'] and n == f'{firmware.ESP_DIR}/{pin["path"]}' for pin in fw) else v) for n, v in want.items()},
        'unixnu_toml': new_text,
    }
    common.write_json(out / f'{a.name}.json', record)
    print(f'{image.name}: {record["bytes"]} bytes sha256 {record["sha256"]}; from {base["image"]}: {a.entry} ({len(line)} bytes, '
          f'byte for byte), payload {payload_dir.removeprefix(ROOT)}, store on {part} LBA {lba}-{store["last_lba"]} bound, seed '
          f'{seed["identity_sha256"][:16]}; entries {[e["id"] for e in entries]} + actions'
          + ('' if diag_info['included'] else f'; no -diag entry ({diag_info["why"]})') +
          f'; dropped {len(dropped)} files of the base (the other payloads)')


if __name__ == '__main__':
    main()
