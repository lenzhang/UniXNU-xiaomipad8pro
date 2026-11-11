#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""The run-time firmware files of the native kexts (package/include/PianoFirmware.h, docs/kc-space.md): the pins a kernel collection's
kexts carry, the staged copies in the ignored out/firmware/, the ESP's EFI/UniXNU/firmware/ and a RAM image's addon.

  firmware.py pins KC                        the pins in the collection (path, size, SHA-256 but for private ones)
  firmware.py check KC [--dir DIR]           which pins the staged copies under DIR (default out/firmware) satisfy
  firmware.py stage --kext NAME --header H --source C [--embed] FILE...
                                             a kext's build script: each FILE checked (FILE=BYTES:SHA256: a file pinned in committed
                                             code, whose pin is written even when this host lacks FILE, so the collection does not
                                             depend on it; a bare FILE is pinned as it is and private: this unit's own data, whose
                                             hash no committed file holds), copied to out/firmware/NAME/<basename>,
                                             its PIANO_FW_PIN line written to the header H and its symbols to the C
                                             source C: the file inside (--embed, .incbin into the kext's __TEXT: pointer
                                             free, no __DATA_CONST) or size 0 (read at run time)
  firmware.py addon KC OUT [--dir DIR]       the NGPUADD2 addon of the collection's pins alone (build-ram-images.py puts these
                                             entries into the RAM image's one Payloads/kernel.macho.gpuaddon, next to
                                             PianoSensors' pack entry when PIANO_SENSOR_PACK is set)
  firmware.py dump-addon ADDON               its entries

A pin (PianoFirmwarePin, 136 bytes): "PianoFw1", the path below EFI/UniXNU/firmware (88 bytes with its NUL), u32 size, u32 flags
(1: private, this unit's own data: no hash is printed or recorded), the SHA-256. The kexts keep their pins (PIANO_FW_PIN, `used`), so
the collection itself says which files its kexts read; the builders place exactly those, checked, and nothing else.
The addon is UniXNU's container (scripts/make-gpu-addon.py): "NGPUADD2", u32 count, u32 0, u64 total; per file u32 path bytes (with
the NUL, at most 160), u32 mode, u64 size, u64 compressed size, the path and the zlib stream each padded to 8 bytes. DarwinBoot loads
<kernel>.gpuaddon from the payload only when the ADT has a /chosen/memory-map GpuAddon slot, and refuses to boot when one is there
without the other: adt_with_slot() / adt_slot() keep the two together. It loads one such file: every user's entries share it
(container()), and each reader takes only its own paths.
"""
import argparse
import hashlib
import re
import shutil
import struct
import sys
import zlib
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[2]
MAGIC = b'PianoFw1'
PIN = struct.Struct('<8s88sII32s')
PATH_BYTES = 88
MAX_BYTES = 16 << 20
ADDON_MAX = 64 << 20
FLAGS = {'private': 1}
ESP_DIR = 'EFI/UniXNU/firmware'
PATH = re.compile(r'(?:[A-Za-z0-9._-]+/)*[A-Za-z0-9._-]+')
ADDON_MAGIC = b'NGPUADD2'
SLOT = 'GpuAddon'   # DarwinBoot's addon slot in /chosen/memory-map (named for its first use, the VM-era GPU plugin)


def path_ok(path):
    return bool(PATH.fullmatch(path)) and all(part.strip('.') for part in path.split('/')) and len(path) < PATH_BYTES


def parse_pin(raw):
    magic, path, size, flags, digest = PIN.unpack(raw)
    if magic != MAGIC or b'\0' not in path:
        return None
    text, rest = path.split(b'\0', 1)
    try:
        text = text.decode('ascii')
    except UnicodeDecodeError:
        return None
    if not path_ok(text) or rest.strip(b'\0') or not 0 < size <= MAX_BYTES or flags & ~1:
        return None
    return {'path': text, 'size': size, 'flags': flags, 'private': bool(flags & 1), 'sha256': digest.hex()}


def pins_in(data):
    """The collection's pins, one per path. Raises SystemExit when two pins name one path with different contents."""
    found, at = {}, data.find(MAGIC)
    while at >= 0:
        pin = parse_pin(data[at:at + PIN.size]) if at + PIN.size <= len(data) else None
        if pin:
            other = found.get(pin['path'])
            if other and (other['size'], other['sha256']) != (pin['size'], pin['sha256']):
                raise SystemExit(f'two pins for {ESP_DIR}/{pin["path"]} with different contents: the kexts disagree')
            if other:
                other['private'] |= pin['private']
            else:
                found[pin['path']] = pin
        at = data.find(MAGIC, at + 1)
    return sorted(found.values(), key=lambda p: p['path'])


def kc_pins(kc):
    return pins_in(Path(kc).read_bytes())


def staged(pins, directory):
    """(found [(pin, data)], problems [(pin, text)]) for the copies under DIRECTORY (out/firmware: DIRECTORY/<pin path>)."""
    found, problems = [], []
    for pin in pins:
        path = Path(directory) / pin['path']
        if not path.is_file():
            problems.append((pin, f'missing ({path})'))
            continue
        data = path.read_bytes()
        if len(data) != pin['size']:
            problems.append((pin, f'{len(data)} bytes, the pin says {pin["size"]}'))
        elif hashlib.sha256(data).hexdigest() != pin['sha256']:
            problems.append((pin, 'another SHA-256 than the pin\'s'))
        else:
            found.append((pin, data))
    return found, problems


def describe(pin):
    digest = 'private: no hash shown' if pin['private'] else pin['sha256']
    return f'{ESP_DIR}/{pin["path"]}  {pin["size"]} bytes  {digest}'


def record(pins, found, problems):
    """For a build record (JSON): no hash of a private pin."""
    ok = {id(p) for p, _ in found}
    return [{'path': f'{ESP_DIR}/{p["path"]}', 'bytes': p['size'], 'private': p['private'],
             'sha256': None if p['private'] else p['sha256'], 'placed': id(p) in ok,
             'problem': next((text for q, text in problems if q is p), None)} for p in pins]


def firmware_entries(found):
    """FOUND [(pin, data)] as addon entries [(path, mode, data)]: EFI/UniXNU/firmware/<path>, mode 0644, sorted."""
    return [(f'{ESP_DIR}/{pin["path"]}', 0o644, data) for pin, data in sorted(found, key=lambda item: item[0]['path'])]


def container(entries):
    """The NGPUADD2 container of ENTRIES [(path, mode, data)] in their order, zlib level 9 (one container carries every user's
    entries: the firmware's, PianoSensors' pack; each reader takes only its own paths)."""
    body = b''
    for path, mode, data in entries:
        name = path.encode() + b'\0'
        if len(name) > 160 or len({p for p, _, _ in entries}) != len(entries):
            raise SystemExit(f'{path}: an addon path has at most 160 bytes and appears once')
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(name), mode, len(data), len(packed)) + pad8(name) + pad8(packed)
    blob = ADDON_MAGIC + struct.pack('<IIQ', len(entries), 0, 24 + len(body)) + body
    if len(blob) > ADDON_MAX:
        raise SystemExit(f'the addon has {len(blob)} bytes; DarwinBoot reads at most {ADDON_MAX}')
    return blob


def addon_bytes(found):
    """The NGPUADD2 container of FOUND [(pin, data)] alone."""
    return container(firmware_entries(found))


def pad8(data):
    return data + bytes(-len(data) % 8)


def parse_addon(blob):
    """[(path, mode, data)] of an NGPUADD2 container, every length checked as PianoFirmwareCore.c checks it."""
    if len(blob) < 24 or blob[:8] != ADDON_MAGIC:
        raise ValueError('not an NGPUADD2 addon')
    count, zero, total = struct.unpack_from('<IIQ', blob, 8)
    if zero or total != len(blob) or count > 4096:
        raise ValueError('addon header')
    at, out = 24, []
    for _ in range(count):
        if len(blob) - at < 24:
            raise ValueError('addon entry')
        name_bytes, mode, size, packed = struct.unpack_from('<IIQQ', blob, at)
        name_at = at + 24
        data_at = name_at + (name_bytes + 7) // 8 * 8
        end = data_at + (packed + 7) // 8 * 8
        if not 2 <= name_bytes <= 160 or end > len(blob):
            raise ValueError('addon entry bounds')
        name = blob[name_at:name_at + name_bytes]
        if name[-1] or b'\0' in name[:-1]:
            raise ValueError('addon entry name')
        data = zlib.decompress(blob[data_at:data_at + packed])
        if len(data) != size:
            raise ValueError(f'{name[:-1].decode()}: {len(data)} bytes inflated, {size} recorded')
        out.append((name[:-1].decode(), mode, data))
        at = end
    if at != len(blob):
        raise ValueError('addon total')
    return out


# ------------------------------------------------------------------------------------------------ the ADT's /chosen/memory-map slot
def _node(data, at):
    """(end, props [(name, name_at, value_at, length)], children [start]) of the ADT node at AT."""
    nprops, nchildren = struct.unpack_from('<II', data, at)
    props, offset = [], at + 8
    for _ in range(nprops):
        name = data[offset:offset + 32].split(b'\0', 1)[0].decode(errors='replace')
        length = struct.unpack_from('<I', data, offset + 32)[0] & 0x7fffffff
        props.append((name, offset, offset + 36, length))
        offset += 36 + (length + 3) // 4 * 4
    children = []
    for _ in range(nchildren):
        children.append(offset)
        offset = _node(data, offset)[0]
    return offset, props, children


def _child(data, at, name):
    for child in _node(data, at)[2]:
        props = {n: data[v:v + length] for n, _, v, length in _node(data, child)[1]}
        if props.get('name', b'').rstrip(b'\0') == name.encode():
            return child
    return None


def _memory_map(adt):
    chosen = _child(adt, 0, 'chosen')
    return None if chosen is None else _child(adt, chosen, 'memory-map')


def adt_slot(adt, name=SLOT):
    """True when the ADT's /chosen/memory-map has the slot NAME (16 bytes)."""
    at = _memory_map(adt)
    return at is not None and any(n == name and length == 16 for n, _, _, length in _node(adt, at)[1])


def adt_with_slot(adt, name=SLOT):
    """The ADT with an empty 16-byte /chosen/memory-map slot NAME, which DarwinBoot fills (a no-op when it is there)."""
    adt = bytes(adt)
    if adt_slot(adt, name):
        return adt
    at = _memory_map(adt)
    if at is None:
        raise SystemExit('the ADT has no /chosen/memory-map')
    props = _node(adt, at)[1]
    end_of_props = props[-1][2] + (props[-1][3] + 3) // 4 * 4 if props else at + 8
    nprops, = struct.unpack_from('<I', adt, at)
    prop = name.encode().ljust(32, b'\0') + struct.pack('<I', 16) + bytes(16)
    out = bytearray(adt[:end_of_props] + prop + adt[end_of_props:])
    struct.pack_into('<I', out, at, nprops + 1)
    if _node(bytes(out), 0)[0] != len(out) or not adt_slot(bytes(out), name):
        raise SystemExit('the ADT did not take the addon slot')
    return bytes(out)


# ------------------------------------------------------------------------------------------------------------------- stage
def stage(a):
    """A kext's build script: check, copy to out/firmware/<kext>/, write the pin header and the symbols' source (the files inside
    with --embed, else every symbol at size 0: the kext then reads the files at run time)."""
    if not re.fullmatch(r'[A-Za-z0-9]+', a.kext):
        raise SystemExit(f'--kext {a.kext}: a kext name')
    out_dir = Path(a.dir) / a.kext
    out_dir.mkdir(parents=True, exist_ok=True)
    lines = [f'// Generated by tooling/persist/firmware.py stage for {a.kext}: the pins of its run-time files (<PianoFirmware.h>).',
             '// From the ignored out/ copies; never committed.', '#pragma once', '#include <PianoFirmware.h>']
    symbols = [f'// Generated by tooling/persist/firmware.py stage for {a.kext}: ' + ('the files inside the kext (the embedded path), in its'
               ' __TEXT: pointer free, they need no __DATA_CONST' if a.embed else 'no file inside: every size is 0, the kext reads its'
               ' files at run time') + '. Never committed.']
    for spec in a.files:
        source, _, want = spec.partition('=')
        source = Path(source)
        private = not want
        if want:   # a pin of committed code: the pin stands whether or not this build host has the file
            size, _, sha = want.partition(':')
            size, digest = int(size, 0), sha.lower()
            if not re.fullmatch(r'[0-9a-f]{64}', digest):
                raise SystemExit(f'{spec}: FILE=BYTES:SHA256')
        elif not source.is_file():
            raise SystemExit(f'{source}: missing (a file without BYTES:SHA256 is pinned as it is, so it must be there)')
        data = source.read_bytes() if source.is_file() else None
        if data is not None and not want:
            size, digest = len(data), hashlib.sha256(data).hexdigest()
        if data is not None and (len(data) != size or hashlib.sha256(data).hexdigest() != digest):
            raise SystemExit(f'{source} is not the pinned file ({size} bytes, sha256 {digest})')
        path = f'{a.kext}/{source.name}'
        if not path_ok(path) or not 0 < size <= MAX_BYTES:
            raise SystemExit(f'{path}: not a firmware path or size (<PianoFirmware.h>)')
        copy = out_dir / source.name
        if data is None:
            copy.unlink(missing_ok=True)
            print(f'Warning: no {source}: {a.kext} is built with its pin and without the file; stage it before building an ESP or a '
                  'RAM image (else the kext reports it missing)', file=sys.stderr)
        elif not copy.is_file() or copy.read_bytes() != data:
            shutil.copyfile(source, copy)
        symbol = 'piano_fw_' + re.sub(r'[^A-Za-z0-9]', '_', f'{a.kext}_{source.stem}').lower()
        flags = 'PIANO_FW_PRIVATE' if private else '0'
        lines.append(f'PIANO_FW_PIN({symbol}_pin, "{path}", {size}, {flags}, '
                     + ', '.join(f'0x{b:02x}' for b in bytes.fromhex(digest)) + ');')
        # the embedded copy and its size, defined by the --source file (size 0 unless --embed and the file is there)
        lines += ['#ifdef __cplusplus', 'extern "C" {', '#endif', f'extern const uint8_t {symbol}[];',
                  f'extern const uint64_t {symbol}_size;', '#ifdef __cplusplus', '}', '#endif']
        inside = a.embed and data is not None
        body = f'.incbin \\"{copy.resolve()}\\"\\n' if inside else ''
        symbols.append(f'__asm__(".section __TEXT,__const\\n.p2align 3\\n.globl _{symbol}\\n_{symbol}:\\n{body}.p2align 3\\n'
                       f'.globl _{symbol}_size\\n_{symbol}_size:\\n.quad {size if inside else 0}\\n");')
        print(f'{a.kext}: {ESP_DIR}/{path}: {size} bytes' + (' (private)' if private else f' sha256 {digest}')
              + (', embedded' if inside else '') + ('' if data is not None else ', NOT STAGED'), file=sys.stderr)
    header = Path(a.header)
    header.parent.mkdir(parents=True, exist_ok=True)
    text = '\n'.join(lines) + '\n'
    if not header.is_file() or header.read_text() != text:
        header.write_text(text)
    source = Path(a.source)
    source.parent.mkdir(parents=True, exist_ok=True)
    text = '\n'.join(symbols) + '\n'
    if not source.is_file() or source.read_text() != text:
        source.write_text(text)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    s = sub.add_parser('pins')
    s.add_argument('kc', type=Path)
    s = sub.add_parser('check')
    s.add_argument('kc', type=Path)
    s.add_argument('--dir', type=Path, default=DEVICE / 'out/firmware')
    s = sub.add_parser('stage')
    s.add_argument('--kext', required=True)
    s.add_argument('--header', required=True)
    s.add_argument('--source', required=True, help="the C file that defines the files' symbols (compiled into the kext)")
    s.add_argument('--embed', action='store_true', help='the files inside the kext (.incbin), else size 0: read at run time')
    s.add_argument('--dir', default=str(DEVICE / 'out/firmware'))
    s.add_argument('files', nargs='+', metavar='FILE[=BYTES:SHA256]')
    s = sub.add_parser('addon')
    s.add_argument('kc', type=Path)
    s.add_argument('output', type=Path)
    s.add_argument('--dir', type=Path, default=DEVICE / 'out/firmware')
    s = sub.add_parser('dump-addon')
    s.add_argument('addon', type=Path)
    a = p.parse_args()
    if a.command == 'stage':
        return stage(a)
    if a.command == 'dump-addon':
        for path, mode, data in parse_addon(a.addon.read_bytes()):
            print(f'{mode:o} {len(data):>9} {path}')
        return None
    pins = kc_pins(a.kc)
    if a.command == 'pins':
        print(f'{a.kc.name}: {len(pins)} pin(s)')
        for pin in pins:
            print('  ' + describe(pin))
        return None
    found, problems = staged(pins, a.dir)
    if a.command == 'check':
        print(f'{a.kc.name}: {len(pins)} pin(s), {len(found)} staged in {a.dir}')
        for pin, text in problems:
            print(f'  {ESP_DIR}/{pin["path"]}: {text}')
        return 1 if problems else None
    if problems:
        raise SystemExit('; '.join(f'{pin["path"]}: {text}' for pin, text in problems))
    a.output.write_bytes(addon_bytes(found))
    print(f'{a.output}: {len(found)} file(s), {a.output.stat().st_size} bytes')
    return None


if __name__ == '__main__':
    sys.exit(main())
