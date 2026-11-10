#!/usr/bin/env python3
"""The new LUN 0 GPT of the persistent boot (docs/persistent-boot.md, plan option A1), from the tables on the tablet now
(CRITICAL-BACKUP/new-primary.bin and new-backup.bin, written at install step 6 on 2026-11-08; sha256 in GPT-HASHES.txt):

  partition 35 linux_rootfs   56060928-71789567  ->  56060928-71527423   (entry index 34: EndingLBA only)
  partition 37 esp (new)      71527424-71789567, type C12A7328-F81F-11D2-BA4B-00A0C93EC93B (EFI system), attributes 0
  entries CRC (header offset 88) and header CRC (offset 16) recomputed in both copies; nothing else changes.

Outputs (out/persistent/): gpt-head.bin (LBA 0-5: protective MBR, header, entries, padding; 24576 bytes), gpt-tail.bin (LBA
124552187-124552191: entries, padding, header; 20480 bytes), gpt.json (every field, GUID and sha256) and gpt-diff.txt (every byte
that differs from the originals, with the field it belongs to). The script refuses to write them if any other byte differs, if the
new entry overlaps or leaves the usable range, or if the result does not parse back to exactly the expected table. The ESP's unique
GUID is derived from the disk GUID (uuid5), so a rebuild gives the same tables; --esp-guid overrides it. Nothing touches a device.

  build-gpt.py [--backup-dir CRITICAL-BACKUP] [--out out/persistent] [--esp-guid GUID]
"""
import argparse
import struct
import sys
import uuid
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

B = common.LBA
ENTRY = 128
HEADER_FIELDS = [('signature', 0, '8s'), ('revision', 8, 'I'), ('header_size', 12, 'I'), ('header_crc32', 16, 'I'),
                 ('reserved', 20, 'I'), ('my_lba', 24, 'Q'), ('alternate_lba', 32, 'Q'), ('first_usable_lba', 40, 'Q'),
                 ('last_usable_lba', 48, 'Q'), ('disk_guid', 56, '16s'), ('entries_lba', 72, 'Q'), ('entry_count', 80, 'I'),
                 ('entry_size', 84, 'I'), ('entries_crc32', 88, 'I')]
ENTRY_FIELDS = [('type_guid', 0, 16), ('unique_guid', 16, 16), ('first_lba', 32, 8), ('last_lba', 40, 8), ('attributes', 48, 8),
                ('name', 56, 72)]


def header(block):
    return {n: struct.unpack_from('<' + f, block, o)[0] for n, o, f in HEADER_FIELDS}


def header_crc(block, size):
    h = bytearray(block[:size])
    h[16:20] = bytes(4)
    return zlib.crc32(h) & 0xffffffff


def entries_of(blob, count):
    out = []
    for i in range(count):
        e = blob[i * ENTRY:(i + 1) * ENTRY]
        if not any(e[:16]):
            continue
        t, u, first, last, attrs = struct.unpack_from('<16s16sQQQ', e, 0)
        out.append({'index': i, 'number': i + 1, 'name': e[56:128].decode('utf-16le').rstrip('\0'),
                    'type': str(uuid.UUID(bytes_le=t)).upper(), 'guid': str(uuid.UUID(bytes_le=u)).upper(),
                    'first_lba': first, 'last_lba': last, 'attributes': attrs})
    return out


def check_table(entries, first_usable, last_usable):
    used = sorted(entries, key=lambda e: e['first_lba'])
    for e in used:
        if not first_usable <= e['first_lba'] <= e['last_lba'] <= last_usable:
            raise SystemExit(f'partition {e["number"]} {e["name"]} leaves the usable range')
    for a, b in zip(used, used[1:]):
        if b['first_lba'] <= a['last_lba']:
            raise SystemExit(f'partitions {a["number"]} {a["name"]} and {b["number"]} {b["name"]} overlap')
    if sum(e['name'] == 'linux_rootfs' for e in entries) != 1:
        raise SystemExit('exactly one linux_rootfs is required (NativeUfs RootLayout.h)')
    if len({e['guid'] for e in entries}) != len(entries):
        raise SystemExit('duplicate unique partition GUIDs')


def field_of(copy, offset, entries_offset, header_offset):
    """Name of the GPT field a byte offset (in gpt-head.bin / gpt-tail.bin) belongs to."""
    if header_offset <= offset < header_offset + B:
        rel = offset - header_offset
        for n, o, f in HEADER_FIELDS:
            if o <= rel < o + struct.calcsize('<' + f):
                return f'{copy} header {n}'
        return f'{copy} header block +{rel}'
    if entries_offset <= offset < entries_offset + 64 * ENTRY:
        rel = offset - entries_offset
        index, inner = divmod(rel, ENTRY)
        for n, o, size in ENTRY_FIELDS:
            if o <= inner < o + size:
                return f'{copy} entry index {index} (partition {index + 1}) {n}'
    return f'{copy} block {offset // B} +{offset % B}'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--backup-dir', type=Path, default=common.BACKUP)
    p.add_argument('--out', type=Path, default=common.DEVICE / 'out/persistent')
    p.add_argument('--esp-guid', type=uuid.UUID)
    a = p.parse_args()
    head0 = (a.backup_dir / 'new-primary.bin').read_bytes()
    tail0 = (a.backup_dir / 'new-backup.bin').read_bytes()
    if common.hashlib.sha256(head0).hexdigest() != common.GPT_PRIMARY_SHA256 or common.hashlib.sha256(tail0).hexdigest() != common.GPT_BACKUP_SHA256:
        raise SystemExit('new-primary.bin / new-backup.bin are not the tables on the tablet (GPT-HASHES.txt)')
    if len(head0) != 6 * B or len(tail0) != 5 * B:
        raise SystemExit('unexpected backup sizes')
    tail_first = common.LUN0_BLOCKS - 5
    h1, h2 = header(head0[B:2 * B]), header(tail0[4 * B:5 * B])
    for h, my, alt, elba in ((h1, 1, common.LUN0_BLOCKS - 1, 2), (h2, common.LUN0_BLOCKS - 1, 1, tail_first)):
        if (h['signature'], h['revision'], h['header_size'], h['my_lba'], h['alternate_lba'], h['entries_lba'], h['entry_count'],
                h['entry_size']) != (b'EFI PART', 0x10000, 92, my, alt, elba, 64, ENTRY):
            raise SystemExit(f'unexpected GPT header geometry: {h}')
    first_usable, last_usable = h1['first_usable_lba'], h1['last_usable_lba']
    entries_blob = head0[2 * B:2 * B + 64 * ENTRY]
    if (tail0[:64 * ENTRY] != entries_blob or zlib.crc32(entries_blob) & 0xffffffff != h1['entries_crc32']
            or h2['entries_crc32'] != h1['entries_crc32']):
        raise SystemExit('primary and backup entry arrays differ, or the entries CRC is wrong')
    if header_crc(head0[B:2 * B], 92) != h1['header_crc32'] or header_crc(tail0[4 * B:], 92) != h2['header_crc32']:
        raise SystemExit('a header CRC of the originals is wrong')
    old = entries_of(entries_blob, 64)
    by_index = {e['index']: e for e in old}
    rootfs, data = by_index.get(common.ROOTFS_INDEX), by_index.get(common.DATA_INDEX)
    if not rootfs or (rootfs['name'], rootfs['first_lba'], rootfs['last_lba']) != ('linux_rootfs', common.ROOTFS_FIRST, common.ROOTFS_LAST):
        raise SystemExit(f'entry index {common.ROOTFS_INDEX} is not linux_rootfs {common.ROOTFS_FIRST}-{common.ROOTFS_LAST}: {rootfs}')
    if not data or (data['name'], data['first_lba']) != ('linux_data', common.DATA_FIRST):
        raise SystemExit(f'entry index {common.DATA_INDEX} is not linux_data at {common.DATA_FIRST}: {data}')
    if common.ESP_INDEX in by_index:
        raise SystemExit(f'entry index {common.ESP_INDEX} is in use: {by_index[common.ESP_INDEX]}')
    disk_guid = uuid.UUID(bytes_le=h1['disk_guid'])
    esp_guid = a.esp_guid or uuid.uuid5(disk_guid, 'piano persistent boot: esp (LUN 0 partition 37)')
    # The new entries.
    blob = bytearray(entries_blob)
    struct.pack_into('<Q', blob, common.ROOTFS_INDEX * ENTRY + 40, common.ROOTFS_NEW_LAST)
    esp = bytearray(ENTRY)
    esp[0:16] = uuid.UUID(common.ESP_TYPE).bytes_le
    esp[16:32] = esp_guid.bytes_le
    struct.pack_into('<QQQ', esp, 32, common.ESP_FIRST, common.ESP_FIRST + common.ESP_BLOCKS - 1, 0)
    name = common.ESP_NAME.encode('utf-16le')
    esp[56:56 + len(name)] = name
    blob[common.ESP_INDEX * ENTRY:(common.ESP_INDEX + 1) * ENTRY] = esp
    blob = bytes(blob)
    new = entries_of(blob, 64)
    check_table(new, first_usable, last_usable)
    ecrc = zlib.crc32(blob) & 0xffffffff

    def rebuild(original, header_offset, entries_offset):
        out = bytearray(original)
        out[entries_offset:entries_offset + 64 * ENTRY] = blob
        struct.pack_into('<I', out, header_offset + 88, ecrc)
        struct.pack_into('<I', out, header_offset + 16, 0)
        struct.pack_into('<I', out, header_offset + 16, header_crc(out[header_offset:header_offset + B], 92))
        return bytes(out)

    head = rebuild(head0, B, 2 * B)
    tail = rebuild(tail0, 4 * B, 0)
    # Allowed differences: entry 34's EndingLBA, entry 36, and the two CRCs per copy.
    allowed = {'entry index 34 (partition 35) last_lba', 'entry index 36 (partition 37) type_guid',
               'entry index 36 (partition 37) unique_guid', 'entry index 36 (partition 37) first_lba',
               'entry index 36 (partition 37) last_lba', 'entry index 36 (partition 37) name',
               'header header_crc32', 'header entries_crc32'}
    lines = []
    for copy, new_bytes, old_bytes, first_lba, header_offset, entries_offset in (
            ('primary', head, head0, 0, B, 2 * B), ('backup', tail, tail0, tail_first, 4 * B, 0)):
        for off in range(len(new_bytes)):
            if new_bytes[off] != old_bytes[off]:
                where = field_of(copy, off, entries_offset, header_offset)
                if where.split(' ', 1)[1] not in allowed:
                    raise SystemExit(f'unexpected difference at {copy} file offset {off}: {where}')
                lines.append(f'{copy:7} file +{off:5d}  LUN0 LBA {first_lba + off // B} +{off % B:4d}  {old_bytes[off]:02x} -> {new_bytes[off]:02x}  {where}')
    # Parse the results back, as a reader would.
    for copy, data_bytes, hoff, eoff, my in (('primary', head, B, 2 * B, 1), ('backup', tail, 4 * B, 0, common.LUN0_BLOCKS - 1)):
        hh = header(data_bytes[hoff:hoff + B])
        if header_crc(data_bytes[hoff:hoff + B], 92) != hh['header_crc32'] or zlib.crc32(data_bytes[eoff:eoff + 64 * ENTRY]) & 0xffffffff != hh['entries_crc32']:
            raise SystemExit(f'{copy}: CRCs do not verify')
        if hh['my_lba'] != my or entries_of(data_bytes[eoff:eoff + 64 * ENTRY], 64) != new:
            raise SystemExit(f'{copy}: does not parse back to the new table')
    if head[:B] != head0[:B]:
        raise SystemExit('the protective MBR changed')
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / 'gpt-head.bin').write_bytes(head)
    (a.out / 'gpt-tail.bin').write_bytes(tail)
    changed = [e for e in new if e not in old] + [e for e in old if e not in new]
    record = {
        'status': 'OFFLINE_BUILD_NOT_DEVICE_TESTED', 'lun': 0, 'lba_bytes': B, 'disk_blocks': common.LUN0_BLOCKS,
        'disk_guid': str(disk_guid).upper(), 'first_usable_lba': first_usable, 'last_usable_lba': last_usable,
        'originals': {'new-primary.bin': common.GPT_PRIMARY_SHA256, 'new-backup.bin': common.GPT_BACKUP_SHA256},
        'gpt-head.bin': {'sha256': common.hashlib.sha256(head).hexdigest(), 'bytes': len(head), 'lba': 0, 'blocks': 6,
                         'dd': 'dd if=gpt-head.bin of=/dev/sda bs=4096 seek=0 conv=fsync'},
        'gpt-tail.bin': {'sha256': common.hashlib.sha256(tail).hexdigest(), 'bytes': len(tail), 'lba': tail_first, 'blocks': 5,
                         'dd': f'dd if=gpt-tail.bin of=/dev/sda bs=4096 seek={tail_first} conv=fsync'},
        'header_crc32': {'primary_old': f'{h1["header_crc32"]:08x}', 'primary_new': f'{header(head[B:2 * B])["header_crc32"]:08x}',
                         'backup_old': f'{h2["header_crc32"]:08x}', 'backup_new': f'{header(tail[4 * B:])["header_crc32"]:08x}'},
        'entries_crc32': {'old': f'{h1["entries_crc32"]:08x}', 'new': f'{ecrc:08x}'},
        'esp_guid': str(esp_guid).upper(),
        'changed_entries': changed,
        'after': [e for e in new if e['index'] >= 33],
        'linux_sysfs_after': {'sda35': {'start': common.ROOTFS_FIRST * 8, 'size': (common.ROOTFS_NEW_LAST - common.ROOTFS_FIRST + 1) * 8},
                              'sda37': {'start': common.ESP_FIRST * 8, 'size': common.ESP_BLOCKS * 8}},
        'differing_bytes': len(lines),
    }
    common.write_json(a.out / 'gpt.json', record)
    report = [f'# LUN 0 GPT: new tables against CRITICAL-BACKUP new-primary.bin ({common.GPT_PRIMARY_SHA256[:16]}) and new-backup.bin '
              f'({common.GPT_BACKUP_SHA256[:16]})',
              f'# gpt-head.bin {record["gpt-head.bin"]["sha256"]}  gpt-tail.bin {record["gpt-tail.bin"]["sha256"]}',
              f'# esp unique GUID {record["esp_guid"]}; entries CRC {record["entries_crc32"]["old"]} -> {record["entries_crc32"]["new"]}',
              f'# {len(lines)} differing bytes; every other byte of both copies is identical (checked)', ''] + lines + ['']
    report += ['# entries before -> after (indexes 33-36)']
    for e in old:
        if e['index'] >= 33:
            report.append(f'before  {e["number"]:2d} {e["name"]:13} {e["first_lba"]:>10}-{e["last_lba"]:<10} {e["type"]} {e["guid"]} attrs {e["attributes"]:#x}')
    for e in new:
        if e['index'] >= 33:
            report.append(f'after   {e["number"]:2d} {e["name"]:13} {e["first_lba"]:>10}-{e["last_lba"]:<10} {e["type"]} {e["guid"]} attrs {e["attributes"]:#x}')
    (a.out / 'gpt-diff.txt').write_text('\n'.join(report) + '\n')
    print('\n'.join(report[:4]))
    print(f'gpt-head.bin {record["gpt-head.bin"]["sha256"]}\ngpt-tail.bin {record["gpt-tail.bin"]["sha256"]}')


if __name__ == '__main__':
    main()
