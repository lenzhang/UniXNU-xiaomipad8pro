#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""PianoAudio's macOS devices are C++ subclasses of IOService and IOUserClient whose metaclasses are linked, at run time, under
IOAudio2Family's IOAudio2Device and IOAudio2DeviceUserClient (package/PianoAudio/PianoAudioDevice.cpp, DroidVMAudio's scheme). The KC
builder's vtable check covers subclasses of a kept kext's class; these derive from kernel classes, so this checks what the run-time
link relies on instead, in the KC base the collection is built from:

  - com.apple.iokit.IOAudio2Family is in the base and among the kexts the native collection keeps (its build log, when given);
  - it defines IOAudio2Device's and IOAudio2DeviceUserClient's metaclasses and vtables;
  - their superClass pointers point exactly at IOService's and IOUserClient's metaclasses in the kernel: the C++ bases of PianoAudio's
    classes, so every cast the C++ classes allow stays true after the link.

    check-audio2-family.py KC_BASE [--kc-log KC.log] [--tree K27_TREE]

Exit 0 and one summary line when all hold; 1 with the reason otherwise; 2 when an input is missing.
"""
import argparse
import os
import re
import sys
from pathlib import Path

FAMILY = 'com.apple.iokit.IOAudio2Family'
CLASSES = (('IOAudio2Device', 'IOService'), ('IOAudio2DeviceUserClient', 'IOUserClient'))


def mangled(cls, member):
    return f'__ZN{len(cls)}{cls}{len(member)}{member}E'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('kc', type=Path)
    parser.add_argument('--kc-log', type=Path)
    parser.add_argument('--tree', type=Path, default=Path(os.environ.get(
        'K27_TREE', Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM')))
    args = parser.parse_args()
    if not args.kc.is_file():
        print(f'check-audio2-family: no KC base at {args.kc}', file=sys.stderr)
        return 2
    sys.path.insert(0, str(args.tree / 'scripts'))
    try:
        from fileset_kc import Kernelcache   # the k27 tree's read-only MH_FILESET parser
    except ImportError:
        print(f'check-audio2-family: no fileset_kc.py under {args.tree}/scripts', file=sys.stderr)
        return 2
    kc = Kernelcache(args.kc.read_bytes())
    if FAMILY not in kc.entries:
        print(f'check-audio2-family: {FAMILY} is not in {args.kc.name}')
        return 1
    if args.kc_log:
        kept = re.search(r'^keeping \d+ kexts: (.*)$', args.kc_log.read_text(errors='replace'), re.M)
        if not kept or f"'{FAMILY}'" not in kept.group(1):
            print(f'check-audio2-family: {args.kc_log.name} does not list {FAMILY} among the kept kexts')
            return 1
    family = {name: value for name, kind, sect, desc, value in kc.symbols(kc.macho(FAMILY)) if kind & 0x0e == 0x0e}
    wanted = {mangled(base, 'gMetaClass') for _, base in CLASSES}
    kernel = {name: value for name, kind, sect, desc, value in kc.symbols(kc.macho('com.apple.kernel'))
              if kind & 0x0e == 0x0e and name in wanted}
    sites = {}
    links = {}
    for cls, base in CLASSES:
        for need in (mangled(cls, 'gMetaClass'), mangled(cls, 'superClass'), f'__ZTV{len(cls)}{cls}'):
            if need not in family:
                print(f'check-audio2-family: {FAMILY} does not define {need}')
                return 1
        if mangled(base, 'gMetaClass') not in kernel:
            print(f'check-audio2-family: the kernel does not define {mangled(base, "gMetaClass")}')
            return 1
        sites[family[mangled(cls, 'superClass')]] = cls
    for site, value, target, *rest in kc.chained_sites():
        if site in sites:
            links[sites[site]] = target
    for cls, base in CLASSES:
        target, want = links.get(cls), kernel[mangled(base, 'gMetaClass')]
        if target != want:
            print(f'check-audio2-family: {cls}::superClass points at {target and hex(target)}, not {base}::gMetaClass ({want:#x})')
            return 1
    print(f'check-audio2-family: {FAMILY} defines ' +
          ', '.join(f'{cls} (superclass {base})' for cls, base in CLASSES) +
          ': PianoAudioDevice / PianoAudioUserClient can be linked under them')
    return 0


if __name__ == '__main__':
    sys.exit(main())
