"""Shared pieces of tooling/persist (docs/persistent-boot.md): the fixed LUN 0 layout of the A1 install, the Data store geometry
(package/NativeUfs, package/NativeRamBlock/PersistOverlay.h), the seed identity NativeRamBlock writes into the store header, a small
independent FAT32 reader for checking the ESP image, and build-ram-images.py loaded as a module (K27_ARGS, kernel_field, mkbootimg).
Nothing here touches a device."""
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[2]
BACKUP = Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/CRITICAL-BACKUP'
PARTITION_BACKUP = Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/piano-backup-20261110'

# LUN 0 (4096-byte LBAs), CRITICAL-BACKUP/gpt-after.json = new-primary.bin / new-backup.bin (the tables on the tablet since
# 2026-11-08). Entry indexes are 0-based (index 34 is partition 35).
LBA = 4096
LUN0_BLOCKS = 124552192
GPT_PRIMARY_SHA256 = '90a9d3bb038bca211aa60a6ea28dc787f8b744c0d213da156a19ab6541cceb99'
GPT_BACKUP_SHA256 = '21b04d671244d063d6eb6d8da618992a820dada5f81e9a1c093eb6e58e4455d3'
ROOTFS_INDEX, ROOTFS_FIRST, ROOTFS_LAST = 34, 56060928, 71789567
DATA_INDEX, DATA_FIRST = 35, 71789568
ESP_INDEX = 36                                   # partition 37
ESP_BLOCKS = 262144                              # 1 GiB
ESP_FIRST = ROOTFS_LAST + 1 - ESP_BLOCKS         # 71527424 = bs=1M seek=279404
ROOTFS_NEW_LAST = ESP_FIRST - 1                  # 71527423
ESP_TYPE = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B'
ESP_NAME = 'esp'
# NativeUfs's reserve at the end of linux_rootfs (log-reserve, default 1024 MiB; unused while log=1 is never set) and the Data store
# just below it: first LBA = partition last + 1 - reserve - W (native_ufs_store_open).
RESERVE_BLOCKS = 1024 * 256
# A copy of the store in the free gap of linux_rootfs, well above the root image and below both store places (plan 3.0).
STORE_SCRATCH_FIRST = 64711424
# The boot triplet on LUN 4 (CRITICAL-BACKUP/boot-partitions.json; backups droidvm-build/piano-backup-20261110/sde/).
BOOT_A_SHA256 = '389d4271743bb4c69664bdb31c34440589978c8add3af0ce9190c46045259a87'      # 13-boot_a (Ubuntu, key f5151930)
VENDOR_BOOT_A_SHA256 = '50f70e361b927945afa4adb5ddc9947e39a136cd2cd2e28a311a4c3d2163829a'  # 23-vendor_boot_a (Ubuntu)
INIT_BOOT_A_SHA256 = 'bbebc3d156fa4bffbdf2fdf83940a71a1ec6d64dcddc60977ece057eb1147061'    # 28-init_boot_a (Ubuntu)
BOOT_PARTITION_BYTES = 100663296
BOOT_A_START_512, BOOT_A_SIZE_512 = 618800, 196608   # /sys/class/block/<boot_a>/{start,size}


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            h.update(chunk)
    return h.hexdigest()


def ram_images():
    """tooling/bootimg/build-ram-images.py as a module (its name has dashes)."""
    spec = importlib.util.spec_from_file_location('build_ram_images', DEVICE / 'tooling/bootimg/build-ram-images.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def store_blocks(pages):
    """PersistOverlay.h windowBlocks: header + presence bitmap + one block per page."""
    return 1 + (pages + 8 * LBA - 1) // (8 * LBA) + pages


def store_first(partition_last, window):
    return partition_last + 1 - RESERVE_BLOCKS - window


def seed_info(path):
    """The packed Data seed (DBRDISK1, SparseDisk.h ramPackedLayout): logical size, pages, the store window W, and the seed identity
    NativeRamBlock records in the store header (SeedDisk.h seedIdentity for a disk above 2 GiB; SHA-256 of the whole disk below)."""
    data = Path(path).read_bytes()
    if data[:8] != b'DBRDISK1' or struct.unpack_from('<II', data, 8) != (1, 4096):
        raise SystemExit(f'{path}: not a DBRDISK1 packed seed')
    size, count = struct.unpack_from('<QQ', data, 16)
    cursor, end, extents = 32 + count * 16, 0, []
    for i in range(count):
        at, length = struct.unpack_from('<QQ', data, 32 + i * 16)
        if not length or (at | length) & 4095 or at < end or at + length > size:
            raise SystemExit(f'{path}: extent {i} is malformed')
        extents.append((at, cursor, length))
        end, cursor = at + length, cursor + length
    if cursor != len(data):
        raise SystemExit(f'{path}: extents do not cover the file')
    zero = bytes(4096)
    h = hashlib.sha256()
    if size > 0x80000000:
        h.update(b'SHNGSEED2' + struct.pack('<Q', size))
        for at, offset, length in extents:
            for j in range(0, length, 4096):
                page = data[offset + j:offset + j + 4096]
                if page != zero:
                    h.update(struct.pack('<Q', (at + j) // 4096) + page)
    else:
        nxt = 0
        for at, offset, length in extents:
            for j in range(0, length, 4096):
                page = data[offset + j:offset + j + 4096]
                index = (at + j) // 4096
                if page == zero:
                    continue
                h.update(zero * (index - nxt))
                h.update(page)
                nxt = index + 1
        h.update(zero * (size // 4096 - nxt))
    pages = size // 4096
    return {'file_sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data), 'logical_bytes': size, 'extents': count,
            'pages': pages, 'store_blocks': store_blocks(pages), 'identity_sha256': h.hexdigest()}


# ---------------------------------------------------------------------------------------------------------------------------
# FAT32 reader (independent of mtools, which wrote the image): BPB checks as EDK2's FatPkg (EnhancedFatDxe/Init.c FatCheckVolume)
# makes them, then every file through its cluster chain.

def fat32_read(image):
    data = Path(image).read_bytes() if not isinstance(image, (bytes, bytearray)) else image
    bs = data[:512]
    (sector, spc, reserved, nfats, root_entries, small, media, fat16_len, _spt, _heads, hidden, big,
     fat_len, ext_flags, fs_version, root_cluster, fsinfo, backup) = struct.unpack_from('<HBHBHHBHHHIIIHHIHH', bs, 11)
    problems = []
    if bs[510:512] != b'\x55\xaa':
        problems.append('boot sector signature')
    if sector & (sector - 1) or not 512 <= sector <= 4096:
        problems.append(f'sector size {sector}')
    if spc & (spc - 1) or not spc:
        problems.append(f'sectors per cluster {spc}')
    if fat16_len or root_entries or fs_version or ext_flags & 0x80 or media < 0xf8:
        problems.append('not a mirrored FAT32 EDK2 mounts (FsVersion 0, ExtendedFlags bit 7 clear, media >= 0xf8)')
    sectors = small or big
    first_data = reserved + nfats * fat_len
    clusters = (sectors - first_data) // spc
    if clusters < 65525:
        problems.append(f'{clusters} clusters: too few for FAT32')
    if fat_len * sector // 4 < clusters + 2:
        problems.append('FAT shorter than the cluster count')
    info = data[fsinfo * sector:fsinfo * sector + 512]
    if info[:4] != b'RRaA' or info[484:488] != b'rrAa' or info[508:512] != b'\x00\x00\x55\xaa':
        problems.append('FSInfo signatures')
    fats = [data[(reserved + i * fat_len) * sector:(reserved + (i + 1) * fat_len) * sector] for i in range(nfats)]
    if any(f != fats[0] for f in fats[1:]):
        problems.append('FAT copies differ')
    fat = fats[0]
    csize = sector * spc

    def chain(first):
        out, c, seen = [], first, set()
        while 2 <= c < 0x0ffffff8:
            if c in seen or c >= clusters + 2:
                problems.append(f'bad cluster chain at {c}')
                break
            seen.add(c)
            out.append(c)
            c = struct.unpack_from('<I', fat, c * 4)[0] & 0x0fffffff
        return out

    def read(first, size=None):
        blob = b''.join(data[(first_data + (c - 2) * spc) * sector:(first_data + (c - 1) * spc) * sector] for c in chain(first))
        return blob if size is None else blob[:size]

    files, label = {}, None

    def walk(cluster, prefix):
        nonlocal label
        raw, lfn = read(cluster), []
        for off in range(0, len(raw), 32):
            e = raw[off:off + 32]
            if e[0] == 0:
                break
            if e[0] == 0xe5:
                lfn = []
                continue
            if e[11] == 0x0f:
                lfn.insert(0, (e[1:11] + e[14:26] + e[28:32]).decode('utf-16le', 'replace'))
                continue
            name = ''.join(lfn).split('\0')[0] if lfn else (e[0:8].decode().rstrip() + ('.' + e[8:11].decode().rstrip() if e[8:11].strip() else ''))
            lfn = []
            attr = e[11]
            if attr & 0x08:
                label = e[0:11].decode().rstrip()
                continue
            if name in ('.', '..'):
                continue
            first = struct.unpack_from('<H', e, 20)[0] << 16 | struct.unpack_from('<H', e, 26)[0]
            size = struct.unpack_from('<I', e, 28)[0]
            if attr & 0x10:
                walk(first, prefix + name + '/')
            else:
                content = read(first, size) if size else b''
                if len(content) != size:
                    problems.append(f'{prefix}{name}: chain holds {len(content)} of {size} bytes')
                files[prefix + name] = content

    walk(root_cluster, '')
    return {'sector_bytes': sector, 'sectors_per_cluster': spc, 'reserved': reserved, 'fats': nfats, 'fat_sectors': fat_len,
            'total_sectors': sectors, 'hidden_sectors': hidden, 'clusters': clusters, 'root_cluster': root_cluster,
            'backup_boot': backup, 'media': media, 'label': label, 'serial': f'{struct.unpack_from("<I", bs, 67)[0]:08x}',
            'boot_label': bs[71:82].decode(errors='replace').rstrip(), 'problems': problems, 'files': files}


def write_json(path, obj):
    Path(path).write_text(json.dumps(obj, indent=2) + '\n')
