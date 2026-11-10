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
  test_userclient (ADRENO830_MACOS pass only): asserts iPadOS's process names in the client lists; the macOS lists get their own
      structural check instead (macos_lists below: every entry matches as proc_selfname gives it, at 16 and 19 characters; the large
      list is part of the grant list; WindowServer is granted and is not a large client; iPadOS's names are absent).
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


KNOWN_MACOS = {'test_userclient': "asserts iPadOS's client lists by name (macOS lists: macos_lists)"}
MACOS_LISTS = r"""
#include "NativeAdrenoPublish.h"
#include <cstdio>
#include <cstring>
static int bad;
static void check(bool ok, const char *what) {if(!ok) {std::printf("FAIL %s\n", what); ++bad;}}
int main()
{
    unsigned entries = 0;
    for (const char *at = NativeAdrenoSandboxGrantList; *at; ++entries) {
        size_t length = 0;
        while (at[length] && at[length] != ',') ++length;
        char name[17] = {}, full[20] = {};
        std::memcpy(name, at, length < 16 ? length : 16);
        std::memcpy(full, at, length < 19 ? length : 19);
        check(length > 0 && NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList, name, 17), "entry at 16 characters");
        check(NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList, full, 20), "entry at 19 characters");
        at += length + (at[length] == ',' ? 1 : 0);
    }
    for (const char *at = NativeAdrenoLargeClientList; *at;) {
        size_t length = 0;
        while (at[length] && at[length] != ',') ++length;
        char full[20] = {};
        std::memcpy(full, at, length < 19 ? length : 19);
        check(NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList, full, 20), "large client also granted");
        at += length + (at[length] == ',' ? 1 : 0);
    }
    check(NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList, "WindowServer", 20), "WindowServer granted");
    check(!NativeAdrenoListNamesProcess(NativeAdrenoLargeClientList, "WindowServer", 20), "WindowServer not large");
    check(NativeAdrenoListNamesProcess(NativeAdrenoLargeClientList, "Dock", 20), "Dock large");
    check(NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList, "com.apple.WebKit", 17), "WebKit GPU process at 16");
    for (const char *ios : {"backboardd", "SpringBoard", "PosterBoard", "WidgetRenderer_Defa"})
        check(!NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList, ios, 20), "no iPadOS name");
    check(!NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList, "Docker", 20), "a longer name is not Dock");
    if (!bad) std::printf("PASS: macOS client lists: %u entries, grant/large structure, WindowServer the render server only\n", entries);
    return bad ? 1 : 0;
}
"""


def macos_lists(root, out):
    """The macOS client lists of NativeAdrenoPublish.h, built with -DADRENO830_MACOS over the runner's host doubles."""
    source, binary = out / 'macos_lists.cpp', out / 'macos_lists'
    source.write_text(MACOS_LISTS)
    command = ['xcrun', 'clang++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-DADRENO830_MACOS',
               '-DADRENO830_HOST_TEST', '-I', str(out / 'macos/host-include'), '-I', str(root / 'tests'), '-I', str(root),
               str(source), '-o', str(binary)]
    built = subprocess.run(command, capture_output=True, text=True)
    if built.returncode:
        return False, built.stderr[-2000:]
    ran = subprocess.run([str(binary)], capture_output=True, text=True)
    return ran.returncode == 0, ran.stdout.strip()


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
        table = {**KNOWN, **(KNOWN_MACOS if macos else {})}
        known = [f for f in failed if f in table]
        real = [f for f in failed if f not in table]
        print(f'host tests ({label}): {len(passed)} passed, {len(known)} known ({"; ".join(f"{k}: {table[k]}" for k in known) or "none"}), '
              f'{len(real)} failed{": " + ", ".join(real) if real else ""}')
        bad += len(real)
        if macos and not args.names:
            ok, text = macos_lists(root, args.out)
            print(f'macos_lists: {text}')
            bad += 0 if ok else 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
