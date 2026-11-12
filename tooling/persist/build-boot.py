#!/usr/bin/env python3
"""The boot_a images of the persistent boot (docs/persistent-boot.md, plan option A1): BootShim + FD only, the payload is on the
UFS ESP. Nothing here touches a device; signing is tooling/persist/sign.sh (the key stays on the signing host).

  boot-piano-macos-persistent-v4.img  the image for boot_a: Android boot v4, page 4096, os_version 15.0.0, patch level 2026-02
                                      (the installed Ubuntu boot_a's), the kernel field only (no ramdisk, empty command line).
                                      ABL takes vendor_boot_a (Ubuntu's DTB = out/inputs/piano-boot.dtb, its dracut ramdisk) and
                                      init_boot_a from the partitions, as for Ubuntu; neither is written.
  boot-piano-esp-test-v2.img          the RAM test of the same boot path: the v2 container of the RAM images with the same kernel
                                      field and DTB and a ramdisk of 4096 zero bytes (no FAT, no config), so UniXNU finds the ESP.
                                      Signed, it is the fallback for boot_a if ABL refuses the v4 container.
The kernel field is build-ram-images.py's (kernel_field: UniXNU's BootShim with a Linux Image header + FD + the boot DTB).

Series (--series N, default 4) number the boot_a images by the FD they carry, not by their container (always boot v4). 4 is the
image installed on 2026-11-11 (record boot.json, both images above). From 5: only boot-piano-macos-persistent-vN.img (ABL took
the v4 container in session 2 and boots it since), record boot-vN.json and kernel-field-vN.bin beside the older ones, so one
install directory keeps every series' record (maint-session.py reads boot.json and every boot-v*.json). From 5 the FD must carry
both stamps of tooling/firmware/build-fd.sh, and the record keeps its DarwinBoot window bound (fd.ram_window), with which
maint-session.py keeps the pair on the tablet bootable: an ESP whose ADT asks for more than boot_a's DarwinBoot takes never boots.
Series 5 = the FD with firmware/patches/darwinboot/0001 (XNU's RAM window up to 13 GiB, docs/persistent-boot.md section 12).

  build-boot.py [--series N] [--fd out/fd/piano-uefi-A-bundled.fd] [--dtb out/inputs/piano-boot.dtb] [--out out/persistent]
                [--same-kernel-as RECORD]   RECORD: a build-ram-images.py record (build-<fd>.json) whose RAM images ran on the
                                            tablet: the kernel field must be theirs byte for byte (the same shim, FD and DTB)
  build-boot.py --verify [--series N] [--out out/persistent]     after signing: check the -signed images, assemble macos-set*/
"""
import os
import argparse
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

V4_NAME, V2_NAME = 'boot-piano-macos-persistent-v4', 'boot-piano-esp-test-v2'
V4 = ['--header_version', '4', '--pagesize', '4096', '--os_version', '15.0.0', '--os_patch_level', '2026-02', '--cmdline', '']
UBUNTU_OS_VERSION = 0x1e0001a2           # 15.0.0, 2026-02 (13-boot_a header)
V110 = Path(os.path.expanduser(os.environ.get('PIANO_V110', str(Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/v1.1.0'))))   # the Ubuntu v1.1.0 release (or its few files)
AVBTOOL = V110 / 'upstream/xiaomipad-8pro-mainline-1.1.0/tools/local/aosp-avb/avbtool.py'
AVBTOOL_SHA256 = 'e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b'
PROFILE = V110 / 'release/boot/boot-profile.json'
PUBKEY_SHA1 = 'f5151930dc69791637b2a48505cb175f5f78a522'   # the key that signed this tablet's boot_a (13-boot_a)
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what)
    if not ok:
        failures.append(what)


def series(n):
    """Series N's names: boot_a image, v2 test twin (None from 5), record, kernel field file, {image: macos-set directory}."""
    if n == 4:
        return V4_NAME, V2_NAME, 'boot.json', 'kernel-field.bin', {V4_NAME: 'macos-set', V2_NAME: 'macos-set-v2'}
    name = f'boot-piano-macos-persistent-v{n}'
    return name, None, f'boot-v{n}.json', f'kernel-field-v{n}.bin', {name: f'macos-set-v{n}'}


def v4_sections(data):
    magic, kernel, ramdisk, os_version, header = struct.unpack_from('<8sIIII', data, 0)
    version = struct.unpack_from('<I', data, 40)[0]
    cmdline = data[44:44 + 1536].rstrip(b'\0')
    signature = struct.unpack_from('<I', data, 44 + 1536)[0]
    return {'magic': magic, 'kernel_size': kernel, 'ramdisk_size': ramdisk, 'os_version': os_version, 'header_size': header,
            'header_version': version, 'cmdline': cmdline, 'signature_size': signature, 'kernel': data[4096:4096 + kernel]}


def v2_sections(data):
    f = struct.unpack_from('<8s10I', data, 0)
    page = f[8]
    pages = lambda n: -(-n // page) * page  # noqa: E731
    kernel_size, ramdisk_size = f[1], f[3]
    dtb_size = struct.unpack_from('<I', data, 1648)[0]   # boot_img_hdr_v2: after recovery_dtbo_{size,offset} and header_size
    o = page   # second stage and recovery DTBO are empty
    kernel = data[o:o + kernel_size]; o += pages(kernel_size)
    ramdisk = data[o:o + ramdisk_size]; o += pages(ramdisk_size)
    dtb = data[o:o + dtb_size]
    return {'magic': f[0], 'kernel_addr': f[2], 'ramdisk_addr': f[4], 'tags_addr': f[7], 'page_size': page, 'header_version': f[9],
            'kernel': kernel, 'ramdisk': ramdisk, 'dtb': dtb}


def fd_stamps(fd):
    """Both stamps of tooling/firmware/build-fd.sh for this FD's SHA-256 (series 5 on), else SystemExit."""
    digest, stamps = common.sha(fd), {}
    for stamp in ('ram-payload-first', 'ram-window'):
        path = Path(str(fd) + '.' + stamp)
        fields = path.read_text().split() if path.is_file() else []
        if not fields or fields[0] != digest:
            raise SystemExit(f'{fd}: no {path.name} stamp for this FD (sha256 {digest[:16]}): tooling/firmware/build-fd.sh builds the FD '
                             'with firmware/patches/unixnu/0001 and darwinboot/0001 and stamps it')
        stamps[stamp] = fields
    return stamps


def build(a):
    name, twin, record_name, field_name, _ = series(a.series)
    stamps = fd_stamps(a.fd) if a.series >= 5 else {}
    ram = common.ram_images()
    if common.sha(ram.MKBOOTIMG) != ram.MKBOOTIMG_SHA256:
        raise SystemExit(f'{ram.MKBOOTIMG} is not the pinned mkbootimg.py')
    a.out.mkdir(parents=True, exist_ok=True)
    dtb = a.dtb.read_bytes()
    v2 = None
    with tempfile.TemporaryDirectory(prefix='piano-boot-') as scratch:
        work = Path(scratch)
        kernel, info = ram.kernel_field(a.fd, a.dtb, work, 'persistent')
        field = kernel.read_bytes()
        shutil.copyfile(kernel, a.out / field_name)
        v4 = a.out / f'{name}.img'
        subprocess.run(['python3', str(ram.MKBOOTIMG), *V4, '--kernel', str(kernel), '--output', str(v4)], check=True)
        if twin:
            zero = work / 'zero.ramdisk'
            zero.write_bytes(bytes(4096))
            v2 = a.out / f'{twin}.img'
            subprocess.run(['python3', str(ram.MKBOOTIMG), *ram.V2, '--kernel', str(kernel), '--ramdisk', str(zero), '--dtb', str(a.dtb),
                            '--output', str(v2)], check=True)
    print(f'kernel field {len(field)} bytes sha256 {info["kernel_sha256"]} (shim {info["shim_bytes"]} + FD + DTB)')
    same = None
    if a.same_kernel_as:
        ref = json.loads(a.same_kernel_as.read_text())
        kernels = {r.get('kernel_sha256') for r in ref.get('images', {}).values() if isinstance(r, dict)} - {None}
        check(ref.get('fd', {}).get('sha256') == common.sha(a.fd), f'{a.same_kernel_as.name}: its RAM images carry this FD')
        check(kernels == {info['kernel_sha256']}, f'kernel field = the one of the RAM images of {a.same_kernel_as.parent.name}/'
              f'{a.same_kernel_as.name} ({", ".join(k[:16] for k in sorted(kernels))})')
        same = {'record': str(a.same_kernel_as.resolve()), 'kernel_sha256': sorted(kernels)}
    h = v4_sections(v4.read_bytes())
    print(f'== {v4.name}')
    check(h['magic'] == b'ANDROID!' and h['header_version'] == 4 and h['header_size'] == 1584, 'boot v4 header, header_size 1584')
    check(h['kernel'] == field and h['ramdisk_size'] == 0 and h['signature_size'] == 0 and h['cmdline'] == b'',
          'kernel = the kernel field, no ramdisk, no boot signature, empty command line')
    check(h['os_version'] == UBUNTU_OS_VERSION, f'os_version/patch level {h["os_version"]:#x} = the installed Ubuntu boot_a\'s')
    check(v4.stat().st_size <= common.BOOT_PARTITION_BYTES - (1 << 20), 'fits boot_a with the signing helper\'s 1 MiB footer margin')
    if v2:
        s = v2_sections(v2.read_bytes())
        ref = v2_sections(Path(a.reference).read_bytes())
        print(f'== {v2.name}')
        check(s['magic'] == b'ANDROID!' and s['header_version'] == 2 and s['page_size'] == 4096, 'boot v2 header, page 4096')
        check((s['kernel_addr'], s['ramdisk_addr'], s['tags_addr']) == (ref['kernel_addr'], ref['ramdisk_addr'], ref['tags_addr']),
              'load addresses = the Ubuntu RAM set\'s (boot-ram-maintenance.img)')
        check(s['kernel'] == field and s['ramdisk'] == bytes(4096) and s['dtb'] == dtb, 'kernel field, 4096 zero bytes of ramdisk, the boot DTB')
    fd = {'path': str(a.fd.resolve()), 'sha256': common.sha(a.fd)}
    if stamps:
        fd.update(ram_payload_first=True, ram_window=int(stamps['ram-window'][1], 0))
        print(f'FD {fd["sha256"]}: RAM payload first, DarwinBoot window bound {fd["ram_window"]:#x} ({fd["ram_window"] >> 20} MiB)')
    record = {'status': 'OFFLINE_BUILD_NOT_DEVICE_TESTED', 'series': a.series, 'fd': fd,
              'dtb_sha256': common.sha(a.dtb), 'kernel_field': {**info, 'file': field_name, 'same_as': same},
              'mkbootimg_sha256': ram.MKBOOTIMG_SHA256,
              'images': {p.name: {'bytes': p.stat().st_size, 'sha256': common.sha(p)} for p in (v4, v2) if p}}
    common.write_json(a.out / record_name, record)
    for image, r in record['images'].items():
        print(f'{image}: {r["bytes"]} bytes sha256 {r["sha256"]}')


def avb(*args):
    return subprocess.run(['python3', str(AVBTOOL), *args], capture_output=True, text=True)


def verify(a):
    if common.sha(AVBTOOL) != AVBTOOL_SHA256:
        raise SystemExit(f'{AVBTOOL} is not the pinned avbtool 1.3.0')
    profile = json.loads(PROFILE.read_text())['boot']
    pubkey = common.DEVICE / 'out/keys/piano-boot-signing.pub.pem' if not a.pubkey else a.pubkey
    main_name, twin, record_name, _, sets = series(a.series)
    record = json.loads((a.out / record_name).read_text())
    signed = {}
    for name in [n for n in (main_name, twin) if n]:
        unsigned, image = a.out / f'{name}.img', a.out / f'{name}-signed.img'
        if not image.is_file():
            print(f'== {image.name}: not signed yet (tooling/persist/sign.sh)')
            continue
        data = image.read_bytes()
        before = len(failures)
        print(f'== {image.name}  {len(data)} bytes  sha256 {common.sha(image)}')
        check(len(data) == common.BOOT_PARTITION_BYTES, 'exactly the boot partition (100663296 bytes)')
        check(common.sha(unsigned) == record['images'][unsigned.name]['sha256'], f'{unsigned.name} is the one {record_name} recorded')
        check(data.startswith(unsigned.read_bytes()), f'starts with {unsigned.name} byte for byte')
        info = avb('info_image', '--image', str(image))
        text = info.stdout
        print('\n'.join('    ' + line for line in text.splitlines() if line.strip()))
        check(info.returncode == 0 and 'Partition Name:        boot' in text, 'AVB hash descriptor for partition "boot"')
        check('Algorithm:                SHA256_RSA4096' in text, 'SHA256_RSA4096')
        check(f'Rollback Index:           {profile["rollback_index"]}' in text, f'rollback index {profile["rollback_index"]}')
        for key, value in profile['avb_properties'].items():
            check(f"Prop: {key} -> '{value}'" in text, f'property {key}')
        check(f'Public key (sha1):        {PUBKEY_SHA1}' in text, f'public key sha1 {PUBKEY_SHA1} (the installed boot_a\'s)')
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'boot.img').symlink_to(image.resolve())
            v = avb('verify_image', '--image', str(Path(tmp) / 'boot.img'), '--key', str(pubkey))
        print('    ' + (v.stdout + v.stderr).strip().replace('\n', '\n    '))
        check(v.returncode == 0, f'avbtool verify_image with {pubkey.name}')
        # Only an image that passed every check is recorded as signed: maint-session.py's boot-a writes what "signed" names.
        if len(failures) == before:
            signed[image.name] = {'bytes': len(data), 'sha256': common.sha(image)}
    record['signed'] = signed
    # macos-set*/: piano-switch.py's image set (boot = the signed image, init_boot and vendor_boot = slot A's current Ubuntu images).
    if f'{main_name}-signed.img' in signed and not failures:
        for boot, set_name in sets.items():
            if f'{boot}-signed.img' not in signed:
                continue
            d = a.out / set_name
            d.mkdir(exist_ok=True)
            shutil.copyfile(a.out / f'{boot}-signed.img', d / 'boot.img')
            for part, backup, digest in (('init_boot', '28-init_boot_a', common.INIT_BOOT_A_SHA256),
                                         ('vendor_boot', '23-vendor_boot_a', common.VENDOR_BOOT_A_SHA256)):
                source = a.out / 'ref' / f'{backup}.img'
                if not source.is_file():
                    subprocess.run(['zstd', '-q', '-d', '-f', '-o', str(source), str(common.partition_backup() / 'sde' / f'{backup}.img.zst')], check=True)
                check(common.sha(source) == digest, f'{set_name}/{part}.img is slot A\'s current {part} ({digest[:16]})')
                shutil.copyfile(source, d / f'{part}.img')
            (d / 'SHA256SUMS').write_text(''.join(f'{common.sha(d / n)}  {n}\n' for n in ('boot.img', 'init_boot.img', 'vendor_boot.img')))
            record[set_name] = (d / 'SHA256SUMS').read_text()
            print(f'{set_name}/:\n' + record[set_name])
    common.write_json(a.out / record_name, record)
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    sys.exit(1 if failures else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--fd', type=Path, default=common.DEVICE / 'out/fd/piano-uefi-A-bundled.fd')
    p.add_argument('--dtb', type=Path, default=common.DEVICE / 'out/inputs/piano-boot.dtb')
    p.add_argument('--reference', type=Path, default=common.DEVICE / 'out/inputs/ubuntu-boot-ram-maintenance.img')
    p.add_argument('--out', type=Path, default=common.DEVICE / 'out/persistent')
    p.add_argument('--series', type=int, default=4, help='4: the image installed on 2026-11-11 (boot.json); 5 and later: '
                   'boot-piano-macos-persistent-vN.img and boot-vN.json from an FD with both build-fd.sh stamps')
    p.add_argument('--same-kernel-as', type=Path, help='a build-ram-images.py record whose RAM images ran on the tablet')
    p.add_argument('--verify', action='store_true', help='check the signed images and assemble macos-set*/')
    p.add_argument('--pubkey', type=Path)
    a = p.parse_args()
    if a.series < 4:
        p.error('--series starts at 4')
    if a.verify:
        verify(a)
    build(a)
    if failures:
        sys.exit(f'FAIL: {len(failures)} failure(s)')


if __name__ == '__main__':
    main()
