#!/usr/bin/env python3
"""Offline verification of the piano RAM-boot images (plan 4(b) step 4). Nothing here touches a device.

  verify-ram-images.py BUILD_JSON [--reference UBUNTU_RAM_IMG] [--avbtool AVBTOOL --pubkey KEY.avbpubkey] [IMG ...]

Per image: the v2 header equals the Ubuntu RAM set's (v1.1.0 boot-ram-maintenance.img) in every field but the sizes,
command line and id; the id is recomputed; the kernel field is BootShim (ARM64 header: text offset = FD base, image size
= shim + FD) + the exact FD + the boot DTB; the v2 dtb section is the boot DTB; the ramdisk is a FAT volume whose files
are listed and, for the macOS image, hashed against the inputs; an AVB footer, when present, is shown and verified with
the public key. Prints a log; exits 1 on any failure.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HEADER = struct.Struct('<8s10I16s512s32s1024sIQII Q')   # boot_img_hdr_v2
FIELDS = ('magic', 'kernel_size', 'kernel_addr', 'ramdisk_size', 'ramdisk_addr', 'second_size', 'second_addr', 'tags_addr',
          'page_size', 'header_version', 'os_version', 'name', 'cmdline', 'id', 'extra_cmdline', 'recovery_dtbo_size',
          'recovery_dtbo_offset', 'header_size', 'dtb_size', 'dtb_addr')
SAME_AS_REFERENCE = ('magic', 'kernel_addr', 'ramdisk_addr', 'second_size', 'second_addr', 'tags_addr', 'page_size',
                     'header_version', 'os_version', 'name', 'recovery_dtbo_size', 'recovery_dtbo_offset', 'header_size',
                     'dtb_addr')
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what)
    if not ok:
        failures.append(what)


def header(data):
    values = dict(zip(FIELDS, HEADER.unpack_from(data, 0)))
    return values


def sections(data, h):
    page = h['page_size']
    def pages(n):
        return -(-n // page) * page
    offset = page   # header occupies one page (header_size 1660 < 4096)
    kernel = data[offset:offset + h['kernel_size']]; offset += pages(h['kernel_size'])
    ramdisk = data[offset:offset + h['ramdisk_size']]; offset += pages(h['ramdisk_size'])
    offset += pages(h['second_size']) + pages(h['recovery_dtbo_size'])
    dtb = data[offset:offset + h['dtb_size']]; offset += pages(h['dtb_size'])
    return kernel, ramdisk, dtb, offset


def mkbootimg_id(kernel, ramdisk, dtb):
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b'', b'', dtb):   # kernel, ramdisk, second, recovery dtbo, dtb (mkbootimg v2 order)
        sha.update(blob)
        sha.update(struct.pack('<I', len(blob)))
    return sha.digest()


def fat_files(ramdisk):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    with tempfile.TemporaryDirectory() as tmp:
        image = Path(tmp) / 'fat.img'
        image.write_bytes(ramdisk)
        listing = subprocess.run(['mdir', '-/', '-b', '-i', str(image), '::'], capture_output=True, text=True, env=env)
        files = {}
        for line in sorted(listing.stdout.split()):
            if line.endswith('/'):
                continue
            out = Path(tmp) / 'f'
            subprocess.run(['mcopy', '-n', '-o', '-i', str(image), line, str(out)], check=True, env=env, capture_output=True)
            files[line.removeprefix('::').lstrip('/')] = (out.stat().st_size, hashlib.sha256(out.read_bytes()).hexdigest(),
                                                               out.read_text(errors='replace') if line.endswith('.toml') else None)
        return listing.returncode == 0, files


def main():
    p = argparse.ArgumentParser()
    p.add_argument('build_json', type=Path)
    p.add_argument('images', nargs='*', type=Path)
    p.add_argument('--reference', type=Path, default=Path(__file__).resolve().parents[2] / 'out/inputs/ubuntu-boot-ram-maintenance.img')
    p.add_argument('--fd', type=Path)
    p.add_argument('--dtb', type=Path)
    p.add_argument('--avbtool', type=Path)
    p.add_argument('--pubkey', type=Path)
    a = p.parse_args()
    build = json.loads(a.build_json.read_text())
    root = a.build_json.parent
    fd = (a.fd or Path(__file__).resolve().parents[2] / 'out/fd' / build['fd']['file']).read_bytes()
    dtb_bytes = (a.dtb or Path(__file__).resolve().parents[2] / 'out/inputs/piano-boot.dtb').read_bytes()
    ref = header(a.reference.read_bytes())
    print(f'reference container: {a.reference.name} sha256 {hashlib.sha256(a.reference.read_bytes()).hexdigest()}')
    print(f'FD {build["fd"]["file"]} sha256 {hashlib.sha256(fd).hexdigest()} (build record {build["fd"]["sha256"]})')
    check(hashlib.sha256(fd).hexdigest() == build['fd']['sha256'], 'FD file is the one the build used')
    print(f'boot DTB sha256 {hashlib.sha256(dtb_bytes).hexdigest()}')
    images = a.images or [root / name for name in build['images']]
    for path in images:
        data = path.read_bytes()
        info = build['images'].get(path.name.replace('-signed.img', '.img'), {})
        print(f'\n== {path.name}  {len(data)} bytes  sha256 {hashlib.sha256(data).hexdigest()}')
        h = header(data)
        kernel, ramdisk, dtb, end = sections(data, h)
        for field in SAME_AS_REFERENCE:
            check(h[field] == ref[field], f'header {field} = {h[field]!r:.60} (Ubuntu RAM set: {ref[field]!r:.60})')
        print(f'  info  kernel_size {h["kernel_size"]}  ramdisk_size {h["ramdisk_size"]}  dtb_size {h["dtb_size"]}  '
              f'cmdline {h["cmdline"].rstrip(bytes(1))!r}')
        check(h['id'][:20] == mkbootimg_id(kernel, ramdisk, dtb), 'header id is the SHA-1 of the sections (mkbootimg)')
        check(kernel[56:60] == b'ARM\x64', 'kernel field starts with an ARM64 image header')
        text_offset, image_size = struct.unpack_from('<QQ', kernel, 8)
        shim = info.get('shim_bytes') or (len(kernel) - len(fd) - len(dtb_bytes))
        check(text_offset == int(info['fd_base'], 16) if info else False,
              f'ARM64 header text offset {text_offset:#x} = FD base')
        check(image_size == shim + len(fd), f'ARM64 header image size {image_size:#x} = shim {shim:#x} + FD {len(fd):#x}')
        check(kernel[shim:shim + len(fd)] == fd, 'kernel field holds the exact FD after the shim')
        check(kernel[shim + len(fd):] == dtb_bytes, 'the boot DTB is appended after the FD')
        check(dtb == dtb_bytes, 'v2 dtb section is the boot DTB')
        check(hashlib.sha256(ramdisk).hexdigest() == info.get('payload_sha256'), 'ramdisk is the payload the build recorded')
        ok, files = fat_files(ramdisk)
        check(ok and files, f'ramdisk is a FAT volume with {len(files)} files')
        for name, (size, digest, text) in files.items():
            print(f'        {name}  {size}  {digest}')
        want = {'EFI/UniXNU/Payloads/kernel.macho': info.get('kc_sha256'), 'EFI/UniXNU/Payloads/kernel.macho.adt': info.get('adt_sha256'),
                'EFI/UniXNU/Payloads/kernel.macho.trustcache': info.get('trust_sha256'),
                'EFI/UniXNU/Payloads/scratch-data.img': info.get('seed_sha256')}
        for name, digest in want.items():
            if digest:
                check(files.get(name, (0, None))[1] == digest, f'{name} is the input the build recorded')
        config = files.get('EFI/UniXNU/unixnu.toml', (0, None, None))[2]
        check(config is not None, 'EFI/UniXNU/unixnu.toml present')
        if config:
            print('  unixnu.toml:\n' + '\n'.join('    | ' + line[:150] for line in config.splitlines()))
        rest = data[end:]
        if rest.strip(b'\0'):
            if a.avbtool and a.pubkey:
                infotext = subprocess.run(['python3', str(a.avbtool), 'info_image', '--image', str(path)], capture_output=True, text=True)
                print('\n'.join('    ' + line for line in infotext.stdout.splitlines() if line.strip()))
                # verify_image finds the partition's image as <partition_name>.img next to the one it checks.
                with tempfile.TemporaryDirectory() as tmp:
                    (Path(tmp) / 'boot.img').symlink_to(path.resolve())
                    v = subprocess.run(['python3', str(a.avbtool), 'verify_image', '--image', str(Path(tmp) / 'boot.img'),
                                        '--key', str(a.pubkey)], capture_output=True, text=True)
                print('    ' + (v.stdout + v.stderr).strip().replace('\n', '\n    '))
                check(v.returncode == 0, 'avbtool verify_image with the piano public key')
            else:
                print('  info  data after the last section (AVB footer?): pass --avbtool and --pubkey to verify it')
        else:
            print('  info  no AVB footer (unsigned, as the Ubuntu RAM set)')
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
