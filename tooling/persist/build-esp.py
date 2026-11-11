#!/usr/bin/env python3
"""The 1 GiB FAT32 ESP of the persistent boot (docs/persistent-boot.md, plan option A1): GPT partition 37 on LUN 0, LBAs
71527424-71789567 (4096-byte sectors), written by tooling/persist/maint-session.py from the maintenance RAM image. Nothing here
touches a device.

The payload layout is build-ram-images.py's (EFI/UniXNU/unixnu.toml + EFI/UniXNU/Payloads/{kernel.macho,kernel.macho.adt,
kernel.macho.trustcache,scratch-data.img}); with --prev-* a previous kernel collection goes to Payloads/previous/ ("macos-prev").
The volume: mformat (mtools) FAT32, 4096-byte sectors, 1 sector per cluster, hidden sectors = the partition's first LBA, label
PIANOESP, serial 50494e4f; file and directory times SOURCE_DATE_EPOCH 2010-01-01, so the same inputs give the same image.
It is checked by an independent reader (tooling/persist/common.py fat32_read: EDK2 FatPkg's BPB rules, every file's cluster chain
hashed against its input) and by mtools (minfo, mcopy back).

Configuration (selectable, see PROFILES): the k27 arguments of build-ram-images.py K27_ARGS without piano-reset-after/-to (the
watchdog is not armed without them) plus the profile's and --arg's additions. Entries:
  macos        the arguments + sheng-smp=1
  macos-diag   the same + piano-reset-after=DIAG piano-reset-to=bootloader (a run of DIAG s, then ABL fastboot, as the loop)
  macos-prev   only with --prev-kc (Payloads/previous/)
  fastboot     the seed, no UFS root, data-persist=0, no pstore console (the last run's log stays): reset into ABL fastboot after 1 s
  reset, shutdown, tools
  build-esp.py --profile base|softgpu|gpu|daily [--default macos|macos-diag] [--arg NAME=VALUE ...] [--from REPO] [--kc K --adt A --trust T
               --seed S] [--name esp-diag] [--out out/persistent]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

EPOCH = 1262304000
LABEL, SERIAL = 'PIANOESP', '50494e4f'
# Inputs relative to --from (a checkout of this repository whose out/ holds them).
PROFILES = {
    'base': {
        'about': 'build-ram-images.py K27_ARGS as committed: ADT vmm-present 1, the input Data seed and trust cache',
        'kc': 'out/k27/kc-k27-native.bin', 'adt': 'out/adt/piano-k27.adt',
        'trust': 'out/inputs/kernel.macho.trustcache', 'seed': 'out/inputs/scratch-data.img',
        'args': [], 'vmm_present': None, 'kexts': []},
    'softgpu': {
        'about': '2026-11-11 dev additions (docs/drivers/soft-metal.md section 10): ADT vmm-present 0, panic_on_crash_disabled=1, '
                 'NativeSoftGpu in the collection and its staged seed and trust cache (out/soft-metal)',
        'kc': 'out/k27/kc-k27-native.bin', 'adt': 'out/adt/piano-k27-vmm0.adt',
        'trust': 'out/soft-metal/kernel.macho.trustcache', 'seed': 'out/soft-metal/scratch-data.img',
        'args': ['panic_on_crash_disabled=1'], 'vmm_present': 0, 'kexts': ['com.unixnu.driver.NativeSoftGpu']},
    'gpu': {
        'about': 'the A830 path that showed the login screen on 2026-11-11 07:45: PianoA830 in the collection '
                 '(PIANO_EXTRA_KEXTS=$PWD/out/k27/kext/PianoA830.json tooling/k27/build-native-image.sh, with PIANO_VMM_PRESENT=0), '
                 'the Turnip plugin staged into the seed and trust cache (out/turnip-a830/stage), ADT vmm-present 0',
        'kc': 'out/k27/kc-k27-native.bin', 'adt': 'out/adt/piano-k27.adt',
        'trust': 'out/turnip-a830/stage/kernel.macho.trustcache', 'seed': 'out/turnip-a830/stage/scratch-data.img',
        'args': ['piano-gpu=2', 'piano-gpu-delay=20', 'panic_on_crash_disabled=1'], 'vmm_present': 0,
        'kexts': ['com.unixnu.driver.PianoA830'], 'kexts_absent': ['com.unixnu.driver.NativeSoftGpu']},
    'daily': {
        'about': 'the installed configuration from 2026-11-11 evening: gpu + PianoPower level 1 (battery and Type-C readout, no writes, '
                 'the DSP not started) + the USB debug console (PianoDwc3: kernel console, monitor with "reset fastboot", user pipe) '
                 '+ the keyboard and pointer from the Mac (piano-usb-hid=1)',
        'kc': 'out/k27/kc-k27-native.bin', 'adt': 'out/adt/piano-k27.adt',
        'trust': 'out/turnip-a830/stage/kernel.macho.trustcache', 'seed': 'out/turnip-a830/stage/scratch-data.img',
        'args': ['piano-gpu=2', 'piano-gpu-delay=20', 'panic_on_crash_disabled=1', 'piano-power=1', 'piano-usb=2', 'piano-usb-hid=1'],
        'vmm_present': 0,
        'kexts': ['com.unixnu.driver.PianoA830', 'com.unixnu.driver.PianoPower', 'com.unixnu.driver.PianoDwc3'],
        'kexts_absent': ['com.unixnu.driver.NativeSoftGpu']},
}
# Profile arguments the menu's "fastboot" entry (a reset after 1 s) leaves out.
NOT_IN_FASTBOOT = ('piano-gpu', 'piano-gpu-delay', 'piano-power', 'piano-usb', 'piano-usb-hid')
DROPPED = ('piano-reset-after', 'piano-reset-to', 'data-persist', 'droidvm-utc')


def adt_vmm_present(path):
    spec = common.importlib.util.spec_from_file_location('adt_dump', common.DEVICE / 'tooling/adt/adt-dump.py')
    adt = common.importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adt)
    (props, children), _ = adt.parse(Path(path).read_bytes())
    for child_props, _ in children:
        named = dict(child_props)
        if named.get('name', b'').rstrip(b'\0') == b'defaults' and 'vmm-present' in named:
            return int.from_bytes(named['vmm-present'][:4], 'little')
    return None


def kc_bundles(path):
    data = Path(path).read_bytes()
    if data[:4] != b'\xcf\xfa\xed\xfe' or int.from_bytes(data[12:16], 'little') != 12:
        raise SystemExit(f'{path}: not an MH_FILESET kernel collection')
    natives = sorted({m.decode() for m in re.findall(rb'com\.unixnu\.driver\.[A-Za-z0-9]+', data)})
    droidvm = sorted({m.decode() for m in re.findall(rb'com\.[A-Za-z0-9.]*[Dd]roid[Vv][Mm][A-Za-z0-9.]*', data)})
    return natives, droidvm


def args_of(a, profile, utc):
    ram = common.ram_images()
    base = [x for x in ram.K27_ARGS if x.split('=', 1)[0] not in DROPPED]
    extra = profile['args'] + a.arg
    names = {x.split('=', 1)[0] for x in extra}
    base = [x for x in base if x.split('=', 1)[0] not in names] + extra
    base += ['data-persist=2', f'droidvm-utc={utc}']
    macos = base + ['sheng-smp=1']
    diag = base + [f'piano-reset-after={a.diag_reset_after}', 'piano-reset-to=bootloader', 'sheng-smp=1']
    # The way back to ABL fastboot from the menu: no UFS root, no store, no console mirror (the previous run's log stays in cvp).
    quiet = ('native-ufs-root', 'native-ufs-diagnostics', 'pstore-console', 'pstore-console-size') + NOT_IN_FASTBOOT
    fastboot = [x for x in base if x.split('=', 1)[0] not in quiet + ('data-persist',)] + [
        'data-persist=0', 'piano-reset-after=1', 'piano-reset-to=bootloader', 'sheng-smp=1']
    lines = {'macos': ' '.join(macos), 'macos-diag': ' '.join(diag), 'fastboot': ' '.join(fastboot)}
    for name, line in lines.items():
        if len(line) >= 1000:
            raise SystemExit(f'{name}: kernel command line is {len(line)} bytes; DarwinBoot passes at most 1023')
        if '"' in line or '\\' in line:
            raise SystemExit(f'{name}: a quote or backslash in the arguments')
    return lines


def config_text(a, lines, prev):
    entry = ('[[entry]]\nid = "{id}"\ntitle = "{title}"\ntype = "xnu"\npath = "Payloads/{dir}kernel.macho"\n'
             'initrd = "Payloads/scratch-data.img"\nargs = "{args}"\n')
    text = ('# piano persistent boot (docs/persistent-boot.md): the ESP on UFS LUN 0, partition 37. boot_a holds only BootShim + FD; a\n'
            '# "fastboot boot" image with its own payload still wins (firmware/patches/unixnu/0001).\n'
            f'# profile {a.profile}; clock (droidvm-utc) {time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(a.utc))}\n'
            'version = 1\n[boot]\ntimeout = ' + str(a.timeout) + '\ndefault = "' + a.default + '"\n[kits]\nautoload = false\n')
    text += entry.format(id='macos', title='macOS (native)', dir='', args=lines['macos'])
    text += entry.format(id='macos-diag', title=f'macOS, reset to fastboot after {a.diag_reset_after} s', dir='', args=lines['macos-diag'])
    if prev:
        text += entry.format(id='macos-prev', title='macOS, previous kernel collection', dir='previous/', args=lines['macos'])
    text += entry.format(id='fastboot', title='Reset into fastboot (ABL)', dir='', args=lines['fastboot'])
    text += ('[[entry]]\nid = "reset"\ntitle = "Restart"\ntype = "action"\naction = "reset"\n'
             '[[entry]]\nid = "shutdown"\ntitle = "Power off"\ntype = "action"\naction = "shutdown"\n'
             '[[entry]]\nid = "tools"\ntitle = "Tools"\ntype = "action"\naction = "tools"\n')
    parsed = tomllib.loads(text)
    ids = [e['id'] for e in parsed['entry']]
    if a.default not in ids:
        raise SystemExit(f'default {a.default} is not one of {ids}')
    return text


def mtools(*command, capture=True):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC', SOURCE_DATE_EPOCH=str(EPOCH))
    return subprocess.run(list(command), check=True, env=env, capture_output=capture, text=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--profile', choices=sorted(PROFILES), required=True)
    p.add_argument('--from', dest='source', type=Path, default=common.DEVICE, help='repository checkout whose out/ holds the inputs')
    for name in ('kc', 'adt', 'trust', 'seed'):
        p.add_argument('--' + name, type=Path, help=f'override the profile\'s {name}')
    p.add_argument('--prev-kc', type=Path, help='a previous kernel collection for "macos-prev" (with --prev-adt, --prev-trust)')
    p.add_argument('--prev-adt', type=Path)
    p.add_argument('--prev-trust', type=Path)
    p.add_argument('--arg', action='append', default=[], help='one more kernel argument NAME=VALUE (replaces the same NAME)')
    p.add_argument('--default', default='macos-diag', help='the menu default (macos-diag for the test phase, macos for daily use)')
    p.add_argument('--diag-reset-after', type=int, default=240)
    p.add_argument('--timeout', type=int, default=3, help='menu seconds (the UEFI log can be read with oem dump meanwhile)')
    p.add_argument('--utc', type=int, default=int(time.time()), help='droidvm-utc: the clock until there is an RTC driver')
    p.add_argument('--name', help='output base name (default esp-<profile>-<default>)')
    p.add_argument('--out', type=Path, default=common.DEVICE / 'out/persistent')
    a = p.parse_args()
    profile = PROFILES[a.profile]
    inputs = {n: (getattr(a, n) or a.source / profile[n]).resolve() for n in ('kc', 'adt', 'trust', 'seed')}
    for n, path in inputs.items():
        if not path.is_file():
            raise SystemExit(f'{n}: {path} is missing')
    natives, droidvm = kc_bundles(inputs['kc'])
    if droidvm:
        raise SystemExit(f'{inputs["kc"]} links {droidvm}: no DroidVM kext natively (docs/PROJECT-MAP.md rule 8)')
    for bundle in profile['kexts']:
        if bundle not in natives:
            raise SystemExit(f'profile {a.profile} needs {bundle} in the kernel collection; {inputs["kc"]} has {natives} '
                             f'(build it with PIANO_EXTRA_KEXTS=$PWD/out/k27/kext/<kext>.json tooling/k27/build-native-image.sh)')
    for bundle in profile.get('kexts_absent', []):
        if bundle in natives:
            raise SystemExit(f'profile {a.profile}: {inputs["kc"]} links {bundle}, which this profile excludes')
    vmm = adt_vmm_present(inputs['adt'])
    if profile['vmm_present'] is not None and vmm != profile['vmm_present']:
        raise SystemExit(f'profile {a.profile} needs an ADT with vmm-present {profile["vmm_present"]}; {inputs["adt"]} has {vmm}')
    prev = None
    if a.prev_kc:
        prev = {'kc': a.prev_kc.resolve(), 'adt': (a.prev_adt or inputs['adt']).resolve(), 'trust': (a.prev_trust or inputs['trust']).resolve()}
    seed = common.seed_info(inputs['seed'])
    window = seed['store_blocks']
    lines = args_of(a, profile, a.utc)
    config = config_text(a, lines, prev)
    name = a.name or f'esp-{a.profile}-{a.default}'
    a.out.mkdir(parents=True, exist_ok=True)
    image = a.out / f'{name}.img'
    with tempfile.TemporaryDirectory(prefix='piano-esp-') as scratch:
        stage = Path(scratch) / 'stage'
        payloads = stage / 'EFI/UniXNU/Payloads'
        payloads.mkdir(parents=True)
        (stage / 'EFI/UniXNU/unixnu.toml').write_text(config)
        shutil.copyfile(inputs['kc'], payloads / 'kernel.macho')
        shutil.copyfile(inputs['adt'], payloads / 'kernel.macho.adt')
        shutil.copyfile(inputs['trust'], payloads / 'kernel.macho.trustcache')
        shutil.copyfile(inputs['seed'], payloads / 'scratch-data.img')
        if prev:
            (payloads / 'previous').mkdir()
            shutil.copyfile(prev['kc'], payloads / 'previous/kernel.macho')
            shutil.copyfile(prev['adt'], payloads / 'previous/kernel.macho.adt')
            shutil.copyfile(prev['trust'], payloads / 'previous/kernel.macho.trustcache')
        for path in [stage, *stage.rglob('*')]:
            os.utime(path, (EPOCH, EPOCH))
        want = {str(f.relative_to(stage)): common.sha(f) for f in sorted(stage.rglob('*')) if f.is_file()}
        image.unlink(missing_ok=True)
        mtools('mformat', '-C', '-F', '-S', '5', '-M', '4096', '-T', str(common.ESP_BLOCKS), '-h', '1', '-s', '1', '-c', '1',
               '-H', str(common.ESP_FIRST), '-v', LABEL, '-N', SERIAL, '-i', str(image), '::')
        mtools('mcopy', '-i', str(image), '-s', '-Q', '-m', str(stage / 'EFI'), '::/')
        # mtools' own view: geometry, and every file copied back out.
        minfo = mtools('minfo', '-i', str(image), '::').stdout
        back = Path(scratch) / 'back'
        back.mkdir()
        mtools('mcopy', '-i', str(image), '-s', '-n', '::/EFI', str(back))
        mback = {str(f.relative_to(back)): common.sha(f) for f in sorted(back.rglob('*')) if f.is_file()}
    if image.stat().st_size != common.ESP_BLOCKS * common.LBA:
        raise SystemExit(f'{image}: {image.stat().st_size} bytes, not the partition\'s {common.ESP_BLOCKS * common.LBA}')
    fat = common.fat32_read(image)
    got = {n: common.hashlib.sha256(b).hexdigest() for n, b in fat['files'].items()}
    problems = list(fat['problems'])
    if fat['sector_bytes'] != 4096 or fat['sectors_per_cluster'] != 1 or fat['total_sectors'] != common.ESP_BLOCKS:
        problems.append('geometry is not 4096-byte sectors, 1 per cluster, 262144 sectors')
    if fat['hidden_sectors'] != common.ESP_FIRST or fat['label'] != LABEL or fat['serial'] != SERIAL:
        problems.append('hidden sectors, label or serial')
    if got != want:
        problems.append(f'independent reader: files differ from the stage ({sorted(set(got) ^ set(want)) or "contents"})')
    if mback != want:
        problems.append('mtools copy-back differs from the stage')
    if 'sector size: 4096 bytes' not in minfo:
        problems.append('minfo does not report 4096-byte sectors')
    if problems:
        raise SystemExit(f'{image}: ' + '; '.join(problems))
    record = {
        'status': 'OFFLINE_BUILD_NOT_DEVICE_TESTED', 'image': image.name, 'bytes': image.stat().st_size, 'sha256': common.sha(image),
        'partition': {'lun': 0, 'index': common.ESP_INDEX + 1, 'first_lba': common.ESP_FIRST, 'blocks': common.ESP_BLOCKS,
                      'dd': f'bs=1M seek={common.ESP_FIRST * common.LBA >> 20} count={common.ESP_BLOCKS * common.LBA >> 20}'},
        'profile': a.profile, 'about': profile['about'], 'extra_args': a.arg, 'default': a.default, 'timeout': a.timeout,
        'utc': a.utc, 'cmdlines': lines,
        'inputs': {n: {'path': str(path), 'sha256': common.sha(path)} for n, path in inputs.items()},
        'previous': {n: {'path': str(path), 'sha256': common.sha(path)} for n, path in prev.items()} if prev else None,
        'kc_natives': natives, 'adt_vmm_present': vmm,
        'seed': seed,
        'store': {'window_blocks': window,
                  'first_lba_now': common.store_first(common.ROOTFS_LAST, window),
                  'first_lba_after': common.store_first(common.ROOTFS_NEW_LAST, window)},
        'fat': {k: v for k, v in fat.items() if k not in ('files', 'problems')},
        'files': want, 'unixnu_toml': config,
    }
    common.write_json(a.out / f'{name}.json', record)
    print(f'{image.name}: {record["bytes"]} bytes sha256 {record["sha256"]} (profile {a.profile}, default {a.default}, '
          f'seed identity {seed["identity_sha256"][:16]})')


if __name__ == '__main__':
    main()
