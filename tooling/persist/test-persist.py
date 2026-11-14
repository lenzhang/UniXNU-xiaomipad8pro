#!/usr/bin/env python3
"""Host-only tests of tooling/persist (no tablet, no fastboot, no NAS):

1. tablet.sh's read/write subcommands on a scratch file (hash, peek, zero-sha, copy, write, fetch, send, restore), against Python.
2. maint-session.py's whole write path against a FAKE TABLET: a sparse 510 GB file as LUN 0 holding the real current GPT
   (CRITICAL-BACKUP new-primary/new-backup.bin), a root manifest and a Data store bound to a test seed (window shrunk to 2049
   blocks), a sparse LUN 4 with boot_a/vendor_boot_a/init_boot_a/boot_b, a temporary "NAS" directory, and a stand-in for
   maint-shell.py that runs tablet.sh's subcommands in Python over those files (partitions as offset views of the disks).
   Runs map, backup, gpt, store (move), esp, check, boot-a, check, then the rollbacks esp, store, gpt, boot-a, and checks that
   every region is byte for byte what it was. Also checks that a changed-but-not-written region stops the session, and that once
   installed, esp-final refuses an ESP whose seed is not the store's (docs/data-store.md section 9) unless its record says
   --replace-data, and writes one of the store's seed.
  test-persist.py [--keep DIR]
"""
import argparse
import hashlib
import http.client
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import common  # noqa: E402

B = common.LBA
MiB = 1 << 20
LUN4 = {'boot_a': ('sde13', 316825600, 100663296), 'vendor_boot_a': ('sde23', 497049600, 100663296),
        'init_boot_a': ('sde28', 736387072, 8388608), 'boot_b': ('sdf13', 838860800, 100663296)}
PAGES = 2047                                   # test seed: 2047 pages -> W = 1 + 1 + 2047 = 2049 blocks
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)


# ---------------------------------------------------------------------------------------------------------------------------
# The fake tablet (this file is also the maint-shell stand-in: test-persist.py --fake-shell COMMAND TIMEOUT)

def fake_nodes(root):
    lun0 = root / 'lun0.img'
    nodes = {'sda': (lun0, 0, common.LUN0_BLOCKS * B), 'sde': (root / 'lun4.img', 0, 1 << 30), 'sdf': (root / 'lun4.img', 0, 1 << 30)}
    with open(lun0, 'rb') as f:
        f.seek(2 * B)
        entries = f.read(64 * 128)
    for i in range(64):
        e = entries[i * 128:(i + 1) * 128]
        if any(e[:16]):
            first, last = struct.unpack_from('<QQ', e, 32)
            nodes[f'sda{i + 1}'] = (lun0, first * B, (last - first + 1) * B)
    for name, (node, off, size) in LUN4.items():
        nodes[node] = (root / 'lun4.img', off, size)
    return nodes


def fake_shell(command):
    root = Path(os.environ['FAKE_ROOT'])
    pp = root / 'pp'
    pp.mkdir(exist_ok=True)
    out = []
    for part in [c.strip() for c in command.split(';')]:
        if not part or part.startswith('stty') or part == 'sync':
            continue
        argv = part.split()
        if argv[0] == 'sh' and argv[1] == '/run/pp/tablet.sh':
            out.append(fake_tablet(root, pp, argv[2:]))
        elif argv[0] == 'piano-maint':
            out.append(f'piano-maint {" ".join(argv[1:])}: ok')
        elif argv[:2] == ['sgdisk', '-v']:
            out.append('No problems found. 0 free sectors (0 bytes) available in 0\nsegments, the largest of which is' if gpt_ok(root) else 'Problem: CRC')
        elif argv[:2] == ['sgdisk', '-p']:
            out.append('\n'.join(f'{k} {v[1] // B}-{(v[1] + v[2]) // B - 1}' for k, v in fake_nodes(root).items() if k.startswith('sda3')))
        else:
            out.append(f'fake shell: {part}')
    return '\n'.join(out) + '\n'


def gpt_ok(root):
    with open(root / 'lun0.img', 'rb') as f:
        head = f.read(6 * B)
    h = bytearray(head[B:B + 92])
    crc = struct.unpack_from('<I', h, 16)[0]
    h[16:20] = bytes(4)
    return zlib.crc32(h) & 0xffffffff == crc and zlib.crc32(head[2 * B:2 * B + 8192]) & 0xffffffff == struct.unpack_from('<I', head, B + 88)[0]


def region(nodes, dev, skip, count, bs):
    path, off, size = nodes[dev]
    start, length = off + skip * bs, count * bs
    if skip * bs + length > size:
        raise ValueError(f'{dev}: beyond the end')
    return path, start, length


def read(path, start, length):
    with open(path, 'rb') as f:
        f.seek(start)
        return f.read(length)


def sha_region(path, start, length):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        f.seek(start)
        left = length
        while left:
            chunk = f.read(min(8 * MiB, left))
            h.update(chunk)
            left -= len(chunk)
    return h.hexdigest()


def write_at(path, start, data):
    with open(path, 'r+b') as f:
        f.seek(start)
        f.write(data)


def http_get(port, name):
    c = http.client.HTTPConnection('127.0.0.1', int(port), timeout=60)
    c.request('GET', '/' + name)
    r = c.getresponse()
    if r.status != 200:
        raise ValueError(f'GET {name}: {r.status}')
    return r.read()


def fake_tablet(root, pp, argv):
    nodes = fake_nodes(root)
    cmd, a = argv[0], argv[1:]
    try:
        if cmd == 'map':
            def show(n):
                if n not in nodes or n == 'sda':
                    return 'none'
                return f'{n} {nodes[n][1] // 512 if n.startswith("sda") else nodes[n][1] // 512} {nodes[n][2] // 512}'
            lines = ['lun0=sda', 'lun0_block_size=4096', f'lun0_bytes={common.LUN0_BLOCKS * B}', 'mounted=0']
            for name, node in (('boot_a', 'sde13'), ('boot_b', 'sdf13'), ('vendor_boot_a', 'sde23'), ('init_boot_a', 'sde28'),
                               ('linux_rootfs', 'sda35'), ('linux_data', 'sda36'), ('esp', 'sda37')):
                lines.append(f'{name}={show(node)}')
            return '\n'.join(lines)
        if cmd == 'hash':
            bs = int(a[3]) if len(a) > 3 else B
            return 'sha256=' + sha_region(*region(nodes, a[0], int(a[1]), int(a[2]), bs))
        if cmd == 'zero-sha':
            bs = int(a[1]) if len(a) > 1 else B
            h = hashlib.sha256()
            for _ in range(int(a[0])):
                h.update(bytes(bs))
            return 'sha256=' + h.hexdigest()
        if cmd == 'peek':
            path, start, _ = region(nodes, a[0], int(a[1]), 1, B)
            return 'hex=' + read(path, start + int(a[2]), int(a[3])).hex()
        if cmd == 'send':
            path, start, length = region(nodes, a[0], int(a[1]), int(a[2]), int(a[3]))
            s = socket.create_connection(('127.0.0.1', int(a[4])))
            # FAKE_SEND_FAIL=once:SKIP|always:SKIP: the stream of the piece at block SKIP breaks halfway (a flaky USB link)
            how, _, at = os.environ.get('FAKE_SEND_FAIL', '').partition(':')
            marker = root / f'send-failed-{at}'
            if at == a[1] and (how == 'always' or (how == 'once' and not marker.exists())):
                marker.touch()
                length //= 2
            with open(path, 'rb') as f:
                f.seek(start)
                left = length
                while left:
                    chunk = f.read(min(MiB, left))
                    s.sendall(chunk)
                    left -= len(chunk)
            s.close()
            return 'sent=0'
        if cmd == 'fetch':
            data = http_get(a[2], a[0])
            got = hashlib.sha256(data).hexdigest()
            if got != a[1]:
                return f'error={a[0]} sha256 {got}, expected {a[1]}'
            (pp / a[0]).write_bytes(data)
            return f'fetched={a[0]} sha256={got} bytes={len(data)}'
        if cmd == 'copy':
            path, src, length = region(nodes, a[0], int(a[1]), int(a[3]), B)
            _, dst, _ = region(nodes, a[0], int(a[2]), int(a[3]), B)
            for off in range(0, length, 256 * B):   # forward, chunk read before written, as dd
                n = min(256 * B, length - off)
                write_at(path, dst + off, read(path, src + off, n))
            return f'copied={a[3]}'
        if cmd == 'write':
            data = (pp / a[0]).read_bytes()
            path, start, _ = region(nodes, a[1], int(a[2]), len(data) // int(a[3]) or 1, int(a[3]))
            write_at(path, start, data)
            return f'written={a[0]}'
        if cmd == 'restore':
            data = http_get(a[4], a[0])
            path, start, _ = region(nodes, a[1], int(a[2]), len(data) // int(a[3]), int(a[3]))
            write_at(path, start, data)
            return f'restored={a[0]}'
        if cmd == 'job':
            text = fake_tablet(root, pp, a[1:])
            (pp / f'{a[0]}.log').write_text(text + '\n')
            (pp / f'{a[0]}.rc').write_text('1\n' if text.startswith('error=') else '0\n')
            return f'started={a[0]}'
        if cmd == 'job-status':
            if (pp / f'{a[0]}.rc').is_file():
                return 'rc=' + (pp / f'{a[0]}.rc').read_text().strip() + '\n' + (pp / f'{a[0]}.log').read_text()
            return 'running=1'
        if cmd == 'drop-caches':
            return 'dropped=1'
        if cmd == 'rereadpt':
            return 'reread=partx'
        if cmd == 'discard-info':
            on = os.environ.get('FAKE_NO_DISCARD') != '1'
            return f'discard_max_bytes={4 << 30 if on else 0}\ndiscard_granularity=4096\ntool=busybox-blkdiscard\ndiscard={int(on)}'
        if cmd in ('discard', 'zero'):
            # tablet.sh part_guard: a LUN 0 partition with this name, start and size (512-byte units), never the disk
            names = {'sda35': 'linux_rootfs', 'sda36': 'linux_data', 'sda37': 'esp'}
            node, name, start, size = a[0], a[1], int(a[2]), int(a[3])
            if names.get(node) != name or node not in nodes or (nodes[node][1] // 512, nodes[node][2] // 512) != (start, size):
                return f'error={node} is not the partition {name} at {start}+{size}'
            path, off, length = nodes[node]
            if cmd == 'discard' and os.environ.get('FAKE_DISCARD_KEEPS') == '1':
                return f'discarded={node} bytes={length} tool=busybox-blkdiscard'   # a device that reads old data after unmap
            for at in range(0, length, 4 * MiB):
                write_at(path, off + at, bytes(min(4 * MiB, length - at)))
            return f'{"discarded" if cmd == "discard" else "zeroed"}={node} bytes={length}'
        return f'error=unknown {cmd}'
    except Exception as e:  # noqa: BLE001
        return f'error={e}'


# ---------------------------------------------------------------------------------------------------------------------------

def test_tablet_sh(tmp):
    print('== tablet.sh on a scratch file')
    disk = tmp / 'disk.bin'
    disk.write_bytes(os.urandom(64 * B))
    env = dict(os.environ, PIANO_PP_DIR=str(tmp / 'pp'), PIANO_HOST_IP='127.0.0.1')
    dev = '../' + str(disk).lstrip('/')    # /dev/../<path>

    def run(*argv):
        r = subprocess.run(['sh', str(HERE / 'tablet.sh'), *map(str, argv)], capture_output=True, text=True, env=env, cwd=tmp)
        return dict(line.split('=', 1) for line in r.stdout.splitlines() if '=' in line)

    data = disk.read_bytes()
    check(run('hash', dev, 2, 3)['sha256'] == hashlib.sha256(data[2 * B:5 * B]).hexdigest(), 'hash DEV SKIP COUNT')
    check(run('hash', dev, 1, 2, 8192)['sha256'] == hashlib.sha256(data[8192:3 * 8192]).hexdigest(), 'hash with BS')
    check(run('peek', dev, 1, 16, 8)['hex'] == data[B + 16:B + 24].hex(), 'peek')
    check(run('zero-sha', 2)['sha256'] == hashlib.sha256(bytes(2 * B)).hexdigest(), 'zero-sha')
    expect = bytearray(data)
    expect[10 * B:30 * B] = data[40 * B:60 * B]
    run('copy', dev, 40, 10, 20)
    # BSD dd truncates of= without conv=notrunc on a regular file; the tablet writes block devices, which do not truncate
    got = disk.read_bytes()
    check(got[:30 * B] == bytes(expect[:30 * B]), 'copy (forward, within the device)')
    disk.write_bytes(data)
    (tmp / 'pp').mkdir(exist_ok=True)
    (tmp / 'pp' / 'w.bin').write_bytes(b'\xab' * (2 * B))
    run('write', 'w.bin', dev, 5, B)
    check(disk.read_bytes()[5 * B:7 * B] == b'\xab' * (2 * B), 'write FILE DEV SEEK BS')
    # map, on a fake sysfs (a copy of tablet.sh with /sys pointing there): LUN 0 by UFS target 0, partitions by PARTNAME, LUN 0's
    # own entries (linux_*, esp) only on LUN 0 even if another LUN has a partition of the same name
    fs = tmp / 'fs'
    ufs = fs / 'sys/devices/platform/soc@0/1d84000.ufshc/host0'
    for disk, lun in (('sda', 0), ('sde', 4), ('sdf', 5)):
        target = ufs / f'target0:0:0/0:0:0:{lun}'
        target.mkdir(parents=True)
        (fs / f'sys/block/{disk}/queue').mkdir(parents=True)
        (fs / f'sys/block/{disk}/queue/logical_block_size').write_text('4096\n')
        os.symlink(target, fs / f'sys/block/{disk}/device')
    (fs / 'sys/class/block').mkdir(parents=True)
    for node, name, start, size in (('sda35', 'linux_rootfs', 448487424, 123731968), ('sda36', 'linux_data', 574316544, 422100952),
                                    ('sda37', 'esp', 572219392, 2097152), ('sde13', 'boot_a', 618800, 196608),
                                    ('sde23', 'vendor_boot_a', 970800, 196608), ('sde28', 'init_boot_a', 1438256, 16384),
                                    ('sdf13', 'boot_b', 618800, 196608), ('sde40', 'esp', 4000000, 8)):
        d = fs / 'sys/class/block' / node
        d.mkdir()
        (d / 'partition').write_text(node.lstrip('sdabcdef') + '\n')
        (d / 'uevent').write_text(f'MAJOR=8\nPARTN=1\nDEVNAME={node}\nDEVTYPE=partition\nPARTNAME={name}\n')
        (d / 'start').write_text(f'{start}\n')
        (d / 'size').write_text(f'{size}\n')
    script = tmp / 'tablet-fakesys.sh'
    script.write_text((HERE / 'tablet.sh').read_text().replace('/sys/', f'{fs}/sys/'))
    r = subprocess.run(['sh', str(script), 'map'], capture_output=True, text=True, env=env, cwd=tmp)
    got = dict(line.split('=', 1) for line in r.stdout.splitlines() if '=' in line)
    check(got.get('lun0') == 'sda' and got.get('lun0_block_size') == '4096', f'map: LUN 0 = sda, 4096-byte blocks ({got.get("lun0")})')
    check(got.get('esp') == 'sda37 572219392 2097152', f'map: esp is LUN 0\'s partition 37, not sde40 ({got.get("esp")})')
    check(got.get('boot_a') == 'sde13 618800 196608' and got.get('boot_b') == 'sdf13 618800 196608', 'map: boot_a, boot_b by name')
    check(got.get('linux_rootfs') == 'sda35 448487424 123731968', 'map: linux_rootfs')


def build_fake(tmp):
    root = tmp / 'tablet'
    root.mkdir()
    lun0 = root / 'lun0.img'
    with open(lun0, 'wb') as f:
        f.truncate(common.LUN0_BLOCKS * B)
    write_at(lun0, 0, (common.BACKUP / 'new-primary.bin').read_bytes())
    write_at(lun0, (common.LUN0_BLOCKS - 5) * B, (common.BACKUP / 'new-backup.bin').read_bytes())
    manifest = bytearray(B)
    manifest[:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', manifest, 8, 1, 2, 31 * 10 ** 9 // B * B)
    write_at(lun0, common.ROOTFS_FIRST * B, bytes(manifest))
    window = common.store_blocks(PAGES)
    identity = hashlib.sha256(b'test seed').digest()
    header = bytearray(B)
    header[:8] = b'SHNGDATA'
    struct.pack_into('<IIQ', header, 8, 1, 64, PAGES)
    header[24:56] = identity
    struct.pack_into('<I', header, 56, 7)
    struct.pack_into('<I', header, 60, zlib.crc32(bytes(header[:60])) & 0xffffffff)
    old = common.store_first(common.ROOTFS_LAST, window)
    write_at(lun0, old * B, bytes(header) + os.urandom((window - 1) * B))
    write_at(lun0, common.ESP_FIRST * B, os.urandom(3 * MiB))          # leftovers in the tail
    lun4 = root / 'lun4.img'
    with open(lun4, 'wb') as f:
        f.truncate(1 << 30)
    ref = common.DEVICE / 'out/persistent/ref'   # slot A's current images (build-boot.py --verify / the zstd backups)
    for name, (_, off, size) in LUN4.items():
        source = {'boot_a': '13-boot_a', 'vendor_boot_a': '23-vendor_boot_a', 'init_boot_a': '28-init_boot_a'}.get(name)
        data = (ref / f'{source}.img').read_bytes() if source else os.urandom(size)
        assert len(data) == size
        write_at(lun4, off, data)
    out = tmp / 'out'
    out.mkdir()
    real = common.DEVICE / 'out/persistent'
    for name in ('gpt.json', 'gpt-head.bin', 'gpt-tail.bin'):
        shutil.copyfile(real / name, out / name)
    esp = out / 'esp-test.img'
    with open(esp, 'wb') as f:
        f.truncate(common.ESP_BLOCKS * B)
    write_at(esp, 0, b'\xeb\x58\x90MTOO4050' + os.urandom(4 * MiB))
    rec = {'image': esp.name, 'sha256': common.sha(esp), 'default': 'macos-diag', 'profile': 'test',
           'cmdlines': {'macos-diag': 'piano-reset-after=240'}, 'seed': {'identity_sha256': identity.hex(), 'pages': PAGES},
           'store': {'window_blocks': window}}
    common.write_json(out / 'esp-test.json', rec)
    boot = out / 'boot-test-signed.img'
    boot.write_bytes(b'ANDROID!' + os.urandom(common.BOOT_PARTITION_BYTES - 8))
    common.write_json(out / 'boot.json', {'signed': {boot.name: {'sha256': common.sha(boot)}}})
    return root, out, window, old


def snapshot(root, window, old):
    lun0, lun4 = root / 'lun0.img', root / 'lun4.img'
    new = common.store_first(common.ROOTFS_NEW_LAST, window)
    return {'gpt-head': sha_region(lun0, 0, 6 * B), 'gpt-tail': sha_region(lun0, (common.LUN0_BLOCKS - 5) * B, 5 * B),
            'store-region': sha_region(lun0, new * B, (old + window - new) * B),
            'esp-region': sha_region(lun0, common.ESP_FIRST * B, common.ESP_BLOCKS * B),
            'boot_a': sha_region(lun4, LUN4['boot_a'][1], LUN4['boot_a'][2]),
            'boot_b': sha_region(lun4, LUN4['boot_b'][1], LUN4['boot_b'][2]),
            'manifest': sha_region(lun0, common.ROOTFS_FIRST * B, B)}


def test_session(tmp):
    print('== maint-session.py against a fake tablet')
    root, out, window, old = build_fake(tmp)
    nas = tmp / 'nas'
    nas.mkdir()
    env = dict(os.environ, FAKE_ROOT=str(root), PIANO_PERSIST_OUT=str(out), PIANO_MAINT_HOST='127.0.0.1',
               PIANO_BACKUP_DIR=str(nas), PIANO_SERIAL='test', PIANO_MAINT_SHELL=str(tmp / 'fake-shell.py'),
               PYTHONDONTWRITEBYTECODE='1')
    (tmp / 'fake-shell.py').write_text(f'import sys; sys.path.insert(0, {str(HERE)!r}); import importlib.util\n'
                                       f'spec = importlib.util.spec_from_file_location("t", {str(Path(__file__).resolve())!r})\n'
                                       'm = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)\n'
                                       'sys.stdout.write(m.fake_shell(sys.argv[1]))\n')
    base = ['python3', str(HERE / 'maint-session.py'), '--session', str(tmp / 'session'), '--esp', 'esp-test.img',
            '--esp-final', 'esp-test.img', '--boot-image', 'boot-test-signed.img', '--http-port', '18000', '--nc-port', '19000']

    def run(*steps, expect=0):
        r = subprocess.run(base + list(steps), capture_output=True, text=True, env=env, timeout=3600)
        log = r.stdout + r.stderr
        (tmp / 'session-out.txt').open('a').write(log)
        if r.returncode != expect:
            print(log[-3000:])
        check(r.returncode == expect, f'maint-session.py {" ".join(steps)} -> {r.returncode}')
        return log

    before = snapshot(root, window, old)
    run('map', 'backup')
    state = json.loads((tmp / 'session/state.json').read_text())
    check(state.get('store_plan') == 'move', 'auto: the store is bound to the ESP seed, so it moves')
    names = sorted(state.get('backups', {}))
    check(names == sorted(['lun0-gpt-primary', 'lun0-gpt-backup', 'boot_a', 'boot_b', 'vendor_boot_a', 'init_boot_a',
                           'linux_rootfs-tail-esp', 'linux_rootfs-store']), f'backups {names}')
    bdir = nas / state['backup_name']
    check(all(common.sha(bdir / e['file']) == e['sha256'] for e in state['backups'].values()), 'every NAS copy hashes to its record')
    check(state['backups']['lun0-gpt-primary']['sha256'] == common.GPT_PRIMARY_SHA256, 'GPT backup = CRITICAL-BACKUP new-primary.bin')
    run('gpt', 'store', 'esp', 'check')
    lun0 = root / 'lun0.img'
    gpt = json.loads((out / 'gpt.json').read_text())
    check(sha_region(lun0, 0, 6 * B) == gpt['gpt-head.bin']['sha256'], 'GPT head on the fake disk = gpt-head.bin')
    check(sha_region(lun0, (common.LUN0_BLOCKS - 5) * B, 5 * B) == gpt['gpt-tail.bin']['sha256'], 'GPT tail = gpt-tail.bin')
    new = common.store_first(common.ROOTFS_NEW_LAST, window)
    check(read(lun0, new * B, 8) == b'SHNGDATA', f'store header at the new place {new}')
    check(sha_region(lun0, common.ESP_FIRST * B, common.ESP_BLOCKS * B) == common.sha(out / 'esp-test.img'), 'ESP region = the image')
    run('boot-a', 'check')
    check(sha_region(root / 'lun4.img', LUN4['boot_a'][1], LUN4['boot_a'][2]) == common.sha(out / 'boot-test-signed.img'), 'boot_a = the image')
    # An ESP update once installed: the store at the new place is the one its first boot opens.
    rec = json.loads((out / 'esp-test.json').read_text())
    shutil.copyfile(out / 'esp-test.img', out / 'esp-other.img')
    common.write_json(out / 'esp-other.json', {**rec, 'image': 'esp-other.img', 'seed': {**rec['seed'], 'identity_sha256': 'ab' * 32}})
    log = run('map', 'esp-final', '--esp', 'esp-other.img', '--esp-final', 'esp-other.img', expect=3)
    check('is NOT the one this ESP resumes' in log and 'is not the seed of the store' in log,
          'installed: esp-final refuses an ESP of another seed (its boots would run Data volatile)')
    common.write_json(out / 'esp-other.json', {**rec, 'image': 'esp-other.img', 'seed': {**rec['seed'], 'identity_sha256': 'ab' * 32},
                                                'data_store': {'replace': 'cd' * 4}})
    run('map', 'esp-final', '--esp', 'esp-other.img', '--esp-final', 'esp-other.img')
    check(True, '... and writes it when its record says --replace-data (the owner\'s decision)')
    log = run('map', 'esp-final')
    check('IS the one this ESP resumes' in log and
          sha_region(lun0, common.ESP_FIRST * B, common.ESP_BLOCKS * B) == common.sha(out / 'esp-test.img'),
          'installed: esp-final writes an ESP of the store\'s seed')
    run('backup')   # again, after the writes: must keep the originals as the rollback sources
    state2 = json.loads((tmp / 'session/state.json').read_text())
    check(state2['backups'] == state['backups'], 'a second backup run keeps the original backups')
    # store-backup: the installed store as it is now (the owner's Data before a new ESP's first boot), and its rollback
    window_now = sha_region(lun0, new * B, window * B)
    run('map', 'store-backup')
    entry = json.loads((tmp / 'session/state.json').read_text())['backups'].get('linux_rootfs-store-now') or {}
    check(entry.get('sha256') == window_now and entry.get('skip') == new and entry.get('count') == window and
          common.sha(bdir / entry['file']) == window_now,
          'store-backup: the store window at its installed place, verified on the NAS')
    write_at(lun0, (new + 5) * B, b'macOS wrote Data since')
    log = run('map', 'store-backup', expect=3)
    check('changed since its verified backup' in log, 'store-backup again in the same set after the store changed: stops (--new-backup)')
    run('rollback', 'store-backup')
    check(sha_region(lun0, new * B, window * B) == window_now, 'rollback store-backup: the store as it was backed up')
    run('rollback', 'esp', 'store', 'gpt', 'boot-a')
    after = snapshot(root, window, old)
    for key in before:
        check(after[key] == before[key], f'after the rollbacks: {key} is as before')
    # a region that changes without this install writing it stops the session
    write_at(root / 'lun4.img', LUN4['boot_b'][1], b'changed')
    log = run('map', 'backup', expect=3)
    check('changed since its verified backup' in log, 'an unexplained change of boot_b stops the backup step')


def test_pair_rule(tmp):
    """The boot_a/ESP pair (docs/persistent-boot.md section 12): an ESP asking for a 13 GiB RAM window only after boot_a has an FD
    whose DarwinBoot takes it, and the rollbacks in the reverse order."""
    print('== maint-session.py: the boot_a FD and the ESP\'s RAM window (a fake tablet)')
    root, out, window, old = build_fake(tmp)
    nas = tmp / 'nas'
    nas.mkdir()
    env = dict(os.environ, FAKE_ROOT=str(root), PIANO_PERSIST_OUT=str(out), PIANO_MAINT_HOST='127.0.0.1',
               PIANO_BACKUP_DIR=str(nas), PIANO_SERIAL='test', PIANO_MAINT_SHELL=str(tmp / 'fake-shell.py'),
               PYTHONDONTWRITEBYTECODE='1')
    (tmp / 'fake-shell.py').write_text(f'import sys; sys.path.insert(0, {str(HERE)!r}); import importlib.util\n'
                                       f'spec = importlib.util.spec_from_file_location("t", {str(Path(__file__).resolve())!r})\n'
                                       'm = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)\n'
                                       'sys.stdout.write(m.fake_shell(sys.argv[1]))\n')
    base = ['python3', str(HERE / 'maint-session.py'), '--session', str(tmp / 'session'), '--http-port', '18010', '--nc-port', '19010']

    def run(*steps, expect=0):
        r = subprocess.run(base + list(steps), capture_output=True, text=True, env=env, timeout=3600)
        log = r.stdout + r.stderr
        (tmp / 'session-out.txt').open('a').write(log)
        if r.returncode != expect:
            print(log[-3000:])
        check(r.returncode == expect, f'maint-session.py {" ".join(steps)} -> {r.returncode}')
        return log

    old_boot = ['--boot-image', 'boot-test-signed.img']
    new_boot = ['--boot-image', 'boot-test5-signed.img']
    small = ['--esp', 'esp-test.img', '--esp-final', 'esp-test.img']
    big = ['--esp', 'esp-13g.img', '--esp-final', 'esp-13g.img']
    # the installed state: boot_a = an image of an FD without darwinboot/0001 (boot.json: no fd.ram_window), an 8 GiB ESP
    run('map', 'backup', 'gpt', 'store', 'esp', 'boot-a', 'check', *old_boot, *small)
    image5 = out / 'boot-test5-signed.img'
    image5.write_bytes(b'ANDROID!' + os.urandom(common.BOOT_PARTITION_BYTES - 8))
    common.write_json(out / 'boot-v5.json', {'series': 5, 'fd': {'sha256': 'ce' * 32, 'ram_window': common.DARWIN_WINDOW_MAX},
                                             'signed': {image5.name: {'sha256': common.sha(image5)}}})
    rec = json.loads((out / 'esp-test.json').read_text())
    shutil.copyfile(out / 'esp-test.img', out / 'esp-13g.img')
    write_at(out / 'esp-13g.img', 8 * MiB, b'the 13 GiB ADT')
    common.write_json(out / 'esp-13g.json', {**rec, 'image': 'esp-13g.img', 'sha256': common.sha(out / 'esp-13g.img'),
                                             'ram_window': 0x33fe00000})
    installed = snapshot(root, window, old)
    log = run('--dry-run', 'map', 'backup', 'esp-final', 'boot-a', 'check', *new_boot, *big)
    check('would stop a real run' in log and 'that pair never boots' in log, 'dry run, ESP before boot_a: reported as a stop')
    log = run('--dry-run', 'map', 'backup', 'boot-a', 'esp-final', 'check', *new_boot, *big)
    check('would stop a real run' not in log, 'dry run, boot_a before the ESP: clean')
    log = run('map', 'backup', 'esp-final', 'check', *old_boot, *big, '--new-backup', expect=3)
    check('that pair never boots' in log and snapshot(root, window, old)['esp-region'] == installed['esp-region'],
          'esp-final refuses a 13 GiB ESP while boot_a\'s DarwinBoot takes 8 GiB; the ESP region is untouched')
    run('map', 'boot-a', 'esp-final', 'check', *new_boot, *big)
    lun0, lun4 = root / 'lun0.img', root / 'lun4.img'
    check(sha_region(lun4, LUN4['boot_a'][1], LUN4['boot_a'][2]) == common.sha(image5) and
          sha_region(lun0, common.ESP_FIRST * B, common.ESP_BLOCKS * B) == common.sha(out / 'esp-13g.img'),
          'boot-a then esp-final: boot_a = the series 5 image, the ESP = the 13 GiB one')
    log = run('rollback', 'boot-a', *new_boot, *big, expect=3)
    check('that pair never boots' in log and sha_region(lun4, LUN4['boot_a'][1], LUN4['boot_a'][2]) == common.sha(image5),
          'rollback boot-a refuses the 8 GiB FD while the 13 GiB ESP is on the tablet; boot_a untouched')
    log = run('map', 'boot-a', *old_boot, *small, expect=3)
    check('that pair never boots' in log, 'boot-a refuses to write the 8 GiB FD under the 13 GiB ESP')
    run('rollback', 'esp-final', 'boot-a', *new_boot, *big)
    after = snapshot(root, window, old)
    check(after['boot_a'] == installed['boot_a'] and after['esp-region'] == installed['esp-region'],
          'rollback esp-final, then boot-a: both as installed (the new backup set\'s copies)')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--fake-shell', nargs=2)
    p.add_argument('--keep', type=Path)
    a = p.parse_args()
    if a.fake_shell:
        sys.stdout.write(fake_shell(a.fake_shell[0]))
        return
    tmp = Path(tempfile.mkdtemp(prefix='piano-persist-test-')) if not a.keep else a.keep
    tmp.mkdir(parents=True, exist_ok=True)
    try:
        test_tablet_sh(tmp)
        test_session(tmp)
        (tmp / 'pair').mkdir()
        test_pair_rule(tmp / 'pair')
    finally:
        if not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
