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

Configuration (selectable, see PROFILES): the k27 arguments of build-ram-images.py K27_ARGS without piano-reset-after/-to plus the
profile's and --arg's additions. Entries:
  macos        the arguments + sheng-smp=1
  macos-diag   the same + piano-reset-after=DIAG piano-reset-to=bootloader (a run of DIAG s, then ABL fastboot, as the loop)
  macos-prev   only with --prev-kc (Payloads/previous/)
  fastboot     the seed, no UFS root, data-persist=0, no pstore console (the last run's log stays): reset into ABL fastboot after 1 s
  reset, shutdown, tools
  build-esp.py --profile base|softgpu|gpu|daily [--default macos|macos-diag] [--arg NAME=VALUE ...] [--rtc-offset N|sdam] [--from REPO]
               [--fast-boot 0|1|2] [--boot-guard SECS] [--replace-data [1|HEX8]] [--flatten-data] [--sensor-pack PACK]
               [--audio-preset FSM [--audio-preset-any]]
               [--kc K --adt A --trust T --seed S] [--name esp-diag] [--out out/persistent]

The Data store (docs/data-store.md): the macos entries open it read-write (data-persist=2). When the installed seed is known
(PIANO_INSTALLED_SEED, or out/persistent/installed.json) and differs from this ESP's seed, the first boot of this ESP finds another
seed's store: with the store guard in the collection it keeps it and runs Data volatile (unless the store is self-contained, which any
seed resumes); --replace-data adds data-store-replace (=1: any other seed's store; =HEX8: only the store of that seed) for an intended
one-time switch that starts the store over (the owner's Data changes are dropped). A collection without the guard would replace the
store silently: refused then, unless --replace-data says so. --flatten-data adds data-store-flatten=1 (the store of this ESP's seed
becomes self-contained at its next boot; a no-op once it is).

The clock (docs/drivers/rtc.md, docs/k27-userland-args.md): droidvm-utc=<--utc> is always there, the OS image's settime.sh sets it
when the calendar is still near 1970. --rtc-offset N (measured once: tooling/native/rtc-offset.py) adds piano-rtc=1
piano-rtc-offset=N, --rtc-offset sdam only piano-rtc=1 (the offset from the PMIC's SDAM cell): the kernel (k27 0060, checked in the collection) then starts the calendar from the
PMIC RTC, settime.sh sees a set clock and leaves it, and droidvm-utc only matters when the RTC gives no date.
The boot guard (docs/boot-guard.md, k27 0063): --boot-guard SECS adds piano-boot-guard=SECS to the macos entries (the daily profile
has it at 180 s unless --boot-guard 0): every boot of the installed system holds the Gunyah watchdog from the start, with the PMIC
restart reason bootloader until it is healthy at SECS s (launchd has run, every CPU takes interrupts), normal after. A hang or panic
in the boot then lands in ABL fastboot, a later hang restarts macOS. The kernel in the collection must have 0063 (checked); macos-diag
keeps piano-reset-after, to which the guard defers, and the fastboot entry has none of it.
The sensors (docs/drivers/sensors.md section 5): --sensor-pack PACK puts this tablet's own sensor pack (made by
package/PianoSensors/tools/make-sensor-pack.py under the ignored out/ or private/) at EFI/UniXNU/piano-sensors.pack, where PianoSensors
level 2 reads it. The image then holds this unit's calibration: keep it local, never publish or share it, and never use another
unit's pack.
The amplifiers (docs/drivers/audio.md section 10.3): --audio-preset FSM puts the owner's own copy of the stock amplifier preset
(foursemi/piano/fs19xx.fsm of the tablet's odm_a, 4348 bytes) at EFI/UniXNU/piano-fs19xx.fsm, where PianoAudio level 5 reads it. It is
checked as the kernel checks it (the container's size field and CRC16) and must be the stock file (size and SHA-256 as Ubuntu publishes
them for OS3.0.303.0.WPYCNXM) unless --audio-preset-any; only its sha256 goes into the record. It is vendor data: never commit it, and
keep the image local.
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
        'kexts_absent': ['com.unixnu.driver.NativeSoftGpu'],
        # k27 0063 (docs/boot-guard.md): the installed system always guards its boot; --boot-guard 0 leaves it out
        'boot_guard': 180},
}
# Profile arguments the menu's "fastboot" entry (a reset after 1 s) leaves out.
NOT_IN_FASTBOOT = ('piano-gpu', 'piano-gpu-delay', 'piano-power', 'piano-usb', 'piano-usb-hid')
DROPPED = ('piano-reset-after', 'piano-reset-to', 'data-persist', 'droidvm-utc')
# Set only by their own options (--replace-data, --flatten-data; data-persist is always 2 here), so the intent is explicit and recorded.
STORE_ARGS = ('data-persist', 'data-store-replace', 'data-store-flatten')
# k27 0063's arguments (piano-boot-guard, -exec, -ping, -cpus, -test): never on the fastboot entry.
GUARD_PREFIX = 'piano-boot-guard'


def store_args(a):
    """The macos entries' Data store arguments beyond data-persist=2 (docs/data-store.md)."""
    out = []
    if a.replace_data:
        value = a.replace_data.lower().removeprefix('0x')
        if value != '1' and not re.fullmatch(r'[0-9a-f]{8}', value):
            raise SystemExit(f'--replace-data {a.replace_data}: 1 (any other seed\'s store) or the store\'s seed as 8 hex digits')
        out.append('data-store-replace=' + ('1' if value == '1' else '0x' + value))
    if a.flatten_data:
        out.append('data-store-flatten=1')
    return out


def loud(text):
    print('\n'.join(['', '*' * 100, *[f'*  {line}' for line in text.splitlines()], '*' * 100, '']), file=sys.stderr, flush=True)


def check_trust(a, trust):
    """A self-contained (flattened) store keeps the Metal plugin the installed trust cache trusts, whatever seed a new ESP brings: a
    trust cache without those CDHashes stops it loading (WindowServer crash loop). A note when CDHashes are missing."""
    installed = a.installed_trust or common.installed_trust(a.out)
    if not installed:
        return None
    missing = common.trust_hashes(installed) - common.trust_hashes(trust)
    if missing:
        loud(f'{len(missing)} CDHash(es) of the installed trust cache ({installed.name}) are NOT in this ESP\'s trust cache. If the '
             'store is self-contained (flattened), it keeps the installed /Library/GPUBundles plugin, which would then no longer load '
             '(WindowServer crash loop). Stage this seed\'s plugin with tooling/k27/stage-soft-metal.py --trust <the installed trust '
             'cache> (its modules are kept), or build this ESP with --trust <the installed trust cache>')
    return {'installed_trust': str(installed), 'missing_cdhashes': len(missing)}


def check_store(a, seed, kc):
    """This ESP's seed against the installed one, and the collection's store guard: a record, a loud note, or a refusal."""
    installed, source = common.installed_seed(a.out)
    same = common.same_seed(seed['identity_sha256'], installed)
    guard = common.kc_has_store_guard(kc)
    this, theirs = seed['identity_sha256'][:8], (installed or '')[:8]
    common.check_store_size(seed)
    if a.flatten_data and not guard:
        raise SystemExit(f'--flatten-data: {kc} has no store guard (NativeRamBlock before docs/data-store.md): it cannot flatten')
    if installed and not same:
        if not guard and not a.replace_data:
            raise SystemExit(f'this ESP\'s seed {this} is not the installed seed {theirs} ({source}), and {kc} has no store guard: its '
                             'first boot would replace the store and drop the owner\'s Data. Build the collection from this tree, or '
                             'say --replace-data if that is the intent')
        if a.replace_data:
            loud(f'--replace-data: this ESP\'s first boot STARTS THE STORE OVER for seed {this} (installed seed {theirs}, {source}): the '
                 'owner\'s Data changes are dropped' + ('' if guard else ' (this collection has no store guard: it would anyway)'))
        else:
            loud(f'this ESP\'s seed {this} is not the installed seed {theirs} ({source}). With the store guard its boots keep that store '
                 'untouched and run Data VOLATILE ("the store belongs to seed ...: kept"), unless the store is self-contained '
                 '(flattened: then any seed resumes it). --replace-data starts the store over for this seed instead')
    elif installed is None:
        loud(f'the installed seed is unknown ({source}); this ESP\'s seed is {this}. '
             + ('With the store guard another seed\'s store is kept (Data volatile) and a self-contained one resumed.' if guard else
                f'{kc} has NO store guard: if the store is another seed\'s, its first boot replaces it and drops the owner\'s Data.'))
    elif a.replace_data:
        loud(f'--replace-data with the installed seed {this}: inert (data-store-replace only acts on another seed\'s store)')
    return {'installed_seed': installed, 'installed_seed_from': source, 'same_seed': same, 'kc_has_store_guard': guard,
            'replace': a.replace_data, 'flatten': a.flatten_data}


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


# The VM-era kexts' bundle identifiers (the native kext rule's denylist, tooling/k27/check-native-kexts.py): their legacy
# "DroidVM" name and the virtio block kext. None may be in an installed collection.
VM_ERA_BUNDLE = re.compile(rb'com\.[A-Za-z0-9.]*[Dd]roid[Vv][Mm][A-Za-z0-9.]*|com\.unixnu\.driver\.NativeVirtioBlock')


def kc_bundles(path):
    data = Path(path).read_bytes()
    if data[:4] != b'\xcf\xfa\xed\xfe' or int.from_bytes(data[12:16], 'little') != 12:
        raise SystemExit(f'{path}: not an MH_FILESET kernel collection')
    natives = sorted({m.decode() for m in re.findall(rb'com\.unixnu\.driver\.[A-Za-z0-9]+', data)})
    vm_era = sorted({m.decode() for m in re.findall(VM_ERA_BUNDLE, data)})
    rtc = b'piano-rtc: count %u + offset %u' in data   # the kernel has k27 0060
    guard = b'piano-boot-guard: healthy after %u s' in data   # the kernel has k27 0063
    return natives, vm_era, rtc, guard


def boot_guard(a, profile):
    """The boot guard's seconds for the macos entries (0: none): --boot-guard, else the profile's; --arg piano-boot-guard=N instead
    of the option is refused, so the record always says it."""
    named = [x for x in a.arg if x.split('=', 1)[0] == GUARD_PREFIX]
    if named:
        raise SystemExit(f'--arg {named[0]}: the boot guard comes from --boot-guard SECS (0: none)')
    secs = getattr(a, 'boot_guard', None)
    if secs is None:
        secs = profile.get('boot_guard', 0)
    if secs < 0 or secs > 86400:
        raise SystemExit(f'--boot-guard {secs}: seconds 1..86400, or 0 for none')
    if 0 < secs < 60:
        raise SystemExit(f'--boot-guard {secs}: launchd runs about 30-50 s into a boot; under 60 s the guard would reset a sound '
                         'boot into ABL fastboot (docs/boot-guard.md)')
    return secs


def rtc_args(value):
    """--rtc-offset: N (UTC - the RTC's count, 0..2^32-1) or 'sdam' -> the kernel arguments of k27 0060."""
    if value is None:
        return []
    if value == 'sdam':
        return ['piano-rtc=1']
    if not re.fullmatch(r'\d+', value) or int(value) >= 1 << 32:
        raise SystemExit(f'--rtc-offset {value}: a number 0..4294967295 (tooling/native/rtc-offset.py prints it) or "sdam"')
    return ['piano-rtc=1', f'piano-rtc-offset={int(value)}']


def args_of(a, profile, utc):
    ram = common.ram_images()
    base = [x for x in ram.K27_ARGS if x.split('=', 1)[0] not in DROPPED]
    if a.rtc_offset is not None and any(x.split('=', 1)[0] in ('piano-rtc', 'piano-rtc-offset') for x in a.arg):
        raise SystemExit('--rtc-offset and --arg piano-rtc...: one of them')
    guard = boot_guard(a, profile)
    extra = profile['args'] + a.arg + rtc_args(a.rtc_offset) + ([f'{GUARD_PREFIX}={guard}'] if guard else [])
    names = {x.split('=', 1)[0] for x in extra}
    if names & set(STORE_ARGS):
        raise SystemExit(f'--arg {", ".join(sorted(names & set(STORE_ARGS)))}: the Data store arguments come from --replace-data / '
                         '--flatten-data (data-persist is always 2 on the macos entries)')
    # --fast-boot N: build-ram-images.py's PIANO_FAST_BOOT=N (docs/boot-time.md), given here explicitly so that an environment
    # variable left set for a RAM test never changes an installed ESP.
    base = ram.fast_boot(base, extra, a.fast_boot)
    base = [x for x in base if x.split('=', 1)[0] not in names] + extra
    base += ['data-persist=2', f'droidvm-utc={utc}']
    store = store_args(a)
    macos = base + store + ['sheng-smp=1']
    diag = base + store + [f'piano-reset-after={a.diag_reset_after}', 'piano-reset-to=bootloader', 'sheng-smp=1']
    # The way back to ABL fastboot from the menu: no UFS root, no store, no console mirror (the previous run's log stays in cvp).
    quiet = ('native-ufs-root', 'native-ufs-diagnostics', 'pstore-console', 'pstore-console-size') + NOT_IN_FASTBOOT
    fastboot = [x for x in base if x.split('=', 1)[0] not in quiet + ('data-persist',) and not x.startswith(GUARD_PREFIX)] + [
        'data-persist=0', 'piano-reset-after=1', 'piano-reset-to=bootloader', 'sheng-smp=1']
    lines = {'macos': ' '.join(macos), 'macos-diag': ' '.join(diag), 'fastboot': ' '.join(fastboot)}
    for name, line in lines.items():
        if len(line) >= 1000:
            raise SystemExit(f'{name}: kernel command line is {len(line)} bytes; DarwinBoot passes at most 1023')
        if '"' in line or '\\' in line:
            raise SystemExit(f'{name}: a quote or backslash in the arguments')
    return lines


def clock_text(a):
    utc = f'droidvm-utc {time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(a.utc))}'
    if a.rtc_offset is None:
        return utc
    offset = 'its SDAM offset cell' if a.rtc_offset == 'sdam' else f'offset {a.rtc_offset}'
    return f'the PMIC RTC + {offset} (k27 0060 piano-rtc=1), else {utc}'


def config_text(a, lines, prev, guard=0):
    entry = ('[[entry]]\nid = "{id}"\ntitle = "{title}"\ntype = "xnu"\npath = "Payloads/{dir}kernel.macho"\n'
             'initrd = "Payloads/scratch-data.img"\nargs = "{args}"\n')
    text = ('# piano persistent boot (docs/persistent-boot.md): the ESP on UFS LUN 0, partition 37. boot_a holds only BootShim + FD; a\n'
            '# "fastboot boot" image with its own payload still wins (firmware/patches/unixnu/0001).\n'
            f'# profile {a.profile}{f"; fast boot {a.fast_boot}" if a.fast_boot else ""}'
            f'{f"; boot guard {guard} s" if guard else ""}; clock: {clock_text(a)}\n'
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


def check_sensor_pack(path):
    """The sensor pack as PianoSensors' loader takes it (SensorsLoad.h / SensorsPack.h): \"SSCPACK1\", its own size, at most 4 MiB."""
    path = path.resolve()
    if not path.is_file():
        raise SystemExit(f'--sensor-pack: {path} is missing')
    data = path.read_bytes()
    if len(data) < 32 or data[:8] != b'SSCPACK1' or int.from_bytes(data[16:24], 'little') != len(data):
        raise SystemExit(f'--sensor-pack: {path} is not a sensor pack (package/PianoSensors/tools/make-sensor-pack.py)')
    if len(data) > 4 << 20:
        raise SystemExit(f'--sensor-pack: {path} has {len(data)} bytes; PianoSensors takes at most 4 MiB')
    return path


# The stock amplifier preset's identity (xiaomipad-8pro-mainline firmware/piano-audio.json; package/PianoAudio/AudioSha256.c pins the
# same): only these two facts are in this repository, never the file.
AUDIO_PRESET_BYTES = 4348
AUDIO_PRESET_SHA256 = 'ab060b8476065bd789f327f7f8e724a2e17d94e93a85e2e8aa4b4954ce88b90e'
AUDIO_PRESET_MAX = 64 << 10


def fsm_crc16(data):
    """CRC-16/ARC (polynomial 0x8005 reflected, initial value 0): the container's checksum (Fs19xx.c fsm_crc16)."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def check_audio_preset(path, any_preset):
    """The amplifier preset as PianoAudio's level 5 takes it (AudioAmps.c audio_preset_check): at most 64 KiB, the container's size
    field (offset 22) and CRC16 (offset 24, over bytes 26..) right, the stock file unless any_preset. Returns (path, stock)."""
    path = path.resolve()
    if not path.is_file():
        raise SystemExit(f'--audio-preset: {path} is missing')
    data = path.read_bytes()
    if not 28 <= len(data) <= AUDIO_PRESET_MAX:
        raise SystemExit(f'--audio-preset: {path} has {len(data)} bytes: not an fs19xx.fsm container')
    if int.from_bytes(data[22:24], 'little') != len(data) or int.from_bytes(data[24:26], 'little') != fsm_crc16(data[26:]):
        raise SystemExit(f'--audio-preset: {path} is not a valid fs19xx.fsm container (size field or CRC16)')
    stock = len(data) == AUDIO_PRESET_BYTES and common.sha(path) == AUDIO_PRESET_SHA256
    if not stock and not any_preset:
        raise SystemExit(f'--audio-preset: {path} is not the stock fs19xx.fsm (OS3.0.303.0.WPYCNXM: {AUDIO_PRESET_BYTES} bytes, sha256 '
                         f'{AUDIO_PRESET_SHA256[:16]}...); --audio-preset-any takes it (and the kernel then needs piano-audio-preset-any=1)')
    return path, stock


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
    p.add_argument('--utc', type=int, default=int(time.time()), help='droidvm-utc: the clock when the RTC gives none')
    p.add_argument('--rtc-offset', metavar='N|sdam', help='the calendar from the PMIC RTC (k27 0060): piano-rtc=1 and '
                   'piano-rtc-offset=N (tooling/native/rtc-offset.py), or "sdam" for the PMIC\'s own offset cell')
    p.add_argument('--fast-boot', type=int, choices=(0, 1, 2), default=0,
                   help='build-ram-images.py PIANO_FAST_BOOT: 1 no -v (no console text drawn) and no droidvm-disable-jobs; '
                   '2 also k27-deny-exec for watchdogd (docs/boot-time.md)')
    p.add_argument('--boot-guard', type=int, metavar='SECS',
                   help='piano-boot-guard=SECS on the macos entries (k27 0063, docs/boot-guard.md): the watchdog held from the start, '
                   'the restart reason bootloader until healthy at SECS s; 0 none. Default: the profile\'s (daily 180, others none)')
    p.add_argument('--replace-data', nargs='?', const='1', metavar='1|HEX8',
                   help='data-store-replace on the macos entries: an intended one-time switch to this seed that STARTS THE DATA STORE '
                   'OVER when it belongs to another seed (1: any other seed\'s store; HEX8: only the store of that seed, the 8 hex '
                   'digits NativeRamBlock prints). Without it another seed\'s store is kept and Data runs volatile (store guard)')
    p.add_argument('--flatten-data', action='store_true',
                   help='data-store-flatten=1 on the macos entries: the store of this ESP\'s seed becomes self-contained at the next '
                   'boot (docs/data-store.md); then every later seed boots it')
    p.add_argument('--installed-trust', type=Path, help='the installed ESP\'s trust cache, for the self-contained store\'s plugin check '
                   '(default: from PIANO_INSTALLED_SEED=<record>.json or out/persistent/installed.json)')
    p.add_argument('--sensor-pack', type=Path, metavar='PACK',
                   help="this tablet's sensor pack (make-sensor-pack.py) -> EFI/UniXNU/piano-sensors.pack (holds the unit's calibration)")
    p.add_argument('--audio-preset', type=Path, metavar='FSM',
                   help="the owner's own stock amplifier preset (fs19xx.fsm from odm_a) -> EFI/UniXNU/piano-fs19xx.fsm (vendor data)")
    p.add_argument('--audio-preset-any', action='store_true', help='take a valid preset that is not the stock file')
    p.add_argument('--name', help='output base name (default esp-<profile>-<default>)')
    p.add_argument('--out', type=Path, default=common.DEVICE / 'out/persistent')
    a = p.parse_args()
    profile = PROFILES[a.profile]
    inputs = {n: (getattr(a, n) or a.source / profile[n]).resolve() for n in ('kc', 'adt', 'trust', 'seed')}
    for n, path in inputs.items():
        if not path.is_file():
            raise SystemExit(f'{n}: {path} is missing')
    pack = check_sensor_pack(a.sensor_pack) if a.sensor_pack else None
    preset, preset_stock = check_audio_preset(a.audio_preset, a.audio_preset_any) if a.audio_preset else (None, False)
    natives, vm_era, kc_rtc, kc_guard = kc_bundles(inputs['kc'])
    if vm_era:
        raise SystemExit(f'{inputs["kc"]} links {vm_era}: the native kext rule allows only this repository\'s kexts '
                         '(docs/PROJECT-MAP.md rule 8)')
    if a.rtc_offset is not None and not kc_rtc:
        raise SystemExit(f'--rtc-offset: the kernel in {inputs["kc"]} has no k27 0060 (build the collection with '
                         'K27_KERNEL=<kernel-k27-0060-native.macho> tooling/k27/build-native-image.sh)')
    guard = boot_guard(a, profile)
    if guard and not kc_guard:
        raise SystemExit(f'--boot-guard {guard} (profile {a.profile}\'s default unless given): the kernel in {inputs["kc"]} has no k27 '
                         '0063 (build the collection with K27_KERNEL=<kernel-k27-0063-native.macho> tooling/k27/build-native-image.sh), '
                         'or say --boot-guard 0')
    for bundle in profile['kexts']:
        if bundle not in natives:
            raise SystemExit(f'profile {a.profile} needs {bundle} in the kernel collection; {inputs["kc"]} has {natives} '
                             f'(build it with PIANO_EXTRA_KEXTS=$PWD/out/k27/kext/<kext>.json tooling/k27/build-native-image.sh)')
    for bundle in profile.get('kexts_absent', []):
        if bundle in natives:
            raise SystemExit(f'profile {a.profile}: {inputs["kc"]} links {bundle}, which this profile excludes')
    for adt in {inputs['adt'], (a.prev_adt or inputs['adt']).resolve()}:
        if common.adt_addon_slot(adt):
            raise SystemExit(f'{adt}: its /chosen/memory-map has the GpuAddon slot of a RAM test image carrying the sensor pack '
                             '(PIANO_SENSOR_PACK): DarwinBoot refuses that slot without Payloads/kernel.macho.gpuaddon, which an ESP never '
                             'carries; use the ADT made without it (out/adt/piano-k27.adt)')
    vmm = adt_vmm_present(inputs['adt'])
    if profile['vmm_present'] is not None and vmm != profile['vmm_present']:
        raise SystemExit(f'profile {a.profile} needs an ADT with vmm-present {profile["vmm_present"]}; {inputs["adt"]} has {vmm}')
    prev = None
    if a.prev_kc:
        prev = {'kc': a.prev_kc.resolve(), 'adt': (a.prev_adt or inputs['adt']).resolve(), 'trust': (a.prev_trust or inputs['trust']).resolve()}
    seed = common.seed_info(inputs['seed'])
    window = seed['store_blocks']
    data_store = check_store(a, seed, inputs['kc'])
    data_store['trust'] = check_trust(a, inputs['trust'])
    lines = args_of(a, profile, a.utc)
    config = config_text(a, lines, prev, guard)
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
        if pack:   # a long name (FAT keeps its case): PianoSensors' SensorsLoad.h SL_PATH
            shutil.copyfile(pack, stage / 'EFI/UniXNU/piano-sensors.pack')
        if preset:   # a long name as well: PianoAudio's AudioPreset.h AUDIO_PRESET_PATH
            shutil.copyfile(preset, stage / 'EFI/UniXNU/piano-fs19xx.fsm')
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
        'profile': a.profile, 'about': profile['about'], 'extra_args': a.arg, 'fast_boot': a.fast_boot, 'default': a.default,
        'timeout': a.timeout,
        'utc': a.utc, 'rtc_offset': a.rtc_offset, 'kc_has_rtc_calendar': kc_rtc, 'boot_guard': guard,
        'kc_has_boot_guard': kc_guard, 'cmdlines': lines,
        'inputs': {n: {'path': str(path), 'sha256': common.sha(path)} for n, path in inputs.items()},
        'previous': {n: {'path': str(path), 'sha256': common.sha(path)} for n, path in prev.items()} if prev else None,
        'kc_natives': natives, 'adt_vmm_present': vmm,
        'sensor_pack': {'sha256': common.sha(pack), 'bytes': pack.stat().st_size, 'private': 'this unit only'} if pack else None,
        'audio_preset': {'sha256': common.sha(preset), 'bytes': preset.stat().st_size, 'stock': preset_stock,
                         'vendor_data': 'the owner\'s own copy: never committed'} if preset else None,
        'seed': seed, 'data_store': data_store,
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
