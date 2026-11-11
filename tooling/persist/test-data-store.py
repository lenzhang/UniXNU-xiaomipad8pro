#!/usr/bin/env python3
"""Host-only tests of the Data store tooling (docs/data-store.md); no tablet, no device:

1. common.store_header against NativeRamBlock's own encoder (package/NativeRamBlock/Test/header-dump.cpp, built here): version 1
   headers, the self-contained extension, a stale or damaged extension, damaged and blank blocks; common.store_usable.
2. common.installed_seed: PIANO_INSTALLED_SEED (hex prefix, ESP record), out/persistent/installed.json, unknown.
3. build-ram-images.py data_store_args: which data-persist a RAM image gets for the installed seed, another seed, an unknown one,
   with and without the store guard in the kernel collection; PIANO_DATA_PERSIST, PIANO_DATA_STORE_REPLACE / _FLATTEN; refusals.
4. build-esp.py: check_store's refusals and records, store_args, and --arg data-store-* refused.
  test-data-store.py
"""
import contextlib
import hashlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import common  # noqa: E402

failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)


def refused(fn, *args):
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            fn(*args)
    except SystemExit as e:
        return str(e)
    return None


def packed_seed(path, logical, pages):
    """A DBRDISK1 seed with one extent per {page index: fill byte}."""
    extents = sorted(pages.items())
    head = b'DBRDISK1' + struct.pack('<IIQQ', 1, 4096, logical, len(extents))
    table = b''.join(struct.pack('<QQ', i * 4096, 4096) for i, _ in extents)
    path.write_bytes(head + table + b''.join(bytes([v]) * 4096 for _, v in extents))
    return common.seed_info(path)


def load(name, path):
    spec = common.importlib.util.spec_from_file_location(name, path)
    module = common.importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    tmp = Path(tempfile.mkdtemp(prefix='piano-data-store-test-'))
    ram = common.ram_images()                       # (their tool paths come from HOME: loaded before it moves)
    esp = load('build_esp', HERE / 'build-esp.py')
    os.environ['HOME'] = str(tmp / 'home')          # no ~/.config/piano/env of this Mac
    (tmp / 'home').mkdir()
    for k in [k for k in os.environ if k.startswith('PIANO_')]:
        del os.environ[k]
    os.environ['PIANO_PERSIST_OUT'] = str(tmp / 'persistent')
    (tmp / 'persistent').mkdir()

    print('== store headers: the driver\'s encoder, the tooling\'s reader')
    dump = tmp / 'header-dump'
    subprocess.run(['clang++', '-std=c++17', '-O1', '-o', str(dump), str(common.DEVICE / 'package/NativeRamBlock/Test/header-dump.cpp')],
                   check=True)
    seed = hashlib.sha256(b'seed X').hexdigest()

    def header(pages, gen, flags, flattened):
        return subprocess.run([str(dump), str(pages), seed, str(gen), str(flags), str(flattened)], capture_output=True, check=True).stdout

    plain, flat = header(3145728, 10, 0, 0), header(3145728, 10, 1, 31234)
    check(len(plain) == 4096 and plain[64:] == bytes(4032), 'flags 0: a plain version 1 header, bytes 64.. zero')
    v1 = bytearray(4096)                             # as tooling/persist/test-persist.py and every earlier kernel build it
    v1[:8] = b'SHNGDATA'
    struct.pack_into('<IIQ', v1, 8, 1, 64, 3145728)
    v1[24:56] = bytes.fromhex(seed)
    struct.pack_into('<I', v1, 56, 10)
    struct.pack_into('<I', v1, 60, zlib.crc32(bytes(v1[:60])) & 0xffffffff)
    check(plain == bytes(v1), 'flags 0 = the earlier header byte for byte')
    h = common.store_header(plain)
    check(h['state'] == 'valid' and h['pages'] == 3145728 and h['identity'] == seed and h['generation'] == 10 and not h['self_contained'],
          f'version 1 header read ({h})')
    h = common.store_header(flat)
    check(h['self_contained'] and h['flags'] == 1 and h['flattened'] == 31234 and h['identity'] == seed, f'extension read ({h})')
    check(flat[:64] == plain[:64], 'the extension leaves the first 64 bytes as they were')
    check(common.store_header(flat[:128])['self_contained'], 'the first 128 bytes are enough (maint-session peeks 128)')
    check(not common.store_header(flat[:64])['self_contained'], 'a 64-byte peek sees a plain header')
    bad = bytearray(flat)
    bad[90] ^= 1
    check(common.store_header(bad)['state'] == 'valid' and not common.store_header(bad)['self_contained'], 'a damaged extension is ignored')
    stale = bytearray(flat)
    struct.pack_into('<I', stale, 56, 11)
    struct.pack_into('<I', stale, 60, zlib.crc32(bytes(stale[:60])) & 0xffffffff)
    check(not common.store_header(stale)['self_contained'], 'an extension left over after the version 1 part changed is void')
    bad = bytearray(plain)
    bad[30] ^= 1
    check(common.store_header(bad)['state'] == 'damaged', 'a damaged version 1 header')
    check(common.store_header(b'\xa5' * 128)['state'] == 'blank', 'a blank window')
    mine = {'pages': 3145728, 'identity_sha256': seed}
    other = {'pages': 3145728, 'identity_sha256': hashlib.sha256(b'seed Y').hexdigest()}
    check(common.store_usable(common.store_header(plain), mine) and not common.store_usable(common.store_header(plain), other),
          'store_usable: a plain store only with its seed')
    check(common.store_usable(common.store_header(flat), other) and
          not common.store_usable(common.store_header(header(1000, 1, 1, 0)), other), 'store_usable: a self-contained store with any seed of its size')

    print('== the installed seed')
    check(common.installed_seed()[0] is None, 'unknown without PIANO_INSTALLED_SEED or installed.json')
    record = tmp / 'persistent/esp-x.json'
    common.write_json(record, {'seed': {'identity_sha256': seed}})
    common.write_json(tmp / 'persistent/installed.json', {'image': 'esp-x.img', 'seed': {'identity_sha256': seed}})
    check(common.installed_seed() == (seed, 'persistent/installed.json'), 'out/persistent/installed.json')
    (tmp / 'home/.config/piano').mkdir(parents=True)
    (tmp / 'home/.config/piano/env').write_text(f'PIANO_INSTALLED_SEED={other["identity_sha256"][:8].upper()}\n')
    check(common.installed_seed()[0] == other['identity_sha256'][:8], '~/.config/piano/env PIANO_INSTALLED_SEED wins (a prefix)')
    os.environ['PIANO_INSTALLED_SEED'] = str(record)
    check(common.installed_seed()[0] == seed, 'PIANO_INSTALLED_SEED=<ESP record>.json, the environment over the file')
    os.environ['PIANO_INSTALLED_SEED'] = 'xyz'
    check(refused(common.installed_seed) is not None, 'a malformed PIANO_INSTALLED_SEED is refused')
    del os.environ['PIANO_INSTALLED_SEED']
    (tmp / 'home/.config/piano/env').unlink()
    (tmp / 'persistent/installed.json').unlink()
    check(common.same_seed(seed, seed[:8]) and not common.same_seed(seed, other['identity_sha256'][:8]) and not common.same_seed(seed, None),
          'same_seed with a prefix')

    print('== build-ram-images.py data_store_args')
    seed_x = packed_seed(tmp / 'x.img', 12 << 30, {0: 1, 5: 2, 9000: 3})              # 12 GiB logical, as the installed store
    seed_y = packed_seed(tmp / 'y.img', 12 << 30, {0: 1, 5: 2, 9000: 4})
    seed_z = packed_seed(tmp / 'z.img', 8 << 30, {0: 1, 5: 2, 9000: 4})               # another size: another window place
    check(seed_x['identity_sha256'] != seed_y['identity_sha256'], 'two seeds, two identities')
    check(seed_x['pages'] == common.INSTALLED_STORE_PAGES and seed_z['pages'] != common.INSTALLED_STORE_PAGES, 'seed sizes')
    guarded, old = tmp / 'kc-guard.bin', tmp / 'kc-old.bin'
    guarded.write_bytes(b'\xcf\xfa\xed\xfe' + b'x' * 100 + b'data-store-replace\0' + b'y' * 100)
    old.write_bytes(b'\xcf\xfa\xed\xfe' + b'x' * 300)
    check(common.kc_has_store_guard(guarded) and not common.kc_has_store_guard(old), 'kc_has_store_guard')

    def args(seed_img, kc, **env):
        for k in ('PIANO_DATA_PERSIST', 'PIANO_DATA_STORE_REPLACE', 'PIANO_DATA_STORE_FLATTEN', 'PIANO_INSTALLED_SEED'):
            os.environ.pop(k, None)
        os.environ.update(env)
        err = io.StringIO()
        try:
            with contextlib.redirect_stderr(err):
                got, rec = ram.data_store_args(seed_img, kc)
            return got, rec, err.getvalue()
        except SystemExit as e:
            return None, None, str(e)

    x, y = tmp / 'x.img', tmp / 'y.img'
    inst = {'PIANO_INSTALLED_SEED': seed_x['identity_sha256'][:16]}
    got, rec, note = args(x, guarded, **inst)
    check(got == ['data-persist=2'] and rec['same_seed'], f'installed seed: data-persist=2 ({got})')
    got, rec, note = args(y, guarded, **inst)
    check(got == ['data-persist=0'] and 'not the installed seed' in note, f'another seed: data-persist=0 and a loud note ({got})')
    got, rec, note = args(y, old, **inst)
    check(got == ['data-persist=0'], 'another seed, no guard: data-persist=0')
    got, rec, note = args(y, guarded, PIANO_DATA_PERSIST='2', **inst)
    check(got == ['data-persist=2'] and 'kept' in note, 'another seed, PIANO_DATA_PERSIST=2 (the guard test): 2, with the note')
    got, rec, note = args(y, old, PIANO_DATA_PERSIST='2', **inst)
    check(got is None and 'WITHOUT the store guard' in note, 'another seed, PIANO_DATA_PERSIST=2, no guard: refused')
    got, rec, note = args(y, guarded)
    check(got == ['data-persist=2'] and 'unknown' in note, 'unknown installed seed, guard: 2 with a note')
    got, rec, note = args(y, old)
    check(got == ['data-persist=0'], 'unknown installed seed, no guard: 0')
    got, rec, note = args(y, old, PIANO_DATA_PERSIST='2')
    check(got is None, 'unknown installed seed, no guard, PIANO_DATA_PERSIST=2: refused')
    got, rec, note = args(x, old, PIANO_DATA_PERSIST='2', **inst)
    check(got == ['data-persist=2'], 'the installed seed with an old collection: 2 (it resumes)')
    got, rec, note = args(x, guarded, PIANO_DATA_STORE_FLATTEN='1', **inst)
    check(got == ['data-persist=2', 'data-store-flatten=1'], f'flatten ({got})')
    got, rec, note = args(y, guarded, PIANO_DATA_PERSIST='2', PIANO_DATA_STORE_REPLACE=seed_x['identity_sha256'][:8], **inst)
    check(got == ['data-persist=2', 'data-store-replace=0x' + seed_x['identity_sha256'][:8]] and 'STARTED OVER' in note,
          f'replace one store ({got})')
    got, rec, note = args(y, guarded, PIANO_DATA_STORE_REPLACE='1', **inst)
    check(got is None, 'replace with data-persist=0 (the default for another seed): refused')
    got, rec, note = args(x, old, PIANO_DATA_STORE_FLATTEN='1', **inst)
    check(got is None, 'flatten with an old collection: refused')
    z = tmp / 'z.img'
    got, rec, note = args(z, guarded, PIANO_DATA_PERSIST='2', **inst)
    check(got is None and 'window would start elsewhere' in note, 'a seed of another size with data-persist=2: refused')
    got, rec, note = args(z, guarded, **inst)
    check(got == ['data-persist=0'], 'a seed of another size by default: data-persist=0')
    got, rec, note = args(z, guarded, PIANO_DATA_PERSIST='2', PIANO_DATA_STORE_RELAYOUT='1', **inst)
    check(got == ['data-persist=2'], '... unless PIANO_DATA_STORE_RELAYOUT=1')
    os.environ.pop('PIANO_DATA_STORE_RELAYOUT', None)
    got, rec, note = args(x, guarded, PIANO_DATA_PERSIST='3')
    check(got is None, 'PIANO_DATA_PERSIST=3: refused')
    got, rec, note = args(x, guarded, PIANO_DATA_PERSIST='2', PIANO_DATA_STORE_REPLACE='12')
    check(got is None, 'a malformed PIANO_DATA_STORE_REPLACE: refused')
    os.environ.pop('PIANO_DATA_PERSIST', None)

    print('== build-esp.py')

    def a(**kw):
        return SimpleNamespace(out=tmp / 'persistent', replace_data=kw.get('replace'), flatten_data=kw.get('flatten', False),
                               installed_trust=kw.get('trust'))

    def trust(path, *modules):                      # stage-soft-metal.py trust_bundle's layout
        blobs = [struct.pack('<I16sI', 2, bytes(16), len(m)) + b''.join(h + bytes(4) for h in m) for m in modules]
        offsets, cursor = [], 4 + 4 * len(blobs)
        for b in blobs:
            offsets.append(cursor)
            cursor += len(b)
        path.write_bytes(struct.pack(f'<{len(blobs) + 1}I', len(blobs), *offsets) + b''.join(blobs))
        return path

    h = [hashlib.sha256(bytes([k])).digest()[:20] for k in range(5)]
    t_old = trust(tmp / 'old.trust', [h[0], h[1]], [h[2]])                 # the installed one: plugin module + the OS
    t_new = trust(tmp / 'new.trust', [h[3]], [h[2]])                       # a new plugin staged onto the input trust cache
    t_union = trust(tmp / 'union.trust', [h[3]], [h[0], h[1]], [h[2]])     # staged with --trust <the installed trust cache>
    check(common.trust_hashes(t_old) == {h[0], h[1], h[2]}, 'trust_hashes')
    with contextlib.redirect_stderr(io.StringIO()) as err:
        rec = esp.check_trust(a(trust=t_old), t_new)
    check(rec['missing_cdhashes'] == 2 and 'NOT in this ESP' in err.getvalue(), 'check_trust: the installed plugin\'s CDHashes missing: a note')
    with contextlib.redirect_stderr(io.StringIO()) as err:
        rec = esp.check_trust(a(trust=t_old), t_union)
    check(rec['missing_cdhashes'] == 0 and not err.getvalue(), 'check_trust: a union trust cache: no note')
    common.write_json(tmp / 'persistent/installed.json', {'seed': {'identity_sha256': seed_x['identity_sha256']},
                                                          'inputs': {'trust': {'path': str(t_old)}}})
    check(common.installed_trust(tmp / 'persistent') == t_old, 'installed_trust from installed.json')

    for k in ('PIANO_DATA_STORE_REPLACE', 'PIANO_DATA_STORE_FLATTEN', 'PIANO_INSTALLED_SEED'):
        os.environ.pop(k, None)
    common.write_json(tmp / 'persistent/installed.json', {'seed': {'identity_sha256': seed_x['identity_sha256']}})
    check(refused(esp.check_store, a(), seed_y, old) is not None, 'another seed, no guard: refused')
    check(refused(esp.check_store, a(replace='1'), seed_y, old) is None, '... unless --replace-data')
    with contextlib.redirect_stderr(io.StringIO()) as err:
        rec = esp.check_store(a(), seed_y, guarded)
    check(not rec['same_seed'] and rec['kc_has_store_guard'] and 'VOLATILE' in err.getvalue(), 'another seed, guard: a loud note')
    with contextlib.redirect_stderr(io.StringIO()) as err:
        rec = esp.check_store(a(), seed_x, old)
    check(rec['same_seed'] and not err.getvalue(), 'the installed seed: no note, even without the guard')
    check(refused(esp.check_store, a(flatten=True), seed_x, old) is not None, '--flatten-data with an old collection: refused')
    check(refused(esp.check_store, a(replace='1'), seed_z, guarded) is not None, 'an ESP seed of another size: refused')
    check(esp.store_args(a(replace='1', flatten=True)) == ['data-store-replace=1', 'data-store-flatten=1'], 'store_args')
    check(esp.store_args(a(replace='0xACED5BF9')) == ['data-store-replace=0xaced5bf9'], 'store_args: one store')
    check(refused(esp.store_args, a(replace='aced')) is not None, 'store_args: a malformed seed refused')
    profile = esp.PROFILES['base']
    ns = SimpleNamespace(arg=['data-store-replace=1'], rtc_offset=None, fast_boot=0, diag_reset_after=240, replace_data=None, flatten_data=False)
    check(refused(esp.args_of, ns, profile, 1) is not None, '--arg data-store-replace refused (the option says it)')
    ns.arg, ns.replace_data, ns.flatten_data = [], '1', True
    lines = esp.args_of(ns, profile, 1)
    check('data-store-replace=1 data-store-flatten=1' in lines['macos'] and 'data-store-replace' in lines['macos-diag'] and
          'data-store' not in lines['fastboot'] and 'data-persist=0' in lines['fastboot'], 'the macos entries carry them, fastboot not')
    # k27 0063's boot guard (docs/boot-guard.md): the daily profile's default on the macos entries, never on fastboot
    ns = SimpleNamespace(arg=[], rtc_offset=None, fast_boot=0, diag_reset_after=240, replace_data=None, flatten_data=False,
                         boot_guard=None)
    daily = esp.PROFILES['daily']
    lines = esp.args_of(ns, daily, 1)
    check('piano-boot-guard=180' in lines['macos'].split() and 'piano-boot-guard=180' in lines['macos-diag'].split() and
          'piano-boot-guard' not in lines['fastboot'], 'daily: piano-boot-guard=180 on macos and macos-diag, not on fastboot')
    check(not any(x.startswith('piano-boot-guard') for x in esp.args_of(ns, profile, 1)['macos'].split()),
          'base: no boot guard unless asked')
    ns.boot_guard = 0
    check('piano-boot-guard' not in esp.args_of(ns, daily, 1)['macos'], '--boot-guard 0: none on daily')
    ns.boot_guard = 240
    check('piano-boot-guard=240' in esp.args_of(ns, profile, 1)['macos'].split(), '--boot-guard 240 on base')
    ns.boot_guard = 30
    check(refused(esp.args_of, ns, daily, 1) is not None, '--boot-guard 30: refused (under launchd\'s time)')
    ns.boot_guard, ns.arg = None, ['piano-boot-guard=200']
    check(refused(esp.args_of, ns, daily, 1) is not None, '--arg piano-boot-guard=200 refused (the option says it)')
    ns.arg = ['piano-boot-guard-exec=loginwindow']
    lines = esp.args_of(ns, daily, 1)
    check('piano-boot-guard-exec=loginwindow' in lines['macos'].split() and 'piano-boot-guard=180' in lines['macos'].split() and
          'piano-boot-guard' not in lines['fastboot'], '--arg piano-boot-guard-exec with the default guard; neither on fastboot')

    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
