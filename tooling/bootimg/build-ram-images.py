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
MKBOOTIMG = Path(os.environ.get('PIANO_MKBOOTIMG', Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/v1.1.0/host-tools/aosp-mkbootimg-prefetched/mkbootimg.py'))
MKBOOTIMG_SHA256 = '37d84b3d162e0bc62e36c1f4e1c63c85ea0caa9f29be023eb2f8efe006ad948c'   # the Ubuntu RAM set's (maintenance-manifest.json)
V2 = ['--header_version', '2', '--pagesize', '4096', '--base', '0', '--kernel_offset', '0x00008000',
      '--ramdisk_offset', '0x01000000', '--tags_offset', '0x00000100', '--dtb_offset', '0x01f00000']
# piano's keys reach UEFI but do not move UniXNU's menu (2026-11-10 22:37), so the macOS image boots its default on its own.
# 5 s: the window in which tooling/native/native-loop.sh reads the previous run's log through the UEFI's debug fastboot.
MENU_TIMEOUT = int(os.environ.get('PIANO_MENU_TIMEOUT', '0'))   # 0: boot the default at once (UniXNU Menu.c); the loop's log dump still fits before ExitBootServices
# 1 CPU (cpus=1) panicked in recount_switch_thread on the tablet (2026-11-10 22:41); the 8-CPU entry is the default.
MENU_DEFAULT = os.environ.get('PIANO_MENU_DEFAULT', 'macos-8cpu')
EPOCH = 1262304000  # UniXNU payload timestamps (2010-01-01)

# The k27 arguments natively (docs/native-droidvm-inventory.md). From the VM's boot disk (logs/adt/vm-unixnu.toml) minus its
# devices (droidvm-display-mm, vgpu-3d, droidvm-vt-paravirt, droidvm-monitor, droidvm-data-persist), sheng-touch-hid, and the ones
# nothing in the native collection reads (2026-11-11: offline-hid-input, offline-hid-home, offline-key-events, sheng-aks,
# sheng-aks-sls, ml-timeout-lock-scale, ml-timeout-lock-panic-scale; ca-triple-buffer, ca-free-wakeup: the VM's Metal plugin).
K27_ARGS = ['-v', 'debug=0x14e', 'kt_fixed_zones=120', '-no_slto_panic', 'tlto_us=0',
            '-apfs_shared_datavolume', 'offline-apfs-load=1', 'owner-staged-apps=1',
            # droidvm-disable-jobs: the jobs without hardware here, but inert natively (its reader was the iPadOS line's kernel,
            # DroidVMLaunchPolicy.c; k27 has none, and the listed jobs run: docs/boot-time.md). PIANO_FAST_BOOT drops it. The clock
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


def persist_common():
    """tooling/persist/common.py (seed identity, installed seed, store guard), loaded by path."""
    spec = importlib.util.spec_from_file_location('persist_common', DEVICE / 'tooling/persist/common.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def loud(text):
    print('\n'.join(['', '*' * 100, *[f'*  {line}' for line in text.splitlines()], '*' * 100, '']), file=sys.stderr, flush=True)


def data_store_args(seed, kc):
    """The Data store arguments of a RAM test image (docs/data-store.md): (arguments, record).
    PIANO_DATA_PERSIST=0|1|2 when set; else 2 only when the image's seed is the installed one (PIANO_INSTALLED_SEED, or
    out/persistent/installed.json), 0 when it is another seed's, and when the installed seed is unknown 2 with a kernel collection that
    has the store guard (another seed's store is then kept and Data runs volatile) or 0 without it. A collection without the guard
    replaces another seed's store: data-persist=2 is refused for it unless the seed is known to be the installed one.
    PIANO_DATA_STORE_REPLACE=1|<8 hex>: data-store-replace (start the installed store over for this seed: drops the owner's Data).
    PIANO_DATA_STORE_FLATTEN=1: data-store-flatten=1 (make this seed's store self-contained; only under the store's own seed)."""
    common = persist_common()
    identity = common.seed_info(seed)['identity_sha256']
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
    # PIANO_EXTRA_ARGS: more kernel arguments for one test image (e.g. piano-usb=2, docs/drivers/usb-device-debug.md).
    extra = os.environ.get('PIANO_EXTRA_ARGS', '').split()
    store = {'data-persist', 'data-store-replace', 'data-store-flatten'} & {x.split('=', 1)[0] for x in extra}
    if store:
        raise SystemExit(f'PIANO_EXTRA_ARGS: {", ".join(sorted(store))}: use PIANO_DATA_PERSIST / PIANO_DATA_STORE_REPLACE / '
                         'PIANO_DATA_STORE_FLATTEN (checked against the installed seed, docs/data-store.md)')
    persist, store_record = data_store_args(seed, kc)
    base = [x for x in fast_boot(K27_ARGS, extra) if not x.startswith('data-persist=')] + persist + [f'droidvm-utc={utc}'] + extra
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
                   'data_store': store_record}


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
                                            'kc_sha256': sha(a.kc), 'adt_sha256': sha(a.adt), 'trust_sha256': sha(a.trust),
                                            'seed_sha256': sha(a.seed), 'sha256': sha(image), 'bytes': image.stat().st_size}
    (a.out / f'build-{a.fd_name}.json').write_text(json.dumps(report, indent=2) + '\n')
    for name, info in report['images'].items():
        print(f'{name}: {info["bytes"]} bytes sha256 {info["sha256"]}')


if __name__ == '__main__':
    main()
