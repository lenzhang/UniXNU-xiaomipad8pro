#!/usr/bin/env python3
"""Decrypt an AEA disk image of the 27.3 IPSW beside it (NAME.dmg.aea -> NAME.dmg), as UniXNU's scripts/unixnu/ipsw.py
decrypt_aea() does: the key from `ipsw fw aea --key` (blacktop/ipsw: the archive header's key URL), passed to macOS /usr/bin/aea;
the key is never written to disk or printed. Records NAME.dmg.json {member, aea_sha256, size, sha256}.

  decrypt-aea.py FILE.dmg.aea [--remove]      --remove deletes the archive once the plain image is written and hashed

The archive's sha256 must match the one tooling/k273/ipsw-members.py recorded at the download (FILE.dmg.aea.json).
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while chunk := f.read(16 << 20):
            h.update(chunk)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('archive', type=Path)
    p.add_argument('--remove', action='store_true')
    a = p.parse_args()
    if a.archive.suffix != '.aea':
        sys.exit('not an .aea file')
    plain = a.archive.with_suffix('')
    record = plain.with_name(plain.name + '.json')
    origin = json.loads(a.archive.with_name(a.archive.name + '.json').read_text())
    if record.is_file() and plain.is_file() and json.loads(record.read_text()).get('size') == plain.stat().st_size:
        print(f'{plain}: present')
    else:
        digest = sha256(a.archive)
        if digest != origin['sha256']:
            sys.exit(f'{a.archive}: sha256 {digest}, the download recorded {origin["sha256"]}')
        result = subprocess.run(['ipsw', 'fw', 'aea', '--key', str(a.archive)], capture_output=True, text=True)
        keys = [line.strip() for line in result.stdout.splitlines() if line.strip().startswith('base64:')]
        if result.returncode or len(keys) != 1:
            sys.exit(f'no AEA key for {a.archive.name}: {result.stderr.strip()[-300:]}')
        partial = plain.with_name(plain.name + '.partial')
        result = subprocess.run(['/usr/bin/aea', 'decrypt', '-i', str(a.archive), '-o', str(partial), '-key-value', keys[0]],
                                capture_output=True, text=True)
        if result.returncode:
            partial.unlink(missing_ok=True)
            sys.exit(f'aea decrypt failed: {result.stderr.strip()[-300:]}')
        partial.rename(plain)
        record.write_text(json.dumps({'member': origin['member'], 'aea_sha256': digest, 'size': plain.stat().st_size,
                                      'sha256': sha256(plain)}, indent=2) + '\n')
        print(f'{plain}: {plain.stat().st_size} bytes, sha256 {json.loads(record.read_text())["sha256"]}')
    if a.remove:
        a.archive.unlink()
        print(f'{a.archive}: removed')


if __name__ == '__main__':
    main()
