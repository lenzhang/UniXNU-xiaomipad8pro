#!/usr/bin/env python3
"""The installed ESP with a second OS entry: "macos-27.3" (the 27.3 line, docs/k273-line.md) beside today's "macos" (27.0.1).

  build-dual-esp.py [--base ESP.json] --kc KC --adt ADT --trust TRUST --seed SEED [--name esp-dual] [--out out/persistent]
                    [--id macos-27.3] [--title "macOS 27.3"] [--arg NAME=VALUE ...]

The base is an ESP build record of tooling/persist/build-esp.py (default: the installed one, out/persistent/installed.json's image) and
its image. Everything of the base stays as it is, byte for byte: every file (the 27.0.1 collection, ADT, trust cache, seed, firmware)
and every entry of its unixnu.toml (the default, the timeout, macos, macos-diag, fastboot, ...). Only added:
  EFI/UniXNU/Payloads/k273/{kernel.macho,kernel.macho.adt,kernel.macho.trustcache,scratch-data.img}   the 27.3 line's payload
  [[entry]] macos-27.3 after the base's macos entries: path Payloads/k273/kernel.macho, initrd Payloads/k273/scratch-data.img, args
      the base's macos line with native-ufs-root-part=linux_data and data-store-lba=N (and --arg's), so the drivers and their levels
      are the daily profile's
The 27.3 entry's Data store is its own (docs/k273-line.md §6): NativeUfs puts the window at the end of linux_data (the root partition
it was given), data-store-lba=N makes NativeUfs refuse any other place and NativeRamBlock keep any store not bound to N
(PersistOverlay.h kBound), and its seed (another identity and size than the installed one) keeps the seed guard between the two as
well. The 27.0.1 entries never name linux_data and their collection has no data-store-lba: they cannot reach the 27.3 store either.
Checks: the base image is its record's; the 27.3 collection links only this repository's kexts and has the binding and the boot
guard; its firmware pins are the base's files; the ADT asks for the base's RAM window (the installed boot_a's DarwinBoot takes it);
every command line is under DarwinBoot's 1000 bytes; the image is read back by the independent FAT reader and by mtools.
The record (OUT/NAME.json) is a build-esp.py record (maint-session.py esp writes it the same way), its seed the base's (the store
in linux_rootfs, which maint-session.py checks against), with "k273" for the second entry: its seed, its store window and the line.
"""
import argparse
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(D / 'tooling/persist'))
import common  # noqa: E402
import firmware  # noqa: E402

spec = importlib.util.spec_from_file_location('build_esp', D / 'tooling/persist/build-esp.py')
build_esp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_esp)

ENTRY_MARKERS = [   # the 27.3 entry's arguments need these in its collection
    (b'data-store-lba=%llu', 'NativeRamBlock\'s window binding (data-store-lba)'),
    (b'piano-boot-guard: healthy after %u s', 'k27 0063, the boot guard'),
    (b'root-part=%s', 'NativeUfs\'s native-ufs-root-part'),
    (b'piano-power-rp-max', 'PianoPower\'s stepped Type-C current'),
]


def entry_text(entry):
    return ''.join(f'{k} = "{v}"\n' for k, v in entry.items())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--base', type=Path, help='the base ESP\'s build record (default: the installed one)')
    for name in ('kc', 'adt', 'trust', 'seed'):
        p.add_argument('--' + name, type=Path, required=True, help=f'the 27.3 entry\'s {name}')
    p.add_argument('--id', default='macos-27.3')
    p.add_argument('--title', default='macOS 27.3 (Mac17,11)')
    p.add_argument('--arg', action='append', default=[], help='one more argument for the 27.3 entry (NAME=VALUE, replaces NAME)')
    p.add_argument('--root-json', type=Path, default=common.DEVICE / 'out/inputs/k273/root/root.json',
                   help='the 27.3 root image\'s record (tooling/k273/make-root-image.sh): the store must lie after it')
    p.add_argument('--name', default='esp-dual')
    p.add_argument('--out', type=Path, default=common.DEVICE / 'out/persistent')
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
    for n in ('kc', 'adt', 'trust', 'seed'):
        if not getattr(a, n).is_file():
            raise SystemExit(f'--{n} {getattr(a, n)}: missing')

    # the 27.3 collection: the native kext rule, the markers its arguments need
    natives, vm_era, _, kc_guard = build_esp.kc_bundles(a.kc)
    if vm_era:
        raise SystemExit(f'{a.kc} links {vm_era}: the native kext rule allows only this repository\'s kexts')
    kc_bytes = a.kc.read_bytes()
    for marker, why in ENTRY_MARKERS:
        if marker not in kc_bytes:
            raise SystemExit(f'{a.kc} lacks {why} ("{marker.decode()}")')
    for bundle in build_esp.PROFILES['daily']['kexts'] + build_esp.CHARGING[2]['kexts']:
        if bundle not in natives:
            raise SystemExit(f'{a.kc} lacks {bundle}, which the daily arguments turn on')
    # the RAM window: the installed boot_a's DarwinBoot takes the base's, so the same
    window, base_window = common.adt_ram_window(a.adt), base.get('ram_window')
    if window != base_window:
        raise SystemExit(f'{a.adt} asks for a {window} byte RAM window, the base ESP {base_window}: make the ADT with '
                         f'tooling/adt/make-piano-adt.py --ram-window {base_window:#x}')
    if common.adt_addon_slot(a.adt):
        raise SystemExit(f'{a.adt} has the GpuAddon slot of a RAM test image')

    # the 27.3 entry's store
    seed = common.seed_info(a.seed)
    first = common.k273_store_first(seed)
    root = json.loads(a.root_json.read_text())
    root_last = root['write']['last_lba']
    if root['write']['first_lba'] != common.DATA_FIRST or first <= root_last:
        raise SystemExit(f'the store window at LBA {first} is not after the root image (LBA {root["write"]["first_lba"]}-{root_last})')
    base_seed = base['seed']['identity_sha256']
    if seed['identity_sha256'] == base_seed:
        raise SystemExit('the 27.3 seed is the installed seed: the two stores must not share a seed identity')

    with tempfile.TemporaryDirectory(prefix='piano-dual-esp-') as scratch:
        stage = Path(scratch) / 'stage'
        stage.mkdir()
        build_esp.mtools('mcopy', '-i', str(base_img), '-s', '-n', '::/EFI', str(stage))
        have = {str(f.relative_to(stage)): common.sha(f) for f in sorted(stage.rglob('*')) if f.is_file()}
        recorded = {n: h for n, h in base['files'].items() if h != 'private'}
        if any(have.get(n) != h for n, h in recorded.items()) or set(have) != set(base['files']):
            raise SystemExit(f'{base_img}: its files differ from its record')
        toml_path = stage / 'EFI/UniXNU/unixnu.toml'
        text = toml_path.read_text()
        if text != base['unixnu_toml']:
            raise SystemExit(f'{base_img}: unixnu.toml differs from its record')
        config = tomllib.loads(text)
        entries = config['entry']
        ids = [e['id'] for e in entries]
        if a.id in ids:
            raise SystemExit(f'{base_img} has an entry {a.id} already')
        macos = next(e for e in entries if e['id'] == 'macos')
        args = macos['args'].split()
        names = {x.split('=', 1)[0] for x in a.arg}
        args = [x for x in args if x.split('=', 1)[0] not in names | {'native-ufs-root-part', 'data-store-lba'}]
        tail = args.index('data-persist=2')   # the store arguments' place in the base line
        args = args[:tail] + a.arg + ['native-ufs-root-part=linux_data', f'data-store-lba={first}'] + args[tail:]
        common.check_store_size(seed, args)
        line = ' '.join(args)
        if len(line) >= 1000:
            raise SystemExit(f'{a.id}: kernel command line is {len(line)} bytes; DarwinBoot passes at most 1023 (keep under 1000)')
        # firmware: the 27.3 collection's pins must be the files the base carries
        problems = []
        for pin in firmware.kc_pins(a.kc):
            path = f'{firmware.ESP_DIR}/{pin["path"]}'
            if pin['private']:
                if path not in base['files']:
                    problems.append(f'{path}: private pin, not on the base ESP')
            elif have.get(path) != pin['sha256']:
                problems.append(f'{path}: pinned {pin["sha256"][:16]}, the base has {str(have.get(path))[:16]}')
        if problems:
            raise SystemExit('the 27.3 collection\'s firmware pins are not the base ESP\'s files:\n' + '\n'.join(problems))
        k273 = stage / 'EFI/UniXNU/Payloads/k273'
        k273.mkdir()
        for name, src in (('kernel.macho', a.kc), ('kernel.macho.adt', a.adt), ('kernel.macho.trustcache', a.trust),
                          ('scratch-data.img', a.seed)):
            shutil.copyfile(src, k273 / name)
        entry = {'id': a.id, 'title': a.title, 'type': 'xnu', 'path': 'Payloads/k273/kernel.macho',
                 'initrd': 'Payloads/k273/scratch-data.img', 'args': line}
        # after the base's macos entries (macos, macos-diag, macos-prev), before fastboot and the actions
        marker = '[[entry]]\nid = "fastboot"\n'
        if text.count(marker) != 1:
            raise SystemExit(f'{base_img}: unixnu.toml has no single fastboot entry to insert before')
        new_text = text.replace(marker, '[[entry]]\n' + entry_text(entry) + marker)
        parsed = tomllib.loads(new_text)
        if [e for e in parsed['entry'] if e['id'] != a.id] != entries or parsed['boot'] != config['boot']:
            raise SystemExit('the base\'s entries or boot settings changed: refused')
        toml_path.write_text(new_text)
        for path in [stage, *stage.rglob('*')]:
            build_esp.os.utime(path, (build_esp.EPOCH, build_esp.EPOCH))
        want = {str(f.relative_to(stage)): common.sha(f) for f in sorted(stage.rglob('*')) if f.is_file()}
        image = out / f'{a.name}.img'
        image.unlink(missing_ok=True)
        build_esp.mtools('mformat', '-C', '-F', '-S', '5', '-M', '4096', '-T', str(common.ESP_BLOCKS), '-h', '1', '-s', '1', '-c', '1',
                         '-H', str(common.ESP_FIRST), '-v', build_esp.LABEL, '-N', build_esp.SERIAL, '-i', str(image), '::')
        build_esp.mtools('mcopy', '-i', str(image), '-s', '-Q', '-m', str(stage / 'EFI'), '::/')
        minfo = build_esp.mtools('minfo', '-i', str(image), '::').stdout
        back = Path(scratch) / 'back'
        back.mkdir()
        build_esp.mtools('mcopy', '-i', str(image), '-s', '-n', '::/EFI', str(back))
        mback = {str(f.relative_to(back)): common.sha(f) for f in sorted(back.rglob('*')) if f.is_file()}
    fat = common.fat32_read(image)
    got = {n: common.hashlib.sha256(b).hexdigest() for n, b in fat['files'].items()}
    problems = list(fat['problems'])
    if image.stat().st_size != common.ESP_BLOCKS * common.LBA:
        problems.append('image size')
    if fat['sector_bytes'] != 4096 or fat['sectors_per_cluster'] != 1 or fat['total_sectors'] != common.ESP_BLOCKS:
        problems.append('geometry')
    if fat['hidden_sectors'] != common.ESP_FIRST or fat['label'] != build_esp.LABEL or fat['serial'] != build_esp.SERIAL:
        problems.append('hidden sectors, label or serial')
    if got != want or mback != want:
        problems.append('read back differs from the stage')
    if 'sector size: 4096 bytes' not in minfo:
        problems.append('minfo sector size')
    for n, h in base['files'].items():   # the base's files, unchanged in the new image (unixnu.toml: checked above, one entry more)
        if h != 'private' and n != 'EFI/UniXNU/unixnu.toml' and got.get(n) != h:
            problems.append(f'{n} changed')
    if problems:
        raise SystemExit(f'{image}: ' + '; '.join(problems))
    record = dict(base)
    record.update({
        'status': 'OFFLINE_BUILD_NOT_DEVICE_TESTED', 'image': image.name, 'bytes': image.stat().st_size, 'sha256': common.sha(image),
        'about': f'{base["image"]} (its entries and files byte for byte) + the {a.id} entry (docs/k273-line.md)',
        'base_esp': {'image': base['image'], 'sha256': base['sha256'], 'record': base_json.name},
        'files': {n: ('private' if base['files'].get(n) == 'private' else h) for n, h in want.items()},
        'unixnu_toml': new_text,
        'cmdlines': {**base['cmdlines'], a.id: line},
        'fat': {k: v for k, v in fat.items() if k not in ('files', 'problems')},
        'k273': {'id': a.id, 'title': a.title, 'cmdline': line, 'bytes': len(line),
                 'inputs': {n: {'path': str(getattr(a, n)), 'sha256': common.sha(getattr(a, n))} for n in ('kc', 'adt', 'trust', 'seed')},
                 'kc_natives': natives, 'kc_has_boot_guard': kc_guard, 'ram_window': window, 'seed': seed,
                 'store': {'partition': 'linux_data', 'first_lba': first, 'blocks': seed['store_blocks'],
                           'last_lba': first + seed['store_blocks'] - 1, 'reserve_from': common.DATA_LAST + 1 - common.RESERVE_BLOCKS,
                           'bound': True, 'other_store': f'linux_rootfs, seed {base_seed[:16]}, unbound',
                           'root_first_lba': root['write']['first_lba'], 'root_last_lba': root_last,
                           'root_image_sha256': root['image_sha256']}},
    })
    common.write_json(out / f'{a.name}.json', record)
    print(f'{image.name}: {record["bytes"]} bytes sha256 {record["sha256"]}; base {base["image"]} kept; {a.id}: {len(line)} bytes of '
          f'arguments, store on linux_data LBA {first}-{first + seed["store_blocks"] - 1} bound, seed {seed["identity_sha256"][:16]} '
          f'({seed["logical_bytes"] >> 30} GiB)')


if __name__ == '__main__':
    main()
