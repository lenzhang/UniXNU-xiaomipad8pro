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

  build-boot.py [--fd out/fd/piano-uefi-A-bundled.fd] [--dtb out/inputs/piano-boot.dtb] [--out out/persistent]
  build-boot.py --verify [--out out/persistent]     after signing: check the -signed images and assemble macos-set/
"""
import argparse
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
AVBTOOL = Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/v1.1.0/upstream/xiaomipad-8pro-mainline-1.1.0/tools/local/aosp-avb/avbtool.py'
AVBTOOL_SHA256 = 'e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b'
PROFILE = Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/v1.1.0/release/boot/boot-profile.json'
PUBKEY_SHA1 = 'f5151930dc69791637b2a48505cb175f5f78a522'   # the key that signed this tablet's boot_a (13-boot_a)
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what)
    if not ok:
        failures.append(what)


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


def build(a):
    ram = common.ram_images()
    if common.sha(ram.MKBOOTIMG) != ram.MKBOOTIMG_SHA256:
        raise SystemExit(f'{ram.MKBOOTIMG} is not the pinned mkbootimg.py')
    a.out.mkdir(parents=True, exist_ok=True)
    dtb = a.dtb.read_bytes()
    with tempfile.TemporaryDirectory(prefix='piano-boot-') as scratch:
        work = Path(scratch)
        kernel, info = ram.kernel_field(a.fd, a.dtb, work, 'persistent')
        field = kernel.read_bytes()
        shutil.copyfile(kernel, a.out / 'kernel-field.bin')
        v4 = a.out / f'{V4_NAME}.img'
        subprocess.run(['python3', str(ram.MKBOOTIMG), *V4, '--kernel', str(kernel), '--output', str(v4)], check=True)
        zero = work / 'zero.ramdisk'
        zero.write_bytes(bytes(4096))
        v2 = a.out / f'{V2_NAME}.img'
        subprocess.run(['python3', str(ram.MKBOOTIMG), *ram.V2, '--kernel', str(kernel), '--ramdisk', str(zero), '--dtb', str(a.dtb),
                        '--output', str(v2)], check=True)
    print(f'kernel field {len(field)} bytes sha256 {info["kernel_sha256"]} (shim {info["shim_bytes"]} + FD + DTB)')
    h = v4_sections(v4.read_bytes())
    print(f'== {v4.name}')
    check(h['magic'] == b'ANDROID!' and h['header_version'] == 4 and h['header_size'] == 1584, 'boot v4 header, header_size 1584')
    check(h['kernel'] == field and h['ramdisk_size'] == 0 and h['signature_size'] == 0 and h['cmdline'] == b'',
          'kernel = the kernel field, no ramdisk, no boot signature, empty command line')
    check(h['os_version'] == UBUNTU_OS_VERSION, f'os_version/patch level {h["os_version"]:#x} = the installed Ubuntu boot_a\'s')
    check(v4.stat().st_size <= common.BOOT_PARTITION_BYTES - (1 << 20), 'fits boot_a with the signing helper\'s 1 MiB footer margin')
    s = v2_sections(v2.read_bytes())
    ref = v2_sections(Path(a.reference).read_bytes())
    print(f'== {v2.name}')
    check(s['magic'] == b'ANDROID!' and s['header_version'] == 2 and s['page_size'] == 4096, 'boot v2 header, page 4096')
    check((s['kernel_addr'], s['ramdisk_addr'], s['tags_addr']) == (ref['kernel_addr'], ref['ramdisk_addr'], ref['tags_addr']),
          'load addresses = the Ubuntu RAM set\'s (boot-ram-maintenance.img)')
    check(s['kernel'] == field and s['ramdisk'] == bytes(4096) and s['dtb'] == dtb, 'kernel field, 4096 zero bytes of ramdisk, the boot DTB')
    record = {'status': 'OFFLINE_BUILD_NOT_DEVICE_TESTED', 'fd': {'path': str(a.fd.resolve()), 'sha256': common.sha(a.fd)},
              'dtb_sha256': common.sha(a.dtb), 'kernel_field': {**info, 'file': 'kernel-field.bin'},
              'mkbootimg_sha256': ram.MKBOOTIMG_SHA256,
              'images': {p.name: {'bytes': p.stat().st_size, 'sha256': common.sha(p)} for p in (v4, v2)}}
    common.write_json(a.out / 'boot.json', record)
    for name, r in record['images'].items():
        print(f'{name}: {r["bytes"]} bytes sha256 {r["sha256"]}')


def avb(*args):
    return subprocess.run(['python3', str(AVBTOOL), *args], capture_output=True, text=True)


def verify(a):
    import json
    if common.sha(AVBTOOL) != AVBTOOL_SHA256:
        raise SystemExit(f'{AVBTOOL} is not the pinned avbtool 1.3.0')
    profile = json.loads(PROFILE.read_text())['boot']
    pubkey = common.DEVICE / 'out/keys/piano-boot-signing.pub.pem' if not a.pubkey else a.pubkey
    record = json.loads((a.out / 'boot.json').read_text())
    signed = {}
    for name in (V4_NAME, V2_NAME):
        unsigned, image = a.out / f'{name}.img', a.out / f'{name}-signed.img'
        if not image.is_file():
            print(f'== {image.name}: not signed yet (tooling/persist/sign.sh)')
            continue
        data = image.read_bytes()
        print(f'== {image.name}  {len(data)} bytes  sha256 {common.sha(image)}')
        check(len(data) == common.BOOT_PARTITION_BYTES, 'exactly the boot partition (100663296 bytes)')
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
        signed[image.name] = {'bytes': len(data), 'sha256': common.sha(image)}
    record['signed'] = signed
    # macos-set/: piano-switch.py's image set (boot = the signed v4, init_boot and vendor_boot = slot A's current Ubuntu images).
    if f'{V4_NAME}-signed.img' in signed and not failures:
        for set_name, boot in (('macos-set', V4_NAME), ('macos-set-v2', V2_NAME)):
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
    common.write_json(a.out / 'boot.json', record)
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    sys.exit(1 if failures else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--fd', type=Path, default=common.DEVICE / 'out/fd/piano-uefi-A-bundled.fd')
    p.add_argument('--dtb', type=Path, default=common.DEVICE / 'out/inputs/piano-boot.dtb')
    p.add_argument('--reference', type=Path, default=common.DEVICE / 'out/inputs/ubuntu-boot-ram-maintenance.img')
    p.add_argument('--out', type=Path, default=common.DEVICE / 'out/persistent')
    p.add_argument('--verify', action='store_true', help='check the signed images and assemble macos-set/')
    p.add_argument('--pubkey', type=Path)
    a = p.parse_args()
    if a.verify:
        verify(a)
    build(a)
    if failures:
        sys.exit(f'FAIL: {len(failures)} failure(s)')


if __name__ == '__main__':
    main()
