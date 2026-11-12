#!/usr/bin/env python3
"""The members of the 27.3 line's IPSW, fetched one by one with resumable HTTP range requests.

The UniversalMac restore IPSW of the UniXNU macOS 27.3 profile is 27 GB; the line needs only the K114c
build identity's members (kernelcache, device tree, trust caches, the OS and cryptex disk images). This
reads the zip's central directory and BuildManifest.plist over HTTP ranges, then downloads each member's
stored bytes with curl into DIR/<member>.partial, appending on every retry (an interrupted run resumes
where it stopped), checks the size and the zip CRC-32, and records the sha256 in DIR/<member>.json.
Deflated members (small plists) are inflated after the download.

  ipsw-members.py list  [--url URL] [--identity k114cap]
  ipsw-members.py fetch [--url URL] --out DIR COMPONENT...      (BuildManifest component names, or 'all')

Nothing fetched here is committed: Apple payloads stay under out/.
"""
import argparse
import concurrent.futures
import hashlib
import json
import plistlib
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import zlib
from pathlib import Path

URL = ('https://updates.cdn-apple.com/2026FallSeed/53df7654-4865-4259-8dae-30177b76f104/'
       'UniversalMac_27.3_26C5035r_Restore.ipsw')
STREAMS = 4
# The components the line uses (UniXNU profile [ipsw].fetch + the userdata windows).
DEFAULT = ['KernelCache', 'DeviceTree', 'StaticTrustCache', 'Cryptex1,SystemTrustCache', 'Cryptex1,AppTrustCache',
           'SystemVolume', 'Cryptex1,SystemVolume', 'Cryptex1,AppVolume', 'OS', 'Cryptex1,SystemOS', 'Cryptex1,AppOS']


def get_range(url, start, end):
    for attempt in range(6):
        try:
            request = urllib.request.Request(url, headers={'Range': f'bytes={start}-{end}'})
            with urllib.request.urlopen(request, timeout=60) as response:
                data = response.read()
            if len(data) != end - start + 1:
                raise OSError('short range response')
            return data
        except OSError as error:
            if attempt == 5:
                raise
            print(f'range {start}-{end}: {error}; retry', file=sys.stderr)
            time.sleep(2 + 3 * attempt)


def size_of(url):
    request = urllib.request.Request(url, method='HEAD')
    with urllib.request.urlopen(request, timeout=60) as response:
        return int(response.headers['Content-Length']), response.headers.get('x-amz-meta-digest-sha256')


class Zip:
    def __init__(self, url):
        self.url = url
        self.size, self.server_sha256 = size_of(url)
        tail = get_range(url, max(0, self.size - (1 << 16)), self.size - 1)
        at = tail.rfind(b'PK\x05\x06')
        if at < 0:
            raise SystemExit('no end of central directory')
        count, cd_size, cd_offset = struct.unpack_from('<HII', tail, at + 10)
        if cd_offset == 0xffffffff or count == 0xffff:
            loc = tail.rfind(b'PK\x06\x07')
            (eocd64,) = struct.unpack_from('<Q', tail, loc + 8)
            record = get_range(url, eocd64, eocd64 + 55)
            count, cd_size, cd_offset = struct.unpack_from('<QQQ', record, 32)
        directory = get_range(url, cd_offset, cd_offset + cd_size - 1)
        self.entries = {}
        at = 0
        while at < len(directory):
            if directory[at:at + 4] != b'PK\x01\x02':
                raise SystemExit('bad central directory')
            method, crc, csize, usize = struct.unpack_from('<H4xIII', directory, at + 10)
            nlen, elen, clen = struct.unpack_from('<HHH', directory, at + 28)
            (offset,) = struct.unpack_from('<I', directory, at + 42)
            name = directory[at + 46:at + 46 + nlen].decode()
            extra = directory[at + 46 + nlen:at + 46 + nlen + elen]
            e = 0
            while e + 4 <= len(extra):
                tag, length = struct.unpack_from('<HH', extra, e)
                if tag == 1:
                    values = list(struct.unpack_from('<' + 'Q' * (length // 8), extra, e + 4))
                    if usize == 0xffffffff:
                        usize = values.pop(0)
                    if csize == 0xffffffff:
                        csize = values.pop(0)
                    if offset == 0xffffffff:
                        offset = values.pop(0)
                e += 4 + length
            self.entries[name] = dict(method=method, crc=crc, csize=csize, usize=usize, header=offset)
            at += 46 + nlen + elen + clen

    def data_offset(self, name):
        entry = self.entries[name]
        header = get_range(self.url, entry['header'], entry['header'] + 29)
        nlen, elen = struct.unpack_from('<HH', header, 26)
        return entry['header'] + 30 + nlen + elen

    def read(self, name):
        entry = self.entries[name]
        start = self.data_offset(name)
        raw = get_range(self.url, start, start + entry['csize'] - 1)
        return zlib.decompress(raw, -15) if entry['method'] == 8 else raw


def identity_of(manifest, device_class):
    for item in manifest['BuildIdentities']:
        if item['Info']['DeviceClass'] == device_class and item['Info']['RestoreBehavior'] == 'Erase':
            return item
    raise SystemExit(f'no Erase identity for {device_class}')


def fetch(z, name, out):
    entry = z.entries[name]
    target = out / Path(name).name
    record = target.with_name(target.name + '.json')
    if target.is_file() and record.is_file() and json.loads(record.read_text()).get('size') == target.stat().st_size:
        print(f'{name}: present')
        return
    partial = target.with_name(target.name + '.partial')
    start = z.data_offset(name)
    total = entry['csize']
    print(f'{name}: {total >> 20} MiB ({"stored" if entry["method"] == 0 else "deflated"})', flush=True)
    # A large member goes in STREAMS ranges at once (one curl each; the CDN serves one connection at about 4 MiB/s). The split
    # is recorded beside the download (.split), so a later run resumes every range where it stopped; the ranges are appended
    # to the first one in order at the end.
    split = target.with_name(target.name + '.split')
    have = partial.stat().st_size if partial.exists() else 0
    if split.is_file():
        cuts = json.loads(split.read_text())
    else:
        streams = max(1, min(STREAMS, (total - have) // (256 << 20)))
        step = (total - have) // streams
        cuts = [0] + [have + step * k for k in range(1, streams)] + [total]
        split.write_text(json.dumps(cuts))
    pieces = [(partial if k == 0 else target.with_name(f'{target.name}.partial{k}'), cuts[k], cuts[k + 1]) for k in range(len(cuts) - 1)]

    def get(piece):
        path, low, high = piece
        while (got := path.stat().st_size if path.exists() else 0) < high - low:
            result = subprocess.run(['curl', '-sS', '--fail', '--retry', '20', '--retry-delay', '5', '--speed-limit', '20000',
                                     '--speed-time', '60', '-r', f'{start + low + got}-{start + high - 1}', z.url],
                                    stdout=path.open('ab'))
            if result.returncode:
                print(f'{path.name}: curl {result.returncode} at {got >> 20} MiB; resuming', file=sys.stderr, flush=True)
                time.sleep(5)
        if path.stat().st_size != high - low:
            raise SystemExit(f'{path.name}: {path.stat().st_size} bytes, expected {high - low}')

    with concurrent.futures.ThreadPoolExecutor(len(pieces)) as pool:
        list(pool.map(get, pieces))
    with partial.open('ab') as sink:
        for path, low, high in pieces[1:]:
            if partial.stat().st_size == low:
                with path.open('rb') as source:
                    shutil.copyfileobj(source, sink, 64 << 20)
                sink.flush()
            else:
                raise SystemExit(f'{partial.name}: {partial.stat().st_size} bytes before {path.name}, expected {low}: remove the pieces and fetch again')
            path.unlink()
    split.unlink()
    if partial.stat().st_size != total:
        raise SystemExit(f'{name}: {partial.stat().st_size} bytes, expected {total}')
    digest, crc = hashlib.sha256(), 0
    inflater = zlib.decompressobj(-15) if entry['method'] == 8 else None
    plain = target.with_name(target.name + '.plain') if inflater else partial
    sink = plain.open('wb') if inflater else None
    with partial.open('rb') as source:
        while chunk := source.read(8 << 20):
            if inflater:
                chunk = inflater.decompress(chunk)
                sink.write(chunk)
            digest.update(chunk)
            crc = zlib.crc32(chunk, crc)
    if inflater:
        sink.write(tail := inflater.flush())
        digest.update(tail)
        crc = zlib.crc32(tail, crc)
        sink.close()
        partial.unlink()
    if crc != entry['crc']:
        raise SystemExit(f'{name}: CRC-32 {crc:08x}, the zip says {entry["crc"]:08x}')
    plain.rename(target)
    record.write_text(json.dumps({'member': name, 'size': target.stat().st_size, 'crc32': f'{crc:08x}',
                                  'sha256': digest.hexdigest(), 'ipsw': z.url, 'ipsw_size': z.size,
                                  'ipsw_server_sha256': z.server_sha256}, indent=2) + '\n')
    print(f'{name}: ok sha256 {digest.hexdigest()}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['list', 'fetch'])
    parser.add_argument('components', nargs='*')
    parser.add_argument('--url', default=URL)
    parser.add_argument('--identity', default='k114cap')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    z = Zip(args.url)
    manifest = plistlib.loads(z.read('BuildManifest.plist'))
    identity = identity_of(manifest, args.identity)
    print(f'ipsw {manifest["ProductVersion"]} ({manifest["ProductBuildVersion"]}), {z.size} bytes, '
          f'server sha256 {z.server_sha256}; identity {args.identity} chip {identity["ApChipID"]} board {identity["ApBoardID"]}')
    paths = {key: value['Info']['Path'] for key, value in identity['Manifest'].items() if 'Info' in value and 'Path' in value['Info']}
    if args.command == 'list':
        for key, path in sorted(paths.items()):
            entry = z.entries.get(path)
            size = f'{entry["usize"] >> 20:8d} MiB' if entry else '   absent'
            print(f'{size}  {key:40s} {path}')
        return
    if not args.out:
        raise SystemExit('fetch needs --out DIR')
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / 'BuildManifest.plist').write_bytes(z.read('BuildManifest.plist'))
    wanted = DEFAULT if args.components in ([], ['all']) else args.components
    # Small members first, so the kernel/KC work can start while the disk images download.
    for component in sorted(wanted, key=lambda c: z.entries[paths[c]]['csize']):
        fetch(z, paths[component], args.out)


if __name__ == '__main__':
    main()
