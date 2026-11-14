#!/usr/bin/env python3
"""Host-only tests of the 27.3-only ESP and the retirement of linux_rootfs (docs/persistent-boot.md section 13). No tablet, no
fastboot, no backup disk, and no file of out/ or CRITICAL-BACKUP: every fixture is made here, in a temporary directory.

1. common.entry_uses_rootfs.
2. tooling/k273/build-k273-esp.py on a made-up dual ESP (mtools; a stand-in 27.3 collection with its markers and a firmware pin, an
   ADT asking for the 13 GiB window, a 24 GiB DBRDISK1 seed): the 27.3 payload and line byte for byte, the fastboot entry, no 27.0.1
   payload, the record's store facts, the same image twice, the -diag entry only under 1000 bytes; refusals (--expect-*, a base
   that is not its record's, a line without a bound store, a store LBA that is not NativeUfs's place for the seed).
3. tablet.sh discard-info / discard / zero on a fake sysfs: the partition guard (never the disk, never another partition).
4. maint-session.py against test-persist.py's fake tablet, on a made-up GPT (linux_rootfs small, holding the 27.0.1 store header
   place LBA 68119455; linux_data and the ESP where they are), the 27.3 store bound at the end of linux_data, a boot_a v5 stand-in:
   the 27.3-only gate (map's "IS the one this ESP resumes" line; esp-final refuses a store bound elsewhere, of another seed,
   unbound, blank, a record whose window is not NativeUfs's place, and an 8 GiB FD under the 13 GiB ESP), the install and its
   rollback (installed.json follows), rootfs-backup (pieces, a broken stream retried, a stopped run resumed, gzip with the exact
   checks), rootfs-clear's refusals (no backup, a damaged piece, a changed partition, another geometry, an ESP of no record or of a
   record whose menu is not the image's, the dual ESP) and its success (discard, and a device that keeps data after a discard:
   zeros), check and map after it, rollback esp-final refused while linux_rootfs is cleared under a 27.0.1 default, rollback
   rootfs-clear (and resumed), and at the end every region as it was.
  test-k273-only.py [--keep DIR]
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import common  # noqa: E402

D = HERE.parents[1]
spec = importlib.util.spec_from_file_location('test_persist', HERE / 'test-persist.py')
tp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tp)

B, MiB = common.LBA, 1 << 20
PY = sys.executable
BUILDER = D / 'tooling/k273/build-k273-esp.py'
MAINT = HERE / 'maint-session.py'
WINDOW13 = 0x33fe00000
STORE_LBA = 117998394                                  # NativeUfs's place for a 24 GiB seed at the end of linux_data
ROOTFS_FIRST, ROOTFS_BLOCKS = 68110000, 40960         # a small linux_rootfs around the 27.0.1 store header place (68119455)
ROOT_IMAGE_BYTES = 20936916992
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)


def sha(data):
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------------------------------------------------------
# fixtures

def adt_node(props, children=()):
    out = struct.pack('<II', len(props), len(children))
    for name, value in props:
        out += name.encode().ljust(32, b'\0') + struct.pack('<I', len(value)) + value + bytes(-len(value) % 4)
    return out + b''.join(children)


def make_adt(window):
    chosen = adt_node([('name', b'chosen\0'), ('darwinboot-memory-size', struct.pack('<Q', window))], [adt_node([('name', b'memory-map\0')])])
    return adt_node([('name', b'device-tree\0')], [chosen])


def pin(path, data, private=False):
    return struct.pack('<8s88sII32s', b'PianoFw1', path.encode(), len(data), int(private), hashlib.sha256(data).digest())


def make_kc(markers, pins):
    head = b'\xcf\xfa\xed\xfe' + bytes(8) + struct.pack('<I', 12) + bytes(16)
    return head + b'\0'.join(markers) + b'\0' * 8 + b''.join(pins) + os.urandom(4096)


def make_seed(logical, pages=1):
    """A DBRDISK1 packed seed of LOGICAL bytes with PAGES non-zero pages at its start."""
    data = os.urandom(pages * 4096)
    return b'DBRDISK1' + struct.pack('<II', 1, 4096) + struct.pack('<QQ', logical, 1) + struct.pack('<QQ', 0, len(data)) + data


def mtools(*command):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC', SOURCE_DATE_EPOCH='1262304000')
    subprocess.run(list(command), check=True, env=env, capture_output=True, text=True)


def make_esp(image, files):
    """A 1 GiB FAT32 ESP as build-esp.py makes one, holding FILES {path: bytes} (sparse: a few MiB on disk)."""
    with tempfile.TemporaryDirectory(prefix='piano-k273-test-esp-') as scratch:
        stage = Path(scratch)
        for path, data in files.items():
            (stage / path).parent.mkdir(parents=True, exist_ok=True)
            (stage / path).write_bytes(data)
        for path in [stage, *stage.rglob('*')]:
            os.utime(path, (1262304000, 1262304000))
        image.unlink(missing_ok=True)
        mtools('mformat', '-C', '-F', '-S', '5', '-M', '4096', '-T', str(common.ESP_BLOCKS), '-h', '1', '-s', '1', '-c', '1',
               '-H', str(common.ESP_FIRST), '-v', 'PIANOESP', '-N', '50494e4f', '-i', str(image), '::')
        mtools('mcopy', '-i', str(image), '-s', '-Q', '-m', str(stage / 'EFI'), '::/')


GENERIC = ('debug=0x14e kt_fixed_zones=120 -no_slto_panic tlto_us=0 -apfs_shared_datavolume offline-apfs-load=1 owner-staged-apps=1 '
           'mac-amfi-launch-permissive=1 union-hide-lower=1 malloc-ranges-off=1 arm64_maxoffset=0x7FFE000000 '
           'droidvm-timebase-hz=24000000 rootauth=0 vm_compression_limit=1572864 droidvm-tz=UTC pstore-console=0x9cb00000 '
           'pstore-console-size=0x700000 native-ufs-root=1')
DRIVERS = ('piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-gpu-hang-ms=10000 '
           'piano-keyboard=2 piano-keyboard-gestures=1 piano-backlight=2 piano-backlight-keys=1 piano-touch=2 piano-power=2 '
           'piano-power-policy=1 piano-power-rp=1 piano-thermal=1 piano-thermal-adc=1 piano-usb-net=1 piano-usb-shell=1 piano-wifi=6 '
           'piano-wifi-iova=1 piano-power-delay=0 piano-rtc=1 piano-rtc-offset=1000000 piano-boot-guard=180')


def line_273(store=f'native-ufs-root-part=linux_data data-store-lba={STORE_LBA} ', long=True):
    """A 27.3 line: LONG as the installed one (960-999 bytes: no room for a -diag entry), else a short one."""
    line, pad = '', 0
    while not line or (long and len(line) < 960):
        extra = ''.join(f'p{i}=0 ' for i in range(pad))
        line = f'{GENERIC} {DRIVERS if long else "piano-usb=2"} {extra}{store}data-persist=2 droidvm-utc=1700000000 sheng-smp=1'
        pad += 1
    assert len(line) < 1000, len(line)
    return line


def entry(id_, title, path, initrd, args):
    return f'[[entry]]\nid = "{id_}"\ntitle = "{title}"\ntype = "xnu"\npath = "{path}"\ninitrd = "{initrd}"\nargs = "{args}"\n'


ACTIONS = ('[[entry]]\nid = "reset"\ntitle = "Restart"\ntype = "action"\naction = "reset"\n'
           '[[entry]]\nid = "shutdown"\ntitle = "Power off"\ntype = "action"\naction = "shutdown"\n'
           '[[entry]]\nid = "tools"\ntitle = "Tools"\ntype = "action"\naction = "tools"\n')


def make_dual(out, name, line, inputs=None):
    """A made-up dual ESP (build-dual-esp.py's shape) and its record in OUT. Returns (record, the payload inputs)."""
    if inputs is None:
        fsm = os.urandom(4348)
        old_fw = os.urandom(1000)
        inputs = {
            'fsm': fsm, 'old_fw': old_fw,
            'kc273': make_kc([b'com.unixnu.driver.NativeUfs', b'com.unixnu.driver.NativeRamBlock', b'data-store-lba=%llu',
                              b'piano-boot-guard: healthy after %u s', b'root-part=%s', b'piano-power-rp-max', b'data-store-replace',
                              b'piano-rtc: count %u + offset %u'], [pin('PianoAudio/fs19xx.fsm', fsm)]),
            'adt273': make_adt(WINDOW13), 'trust273': os.urandom(3000), 'seed273': make_seed(24 << 30, 3),
            'kc': make_kc([b'com.unixnu.driver.NativeUfs', b'data-store-replace'],
                          [pin('PianoAudio/fs19xx.fsm', fsm), pin('Old/old.bin', old_fw)]),
            'adt': make_adt(WINDOW13), 'trust': os.urandom(2000), 'seed': make_seed(12 << 30, 2)}
    old_line = f'{GENERIC} piano-usb=2 data-persist=2 droidvm-utc=1700000000 sheng-smp=1'
    toml = ('# made-up dual ESP (test-k273-only.py)\nversion = 1\n[boot]\ntimeout = 3\ndefault = "macos-27.3"\n[kits]\nautoload = false\n' +
            entry('macos', 'macOS (native)', 'Payloads/kernel.macho', 'Payloads/scratch-data.img', old_line) +
            entry('macos-27.3', 'macOS 27.3 (Mac17,11)', 'Payloads/k273/kernel.macho', 'Payloads/k273/scratch-data.img', line) +
            entry('fastboot', 'Reset into fastboot (ABL)', 'Payloads/kernel.macho', 'Payloads/scratch-data.img',
                  f'{GENERIC.replace(" native-ufs-root=1", "")} data-persist=0 piano-reset-after=1 piano-reset-to=bootloader sheng-smp=1') +
            ACTIONS)
    files = {'EFI/UniXNU/unixnu.toml': toml.encode(),
             'EFI/UniXNU/Payloads/kernel.macho': inputs['kc'], 'EFI/UniXNU/Payloads/kernel.macho.adt': inputs['adt'],
             'EFI/UniXNU/Payloads/kernel.macho.trustcache': inputs['trust'], 'EFI/UniXNU/Payloads/scratch-data.img': inputs['seed'],
             'EFI/UniXNU/Payloads/k273/kernel.macho': inputs['kc273'], 'EFI/UniXNU/Payloads/k273/kernel.macho.adt': inputs['adt273'],
             'EFI/UniXNU/Payloads/k273/kernel.macho.trustcache': inputs['trust273'],
             'EFI/UniXNU/Payloads/k273/scratch-data.img': inputs['seed273'],
             'EFI/UniXNU/firmware/PianoAudio/fs19xx.fsm': inputs['fsm'], 'EFI/UniXNU/firmware/Old/old.bin': inputs['old_fw']}
    image = out / f'{name}.img'
    make_esp(image, files)
    with tempfile.TemporaryDirectory() as scratch:
        (Path(scratch) / 's.img').write_bytes(inputs['seed273'])
        seed273 = common.seed_info(Path(scratch) / 's.img')
        (Path(scratch) / 's.img').write_bytes(inputs['seed'])
        seed = common.seed_info(Path(scratch) / 's.img')
    record = {'status': 'TEST', 'image': image.name, 'bytes': image.stat().st_size, 'sha256': common.sha(image), 'profile': 'daily',
              'default': 'macos-27.3', 'cmdlines': {'macos': old_line, 'macos-27.3': line}, 'ram_window': WINDOW13, 'seed': seed,
              'store': {'window_blocks': seed['store_blocks']}, 'files': {n: sha(b) for n, b in files.items()}, 'unixnu_toml': toml,
              'k273': {'id': 'macos-27.3', 'cmdline': line, 'ram_window': WINDOW13, 'seed': seed273,
                       'store': {'partition': 'linux_data', 'first_lba': STORE_LBA, 'blocks': seed273['store_blocks']}}}
    common.write_json(out / f'{name}.json', record)
    return record, inputs, seed273


def build(out, base, *extra, expect=0):
    r = subprocess.run([PY, str(BUILDER), '--base', str(out / f'{base}.json'), '--out', str(out), *extra], capture_output=True, text=True)
    if (r.returncode == 0) != (expect == 0):
        print((r.stdout + r.stderr)[-2000:])
    return r.returncode, r.stdout + r.stderr


# ---------------------------------------------------------------------------------------------------------------------------

def test_entry_uses_rootfs():
    print('== common.entry_uses_rootfs')
    x = lambda args: common.entry_uses_rootfs({'type': 'xnu', 'args': args})  # noqa: E731
    check(common.entry_uses_rootfs({'type': 'action', 'action': 'reset'}) is None, 'an action: no')
    check(x('native-ufs-root=1 data-persist=2') is not None, 'a UFS root without native-ufs-root-part: linux_rootfs')
    check(x('data-persist=2') is not None, 'a store without native-ufs-root-part: on linux_rootfs')
    check(x(f'native-ufs-root=1 native-ufs-root-part=linux_data data-store-lba={STORE_LBA} data-persist=2') is None, 'the 27.3 line: no')
    check(x('data-persist=0 piano-reset-after=1') is None, 'no root, no store: no')
    check(x('native-ufs-root=1 native-ufs-root-part=linux_rootfs') is not None, 'native-ufs-root-part=linux_rootfs: yes')


def test_builder(tmp):
    print('== build-k273-esp.py')
    out = tmp / 'out'
    out.mkdir()
    base, inputs, seed273 = make_dual(out, 'esp-dual', line_273())
    rc, log = build(out, 'esp-dual', '--expect-seed', seed273['identity_sha256'][:8], '--expect-store-lba', str(STORE_LBA),
                    '--expect-store-part', 'linux_data')
    check(rc == 0, f'builds from the dual ESP ({log.strip().splitlines()[-1][:100] if log.strip() else ""})')
    rec = json.loads((out / 'esp-k273-only.json').read_text())
    img = out / 'esp-k273-only.img'
    fat = common.fat32_read(img)
    files = fat['files']
    check(not fat['problems'] and common.sha(img) == rec['sha256'] and img.stat().st_size == common.ESP_BLOCKS * B,
          'the image: 1 GiB FAT32 that the independent reader reads, its record\'s sha256')
    want = {'EFI/UniXNU/Payloads/k273/kernel.macho': inputs['kc273'], 'EFI/UniXNU/Payloads/k273/kernel.macho.adt': inputs['adt273'],
            'EFI/UniXNU/Payloads/k273/kernel.macho.trustcache': inputs['trust273'],
            'EFI/UniXNU/Payloads/k273/scratch-data.img': inputs['seed273'], 'EFI/UniXNU/firmware/PianoAudio/fs19xx.fsm': inputs['fsm']}
    check(all(files.get(p) == b for p, b in want.items()), 'the 27.3 payload and its pinned firmware byte for byte')
    check(set(files) == set(want) | {'EFI/UniXNU/unixnu.toml'}, f'nothing else: no 27.0.1 payload, no unpinned firmware ({sorted(files)})')
    check(sorted(rec['dropped']) == sorted(p for p in base['files'] if p not in want and p != 'EFI/UniXNU/unixnu.toml'),
          'the record lists the dropped files')
    import tomllib
    menu = tomllib.loads(files['EFI/UniXNU/unixnu.toml'].decode())
    ids = [e['id'] for e in menu['entry']]
    k = next(e for e in menu['entry'] if e['id'] == 'macos-27.3')
    fb = next(e for e in menu['entry'] if e['id'] == 'fastboot')
    check(menu['boot']['default'] == 'macos-27.3' and ids == ['macos-27.3', 'fastboot', 'reset', 'shutdown', 'tools'],
          f'the menu: default macos-27.3, {ids} (no -diag: the line is {len(k["args"])} bytes)')
    check(k['args'] == base['k273']['cmdline'] and k['path'] == 'Payloads/k273/kernel.macho', 'the 27.3 entry\'s line and path byte for byte')
    fbargs = fb['args'].split()
    check(fb['path'] == k['path'] and fb['initrd'] == k['initrd'] and 'data-persist=0' in fbargs and 'piano-reset-after=1' in fbargs and
          'piano-reset-to=bootloader' in fbargs and not any(a.split('=')[0] in ('native-ufs-root', 'native-ufs-root-part', 'data-store-lba',
                                                                               'pstore-console') for a in fbargs) and
          [a for a in fbargs if a.startswith('piano-')] == ['piano-reset-after=1', 'piano-reset-to=bootloader'],
          'fastboot: the 27.3 payload, reset after 1 s, no UFS root, no store, no console mirror, no piano-* driver')
    check(not any(common.entry_uses_rootfs(e) for e in menu['entry']), 'no entry uses linux_rootfs')
    check(rec['kind'] == 'k273-only' and rec['seed'] == seed273 and rec['store']['partition'] == 'linux_data' and
          rec['store']['first_lba'] == STORE_LBA and rec['store']['blocks'] == seed273['store_blocks'] and rec['ram_window'] == WINDOW13
          and rec['default'] == 'macos-27.3' and 'window_blocks' not in rec['store'],
          'the record: the 27.3 seed, the store on linux_data at its LBA from the line, the 13 GiB window')
    first = rec['sha256']
    rc, _ = build(out, 'esp-dual')
    check(rc == 0 and json.loads((out / 'esp-k273-only.json').read_text())['sha256'] == first, 'the same inputs: the same image')
    # refusals
    rc, log = build(out, 'esp-dual', '--expect-seed', '00000000', '--name', 'x', expect=1)
    check(rc != 0 and 'not 00000000' in log, '--expect-seed of another seed: refused')
    rc, log = build(out, 'esp-dual', '--expect-store-lba', '1', '--name', 'x', expect=1)
    check(rc != 0 and 'not 1' in log, '--expect-store-lba of another LBA: refused')
    bad = dict(base, sha256='0' * 64)
    common.write_json(out / 'esp-bad.json', dict(bad, image='esp-dual.img'))
    rc, log = build(out, 'esp-bad', '--name', 'x', expect=1)
    check(rc != 0 and 'not the image its record' in log, 'a base image that is not its record\'s: refused')
    files_bad = dict(base['files'], **{'EFI/UniXNU/Payloads/k273/kernel.macho': '1' * 64})
    common.write_json(out / 'esp-bad.json', dict(base, files=files_bad))
    rc, log = build(out, 'esp-bad', '--name', 'x', expect=1)
    check(rc != 0 and 'differ from its record' in log, 'a base whose files are not its record\'s: refused')
    make_dual(out, 'esp-nolba', line_273(store='native-ufs-root-part=linux_data '), inputs)
    rc, log = build(out, 'esp-nolba', '--name', 'x', expect=1)
    check(rc != 0 and 'no bound read-write store' in log, 'a 27.3 line without data-store-lba: refused')
    make_dual(out, 'esp-otherlba', line_273(store=f'native-ufs-root-part=linux_data data-store-lba={STORE_LBA + 8} '), inputs)
    rc, log = build(out, 'esp-otherlba', '--name', 'x', expect=1)
    check(rc != 0 and f'needs data-store-lba={STORE_LBA}' in log, 'a store LBA that is not NativeUfs\'s place for the seed: refused')
    make_dual(out, 'esp-short', line_273(long=False), inputs)
    rc, _ = build(out, 'esp-short', '--name', 'esp-short-only')
    short = json.loads((out / 'esp-short-only.json').read_text()) if rc == 0 else {}
    check(rc == 0 and 'macos-27.3-diag' in short.get('cmdlines', {}) and 'piano-reset-after=240' in short['cmdlines']['macos-27.3-diag'],
          'a short line: the -diag entry is there (reset after 240 s)')
    for n in ('esp-bad', 'esp-nolba', 'esp-otherlba', 'esp-short', 'esp-short-only'):
        for suffix in ('.img', '.json'):
            (out / (n + suffix)).unlink(missing_ok=True)
    return out, base, inputs, seed273


def test_tablet_sh(tmp):
    print('== tablet.sh discard-info / discard / zero (a fake sysfs)')
    fs, dev = tmp / 'fs', tmp / 'dev'
    dev.mkdir()
    ufs = fs / 'sys/devices/platform/soc@0/1d84000.ufshc/host0'
    for disk, lun in (('sda', 0), ('sde', 4)):
        target = ufs / f'target0:0:0/0:0:0:{lun}'
        target.mkdir(parents=True)
        (fs / f'sys/block/{disk}/queue').mkdir(parents=True)
        (fs / f'sys/block/{disk}/queue/logical_block_size').write_text('4096\n')
        (fs / f'sys/block/{disk}/queue/discard_max_bytes').write_text('2147483648\n')
        (fs / f'sys/block/{disk}/queue/discard_granularity').write_text('4096\n')
        os.symlink(target, fs / f'sys/block/{disk}/device')
    (fs / 'sys/class/block').mkdir(parents=True)
    for node, name, start, size in (('sda35', 'linux_rootfs', 448487424, 4096), ('sda36', 'linux_data', 574316544, 4096),
                                    ('sde13', 'boot_a', 618800, 4096)):
        d = fs / 'sys/class/block' / node
        d.mkdir()
        (d / 'partition').write_text(node.lstrip('sdae') + '\n')
        (d / 'uevent').write_text(f'MAJOR=8\nDEVTYPE=partition\nPARTNAME={name}\n')
        (d / 'start').write_text(f'{start}\n')
        (d / 'size').write_text(f'{size}\n')
        (dev / node).write_bytes(os.urandom(size * 512 + 4096))
    (fs / 'sys/class/block/sda').mkdir()
    (fs / 'sys/class/block/sda/uevent').write_text('DEVTYPE=disk\n')
    (dev / 'sda').write_bytes(os.urandom(8192))
    script = tmp / 'tablet-fakesys.sh'
    script.write_text((HERE / 'tablet.sh').read_text().replace('/sys/', f'{fs}/sys/'))
    env = dict(os.environ, PIANO_PP_DIR=str(tmp / 'pp'), PIANO_DEV_DIR=str(dev), PIANO_HOST_IP='127.0.0.1')

    def run(*argv):
        r = subprocess.run(['sh', str(script), *map(str, argv)], capture_output=True, text=True, env=env)
        return dict(line.split('=', 1) for line in r.stdout.splitlines() if '=' in line)

    tail = (dev / 'sda35').read_bytes()[4096 * 512:]
    got = run('zero', 'sda35', 'linux_rootfs', 448487424, 4096)
    data = (dev / 'sda35').read_bytes()
    check(got.get('zeroed') == 'sda35 bytes=2097152' and data[:2097152] == bytes(2097152) and data[2097152:] == tail,
          f'zero: exactly the partition\'s bytes ({got})')
    before = {n: (dev / n).read_bytes() for n in ('sda', 'sda36', 'sde13')}
    for argv, why in ((('sda36', 'linux_rootfs', 574316544, 4096), 'named \'linux_data\''),
                      (('sda', 'linux_rootfs', 0, 16), 'not a partition'),
                      (('sde13', 'boot_a', 618800, 4096), 'not a partition of LUN 0'),
                      (('sda35', 'linux_rootfs', 448487432, 4096), 'starts at'),
                      (('sda35', 'linux_rootfs', 448487424, 4104), 'sectors, not'),
                      (('../sda35', 'linux_rootfs', 448487424, 4096), 'node')):
        for cmd in ('zero', 'discard'):
            got = run(cmd, *argv)
            check(why in got.get('error', ''), f'{cmd} {argv[0]} {argv[1]} {argv[2]}+{argv[3]}: refused ({got.get("error")})')
    check(all((dev / n).read_bytes() == b for n, b in before.items()), 'the refused ones wrote nothing')
    got = run('discard-info', 'sda35')
    check(got.get('discard_max_bytes') == '2147483648' and got.get('discard') in ('0', '1') and 'tool' in got, f'discard-info: {got}')


# ---------------------------------------------------------------------------------------------------------------------------
# the fake tablet

def make_gpt(parts):
    """A GPT of LUN 0 with PARTS [(index, name, first, last)]: (head 6 blocks, tail 5 blocks), CRCs right."""
    entries = bytearray(64 * 128)
    for index, name, first, last in parts:
        struct.pack_into('<16s16sQQQ', entries, index * 128, os.urandom(16), os.urandom(16), first, last, 0)
        entries[index * 128 + 56:index * 128 + 56 + 2 * len(name)] = name.encode('utf-16le')
    crc = zlib.crc32(entries)
    disk = os.urandom(16)

    def header(current, backup, at):
        h = bytearray(92)
        struct.pack_into('<8sIIIIQQQQ16sQIII', h, 0, b'EFI PART', 0x00010000, 92, 0, 0, current, backup, 6, common.DATA_LAST, disk, at,
                         64, 128, crc)
        struct.pack_into('<I', h, 16, zlib.crc32(h))
        return bytes(h).ljust(B, b'\0')

    mbr = bytearray(B)
    mbr[510:512] = b'\x55\xaa'
    last = common.LUN0_BLOCKS - 1
    head = bytes(mbr) + header(1, last, 2) + bytes(entries) + bytes(2 * B)
    tail = bytes(entries) + bytes(2 * B) + header(last, 1, last - 4)
    return head, tail


def store_block(identity, pages, generation, bound=None, ext=True):
    raw = bytearray(B)
    raw[:8] = b'SHNGDATA'
    struct.pack_into('<IIQ', raw, 8, 1, 64, pages)
    raw[24:56] = identity
    struct.pack_into('<I', raw, 56, generation)
    struct.pack_into('<I', raw, 60, zlib.crc32(bytes(raw[:60])))
    if ext:
        raw[64:72] = b'SHNGEXT1'
        struct.pack_into('<IIQQ', raw, 72, common.BOUND if bound is not None else 0, 0, 0, bound or 0)
        struct.pack_into('<I', raw, 124, zlib.crc32(bytes(raw[64:124]), zlib.crc32(bytes(raw[:60]))))
    return bytes(raw)


def copy_sparse(src, dst, offset):
    """SRC into the sparse file DST at OFFSET, skipping its all-zero MiB (DST holds zeros there)."""
    with open(src, 'rb') as f, open(dst, 'r+b') as g:
        at = 0
        while chunk := f.read(MiB):
            if any(chunk):
                g.seek(offset + at)
                g.write(chunk)
            at += len(chunk)


def build_tablet(tmp, out, dual, seed273):
    root = tmp / 'tablet'
    root.mkdir()
    lun0 = root / 'lun0.img'
    with open(lun0, 'wb') as f:
        f.truncate(common.LUN0_BLOCKS * B)
    head, tail = make_gpt([(34, 'linux_rootfs', ROOTFS_FIRST, ROOTFS_FIRST + ROOTFS_BLOCKS - 1),
                           (35, 'linux_data', common.DATA_FIRST, common.DATA_LAST),
                           (36, 'esp', common.ESP_FIRST, common.ESP_FIRST + common.ESP_BLOCKS - 1)])
    tp.write_at(lun0, 0, head)
    tp.write_at(lun0, (common.LUN0_BLOCKS - 5) * B, tail)
    (out / 'gpt-head.bin').write_bytes(head)
    (out / 'gpt-tail.bin').write_bytes(tail)
    common.write_json(out / 'gpt.json', {'gpt-head.bin': {'sha256': sha(head)}, 'gpt-tail.bin': {'sha256': sha(tail)},
                                         'esp_guid': '3F8B0988-0000-0000-0000-000000000000'})
    # linux_rootfs: the 27.0.1 root's manifest, random contents, its store's header (unbound) at today's place
    tp.write_at(lun0, ROOTFS_FIRST * B, os.urandom(ROOTFS_BLOCKS * B))
    manifest = bytearray(B)
    manifest[:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', manifest, 8, 1, 2, 31 * 10 ** 9 // B * B)
    tp.write_at(lun0, ROOTFS_FIRST * B, bytes(manifest))
    tp.write_at(lun0, 68119455 * B, store_block(bytes.fromhex(dual['seed']['identity_sha256']), dual['seed']['pages'], 5))
    # linux_data: the 27.3 root's manifest and the 27.3 store bound to its LBA
    manifest = bytearray(B)
    manifest[:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', manifest, 8, 1, 2, ROOT_IMAGE_BYTES)
    tp.write_at(lun0, common.DATA_FIRST * B, bytes(manifest) + os.urandom(B))
    tp.write_at(lun0, STORE_LBA * B, store_block(bytes.fromhex(seed273['identity_sha256']), seed273['pages'], 3, bound=STORE_LBA) +
                os.urandom(4 * B))
    copy_sparse(out / dual['image'], lun0, common.ESP_FIRST * B)
    # LUN 4: boot_a = the v5 stand-in (13 GiB DarwinBoot), the other three their own markers; all sparse
    lun4 = root / 'lun4.img'
    with open(lun4, 'wb') as f:
        f.truncate(1 << 30)
    boot5 = out / 'boot-test5-signed.img'
    with open(boot5, 'wb') as f:
        f.truncate(common.BOOT_PARTITION_BYTES)
        f.write(b'ANDROID!' + b'v5 stand-in')
    boot4 = out / 'boot-test4-signed.img'
    with open(boot4, 'wb') as f:
        f.truncate(common.BOOT_PARTITION_BYTES)
        f.write(b'ANDROID!' + b'v4 stand-in')
    common.write_json(out / 'boot-v5.json', {'series': 5, 'fd': {'ram_window': common.DARWIN_WINDOW_MAX},
                                             'signed': {boot5.name: {'sha256': common.sha(boot5)}}})
    common.write_json(out / 'boot.json', {'signed': {boot4.name: {'sha256': common.sha(boot4)}}})
    for name, (_, off, size) in tp.LUN4.items():
        tp.write_at(lun4, off, b'ANDROID!v5 stand-in' if name == 'boot_a' else f'{name} of slot A'.encode())
    return root


def snapshot(root, out):
    lun0, lun4 = root / 'lun0.img', root / 'lun4.img'
    return {'GPT head': tp.sha_region(lun0, 0, 6 * B), 'GPT tail': tp.sha_region(lun0, (common.LUN0_BLOCKS - 5) * B, 5 * B),
            'linux_rootfs': tp.sha_region(lun0, ROOTFS_FIRST * B, ROOTFS_BLOCKS * B),
            'ESP': tp.sha_region(lun0, common.ESP_FIRST * B, common.ESP_BLOCKS * B),
            'linux_data block 0': tp.sha_region(lun0, common.DATA_FIRST * B, B),
            'the 27.3 store window head': tp.sha_region(lun0, STORE_LBA * B, 5 * B),
            'boot_a': tp.sha_region(lun4, tp.LUN4['boot_a'][1], tp.LUN4['boot_a'][2]),
            'boot_b': tp.sha_region(lun4, tp.LUN4['boot_b'][1], tp.LUN4['boot_b'][2])}


def test_session(tmp, out, dual, seed273):
    print('== maint-session.py: the 27.3-only ESP and linux_rootfs (a fake tablet)')
    root = build_tablet(tmp, out, dual, seed273)
    lun0, lun4 = root / 'lun0.img', root / 'lun4.img'
    nas, home = tmp / 'nas', tmp / 'home'
    nas.mkdir()
    home.mkdir()
    (tmp / 'fake-shell.py').write_text(f'import sys; sys.path.insert(0, {str(HERE)!r}); import importlib.util\n'
                                       f'spec = importlib.util.spec_from_file_location("t", {str(HERE / "test-persist.py")!r})\n'
                                       'm = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)\n'
                                       'sys.stdout.write(m.fake_shell(sys.argv[1]))\n')
    # maint-session.py with slot A's vendor_boot_a / init_boot_a of this fake tablet in place of Ubuntu's (common's constants)
    (tmp / 'maint-wrap.py').write_text(f'import os, runpy, sys\nsys.path.insert(0, {str(HERE)!r})\nimport common\n'
                                       'common.VENDOR_BOOT_A_SHA256 = os.environ["FAKE_VENDOR_BOOT_A_SHA256"]\n'
                                       'common.INIT_BOOT_A_SHA256 = os.environ["FAKE_INIT_BOOT_A_SHA256"]\n'
                                       f'sys.argv[0] = {str(MAINT)!r}\nrunpy.run_path({str(MAINT)!r}, run_name="__main__")\n')
    env = dict(os.environ, FAKE_ROOT=str(root), PIANO_PERSIST_OUT=str(out), PIANO_MAINT_HOST='127.0.0.1', PIANO_BACKUP_DIR=str(nas),
               PIANO_SERIAL='test', PIANO_MAINT_SHELL=str(tmp / 'fake-shell.py'), PYTHONDONTWRITEBYTECODE='1', HOME=str(home),
               PIANO_JOB_POLL='0.2',
               FAKE_VENDOR_BOOT_A_SHA256=tp.sha_region(lun4, tp.LUN4['vendor_boot_a'][1], tp.LUN4['vendor_boot_a'][2]),
               FAKE_INIT_BOOT_A_SHA256=tp.sha_region(lun4, tp.LUN4['init_boot_a'][1], tp.LUN4['init_boot_a'][2]))
    for k in [k for k in env if k.startswith('PIANO_') and k not in ('PIANO_PERSIST_OUT', 'PIANO_MAINT_HOST', 'PIANO_BACKUP_DIR',
                                                                       'PIANO_SERIAL', 'PIANO_MAINT_SHELL', 'PIANO_JOB_POLL')]:
        env.pop(k)
    E = 'esp-k273-only.img'
    esp = ['--esp', E, '--esp-final', E]
    base = [PY, str(tmp / 'maint-wrap.py'), '--session', str(tmp / 'session'), '--http-port', '18020', '--nc-port', '19020',
            '--rootfs-piece-mib', '32']
    state_path = tmp / 'session/state.json'

    def run(*steps, expect=0, **extra):
        r = subprocess.run(base + list(steps), capture_output=True, text=True, env=dict(env, **extra), timeout=3600)
        log = r.stdout + r.stderr
        with open(tmp / 'session-out.txt', 'a') as f:
            f.write(log)
        if r.returncode != expect:
            print(log[-4000:])
        check(r.returncode == expect, f'maint-session.py {" ".join(steps)}{" " + str(extra) if extra else ""} -> {r.returncode}')
        return log

    def state():
        return json.loads(state_path.read_text())

    def esp_region():
        return tp.sha_region(lun0, common.ESP_FIRST * B, common.ESP_BLOCKS * B)

    rec = json.loads((out / 'esp-k273-only.json').read_text())
    start = snapshot(root, out)
    rootfs0 = tp.read(lun0, ROOTFS_FIRST * B, ROOTFS_BLOCKS * B)
    line = f'the store at LBA {STORE_LBA} on linux_data (seed {seed273["identity_sha256"][:16]}'
    log = run('map', 'backup', *esp, '--new-backup')
    check(line in log and 'IS the one this ESP resumes' in log, 'map: "the store at LBA 117998394 on linux_data ... IS the one this ESP resumes"')
    check('is NOT the one' not in log and 'no entry of esp-k273-only.img uses it' in log and 'linux_data block 0 is the SHNGROOT manifest' in log,
          'map: linux_rootfs only reported, the 27.3 root\'s manifest checked')
    log = run('--dry-run', 'map', 'backup', 'esp-final', 'check', 'maint-exit', *esp)
    check('would stop a real run' not in log and 'done (dry run' in log, 'dry run of the install session: clean')
    log = run('--dry-run', 'map', 'rootfs-backup', 'rootfs-clear', 'check', *esp)
    check('would stop a real run' not in log and 'done (dry run' in log, 'dry run of the rootfs session: clean')

    # -- the gate: esp-final refuses any other store, and the pair rule holds --------------------------------------------------
    hdr = STORE_LBA * B
    good = tp.read(lun0, hdr, B)
    other = os.urandom(32)
    for what, block, why in (
            ('bound to another LBA', store_block(bytes.fromhex(seed273['identity_sha256']), seed273['pages'], 3, bound=STORE_LBA + 1),
             f'bound to {STORE_LBA + 1}'),
            ('of another seed', store_block(other, seed273['pages'], 3, bound=STORE_LBA), 'of seed'),
            ('unbound', store_block(bytes.fromhex(seed273['identity_sha256']), seed273['pages'], 3), 'unbound'),
            ('blank', bytes(B), 'header is blank')):
        tp.write_at(lun0, hdr, block)
        log = run('map', 'esp-final', *esp, expect=3)
        check('is NOT the one this ESP resumes' in log and why in log and esp_region() == start['ESP'],
              f'esp-final refuses the 27.3-only ESP over a store {what}; the ESP untouched')
    tp.write_at(lun0, hdr, good)
    common.write_json(out / 'esp-k273-off.json', dict(rec, image='esp-k273-off.img', sha256='ab' * 32,
                                                      store=dict(rec['store'], first_lba=STORE_LBA + 8)))
    log = run('map', 'esp-final', '--esp', 'esp-k273-off.img', '--esp-final', 'esp-k273-off.img', expect=3)
    check('NativeUfs puts a' in log and esp_region() == start['ESP'], 'esp-final refuses a record whose window is not NativeUfs\'s place')
    (out / 'esp-k273-off.json').unlink()
    tp.write_at(lun4, tp.LUN4['boot_a'][1], b'ANDROID!v4 stand-in')
    log = run('map', 'esp-final', *esp, '--boot-image', 'boot-test4-signed.img', expect=3)
    check('that pair never boots' in log and esp_region() == start['ESP'], 'esp-final refuses the 13 GiB ESP over an 8 GiB FD (the pair rule)')
    tp.write_at(lun4, tp.LUN4['boot_a'][1], b'ANDROID!v5 stand-in')

    # -- install, and the rollback ------------------------------------------------------------------------------------------
    log = run('map', 'esp-final', 'check', *esp)
    installed = json.loads((out / 'installed.json').read_text())
    check(esp_region() == rec['sha256'] and installed['seed']['identity_sha256'] == seed273['identity_sha256'] and
          'IS the one this ESP resumes' in log.split('step check')[-1],
          'esp-final writes the 27.3-only ESP; installed.json has the 27.3 seed; check reads the bound store')

    # -- rootfs-backup -------------------------------------------------------------------------------------------------------
    log = run('map', 'rootfs-clear', *esp, expect=3)
    check('no verified backup of linux_rootfs' in log and tp.read(lun0, ROOTFS_FIRST * B, ROOTFS_BLOCKS * B) == rootfs0,
          'rootfs-clear without a backup: refused, nothing written')
    log = run('rootfs-backup', *esp, expect=3, FAKE_SEND_FAIL='always:96')
    prog = state().get('rootfs_backup_progress') or {}
    check('piece 3 of linux_rootfs failed 3 times' in log and sorted(prog.get('pieces', {})) == ['0', '1', '2'],
          'a piece whose stream always breaks: stopped after 3 tries, pieces 0-2 kept as verified')
    log = run('rootfs-backup', *esp, FAKE_SEND_FAIL='once:128')
    st = state()
    entry = st['backups'].get('linux_rootfs') or {}
    check(log.count('verified in an earlier run') == 3 and 'piece 4, try 1' in log and entry.get('verified'),
          'run again: resumes at piece 3, piece 4\'s broken stream retried, the backup verified')
    folder = nas / st['backup_name']
    inflated = b''.join(gzip.decompress((folder / q['file']).read_bytes()) for q in entry.get('pieces', []))
    sums = (folder / 'SHA256SUMS').read_text()
    check(inflated == rootfs0 and entry.get('sha256') == sha(rootfs0) and len(entry.get('pieces', [])) == 5 and
          all(f'{q["stored_sha256"]}  {q["file"]}' in sums and common.sha(folder / q['file']) == q['stored_sha256'] for q in entry['pieces']),
          'the stored pieces inflate to linux_rootfs byte for byte; SHA256SUMS has each stored piece')
    log = run('rootfs-backup', *esp)
    check('already backed up and verified' in log, 'rootfs-backup again: already backed up (the partition hashed again)')

    # -- rootfs-clear's refusals ---------------------------------------------------------------------------------------------
    def refused(what, why):
        log = run('map', 'rootfs-clear', *esp, expect=3)
        check(why in log and tp.sha_region(lun0, ROOTFS_FIRST * B, ROOTFS_BLOCKS * B) == start['linux_rootfs'],
              f'rootfs-clear refuses {what}; linux_rootfs untouched')

    piece = folder / entry['pieces'][2]['file']
    keep = piece.read_bytes()
    piece.write_bytes(keep[:100] + bytes([keep[100] ^ 1]) + keep[101:])
    refused('a stored piece that does not hash to its record', 'FAIL  every stored piece')
    piece.write_bytes(keep)
    tp.write_at(lun0, (ROOTFS_FIRST + 7) * B, b'changed')
    log = run('map', 'rootfs-clear', *esp, expect=3)
    check('FAIL  linux_rootfs read now = its backup' in log, 'rootfs-clear refuses a linux_rootfs that changed since its backup')
    tp.write_at(lun0, ROOTFS_FIRST * B, rootfs0[:8 * B])
    saved = state_path.read_text()
    st = state()
    st['backups']['linux_rootfs']['first_lba'] += 1
    state_path.write_text(json.dumps(st))
    refused('a backup of another geometry', 'FAIL  the backup is of linux_rootfs as mapped')
    state_path.write_text(saved)
    rec_text = (out / 'esp-k273-only.json').read_text()
    common.write_json(out / 'esp-k273-only.json', dict(rec, unixnu_toml=rec['unixnu_toml'] + '# changed\n'))
    refused('an ESP record whose menu is not the image\'s', 'its unixnu.toml is not its record\'s')
    (out / 'esp-k273-only.json').write_text(rec_text)
    keep_esp = tp.read(lun0, (common.ESP_FIRST + 1000) * B, 16)
    tp.write_at(lun0, (common.ESP_FIRST + 1000) * B, b'not an ESP image')
    refused('an ESP of no build record', 'is the image of a build record')
    tp.write_at(lun0, (common.ESP_FIRST + 1000) * B, keep_esp)
    run('map', *esp)
    run('rollback', 'esp-final', *esp)
    installed = json.loads((out / 'installed.json').read_text())
    check(esp_region() == start['ESP'] and installed['image'] == dual['image'] and
          installed['seed']['identity_sha256'] == dual['seed']['identity_sha256'],
          'rollback esp-final: the dual ESP again, installed.json its record (its seed)')
    refused('the dual ESP on the tablet (entries on linux_rootfs)', 'FAIL  esp-dual.img is a 27.3-only ESP')
    run('map', 'esp-final', *esp)
    check(esp_region() == rec['sha256'], 'esp-final again: the 27.3-only ESP')

    # -- rootfs-clear: a device that keeps data after a discard (zeros), then check and map -----------------------------------
    before = snapshot(root, out)
    log = run('map', 'rootfs-clear', 'check', *esp, FAKE_DISCARD_KEEPS='1')
    after = snapshot(root, out)
    zero = tp.sha_region(lun0, ROOTFS_FIRST * B, ROOTFS_BLOCKS * B) == sha(bytes(ROOTFS_BLOCKS * B))
    check(zero and 'zeros instead' in log and state()['rootfs_cleared']['method'] == 'discard+zero',
          'rootfs-clear: the discard left data (this device), so zeros: linux_rootfs reads zero')
    check(all(after[k] == before[k] for k in before if k != 'linux_rootfs'),
          'the GPT, the ESP, linux_data\'s manifest, the 27.3 store, boot_a/boot_b unchanged')
    check('ok    linux_rootfs reads zero from the medium: LBA 68110000+16384, LBA 68119455+16384, LBA 68134576+16384' in log,
          'the regions read back: first and last 64 MiB, 64 MiB from the old store header place LBA 68119455')
    log = run('map', 'check', *esp)
    check('linux_rootfs block 0: zero (cleared)' in log and 'IS the one this ESP resumes' in log, 'map and check after the clear: clean')
    dual_text = (out / 'esp-dual.json').read_text()
    common.write_json(out / 'esp-dual.json', dict(dual, default='macos'))
    log = run('rollback', 'esp-final', *esp, expect=3)
    check('holds no root image now: rollback rootfs-clear first' in log and esp_region() == rec['sha256'],
          'rollback esp-final to an ESP whose default boots linux_rootfs: refused while it is cleared')
    (out / 'esp-dual.json').write_text(dual_text)

    # -- rollback rootfs-clear, resumed; the plain discard; everything back -----------------------------------------------------
    log = run('rollback', 'rootfs-clear', *esp)
    check(tp.read(lun0, ROOTFS_FIRST * B, ROOTFS_BLOCKS * B) == rootfs0 and state().get('rootfs_cleared') is None,
          'rollback rootfs-clear: linux_rootfs byte for byte as backed up')
    log = run('rollback', 'rootfs-clear', *esp)
    check(log.count('already as backed up') == 5, 'rollback rootfs-clear again: every piece already as backed up (resumes)')
    log = run('map', 'rootfs-clear', *esp)
    check(tp.sha_region(lun0, ROOTFS_FIRST * B, ROOTFS_BLOCKS * B) == sha(bytes(ROOTFS_BLOCKS * B)) and
          state()['rootfs_cleared']['method'] == 'discard' and 'zeros instead' not in log, 'rootfs-clear: the discard')
    run('rollback', 'rootfs-clear', *esp)
    run('map', *esp)
    run('rollback', 'esp-final', *esp)
    end = snapshot(root, out)
    for key in start:
        check(end[key] == start[key], f'at the end: {key} as at the start')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--keep', type=Path)
    a = p.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix='piano-k273-test-')) if not a.keep else a.keep
    tmp.mkdir(parents=True, exist_ok=True)
    try:
        test_entry_uses_rootfs()
        out, dual, inputs, seed273 = test_builder(tmp)
        (tmp / 'sh').mkdir()
        test_tablet_sh(tmp / 'sh')
        test_session(tmp, out, dual, seed273)
    finally:
        if not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
