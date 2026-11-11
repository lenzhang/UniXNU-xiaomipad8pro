#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
"""Host tests of PianoAuto, the macOS-side consumers of PianoSensors (docs/drivers/sensors.md section 5a), all with -Wall -Wextra
-Werror and ASan/UBSan (compilers from the environment's CC and CXX, default cc and c++):

  c        tests/auto_host.c: TouchRotation.h (PianoTouch's transform) and AutoRotate.c (piano-rotated's policy) over PianoSensors'
           SensorsPolicy.c, which is compiled from its own package, unchanged
  kernel   every part a kernel driver includes compiles freestanding with AArch64 general registers only (no C library, no floating
           point): TouchRotation.h in C and C++, AutoRotate.c (portable, though only piano-rotated uses it)

Synthetic inputs only; nothing here talks to a display, a sensor or a driver."""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PKG = HERE.parent
SENSORS = PKG.parent / 'PianoSensors'
POLICY = [SENSORS / n for n in ('SensorsPolicy.c', 'SensorsOrient.c', 'SensorsFixed.c')]
INCLUDES = ['-I', str(PKG), '-I', str(SENSORS), '-I', str(SENSORS / 'tests')]
SANITIZE = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
WARN = ['-Wall', '-Wextra', '-Werror']
KERNEL = ['-target', 'arm64-apple-macos13', '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', *WARN]


def cc():
    return os.environ.get('CC', 'cc')


def run(exe):
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    text = (result.stdout + result.stderr).strip()
    print(text)
    if result.returncode or not text.splitlines()[-1].startswith('PASS'):
        sys.exit(f'FAIL: {exe.name}')


def c_tests(out, defines=(), extra=()):
    exe = out / 'auto_host'
    subprocess.run([cc(), '-std=c11', *WARN, *SANITIZE, *INCLUDES, *defines, str(HERE / 'auto_host.c'), str(PKG / 'AutoRotate.c'),
                    *map(str, extra), *map(str, POLICY), '-o', str(exe)], check=True)
    run(exe)


def kernel_checks(out, c_files=(), headers=()):
    for source in c_files:
        subprocess.run([cc(), '-std=c11', *KERNEL, *INCLUDES, '-c', str(source), '-o', str(out / 'k.o')], check=True)
    for header in headers:
        for language, std in (('c', 'c11'), ('c++', 'c++17')):
            probe = out / f'probe.{"c" if language == "c" else "cpp"}'
            probe.write_text(f'#include "{header}"\n')
            compiler = cc() if language == 'c' else os.environ.get('CXX', 'c++')
            flags = ['-fno-exceptions', '-fno-rtti'] if language == 'c++' else []
            subprocess.run([compiler, f'-std={std}', *KERNEL, *flags, *INCLUDES, '-c', str(probe), '-o', str(out / 'k.o')], check=True)
    print(f'PASS kernel constraints: {len(c_files)} C files and {len(headers)} headers freestanding, general registers only')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--only', action='append', default=[], choices=['c', 'kernel'])
    args = parser.parse_args()
    want = set(args.only) or {'c', 'kernel'}
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        if 'c' in want:
            c_tests(out)
        if 'kernel' in want:
            kernel_checks(out, c_files=[PKG / 'AutoRotate.c'], headers=['TouchRotation.h'])
    print('PASS PianoAuto host tests')


if __name__ == '__main__':
    main()
