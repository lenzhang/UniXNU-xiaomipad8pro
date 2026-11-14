#!/usr/bin/env python3
"""Offline verification of the piano RAM-boot images (plan 4(b) step 4). Nothing here touches a device.

  verify-ram-images.py BUILD_JSON [--reference UBUNTU_RAM_IMG] [--avbtool AVBTOOL --pubkey KEY.avbpubkey] [IMG ...]

Per image: the v2 header equals the Ubuntu RAM set's (v1.1.0 boot-ram-maintenance.img) in every field but the sizes,
command line and id; the id is recomputed; the kernel field is BootShim (ARM64 header: text offset = FD base, image size
= shim + FD) + the exact FD + the boot DTB; the v2 dtb section is the boot DTB; the ramdisk is a FAT volume whose files
are listed and, for the macOS image, hashed against the inputs, with the boot addon and the ADT's GpuAddon slot present
together or not at all and every addon entry matching its pin in the payload's collection or the recorded sensor pack
(docs/kc-space.md); an AVB footer,
when present, is shown and verified with the public key. Prints a log; exits 1 on any failure.
"""
import argparse
import hashlib
import importlib.util
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
ROOT_27_0_1 = b'C73696D4-CAC4-40A0-90CB-7D0463E4E3E6'   # configs/k27-adt.toml root_match_volume_uuid (retired line)
ROOT_27_3 = b'84581E99-85BE-4957-AC53-2947E887B3E4'     # configs/k273-adt.toml
IMAGE_MAX_BYTES = int(os.environ.get('PIANO_IMAGE_MAX_BYTES', '250000000'))   # build-ram-images.py's
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


def fat_file(ramdisk, name):
    """The bytes of one file of the payload FAT."""
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    with tempfile.TemporaryDirectory() as tmp:
        image = Path(tmp) / 'fat.img'
        image.write_bytes(ramdisk)
        out = Path(tmp) / 'f'
        subprocess.run(['mcopy', '-n', '-o', '-i', str(image), '::/' + name, str(out)], check=True, env=env, capture_output=True)
        return out.read_bytes()


def check_firmware(ramdisk, files, info):
    """The boot addon (docs/kc-space.md, docs/drivers/sensors.md section 5b): present exactly when the build recorded one (firmware
    entries or the sensor pack), then the ADT has the GpuAddon slot and every entry is a pin of the payload's own collection with that
    pin's size and SHA-256, or the recorded sensor pack; absent: no slot. No private hash shown."""
    spec = importlib.util.spec_from_file_location('piano_firmware', Path(__file__).resolve().parents[2] / 'tooling/persist/firmware.py')
    fw = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fw)
    record, sensor = info.get('firmware') or {}, info.get('sensor_pack')
    name = 'EFI/UniXNU/Payloads/kernel.macho.gpuaddon'
    adt = fat_file(ramdisk, 'EFI/UniXNU/Payloads/kernel.macho.adt')
    if not record.get('addon') and not sensor:
        check(name not in files, 'no boot addon (the build recorded none)')
        check(not fw.adt_slot(adt), 'the ADT has no /chosen/memory-map GpuAddon slot (DarwinBoot refuses a slot without an addon)')
        return
    if record.get('addon_sha256'):
        check(files.get(name, (0, None))[1] == record['addon_sha256'], f'{name} is the addon the build recorded')
    check(name in files, f'{name} present')
    check(fw.adt_slot(adt), 'the ADT has the /chosen/memory-map GpuAddon slot DarwinBoot fills with the addon')
    if name not in files:
        return
    pins = {f'{fw.ESP_DIR}/{p["path"]}': p for p in fw.pins_in(fat_file(ramdisk, 'EFI/UniXNU/Payloads/kernel.macho'))}
    try:
        entries = fw.parse_addon(fat_file(ramdisk, name))
    except ValueError as problem:
        check(False, f'the addon parses ({problem})')
        return
    for path, mode, data in entries:
        digest = hashlib.sha256(data).hexdigest()
        if path == 'sensors/piano-sensors.pack':   # PianoSensors' SensorsAddon.h SA_PACK_PATH: this unit's pack (private)
            check(bool(sensor) and digest == sensor.get('sha256'), f'addon {path} ({len(data)} bytes) is the sensor pack the build recorded')
            continue
        pin = pins.get(path)
        ok = pin is not None and len(data) == pin['size'] and digest == pin['sha256']
        shown = 'private' if pin and pin['private'] else digest[:16]
        check(ok, f'addon {path} ({len(data)} bytes, {shown}) matches its pin in the payload\'s collection')
    missing = sorted(set(pins) - {path for path, _, _ in entries})
    if missing:
        print('  info  pinned but not in the addon (the kexts try the ESP): ' + ', '.join(missing))


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
        # build-ram-images.py IMAGE_MAX_BYTES: a larger image stays in ABL (the UEFI's 256 MiB kernel region; 2026-11-13)
        check(len(data) <= IMAGE_MAX_BYTES, f'image size {len(data)} <= {IMAGE_MAX_BYTES} bytes (PIANO_IMAGE_MAX_BYTES)')
        h = header(data)
        kernel, ramdisk, dtb, end = sections(data, h)
        for field in SAME_AS_REFERENCE:
            check(h[field] == ref[field], f'header {field} = {h[field]!r:.60} (Ubuntu RAM set: {ref[field]!r:.60})')
        print(f'  info  kernel_size {h["kernel_size"]}  ramdisk_size {h["ramdisk_size"]}  dtb_size {h["dtb_size"]}  '
              f'cmdline {h["cmdline"].rstrip(bytes(1))!r}')
        check(h['id'][:20] == mkbootimg_id(kernel, ramdisk, dtb), 'header id is the SHA-1 of the sections (mkbootimg)')
        check(kernel[56:60] == b'ARM\x64', 'kernel field starts with an ARM64 image header')
        text_offset, image_size, flags = struct.unpack_from('<QQQ', kernel, 8)
        shim = info.get('shim_bytes') or (len(kernel) - len(fd) - len(dtb_bytes))
        check(text_offset == 0 and flags == 0xa, f'ARM64 header as a Linux Image (text offset {text_offset:#x}, flags {flags:#x}; Ubuntu kernel: 0, 0xa)')
        check(image_size >= shim + len(fd), f'ARM64 header image size {image_size:#x} covers shim {shim:#x} + FD {len(fd):#x}')
        check(kernel[shim:shim + len(fd)] == fd, 'kernel field holds the exact FD after the shim')
        check(kernel[shim + len(fd):] == dtb_bytes, 'the boot DTB is appended after the FD')
        check(dtb == dtb_bytes, 'v2 dtb section is the boot DTB')
        check(hashlib.sha256(ramdisk).hexdigest() == info.get('payload_sha256'), 'ramdisk is the payload the build recorded')
        store = info.get('data_store')
        if store:
            seed, installed = (store.get('seed_identity') or '')[:8], (store.get('installed_seed') or '')[:8]
            print(f'  info  data store: data-persist={store.get("data_persist")}, seed {seed or "?"}, installed seed {installed or "unknown"} '
                  f'({store.get("installed_seed_from", "?")}){" (the same)" if store.get("same_seed") else ""}')
            if store.get('data_persist') == 2 and not store.get('same_seed'):
                print(f'  WARN  data-persist=2 with seed {seed}, not the installed seed {installed or "unknown"}: the store guard keeps '
                      'the owner\'s store only while it is not self-contained (flattened); is the installed seed record current?')
            # 2026-11-14: a warm RAM test seed (tooling/k27/warm-seed.py registers each in out/warm-seeds.json) never opens the store
            registry = Path(__file__).resolve().parents[2] / 'out/warm-seeds.json'
            warm = json.loads(registry.read_text()).get(info.get('seed_sha256', ''), None) if registry.is_file() else None
            if warm:
                print(f'  info  warm seed {warm.get("identity", "")[:16]} (pack {warm.get("pack_sha256", "")[:16]}, '
                      f'{warm.get("pack_bytes")} bytes; {warm.get("out")})')
                check(store.get('data_persist') == 0, f'a warm test seed runs data-persist=0 (this image: {store.get("data_persist")})')
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
        if 'EFI/UniXNU/Payloads/kernel.macho' in files:
            check_firmware(ramdisk, files, info)
        if 'EFI/UniXNU/Payloads/kernel.macho.adt' in files:
            # only the 27.3 line is built and booted (owner, 2026-11-13); a batch-2 probe once carried the 27.0.1 root
            # identity and waited on that root all boot (configs/k27-adt.toml vs configs/k273-adt.toml root_match_volume_uuid)
            adt = fat_file(ramdisk, 'EFI/UniXNU/Payloads/kernel.macho.adt').upper()
            check(ROOT_27_0_1 not in adt, 'the ADT does not name the retired 27.0.1 root (configs/k27-adt.toml)')
            check(ROOT_27_3 in adt, 'the ADT names the 27.3 root (configs/k273-adt.toml root_match_volume_uuid)')
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
