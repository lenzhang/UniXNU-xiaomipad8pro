#!/usr/bin/env python3
"""The two piano RAM-boot images of plan section 4(b), for "fastboot boot" only (never flashed):

  boot-piano-uefi-ram-<fd>.img    UEFI (pianoPkg FD) + the UniXNU menu; the payload has no OS entry, the menu restarts
                                  after 60 s, so an unattended run returns to Ubuntu.
  boot-piano-macos-ram-<fd>.img   the same + the k27 native kernel collection, the piano ADT, trust cache and Data seed.

Kernel field: UniXNU's BootShim (uefi-sheng's BootShim.S through bootimg.compile_shim, FD base/size from Resources/Configs/piano.toml) + FD + the boot DTB
appended, uncompressed (piano.toml kernel_compression = "none"). Container: the stock v2 header of the Ubuntu RAM set
(xiaomipad-8pro-mainline tools/build-boot.py, v1.1.0 boot-ram-maintenance.img): base 0, kernel 0x8000, ramdisk 0x1000000,
tags 0x100, dtb 0x1f00000, page 4096, the DTB also in the v2 dtb section, made by the same AOSP mkbootimg.py. The ramdisk is
the FAT payload (UniXNU payload layout, written with UniXNU's payload.write_volume). Signing is a separate step
(sign-on-remote.sh); verify-ram-images.py checks everything offline.

  build-ram-images.py --fd FD --fd-name NAME --out DIR [--kc KC --trust T --seed S --adt ADT] [--utc SECONDS]

The Data store (docs/data-store.md): a macOS image opens the tablet's store (data-persist=2) only when its seed is the installed one
(PIANO_INSTALLED_SEED in ~/.config/piano/env, or out/persistent/installed.json), else it runs Data volatile (data-persist=0), with a
note on stderr either way. PIANO_DATA_PERSIST=0|1|2 overrides that (2 with another seed: the RAM test of the store guard, which keeps
the store); PIANO_DATA_STORE_REPLACE=1|<8 hex> and PIANO_DATA_STORE_FLATTEN=1 add data-store-replace / data-store-flatten=1.

PIANO_SENSOR_PACK=PACK (docs/drivers/sensors.md section 5b): the macOS image (and the dump image) carries this unit's sensor pack as
Payloads/kernel.macho.gpuaddon (package/PianoSensors/tools/sensor_addon.py), which DarwinBoot hands to the kernel through the ADT's
GpuAddon slot and PianoSensors takes before the ESP's; the ADT must have the slot (make-piano-adt.py --addon: build-native-image.sh makes
out/adt/piano-k27-addon.adt with PIANO_SENSOR_PACK) and an ADT with the slot is refused without a pack. Only a pack under this
repository's out/ or private/; the image then holds the unit's calibration (keep it local); the record has its sha256 only.
The firmware (docs/kc-space.md, package/include/PianoFirmware.h): a macOS image also carries the files its collection's kexts pin
(tooling/persist/firmware.py finds the pins in the KC), as entries EFI/UniXNU/firmware/<path> of the same addon (one container
for both: PianoSensors takes only its entry, the firmware loader only its own), taken from PIANO_FW_DIR (default out/firmware, staged
by the kexts' build scripts) after their size and SHA-256 are checked. The payload's ADT has the GpuAddon slot exactly when the
payload has the addon: the input ADT's (made with --addon for PIANO_SENSOR_PACK), or added to the payload's copy when only firmware
is carried. A collection without pins adds nothing, so the image is as before. PIANO_FW_ADDON=1 carries the firmware (default 0 until proven on the tablet: the kexts
then read the tablet's ESP); a pin whose file is not staged is left out with a note.
PIANO_DENY_EXEC=PATH[,PATH...] (docs/power.md 1.2): those executables are denied (k27-deny-exec, with the kernel's default
mobileactivationd and PIANO_FAST_BOOT=2's watchdogd in the same argument), e.g. /usr/libexec/PerfPowerServices for a test image
without the daemon that spun a core in every boot of 2026-11-12. launchd keeps retrying the job, throttled.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[2]
RESEARCH = Path(os.environ.get('UNIXNU_RESEARCH', Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research'))
sys.path.insert(0, str(RESEARCH / 'UniXNU/scripts'))
from unixnu.bootimg import compile_shim  # noqa: E402
from unixnu.config import load_toml  # noqa: E402
from unixnu.payload import write_volume  # noqa: E402

MU = RESEARCH / 'Mu-Silicium-piano'
# UniXNU's BootShim (uefi-sheng 66421cb): image size covers shim + FD, the copied FD is cleaned to PoC and the I-cache
# invalidated before the jump, literal pool before _Payload. Mu-Silicium e51cd877's older BootShim has none of these.
SHIM_TREE = RESEARCH / 'uefi-sheng'
MKBOOTIMG = Path(os.environ.get('PIANO_MKBOOTIMG', Path(os.path.expanduser(os.environ.get('PIANO_V110', str(Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/v1.1.0')))) / 'host-tools/aosp-mkbootimg-prefetched/mkbootimg.py'))
MKBOOTIMG_SHA256 = '37d84b3d162e0bc62e36c1f4e1c63c85ea0caa9f29be023eb2f8efe006ad948c'   # the Ubuntu RAM set's (maintenance-manifest.json)
V2 = ['--header_version', '2', '--pagesize', '4096', '--base', '0', '--kernel_offset', '0x00008000',
      '--ramdisk_offset', '0x01000000', '--tags_offset', '0x00000100', '--dtb_offset', '0x01f00000']
# piano's keys reach UEFI but do not move UniXNU's menu (2026-11-10 22:37), so the macOS image boots its default on its own.
# 5 s: the window in which tooling/native/native-loop.sh reads the previous run's log through the UEFI's debug fastboot.
MENU_TIMEOUT = int(os.environ.get('PIANO_MENU_TIMEOUT', '0'))   # 0: boot the default at once (UniXNU Menu.c); the loop's log dump still fits before ExitBootServices
# 1 CPU (cpus=1) panicked in recount_switch_thread on the tablet (2026-11-10 22:41); the 8-CPU entry is the default.
MENU_DEFAULT = os.environ.get('PIANO_MENU_DEFAULT', 'macos-8cpu')
EPOCH = 1262304000  # UniXNU payload timestamps (2010-01-01)

# The k27 arguments natively and their readers: docs/k27-userland-args.md. The VM-era boot disk's set minus its virtual devices'
# arguments, sheng-touch-hid, and the ones nothing in the native collection reads (2026-11-11: offline-hid-input, offline-hid-home,
# offline-key-events, sheng-aks, sheng-aks-sls, ml-timeout-lock-scale, ml-timeout-lock-panic-scale; ca-triple-buffer,
# ca-free-wakeup: the VM-era Metal plugin's). The droidvm-* names are legacy names the k27 kernel and the OS image read.
K27_ARGS = ['-v', 'debug=0x14e', 'kt_fixed_zones=120', '-no_slto_panic', 'tlto_us=0',
            '-apfs_shared_datavolume', 'offline-apfs-load=1', 'owner-staged-apps=1',
            # droidvm-disable-jobs: the jobs without hardware here, but inert natively (its reader was the iPadOS line's kernel
            # launch policy; k27 has none, and the listed jobs run: docs/boot-time.md). PIANO_FAST_BOOT drops it. The clock
            # and time zone (droidvm-utc, droidvm-tz below) are the OS image's settime.sh boot job's.
            'droidvm-disable-jobs=com.apple.watchdogd,com.apple.identityservicesd,com.apple.locationd,com.apple.CommCenter,com.apple.bluetoothd',
            'mac-amfi-launch-permissive=1', 'union-hide-lower=1', 'malloc-ranges-off=1', 'arm64_maxoffset=0x7FFE000000',
            # 0042: userspace sees 24 MHz over the architected counter (19.2 MHz on SM8750 as in the VM).
            'droidvm-timebase-hz=24000000',
            'exec-env=com.apple.WebKit.WebContent:GIGACAGE_ENABLED=no',
            'rootauth=0', 'vm_compression_limit=1572864', 'droidvm-tz=Asia/Singapore',
            # Unattended native runs (k27 0055/0056, docs/bringup-debugging.md): the console mirrored into cvp@9cb00000 (retained
            # across resets, unused by Ubuntu and the firmware; Ubuntu's ramoops zone is overwritten by the firmware's demura), a reset after PIANO_RESET_AFTER s (Gunyah watchdog as the backstop), and no panic-spin, so a panic restarts
            # (PSCI) and the tablet always comes back to Ubuntu with the log in /sys/fs/pstore/console-ramoops-0.
            'pstore-console=0x9cb00000', 'pstore-console-size=0x700000', 'piano-reset-after=' + os.environ.get('PIANO_RESET_AFTER', '45'),
            # every reset of the run lands in ABL fastboot (k27 0057: PMIC SDAM restart reason), not in the installed OS
            'piano-reset-to=bootloader',
            # Root from UFS (package/NativeUfs): the SHNGROOT windows OS/SystemOS in linux_rootfs, read only, unverified
            # build (no NATIVE_VERIFIED_BLOCK: development tablet); diagnostics in the console log.
            'native-ufs-root=1', 'native-ufs-diagnostics=1',
            # NativeRamBlock's changed Data pages in NativeUfs's store window at the end of linux_rootfs (2: read and write). A RAM image
            # takes the value data_store_args gives (PIANO_DATA_PERSIST, else from the installed seed); build-esp.py sets its own.
            'data-persist=' + os.environ.get('PIANO_DATA_PERSIST', '2')]
# PIANO_RESET_AFTER=0: neither the timed reset nor the restart reason (k27 0056/0057 would be inert at 0 anyway), so the image boots
# as the installed system does, e.g. for k27 0063's boot guard tests (docs/boot-guard.md, tooling/k27/build-guard-test-images.sh):
# nothing then resets the tablet but what the image's own arguments arm.
if os.environ.get('PIANO_RESET_AFTER', '45') == '0':
    K27_ARGS = [x for x in K27_ARGS if x.split('=', 1)[0] not in ('piano-reset-after', 'piano-reset-to')]

# PIANO_FAST_BOOT=1|2 (docs/boot-time.md): the boot-time test switch, off (0) by default, so K27_ARGS above stays what boots today.
#   1: no '-v'. XNU then keeps its console in graphics mode and stops drawing every console line into the boot framebuffer, which
#      this configuration maps as Device memory (video_console.c: VM_WIMG_IO, as BCM2837 has neither HAS_UCNORMAL_MEM nor
#      APPLEVIRTUALPLATFORM) and which it otherwise repaints line by line until WindowServer's first frame (NativeGopDisplay
#      releases the console then). console_write mirrors every byte into the pstore mailbox before that check, so the mailbox and
#      the USB console (piano-usb=2) keep every line. The panel shows no kernel text, and launchd prints no verbose lines.
#      Also no droidvm-disable-jobs: nothing native reads it (0 matches in the k27 kernel and the KC; bluetoothd, locationd and
#      watchdogd run with it in every native run).
#   2: 1 + k27-deny-exec for watchdogd (with the kernel's default, mobileactivationd, which setting the argument replaces): exec of
#      them fails with EPERM, so watchdogd stops its crash loop (no watchdog device; vmm-present 0). Needs panic_on_crash_disabled=1.
FAST_BOOT_DROP = ('-v', 'droidvm-disable-jobs')
FAST_BOOT_DENY_EXEC = 'k27-deny-exec=/usr/libexec/mobileactivationd,/usr/libexec/watchdogd'


def fast_boot_level(value=None):
    value = os.environ.get('PIANO_FAST_BOOT', '0') if value is None else value
    if str(value) not in ('', '0', '1', '2'):
        raise SystemExit(f'PIANO_FAST_BOOT={value}: 0, 1 or 2 (docs/boot-time.md)')
    return int(value or 0)


def fast_boot(args, extra=(), level=None):
    """ARGS (K27_ARGS or a profile's base) for fast-boot LEVEL; EXTRA (the arguments added after them) wins over both changes."""
    level = fast_boot_level(level)
    names = {x.split('=', 1)[0] for x in extra}
    out = [x for x in args if not level or x.split('=', 1)[0] not in FAST_BOOT_DROP]
    if level >= 2 and 'k27-deny-exec' not in names and not any(x.startswith('k27-deny-exec=') for x in out):
        # watchdogd's job is _PanicOnCrash: its xpcproxy, failing the exec, may count as its crash (kern_exit.c's panic)
        if 'panic_on_crash_disabled=1' not in [*args, *extra]:
            raise SystemExit('PIANO_FAST_BOOT=2 denies watchdogd: it needs panic_on_crash_disabled=1 too')
        out.append(FAST_BOOT_DENY_EXEC)
    return out


# k27-deny-exec (k27 0035): exec of each listed path fails with EPERM, so launchd's spawn of that job fails (its xpcproxy exits 78) and
# launchd retries it, throttled (about every 10 s for a KeepAlive job). The kernel's own list is mobileactivationd; setting the argument
# replaces it, so every list here starts with it. The value is at most 255 bytes (the kernel's 256-byte buffer).
KERNEL_DENY_EXEC = '/usr/libexec/mobileactivationd'
DENY_EXEC_MAX = 255


def deny_exec(args, extra=(), paths=()):
    """ARGS with PATHS (absolute executable paths: PIANO_DENY_EXEC's for a RAM image, build-esp.py --deny-exec's for an ESP) added to
    their k27-deny-exec (fast boot 2's, else a new one with the kernel's default), as one argument. A k27-deny-exec in EXTRA (the
    arguments added after ARGS) is the caller's own list: then PATHS must be empty. docs/boot-time.md section 4, docs/power.md 1.2."""
    paths = [p for p in paths if p]
    if not paths:
        return list(args)
    if any(x.startswith('k27-deny-exec=') for x in extra):
        raise SystemExit('k27-deny-exec in the extra arguments and a deny list (PIANO_DENY_EXEC / --deny-exec): give one of them')
    for path in paths:
        if not re.fullmatch(r'/[A-Za-z0-9._/+-]+', path) or '/../' in path or path.endswith('/'):
            raise SystemExit(f'deny list: {path!r} is not an absolute executable path (no commas, spaces or quotes)')
    found = [x for x in args if x.startswith('k27-deny-exec=')]
    listed = found[0].split('=', 1)[1].split(',') if found else [KERNEL_DENY_EXEC]
    for path in paths:
        if path not in listed:
            listed.append(path)
    value = ','.join(listed)
    if len(value) > DENY_EXEC_MAX:
        raise SystemExit(f'k27-deny-exec={value}: {len(value)} bytes, the kernel takes at most {DENY_EXEC_MAX}')
    return [x for x in args if not x.startswith('k27-deny-exec=')] + [f'k27-deny-exec={value}']


def deny_exec_env():
    """PIANO_DENY_EXEC: comma- or space-separated paths for a RAM test image (never read by build-esp.py)."""
    return [p for p in re.split(r'[\s,]+', os.environ.get('PIANO_DENY_EXEC', '')) if p]


def persist_common():
    """tooling/persist/common.py (seed identity, installed seed, store guard), loaded by path."""
    spec = importlib.util.spec_from_file_location('persist_common', DEVICE / 'tooling/persist/common.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def loud(text):
    print('\n'.join(['', '*' * 100, *[f'*  {line}' for line in text.splitlines()], '*' * 100, '']), file=sys.stderr, flush=True)


def firmware_module():
    """tooling/persist/firmware.py (the pins, the staged copies, the addon, the ADT slot), loaded by path."""
    spec = importlib.util.spec_from_file_location('piano_firmware', DEVICE / 'tooling/persist/firmware.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def firmware_addon(payloads, kc, adt, addon):
    """The collection's pinned firmware as entries of the payload's addon, next to ADDON's (the sensor pack's container from
    sensor_pack_addon, or None). Returns (the build record's 'firmware', the addon's bytes or None). sensor_pack_addon has checked
    that the input ADT has the GpuAddon slot exactly when ADDON is there; when only firmware is carried, the payload's copy of the ADT
    gets the slot here, so the payload has both or neither. No hash of a private pin in the record."""
    fw = firmware_module()
    pins = fw.kc_pins(kc)
    directory = Path(os.environ.get('PIANO_FW_DIR') or DEVICE / 'out/firmware')
    record = {'pins': len(pins), 'dir': str(directory), 'entries': 0}
    found = []
    if pins:
        found, problems = fw.staged(pins, directory)
        record['files'] = fw.record(pins, found, problems)
        if problems:
            loud('pinned firmware not staged, left out of this image (the kexts then try the tablet\'s ESP):\n'
                 + '\n'.join(f'{fw.ESP_DIR}/{pin["path"]}: {why}' for pin, why in problems))
        # Opt-in (PIANO_FW_ADDON=1) until a tablet boot has shown DarwinBoot takes the addon with its ADT slot (video.md T0).
        if found and os.environ.get('PIANO_FW_ADDON', '0') == '0':
            loud('PIANO_FW_ADDON=0: no firmware in this image\'s addon; the kexts read their files from the tablet\'s ESP')
            found = []
    if found:
        entries = fw.parse_addon(addon) if addon is not None else []
        if any(path.startswith(fw.ESP_DIR + '/') for path, _, _ in entries):
            raise SystemExit('the sensor pack\'s addon already has firmware entries')
        addon = fw.container(entries + fw.firmware_entries(found))
        if not fw.adt_slot(Path(adt).read_bytes()):
            (payloads / 'kernel.macho.adt').write_bytes(fw.adt_with_slot(Path(adt).read_bytes()))
            record.update(adt_input_sha256=sha(adt), adt_sha256=sha(payloads / 'kernel.macho.adt'))
        record['entries'] = len(found)
        print(f'firmware: {len(found)} of {len(pins)} pinned file(s) in the addon', file=sys.stderr)
    record['addon'] = addon is not None
    if addon is not None:
        record.update(addon_bytes=len(addon), addon_sha256=hashlib.sha256(addon).hexdigest())
    return record, addon


def data_store_args(seed, kc):
    """The Data store arguments of a RAM test image (docs/data-store.md): (arguments, record).
    PIANO_DATA_PERSIST=0|1|2 when set; else 2 only when the image's seed is the installed one (PIANO_INSTALLED_SEED, or
    out/persistent/installed.json), 0 when it is another seed's, and when the installed seed is unknown 2 with a kernel collection that
    has the store guard (another seed's store is then kept and Data runs volatile) or 0 without it. A collection without the guard
    replaces another seed's store: data-persist=2 is refused for it unless the seed is known to be the installed one.
    PIANO_DATA_STORE_REPLACE=1|<8 hex>: data-store-replace (start the installed store over for this seed: drops the owner's Data).
    PIANO_DATA_STORE_FLATTEN=1: data-store-flatten=1 (make this seed's store self-contained; only under the store's own seed)."""
    common = persist_common()
    info = common.seed_info(seed)
    identity = info['identity_sha256']
    installed, source = common.installed_seed()
    same = common.same_seed(identity, installed)
    guard = common.kc_has_store_guard(kc)
    explicit = os.environ.get('PIANO_DATA_PERSIST')
    replace = os.environ.get('PIANO_DATA_STORE_REPLACE', '').strip().lower().removeprefix('0x')
    flatten = os.environ.get('PIANO_DATA_STORE_FLATTEN', '') == '1'
    if explicit is not None and explicit not in ('0', '1', '2'):
        raise SystemExit(f'PIANO_DATA_PERSIST={explicit}: 0, 1 or 2')
    if replace and replace != '1' and not re.fullmatch(r'[0-9a-f]{8}', replace):
        raise SystemExit(f'PIANO_DATA_STORE_REPLACE={replace}: 1 (any other seed\'s store) or the store\'s 8 hex digits')
    mode = explicit if explicit is not None else '2' if same or (installed is None and guard) else '0'
    this, theirs = identity[:8], (installed or '')[:8]
    if (replace or flatten) and mode != '2':
        raise SystemExit('PIANO_DATA_STORE_REPLACE / PIANO_DATA_STORE_FLATTEN need data-persist=2 (PIANO_DATA_PERSIST=2)')
    if (replace or flatten) and not guard:
        raise SystemExit(f'{kc}: no store guard in this kernel collection (NativeRamBlock before docs/data-store.md): it knows neither '
                         'data-store-replace nor data-store-flatten')
    if mode == '2':
        common.check_store_size(info)
    if mode == '2' and not same and not guard and not replace:
        raise SystemExit(f'data-persist=2 with seed {this} and a kernel collection WITHOUT the store guard: its NativeRamBlock would '
                         f'replace a store of any other seed (installed: {theirs or "unknown"}, {source}), dropping the owner\'s Data. '
                         'PIANO_DATA_PERSIST=0, or a collection built from this tree, or PIANO_INSTALLED_SEED if this is the installed seed')
    if mode == '2' and same:
        note = f'data-persist=2: seed {this} is the installed seed ({source}): this image resumes the owner\'s Data store and writes into it'
    elif mode == '2' and installed is None:
        note = (f'data-persist=2: the installed seed is unknown ({source}). This collection has the store guard: a store of another seed '
                'is kept and Data runs volatile ("kept; Data is volatile this boot"); a self-contained store (flattened) is resumed and '
                'written')
    elif mode == '2':
        note = (f'data-persist=2 (asked for) with seed {this}, NOT the installed seed {theirs} ({source}). The store guard keeps that '
                'store untouched and runs Data volatile ("NativeRamBlock: the store belongs to seed ...: kept"); if the store is '
                'self-contained (flattened), this image resumes it and writes into the owner\'s Data')
    elif explicit is None:
        note = (f'data-persist=0: seed {this} is not the installed seed {theirs} ({source}), so this image leaves the store alone and '
                'Data is volatile. PIANO_DATA_PERSIST=2 opens the store (the guard keeps another seed\'s store: the RAM test of the guard)')
    else:
        note = f'data-persist={mode} (asked for): seed {this}'
    if replace:
        note += (f'\nPIANO_DATA_STORE_REPLACE={replace}: data-store-replace: a store of another seed is STARTED OVER for seed {this}: '
                 'the owner\'s Data changes of that store are dropped')
    if flatten:
        note += f'\nPIANO_DATA_STORE_FLATTEN=1: data-store-flatten=1: the store of seed {this} is made self-contained (only that seed\'s)'
    loud(note)
    args = [f'data-persist={mode}']
    if replace:
        args.append('data-store-replace=' + ('1' if replace == '1' else '0x' + replace))
    if flatten:
        args.append('data-store-flatten=1')
    return args, {'data_persist': int(mode), 'seed_identity': identity, 'installed_seed': installed, 'installed_seed_from': source,
                  'same_seed': same, 'kc_has_store_guard': guard, 'replace': replace or None, 'flatten': flatten}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sensor_pack_addon(adt):
    """PIANO_SENSOR_PACK (see the module's text): (the addon's bytes, a record without contents), or (None, None) without it."""
    slot = persist_common().adt_addon_slot(adt)
    value = os.environ.get('PIANO_SENSOR_PACK', '')
    if not value:
        if slot:
            raise SystemExit(f'{adt} has the GpuAddon slot but PIANO_SENSOR_PACK is not set: DarwinBoot refuses a slot without its addon '
                             '(build-native-image.sh makes that ADT only with PIANO_SENSOR_PACK; else out/adt/piano-k27.adt)')
        return None, None
    if not slot:
        raise SystemExit(f'PIANO_SENSOR_PACK: {adt} has no GpuAddon slot, and DarwinBoot refuses an addon without it: make the ADT with '
                         'tooling/adt/make-piano-adt.py --addon (build-native-image.sh does with PIANO_SENSOR_PACK)')
    path = Path(value).resolve()
    if not any(path.is_relative_to((DEVICE / d).resolve()) for d in ('out', 'private')):
        raise SystemExit(f'PIANO_SENSOR_PACK={value}: a sensor pack is taken only from this repository\'s out/ or private/ (it holds the '
                         'unit\'s calibration; package/PianoSensors/tools/make-sensor-pack.py writes it there)')
    if not path.is_file():
        raise SystemExit(f'PIANO_SENSOR_PACK={value} is missing')
    spec = importlib.util.spec_from_file_location('sensor_addon', DEVICE / 'package/PianoSensors/tools/sensor_addon.py')
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    data = path.read_bytes()
    try:
        blob = tool.addon_bytes(data)
    except ValueError as error:
        raise SystemExit(f'PIANO_SENSOR_PACK={value}: {error}')
    digest = hashlib.sha256(data).hexdigest()
    loud(f'PIANO_SENSOR_PACK: this image carries this unit\'s sensor pack ({len(data)} bytes, sha256 {digest[:16]}: its calibration) as '
         'Payloads/kernel.macho.gpuaddon,\nwhich PianoSensors takes before the ESP\'s. Keep the image local (out/ is ignored) and never '
         'share it.')
    return blob, {'sha256': digest, 'bytes': len(data), 'private': 'this unit only', 'carrier': 'Payloads/kernel.macho.gpuaddon'}


def payload(stage, image, label, size_mib):
    # mtools stamps the directories it creates with the current time unless SOURCE_DATE_EPOCH is set (mtools 4.0.50):
    # without it two builds a minute apart differ in two bytes of the FAT.
    os.environ['SOURCE_DATE_EPOCH'] = str(EPOCH)
    for path in [stage, *stage.rglob('*')]:
        os.utime(path, (EPOCH, EPOCH))
    write_volume(stage, image, label, size_mib)


def menu_actions():
    return ('[[entry]]\nid = "reset"\ntitle = "Restart (back to Ubuntu)"\ntype = "action"\naction = "reset"\n'
            '[[entry]]\nid = "shutdown"\ntitle = "Power off"\ntype = "action"\naction = "shutdown"\n'
            '[[entry]]\nid = "tools"\ntitle = "Tools"\ntype = "action"\naction = "tools"\n')


def uefi_payload(work):
    stage = work / 'uefi-stage'
    (stage / 'EFI/UniXNU').mkdir(parents=True)
    (stage / 'EFI/UniXNU/unixnu.toml').write_text(
        '# piano M1: the UniXNU menu from a RAM boot, no OS entry. Untouched for 60 s it restarts (ABL then boots Ubuntu).\n'
        'version = 1\n[boot]\ntimeout = 60\ndefault = "reset"\n[kits]\nautoload = false\n' + menu_actions())
    image = work / 'payload-uefi.img'
    payload(stage, image, 'PIANOUEFI', 8)
    return image


def macos_payload(work, kc, trust, seed, adt, utc):
    stage = work / 'macos-stage'
    payloads = stage / 'EFI/UniXNU/Payloads'
    payloads.mkdir(parents=True)
    shutil.copyfile(kc, payloads / 'kernel.macho')
    shutil.copyfile(trust, payloads / 'kernel.macho.trustcache')
    shutil.copyfile(seed, payloads / 'scratch-data.img')
    shutil.copyfile(adt, payloads / 'kernel.macho.adt')
    addon, sensor_pack = sensor_pack_addon(adt)
    firmware, addon = firmware_addon(payloads, kc, adt, addon)
    if addon is not None:
        (payloads / 'kernel.macho.gpuaddon').write_bytes(addon)
        (payloads / 'kernel.macho.gpuaddon').chmod(0o600)
    # PIANO_EXTRA_ARGS: more kernel arguments for one test image (e.g. piano-usb=2, docs/drivers/usb-device-debug.md).
    extra = os.environ.get('PIANO_EXTRA_ARGS', '').split()
    store = {'data-persist', 'data-store-replace', 'data-store-flatten'} & {x.split('=', 1)[0] for x in extra}
    if store:
        raise SystemExit(f'PIANO_EXTRA_ARGS: {", ".join(sorted(store))}: use PIANO_DATA_PERSIST / PIANO_DATA_STORE_REPLACE / '
                         'PIANO_DATA_STORE_FLATTEN (checked against the installed seed, docs/data-store.md)')
    persist, store_record = data_store_args(seed, kc)
    base = deny_exec(fast_boot(K27_ARGS, extra), extra, deny_exec_env())
    base = [x for x in base if not x.startswith('data-persist=')] + persist + [f'droidvm-utc={utc}'] + extra
    one = ' '.join(base + ['cpus=1'])
    smp = ' '.join(base + ['sheng-smp=1'])
    for line in (one, smp):
        if len(line) >= 1000:
            raise SystemExit(f'kernel command line is {len(line)} bytes; DarwinBoot passes at most 1023')
    entry = ('[[entry]]\nid = "{id}"\ntitle = "{title}"\ntype = "xnu"\npath = "Payloads/kernel.macho"\n'
             'initrd = "Payloads/scratch-data.img"\nargs = "{args}"\n')
    (stage / 'EFI/UniXNU/unixnu.toml').write_text(
        '# piano M2: k27 (macOS 27.0.1) natively at EL1, RAM boot. No OS volume natively yet: expect the kernel log on the panel\n'
        '# and a wait for the root volume. The menu boots the default entry after MENU_TIMEOUT s (-1: wait for a key).\n'
        'version = 1\n[boot]\ntimeout = ' + str(MENU_TIMEOUT) + '\ndefault = "' + MENU_DEFAULT + '"\n[kits]\nautoload = false\n'
        + entry.format(id='macos-1cpu', title='macOS k27 native, 1 CPU', args=one)
        + entry.format(id='macos-8cpu', title='macOS k27 native, 8 CPUs (PSCI CPU_ON)', args=smp)
        + menu_actions())
    content = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
    size_mib = max(64, -(-int(content * 1.08) // (1 << 20)) + 8)
    image = work / 'payload-macos.img'
    payload(stage, image, 'PIANOESP', size_mib)
    return image, {'cmdline_1cpu': one, 'cmdline_8cpu': smp, 'payload_mib': size_mib, 'fast_boot': fast_boot_level(),
                   'deny_exec': deny_exec_env(),
                   'data_store': store_record, 'sensor_pack': sensor_pack, 'firmware': firmware}


def kernel_field(fd, dtb, work, name):
    """The kernel field every piano boot image carries (RAM images and tooling/persist's flashed boot_a): BootShim + FD + DTB."""
    config = load_toml(MU / 'Resources/Configs/piano.toml')
    base, size = config['uefi_fd']['base'], config['uefi_fd']['size']
    firmware = Path(fd).read_bytes()
    if len(firmware) != size:
        raise SystemExit(f'{fd} is {len(firmware):#x} bytes, piano.toml says {size:#x}')
    shim = compile_shim(SHIM_TREE, base, size, config['boot_shim']['requires_kernel_header'], work)
    # piano's ABL places a kernel by its ARM64 header: Mu's BootShim writes the FD base into text_offset with flags 0, so ABL
    # put it at DRAM base + 0xA7100000 (not RAM) and its jump failed (2026-11-10 22:12: OKAY, black panel, back in fastboot).
    # A plain Linux Image header instead (as the Ubuntu kernel's: text_offset 0, flags 0xa = LE, 4 KiB, anywhere); the shim's
    # code is position independent and copies the FD to FD_BASE itself.
    shim = bytearray(shim)
    struct.pack_into('<QQQ', shim, 8, 0, -(-(len(shim) + size) // 0x1000) * 0x1000, 0xa)
    shim = bytes(shim)
    kernel = work / f'{name}.kernel'
    kernel.write_bytes(shim + firmware + Path(dtb).read_bytes())
    return kernel, {'shim_bytes': len(shim), 'fd_base': hex(base), 'fd_size': hex(size), 'kernel_sha256': sha(kernel),
                    'kernel_bytes': kernel.stat().st_size}


def pack(out, fd, dtb, ramdisk, work, name):
    kernel, info = kernel_field(fd, dtb, work, name)
    image = out / f'{name}.img'
    subprocess.run(['python3', str(MKBOOTIMG), *V2, '--kernel', str(kernel), '--ramdisk', str(ramdisk),
                    '--dtb', str(dtb), '--output', str(image)], check=True)
    return image, info


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--fd', type=Path, required=True)
    p.add_argument('--fd-name', required=True, help='A-bundled or B-own303')
    p.add_argument('--dtb', type=Path, default=DEVICE / 'out/inputs/piano-boot.dtb')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--kc', type=Path)
    p.add_argument('--trust', type=Path)
    p.add_argument('--seed', type=Path)
    p.add_argument('--adt', type=Path)
    p.add_argument('--utc', type=int, default=int(time.time()))
    a = p.parse_args()
    if sha(MKBOOTIMG) != MKBOOTIMG_SHA256:
        raise SystemExit(f'{MKBOOTIMG} is not the Ubuntu RAM set\'s mkbootimg.py')
    a.out.mkdir(parents=True, exist_ok=True)
    report = {'status': 'OFFLINE_CANDIDATE_NOT_DEVICE_TESTED', 'scope': 'fastboot boot only; never flash',
              'fd': {'file': a.fd.name, 'name': a.fd_name, 'sha256': sha(a.fd)}, 'dtb_sha256': sha(a.dtb),
              'mkbootimg_sha256': MKBOOTIMG_SHA256, 'images': {}}
    if a.kc and a.adt:
        # XNU's RAM window (configs/k27-adt.toml owned_memory_size): an FD whose DarwinBoot lacks firmware/patches/darwinboot/0001
        # refuses an ADT asking for more than 8 GiB, and the image would not boot.
        common = persist_common()
        window, bound = common.adt_ram_window(a.adt), common.fd_ram_window(a.fd)
        if window is not None and window > bound:
            raise SystemExit(f'{a.adt} asks for a {window >> 20} MiB RAM window but {a.fd}\'s DarwinBoot takes at most {bound >> 20} MiB '
                             f'(no {a.fd.name}.ram-window stamp for it: tooling/firmware/build-fd.sh makes the FD with '
                             'firmware/patches/darwinboot/0001); or make the ADT with tooling/adt/make-piano-adt.py --ram-window 0x200000000')
        report['ram_window'] = {'adt_bytes': window, 'fd_bound': bound}
    with tempfile.TemporaryDirectory(prefix='piano-ram-') as scratch:
        work = Path(scratch)
        ramdisk = uefi_payload(work)
        image, info = pack(a.out, a.fd, a.dtb, ramdisk, work, f'boot-piano-uefi-ram-{a.fd_name}')
        report['images'][image.name] = {**info, 'payload_sha256': sha(ramdisk), 'payload_bytes': ramdisk.stat().st_size,
                                        'sha256': sha(image), 'bytes': image.stat().st_size}
        if a.kc:
            ramdisk, extra = macos_payload(work, a.kc, a.trust, a.seed, a.adt, a.utc)
            image, info = pack(a.out, a.fd, a.dtb, ramdisk, work, f'boot-piano-macos-ram-{a.fd_name}')
            report['images'][image.name] = {**info, **extra, 'payload_sha256': sha(ramdisk), 'payload_bytes': ramdisk.stat().st_size,
                                            'kc_sha256': sha(a.kc), 'trust_sha256': sha(a.trust),
                                            # the payload's ADT: the input's, or its copy with the firmware addon's slot
                                            'adt_sha256': extra['firmware'].get('adt_sha256') or sha(a.adt),
                                            'seed_sha256': sha(a.seed), 'sha256': sha(image), 'bytes': image.stat().st_size}
    (a.out / f'build-{a.fd_name}.json').write_text(json.dumps(report, indent=2) + '\n')
    for name, info in report['images'].items():
        print(f'{name}: {info["bytes"]} bytes sha256 {info["sha256"]}')


if __name__ == '__main__':
    main()
