#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host tests of the run-time firmware loader (package/include/PianoFirmware.h, package/common/PianoFirmware*.c) and its tools
(tooling/persist/firmware.py), end to end on synthetic files:

  stage    firmware.py stage writes the pins and the embedded symbols (a pinned file and a bare one, which becomes private)
  loader   firmware_host (ASan/UBSan): the pins, an ESP-like FAT32 image made by mtools as build-esp.py formats the ESP, the
           embedded check, the NGPUADD2 addon that firmware.py addon makes from the pins it finds in the test binary itself (as the
           RAM image builder finds them in a kernel collection), damaged containers, zlib through the loader's allocator
  pins     pins_in on noise, duplicates, conflicts; check and dump-addon
  adt      the /chosen/memory-map GpuAddon slot added to a synthetic ADT and to out/adt/piano-k27.adt when present, the rest
           unchanged, as DarwinBoot walks it
  c++      pins.h and PianoFirmware.h in C++20 (the kexts' language)
  kernel   PianoFirmwareCore.c and FatVolume.c compile freestanding, AArch64 general registers only

Run by the kexts' build scripts that use the loader, and by hand: python3 package/common/tests/run-firmware-tests.py"""
import hashlib
import importlib.util
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMMON = HERE.parent
REPO = COMMON.parents[1]
INCLUDE = REPO / 'package/include'
FAT = REPO / 'package/PianoPower'          # FatVolume.c, the copy the loader's users compile
TOOL = REPO / 'tooling/persist/firmware.py'
CFLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def run(*command, **kw):
    return subprocess.run([str(c) for c in command], check=True, **kw)


def mtools(*command):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC', SOURCE_DATE_EPOCH='1262304000')
    run(*command, env=env, capture_output=True)


def fake_adt(fw, extra=()):
    """A minimal ADT: / -> chosen -> memory-map (RAMDisk, TrustCache, EXTRA), and a sibling after chosen."""
    def prop(name, value):
        return name.encode().ljust(32, b'\0') + struct.pack('<I', len(value)) + value + bytes(-len(value) % 4)

    def node(props, children=()):
        return struct.pack('<II', len(props), len(children)) + b''.join(prop(n, v) for n, v in props) + b''.join(children)
    mmap = node([('name', b'memory-map\0'), ('RAMDisk', bytes(16)), ('TrustCache', bytes(16)), *extra])
    chosen = node([('name', b'chosen\0'), ('bootp', b'x\0')], [mmap, node([('name', b'manifest-properties\0')])])
    return node([('name', b'device-tree\0'), ('compatible', b'piano\0')], [chosen, node([('name', b'cpus\0'), ('n', b'\1\0\0\0')])])


def darwinboot_walk(data):
    """DarwinBoot's AdtNode() acceptance (DarwinBoot.c): names NUL-terminated in 32 bytes, lengths in bounds, 4-byte padding; the
    tree covers the blob exactly. Returns the memory-map's slot names."""
    slots = []

    def walk(at, depth, path):
        nprops, nchildren = struct.unpack_from('<II', data, at)
        at += 8
        name = None
        names = []
        for _ in range(nprops):
            assert b'\0' in data[at:at + 32]
            pname = data[at:at + 32].split(b'\0')[0].decode()
            length, = struct.unpack_from('<I', data, at + 32)
            at += 36
            assert length <= len(data) - at
            if pname == 'name':
                name = data[at:at + length].rstrip(b'\0').decode()
            names.append((pname, length))
            at = (at + length + 3) // 4 * 4
            assert at <= len(data)
        here = path + [name]
        if here[1:] == ['chosen', 'memory-map']:
            slots.extend(n for n, length in names if n != 'name' and length == 16)
        for _ in range(nchildren):
            at = walk(at, depth + 1, here)
        return at
    assert walk(0, 0, []) == len(data)
    return slots


def main():
    for tool in ('mformat', 'mmd', 'mcopy'):
        if not shutil.which(tool):
            print(f'SKIP firmware loader: no {tool} (brew install mtools)')
            return
    fw = module(TOOL, 'piano_firmware')
    dump = module(REPO / 'tooling/adt/adt-dump.py', 'adt_dump')
    rng = random.Random(8750)
    with tempfile.TemporaryDirectory(prefix='piano-fw-') as scratch:
        t = Path(scratch)
        src = t / 'src'
        src.mkdir()
        good = bytes(rng.getrandbits(8) for _ in range(70000))                   # 18 clusters of 4 KiB
        big = (bytes(range(256)) * 600 + bytes(rng.getrandbits(8) for _ in range(150000)))[:300000]
        unit = bytes(rng.getrandbits(8) for _ in range(1000))
        for name, data in (('good.bin', good), ('big.bin', big), ('unit.bin', unit)):
            (src / name).write_bytes(data)
        pinned = [f'{src / n}={len(d)}:{hashlib.sha256(d).hexdigest()}' for n, d in (('good.bin', good), ('big.bin', big))]
        staged = t / 'staged'
        # stage: a wrong pin is refused, then the real ones
        bad = subprocess.run([sys.executable, TOOL, 'stage', '--kext', 'PianoTest', '--header', t / 'x.h', '--source', t / 'x.c',
                              '--dir', staged, f'{src / "good.bin"}=70000:' + '0' * 64], capture_output=True, text=True)
        assert bad.returncode and 'not the pinned file' in bad.stderr, bad.stderr
        out = subprocess.run([sys.executable, TOOL, 'stage', '--kext', 'PianoTest', '--header', t / 'pins.h', '--source', t / 'symbols.c',
                              '--embed', '--dir', staged, *pinned, src / 'unit.bin'], check=True, capture_output=True, text=True).stderr
        assert '(private)' in out and hashlib.sha256(unit).hexdigest() not in out, out
        assert (staged / 'PianoTest/good.bin').read_bytes() == good
        header = (t / 'pins.h').read_text()
        assert 'PIANO_FW_PRIVATE' in header and header.count('PIANO_FW_PIN(') == 3
        # a pinned file this host lacks: its pin all the same, size 0 inside, nothing staged; a bare missing file is refused
        absent = subprocess.run([sys.executable, TOOL, 'stage', '--kext', 'PianoTest', '--header', t / 'y.h', '--source', t / 'y.c',
                                 '--embed', '--dir', t / 'staged-y', f'{src / "absent.bin"}=4096:' + 'ab' * 32],
                                check=True, capture_output=True, text=True).stderr
        assert 'NOT STAGED' in absent and '"PianoTest/absent.bin", 4096, 0, 0xab' in (t / 'y.h').read_text(), absent
        assert '.quad 0' in (t / 'y.c').read_text() and '.incbin' not in (t / 'y.c').read_text()
        assert not (t / 'staged-y/PianoTest/absent.bin').exists()
        refused = subprocess.run([sys.executable, TOOL, 'stage', '--kext', 'PianoTest', '--header', t / 'z.h', '--source', t / 'z.c',
                                  '--dir', t / 'staged-z', src / 'absent.bin'], capture_output=True, text=True)
        assert refused.returncode and 'missing' in refused.stderr
        print('PASS stage: 3 files pinned (one private: no hash printed), copied, the embedded symbols written; a wrong pin refused; '
              'a pinned file the host lacks keeps its pin, size 0 inside')

        # the ESP-like volume
        image = t / 'esp.img'
        mtools('mformat', '-C', '-F', '-S', '5', '-M', '4096', '-T', '70000', '-h', '1', '-s', '1', '-c', '1', '-i', image, '::')
        mtools('mmd', '-i', image, '::/EFI', '::/EFI/UniXNU', '::/EFI/UniXNU/firmware', '::/EFI/UniXNU/firmware/PianoTest',
               '::/EFI/UniXNU/firmware/PianoTest/dir')
        (src / 'short.bin').write_bytes(good[:-1])
        flipped = bytearray(good)
        flipped[12345] ^= 0x20
        (src / 'flipped.bin').write_bytes(bytes(flipped))
        for path in [*(staged / 'PianoTest').iterdir(), src / 'short.bin', src / 'flipped.bin']:
            mtools('mcopy', '-i', image, path, f'::/EFI/UniXNU/firmware/PianoTest/{path.name}')

        # the loader, then the addon of the pins in the test binary itself
        exe = t / 'firmware_host'
        run(os.environ.get('CC', 'cc'), *CFLAGS, '-I', INCLUDE, '-I', COMMON, '-I', FAT, '-I', t, HERE / 'firmware_host.c',
            COMMON / 'PianoFirmwareCore.c', COMMON / 'PianoFirmwareZlib.c', FAT / 'FatVolume.c', t / 'symbols.c', '-lz', '-o', exe)
        pins = fw.kc_pins(exe)
        assert [p['path'] for p in pins] == ['PianoTest/big.bin', 'PianoTest/good.bin', 'PianoTest/unit.bin'], pins
        assert [p['private'] for p in pins] == [False, False, True]
        found, problems = fw.staged(pins, staged)
        assert len(found) == 3 and not problems
        addon = t / 'addon.bin'
        run(sys.executable, TOOL, 'addon', exe, addon, '--dir', staged, capture_output=True)
        entries = fw.parse_addon(addon.read_bytes())
        assert [(n, len(d)) for n, _, d in entries] == [('EFI/UniXNU/firmware/PianoTest/big.bin', len(big)),
                                                        ('EFI/UniXNU/firmware/PianoTest/good.bin', len(good)),
                                                        ('EFI/UniXNU/firmware/PianoTest/unit.bin', len(unit))]
        result = subprocess.run([exe, image, addon], capture_output=True, text=True)
        print((result.stdout + result.stderr).strip())
        if result.returncode:
            sys.exit('FAIL: firmware_host')

        # pins: noise, duplicates, conflicts; check and dump-addon
        raw = exe.read_bytes()
        one = raw[raw.find(fw.MAGIC):raw.find(fw.MAGIC) + fw.PIN.size]
        assert len(fw.pins_in(b'PianoFw1' + bytes(200) + one + one + b'PianoFw1junk')) == 1
        other = bytearray(one)
        other[-1] ^= 1
        try:
            fw.pins_in(one + bytes(8) + bytes(other))
            raise AssertionError('conflicting pins accepted')
        except SystemExit:
            pass
        (staged / 'PianoTest/good.bin').write_bytes(good[:-2])
        check = subprocess.run([sys.executable, TOOL, 'check', exe, '--dir', staged], capture_output=True, text=True)
        assert check.returncode == 1 and 'PianoTest/good.bin' in check.stdout, check.stdout
        listing = subprocess.run([sys.executable, TOOL, 'dump-addon', addon], check=True, capture_output=True, text=True).stdout
        assert listing.count('EFI/UniXNU/firmware/PianoTest/') == 3
        print('PASS pins: found in a binary, noise skipped, duplicates merged, conflicts refused; check reports a changed copy')

        # the ADT slot
        plain = fake_adt(fw)
        assert not fw.adt_slot(plain) and darwinboot_walk(plain) == ['RAMDisk', 'TrustCache']
        slotted = fw.adt_with_slot(plain)
        assert fw.adt_slot(slotted) and darwinboot_walk(slotted) == ['RAMDisk', 'TrustCache', 'GpuAddon']
        assert fw.adt_with_slot(slotted) == slotted
        lines = lambda blob: (dump.walk(dump.parse(blob)[0], None, True, out := []), sorted(out))[1]   # noqa: E731
        added = set(lines(slotted)) - set(lines(plain))
        assert set(lines(plain)) <= set(lines(slotted)) and len(added) == 1 and 'GpuAddon' in added.pop()
        real = REPO / 'out/adt/piano-k27.adt'
        note = ''
        if real.is_file():
            data = real.read_bytes()
            if not fw.adt_slot(data):
                with_slot = fw.adt_with_slot(data)
                assert darwinboot_walk(with_slot)[-1] == 'GpuAddon' and len(with_slot) == len(data) + 52
                assert set(lines(with_slot)) - set(lines(data)) == {'/chosen/memory-map  GpuAddon = ' + '00' * 16}
                assert set(lines(data)) <= set(lines(with_slot))
                note = f', and to {real.name} ({len(data)} bytes)'
        print(f'PASS adt: the GpuAddon slot added to a synthetic ADT{note}, the rest unchanged, idempotent, as DarwinBoot walks it')

        # C++ and the kernel's constraints
        cpp = t / 'pins.cpp'
        cpp.write_text('#include <cstddef>\n#include "pins.h"\nstatic_assert(sizeof(PianoFirmwarePin) == 136);\n'
                       'static_assert(offsetof(PianoFirmwarePin, size) == 96 && offsetof(PianoFirmwarePin, sha256) == 104);\n'
                       'const PianoFirmwarePin *use() {return &piano_fw_pianotest_good_pin;}\n')
        run(os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', INCLUDE, '-I', t, '-c', cpp, '-o', t / 'pins.o')
        for source in (COMMON / 'PianoFirmwareCore.c', FAT / 'FatVolume.c'):
            run(os.environ.get('CC', 'cc'), '-std=c11', '-target', 'arm64-apple-macos13', '-ffreestanding', '-fno-builtin',
                '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-I', INCLUDE, '-I', COMMON, '-I', FAT, '-c', source,
                '-o', t / 'k.o')
        print('PASS c++ and kernel constraints: the pins in C++20; PianoFirmwareCore.c and FatVolume.c freestanding, general registers only')
    print('PASS firmware loader host tests')


if __name__ == '__main__':
    main()
