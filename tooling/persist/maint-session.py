#!/usr/bin/env python3
"""The persistent-boot install on piano, step by step (docs/persistent-boot.md; plan option A1 of the local
persistent-boot-plan.md, with the owner's 2026-11-11 06:42 rules).

  maint-session.py --list
  maint-session.py --dry-run STEP [STEP ...]          print every command; nothing is sent to the tablet, ABL or the NAS
  maint-session.py STEP [STEP ...]                    run the steps (no per-step question)
  maint-session.py [--dry-run] rollback STEP

Rules every write step follows (it aborts on the first failure, before or after the write):
  1. before: every region it writes has a VERIFIED BACKUP on the NAS (step `backup`): exported from the tablet over one TCP
     connection into PIANO_BACKUP_DIR/persistent-boot-<time>/ (read from ~/.config/piano/env at run time, never printed or
     stored in a tracked file), byte count and SHA-256 equal to the tablet's own SHA-256 of the region, then read back from the
     NAS without the page cache (F_NOCACHE) and hashed again. Small regions are also kept in out/persistent/session/.
  2. the write (dd, conv=fsync), then the page cache is dropped and the region read back from the medium: its SHA-256 must be
     the image's. Any mismatch stops the session.
  3. a rollback from the verified backups: `maint-session.py rollback STEP`.
Order (sessions as in the plan; s = the maintenance RAM image, a = ABL fastboot):
  a  abl-check                                       getvar only
  s  maint-boot map backup gpt store esp check maint-exit
  a  ram-esp-test ram-v4-signed ram-precedence       fastboot boot only (RAM)
  s  maint-boot map backup boot-a [esp-final] [slot-success] check maint-exit
  a  abl-final reboot
  later, an ESP update on the installed tablet (docs/persistent-boot.md section 11):
  s  maint-boot map backup [store-backup] esp-final check maint-exit --esp E --esp-final E --new-backup
  later, the 27.3-only ESP, then linux_rootfs retired (docs/persistent-boot.md section 13; E = esp-k273-only.img):
  s  maint-boot map backup esp-final check maint-exit --esp E --esp-final E --new-backup
  s  maint-boot map rootfs-backup rootfs-clear check maint-exit --esp E
  later, a new boot_a FD with its ESP (docs/persistent-boot.md section 12; B = boot-piano-macos-persistent-vN-signed.img):
  a  ram-boot-a --boot-image B                       fastboot boot only: B boots the installed ESP (no reset: the USB console)
  s  maint-boot map backup [store-backup] boot-a esp-final check maint-exit --boot-image B --esp E --esp-final E --new-backup
The pair on the tablet stays bootable: an ESP whose ADT asks for a larger RAM window than boot_a's DarwinBoot takes never boots,
so esp-final refuses such an ESP until boot-a has written an FD that takes it, and boot-a / rollback boot-a refuse an older FD
while such an ESP is on the tablet (rollback esp-final first). Each is checked against the tablet's bytes, not the state file.
Never used: fastboot flash, erase, set_active, any oem to ABL; nothing is written to xbl, abl, tz, hyp, vbmeta, dtbo, persist or
any _b partition (boot_b is backed up only); vendor_boot_a and init_boot_a are only read and backed up. No password anywhere.
The tablet side is tooling/persist/tablet.sh (served over HTTP from the host's USB-link address, a fixed list of files, sha256
checked); commands go through tooling/native/maint-shell.py (PIANO_MAINT_HOST). State: out/persistent/session/state.json.
"""
import argparse
import fcntl
import hashlib
import http.server
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

D = common.DEVICE
# PIANO_PERSIST_OUT: the out/persistent this session's images, records, installed.json and (by default) session/ live in. Default
# this checkout's; set it to run this checkout's tooling on the install state of the checkout that installed the tablet
# (docs/persistent-boot.md section 11.5). tooling/persist/test-persist.py sets it, and PIANO_MAINT_SHELL, for its fake tablet.
OUT = Path(os.environ.get('PIANO_PERSIST_OUT', D / 'out/persistent'))
MAINT_SHELL = Path(os.environ.get('PIANO_MAINT_SHELL', D / 'tooling/native/maint-shell.py'))
TABLET_SH = D / 'tooling/persist/tablet.sh'
XNU_LOG = D / 'tooling/native/xnu-log.py'
V110 = Path(os.path.expanduser(os.environ.get('PIANO_V110', str(Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/v1.1.0'))))   # the Ubuntu v1.1.0 release (or its few files)
MAINT_IMAGE = V110 / 'release/boot/boot-ram-maintenance.img'
MAINT_IMAGE_SHA256 = '54001a50a13fe80c3bd8e926161608b71d91e57e2d6be6b83cdc2bc62b5f6fbc'
PLATFORM_TOOLS = '/opt/homebrew/share/android-commandlinetools/platform-tools'
B = common.LBA
MiB = 1 << 20
LUN0_BYTES = common.LUN0_BLOCKS * B
TAIL_LBA = common.LUN0_BLOCKS - 5
UFS_LUN0_VENDOR = '860845C1-BE09-4355-8BC1-30D64FF8E63A'
RAM_PAYLOAD_VENDOR = 'A9C6827C-1BD8-410C-885A-C46500F7D836'
ABL_EXPECT = {'product': 'piano', 'unlocked': 'yes', 'current-slot': 'a', 'slot-successful:a': 'yes', 'slot-unbootable:a': 'no',
              'battery-soc-ok': 'yes', 'partition-size:boot_a': '0x6000000'}
ABL_SHOW = ('slot-retry-count:a', 'battery-voltage')
SMALL = 128 * MiB            # backups up to this size are also kept in out/persistent/session/
# A slow USB link (the big regions: rootfs-backup, rootfs-clear and its rollback): the slowest rate the time limits allow for, and how
# long a stream may stall before its piece counts as failed (then retried).
SLOW_LINK = 1 * MiB          # bytes per second over USB
SLOW_MEDIUM = 20 * MiB       # bytes per second for a hash, discard or zeroing on the tablet
STALL = 300
PIECE_TRIES = 3


def limit(nbytes, rate=SLOW_LINK, floor=1800):
    """The time limit (s) for moving NBYTES at no less than RATE, never under FLOOR."""
    return max(floor, int(nbytes / rate) + 600)


def store_window(s):
    """The 27.0.1 store's window (linux_rootfs): the --esp record's, else that store's own (12 GiB seed). A record whose store is
    elsewhere (esp_store) has none: the linux_rootfs places are then only read for information."""
    return ((s.esp or {}).get('store') or {}).get('window_blocks') or 3145825


def store_old(s):
    return common.store_first(common.ROOTFS_LAST, store_window(s))      # 68381599


def store_new(s):
    return common.store_first(common.ROOTFS_NEW_LAST, store_window(s))  # 68119455


def rel(lba):
    """An absolute LUN 0 LBA as a block of linux_rootfs (partition 35 starts at 56060928)."""
    return lba - common.ROOTFS_FIRST


class Stop(Exception):
    pass


def esp_store(record):
    """The Data store an ESP resumes when it is not the 27.0.1 line's (unbound, at the end of linux_rootfs): the record's store
    with its partition and first LBA (tooling/k273/build-k273-esp.py: the 27.3 store, bound to that LBA). None: the legacy store,
    checked as before (store_new, the record's seed)."""
    store = (record or {}).get('store') or {}
    if store.get('partition') and store['partition'] != 'linux_rootfs' and store.get('first_lba') is not None:
        return store
    return None


# LUN 0 partitions by their GPT name -> the key map_nodes keeps them under (tablet.sh map reports these)
PART_KEYS = {'linux_rootfs': 'rootfs', 'linux_data': 'linux_data', 'esp': 'esp'}


def read_nocache(path, digest=None):
    """A file's bytes in 4 MiB chunks, read with the page cache off (F_NOCACHE): the backup as stored, not the bytes just written.
    DIGEST (a hashlib object) is fed them."""
    fd = os.open(path, os.O_RDONLY)
    try:
        fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)
        while chunk := os.read(fd, 4 * MiB):
            if digest is not None:
                digest.update(chunk)
            yield chunk
    finally:
        os.close(fd)


def inflate(path, digest=None):
    """A gzip file's contents (every member), read as read_nocache reads it; DIGEST is fed the stored bytes."""
    z = zlib.decompressobj(31)
    for data in read_nocache(path, digest):
        while data:
            out = z.decompress(data, 16 * MiB)    # bounded: a chunk of zeros inflates a thousandfold
            if out:
                yield out
            if z.eof:                             # the next member starts in what is left
                data = z.unused_data
                z = zlib.decompressobj(31)
            else:
                data = z.unconsumed_tail
    out = z.flush()
    if out:
        yield out


class Session:
    def __init__(self, a):
        self.a, self.execute = a, not a.dry_run
        self.dir = a.session
        self.dir.mkdir(parents=True, exist_ok=True)
        self.state_path = self.dir / 'state.json'
        self.state = json.loads(self.state_path.read_text()) if self.state_path.is_file() else {}
        self.env = self.piano_env()
        self.serial = self.env.get('PIANO_SERIAL', '<PIANO_SERIAL>')
        self.http, self.files, self.dry_failures = None, {}, []
        if a.new_backup and self.execute:
            self.state.pop('backup_name', None)
            self.state.pop('backups', None)
        os.environ['PATH'] = PLATFORM_TOOLS + os.pathsep + os.environ['PATH']
        self.gpt = json.loads((OUT / 'gpt.json').read_text()) if (OUT / 'gpt.json').is_file() else None
        self.boot = self.boot_records()
        # What a dry run's earlier steps would have written (boot_a, ESP sha256), for the later steps' pair checks.
        self.sim, self.windows = {}, {}
        self.esp = self.esp_record(a.esp)
        self.nodes = self.state.get('nodes', {'lun0': 'sda', 'rootfs': 'sda35', 'esp': 'sda37', 'boot_a': '<boot_a>',
                                              'boot_b': '<boot_b>', 'vendor_boot_a': '<vendor_boot_a>', 'init_boot_a': '<init_boot_a>'})
        self.sizes = self.state.get('sizes', {'boot_a': common.BOOT_PARTITION_BYTES, 'boot_b': common.BOOT_PARTITION_BYTES,
                                              'vendor_boot_a': common.BOOT_PARTITION_BYTES, 'init_boot_a': 8 * MiB})
        # LUN 0 partitions' first LBAs (4096-byte blocks) as the map found them; the GPT's of today until a map ran
        self.starts = self.state.get('starts', {'rootfs': common.ROOTFS_FIRST, 'linux_data': common.DATA_FIRST, 'esp': common.ESP_FIRST})

    # -- bookkeeping ---------------------------------------------------------------------------------------------------
    @staticmethod
    def piano_env():
        env = {}
        path = Path.home() / '.config/piano/env'
        if path.is_file():
            for line in path.read_text().splitlines():
                if '=' in line and not line.lstrip().startswith('#'):
                    k, v = line.split('=', 1)
                    env[k.strip()] = v.strip().strip('"')
        env.update({k: v for k, v in os.environ.items() if k.startswith('PIANO_')})
        return env

    @staticmethod
    def boot_records():
        """The signed boot_a images of boot.json (series 4) and every boot-v*.json (build-boot.py --series N), each with its record's
        name and its FD's DarwinBoot window bound (common.boot_fd_bound)."""
        signed, unsigned = {}, {}
        for path in [OUT / 'boot.json', *sorted(OUT.glob('boot-v*.json'))]:
            if path.is_file():
                record = json.loads(path.read_text())
                bound = common.boot_fd_bound(record)
                for name, entry in (record.get('signed') or {}).items():
                    signed[name] = {**entry, 'record': path.name, 'fd_bound': bound}
                for name in record.get('images') or {}:
                    unsigned[name] = {'record': path.name, 'fd_bound': bound}
        return {'signed': signed, 'unsigned': unsigned}

    def boot_identity(self, sha):
        """(name, DarwinBoot window bound) of the boot_a contents with this sha256; the bound is None for Ubuntu's boot_a (no UEFI: no
        ESP is booted) and for unknown contents. 'planned:NAME' is a dry run's boot-a of an image not signed yet."""
        if sha == common.BOOT_A_SHA256:
            return 'Ubuntu\'s boot_a (13-boot_a)', None
        if sha and sha.startswith('planned:'):
            name = sha.removeprefix('planned:')
            entry = self.boot['unsigned'].get(name.replace('-signed.img', '.img')) or {}
            return f'{name} (not signed yet)', entry.get('fd_bound')
        for name, entry in self.boot['signed'].items():
            if entry.get('sha256') == sha:
                return name, entry['fd_bound']
        return 'unknown contents', None

    def esp_by_sha(self, sha):
        """The build record (out/persistent/*.json) of the ESP image with this sha256, else None (installed.json is no record)."""
        for path in sorted(OUT.glob('*.json')):
            if path.name == 'installed.json':
                continue
            try:
                record = json.loads(path.read_text())
            except ValueError:
                continue
            if isinstance(record, dict) and record.get('sha256') == sha and record.get('image'):
                return record
        return None

    def esp_window(self, record):
        """XNU's RAM window the ESP's ADT asks for (bytes; None: no ADT, DarwinBoot's 256 MiB): the record's ram_window, else its ADT
        input while that file has the recorded sha256, else the ADT read out of the image itself."""
        if not record:
            return None
        if record.get('ram_window'):
            return int(record['ram_window'])
        key = record.get('sha256')
        if key not in self.windows:
            window = None
            adt = (record.get('inputs') or {}).get('adt') or {}
            if adt.get('path') and Path(adt['path']).is_file() and common.sha(adt['path']) == adt.get('sha256'):
                window = common.adt_ram_window(adt['path'])
            elif (OUT / record['image']).is_file():
                try:
                    data = common.fat32_read(OUT / record['image'])['files'].get('EFI/UniXNU/Payloads/kernel.macho.adt')
                    window = common.adt_ram_window(data) if data else None
                except (ValueError, IndexError, KeyError, struct.error):
                    window = None   # not a FAT image (the tests' stand-ins): no payload, no window
            self.windows[key] = window
        return self.windows[key]

    def tablet_boot_a(self):
        """boot_a's sha256 on the tablet: read from the medium (a real run), what this dry run's steps would have written, else the
        state's."""
        if self.execute:
            return self.region_sha(self.nodes['boot_a'], 0, self.sizes['boot_a'] // MiB, MiB, name='sha-boot_a')
        return self.sim.get('boot_a') or self.state.get('boot_a_written') or self.state.get('boot_a_sha256')

    def tablet_esp(self):
        """The ESP region's sha256 on the tablet (as tablet_boot_a)."""
        if self.execute:
            return self.region_sha(self.nodes['lun0'], common.ESP_FIRST * B // MiB, 1024, MiB, name='esp-region-now')
        return self.sim.get('esp') or self.state.get('esp_sha256')

    def pair(self, boot_sha, esp_record, what):
        """The rule: an ESP whose ADT asks for more than boot_a's DarwinBoot takes never boots (DarwinBoot refuses the ADT)."""
        window = self.esp_window(esp_record)
        name, bound = self.boot_identity(boot_sha)
        if not window or window <= common.OLD_DARWINBOOT_WINDOW_MAX or boot_sha == common.BOOT_A_SHA256:
            self.log(f'  info  {what}: boot_a {name}, ESP {esp_record["image"] if esp_record else "-"} '
                     f'(RAM window {window >> 20 if window else "-"} MiB): every DarwinBoot takes it')
            return
        self.need(bound is not None and bound >= window,
                  f'{what}: boot_a {name} ({boot_sha and boot_sha[:16]}) takes the RAM window of ESP {esp_record["image"]} '
                  f'({window >> 20} MiB): its DarwinBoot\'s bound is {bound >> 20 if bound else "unknown"} MiB' +
                  ('' if bound and bound >= window else ' -> that pair never boots: boot_a first (boot-a with an FD that takes it) '
                   'before esp-final, and rollback esp-final before rollback boot-a'))

    def esp_record(self, name):
        path = OUT / (name.removesuffix('.img') + '.json')
        return json.loads(path.read_text()) if path.is_file() else None

    def save(self, **values):
        self.state.update(values)
        self.state_path.write_text(json.dumps(self.state, indent=2) + '\n')

    def log(self, text):
        print(text, flush=True)
        with open(self.dir / 'session.log', 'a') as f:
            f.write(time.strftime('%H:%M:%S ') + text + '\n')

    def need(self, condition, what):
        """A check: logged; in a real run a failure stops the session. None = only shown (dry run); a host-side check that
        fails in a dry run is counted and reported at the end instead of stopping."""
        if condition is None:
            self.log(f'  check {what}')
            return
        self.log(('  ok    ' if condition else '  FAIL  ') + what)
        if not condition:
            if self.execute:
                raise Stop(what)
            self.dry_failures.append(what)

    def write(self, what):
        self.log(f'  WRITE {what}')
        return self.execute

    # -- the NAS --------------------------------------------------------------------------------------------------------
    def nas(self):
        """The backup directory of this install on the NAS: PIANO_BACKUP_DIR (runtime only) / persistent-boot-<time>."""
        name = self.state.get('backup_name') or f'persistent-boot-{time.strftime("%Y%m%d-%H%M%S")}'
        base = self.backup_root()
        if base is None:
            return None, name
        (base / name).mkdir(exist_ok=True)
        if 'backup_name' not in self.state:
            self.save(backup_name=name, backups={})
        return base / name, name

    def written_by_install(self, key):
        """Whether this install has written region KEY since its backup (then a different SHA-256 now is expected)."""
        st = self.state
        return {'lun0-gpt-primary': st.get('gpt_state') == 'new', 'lun0-gpt-backup': st.get('gpt_state') == 'new',
                'linux_rootfs-tail-esp': bool(st.get('esp_written')), 'boot_a': bool(st.get('boot_a_written')),
                # the store region changes with the move and whenever macOS runs from the ESP (data-persist=2)
                'linux_rootfs-store': st.get('gpt_state') == 'new', 'linux_rootfs-store-header': st.get('gpt_state') == 'new',
                'lun4-gpt-primary': bool(st.get('slot_success_written')),
                'lun4-gpt-backup': bool(st.get('slot_success_written'))}.get(key, False)

    def require_backup(self, key, sha=None):
        entry = self.state.get('backups', {}).get(key)
        if not self.execute:
            self.log(f'  check verified NAS backup of {key} exists (step backup)')
            return entry
        if not entry or not entry.get('verified') or (sha and entry['sha256'] != sha):
            raise Stop(f'no verified NAS backup of {key}{" with sha256 " + sha[:16] if sha else ""}: run step backup first')
        self.log(f'  ok    verified NAS backup of {key}: {entry["file"]} sha256 {entry["sha256"][:16]}')
        return entry

    # -- tablet --------------------------------------------------------------------------------------------------------
    def shell(self, command, timeout=120):
        """One line in the maintenance image's root shell (maint-shell.py); returns its output."""
        self.log(f'  tablet# {command}')
        if not self.execute:
            return None
        r = subprocess.run(['python3', str(MAINT_SHELL), command, str(timeout)], capture_output=True, text=True, timeout=timeout + 30)
        out = r.stdout.replace('\r', '')
        for line in out.strip().splitlines()[-40:]:
            self.log('        | ' + line)
        if r.returncode:
            raise Stop(f'tablet command failed ({r.returncode}): {command}')
        return out

    def t(self, *argv, timeout=120):
        """tablet.sh SUBCOMMAND ARGS; returns its key=value lines as a dict (None in a dry run)."""
        out = self.shell('sh /run/pp/tablet.sh ' + ' '.join(str(x) for x in argv), timeout)
        if out is None:
            return None
        values = dict(line.split('=', 1) for line in out.splitlines() if re.match(r'^[a-z0-9_]+=', line))
        if 'error' in values:
            raise Stop(f'tablet: {values["error"]}')
        return values

    def job(self, name, *argv, timeout=3600, every=15):
        """A long tablet.sh subcommand, detached on the tablet (survives the telnet line), polled until it ends."""
        self.t('job', name, *argv)
        if not self.execute:
            return None
        t0 = time.time()
        while time.time() - t0 < timeout:
            time.sleep(every)
            r = self.t('job-status', name)
            if 'rc' in r:
                if r['rc'] != '0':
                    raise Stop(f'tablet job {name} ended with {r["rc"]}')
                self.log(f'        {name}: {int(time.time() - t0)} s')
                return r
            if int(time.time() - t0) % 300 < every:
                self.shell('piano-maint keepalive')
        raise Stop(f'tablet job {name} still running after {timeout} s')

    def region_sha(self, dev, skip, count, bs=B, name='hash'):
        """SHA-256 of a region read from the medium (tablet.sh hash drops the page cache first)."""
        big = count * bs > 256 * MiB
        r = self.job(name, 'hash', dev, skip, count, bs, timeout=7200) if big else self.t('hash', dev, skip, count, bs)
        return r and r['sha256']

    # -- host services -------------------------------------------------------------------------------------------------
    def host_ip(self):
        tablet = self.env.get('PIANO_MAINT_HOST')
        if not tablet:
            raise Stop('PIANO_MAINT_HOST is not set (environment or ~/.config/piano/env)')
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((tablet, 9))
            ip = s.getsockname()[0]
        except OSError:
            if self.execute:
                raise Stop('no route to PIANO_MAINT_HOST (is the USB NCM link up?)')
            ip = '<host-ip>'        # dry run without the link
        finally:
            s.close()
        return ip

    def serve(self, files):
        """HTTP for the tablet's wget: only the named files, on the host's USB-link address. A value ('gzip', PATH, BYTES) serves
        PATH inflated (BYTES long): a piece of rootfs-backup."""
        self.files.update({name: (path if isinstance(path, tuple) else Path(path)) for name, path in files.items()})
        for name, path in files.items():
            self.log(f'  host  serves {name} ({Path(path[1]).name + ", inflated" if isinstance(path, tuple) else Path(path).name})')
        if not self.execute or self.http:
            return
        session = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                path = session.files.get(self.path.lstrip('/'))
                gz = isinstance(path, tuple)
                if not path or not (path[1] if gz else path).is_file():
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header('Content-Length', str(path[2] if gz else path.stat().st_size))
                self.end_headers()
                if gz:
                    for chunk in inflate(path[1]):
                        self.wfile.write(chunk)
                    return
                with open(path, 'rb') as f:
                    while chunk := f.read(MiB):
                        self.wfile.write(chunk)

            def log_message(self, *args):
                pass

        self.http = http.server.ThreadingHTTPServer((self.host_ip(), self.a.http_port), Handler)
        threading.Thread(target=self.http.serve_forever, daemon=True).start()

    def fetch(self, name, path, digest):
        """The tablet downloads a host file to /run/pp/NAME and checks its SHA-256."""
        self.serve({name: path})
        return self.t('fetch', name, digest, self.a.http_port, timeout=900)

    def receive(self, dev, skip, count, bs, dests):
        """The tablet streams a region to the host over one TCP connection; written to every path in DESTS. Returns
        (bytes, sha256) of what arrived."""
        total = count * bs
        server = socket.create_server((self.host_ip(), self.a.nc_port))
        server.settimeout(120)
        got = {}

        def run():
            conn, files = None, []
            try:
                conn, _ = server.accept()
                h, n = hashlib.sha256(), 0
                files = [open(p, 'wb') for p in dests]
                while n < total:
                    chunk = conn.recv(min(MiB, total - n))
                    if not chunk:
                        break
                    for f in files:
                        f.write(chunk)
                    h.update(chunk)
                    n += len(chunk)
                for f in files:
                    f.flush()
                    os.fsync(f.fileno())
                got.update(bytes=n, sha256=h.hexdigest())
            except OSError as e:          # the NAS or the link failed: close, so the tablet's nc ends too
                got.update(error=str(e).replace(str(Path(dests[0]).parent.parent), '$PIANO_BACKUP_DIR'))
            finally:
                for f in files:
                    f.close()
                if conn:
                    conn.close()

        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        if total > 256 * MiB:
            self.job(f'send-{skip}', 'send', dev, skip, count, bs, self.a.nc_port, timeout=4 * 3600)
        else:
            self.t('send', dev, skip, count, bs, self.a.nc_port, timeout=600)
        thread.join(4 * 3600)
        server.close()
        if 'error' in got:
            raise Stop(f'export of /dev/{dev} failed on the host: {got["error"]}')
        return got.get('bytes'), got.get('sha256')

    @staticmethod
    def sha_nocache(path):
        """SHA-256 of a file read with the page cache off (F_NOCACHE): the NAS copy as stored, not the bytes just sent."""
        h = hashlib.sha256()
        fd = os.open(path, os.O_RDONLY)
        try:
            fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)
            while chunk := os.read(fd, 4 * MiB):
                h.update(chunk)
        finally:
            os.close(fd)
        return h.hexdigest()

    def receive_piece(self, dev, skip, count, bs, dest, compress, name):
        """One piece of a big region, streamed by the tablet over one TCP connection (a detached job: survives the telnet line) into
        DEST, gzip-compressed on the host when COMPRESS. The sha256 is of the raw bytes as they arrive, before compression; a stall of
        STALL s ends the piece. Returns (raw bytes, raw sha256, stored sha256, stored bytes); (None, ...) in a dry run."""
        total = count * bs
        if not self.execute:
            self.t('job', name, 'send', dev, skip, count, bs, self.a.nc_port)
            return None, None, None, None
        server = socket.create_server((self.host_ip(), self.a.nc_port))
        server.settimeout(120)
        got, held = {}, {}

        def run():
            conn, f = None, None
            try:
                conn, _ = server.accept()
                held['conn'] = conn
                conn.settimeout(STALL)
                raw, stored, n, kept = hashlib.sha256(), hashlib.sha256(), 0, 0
                z = zlib.compressobj(1, zlib.DEFLATED, 31) if compress == 'gzip' else None
                f = open(dest, 'wb')
                while n < total:
                    chunk = conn.recv(min(MiB, total - n))
                    if not chunk:
                        break
                    raw.update(chunk)
                    n += len(chunk)
                    out = z.compress(chunk) if z else chunk
                    if out:
                        f.write(out)
                        stored.update(out)
                        kept += len(out)
                if z:
                    out = z.flush()
                    f.write(out)
                    stored.update(out)
                    kept += len(out)
                f.flush()
                os.fsync(f.fileno())
                got.update(bytes=n, sha256=raw.hexdigest(), stored_sha256=stored.hexdigest(), stored_bytes=kept)
            except OSError as e:          # the backup disk or the link failed: close, so the tablet's nc ends too
                got.update(error=str(e).replace(str(Path(dest).parent.parent), '$PIANO_BACKUP_DIR'))
            finally:
                if f:
                    f.close()
                if conn:
                    conn.close()

        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        try:
            if total > 256 * MiB:
                self.job(name, 'send', dev, skip, count, bs, self.a.nc_port, timeout=limit(total))
            else:
                self.t('send', dev, skip, count, bs, self.a.nc_port, timeout=limit(total, floor=600))
            thread.join(STALL + 60)
        finally:
            if thread.is_alive() and held.get('conn'):
                held['conn'].shutdown(socket.SHUT_RDWR)   # the tablet's side ended or failed: do not wait out the stall
                thread.join(30)
            server.close()
        if 'error' in got or 'bytes' not in got:
            raise Stop(f'piece {name} of /dev/{dev} did not arrive: {got.get("error", "the stream ended early")}')
        return got['bytes'], got['sha256'], got['stored_sha256'], got['stored_bytes']

    @staticmethod
    def read_back(path, compress, whole=None):
        """A stored piece read without the page cache (F_NOCACHE): (raw sha256, raw bytes, stored sha256). Inflated when gzip; WHOLE
        (a hashlib object) is fed the raw bytes too."""
        raw, stored, n = hashlib.sha256(), hashlib.sha256(), 0
        for chunk in inflate(path, stored) if compress == 'gzip' else read_nocache(path, stored):
            raw.update(chunk)
            n += len(chunk)
            if whole is not None:
                whole.update(chunk)
        return raw.hexdigest(), n, stored.hexdigest()

    def backup_root(self):
        """PIANO_BACKUP_DIR (the external SSD), a writable directory; None in a dry run."""
        raw = self.env.get('PIANO_BACKUP_DIR')
        if not self.execute:
            return None
        if not raw:
            raise Stop('PIANO_BACKUP_DIR is not set (~/.config/piano/env); the backup comes before any write')
        base = Path(os.path.expanduser(os.path.expandvars(raw)))
        if not base.is_dir() or not os.access(base, os.W_OK):
            raise Stop('PIANO_BACKUP_DIR is not a writable directory (is the backup disk mounted?)')
        return base

    def write_manifest(self, nas, backups):
        """SHA256SUMS (sha256sum -c: each stored file; a piecewise backup's pieces as stored) and manifest.json of a backup set."""
        lines = []
        for e in backups.values():
            if e.get('pieces'):
                lines += [f'{q["stored_sha256"]}  {q["file"]}\n' for q in e['pieces']]
            else:
                lines.append(f'{e["sha256"]}  {e["file"]}\n')
        (nas / 'SHA256SUMS').write_text(''.join(lines))
        (nas / 'manifest.json').write_text(json.dumps({'device': 'piano (Xiaomi Pad 8 Pro), UFS', 'backups': backups}, indent=2) + '\n')

    def backup(self, key, dev, skip, count, bs, what):
        """A verified NAS backup of one region (see the module doc, rule 1). Skipped when an identical verified one exists."""
        total = count * bs
        nas, name = self.nas()
        self.log(f'  backup {key}: {what}, /dev/{dev} blocks {skip}+{count} of {bs} bytes ({total} bytes) -> NAS {name}/{key}.img')
        want = self.region_sha(dev, skip, count, bs, name=f'sha-{key}')
        if not self.execute:
            self.t('send', dev, skip, count, bs, self.a.nc_port)
            self.log(f'  check received bytes and sha256 = the tablet\'s; NAS copy re-read without cache = the same')
            return None
        entry = self.state.get('backups', {}).get(key)
        if entry and entry.get('verified'):
            if entry['sha256'] == want:
                self.log(f'  ok    already backed up and verified (sha256 {want[:16]})')
                return want
            if self.written_by_install(key):
                self.log(f'  info  {key} was written by this install since its backup; the verified backup of before '
                         f'({entry["sha256"][:16]}) stays the rollback source')
                return entry['sha256']
            raise Stop(f'{key} changed since its verified backup ({entry["sha256"][:16]} -> {want[:16]}) and this install did not '
                       f'write it: find out why; --new-backup starts a new backup set')
        dests = [nas / f'{key}.img'] + ([self.dir / f'{key}.img'] if total <= SMALL else [])
        n, sha = self.receive(dev, skip, count, bs, dests)
        self.need(n == total and sha == want, f'{key}: received {n} bytes, sha256 {sha} = the tablet\'s {want}')
        again = self.sha_nocache(nas / f'{key}.img')
        self.need(again == want, f'{key}: NAS copy read back without cache: {again}')
        if total <= SMALL:
            self.need(self.sha_nocache(self.dir / f'{key}.img') == want, f'{key}: local copy session/{key}.img')
        entry = {'file': f'{key}.img', 'sha256': want, 'bytes': total, 'dev': dev, 'skip': skip, 'count': count, 'bs': bs,
                 'what': what, 'verified': True, 'time': time.strftime('%Y-%m-%d %H:%M:%S'), 'local': total <= SMALL}
        backups = dict(self.state.get('backups', {}), **{key: entry})
        self.save(backups=backups)
        self.write_manifest(nas, backups)
        return want

    def restore(self, key, dev, seek, bs):
        """Write a verified backup back (streamed from the NAS, or the local copy), then read it back."""
        entry = self.require_backup(key)
        nas, _ = self.nas()
        source = (self.dir / entry['file']) if self.execute and entry.get('local') else (nas / entry['file'] if nas else None)
        if self.execute:
            self.need(self.sha_nocache(source) == entry['sha256'], f'{key}: the backup still hashes to {entry["sha256"][:16]}')
        self.serve({f'{key}.img': source or Path(f'{key}.img')})
        if self.write(f'backup {key} -> /dev/{dev} at block {seek} of {bs} bytes'):
            self.job(f'restore-{key}', 'restore', f'{key}.img', dev, seek, bs, self.a.http_port, timeout=4 * 3600)
        count = (entry or {}).get('bytes', 0) // bs if entry else 0
        got = self.region_sha(dev, seek, count, bs, name=f'check-{key}')
        self.need(got == entry['sha256'] if self.execute else None, f'{key} restored: read back {got}')

    # -- fastboot (ABL: getvar, boot and reboot only; oem only to our own UEFI) -----------------------------------------
    def fastboot(self, *argv, serial=None, timeout=120, check=True):
        if argv[0] not in ('getvar', 'boot', 'reboot', 'oem', 'get_staged'):
            raise Stop(f'fastboot {argv[0]} is not used by this script')
        if argv[0] in ('oem', 'get_staged') and serial != 'piano-uefi':
            raise Stop('oem goes only to our own UEFI (piano-uefi), never to ABL')
        command = ['fastboot', '-s', serial or self.serial, *argv]
        self.log('  host$ ' + ' '.join(command))
        if not self.execute:
            return ''
        r = subprocess.run(command, capture_output=True, text=True, timeout=timeout, cwd=self.dir)
        out = (r.stdout + r.stderr).strip()
        if check and r.returncode:
            raise Stop(f'{" ".join(command)}: {out}')
        return out

    def devices(self):
        r = subprocess.run(['fastboot', 'devices'], capture_output=True, text=True, timeout=20)
        return {line.split()[0] for line in r.stdout.splitlines() if line.strip()}

    def wait(self, what, present, seconds):
        self.log(f'  host  wait up to {seconds} s for {what} {"to appear" if present else "to go"} (fastboot devices)')
        if not self.execute:
            return True
        t0 = time.time()
        while time.time() - t0 < seconds:
            if (what in self.devices()) == present:
                return True
            time.sleep(0.5)
        return False

    def wait_shell(self, seconds=180):
        host = self.env.get('PIANO_MAINT_HOST', '<PIANO_MAINT_HOST>')
        self.log(f'  host  wait up to {seconds} s for the maintenance shell (PIANO_MAINT_HOST port 2323)')
        if not self.execute:
            return
        t0 = time.time()
        while time.time() - t0 < seconds:
            try:
                socket.create_connection((host, 2323), timeout=3).close()
                return
            except OSError:
                time.sleep(2)
        raise Stop('no maintenance shell (USB NCM link up?)')

    def esp_reset_after(self):
        """The reset time of the default entry of the ESP on the tablet (the one written, else --esp). An ESP whose default does not
        reset (default macos) never comes back to ABL fastboot by itself, so the RAM tests refuse it."""
        name = self.state.get('esp_written') or self.a.esp
        rec = self.esp_record(name) or {}
        m = re.search(r'piano-reset-after=(\d+)', rec.get('cmdlines', {}).get(rec.get('default', ''), ''))
        if not m:
            raise Stop(f'the ESP on the tablet ({name}, default {rec.get("default")}) does not reset into ABL fastboot, so this RAM test '
                       f'would not come back: write the -diag ESP for session 2 (step esp --esp esp-<profile>-diag.img), the -macos one '
                       f'in session 3 (esp-final)')
        return int(m.group(1))


# ------------------------------------------------------------------------------------------------------------------------
# Steps

STEPS, ROLLBACKS = {}, {}


def step(name, kind, writes=None):
    def wrap(f):
        STEPS[name] = {'run': f, 'kind': kind, 'writes': writes, 'doc': (f.__doc__ or '').strip()}
        return f
    return wrap


def rollback(name):
    def wrap(f):
        ROLLBACKS[name] = {'run': f, 'doc': (f.__doc__ or '').strip()}
        return f
    return wrap


def map_nodes(s):
    r = s.t('map')
    if not r:
        return None
    nodes, sizes, starts = {'lun0': r['lun0']}, {}, {}
    for key, name in (('rootfs', 'linux_rootfs'), ('linux_data', 'linux_data'), ('esp', 'esp'), ('boot_a', 'boot_a'), ('boot_b', 'boot_b'),
                      ('vendor_boot_a', 'vendor_boot_a'), ('init_boot_a', 'init_boot_a')):
        fields = r.get(name, 'none').split()
        nodes[key] = fields[0] if fields != ['none'] else None
        if len(fields) == 3:
            sizes[key] = int(fields[2]) * 512
            if name in PART_KEYS:          # LUN 0's: the first LBA in 4096-byte blocks
                starts[key] = int(fields[1]) * 512 // B
    s.nodes, s.sizes, s.starts = nodes, sizes, starts
    s.save(nodes=nodes, sizes=sizes, starts=starts, map=r)
    return r


def part_geometry(s, name):
    """(node, first LBA, blocks) of LUN 0 partition NAME (4096-byte blocks) as the last map found it."""
    key = PART_KEYS.get(name)
    if not key:
        raise Stop(f'partition {name}: not one that tablet.sh map reports ({", ".join(PART_KEYS)})')
    return s.nodes.get(key), s.starts.get(key), (s.sizes.get(key) or 0) // B


def bound_store(s, record):
    """The store an ESP of a bound-store record (esp_store) resumes, read from the tablet: the window inside its partition as mapped
    and at NativeUfs's place there for its size (the end of the partition below the reserve: native_ufs_store_open), the header
    valid, of the record's seed, bound to that LBA (PersistOverlay.h kBound). Returns (True/False, the line; None in a dry run).
    The line of a store that passes reads "... the store at LBA N on PART ... IS the one this ESP resumes ..."."""
    store, seed = esp_store(record), record['seed']
    first, blocks, part = store['first_lba'], store.get('blocks') or seed['store_blocks'], store['partition']
    node, start, size = part_geometry(s, part)
    if not s.execute:
        return None, (f'the store at LBA {first} on {part} (a {blocks}-block window at NativeUfs\'s place in {part} as mapped) is valid, '
                      f'of seed {seed["identity_sha256"][:16]}, bound to LBA {first}: then it IS the one this ESP resumes')
    problems, header = [], {}
    if not node or not size:
        problems.append(f'the map has no partition {part}')
    else:
        last = start + size - 1
        if not start <= first <= first + blocks - 1 <= last:
            problems.append(f'its window LBA {first}-{first + blocks - 1} is not inside {part} (LBA {start}-{last})')
        elif common.store_first(last, blocks) != first:
            problems.append(f'NativeUfs puts a {blocks}-block window on {part} as mapped (LBA {start}-{last}) at LBA '
                            f'{common.store_first(last, blocks)}, not {first}: the binding would refuse it')
    if not problems:
        header = common.store_header(bytes.fromhex(s.t('peek', node, first - start, 0, 128)['hex']))
        if header['state'] != 'valid':
            problems.append(f'its header is {header["state"]} (no store of this seed there)')
        else:
            if header.get('bound_lba') != first:
                problems.append(f'it is bound to {header.get("bound_lba", "no LBA (unbound)")}, not LBA {first}')
            if not common.store_usable(header, seed):
                problems.append(f'it is of seed {header["identity"][:16]} ({header["pages"]} pages), not {seed["identity_sha256"][:16]} '
                                f'({seed["pages"]} pages)')
    s.save(esp_store={'partition': part, 'first_lba': first, 'node': node, 'header': header, 'problems': problems})
    if problems:
        return False, (f'the store at LBA {first} on {part} is NOT the one this ESP resumes (seed {seed["identity_sha256"][:16]}): '
                       + '; '.join(problems) + ': its boots would keep it and run Data VOLATILE, or not find it')
    return True, (f'the store at LBA {first} on {part} (seed {header["identity"][:16]}, generation {header["generation"]}, bound to LBA '
                  f'{header["bound_lba"]}) IS the one this ESP resumes (seed {seed["identity_sha256"][:16]})')


def root_manifest(s, part, below_lba=None):
    """Block 0 of partition PART: (sha256, the SHNGROOT manifest's image bytes or None, the first 8 bytes); a dry run (None, None, None).
    BELOW_LBA: the root image + 1024 blocks must end below it (the NativeUfs rule; the store window's first LBA)."""
    node, start, _ = part_geometry(s, part)
    digest = s.region_sha(node, 0, 1, name=f'sha-{part}-0')
    h = s.t('peek', node, 0, 0, 24)
    if not h:
        return None, None, None
    raw = bytes.fromhex(h['hex'])
    image = int.from_bytes(raw[16:24], 'little') if raw[:8] == b'SHNGROOT' else None
    return digest, image, raw[:8]


def entries_using_rootfs(s, record):
    """{entry id: why} of the ESP's entries that use linux_rootfs (common.entry_uses_rootfs), read from the image file itself when it
    is in OUT and hashes to its record (the bytes the tablet holds), else from the record's unixnu.toml."""
    text = record.get('unixnu_toml', '')
    image = OUT / record['image']
    if image.is_file():
        if common.sha(image) != record['sha256']:
            raise Stop(f'{image.name} does not hash to its record\'s {record["sha256"][:16]}')
        try:
            text = common.fat32_read(image)['files']['EFI/UniXNU/unixnu.toml'].decode()
        except (ValueError, IndexError, KeyError, struct.error):
            raise Stop(f'{image.name}: no unixnu.toml readable in the image')
        if record.get('unixnu_toml') and text != record['unixnu_toml']:
            raise Stop(f'{image.name}: its unixnu.toml is not its record\'s')
    elif s.execute:
        raise Stop(f'{record["image"]} is not in {OUT}: its menu cannot be read from the bytes on the tablet')
    import tomllib   # Python 3.11 (here only: the other steps run on older ones too)
    entries = tomllib.loads(text).get('entry', []) if text else []
    return {e['id']: why for e in entries if (why := common.entry_uses_rootfs(e))}


@step('linux-data-root', 'maintenance, WRITE linux_data from its first block (a second root image)',
      writes='LUN 0 partition linux_data, from its first block, the root image\'s size')
def linux_data_root(s):
    """--root-image IMG (a root image: manifest and windows, docs/drivers/ufs.md; the macOS 27.3 line's) streamed onto
    linux_data from its first block, then read back from the medium. The owner (2026-11-12) gave Linux's space to macOS and does
    not need linux_data's old contents, so there is no backup of them; the image is checked against --root-sha256 on the host
    first (without the page cache) and again on the medium after the write. Nothing else is written: no GPT change, linux_rootfs
    (the installed system's windows and the owner's Data store) untouched."""
    map_nodes(s)
    dev, size = s.nodes.get('linux_data'), s.sizes.get('linux_data')
    s.need(bool(dev) and bool(size) if s.execute else None, f'linux_data found: /dev/{dev}, {size} bytes')
    if not s.a.root_image or not s.a.root_sha256:
        raise Stop('linux-data-root needs --root-image IMG and --root-sha256 HEX')
    img = Path(s.a.root_image).expanduser()
    n = img.stat().st_size if img.is_file() else 0
    s.need(n > 0 and n % B == 0 and (not size or n <= size), f'{img.name}: {n} bytes, a whole number of {B}-byte blocks inside linux_data')
    if s.execute:
        s.need(s.sha_nocache(img) == s.a.root_sha256.lower(), f'{img.name} hashes to {s.a.root_sha256[:16]} on the host')
    s.serve({'root.img': img})
    if s.write(f'{img.name} -> /dev/{dev} from block 0 ({n // B} blocks of {B} bytes)'):
        s.job('root-write', 'restore', 'root.img', dev, 0, B, s.a.http_port, timeout=4 * 3600)
    got = s.region_sha(dev, 0, n // B, B, name='root-check')
    s.need(got == s.a.root_sha256.lower() if s.execute else None, f'linux_data read back over {n} bytes: {got} = {s.a.root_sha256[:16]}')


@step('k273-store', 'maintenance, read only')
def k273_store(s):
    """The 27.3 entry's Data store on linux_data (tooling/k273/build-dual-esp.py, docs/k273-line.md §6), from --esp's record
    ("k273.store"): the window lies inside linux_data, after the root image and below the reserve, clear of linux_rootfs; its header
    block is blank (no SHNGDATA: before the entry's first data-persist=2 boot) or a valid store of the record's seed bound to that
    LBA (PersistOverlay.h kBound). The installed store's header in linux_rootfs is read too: unbound, the installed seed's."""
    record = s.esp_record(s.a.esp)
    k = (record or {}).get('k273') or ({'store': esp_store(record), 'seed': record['seed']} if esp_store(record) else None)
    if not k:
        raise Stop(f'{s.a.esp}: its record has no k273 entry (tooling/k273/build-dual-esp.py) and no store elsewhere (build-k273-esp.py)')
    store = k['store']
    first, blocks = store['first_lba'], store['blocks']
    root_end = (store.get('root_last_lba') or 0) + 1 or None
    s.need(common.DATA_FIRST <= first and first + blocks <= common.DATA_LAST + 1 - common.RESERVE_BLOCKS,
           f'store window LBA {first}-{first + blocks - 1} inside linux_data ({common.DATA_FIRST}-{common.DATA_LAST}) below the reserve')
    if root_end:
        s.need(first >= root_end, f'store window after the root image (which ends before LBA {root_end})')
    s.need(first > common.ROOTFS_LAST, 'store window clear of linux_rootfs')
    map_nodes(s)
    dev = s.nodes.get('linux_data')
    h = s.t('peek', dev, first - common.DATA_FIRST, 0, 128) if dev else None
    if h:
        header = common.store_header(bytes.fromhex(h['hex']))
        s.log(f'  info  27.3 store header at LBA {first}: {header}')
        ok = header['state'] == 'blank' or (header['state'] == 'valid' and header.get('bound_lba') == first and
                                             header['identity'][:16] == k['seed']['identity_sha256'][:16])
        s.need(ok, f'27.3 store: blank, or bound to LBA {first} with seed {k["seed"]["identity_sha256"][:16]}')
    h = s.t('peek', s.nodes['rootfs'], rel(store_new(s)), 0, 128)
    if h:
        header = common.store_header(bytes.fromhex(h['hex']))
        s.log(f'  info  installed store header at LBA {store_new(s)}: {header}')
        s.need(header['state'] != 'valid' or 'bound_lba' not in header, 'installed store unbound (only the 27.3 kernel binds)')


@step('abl-check', 'ABL fastboot, read only')
def abl_check(s):
    """fastboot getvar from ABL (read only): identity, unlock, slot A state, battery, boot_a size."""
    values = {}
    for var in list(ABL_EXPECT) + list(ABL_SHOW):
        out = s.fastboot('getvar', var, check=False)
        m = re.search(rf'{re.escape(var)}:\s*(\S+)', out or '')
        values[var] = m.group(1) if m else None
        if var in ABL_EXPECT and var != 'slot-successful:a':
            s.need(values[var] == ABL_EXPECT[var] if s.execute else None, f'{var} = {ABL_EXPECT[var]} (got {values[var]})')
        else:
            s.log(f'  info  {var}: {values[var]}')
    if s.execute:
        s.save(abl=values)
        if values.get('slot-successful:a') != 'yes':
            s.log('  NOTE  slot A is not marked successful: run step slot-success in session 3 (never set_active)')


@step('maint-boot', 'ABL fastboot -> maintenance RAM image (fastboot boot)')
def maint_boot(s):
    """fastboot boot the Ubuntu release's maintenance RAM image, wait for its shell, set the idle limit to 3600 s, install
    tablet.sh (sha256 checked)."""
    s.need(MAINT_IMAGE.is_file() and common.sha(MAINT_IMAGE) == MAINT_IMAGE_SHA256, f'{MAINT_IMAGE.name} sha256 {MAINT_IMAGE_SHA256[:16]}')
    if not s.wait(s.serial, True, 120):
        raise Stop('ABL fastboot not found (power + volume-down 15-20 s)')
    s.fastboot('boot', str(MAINT_IMAGE), timeout=180)
    s.wait_shell()
    s.shell('piano-maint idle 3600')
    out = s.shell('. /etc/piano-network; echo "host_ip=$PIANO_HOST_IP"')
    if out is not None:
        m = re.search(r'host_ip=(\S+)', out)
        s.need(m and m.group(1) == s.host_ip(), f'the tablet fetches from {m and m.group(1)}: this host\'s USB-link address')
    # tablet.sh is not on the tablet yet: its own 'fetch' cannot bring it. Bootstrap it with busybox wget + sha256sum.
    digest = common.sha(TABLET_SH)
    s.serve({'tablet.sh': TABLET_SH})
    out = s.shell(f'mkdir -p /run/pp && rm -f /run/pp/tablet.sh && wget -q -O /run/pp/tablet.sh http://{s.host_ip()}:{s.a.http_port}/tablet.sh '
                  f'&& sha256sum /run/pp/tablet.sh', 300)
    if out is not None:
        s.need(digest in out, f'tablet.sh fetched, sha256 {digest[:16]}')
    s.shell('piano-maint status')


@step('map', 'maintenance, read only')
def map_step(s):
    """Map and check before any write: LUN 0 geometry, both GPT copies (old or new tables, nothing else), the boot partitions,
    the root manifest and image size, both Data store places with their seed identity (against the ESP's), the ESP region.
    With an ESP whose store is elsewhere (--esp of build-k273-esp.py: the 27.3 store on linux_data, bound): its root's manifest
    and its store (bound_store) instead; linux_rootfs (the 27.0.1 root and store, or nothing once cleared) is then only reported."""
    r = map_nodes(s)
    elsewhere = esp_store(s.esp)
    if r:
        s.need(r['lun0_block_size'] == '4096' and r['lun0_bytes'] == str(LUN0_BYTES), f'LUN 0 {r["lun0"]}: 4096-byte blocks, {LUN0_BYTES} bytes')
        s.need(r['mounted'] == '0', 'nothing from UFS is mounted')
        start, size = (int(x) for x in r['linux_rootfs'].split()[1:]) if r['linux_rootfs'] != 'none' else (None, None)
        geometry = start == common.ROOTFS_FIRST * 8 and size in ((common.ROOTFS_LAST - common.ROOTFS_FIRST + 1) * 8,
                                                                 (common.ROOTFS_NEW_LAST - common.ROOTFS_FIRST + 1) * 8)
        if elsewhere:
            s.log(f'  info  linux_rootfs {r["linux_rootfs"]} (512-byte units): no entry of {s.esp["image"]} uses it')
        else:
            s.need(geometry, f'linux_rootfs {r["linux_rootfs"]} (512-byte units)')
        s.need(r['boot_a'].split()[1:] == [str(common.BOOT_A_START_512), str(common.BOOT_A_SIZE_512)], f'boot_a {r["boot_a"]}')
        s.need(r['boot_b'] != 'none', f'boot_b {r["boot_b"]} (backed up only)')
    lun0, rootfs = s.nodes['lun0'], s.nodes['rootfs']
    head, tail = s.region_sha(lun0, 0, 6), s.region_sha(lun0, TAIL_LBA, 5)
    new = (s.gpt or {}).get('gpt-head.bin', {}).get('sha256'), (s.gpt or {}).get('gpt-tail.bin', {}).get('sha256')
    if s.execute:
        state = 'old' if (head, tail) == (common.GPT_PRIMARY_SHA256, common.GPT_BACKUP_SHA256) else 'new' if (head, tail) == new else 'unknown'
        s.need(state != 'unknown', f'GPT copies are the known old (CRITICAL-BACKUP) or the new (gpt.json) tables: {state}')
        s.save(gpt_state=state, gpt_head=head, gpt_tail=tail)
    else:
        s.log(f'  check GPT head/tail = {common.GPT_PRIMARY_SHA256[:16]}/{common.GPT_BACKUP_SHA256[:16]} (old) or gpt.json\'s (new)')
    signed = ((s.boot or {}).get('signed') or {}).get(s.a.boot_image, {}).get('sha256')
    for key, digest in (('boot_a', common.BOOT_A_SHA256), ('vendor_boot_a', common.VENDOR_BOOT_A_SHA256),
                        ('init_boot_a', common.INIT_BOOT_A_SHA256), ('boot_b', None)):
        got = s.region_sha(s.nodes[key], 0, s.sizes[key] // MiB, MiB, name=f'sha-{key}')
        if key in ('vendor_boot_a', 'init_boot_a'):
            s.need(got == digest if s.execute else None, f'{key} = slot A\'s current image {digest[:16]}')
        elif not s.execute:
            s.log(f'  info  {key} sha256 is recorded ({"Ubuntu 13-boot_a " + digest[:16] + " or the macOS image" if digest else "never written"})')
        elif key == 'boot_a':
            name, bound = s.boot_identity(got)
            s.log(f'  info  boot_a sha256 {got} ({name}' + (f', DarwinBoot window up to {bound >> 20} MiB' if bound else '') +
                  (', the image this session writes' if got == signed else '') + ')')
        else:
            s.log(f'  info  boot_b sha256 {got} (never written)')
        if s.execute:
            s.save(**{f'{key}_sha256': got})
    s.save(manifest_sha256=s.region_sha(rootfs, 0, 1)) if s.execute else s.region_sha(rootfs, 0, 1)
    h = s.t('peek', rootfs, 0, 0, 24)
    if h:
        raw = bytes.fromhex(h['hex'])
        image_bytes = int.from_bytes(raw[16:24], 'little')
        if elsewhere:
            s.log(f'  info  linux_rootfs block 0: ' + ('the SHNGROOT manifest of a root image of ' + str(image_bytes) + ' bytes (the 27.0.1 '
                  'root)' if raw[:8] == b'SHNGROOT' else 'zero (cleared)' if not any(raw) else f'no manifest ({raw[:8].hex()})'))
        else:
            s.need(raw[:8] == b'SHNGROOT', 'linux_rootfs block 0 is the SHNGROOT manifest')
            room = (store_new(s) - common.ROOTFS_FIRST) * B
            s.need(image_bytes + 1024 * B <= room, f'root image ({image_bytes} bytes) + 1024 blocks below the new store place (NativeUfs rule)')
            s.save(image_bytes=image_bytes)
    if elsewhere:
        # this ESP's root: the manifest at block 0 of the store's partition, the image + 1024 blocks below the store window
        part = elsewhere['partition']
        digest, image_bytes, magic = root_manifest(s, part)
        if magic is not None:
            _, start, _ = part_geometry(s, part)
            s.need(image_bytes is not None, f'{part} block 0 is the SHNGROOT manifest (the root of {s.esp["image"]})')
            s.need(image_bytes is not None and image_bytes + 1024 * B <= (elsewhere['first_lba'] - start) * B,
                   f'root image ({image_bytes} bytes) + 1024 blocks below the store window at LBA {elsewhere["first_lba"]} (NativeUfs rule)')
            s.save(root_manifest={'partition': part, 'sha256': digest, 'image_bytes': image_bytes})
    rootfs_first, rootfs_blocks = s.starts.get('rootfs', common.ROOTFS_FIRST), (s.sizes.get('rootfs') or 0) // B
    for where, first in (('old', store_old(s)), ('new', store_new(s))):
        if s.execute and not rootfs_first <= first < rootfs_first + rootfs_blocks:
            s.log(f'  info  the {where} store place LBA {first} is outside linux_rootfs as mapped: not read')
            continue
        h = s.t('peek', rootfs, first - rootfs_first, 0, 128)   # the version 1 header and its extension (docs/data-store.md)
        if h:
            raw = bytes.fromhex(h['hex'])
            info = {'first_lba': first, 'magic': raw[:8].decode(errors='replace'), 'pages': int.from_bytes(raw[16:24], 'little'),
                    'identity': raw[24:56].hex(), 'generation': int.from_bytes(raw[56:60], 'little')}
            header = common.store_header(raw)
            info.update(state=header['state'], self_contained=header.get('self_contained', False), flags=header.get('flags', 0))
            s.log(f'  info  store header at the {where} place (LBA {first}): {info}')
            s.save(**{f'store_{where}': info})
    old = s.state.get('store_old') or {}
    if elsewhere:
        s.save(store_matches_esp=False)
        ok, text = bound_store(s, s.esp)
        s.log(('  check ' if ok is None else '  ok    ' if ok else '  WARN  ') + text +
              ('' if ok is not False else '; write_esp refuses this ESP'))
        current = s.state.get('store_new') or {}
        if current.get('state') == 'valid':
            s.log(f'  info  the 27.0.1 store at LBA {store_new(s)} on linux_rootfs (seed {current["identity"][:16]}, generation '
                  f'{current["generation"]}): no entry of this ESP opens it')
        return
    if s.esp and old:
        # A store the ESP's NativeRamBlock resumes: made for its seed, or self-contained (flattened: any seed of its size).
        same = old.get('magic') == 'SHNGDATA' and old.get('pages') == s.esp['seed']['pages'] and \
            (old.get('identity') == s.esp['seed']['identity_sha256'] or old.get('self_contained', False))
        s.log(f'  info  Data store {"IS" if same else "is NOT"} usable with the ESP\'s seed (identity {s.esp["seed"]["identity_sha256"][:16]}'
              f'{", the store is self-contained" if old.get("self_contained") else ""}): --store auto will '
              f'{"move it" if same else "let it start fresh"}')
        s.save(store_matches_esp=same)
    current = s.state.get('store_new') or {}
    if s.esp and s.state.get('gpt_state') == 'new' and current.get('state') == 'valid':
        # Installed: the store lives at the new place, and it is the one this ESP's first boot opens. Its seed must be this ESP's (the
        # store's base, docs/data-store.md section 9), else that boot keeps the store untouched and runs Data volatile.
        usable = common.store_usable(current, s.esp['seed'])
        s.log(f'  {"ok   " if usable else "WARN "} the store at LBA {store_new(s)} (seed {current["identity"][:16]}, generation '
              f'{current["generation"]}) {"IS" if usable else "is NOT"} the one this ESP resumes (seed {s.esp["seed"]["identity_sha256"][:16]}'
              f'{", the store is self-contained" if current.get("self_contained") else ""})' +
              ('' if usable else ': its boots would keep it and run Data VOLATILE; write_esp refuses such an ESP unless its build '
               'record has --replace-data'))


def store_plan(s):
    mode = s.a.store
    if mode == 'auto':
        mode = 'move' if s.state.get('store_matches_esp') else 'fresh'
    return mode


@step('backup', 'maintenance, read only on the tablet; writes the NAS backups')
def backup_step(s):
    """Verified NAS backups (rule 1) of every region a later step writes, and of the boot partitions: LUN 0 primary GPT
    (LBA 0-5) and backup GPT (last 5 LBAs), boot_a, boot_b, vendor_boot_a, init_boot_a, the linux_rootfs tail that becomes the
    ESP (LBA 71527424-71789567, 1 GiB), and, when the store moves, the store region [68119455, 71527424) (old and new place,
    13.96 GB). Idempotent: a verified backup with the same SHA-256 is not exported again."""
    if s.execute and 'gpt_state' not in s.state:
        raise Stop('run step map first')
    map_nodes(s)   # node names can change from boot to boot
    lun0 = s.nodes['lun0']
    s.backup('lun0-gpt-primary', lun0, 0, 6, B, 'LUN 0 LBA 0-5: protective MBR, primary GPT header and entries')
    s.backup('lun0-gpt-backup', lun0, TAIL_LBA, 5, B, f'LUN 0 LBA {TAIL_LBA}-{common.LUN0_BLOCKS - 1}: backup GPT entries and header')
    if s.execute and s.state.get('gpt_state') == 'old':
        s.need(s.state['backups']['lun0-gpt-primary']['sha256'] == common.GPT_PRIMARY_SHA256 and
               s.state['backups']['lun0-gpt-backup']['sha256'] == common.GPT_BACKUP_SHA256, 'the GPT backups equal CRITICAL-BACKUP new-primary/new-backup.bin')
    for key in ('boot_a', 'boot_b', 'vendor_boot_a', 'init_boot_a'):
        s.backup(key, s.nodes[key], 0, s.sizes[key] // MiB, MiB, f'partition {key} (whole)')
    s.backup('linux_rootfs-tail-esp', lun0, common.ESP_FIRST, common.ESP_BLOCKS, B,
             f'LUN 0 LBA {common.ESP_FIRST}-{common.ESP_FIRST + common.ESP_BLOCKS - 1}: the end of linux_rootfs that becomes partition 37')
    plan = store_plan(s)
    s.log(f'  info  Data store: {plan} (--store {s.a.store})')
    if plan == 'move':
        s.backup('linux_rootfs-store', lun0, store_new(s), store_old(s) + store_window(s) - store_new(s), B,
                 f'LUN 0 LBA {store_new(s)}-{store_old(s) + store_window(s) - 1}: the Data store (old place) and its new place')
    else:
        s.backup('linux_rootfs-store-header', lun0, store_old(s), 1, B, f'LUN 0 LBA {store_old(s)}: the old store header (fresh store)')
    if s.execute:
        s.save(store_plan=plan)


@step('gpt', 'maintenance, WRITE LUN 0 GPT (backup copy first, then the primary)', writes='LUN 0 LBA 0-5 and 124552187-124552191')
def gpt_step(s):
    """The new tables (out/persistent/gpt-head.bin, gpt-tail.bin; gpt-diff.txt): linux_rootfs ends at 71527423, partition 37 esp
    at 71527424-71789567. Each copy read back; then the kernel rereads the table, sgdisk verifies it, sysfs shows both."""
    map_nodes(s)
    lun0 = s.nodes['lun0']
    if s.execute and s.state.get('gpt_state') == 'new':
        s.log('  info  the GPT is already the new one')
        return
    s.require_backup('lun0-gpt-primary', s.state.get('gpt_head') if s.execute else None)
    s.require_backup('lun0-gpt-backup', s.state.get('gpt_tail') if s.execute else None)
    head, tail = s.gpt['gpt-head.bin']['sha256'], s.gpt['gpt-tail.bin']['sha256']
    s.fetch('gpt-tail.bin', OUT / 'gpt-tail.bin', tail)
    s.fetch('gpt-head.bin', OUT / 'gpt-head.bin', head)
    if s.write(f'gpt-tail.bin -> /dev/{lun0} LBA {TAIL_LBA} (backup GPT)'):
        s.t('write', 'gpt-tail.bin', lun0, TAIL_LBA, B)
    got = s.region_sha(lun0, TAIL_LBA, 5)
    s.need(got == tail if s.execute else None, f'backup GPT read back {got} = {tail[:16]}')
    if s.write(f'gpt-head.bin -> /dev/{lun0} LBA 0 (protective MBR + primary GPT)'):
        s.t('write', 'gpt-head.bin', lun0, 0, B)
    got = s.region_sha(lun0, 0, 6)
    s.need(got == head if s.execute else None, f'primary GPT read back {got} = {head[:16]}')
    if s.execute:
        s.save(gpt_state='new')
    s.t('rereadpt', lun0)
    v = s.shell(f'sgdisk -v /dev/{lun0}')
    s.need('No problems found' in v if v is not None else None, 'sgdisk -v: No problems found')
    s.shell(f'sgdisk -p /dev/{lun0} | tail -4')
    r = map_nodes(s)
    if r:
        s.need(r['linux_rootfs'].split()[1:] == [str(common.ROOTFS_FIRST * 8), str((common.ROOTFS_NEW_LAST - common.ROOTFS_FIRST + 1) * 8)],
               f'linux_rootfs now {r["linux_rootfs"]}')
        s.need(r['esp'] != 'none' and r['esp'].split()[1:] == [str(common.ESP_FIRST * 8), str(common.ESP_BLOCKS * 8)], f'esp {r["esp"]}')


@rollback('gpt')
def gpt_back(s):
    """The tables of before (the verified backups lun0-gpt-backup, then lun0-gpt-primary = CRITICAL-BACKUP new-backup.bin /
    new-primary.bin), each read back; the kernel rereads them. Undo `store` first if it moved the store (or accept fresh Data)."""
    map_nodes(s)
    lun0 = s.nodes['lun0']
    s.restore('lun0-gpt-backup', lun0, TAIL_LBA, B)
    s.restore('lun0-gpt-primary', lun0, 0, B)
    s.t('rereadpt', lun0)
    s.shell(f'sgdisk -v /dev/{lun0}; sgdisk -p /dev/{lun0} | tail -3')
    if s.execute:
        s.save(gpt_state='old')


@step('store', 'maintenance, WRITE linux_rootfs (the Data store 1 GiB down) or nothing (fresh)', writes='LUN 0 LBA 68119455-71265279')
def store_step(s):
    """--store move: the store window copied 262144 blocks down inside linux_rootfs (dd, forward: the destination is below the
    source, so every block is read before it is overwritten), then read back against the store's SHA-256 taken just before.
    --store fresh: nothing is written; NativeRamBlock starts a new store at the new place on the first boot (state 3).
    Restart or undo: the verified backup linux_rootfs-store (rollback store)."""
    plan = s.state.get('store_plan') or store_plan(s)
    if plan != 'move':
        s.log('  info  fresh Data store: nothing to write')
        return
    if s.execute and s.state.get('gpt_state') != 'new':
        raise Stop('run step gpt first (the new place is computed from the new partition end)')
    if s.execute and s.state.get('store_moved'):
        s.log('  info  already moved')
        return
    map_nodes(s)
    rootfs, window = s.nodes['rootfs'], store_window(s)
    union = s.region_sha(s.nodes['lun0'], store_new(s), store_old(s) + window - store_new(s), name='store-region-sha')
    s.require_backup('linux_rootfs-store', union if s.execute else None)
    src = s.region_sha(rootfs, rel(store_old(s)), window, name='store-sha')
    s.log(f'  info  store window sha256 before the move: {src}')
    if s.write(f'dd /dev/{rootfs} block {rel(store_old(s))} -> {rel(store_new(s))}, {window} blocks (LUN 0 LBA {store_old(s)} -> {store_new(s)})'):
        s.job('store-move', 'copy', rootfs, rel(store_old(s)), rel(store_new(s)), window, timeout=7200)
    got = s.region_sha(rootfs, rel(store_new(s)), window, name='store-check')
    s.need(got == src if s.execute else None, f'store at LBA {store_new(s)}: read back {got} = before the move')
    h = s.t('peek', rootfs, rel(store_new(s)), 0, 8)
    s.need(bytes.fromhex(h['hex']) == b'SHNGDATA' if h else None, 'SHNGDATA header at the new place')
    if s.execute:
        s.save(store_moved=True, store_sha256=src)


@rollback('store')
def store_back(s):
    """The store region [68119455, 71527424) as backed up (linux_rootfs-store, from the NAS), read back."""
    map_nodes(s)
    s.restore('linux_rootfs-store', s.nodes['lun0'], store_new(s), B)
    if s.execute:
        s.save(store_moved=False)


@step('store-backup', 'maintenance, read only on the tablet; writes the NAS backup of the installed Data store')
def store_backup(s):
    """Optional, on an installed tablet (the new GPT, the store at its place): a verified NAS backup of the Data store as it is now,
    the window at LBA 68119455 (3145825 blocks, 12.9 GB: the owner's Data at this moment), e.g. before the first boot of a new ESP.
    Read only on the tablet; about 7 minutes to the NAS and as long to read it back. Its own backup (linux_rootfs-store-now), not
    the move's (linux_rootfs-store); in a backup set that has one already, a store that changed since stops it (--new-backup).
    rollback store-backup puts the owner's Data back to this moment."""
    if s.execute and s.state.get('gpt_state') != 'new':
        raise Stop('the store lives at its installed place only with the new GPT (an installed tablet): run step map first')
    current = s.state.get('store_new') or {}
    if s.execute and current.get('state') != 'valid':
        raise Stop(f'no valid store header at LBA {store_new(s)} (step map read {current.get("magic", "nothing")}): nothing to back up')
    map_nodes(s)
    s.backup('linux_rootfs-store-now', s.nodes['lun0'], store_new(s), store_window(s), B,
             f'LUN 0 LBA {store_new(s)}-{store_new(s) + store_window(s) - 1}: the installed Data store (seed '
             f'{current.get("identity", "?")[:16]}, generation {current.get("generation", "?")})')


@rollback('store-backup')
def store_backup_back(s):
    """The installed Data store as step store-backup saved it (linux_rootfs-store-now, from the NAS), read back: the owner's Data
    as it was then; whatever changed since is gone. Only with the ESP of that store's seed on the tablet afterwards."""
    map_nodes(s)
    s.restore('linux_rootfs-store-now', s.nodes['lun0'], store_new(s), B)


def write_esp(s, image_name):
    record = s.esp_record(image_name)
    if not record:
        raise Stop(f'{image_name}: no build record in out/persistent (tooling/persist/build-esp.py)')
    lun0 = s.nodes['lun0']
    r = map_nodes(s)
    if r:
        s.need(r['esp'] != 'none' and r['esp'].split()[1:] == [str(common.ESP_FIRST * 8), str(common.ESP_BLOCKS * 8)], f'esp is {r["esp"]}')
    esp = s.nodes.get('esp') or 'sda37'
    if s.state.get('esp_written'):
        s.require_backup('linux_rootfs-tail-esp')   # the region holds an ESP image of this install; the backup is of before
    else:
        now = s.region_sha(lun0, common.ESP_FIRST, common.ESP_BLOCKS, name='esp-region-sha')
        s.require_backup('linux_rootfs-tail-esp', now if s.execute else None)
    current = s.state.get('store_new') or {}
    if esp_store(record):
        # the 27.3-only ESP (build-k273-esp.py): the store it resumes is on its own partition, bound to its LBA; read it now
        ok, text = bound_store(s, record)
        s.need(ok, text)
    elif s.state.get('gpt_state') == 'new' and current.get('state') == 'valid' and not common.store_usable(current, record['seed']) \
            and not (record.get('data_store') or {}).get('replace'):
        raise Stop(f'{record["image"]}: its seed {record["seed"]["identity_sha256"][:16]} is not the seed of the store at LBA {store_new(s)} '
                   f'({current["identity"][:16]}, as step map read it): every boot of it would keep that store untouched and run the '
                   'owner\'s Data volatile. Build it with the store\'s base (build-esp.py --seed base, the daily profile\'s default; '
                   'docs/data-store.md section 9), or with --replace-data if the owner decided to start Data over')
    window = s.esp_window(record)
    if window and window > common.OLD_DARWINBOOT_WINDOW_MAX:   # the pair rule: boot_a's DarwinBoot (read from the tablet) takes it
        s.pair(s.tablet_boot_a(), record, f'{record["image"]} asks for {window >> 20} MiB')
    else:
        s.log(f'  info  {record["image"]}: RAM window {window >> 20 if window else "-"} MiB, which every DarwinBoot takes')
    s.fetch(record['image'], OUT / record['image'], record['sha256'])
    if s.write(f'{record["image"]} (default {record["default"]}, profile {record.get("profile")}) -> /dev/{esp} (LUN 0 LBA {common.ESP_FIRST}, 1 GiB)'):
        s.job('esp-write', 'write', record['image'], esp, 0, MiB, timeout=1800)
    got = s.region_sha(esp, 0, 1024, MiB, name='esp-check')
    got2 = s.region_sha(lun0, common.ESP_FIRST * B // MiB, 1024, MiB, name='esp-check-lun0')
    s.need(got == record['sha256'] and got2 == record['sha256'] if s.execute else None,
           f'ESP read back through /dev/{esp} and /dev/{lun0}: {got} / {got2} = {record["sha256"][:16]}')
    s.sim['esp'] = record['sha256']
    if s.execute:
        s.save(esp_written=record['image'], esp_sha256=record['sha256'])
        # The installed ESP's seed, which build-ram-images.py and build-esp.py compare theirs with (common.installed_seed).
        common.write_json(OUT / 'installed.json', {'image': record['image'], 'sha256': record['sha256'], 'seed': record['seed'],
                                                   'inputs': record.get('inputs'), 'written': time.strftime('%Y-%m-%d %H:%M:%S')})


@step('esp', 'maintenance, WRITE partition 37 (test-phase ESP)', writes='LUN 0 LBA 71527424-71789567')
def esp_step(s):
    """The ESP image (--esp, default esp-base-diag.img: default entry macos-diag) onto partition 37; read back through the
    partition and through LUN 0."""
    write_esp(s, s.a.esp)


@rollback('esp')
def esp_back(s):
    """The linux_rootfs tail as backed up (linux_rootfs-tail-esp, 1 GiB from the NAS), read back. Refused while boot_a's DarwinBoot
    would not take the backed-up ESP's RAM window (the pair rule)."""
    map_nodes(s)
    entry = s.state.get('backups', {}).get('linux_rootfs-tail-esp') or {}
    record = s.esp_by_sha(entry.get('sha256'))
    window = s.esp_window(record)
    if window and window > common.OLD_DARWINBOOT_WINDOW_MAX:
        s.pair(s.tablet_boot_a(), record, f'the backed-up ESP {record["image"]} asks for {window >> 20} MiB')
    if record:
        # an ESP whose default entry boots from linux_rootfs needs its root there: after rootfs-clear, rollback rootfs-clear first
        why = common.entry_uses_rootfs({'type': 'xnu', 'args': (record.get('cmdlines') or {}).get(record.get('default'), '')})
        if why:
            _, image, magic = root_manifest(s, 'linux_rootfs')
            cleared = image is None if magic is not None else bool(s.state.get('rootfs_cleared'))
            s.need(not cleared, f'the backed-up ESP {record["image"]}\'s default entry {record.get("default")} {why}, which ' +
                   ('holds no root image now: rollback rootfs-clear first' if cleared else 'still holds its root image'))
    s.restore('linux_rootfs-tail-esp', s.nodes['lun0'], common.ESP_FIRST, B)
    s.sim['esp'] = entry.get('sha256')
    if s.execute:
        if record:   # an ESP of a build record (an earlier install's): the installed seed is its seed again
            s.save(esp_written=record['image'], esp_sha256=record['sha256'])
            common.write_json(OUT / 'installed.json', {'image': record['image'], 'sha256': record['sha256'], 'seed': record['seed'],
                                                       'inputs': record.get('inputs'), 'written': time.strftime('%Y-%m-%d %H:%M:%S'),
                                                       'by': 'rollback esp'})
        else:
            s.save(esp_written=None, esp_sha256=None)
            (OUT / 'installed.json').unlink(missing_ok=True)   # the region holds what it held before this install


# esp-final writes the same region as esp, so its rollback is the same (the step's own hint names it)
ROLLBACKS['esp-final'] = ROLLBACKS['esp']


@step('check', 'maintenance, read only')
def check_step(s):
    """Before leaving: root manifest block unchanged, vendor_boot_a/init_boot_a/boot_b unchanged, boot_a the old or the new
    image, the GPT the new tables and sgdisk clean, the ESP the image written, the store header in place."""
    lun0, rootfs = s.nodes['lun0'], s.nodes['rootfs']
    m = s.region_sha(rootfs, 0, 1)
    s.need(m == s.state.get('manifest_sha256') if s.execute else None, 'root manifest block unchanged')
    for key in ('vendor_boot_a', 'init_boot_a', 'boot_b'):
        got = s.region_sha(s.nodes[key], 0, s.sizes[key] // MiB, MiB, name=f'sha-{key}')
        s.need(got == s.state.get(f'{key}_sha256') if s.execute else None, f'{key} unchanged')
    got = s.region_sha(s.nodes['boot_a'], 0, s.sizes['boot_a'] // MiB, MiB, name='sha-boot_a')
    s.need(got in (s.state.get('boot_a_sha256'), s.state.get('boot_a_written')) if s.execute else None, f'boot_a as expected ({got})')
    boot_now = got if s.execute else s.tablet_boot_a()
    head, tail = s.region_sha(lun0, 0, 6), s.region_sha(lun0, TAIL_LBA, 5)
    s.need((head, tail) == (s.gpt['gpt-head.bin']['sha256'], s.gpt['gpt-tail.bin']['sha256']) if s.execute else None, 'GPT = the new tables')
    v = s.shell(f'sgdisk -v /dev/{lun0}')
    s.need('No problems found' in v if v is not None else None, 'sgdisk -v clean')
    esp_now = s.tablet_esp() if not s.execute else None
    if s.state.get('esp_sha256') or not s.execute:
        got = s.region_sha(lun0, common.ESP_FIRST * B // MiB, 1024, MiB, name='esp-check-lun0')
        s.need(got == s.state.get('esp_sha256') if s.execute else None, f'ESP = {s.state.get("esp_written", "the image written")}')
        esp_now = got if s.execute else esp_now
    record_now = s.esp_by_sha(esp_now) if esp_now else None
    s.pair(boot_now, record_now, 'the pair on the tablet')
    rec = record_now or (s.esp if not s.execute else None)
    if esp_store(rec):
        # the ESP's own root and store (the 27.3 line on linux_data): the manifest as map read it, the store bound and of its seed
        saved = s.state.get('root_manifest') or {}
        digest, image, _ = root_manifest(s, esp_store(rec)['partition'])
        if saved.get('partition') == esp_store(rec)['partition'] or not s.execute:
            s.need(digest == saved.get('sha256') if s.execute else None,
                   f'{esp_store(rec)["partition"]} block 0 (the root manifest of {rec["image"]}) unchanged')
        else:
            s.log(f'  info  {esp_store(rec)["partition"]} block 0: {"SHNGROOT, " + str(image) + " bytes" if image else "no manifest"} '
                  f'(map ran without --esp {rec["image"]}: nothing to compare with)')
        ok, text = bound_store(s, rec)
        s.need(ok, text)
        return
    h = s.t('peek', rootfs, rel(store_new(s)), 0, 8)
    if h:
        s.log(f'  info  store header at LBA {store_new(s)}: {bytes.fromhex(h["hex"])!r}'
              f'{"" if s.state.get("store_moved") else " (not moved: the first boot starts a fresh store here)"}')


@step('maint-exit', 'maintenance -> ABL fastboot')
def maint_exit(s):
    """piano-maint fastboot: the maintenance image reboots into ABL fastboot."""
    s.log('  tablet# sync; piano-maint fastboot')
    if s.execute:
        subprocess.run(['python3', str(MAINT_SHELL), 'sync; piano-maint fastboot', '20'], capture_output=True, text=True, timeout=60)
    if not s.wait(s.serial, True, 120):
        raise Stop('not back in ABL fastboot')


def ram_test(s, image, expect_uefi, reset_after):
    """fastboot boot IMAGE -> while its UEFI runs, its DEBUG log (oem dump to our own UEFI only) -> XNU runs until its reset ->
    the loop's dump image -> the XNU log (cvp mailbox) -> back in ABL fastboot. RAM boots only, nothing is written."""
    dump = Path(s.a.dump_image)
    s.need(image.is_file(), f'{image.name} exists')
    s.need(dump.is_file(), f'dump image {dump.name} exists (build it with the patched FD: a dump image without '
                           f'firmware/patches/unixnu/0001 would boot the ESP instead of its own payload)')
    name = image.stem
    if not s.wait(s.serial, True, 240):
        raise Stop('no ABL fastboot')
    t0 = time.time()
    s.fastboot('boot', str(image), timeout=180)
    if not s.wait('piano-uefi', True, 40):
        raise Stop('our UEFI did not appear: ABL refused the image? (fastboot reboot bootloader; docs/persistent-boot.md, v2 fallback)')
    uefi_log(s, name, expect_uefi)
    s.wait('piano-uefi', False, 90)
    if not s.wait(s.serial, True, reset_after + 150):
        raise Stop(f'not back in ABL fastboot {reset_after + 150} s later (parked? power + volume-down 15-20 s)')
    if s.execute:
        took = int(time.time() - t0)
        s.log(f'  {"ok   " if reset_after <= took <= reset_after + 150 else "WARN "} back in ABL fastboot after {took} s '
              f'(the configuration it ran resets after {reset_after} s)')
    s.fastboot('boot', str(dump), timeout=180)
    if not s.wait('piano-uefi', True, 40):
        raise Stop('dump image: our UEFI did not appear')
    s.fastboot('oem', 'dump', '9cb00000', '700000', serial='piano-uefi', check=False)
    s.fastboot('get_staged', f'{name}-xnu.bin', serial='piano-uefi', check=False)
    s.log(f'  host$ python3 tooling/native/xnu-log.py decode {name}-xnu.bin > {name}-xnu.txt')
    s.log(f'  check the Data store opens at LBA {store_new(s)} (the new partition end): "NativeRamBlock: Data store state ... first LBA"')
    if s.execute:
        r = subprocess.run(['python3', str(XNU_LOG), 'decode', str(s.dir / f'{name}-xnu.bin')], capture_output=True, text=True)
        (s.dir / f'{name}-xnu.txt').write_text(r.stdout)
        for line in r.stdout.splitlines():
            if re.search(r'NativeRamBlock: Data store|NativeUfs:.*(root|window|store)|panic\(|launchd\[1\]', line):
                s.log('        | ' + line[:200])
        # state 0 = data-persist=0 (no store: the precedence image); 4 = refused, first LBA 0 (a failure worth stopping on);
        # 7 = another seed's store, kept (the store guard, docs/data-store.md): opened at its place, not used
        lbas = re.findall(r'Data store state [1-47] .*?first LBA (\d+)', r.stdout)
        if lbas:
            s.need(all(int(x) == store_new(s) for x in lbas), f'the Data store opened at LBA {lbas[-1]} = {store_new(s)}')
        else:
            s.log('  WARN  no "Data store state" line in the XNU log (store off, or the log ring lost it)')
    s.wait(s.serial, True, 120)


def uefi_log(s, name, expect_uefi):
    """While our UEFI's fastboot is there: its DEBUG log (oem dump to our own UEFI only) into session/NAME-uefi.bin, checked for
    EXPECT_UEFI once UniXNU has logged its configuration volume."""
    # The DEBUG log is dumped repeatedly: right after the UEFI's fastboot appears, UniXNU has not read its configuration yet (seen
    # 2026-11-11: only the UNIXNURAMDISK lines). Until the expected line shows, or the UEFI is gone (DarwinBoot has taken over).
    lines = []
    for attempt in range(25 if s.execute else 1):
        try:   # short: the UEFI may leave (DarwinBoot takes over) while a command runs, and fastboot then hangs
            s.fastboot('oem', 'dump', 'D7F00000', '100000', serial='piano-uefi', check=False, timeout=15)
            s.fastboot('get_staged', f'{name}-uefi.bin', serial='piano-uefi', check=False, timeout=15)
        except subprocess.TimeoutExpired:
            s.log('        (the UEFI left during the dump)')
            break
        if not s.execute:
            break
        path = s.dir / f'{name}-uefi.bin'
        raw = path.read_bytes() if path.is_file() else b''
        got = [m.decode(errors='replace') for m in re.findall(rb'UNIXNU[A-Z]*: [ -~]{0,200}', raw)]
        if len(got) >= len(lines):
            lines = got
        if any('config: volume' in line for line in lines) or not s.wait('piano-uefi', True, 1):
            break
        time.sleep(0.5)
    if s.execute:
        for line in lines[:60]:
            s.log('        | ' + line)
        if any('config: volume' in line for line in lines):
            s.need(any(expect_uefi in line for line in lines), f'UEFI log has "{expect_uefi}"')
        elif lines:
            s.log('  WARN  the UEFI left before UniXNU logged its configuration volume: judged by the run below')
        else:
            s.log('  WARN  the UEFI DEBUG log could not be read (the UEFI was gone before the dump): judged by the run below')
    else:
        s.log(f'  check {name}-uefi.bin has "{expect_uefi}"')
    return lines


@step('ram-esp-test', 'ABL fastboot, RAM boot (no writes)')
def ram_esp_test(s):
    """The flashed boot path from RAM: boot-piano-esp-test-v2.img (kernel field, no payload) must load the ESP's configuration
    from UFS (UEFI log: config: volume VenHw(<LUN 0>)/HD(37,GPT,<esp guid>,...)) and boot it until its 240 s reset."""
    guid = s.gpt['esp_guid'] if s.gpt else '<esp guid>'
    ram_test(s, OUT / 'boot-piano-esp-test-v2.img', f'config: volume VenHw({UFS_LUN0_VENDOR})/HD(37,GPT,{guid}', s.esp_reset_after())


@step('ram-v4-signed', 'ABL fastboot, RAM boot (no writes)')
def ram_v4_signed(s):
    """The exact boot_a bytes from RAM (ABL takes vendor_boot_a and init_boot_a from the partitions). If ABL refuses a v4 image
    for fastboot boot: fastboot reboot bootloader; boot-a with --boot-image boot-piano-esp-test-v2-signed.img (docs)."""
    guid = s.gpt['esp_guid'] if s.gpt else '<esp guid>'
    ram_test(s, OUT / s.a.boot_image, f'config: volume VenHw({UFS_LUN0_VENDOR})/HD(37,GPT,{guid}', s.esp_reset_after())


@step('ram-precedence', 'ABL fastboot, RAM boot (no writes)')
def ram_precedence(s):
    """A dev RAM image built with the patched FD must still boot its own payload with the ESP present (UEFI log: config: volume
    VenHw(A9C6827C-...), UniXNURamDiskDxe's RAM disk). This is the RAM-boot rollback path."""
    image = Path(s.a.dev_image)
    reset = 45
    if image.is_file():
        m = re.search(rb'piano-reset-after=(\d+)', image.read_bytes())
        reset = int(m.group(1)) if m else 45
    ram_test(s, image, f'config: volume VenHw({RAM_PAYLOAD_VENDOR})', reset)


@step('ram-boot-a', 'ABL fastboot, RAM boot of --boot-image (no writes); the installed ESP boots and stays up')
def ram_boot_a(s):
    """The exact bytes of a new boot_a image (--boot-image) from RAM, before boot-a writes them: ABL takes the image, our UEFI
    loads the configuration of the ESP on the tablet from partition 37 (UEFI log: config: volume VenHw(<LUN 0>)/HD(37,GPT,...)),
    DarwinBoot boots its default entry with this FD. The installed daily ESP does not reset: the run is the installed system's
    normal boot (the owner's Data, data-persist=2), judged on the USB console (usb-console.py --capture), and left with the
    console's 'reset fastboot'. Host-side: the image is a signed one of boot.json/boot-v*.json, and its DarwinBoot takes the RAM
    window of the ESP on the tablet (the state's)."""
    image = OUT / s.a.boot_image
    entry = s.boot['signed'].get(s.a.boot_image)
    s.need(bool(entry) and image.is_file() and common.sha(image) == entry['sha256'],
           f'{image.name} is a signed image of {entry["record"] if entry else "no boot record"}' +
           (f' (sha256 {entry["sha256"][:16]}, DarwinBoot window up to {entry["fd_bound"] >> 20} MiB)' if entry else ''))
    esp = s.esp_by_sha(s.state.get('esp_sha256')) or s.esp_record(s.state.get('esp_written') or s.a.esp)
    window = s.esp_window(esp)
    s.need(bool(entry) and (window or 0) <= entry['fd_bound'],
           f'the ESP on the tablet ({esp["image"] if esp else "unknown"}, RAM window {window >> 20 if window else "-"} MiB) boots with this FD')
    guid = s.gpt['esp_guid'] if s.gpt else '<esp guid>'
    if not s.wait(s.serial, True, 240):
        raise Stop('no ABL fastboot (from the installed system: usb-console.py --send "reset fastboot" --capture 5)')
    s.fastboot('boot', str(image), timeout=180)
    if not s.wait('piano-uefi', True, 40):
        raise Stop('our UEFI did not appear: ABL refused the image? (fastboot reboot bootloader)')
    lines = uefi_log(s, image.stem, f'config: volume VenHw({UFS_LUN0_VENDOR})/HD(37,GPT,{guid}')
    if s.execute and not any('config: volume' in line for line in lines):
        s.log('  WARN  no configuration volume in the UEFI log: judge the run by the USB console alone')
    s.log(f'  check the USB console (usb-console.py --capture 400, started before this step): "NativeRamBlock: RAM window ... '
          f'({window >> 20 if window else 256} MiB)", "Data store state 2 ... first LBA {store_new(s)}", '
          '"piano-boot-guard: healthy after 180 s, restart reason normal"; then usb-console.py --send "reset fastboot" --capture 5')


def check_boot_image_pair(s, sha, what):
    """Before boot_a gets the contents SHA: the ESP on the tablet (read from it) must boot with their DarwinBoot (the pair rule).
    An FD whose DarwinBoot takes every window needs no read; Ubuntu's boot_a boots no ESP."""
    name, bound = s.boot_identity(sha)
    if sha == common.BOOT_A_SHA256 or (bound and bound >= common.DARWIN_WINDOW_MAX):
        s.log(f'  info  {what}: {name} (DarwinBoot window up to {bound >> 20 if bound else "-"} MiB): every ESP\'s window boots')
        return
    esp_sha = s.tablet_esp()
    record = s.esp_by_sha(esp_sha) if esp_sha else None
    if record is None:
        s.log(f'  info  {what}: the ESP region ({esp_sha and esp_sha[:16]}) is no ESP of a build record here: nothing to compare')
        return
    s.pair(sha, record, what)


@step('boot-a', 'maintenance, WRITE boot_a by dd (slot bits untouched)', writes='boot_a (LUN 4)')
def boot_a_step(s):
    """boot_a = the signed image (--boot-image: a signed image of boot.json or boot-v*.json; the signed v2 test image is the
    fallback), written by dd from the maintenance image so slot A's successful bit and retry count stay as they are. Read back
    whole. Refused when the ESP on the tablet asks for a RAM window this image's DarwinBoot does not take (the pair rule)."""
    image = OUT / s.a.boot_image
    signed = ((s.boot or {}).get('signed') or {}).get(s.a.boot_image, {}).get('sha256')
    if not signed:
        what = f'{s.a.boot_image} is not in boot.json/boot-v*.json "signed" (tooling/persist/sign.sh OUT SERIES: signs and verifies)'
        if s.execute:
            raise Stop(what)
        s.need(False, what)
    s.need(image.is_file() and common.sha(image) == signed if signed else None, f'{image.name} hashes to its record\'s {signed and signed[:16]}')
    r = map_nodes(s)
    if r:
        s.need(r['boot_a'].split()[1:] == [str(common.BOOT_A_START_512), str(common.BOOT_A_SIZE_512)], f'boot_a is {r["boot_a"]}')
    check_boot_image_pair(s, signed or f'planned:{s.a.boot_image}', f'{image.name} with the ESP on the tablet')
    boot_a = s.nodes['boot_a']
    now = s.region_sha(boot_a, 0, s.sizes['boot_a'] // MiB, MiB, name='sha-boot_a')
    s.require_backup('boot_a', now if s.execute else None)
    s.require_backup('boot_b')
    for key, digest in (('vendor_boot_a', common.VENDOR_BOOT_A_SHA256), ('init_boot_a', common.INIT_BOOT_A_SHA256)):
        got = s.region_sha(s.nodes[key], 0, s.sizes[key] // MiB, MiB, name=f'sha-{key}')
        s.need(got == digest if s.execute else None, f'{key} is slot A\'s current image {digest[:16]} (ABL pairs it with boot_a)')
    s.fetch('boot.img', image, signed or '<signed sha256>')
    if s.write(f'{image.name} -> /dev/{boot_a} ({common.BOOT_PARTITION_BYTES} bytes)'):
        s.t('write', 'boot.img', boot_a, 0, MiB, timeout=600)
    got = s.region_sha(boot_a, 0, s.sizes['boot_a'] // MiB, MiB, name='boot_a-check')
    s.need(got == signed if s.execute else None, f'boot_a read back {got} = {signed and signed[:16]}')
    h = s.t('peek', boot_a, 0, 0, 8)
    s.need(bytes.fromhex(h['hex']) == b'ANDROID!' if h else None, 'boot_a starts with ANDROID!')
    s.sim['boot_a'] = signed or f'planned:{s.a.boot_image}'
    if s.execute:
        s.save(boot_a_written=signed)


@rollback('boot-a')
def boot_a_back(s):
    """boot_a as backed up (the verified backup boot_a of this backup set: Ubuntu's 389d4271 when the install started, the
    image of before in a later set), read back. Refused while the ESP on the tablet asks for a RAM window the backup's DarwinBoot
    does not take: rollback esp-final first (the pair rule)."""
    map_nodes(s)
    entry = s.state.get('backups', {}).get('boot_a') or {}
    if entry.get('sha256'):
        check_boot_image_pair(s, entry['sha256'], f'the boot_a backup ({s.boot_identity(entry["sha256"])[0]}) with the ESP on the tablet')
    s.restore('boot_a', s.nodes['boot_a'], 0, MiB)
    s.sim['boot_a'] = entry.get('sha256')
    if s.execute:
        s.save(boot_a_written=None)


@step('esp-final', 'maintenance, WRITE partition 37 (daily-use ESP)', writes='LUN 0 LBA 71527424-71789567')
def esp_final(s):
    """Optional: the ESP image whose default entry is macos (--esp-final, default esp-base-macos.img), as step esp."""
    write_esp(s, s.a.esp_final)


@step('slot-success', 'maintenance, WRITE LUN 4 GPT (boot_a attribute bit 54 only)', writes='LUN 4 GPT entry of boot_a')
def slot_success(s):
    """Only when abl-check found slot-successful:a = no: set bit 54 of boot_a's GPT entry (piano-slot-success's single-bit
    write), after verified NAS backups of both GPT copies of boot_a's LUN. Read back: the new primary copy may differ from its
    backup only in that entry's attribute field and the two CRCs. Never set_active."""
    if s.execute and (s.state.get('abl') or {}).get('slot-successful:a') == 'yes':
        s.log('  info  slot A is already marked successful: nothing to do')
        return
    boot_a = s.nodes['boot_a']
    disk = re.sub(r'\d+$', '', boot_a)
    part = s.shell(f'cat /sys/class/block/{boot_a}/partition')
    number = int(part.strip().splitlines()[-1]) if part else 0
    s.shell(f'sgdisk -i {number or "<n>"} /dev/{disk}')
    s.backup('lun4-gpt-primary', disk, 0, 6, B, f'{disk} LBA 0-5 (boot_a\'s LUN, primary GPT)')
    blocks = s.shell(f'blockdev --getsize64 /dev/{disk}')
    total = int(blocks.strip().splitlines()[-1]) // B if blocks else 0
    s.backup('lun4-gpt-backup', disk, total - 5 if total else '<last-5>', 5, B, f'{disk} last 5 LBAs (backup GPT)')
    if s.write(f'sgdisk -A {number or "<n>"}:set:54 /dev/{disk} (boot_a successful)'):
        s.shell(f'sgdisk -A {number}:set:54 /dev/{disk}')
        s.save(slot_success_written=True)
    s.shell(f'sgdisk -A {number or "<n>"}:show /dev/{disk}; sgdisk -v /dev/{disk}')
    if s.execute:
        s.t('drop-caches')
        n, _ = s.receive(disk, 0, 6, B, [s.dir / 'lun4-gpt-primary-after.bin'])
        old, new = (s.dir / 'lun4-gpt-primary.img').read_bytes(), (s.dir / 'lun4-gpt-primary-after.bin').read_bytes()
        index = number - 1
        allowed = set(range(B + 16, B + 20)) | set(range(B + 88, B + 92)) | set(range(2 * B + index * 128 + 48, 2 * B + index * 128 + 56))
        diff = [i for i in range(len(old)) if old[i] != new[i]]
        s.need(n == 6 * B and diff and set(diff) <= allowed, f'LUN 4 primary GPT: {len(diff)} bytes changed, all in entry {number}\'s '
               f'attributes and the CRCs')
        attrs = int.from_bytes(new[2 * B + index * 128 + 48:2 * B + index * 128 + 56], 'little')
        s.need(attrs >> 54 & 1 == 1, f'boot_a attributes {attrs:#x}: bit 54 set')


@rollback('slot-success')
def slot_back(s):
    """LUN 4's GPT copies as backed up (lun4-gpt-backup, then lun4-gpt-primary), read back."""
    map_nodes(s)
    disk = re.sub(r'\d+$', '', s.nodes['boot_a'])
    e = s.require_backup('lun4-gpt-backup')
    s.restore('lun4-gpt-backup', disk, (e or {}).get('skip', '<last-5>'), B)
    s.restore('lun4-gpt-primary', disk, 0, B)
    s.shell(f'partx -u /dev/{disk}; sgdisk -v /dev/{disk}')


@step('abl-final', 'ABL fastboot, read only')
def abl_final(s):
    """Before the first boot from boot_a: slot A current, successful and bootable."""
    for var, want in (('current-slot', 'a'), ('slot-successful:a', 'yes'), ('slot-unbootable:a', 'no')):
        out = s.fastboot('getvar', var, check=False)
        m = re.search(rf'{re.escape(var)}:\s*(\S+)', out or '')
        s.need((m and m.group(1)) == want if s.execute else None, f'{var} = {want}')
    s.fastboot('getvar', 'slot-retry-count:a', check=False)


@step('reboot', 'ABL fastboot -> slot A (macOS)')
def reboot(s):
    """fastboot reboot: ABL boots slot A, now BootShim + FD; UniXNU loads the ESP's default entry after 3 s."""
    s.fastboot('reboot')


# ------------------------------------------------------------------------------------------------------------------------
# linux_rootfs once 27.0.1 is retired (docs/persistent-boot.md section 13): a verified backup of the whole partition, then a discard.
# The partition is taken from the map (node, first LBA, size), never from the GPT of today, so a later layout keeps working.

ROOTFS_KEY = 'linux_rootfs'


def rootfs_target(s):
    """linux_rootfs as mapped: (node, first LBA, blocks, bs, count). bs is 1 MiB when the partition is a whole number of MiB (else
    4096); the node must be a partition of LUN 0 that overlaps neither linux_data nor the ESP."""
    node, start, blocks = part_geometry(s, ROOTFS_KEY)
    lun0 = s.nodes.get('lun0')
    if s.execute:
        s.need(bool(node) and blocks > 0, f'linux_rootfs found: /dev/{node}, LBA {start}, {blocks} blocks')
        s.need(node != lun0 and re.fullmatch(re.escape(lun0 or '?') + r'\d+', node or '') is not None,
               f'/dev/{node} is a partition of LUN 0 /dev/{lun0}, not the disk')
        for other in ('linux_data', 'esp'):
            onode, ostart, oblocks = part_geometry(s, other)
            s.need(onode != node and (not oblocks or ostart + oblocks <= start or start + blocks <= ostart),
                   f'/dev/{node} is not {other} (/dev/{onode}, LBA {ostart}+{oblocks}) and does not overlap it')
    else:
        blocks = blocks or (common.ROOTFS_NEW_LAST - common.ROOTFS_FIRST + 1)
    total = blocks * B
    bs = MiB if total % MiB == 0 else B
    return node, start, blocks, bs, total // bs


def rootfs_pieces(s, count, bs):
    per = max(1, s.a.rootfs_piece_mib * MiB // bs)
    return [(i, skip, min(per, count - skip)) for i, skip in enumerate(range(0, count, per))]


@step('rootfs-backup', 'maintenance, read only on the tablet; writes the backup of the whole linux_rootfs (PIANO_BACKUP_DIR)')
def rootfs_backup(s):
    """A verified backup of the whole linux_rootfs (the 27.0.1 root image and its Data store; about 59 GiB), before rootfs-clear.
    The partition as mapped, in pieces of --rootfs-piece-mib (default 4096 MiB) into PIANO_BACKUP_DIR/<set>/linux_rootfs/, each
    gzip-compressed on the host (--rootfs-compress none: raw). The checks stay exact, on the raw bytes: (1) the tablet's own
    sha256 of the piece read from the medium (page cache dropped); (2) the sha256 of the bytes as they arrive, before they are
    compressed; (3) the stored file read back without the page cache (F_NOCACHE) and inflated: its sha256 is (1). The compressed
    file's own sha256 is recorded too (SHA256SUMS), so rootfs-clear and the rollback can recheck the copy without inflating it.
    At the end every piece is read back once more and the whole partition's sha256 is taken over the inflated pieces in order.
    A slow or flaky USB link: each piece is a detached tablet job polled with keepalives, its time limit scaled to its size (at
    least 1 MiB/s), a stream that stalls 300 s fails the piece, a failed piece is tried 3 times, and a step stopped anywhere
    resumes at the first piece not yet verified (the progress is in the state). Nothing is written on the tablet."""
    map_nodes(s)
    node, start, blocks, bs, count = rootfs_target(s)
    total, compress = count * bs, s.a.rootfs_compress
    pieces = rootfs_pieces(s, count, bs)
    nas, name = s.nas()
    s.log(f'  backup {ROOTFS_KEY}: /dev/{node} (LUN 0 LBA {start}-{start + blocks - 1}), {total} bytes in {len(pieces)} pieces of up to '
          f'{s.a.rootfs_piece_mib} MiB, {compress} -> {name}/{ROOTFS_KEY}/')
    entry = (s.state.get('backups') or {}).get(ROOTFS_KEY)
    if entry and entry.get('verified'):
        s.need((entry['first_lba'], entry['blocks']) == (start, blocks), f'the verified backup in {name} is of this partition '
               f'(LBA {entry["first_lba"]}+{entry["blocks"]})')
        now = s.region_sha(node, 0, count, bs, name='sha-rootfs-now')
        s.need(now == entry['sha256'], f'linux_rootfs now = its verified backup ({entry["sha256"][:16]}); --new-backup for another set')
        s.log('  ok    already backed up and verified')
        return
    want_plan = {'set': name, 'first_lba': start, 'blocks': blocks, 'bs': bs, 'piece_mib': s.a.rootfs_piece_mib, 'compress': compress}
    progress = s.state.get('rootfs_backup_progress') or {}
    if {k: progress.get(k) for k in want_plan} != want_plan:
        progress = dict(want_plan, pieces={})
    folder = nas / ROOTFS_KEY if nas else None
    if folder:
        folder.mkdir(exist_ok=True)
    suffix = '.img.gz' if compress == 'gzip' else '.img'
    for i, skip, n in pieces:
        file = f'{ROOTFS_KEY}/piece-{i:03d}{suffix}'
        want = s.region_sha(node, skip, n, bs, name=f'sha-rootfs-{i:03d}')
        if not s.execute:
            s.receive_piece(node, skip, n, bs, None, compress, f'send-rootfs-{i:03d}')
            continue
        done = progress['pieces'].get(str(i))
        if done and done['sha256'] == want and (nas / file).is_file() and (nas / file).stat().st_size == done['stored_bytes']:
            s.log(f'  ok    piece {i}: verified in an earlier run (sha256 {want[:16]})')
            continue
        for attempt in range(1, PIECE_TRIES + 1):
            try:
                got, raw, stored, kept = s.receive_piece(node, skip, n, bs, nas / file, compress, f'send-rootfs-{i:03d}')
                back, back_n, back_stored = s.read_back(nas / file, compress)
                if (got, raw) == (n * bs, want) and (back_n, back, back_stored) == (n * bs, want, stored):
                    break
                s.log(f'  WARN  piece {i}, try {attempt}: received {got} bytes {raw}, read back {back_n} bytes {back}; the tablet\'s {want}')
            except Stop as e:
                s.log(f'  WARN  piece {i}, try {attempt}: {e}')
        else:
            raise Stop(f'piece {i} of linux_rootfs failed {PIECE_TRIES} times: run rootfs-backup again (it resumes here)')
        s.log(f'  ok    piece {i}/{len(pieces) - 1}: {n * bs} bytes, sha256 {want[:16]} = received = read back without cache '
              f'({kept} bytes stored)')
        progress['pieces'][str(i)] = {'file': file, 'skip': skip, 'count': n, 'sha256': want, 'stored_sha256': stored, 'stored_bytes': kept}
        s.save(rootfs_backup_progress=progress)
    if not s.execute:
        s.log('  check every piece: received bytes and sha256 = the tablet\'s; the stored file read back without cache (inflated) = the '
              'same; then the whole partition\'s sha256 over all pieces read back again')
        return
    whole, problems = hashlib.sha256(), []
    for i, skip, n in pieces:
        q = progress['pieces'][str(i)]
        back, back_n, back_stored = s.read_back(nas / q['file'], compress, whole)
        if (back, back_n, back_stored) != (q['sha256'], n * bs, q['stored_sha256']):
            problems.append(f'piece {i}')
    s.need(not problems, f'every piece read back again without cache = its sha256 ({", ".join(problems) or "all"})')
    entry = {'file': f'{ROOTFS_KEY}/', 'sha256': whole.hexdigest(), 'bytes': total, 'dev': node, 'skip': 0, 'count': count, 'bs': bs,
             'partition': ROOTFS_KEY, 'first_lba': start, 'blocks': blocks, 'compression': compress,
             'what': f'partition linux_rootfs (whole, LUN 0 LBA {start}-{start + blocks - 1}), {len(pieces)} pieces',
             'verified': True, 'time': time.strftime('%Y-%m-%d %H:%M:%S'), 'local': False,
             'pieces': [progress['pieces'][str(i)] for i, _, _ in pieces]}
    s.log(f'  ok    linux_rootfs backed up: {total} bytes, sha256 {entry["sha256"]} ({sum(q["stored_bytes"] for q in entry["pieces"])} '
          f'bytes stored)')
    backups = dict(s.state.get('backups', {}), **{ROOTFS_KEY: entry})
    s.state.pop('rootfs_backup_progress', None)
    s.save(backups=backups)
    s.write_manifest(nas, backups)
    (nas / ROOTFS_KEY / 'RAW-SHA256SUMS').write_text(
        ''.join(f'{q["sha256"]}  {q["file"]} (inflated)\n' for q in entry['pieces']) + f'{entry["sha256"]}  linux_rootfs (whole)\n')


def rootfs_backup_entry(s):
    """The verified linux_rootfs backup rootfs-clear and its rollback use: this state's (step rootfs-backup), or the one in
    PIANO_BACKUP_DIR/<--rootfs-backup-set>/manifest.json (an earlier session's set). Every stored piece is there, with its recorded
    size, and hashes to its recorded sha256 read without the page cache. Returns (entry, the set's directory); a dry run (entry, None)."""
    if s.a.rootfs_backup_set:
        if not s.execute:
            s.log(f'  check the verified backup of linux_rootfs in PIANO_BACKUP_DIR/{s.a.rootfs_backup_set} (its manifest.json)')
            return None, None
        where = s.backup_root() / s.a.rootfs_backup_set
        manifest = where / 'manifest.json'
        if not manifest.is_file():
            raise Stop(f'--rootfs-backup-set {s.a.rootfs_backup_set}: no manifest.json in that set')
        entry = json.loads(manifest.read_text())['backups'].get(ROOTFS_KEY)
        source = f'set {s.a.rootfs_backup_set}'
    else:
        entry = (s.state.get('backups') or {}).get(ROOTFS_KEY)
        source = f'this session\'s set {s.state.get("backup_name")}'
        if not s.execute:
            s.log('  check a verified backup of linux_rootfs in this session\'s state (step rootfs-backup), or --rootfs-backup-set NAME')
            return entry, None
        if not entry:
            raise Stop('no verified backup of linux_rootfs in this session\'s state: run step rootfs-backup first, or name an '
                       'earlier set with --rootfs-backup-set NAME')
        where = s.backup_root() / s.state['backup_name']
    s.need(bool(entry) and entry.get('verified') and entry.get('pieces'), f'a verified backup of linux_rootfs in {source}')
    covered = 0
    for q in entry['pieces']:
        s.need(q['skip'] == covered, f'piece {q["file"]} follows the one before (block {covered})')
        covered += q['count']
    s.need(covered == entry['count'], f'the pieces cover the partition ({covered} of {entry["count"]} blocks of {entry["bs"]} bytes)')
    bad = [q['file'] for q in entry['pieces'] if not (where / q['file']).is_file() or (where / q['file']).stat().st_size != q['stored_bytes']
           or s.sha_nocache(where / q['file']) != q['stored_sha256']]
    s.need(not bad, f'every stored piece is there and hashes to its recorded sha256 (read without cache){": " + str(bad) if bad else ""}')
    return entry, where


def rootfs_zero_regions(s, start, blocks):
    """(first LBA, blocks) that must read zero after the clear: the first and last 64 MiB of the partition, and 64 MiB from each
    27.0.1 Data store header place inside it (the map's, and the places of today's layout: LBA 68119455, 68381599)."""
    span = min(blocks, 64 * MiB // B)
    lbas = {start, start + blocks - span}
    for first in [(s.state.get(k) or {}).get('first_lba') for k in ('store_new', 'store_old')] + [store_new(s), store_old(s)]:
        if first is not None and start <= first < start + blocks:
            lbas.add(first)
    return sorted((lba, min(span, start + blocks - lba)) for lba in lbas)


def zero_sha(nbytes, cache={}):
    if nbytes not in cache:
        h, left = hashlib.sha256(), nbytes
        while left:
            h.update(bytes(min(left, 16 * MiB)))
            left -= min(left, 16 * MiB)
        cache[nbytes] = h.hexdigest()
    return cache[nbytes]


def rootfs_zero(s, node, start, blocks):
    """Do the check regions read zero (from the medium)? (all zero, [the regions that do not])."""
    wrong = []
    for lba, n in rootfs_zero_regions(s, start, blocks):
        got = s.region_sha(node, lba - start, n, name=f'zero-check-{lba}')
        if s.execute and got != zero_sha(n * B):
            wrong.append(lba)
    return not wrong, wrong


def keep_hashes(s, record):
    """What rootfs-clear must leave as it is, read from the medium: both GPT copies, the ESP, block 0 of linux_data (the 27.3 root's
    manifest) and the header block of the ESP's store."""
    lun0 = s.nodes['lun0']
    out = {'GPT head': s.region_sha(lun0, 0, 6), 'GPT tail': s.region_sha(lun0, TAIL_LBA, 5), 'ESP': s.tablet_esp()}
    store = esp_store(record)
    if store:
        node, start, _ = part_geometry(s, store['partition'])
        out[f'{store["partition"]} block 0'] = s.region_sha(node, 0, 1)
        out[f'store header LBA {store["first_lba"]}'] = s.region_sha(node, store['first_lba'] - start, 1)
    return out


@step('rootfs-clear', 'maintenance, WRITE (discard) all of linux_rootfs', writes='LUN 0 partition linux_rootfs, all of it (no GPT change)')
def rootfs_clear(s):
    """Retire the 27.0.1 system: discard the whole linux_rootfs partition (its root image and its Data store). Refused unless
    (1) the target is linux_rootfs as mapped: a partition of LUN 0 (never the disk), not linux_data, not the ESP, overlapping
    neither; (2) the ESP on the tablet, read and hashed, is the image of a 27.3-only record here (build-k273-esp.py: its store
    elsewhere, bound) whose menu, read out of that image, has no entry that uses linux_rootfs; (3) a verified backup of exactly this
    partition (rootfs-backup, this session's or --rootfs-backup-set NAME's) is on the backup disk, every stored piece there and
    hashing to its record, and the partition read now on the tablet hashes to the backup. Then `blkdiscard` of the partition node
    (BusyBox's in the maintenance image; tablet.sh checks the node's name, start and size again first), or zeros by dd when the
    disk takes no discards (--rootfs-zero: always zeros). Read back from the medium: the first and last 64 MiB and 64 MiB from each
    old store header place must be zero (a device that does not read zeros after a discard is zeroed, then read again). The GPT,
    the ESP, linux_data's manifest block and the 27.3 store's header are hashed before and after: unchanged. Undo: rollback
    rootfs-clear (the backup, piece by piece)."""
    map_nodes(s)
    node, start, blocks, bs, count = rootfs_target(s)
    esp_sha = s.tablet_esp()
    record = s.esp_by_sha(esp_sha) if esp_sha else (s.esp if not s.execute else None)
    s.need(record is not None if s.execute else None, f'the ESP on the tablet ({esp_sha and esp_sha[:16]}) is the image of a build record '
           f'in {OUT.name}')
    if record:
        s.need(bool(esp_store(record)), f'{record["image"]} is a 27.3-only ESP (its store: '
               f'{(esp_store(record) or {}).get("partition", "linux_rootfs, the 27.0.1 store")})')
        users = entries_using_rootfs(s, record)
        s.need(not users, f'no entry of {record["image"]} uses linux_rootfs (its unixnu.toml read out of the image)' +
               (f': {users}' if users else ''))
    entry, _ = rootfs_backup_entry(s)
    if s.execute:
        s.need((entry['first_lba'], entry['blocks'], entry['bs'], entry['count']) == (start, blocks, bs, count),
               f'the backup is of linux_rootfs as mapped (LBA {entry["first_lba"]}+{entry["blocks"]})')
    now = s.region_sha(node, 0, count, bs, name='sha-rootfs-now')
    s.need(now == entry['sha256'] if s.execute else None, f'linux_rootfs read now = its backup ({entry["sha256"][:16] if entry else "-"})')
    before = keep_hashes(s, record)
    info = s.t('discard-info', node) or {}
    method = 'zero' if s.a.rootfs_zero or (s.execute and info.get('discard') != '1') else 'discard'
    s.log(f'  info  discard: {info or "(dry run)"} -> {method}')
    guard = (node, ROOTFS_KEY, start * B // 512, blocks * B // 512)
    if s.write(f'{method} /dev/{node}: all of linux_rootfs, LUN 0 LBA {start}-{start + blocks - 1} ({blocks * B} bytes)'):
        s.job(f'rootfs-{method}', method, *guard, timeout=limit(blocks * B, SLOW_MEDIUM))
    zero, wrong = rootfs_zero(s, node, start, blocks)
    if s.execute and not zero and method == 'discard':
        s.log(f'  WARN  after the discard, LBA {wrong} do not read zero (the device keeps old data on unmap): zeros instead')
        method = 'discard+zero'
        if s.write(f'zero /dev/{node}: all of linux_rootfs'):
            s.job('rootfs-zero', 'zero', *guard, timeout=limit(blocks * B, SLOW_MEDIUM))
        zero, wrong = rootfs_zero(s, node, start, blocks)
    regions = ', '.join(f'LBA {lba}+{n}' for lba, n in rootfs_zero_regions(s, start, blocks))
    s.need(zero if s.execute else None, f'linux_rootfs reads zero from the medium: {regions}' + (f' (not: {wrong})' if wrong else ''))
    after = keep_hashes(s, record)
    for key in before:
        s.need(after[key] == before[key] if s.execute else None, f'{key} unchanged ({(after[key] or "-")[:16]})')
    if s.execute:
        s.save(rootfs_cleared={'time': time.strftime('%Y-%m-%d %H:%M:%S'), 'method': method, 'first_lba': start, 'blocks': blocks,
                               'sha256_before': now, 'backup_set': s.a.rootfs_backup_set or s.state.get('backup_name')},
               manifest_sha256=s.region_sha(node, 0, 1))


@rollback('rootfs-clear')
def rootfs_clear_back(s):
    """linux_rootfs as rootfs-backup saved it (this session's set, or --rootfs-backup-set NAME), piece by piece from the backup disk:
    each served inflated, written by dd from the maintenance image, read back from the medium = its sha256; a piece that already
    reads as backed up is skipped (a stopped rollback resumes). Then the whole partition = the backup's sha256. The partition as
    mapped must be the backup's (first LBA and size); the GPT is not touched. Only then may an ESP with 27.0.1 entries go back."""
    map_nodes(s)
    node, start, blocks, bs, count = rootfs_target(s)
    entry, where = rootfs_backup_entry(s)
    if not s.execute:
        s.log(f'  check per piece: the tablet\'s sha256 of its blocks; if not the backup\'s: served inflated, restored by dd, read back')
        s.write(f'the pieces of the backup -> /dev/{node} from block 0 ({count} blocks of {bs} bytes)')
        s.region_sha(node, 0, count, bs, name='rootfs-check')
        return
    s.need((entry['first_lba'], entry['blocks'], entry['bs'], entry['count']) == (start, blocks, bs, count),
           f'linux_rootfs as mapped is the backup\'s partition (LBA {entry["first_lba"]}+{entry["blocks"]})')
    for q in entry['pieces']:
        now = s.region_sha(node, q['skip'], q['count'], bs, name=f'sha-rootfs-{q["skip"]}')
        if now == q['sha256']:
            s.log(f'  ok    {q["file"]}: already as backed up')
            continue
        served = ('gzip', where / q['file'], q['count'] * bs) if entry.get('compression') == 'gzip' else where / q['file']
        piece = Path(q['file']).name
        s.serve({piece: served})
        if s.write(f'{q["file"]} -> /dev/{node} at block {q["skip"]} of {bs} bytes'):
            s.job(f'restore-rootfs-{q["skip"]}', 'restore', piece, node, q['skip'], bs, s.a.http_port, timeout=limit(q['count'] * bs))
        got = s.region_sha(node, q['skip'], q['count'], bs, name=f'check-rootfs-{q["skip"]}')
        s.need(got == q['sha256'], f'{q["file"]} restored: read back {got[:16]} = {q["sha256"][:16]}')
    got = s.region_sha(node, 0, count, bs, name='rootfs-check')
    s.need(got == entry['sha256'], f'linux_rootfs restored: {got} = the backup\'s {entry["sha256"][:16]}')
    s.save(rootfs_cleared=None, manifest_sha256=s.region_sha(node, 0, 1))


# ------------------------------------------------------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('steps', nargs='*')
    p.add_argument('--list', action='store_true')
    p.add_argument('--dry-run', action='store_true', help='print every command; send nothing (tablet, ABL, NAS)')
    p.add_argument('--store', choices=('auto', 'move', 'fresh'), default='auto',
                   help='Data store: move it 1 GiB down (keeps Data; needs its 14 GB backup) or let it start fresh; auto: move '
                        'when its seed identity is the ESP\'s')
    p.add_argument('--esp', default='esp-base-diag.img', help='the ESP image of step esp (out/persistent)')
    p.add_argument('--root-image', help='step linux-data-root: the root image written onto linux_data')
    p.add_argument('--root-sha256', help='step linux-data-root: its sha256')
    p.add_argument('--esp-final', default='esp-base-macos.img', help='the ESP image of step esp-final')
    p.add_argument('--boot-image', default='boot-piano-macos-persistent-v4-signed.img', help='the boot_a image (out/persistent)')
    p.add_argument('--dump-image', default=str(D / 'out/ram-dump/boot-piano-macos-ram-A-bundled.img'))
    # ram-precedence's image: patched FD, data-persist=0 (another seed must not reset the installed system's store), reset 45 s
    p.add_argument('--dev-image', default=str(D / 'out/ram-precedence/boot-piano-macos-ram-A-bundled.img'))
    p.add_argument('--session', type=Path, default=OUT / 'session')
    p.add_argument('--new-backup', action='store_true', help='start a new NAS backup set (the old one stays on the NAS)')
    p.add_argument('--rootfs-piece-mib', type=int, default=4096, metavar='MIB',
                   help='rootfs-backup: the size of a piece (each its own stream, checked, retried, resumed)')
    p.add_argument('--rootfs-compress', choices=('gzip', 'none'), default='gzip',
                   help='rootfs-backup: gzip each piece on the host (the checks are on the raw bytes), or store it raw')
    p.add_argument('--rootfs-backup-set', metavar='NAME', help='rootfs-clear and its rollback: the verified linux_rootfs backup of '
                   'this earlier set in PIANO_BACKUP_DIR (default: this session\'s)')
    p.add_argument('--rootfs-zero', action='store_true', help='rootfs-clear: zeros by dd instead of a discard')
    p.add_argument('--http-port', type=int, default=8000)
    p.add_argument('--nc-port', type=int, default=9000)
    a = p.parse_args()
    if a.list or not a.steps:
        for name, st in STEPS.items():
            print(f'{name:15} {st["kind"]}\n{"":15} {st["doc"]}\n')
        print('rollback STEP:')
        for name, rb in ROLLBACKS.items():
            print(f'  {name:13} {rb["doc"]}')
        return
    s = Session(a)
    s.log(f'== {time.strftime("%Y-%m-%d %H:%M:%S")} {"RUN" if s.execute else "DRY RUN"}: {" ".join(a.steps)}')
    todo = [('rollback', n) for n in a.steps[1:]] if a.steps[0] == 'rollback' else [('step', n) for n in a.steps]
    try:
        for kind, name in todo:
            table = ROLLBACKS if kind == 'rollback' else STEPS
            if name not in table:
                raise Stop(f'unknown {kind} {name} (--list)')
            s.log(f'\n-- {kind} {name}: {table[name]["doc"]}')
            if kind == 'step' and table[name]['writes']:
                rb = ROLLBACKS.get(name)
                s.log(f'   writes {table[name]["writes"]}; rollback: maint-session.py rollback {name}'
                      f'{"  (" + rb["doc"] + ")" if rb else ""}')
            table[name]['run'](s)
    except Stop as e:
        s.log(f'\nSTOPPED: {e}')
        sys.exit(3)
    finally:
        if s.http:
            s.http.shutdown()
    if s.dry_failures:
        s.log(f'\ndry run: {len(s.dry_failures)} host-side check(s) would stop a real run:\n  ' + '\n  '.join(s.dry_failures))
    s.log('\ndone' + ('' if s.execute else ' (dry run: nothing was sent)'))


if __name__ == '__main__':
    main()
