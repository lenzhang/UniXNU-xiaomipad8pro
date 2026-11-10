#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run Adreno830's host tests (ASan/UBSan, package/PianoA830/adreno830/tests/run-host-tests.py) on PianoA830's sources, with piano's
firmware pins, twice: as upstream builds them and with -DADRENO830_MACOS (the macOS client lists the kext is built with).

    run-a830-host-tests.py [--out DIR] [--no-firmware] [--adreno-firmware DIR] [names ...]

The runner expects the UniXNU tree layout (ROOT/../UniXNU/kits/{ArmSmmu4K,AdrenoPM4,AdrenoFirmware}), so the sources are staged into
OUT/src/adreno830 next to OUT/src/UniXNU/kits with links to package/PianoA830/kits and, for test_a830votes only, to a checkout of the
AdrenoFirmware kit (BSD-2-Clause-Patent; default unixnu-research/AdrenoFirmware; read, never copied). Known differences on piano,
reported as KNOWN and not as failures:
  test_sha256 gen80000_{sqe,aqe,zap}: the test also requires the image to equal Adreno830's built-in pin table
      (NativeAdrenoFirmware.cpp), which is the 13T's; nothing in the driver uses that table (S0 checks the board's pins, which are
      piano's). The GMU image is identical to the 13T's and passes in full.
  test_a830regs: compares A830Regs.h with upstream's docs/inputs/regs Mesa export, which the published Adreno830 does not carry.
Exit 1 on any other failure.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PACKAGE = REPO / 'package/PianoA830'
KNOWN = {
    'test_sha256 gen80000_sqe.fw': "13T's built-in pin table (test only)",
    'test_sha256 gen80000_aqe.fw': "13T's built-in pin table (test only)",
    'test_sha256 gen80000_zap.mbn': "13T's built-in pin table (test only)",
    'test_a830regs': 'needs upstream docs/inputs/regs (not published)',
}


def stage(out, adreno_firmware):
    src = out / 'src'
    shutil.rmtree(src, ignore_errors=True)
    shutil.copytree(PACKAGE / 'adreno830', src / 'adreno830')
    kits = src / 'UniXNU/kits'
    kits.mkdir(parents=True)
    for name in ('ArmSmmu4K', 'AdrenoPM4'):
        (kits / name).symlink_to(PACKAGE / 'kits' / name)
    if adreno_firmware and (adreno_firmware / 'AdrenoHfiVotes.c').is_file():
        (kits / 'AdrenoFirmware').symlink_to(adreno_firmware)
    return src / 'adreno830'


def run(root, out, firmware, names, macos):
    command = [sys.executable, str(root / 'tests/run-host-tests.py'), '--out', str(out)]
    if firmware:
        command += ['--firmware', str(REPO / 'out/native-kexts/PianoA830/firmware'),
                    '--pins', str(PACKAGE / 'board/piano-gpu-firmware.json')]
    env = dict(os.environ)
    if macos:
        env['CCC_OVERRIDE_OPTIONS'] = '+-DADRENO830_MACOS'
    result = subprocess.run(command + list(names), capture_output=True, text=True, env=env)
    text = result.stdout + result.stderr
    (out.parent / f'{out.name}.log').write_text(text)
    passed = [l for l in result.stdout.splitlines() if re.match(r'^(test_\S+|kernel objects)( \([^)]*\))?: PASS', l)]
    failed = sorted({m.group(1) for m in re.finditer(r'^FAIL (test_[a-z0-9_]+(?: gen80000_\S+)?)', result.stdout, re.M)})
    return passed, failed, text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=REPO / 'out/native-kexts/PianoA830/host-tests')
    parser.add_argument('--no-firmware', action='store_true')
    parser.add_argument('--adreno-firmware', type=Path, default=Path(os.environ.get(
        'ADRENO_FIRMWARE_KIT', Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research/AdrenoFirmware')))
    parser.add_argument('names', nargs='*')
    args = parser.parse_args()
    root = stage(args.out, args.adreno_firmware)
    bad = 0
    for label, macos in (('upstream', False), ('ADRENO830_MACOS', True)):
        passed, failed, _ = run(root, args.out / ('macos' if macos else 'upstream'), not args.no_firmware, args.names, macos)
        known = [f for f in failed if f in KNOWN]
        real = [f for f in failed if f not in KNOWN]
        print(f'host tests ({label}): {len(passed)} passed, {len(known)} known ({"; ".join(f"{k}: {KNOWN[k]}" for k in known) or "none"}), '
              f'{len(real)} failed{": " + ", ".join(real) if real else ""}')
        bad += len(real)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
