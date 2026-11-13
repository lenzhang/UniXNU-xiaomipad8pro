#!/usr/bin/env python3
"""k27 0066's host side: the kernel arguments beyond DarwinBoot's budget live in the ADT (docs/bringup-debugging.md §5.2).

DarwinBoot passes at most 1023 bytes of command line, and the tools keep the line under LINE_MAX (1000). With k27 0066 in the kernel,
the line's piano-boot-args=NAME makes it append the ADT's /chosen/piano-boot-args-NAME to the boot-args (piano-boot-args=0 none; no
selector: /chosen/piano-boot-args), and every PE_parse_boot_argn() sees both (tooling/k27/piano-bootargs/piano_bootargs_policy.h).

  boot_args.py --show ADT [--line LINE]    the ADT's piano-boot-args sets; with LINE, the boot-args the kernel makes of it
  boot_args.py --self-test                 the split, the ADT edit and the merge against the kernel header's numbers

Library (tooling/bootimg/build-ram-images.py):
  fit(args, set_name, reserve=0, force=False) -> (line args, tail args)   the tail is [] when the line fits (or nothing moves)
  with_set(adt_bytes, set_name, tail) -> ADT bytes with /chosen/piano-boot-args-NAME = tail
  sets(adt_bytes) -> {property: text}
  merged(line, adt_bytes) -> (boot-args, status)   what the kernel makes of LINE with this ADT
  kc_has_boot_args(kc_path)                        the collection's kernel has 0066 (its console line's format)

What moves: every piano-* argument but the ones the boot's safety rests on even under a kernel without 0066 (piano-reset-after,
piano-reset-to, piano-boot-guard*, and the selector itself); everything else (the k27 arguments, native-ufs-*, data-*, pstore-*,
droidvm-*, cpus, sheng-smp) stays on the line. static-if keys (validation_disables, serverperfmode, lcks, wdt) must stay on the
line: static_if_init() reads it before the device tree is up. Order is kept within each part, and the kernel appends the tail after
the line, so for one name the last value still wins as before.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[2]
HEADER = DEVICE / 'tooling/k27/piano-bootargs/piano_bootargs_policy.h'
LINE_MAX = 1000        # the tools' budget for DarwinBoot's line (it passes at most 1023 bytes)
MERGED_MAX = 4095      # PIANO_BOOT_ARGS_SIZE - 1
PROP = 'piano-boot-args'
SET_MAX = 15           # PIANO_BOOT_ARGS_SET_MAX
KC_MARKER = b'piano-boot-args: /chosen/%s (%u bytes): %u bytes appended'
KEEP = ('piano-reset-after', 'piano-reset-to', 'piano-boot-guard', PROP)
SET_RE = re.compile(r'[A-Za-z0-9._-]{1,%d}' % SET_MAX)


def name_of(arg):
    return arg.split('=', 1)[0]


def movable(arg):
    n = name_of(arg)
    return n.startswith('piano-') and n not in KEEP and not n.startswith('piano-boot-guard-')


def prop_name(set_name):
    if set_name is None:
        return PROP
    if set_name == '0' or not SET_RE.fullmatch(set_name):
        raise SystemExit(f'piano-boot-args set {set_name!r}: 1-{SET_MAX} of A-Z a-z 0-9 . _ - (and not "0")')
    return f'{PROP}-{set_name}'


def fit(args, set_name, reserve=0, force=False):
    """(line args, tail args) for ARGS: unchanged with an empty tail when ' '.join(ARGS) + RESERVE bytes stays under LINE_MAX (and
    not FORCE); else the movable arguments go to the tail and the line gets piano-boot-args=SET_NAME. SystemExit when the line still
    does not fit, or the merged boot-args would not fit the kernel's buffer."""
    args = list(args)
    if any(name_of(x) == PROP for x in args):
        raise SystemExit(f'the arguments already have {PROP}=: give the whole (merged) arguments, the tools make the selector')
    line = ' '.join(args)
    if not force and len(line) + reserve < LINE_MAX:
        return args, []
    tail = [x for x in args if movable(x)]
    if not tail:
        return args, []
    kept = [x for x in args if not movable(x)] + [f'{PROP}={set_name}']
    prop_name(set_name)
    line = ' '.join(kept)
    if len(line) + reserve >= LINE_MAX:
        raise SystemExit(f'kernel command line is {len(line) + reserve} bytes even with every piano-* argument in the ADT '
                         f'({len(" ".join(tail))} bytes); DarwinBoot passes at most 1023 (the tools keep it under {LINE_MAX})')
    text = ' '.join(tail)
    if len(line) + reserve + 1 + len(text) > MERGED_MAX:
        raise SystemExit(f'the boot-args with the ADT\'s tail would be {len(line) + reserve + 1 + len(text)} bytes; the kernel takes '
                         f'{MERGED_MAX} (k27 0066 PIANO_BOOT_ARGS_SIZE)')
    return kept, tail


# --- the ADT (the format of tooling/adt/set-identity.py: nodes of props [name, size word, value, raw name] and children) ---

def parse(data, at=0):
    nprops, nchildren = struct.unpack_from('<II', data, at)
    at += 8
    props = []
    for _ in range(nprops):
        name = data[at:at + 32].split(b'\0', 1)[0].decode()
        word = struct.unpack_from('<I', data, at + 32)[0]
        size = word & 0x7fffffff
        props.append([name, word, data[at + 36:at + 36 + size], data[at:at + 32]])
        at += 36 + ((size + 3) & ~3)
    children = []
    for _ in range(nchildren):
        child, at = parse(data, at)
        children.append(child)
    return [props, children], at


def serialize(node):
    props, children = node
    out = bytearray(struct.pack('<II', len(props), len(children)))
    for name, word, value, raw_name in props:
        out += raw_name if raw_name is not None else name.encode().ljust(32, b'\0')
        out += struct.pack('<I', word)
        out += value + bytes(((len(value) + 3) & ~3) - len(value))
    for child in children:
        out += serialize(child)
    return bytes(out)


def load(adt):
    data = bytes(adt)
    root, end = parse(data)
    if end != len(data) or serialize(root) != data:
        raise SystemExit('the ADT does not parse and write back byte for byte; not an ADT this tool can edit')
    return root


def chosen(root):
    for c in root[1]:
        for p in c[0]:
            if p[0] == 'name' and p[2].rstrip(b'\0') == b'chosen':
                return c
    raise SystemExit('the ADT has no /chosen')


def sets(adt):
    """{property: text} of the ADT's /chosen/piano-boot-args*."""
    return {p[0]: p[2].split(b'\0', 1)[0].decode('ascii', 'replace') for p in chosen(load(adt))[0]
            if p[0] == PROP or p[0].startswith(PROP + '-')}


def with_set(adt, set_name, tail):
    """ADT bytes with /chosen/piano-boot-args-SET_NAME (or /chosen/piano-boot-args for None) = the arguments TAIL (a list or a
    string), added or replaced."""
    text = ' '.join(tail) if isinstance(tail, (list, tuple)) else tail
    if not text or text != text.strip(' ') or any(not 0x20 <= ord(c) <= 0x7e for c in text):
        raise SystemExit(f'{PROP}: the tail must be printable ASCII without outer spaces, not empty')
    if len(text) > MERGED_MAX:
        raise SystemExit(f'{PROP}: {len(text)} bytes, more than the kernel\'s {MERGED_MAX}')
    root = load(adt)
    node = chosen(root)
    name = prop_name(set_name)
    value = text.encode('ascii') + b'\0'
    p = next((p for p in node[0] if p[0] == name), None)
    if p:
        if p[1] & 0x80000000:
            raise SystemExit(f'/chosen/{name}: a placeholder property (DarwinBoot fills it)')
        p[1], p[2] = len(value), value
    else:
        node[0].append([name, len(value), value, None])
    out = serialize(root)
    if sets(out).get(name) != text:
        raise SystemExit(f'/chosen/{name}: not read back')
    return out


def selector(args):
    """The line's piano-boot-args value as PE_parse_boot_arg_str reads it (the last one; a bare name is "1"), or None."""
    value = None
    for x in args:
        if name_of(x) == PROP:
            value = x.split('=', 1)[1] if '=' in x else '1'
    return value


def merged(line, adt):
    """(boot-args, status) as k27 0066 makes them of LINE with this ADT: status appended, none, off, bad-set, missing, empty,
    bad-char or too-long (the merged text is LINE then)."""
    line = line[:1023]
    sel = selector(line.split())
    if sel == '0':
        return line, 'off'
    if sel is not None and not SET_RE.fullmatch(sel):
        return line, 'bad-set'
    name = PROP if sel is None else f'{PROP}-{sel}'
    p = next((p for p in chosen(load(adt))[0] if p[0] == name), None)
    if p is None:
        return line, 'none' if sel is None else 'missing'
    raw = p[2].split(b'\0', 1)[0]
    if any(not 0x20 <= b <= 0x7e for b in raw):
        return line, 'bad-char'
    text = raw.decode('ascii').strip(' ')
    if not text:
        return line, 'empty'
    out = f'{line} {text}' if line else text
    if len(out) > MERGED_MAX:
        return line, 'too-long'
    return out, 'appended'


def kc_has_boot_args(kc):
    return KC_MARKER in Path(kc).read_bytes()


def header_numbers():
    text = HEADER.read_text()
    size = int(re.search(r'#define PIANO_BOOT_ARGS_SIZE\s+(\d+)', text).group(1))
    set_max = int(re.search(r'#define PIANO_BOOT_ARGS_SET_MAX\s+(\d+)', text).group(1))
    prop = re.search(r'#define PIANO_BOOT_ARGS_PROP\s+"([^"]+)"', text).group(1)
    return size, set_max, prop


def tiny_adt():
    def prop(name, value):
        return name.encode().ljust(32, b'\0') + struct.pack('<I', len(value)) + value + bytes(((len(value) + 3) & ~3) - len(value))
    chosen_node = struct.pack('<II', 2, 0) + prop('name', b'chosen\0') + prop('placeholder', b'\0' * 8)
    other = struct.pack('<II', 1, 0) + prop('name', b'arm-io\0')
    return struct.pack('<II', 1, 2) + prop('name', b'device-tree\0') + other + chosen_node


def self_test():
    size, set_max, prop = header_numbers()
    assert (size - 1, set_max, prop) == (MERGED_MAX, SET_MAX, PROP), 'boot_args.py and piano_bootargs_policy.h disagree'
    # nothing moves while the line fits
    short = ['debug=0x14e', 'piano-gpu=2', 'data-persist=0']
    assert fit(short, 'ram') == (short, [])
    # forced: the piano-* arguments go, the safety ones and the rest stay, order kept, the selector last
    args = ['debug=0x14e', 'piano-gpu=2', 'piano-reset-after=600', 'piano-reset-to=bootloader', 'native-ufs-root=1',
            'piano-boot-guard=180', 'piano-boot-guard-ping=0', 'piano-audio=1', 'data-persist=0', 'piano-gpu=3']
    line, tail = fit(args, 'ram', force=True)
    assert line == ['debug=0x14e', 'piano-reset-after=600', 'piano-reset-to=bootloader', 'native-ufs-root=1', 'piano-boot-guard=180',
                    'piano-boot-guard-ping=0', 'data-persist=0', 'piano-boot-args=ram'], line
    assert tail == ['piano-gpu=2', 'piano-audio=1', 'piano-gpu=3'], tail
    # a long line splits by itself, and RESERVE counts
    long = ['debug=0x14e'] + [f'piano-x{i}={"y" * 40}' for i in range(30)]
    line, tail = fit(long, 'ram')
    assert len(' '.join(line)) < LINE_MAX and len(tail) == 30
    edge = ['a' * 990]
    assert fit(edge, 'ram', reserve=9) == (edge, [])
    try:
        fit(edge + ['piano-q=1'], 'ram', reserve=10)
    except SystemExit:
        pass
    else:
        raise AssertionError('a line that cannot get under the budget must be refused')
    for bad in ('0', '', 'a' * 16, 'a b', 'a/b'):
        try:
            prop_name(bad)
        except SystemExit:
            continue
        raise AssertionError(f'set name {bad!r} accepted')
    try:
        fit(['piano-boot-args=x', 'piano-a=1'], 'ram', force=True)
    except SystemExit:
        pass
    else:
        raise AssertionError('arguments with a selector must be refused')
    # the ADT: added, replaced, read back, the rest of the tree byte for byte
    adt = tiny_adt()
    one = with_set(adt, 'ram', ['piano-gpu=2', 'piano-audio=1'])
    assert sets(one) == {'piano-boot-args-ram': 'piano-gpu=2 piano-audio=1'}
    two = with_set(one, 'ram', 'piano-audio=2')
    assert sets(two) == {'piano-boot-args-ram': 'piano-audio=2'} and len(two) == len(one) - 12
    before, after = load(adt), load(two)
    assert before[0] == after[0] and before[1][0] == after[1][0], 'the root and the other nodes must not change'
    assert after[1][1][0][:2] == before[1][1][0] and after[1][1][1] == before[1][1][1], '/chosen keeps its properties, one added'
    both = with_set(two, None, 'piano-x=1')
    assert sets(both) == {'piano-boot-args-ram': 'piano-audio=2', 'piano-boot-args': 'piano-x=1'}
    # the kernel's merge, redone: the selector picks the set, no selector the plain property, =0 none
    assert merged('debug=0x14e piano-boot-args=ram', one) == ('debug=0x14e piano-boot-args=ram piano-gpu=2 piano-audio=1', 'appended')
    assert merged('debug=0x14e', one) == ('debug=0x14e', 'none')
    assert merged('debug=0x14e', both) == ('debug=0x14e piano-x=1', 'appended')
    assert merged('a piano-boot-args=0', both)[1] == 'off'
    assert merged('a piano-boot-args=other', both)[1] == 'missing'
    assert merged('a piano-boot-args=a/b', both)[1] == 'bad-set'
    full = with_set(adt, None, 'x' * (MERGED_MAX - 2))
    assert merged('a', full)[1] == 'appended' and len(merged('a', full)[0]) == MERGED_MAX
    assert merged('ab', full)[1] == 'too-long'
    for bad in ('', ' x', 'x ', 'a\tb', 'é'):
        try:
            with_set(adt, 'ram', bad)
        except SystemExit:
            continue
        raise AssertionError(f'tail {bad!r} accepted')
    print('boot_args.py: self-test passed (split, ADT set, merge; header numbers PIANO_BOOT_ARGS_SIZE '
          f'{size}, SET_MAX {set_max})')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--show', type=Path, metavar='ADT')
    p.add_argument('--line', help='a command line: print the boot-args the kernel makes of it with --show\'s ADT')
    p.add_argument('--self-test', action='store_true')
    a = p.parse_args()
    if a.self_test:
        self_test()
        return
    if not a.show:
        p.error('--show ADT or --self-test')
    data = a.show.read_bytes()
    found = sets(data)
    for name, text in found.items():
        print(f'/chosen/{name} ({len(text) + 1} bytes): {text}')
    if not found:
        print('no /chosen/piano-boot-args* in this ADT')
    if a.line is not None:
        text, status = merged(a.line, data)
        print(f'{status}: boot-args {len(text)} bytes (line {len(a.line)}): {text}')


if __name__ == '__main__':
    main()
