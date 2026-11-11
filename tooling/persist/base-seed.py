#!/usr/bin/env python3
"""The Data store's immutable base (docs/data-store.md section 9). Nothing here touches a device.

The tablet's Data store is a block overlay over one seed, the store's base: its header holds that seed's identity, and NativeRamBlock
resumes the store only with that seed (the store guard keeps it untouched under any other). Every ESP that is to keep the owner's Data
therefore carries the base byte for byte, whatever else it changes (kernel collection, device tree, trust cache, arguments). The
files that used to be staged into the seed and change (the Metal plugin, our userland) are a separate system layer instead, installed
file by file on the running system (tooling/userland/stage.py --live --gpu-bundle) and trusted through the ESP's trust cache, which is
its own payload file: the base's trust cache plus a module of the layer's CDHashes.

    base-seed.py pin --esp ESP.img [--expect HEX|any]      the seed and trust cache of an installed ESP image -> BASE/<identity16>/
    base-seed.py pin --seed SEED --trust TRUST [--expect HEX|any]
    base-seed.py show [--identity HEX]                     the pinned base (default: the installed seed's), checked
    base-seed.py path [seed|trust] [--identity HEX]        one of its files, e.g. PIANO_SEED=$(python3 tooling/persist/base-seed.py path)
    base-seed.py diff OLD NEW                              the pages two seeds differ in, and which APFS structures they are

BASE is PIANO_BASE_DIR (environment or ~/.config/piano/env), else out/base of this checkout. pin reads an ESP image with the
independent FAT32 reader (common.fat32_read), checks the seed's identity against --expect (default: the installed seed,
PIANO_INSTALLED_SEED), and writes scratch-data.img, kernel.macho.trustcache and base.json read only. A base is never replaced:
pinning the same files again is a no-op, other files under the same identity are refused.

diff: why a seed change cannot be carried into an existing store at the block level. The store's disk is the store's page where its
bitmap bit is set, else the seed's. A new seed differs from the base in the pages its staging wrote: APFS writes copy on write, so
new file data and metadata go to pages free in the base, and the container superblock (page 0) and the checkpoint areas are
rewritten in place. Every read-write mount of the store wrote page 0 and checkpoints too. So the store's pages always intersect the
changed ones (a rebase that demands they do not would always refuse), and where the store's pages win the filesystem is the store's,
whose metadata does not reach the new seed's files: they would sit in its free space, unreachable (a rebase that lets the store win
is safe only because it delivers nothing). Files reach an existing Data volume only through the filesystem.
"""
import argparse
import json
import os
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

FORMAT = 1
SEED_NAME, TRUST_NAME = 'scratch-data.img', 'kernel.macho.trustcache'
ESP_PAYLOADS = 'EFI/UniXNU/Payloads/'


def expected_identity(value):
    """--expect: 'any', a hex prefix, or (None) the installed seed. Returns (prefix or None for any, where it came from)."""
    if value == 'any':
        return None, '--expect any'
    if value:
        value = value.lower().removeprefix('0x')
        if not all(c in '0123456789abcdef' for c in value) or not 8 <= len(value) <= 64:
            raise SystemExit(f'--expect {value}: 8 to 64 hex digits of the seed identity, or any')
        return value, '--expect'
    installed, source = common.installed_seed()
    if not installed:
        raise SystemExit(f'the installed seed is unknown ({source}): say --expect HEX (the store\'s seed) or --expect any')
    return installed.lower(), source


def write_readonly(path, data):
    tmp = path.with_name(path.name + '.new')
    tmp.write_bytes(data)
    os.chmod(tmp, 0o444)
    os.replace(tmp, path)


def pin(seed_bytes, trust_bytes, source, expect):
    """Pin SEED_BYTES and TRUST_BYTES as a base. Returns the base directory."""
    want, want_from = expected_identity(expect)
    with tempfile.TemporaryDirectory(prefix='piano-base-') as tmp:
        seed_tmp, trust_tmp = Path(tmp) / SEED_NAME, Path(tmp) / TRUST_NAME
        seed_tmp.write_bytes(seed_bytes)
        trust_tmp.write_bytes(trust_bytes)
        info = common.seed_info(seed_tmp)
        cdhashes = common.trust_hashes(trust_tmp)
    identity = info['identity_sha256']
    if want and not identity.startswith(want):
        raise SystemExit(f'this seed\'s identity is {identity[:16]}, not {want[:16]} ({want_from}): not the store\'s base; nothing pinned')
    common.check_store_size(info)
    trust_sha = common.hashlib.sha256(trust_bytes).hexdigest()
    record = {'format': FORMAT, 'identity_sha256': identity,
              'about': 'the Data store\'s base (docs/data-store.md section 9): every ESP that keeps this store carries this seed byte for byte',
              'seed': info, 'trust': {'sha256': trust_sha, 'bytes': len(trust_bytes), 'cdhashes': len(cdhashes),
                                       'modules': struct.unpack_from('<I', trust_bytes)[0]},
              'source': source}
    d = common.base_dir() / identity[:16]
    if (d / 'base.json').is_file():
        old = json.loads((d / 'base.json').read_text())
        if (old['seed']['file_sha256'], old['trust']['sha256']) != (info['file_sha256'], trust_sha):
            raise SystemExit(f'{d}: base {identity[:16]} is pinned already with other files (seed {old["seed"]["file_sha256"][:16]}, trust '
                             f'{old["trust"]["sha256"][:16]}); a base is never replaced')
        common.pinned_base(identity)   # its files are still the pinned ones
        print(f'{d}: base {identity[:16]} pinned already (the same files)')
        return d
    d.mkdir(parents=True, exist_ok=True)
    write_readonly(d / SEED_NAME, seed_bytes)
    write_readonly(d / TRUST_NAME, trust_bytes)
    (d / 'base.json').write_text(json.dumps(record, indent=1) + '\n')
    os.chmod(d / 'base.json', 0o444)
    common.pinned_base(identity)
    print(f'{d}: base {identity[:16]} pinned: seed {info["file_sha256"][:16]} ({info["bytes"]} bytes, {info["extents"]} extents), '
          f'trust cache {trust_sha[:16]} ({len(cdhashes)} CDHashes in {record["trust"]["modules"]} modules); from {source["kind"]} '
          f'{source["name"]}')
    return d


def pin_esp(image, expect):
    fat = common.fat32_read(image)
    if fat['problems']:
        raise SystemExit(f'{image}: ' + '; '.join(fat['problems']))
    files = fat['files']
    for name in (SEED_NAME, TRUST_NAME):
        if ESP_PAYLOADS + name not in files:
            raise SystemExit(f'{image}: no {ESP_PAYLOADS}{name}')
    source = {'kind': 'ESP image', 'name': Path(image).name, 'sha256': common.sha(image),
              'unixnu_toml_sha256': common.hashlib.sha256(files.get('EFI/UniXNU/unixnu.toml', b'')).hexdigest()}
    return pin(files[ESP_PAYLOADS + SEED_NAME], files[ESP_PAYLOADS + TRUST_NAME], source, expect)


def pin_files(seed, trust, expect):
    source = {'kind': 'files', 'name': f'{Path(seed).name} + {Path(trust).name}'}
    return pin(Path(seed).read_bytes(), Path(trust).read_bytes(), source, expect)


# ---- diff --------------------------------------------------------------------------------------------------------------------

def pages(path):
    """{page index: 4096 bytes} of the nonzero pages of a DBRDISK1 seed, and its page count."""
    data = Path(path).read_bytes()
    info = common.seed_info(path)
    count, = struct.unpack_from('<Q', data, 24)
    cursor, out, zero = 32 + count * 16, {}, bytes(4096)
    for i in range(count):
        at, length = struct.unpack_from('<QQ', data, 32 + i * 16)
        for j in range(0, length, 4096):
            page = data[cursor + j:cursor + j + 4096]
            if page != zero:
                out[(at + j) // 4096] = page
        cursor += length
    return out, info


def nx_layout(page0):
    """APFS's container superblock (nx_superblock_t, Apple's APFS reference): the checkpoint areas and the next transaction id, or None."""
    if len(page0) < 136 or page0[32:36] != b'NXSB':
        return None
    desc_blocks, data_blocks, desc_base, data_base = struct.unpack_from('<IIQQ', page0, 104)
    next_xid, = struct.unpack_from('<Q', page0, 96)
    return {'next_xid': next_xid, 'desc': (desc_base, desc_blocks & 0x7fffffff), 'data': (data_base, data_blocks & 0x7fffffff),
            'desc_contiguous': not desc_blocks & 0x80000000, 'data_contiguous': not data_blocks & 0x80000000}


def diff(old_path, new_path):
    old, old_info = pages(old_path)
    new, new_info = pages(new_path)
    if old_info['pages'] != new_info['pages']:
        raise SystemExit(f'the seeds have {old_info["pages"]} and {new_info["pages"]} pages: another size is another store window')
    zero = bytes(4096)
    changed = sorted(i for i in set(old) | set(new) if old.get(i, zero) != new.get(i, zero))
    nx = nx_layout(old.get(0, zero))
    nx_new = nx_layout(new.get(0, zero))
    where = {'superblock (page 0)': 0, 'checkpoint descriptor area': 0, 'checkpoint data area': 0,
             'pages zero in OLD (copy-on-write targets)': 0, 'other pages': 0}
    for i in changed:
        if i == 0:
            where['superblock (page 0)'] += 1
        elif nx and nx['desc_contiguous'] and nx['desc'][0] <= i < sum(nx['desc']):
            where['checkpoint descriptor area'] += 1
        elif nx and nx['data_contiguous'] and nx['data'][0] <= i < sum(nx['data']):
            where['checkpoint data area'] += 1
        elif i not in old:
            where['pages zero in OLD (copy-on-write targets)'] += 1
        else:
            where['other pages'] += 1
    report = {'old': {'identity': old_info['identity_sha256'], 'nonzero_pages': len(old), 'apfs': nx},
              'new': {'identity': new_info['identity_sha256'], 'nonzero_pages': len(new), 'apfs': nx_new},
              'changed_pages': len(changed), 'where': where,
              'first': changed[:8], 'last': changed[-8:]}
    print(f'OLD {old_info["identity_sha256"][:16]} ({len(old)} nonzero pages), NEW {new_info["identity_sha256"][:16]} ({len(new)}): '
          f'{len(changed)} pages differ ({len(changed) * 4} KiB)')
    if nx and nx_new:
        print(f'APFS: next transaction {nx["next_xid"]} -> {nx_new["next_xid"]}; checkpoint descriptors {nx["desc"][1]} blocks at '
              f'{nx["desc"][0]}, data {nx["data"][1]} blocks at {nx["data"][0]}')
    for k, v in where.items():
        print(f'  {k}: {v}')
    if changed:
        print('A store over OLD has written page 0 and checkpoints at every read-write mount: its pages intersect these, so the '
              'block-level rebase that needs them disjoint refuses; letting the store\'s pages win shows the store\'s filesystem, '
              'which cannot reach NEW\'s files. NEW\'s files reach that Data volume only as files (docs/data-store.md section 9).')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    s = sub.add_parser('pin')
    s.add_argument('--esp', type=Path)
    s.add_argument('--seed', type=Path)
    s.add_argument('--trust', type=Path)
    s.add_argument('--expect', metavar='HEX|any')
    s = sub.add_parser('show')
    s.add_argument('--identity')
    s.add_argument('--json', action='store_true')
    s = sub.add_parser('path')
    s.add_argument('which', nargs='?', choices=('seed', 'trust'), default='seed')
    s.add_argument('--identity')
    s = sub.add_parser('diff')
    s.add_argument('old', type=Path)
    s.add_argument('new', type=Path)
    s.add_argument('--json', action='store_true')
    a = p.parse_args()
    if a.command == 'pin':
        if bool(a.esp) == bool(a.seed or a.trust) or bool(a.seed) != bool(a.trust):
            raise SystemExit('pin --esp ESP.img, or pin --seed SEED --trust TRUST')
        pin_esp(a.esp, a.expect) if a.esp else pin_files(a.seed, a.trust, a.expect)
    elif a.command == 'show':
        base = common.pinned_base(a.identity)
        if a.json:
            print(json.dumps(base['record'], indent=1))
        else:
            r = base['record']
            print(f'{base["dir"]}: base {r["identity_sha256"][:16]} (checked): seed {r["seed"]["file_sha256"][:16]}, trust cache '
                  f'{r["trust"]["sha256"][:16]} ({r["trust"]["cdhashes"]} CDHashes); from {r["source"]["kind"]} {r["source"]["name"]}')
    elif a.command == 'path':
        print(common.pinned_base(a.identity)[a.which])
    else:
        report = diff(a.old, a.new)
        if a.json:
            print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
