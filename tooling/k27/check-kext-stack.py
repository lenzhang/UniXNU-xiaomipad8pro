#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""The deepest stack a native kext's own code can take from a thread's entry, before the kernel's frames (a static bound).

    check-kext-stack.py OBJECT.o ENTRY [--indirect NAME,...] [--max BYTES]

Each function's frame is read from its prologue (`stp ..., [sp, #-N]!` and `sub sp, sp, #N`), its callees from the object's
BRANCH26 relocations (bl and tail calls); a function with an indirect call (blr) is given edges to every function whose name
contains one of --indirect (the kext's function-pointer targets). Functions outside the object count 0: the kernel's own frames
come on top of the result (XNU's IOKit mapping path took about 3-4 KiB on piano). A kernel thread has 16 KiB (KERNEL_STACK_SIZE);
the camera thread's sequence overflowed it on 2026-11-13 (c3rp: `Invalid kernel stack pointer (probable overflow)`). ENTRY is a
substring of the mangled name. Prints the path; exit 1 when it is deeper than --max.
"""
import argparse
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('object')
    parser.add_argument('entry')
    parser.add_argument('--indirect', default='')
    parser.add_argument('--max', type=int, default=0)
    parser.add_argument('--brief', action='store_true', help='one line per entry (the path only when over --max)')
    args = parser.parse_args()
    dis = subprocess.run(['xcrun', 'llvm-objdump', '-dr', '--no-show-raw-insn', args.object], capture_output=True, text=True,
                         check=True).stdout
    frame, calls, indirect, fn = {}, {}, set(), None
    for line in dis.split('\n'):
        m = re.match(r'^[0-9a-f]+ <(.+)>:', line)
        if m:
            fn = None if m.group(1).startswith('ltmp') else m.group(1)
            if fn:
                frame.setdefault(fn, 0)
                calls.setdefault(fn, set())
            continue
        if not fn:
            continue
        m = re.search(r'\bsub\s+sp, sp, #(0x[0-9a-f]+|\d+)(, lsl #12)?', line)
        if m:
            frame[fn] += int(m.group(1), 0) << (12 if m.group(2) else 0)
        m = re.search(r'\bstp\s+[xdq]\d+, [xdq]\d+, \[sp, #-(0x[0-9a-f]+|\d+)\]!', line)
        if m:
            frame[fn] += int(m.group(1), 0)
        m = re.search(r'ARM64_RELOC_BRANCH26\s+(\S+)', line)
        if m:
            calls[fn].add(m.group(1))
        if re.search(r'\bblr(aa|ab|aaz|abz)?\b', line):
            indirect.add(fn)
    targets = [t for t in args.indirect.split(',') if t]
    for f in indirect:
        calls[f] |= {k for k in frame for t in targets if t in k}
    memo = {}

    def depth(f, seen):
        if f not in frame or f in seen:
            return 0, []
        if f in memo:
            return memo[f]
        best = (0, [])
        for c in sorted(calls[f]):
            d = depth(c, seen | {f})
            if d[0] > best[0]:
                best = d
        memo[f] = (frame[f] + best[0], [f] + best[1])
        return memo[f]

    roots = [k for k in frame if args.entry in k]
    if not roots:
        sys.exit(f'check-kext-stack: no function matching {args.entry} in {args.object}')
    worst = 0
    for r in roots:
        total, path = depth(r, frozenset())
        worst = max(worst, total)
        names = subprocess.run(['xcrun', 'c++filt'], input='\n'.join(path), capture_output=True, text=True).stdout.split('\n')
        print(f'check-kext-stack: {names[0][:90]}: at most {total} bytes of its own frames'
              + (f' (limit {args.max})' if args.max else ''))
        if args.brief and not (args.max and total > args.max):
            continue
        at = 0
        for name, shown in zip(path, names):
            at += frame[name]
            print(f'  {frame[name]:6d} {at:6d}  {shown[:110]}')
    if args.max and worst > args.max:
        print(f'check-kext-stack: deeper than {args.max} bytes: the kernel paths below it may overflow the 16 KiB stack', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
