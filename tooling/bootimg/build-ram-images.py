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
"""
import argparse
import hashlib
import json
import os
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
MENU_TIMEOUT = int(os.environ.get('PIANO_MENU_TIMEOUT', '8'))
# 1 CPU (cpus=1) panicked in recount_switch_thread on the tablet (2026-11-10 22:41); the 8-CPU entry is the default.
MENU_DEFAULT = os.environ.get('PIANO_MENU_DEFAULT', 'macos-8cpu')
EPOCH = 1262304000  # UniXNU payload timestamps (2010-01-01)

# The k27 userspace arguments of the working VM boot disk (logs/adt/vm-unixnu.toml), minus the VM's devices
# (droidvm-display-mm, vgpu-3d, droidvm-vt-paravirt, droidvm-monitor, droidvm-data-persist: virtio display, GPU tunnel,
# video, the host monitor and the data store) and minus sheng-touch-hid (no touch driver natively yet).
K27_ARGS = ['-v', 'debug=0x14e', 'kt_fixed_zones=120', '-no_slto_panic', 'ml-timeout-lock-panic-scale=40', 'tlto_us=0',
            '-apfs_shared_datavolume', 'offline-apfs-load=1', 'offline-hid-input=1', 'offline-hid-home=1', 'offline-key-events=1',
            'sheng-aks=1', 'sheng-aks-sls=3', 'owner-staged-apps=1',
            'droidvm-disable-jobs=com.apple.watchdogd,com.apple.identityservicesd,com.apple.locationd,com.apple.CommCenter,com.apple.bluetoothd',
            'mac-amfi-launch-permissive=1', 'union-hide-lower=1', 'malloc-ranges-off=1', 'arm64_maxoffset=0x7FFE000000',
            'ml-timeout-lock-scale=40',
            # 0042: userspace sees 24 MHz over the architected counter (19.2 MHz on SM8750 as in the VM).
            'droidvm-timebase-hz=24000000',
            'ca-triple-buffer=1', 'ca-free-wakeup=1', 'exec-env=com.apple.WebKit.WebContent:GIGACAGE_ENABLED=no',
            'rootauth=0', 'vm_compression_limit=1572864', 'droidvm-tz=Asia/Singapore',
            # Unattended native runs (k27 0055/0056, docs/bringup-debugging.md): the console mirrored into cvp@9cb00000 (retained
            # across resets, unused by Ubuntu and the firmware; Ubuntu's ramoops zone is overwritten by the firmware's demura), a reset after PIANO_RESET_AFTER s (Gunyah watchdog as the backstop), and no panic-spin, so a panic restarts
            # (PSCI) and the tablet always comes back to Ubuntu with the log in /sys/fs/pstore/console-ramoops-0.
            'pstore-console=0x9cb00000', 'pstore-console-size=0x700000', 'piano-reset-after=' + os.environ.get('PIANO_RESET_AFTER', '50')]


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
    base = K27_ARGS + [f'droidvm-utc={utc}']
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
    return image, {'cmdline_1cpu': one, 'cmdline_8cpu': smp, 'payload_mib': size_mib}


def pack(out, fd, dtb, ramdisk, work, name):
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
    image = out / f'{name}.img'
    subprocess.run(['python3', str(MKBOOTIMG), *V2, '--kernel', str(kernel), '--ramdisk', str(ramdisk),
                    '--dtb', str(dtb), '--output', str(image)], check=True)
    return image, {'shim_bytes': len(shim), 'fd_base': hex(base), 'fd_size': hex(size), 'kernel_sha256': sha(kernel),
                   'kernel_bytes': kernel.stat().st_size}


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
