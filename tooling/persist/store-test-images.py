#!/usr/bin/env python3
"""The RAM images of the Data store tests (docs/data-store.md section 7), from the kernel collection that
tooling/k27/build-native-image.sh made (out/k27/kc-k27-native.bin: it must have the store guard) and its ADT; the loop's dump image
(out/ram-dump) is build-native-image.sh's too. Every image is "fastboot boot" only (tooling/native/native-loop.sh IMAGE).

  out/ram-store-probe    data-persist=1: reads the store's header and bitmap and writes nothing (NativeUfs refuses every store write
                         in mode 1); prints the store's seed, flags, generation and page count. Reset after 90 s.
  out/ram-store-guard    data-persist=2 with a seed that is not the installed one: the store guard must keep the store
                         ("kept; Data is volatile this boot", state 7). Reset after 240 s.
  out/ram-store-flatten  only with --flatten: data-persist=2 data-store-flatten=1 with the INSTALLED seed and trust cache (from
                         --installed-esp): the store becomes self-contained, then the boot goes on with the owner's Data. Reset after 180 s.

  store-test-images.py [--installed-esp out/persistent/esp-gpu-macos.img] [--guard-seed SEED --guard-trust TRUST] [--flatten]

--installed-esp: the ESP image the tablet boots (as written by maint-session.py): its seed, trust cache and ADT are copied out to
out/installed/ (mtools, read only) and its seed identity is the installed seed for the checks; without it PIANO_INSTALLED_SEED (or
out/persistent/installed.json) is used. The guard image's seed (default out/inputs/scratch-data.img) must differ from the installed
seed AND from the store seed the probe prints; this tool checks the first, the procedure the second.
PIANO_EXTRA_ARGS is passed on (e.g. the installed profile's GPU arguments for the flatten boot).
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

D = common.DEVICE


def extract(esp, out):
    """Payloads/{scratch-data.img,kernel.macho.trustcache,kernel.macho.adt} of an ESP image into OUT (mtools, the image only read)."""
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    for name in ('scratch-data.img', 'kernel.macho.trustcache', 'kernel.macho.adt'):
        (out / name).unlink(missing_ok=True)
        subprocess.run(['mcopy', '-n', '-i', str(esp), f'::/EFI/UniXNU/Payloads/{name}', str(out / name)], check=True, env=env)
    return out / 'scratch-data.img', out / 'kernel.macho.trustcache', out / 'kernel.macho.adt'


def build(name, seed, trust, adt, kc, env):
    out = D / 'out' / name
    command = ['python3', str(D / 'tooling/bootimg/build-ram-images.py'), '--fd', str(D / 'out/fd/piano-uefi-A-bundled.fd'),
               '--fd-name', 'A-bundled', '--kc', str(kc), '--trust', str(trust), '--seed', str(seed), '--adt', str(adt),
               '--utc', str(int(time.time())), '--out', str(out)]
    print(f'== {name}: {" ".join(f"{k}={v}" for k, v in env.items() if k.startswith("PIANO_"))}', flush=True)
    subprocess.run(command, check=True, env=dict(os.environ, **env))
    return out / 'boot-piano-macos-ram-A-bundled.img'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--kc', type=Path, default=D / 'out/k27/kc-k27-native.bin')
    p.add_argument('--adt', type=Path, default=D / 'out/adt/piano-k27.adt')
    p.add_argument('--installed-esp', type=Path, help='the ESP image the tablet boots (its seed is the installed seed)')
    p.add_argument('--guard-seed', type=Path, default=D / 'out/inputs/scratch-data.img')
    p.add_argument('--guard-trust', type=Path, default=D / 'out/inputs/kernel.macho.trustcache')
    p.add_argument('--flatten', action='store_true', help='also out/ram-store-flatten (needs --installed-esp)')
    a = p.parse_args()
    if not common.kc_has_store_guard(a.kc):
        raise SystemExit(f'{a.kc}: no store guard (build it from this tree: tooling/k27/build-native-image.sh)')
    if not (D / 'out/ram-dump/boot-piano-macos-ram-A-bundled.img').is_file():
        raise SystemExit('out/ram-dump: no dump image (tooling/k27/build-native-image.sh makes it)')
    env = {}
    flatten = None
    if a.installed_esp:
        seed, trust, adt = extract(a.installed_esp, D / 'out/installed')
        installed = common.seed_info(seed)['identity_sha256']
        env['PIANO_INSTALLED_SEED'] = installed
        flatten = (seed, trust, adt)
        print(f'{a.installed_esp.name}: installed seed {installed[:16]} (out/installed/)')
    else:
        installed, source = common.installed_seed()
        print(f'installed seed {installed[:16] if installed else "unknown"} ({source})')
    if a.flatten and not flatten:
        raise SystemExit('--flatten needs --installed-esp (the flatten boot must bring the store\'s own seed)')
    guard = common.seed_info(a.guard_seed)['identity_sha256']
    if not installed:
        raise SystemExit('the installed seed is unknown: --installed-esp, or PIANO_INSTALLED_SEED in ~/.config/piano/env')
    if common.same_seed(guard, installed):
        raise SystemExit(f'--guard-seed {a.guard_seed} is the installed seed {guard[:8]}: the guard test needs another one '
                         '(out/inputs/scratch-data.img or out/turnip-a830/stage/scratch-data.img)')
    images = {
        'probe': build('ram-store-probe', a.guard_seed, a.guard_trust, a.adt, a.kc, dict(env, PIANO_DATA_PERSIST='1', PIANO_RESET_AFTER='90')),
        'guard': build('ram-store-guard', a.guard_seed, a.guard_trust, a.adt, a.kc, dict(env, PIANO_DATA_PERSIST='2', PIANO_RESET_AFTER='240')),
    }
    if a.flatten:
        seed, trust, adt = flatten
        images['flatten'] = build('ram-store-flatten', seed, trust, adt, a.kc,
                                  dict(env, PIANO_DATA_PERSIST='2', PIANO_DATA_STORE_FLATTEN='1', PIANO_RESET_AFTER='180'))
    print(f'\nseeds: guard/probe {guard[:8]}, installed {installed[:8]}')
    for name, image in images.items():
        print(f'  {name:8} tooling/native/native-loop.sh {image.relative_to(D)}')
    print('expected lines: docs/data-store.md section 7')


if __name__ == '__main__':
    main()
