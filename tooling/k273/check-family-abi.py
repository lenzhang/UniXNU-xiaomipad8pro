#!/usr/bin/env python3
"""Are the Apple family classes our native kexts derive from the same in two kernel collections?

  check-family-abi.py REFERENCE_KC TARGET_KC [CLASS ...]      (default: the IOMobileGraphicsFamily, IOGPUFamily-free set below)

For each class: the vtable (slot count, every slot's PAC diversity and the symbol it holds) and the instance size its OSMetaClass
constructor passes. The native kexts' generated declarations (the k27 tree's scripts/gen-iomfb-abi.py, run by
tooling/k27/build-native-gop-display.sh) and the subclass layouts are made from one collection; this says whether they hold for the
other. The 27.3 line uses it to take the IOMobileFramebuffer declarations generated from the 27.0.1 vma2 collection (whose
AppleParavirtDisplay names the pure virtual slots) for 27.3's mac17j collection, which has no IOMobileFramebufferLegacy subclass.
Exit 0 when every class matches, 1 otherwise (each difference printed). --vtables-only: exit 0 when the vtables match, whatever
the instance sizes (then printed as `size CLASS REFERENCE TARGET`, for a caller that lays the subclass out by the target's size).
Reads the k27 tree's scripts (PIANO_K27_TREE).
"""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'k27'))
from k27_tree import k27_tree  # noqa: E402

TREE = k27_tree()
sys.path.insert(0, str(TREE / 'scripts'))
import fileset_kc  # noqa: E402

DEFAULT = [('IOMobileFramebuffer', 'com.apple.iokit.IOMobileGraphicsFamily'),
           ('IOMobileFramebufferLegacy', 'com.apple.iokit.IOMobileGraphicsFamily')]


def vtable(kc_path, cls):
    out = subprocess.run(['python3', str(TREE / 'scripts/kc-vtable.py'), str(kc_path), cls], capture_output=True, text=True)
    if out.returncode:
        return None
    return [tuple(line.split()[:3]) for line in out.stdout.splitlines()[1:]]


def instance_size(kc, cls, kext):
    symbol = f'__ZN{len(cls)}{cls}9MetaClassC2Ev'
    found = [v for n, k, s, d, v in kc.symbols(kc.macho(kext)) if n == symbol]
    if not found:
        return None
    off = kc.file_offset(found[0])
    for i in range(32):
        word = int.from_bytes(kc.data[off + 4 * i:off + 4 * i + 4], 'little')
        if word & 0xffe0001f == 0x52800003:       # movz w3, #imm16
            return (word >> 5) & 0xffff
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    args = [x for x in sys.argv[1:] if x != '--vtables-only']
    vtables_only = len(args) != len(sys.argv) - 1
    ref, target = Path(args[0]), Path(args[1])
    classes = [tuple(c.split('@')) if '@' in c else (c, None) for c in args[2:]] or DEFAULT
    kcs = [fileset_kc.Kernelcache(p.read_bytes()) for p in (ref, target)]
    bad = 0
    for cls, kext in classes:
        a, b = vtable(ref, cls), vtable(target, cls)
        if a is None or b is None:
            print(f'{cls}: no vtable in {"the reference" if a is None else "the target"}')
            bad += 1
            continue
        diff = [(x, y) for x, y in zip(a, b) if x != y]
        sizes = [instance_size(kc, cls, kext) for kc in kcs] if kext else [None, None]
        same = len(a) == len(b) and not diff and (vtables_only or sizes[0] == sizes[1])
        if vtables_only:
            print(f'size {cls} {sizes[0]} {sizes[1]}')
        print(f'{cls}: {len(a)} / {len(b)} slots, {len(diff)} differing, instance size {sizes[0]} / {sizes[1]}: '
              f'{"same" if same else "DIFFERENT"}')
        for x, y in diff[:20]:
            print(f'  slot {x[0]}: {x[1]} {x[2]}  ->  {y[1]} {y[2]}')
        bad += not same
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
