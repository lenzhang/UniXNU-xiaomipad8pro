#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Space in a built k27 kernel collection: the native kexts' segment sizes, the free gaps per segment kind, the headroom left.

    kc-space.py KC.bin [--natives KC.natives.json] [--top N] [--gaps N] [--fit KIND=SIZE ...]
                [--simulate] [--drop NAME ...] [--add NAME:KIND=SIZE[,KIND=SIZE...] ...] [--list FILE] [--json OUT]

The KC builder of the k27 tree (k27/kc-build27-piano.py over scripts/kc-build.py and kc_native.py, read here, never imported)
places each native kext's four segments into what is left between the kernel's and the kept Apple kexts' segments, one region
per kind (offsets from the collection base):

  __TEXT        0x4000 .. the end of the collection's __PRELINK_TEXT
  __TEXT_EXEC   the collection's __TEXT_EXEC start .. the kernel's __TEXT_EXEC start
  __DATA_CONST  the end of the kernel's __LASTDATA_CONST .. the end of the collection's __DATA_CONST
  __DATA        the kernel's __PRELINK_DATA .. the end of the collection's __DATA

first fit, in link order (the descriptor order of configs/k27-native-kexts.txt), 16-byte aligned. Everything here is read back
from the built collection (its fileset entries' load commands), so it measures what the build did; no k27 tree is needed.

--natives (default KC.natives.json next to the KC): the native kexts' external symbols, for the largest symbols of their
  __DATA_CONST and __DATA (a symbol's size is the distance to the next symbol or section end: approximate, locals are not listed).
--fit KIND=SIZE: would one more segment of SIZE bytes (0x.. or decimal, K/M suffixes) fit in KIND's gaps now.
--simulate: the builder's placement replayed from the fixed segments (kernel + kept kexts) and the natives' sizes (checked to
  reproduce the build's addresses), then the same set placed largest first (first fit and best fit): the headroom each leaves.
  --drop NAME (a native's short name, e.g. PianoTouch) and --add NAME:KIND=SIZE,... change the simulated set (an added kext
  takes its place of --list, default configs/k27-native-kexts.txt, else goes last).
--json OUT: everything above as JSON.
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KINDS = ('__TEXT', '__TEXT_EXEC', '__DATA_CONST', '__DATA')
SHORT = {'__TEXT': 'TEXT', '__TEXT_EXEC': 'TEXT_EXEC', '__DATA_CONST': 'DATA_CONST', '__DATA': 'DATA'}
ALIGN = 16                      # kc-build.py: kc_native.link asks allocate(segment, size, 16)
NATIVE_PREFIX = 'com.unixnu.'   # this repository's bundles (tooling/k27/kext/*.json)
LC_SEGMENT_64, LC_FILESET_ENTRY = 0x19, 0x80000035


def up(value, align=ALIGN):
    return (value + align - 1) // align * align


def cstr(raw):
    return raw.split(b'\0')[0].decode(errors='replace')


def kib(value):
    return f'{value / 1024:.1f}'


def parse_size(text):
    m = re.fullmatch(r'(0x[0-9a-fA-F]+|\d+)([kKmM]?)', text.strip())
    if not m:
        raise SystemExit(f'{text}: a size (0x.., decimal, optional K/M)')
    value = int(m.group(1), 0)
    return value * {'': 1, 'k': 1024, 'm': 1 << 20}[m.group(2).lower()]


def parse_kind(text):
    name = text if text.startswith('__') else '__' + text.upper()
    if name not in KINDS:
        raise SystemExit(f'{text}: one of {", ".join(SHORT[k] for k in KINDS)}')
    return name


class Collection:
    """An MH_FILESET kernel collection: top-level segments and every fileset entry's segments and sections."""

    def __init__(self, path):
        data = self.data = Path(path).read_bytes()
        magic, cpu, _, kind, count, _ = struct.unpack_from('<6I', data, 0)
        if (magic, kind) != (0xfeedfacf, 12):
            raise SystemExit(f'{path}: not an MH_FILESET kernel collection')
        self.top, self.entries = {}, []
        offset = 32
        for _ in range(count):
            cmd, length = struct.unpack_from('<II', data, offset)
            if cmd == LC_SEGMENT_64:
                segment = self.segment(offset)
                self.top[segment['name']] = segment
            elif cmd == LC_FILESET_ENTRY:
                va, fileoff, name_at = struct.unpack_from('<QQI', data, offset + 8)
                self.entries.append({'bundle': cstr(data[offset + name_at:offset + length]), 'va': va, 'fileoff': fileoff})
            offset += length
        self.base = self.top['__TEXT']['va']
        for entry in self.entries:
            entry['segments'] = self.macho_segments(entry['fileoff'])

    def segment(self, offset):
        name, va, size, fileoff, filesize, _, _, nsects, _ = struct.unpack_from('<16s4Q4I', self.data, offset + 8)
        sections = []
        for index in range(nsects):
            sname, _, sva, ssize = struct.unpack_from('<16s16sQQ', self.data, offset + 72 + index * 80)
            sections.append({'name': cstr(sname), 'va': sva, 'size': ssize})
        return {'name': cstr(name), 'va': va, 'size': size, 'sections': sections}

    def macho_segments(self, at):
        count, = struct.unpack_from('<I', self.data, at + 16)
        offset, out = at + 32, []
        for _ in range(count):
            cmd, length = struct.unpack_from('<II', self.data, offset)
            if cmd == LC_SEGMENT_64:
                out.append(self.segment(offset))
            offset += length
        return out

    def rel(self, va):
        return va - self.base

    def entry(self, bundle):
        return next(e for e in self.entries if e['bundle'] == bundle)

    def seg(self, bundle, name):
        return next(s for s in self.entry(bundle)['segments'] if s['name'] == name)

    def regions(self):
        """The four allocation regions of kc-build.py's allocate(), as offsets from the base."""
        kernel = 'com.apple.kernel'
        end = lambda s: self.rel(s['va']) + s['size']   # noqa: E731
        last = self.seg(kernel, '__LASTDATA_CONST')
        return {'__TEXT': (0x4000, end(self.top['__PRELINK_TEXT'])),
                '__TEXT_EXEC': (self.rel(self.top['__TEXT_EXEC']['va']), self.rel(self.seg(kernel, '__TEXT_EXEC')['va'])),
                '__DATA_CONST': (end(last), end(self.top['__DATA_CONST'])),
                '__DATA': (self.rel(self.seg(kernel, '__PRELINK_DATA')['va']), end(self.top['__DATA']))}


def spans(collection, entries):
    """(start, end, bundle) of every non-empty segment but the shared __LINKEDIT, as offsets."""
    out = []
    for entry in entries:
        for s in entry['segments']:
            if s['size'] and s['name'] != '__LINKEDIT':
                start = collection.rel(s['va'])
                out.append((start, start + s['size'], entry['bundle']))
    return sorted(out)


def free_gaps(region, used):
    """Free (start, end) intervals of REGION outside USED, each start 16-byte aligned (a native segment's alignment)."""
    low, high = region
    gaps, at = [], low
    for start, end, *_ in sorted(used) + [(high, high)]:
        if end <= low or start >= high and at >= high:
            continue
        start, end = max(start, low), min(end, high)
        if start > at and start - up(at) > 0:
            gaps.append((up(at), start))
        at = max(at, end)
    return gaps


def first_fit(region, used, size):
    """kc-build.py's allocate(): the lowest 16-byte aligned place of SIZE above region low, or None."""
    low, high = region
    cursor = low
    for start, end, *_ in sorted(used):
        if end <= cursor:
            continue
        if start >= cursor + size:
            break
        cursor = up(max(cursor, end))
    return cursor if cursor + size <= high else None


def best_fit(region, used, size):
    """The smallest gap that holds SIZE (lowest such gap on a tie), or None."""
    fits = [(end - start, start) for start, end in free_gaps(region, used) if end - start >= size]
    return min(fits)[1] if fits else None


def place(regions, fixed, kexts, strategy):
    """Place KEXTS ([{name, sizes: {kind: size}}], link order) over FIXED spans. Returns ({name: {kind: offset}}, failures, used)."""
    used = {kind: [s for s in fixed if regions[kind][0] < s[1] and s[0] < regions[kind][1]] for kind in KINDS}
    order = list(kexts)
    if strategy != 'first-fit':
        # largest first per kind: each kind's segments are placed independently of the others
        pass
    placed, failures = {k['name']: {} for k in kexts}, []
    for kind in KINDS:
        sequence = [k for k in order if k['sizes'].get(kind)]
        if strategy != 'first-fit':
            sequence = sorted(sequence, key=lambda k: -k['sizes'][kind])   # stable: link order among equal sizes
        for k in sequence:
            size = k['sizes'][kind]
            at = (best_fit if strategy == 'largest-first best-fit' else first_fit)(regions[kind], used[kind], size)
            if at is None:
                failures.append((k['name'], kind, size, max((e - s for s, e in free_gaps(regions[kind], used[kind])), default=0)))
                continue
            placed[k['name']][kind] = at
            used[kind].append((at, at + size, k['name']))
    return placed, failures, used


def summary(regions, used):
    out = {}
    for kind in KINDS:
        gaps = free_gaps(regions[kind], used[kind])
        sizes = sorted((e - s for s, e in gaps), reverse=True)
        out[kind] = {'free': sum(sizes), 'largest': sizes[0] if sizes else 0, 'gaps': len(sizes), 'second': sizes[1] if len(sizes) > 1 else 0}
    return out


def symbol_sizes(natives, symbols):
    """[(size, symbol, bundle, kind, section)] for the natives' external symbols in __DATA_CONST and __DATA. What lies before a
    section's first external symbol (local data: statics, a zerofill section's buffers) is listed as '(locals before ...)'."""
    out = []
    for entry in natives:
        for s in entry['segments']:
            if s['name'] not in ('__DATA_CONST', '__DATA'):
                continue
            for section in s['sections']:
                low, high = section['va'], section['va'] + section['size']
                inside = sorted((va, name) for name, va in symbols.items() if low <= va < high)
                if not inside or inside[0][0] > low:
                    first = inside[0][1] if inside else None
                    out.append(((inside[0][0] if inside else high) - low, f'(locals before {first})' if first else '(locals only)',
                                entry['bundle'], s['name'], section['name']))
                for index, (va, name) in enumerate(inside):
                    end = inside[index + 1][0] if index + 1 < len(inside) else high
                    out.append((end - va, name, entry['bundle'], s['name'], section['name']))
    return sorted(out, reverse=True)


def short(bundle):
    return bundle.rsplit('.', 1)[-1]


def list_order(path):
    if not path or not Path(path).is_file():
        return []
    return [line.split('#', 1)[0].split()[0] for line in Path(path).read_text().splitlines() if line.split('#', 1)[0].strip()]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('kc', type=Path)
    p.add_argument('--natives', type=Path, help='KC.natives.json (default: next to the KC)')
    p.add_argument('--top', type=int, default=12, help='largest native data symbols to list (0: none)')
    p.add_argument('--gaps', type=int, default=6, help='largest gaps to list per kind')
    p.add_argument('--fit', action='append', default=[], metavar='KIND=SIZE')
    p.add_argument('--simulate', action='store_true')
    p.add_argument('--drop', action='append', default=[], metavar='NAME')
    p.add_argument('--add', action='append', default=[], metavar='NAME:KIND=SIZE,...')
    p.add_argument('--list', type=Path, default=REPO / 'configs/k27-native-kexts.txt', help='link order for --add')
    p.add_argument('--json', type=Path)
    p.add_argument('--brief', action='store_true', help='one line: free and largest gap per kind (the KC build prints it)')
    a = p.parse_args()

    kc = Collection(a.kc)
    regions = kc.regions()
    natives = [e for e in kc.entries if e['bundle'].startswith(NATIVE_PREFIX)]
    fixed_entries = [e for e in kc.entries if not e['bundle'].startswith(NATIVE_PREFIX)]
    fixed = spans(kc, fixed_entries)
    used_now = spans(kc, kc.entries)
    if a.brief:
        parts = []
        for k in KINDS:
            sizes = [e - s for s, e in free_gaps(regions[k], used_now)]
            parts.append(f'{SHORT[k]} {kib(sum(sizes))} free, largest {kib(max(sizes, default=0))}')
        print(f'kc-space: {len(natives)} native kexts; ' + '; '.join(parts) + ' (KiB; tooling/k27/kc-space.py has more)')
        return None
    report = {'kc': str(a.kc), 'base': hex(kc.base), 'kept_kexts': len(fixed_entries) - 1, 'natives': [], 'regions': {}}

    print(f'{a.kc.name}: base {kc.base:#x}, {len(fixed_entries) - 1} kept Apple kexts, {len(natives)} native kexts (link order)')
    # 1. per native kext
    print(f'\n{"native kext":22}' + ''.join(f'{SHORT[k]:>12}' for k in KINDS) + f'{"total":>12}   (KiB)')
    totals = dict.fromkeys(KINDS, 0)
    for entry in natives:
        sizes = {s['name']: s['size'] for s in entry['segments'] if s['name'] in KINDS}
        for k in KINDS:
            totals[k] += sizes.get(k, 0)
        report['natives'].append({'bundle': entry['bundle'], 'sizes': sizes,
                                  'at': {s['name']: hex(kc.rel(s['va'])) for s in entry['segments'] if s['name'] in KINDS}})
        print(f'{short(entry["bundle"]):22}' + ''.join(f'{kib(sizes.get(k, 0)):>12}' for k in KINDS) + f'{kib(sum(sizes.values())):>12}')
    print(f'{"all natives":22}' + ''.join(f'{kib(totals[k]):>12}' for k in KINDS) + f'{kib(sum(totals.values())):>12}')
    kept = dict.fromkeys(KINDS, 0)
    for start, end, bundle in fixed:
        for k in KINDS:
            if bundle != 'com.apple.kernel' and regions[k][0] <= start < regions[k][1]:
                kept[k] += end - start
    print(f'{"kept Apple kexts":22}' + ''.join(f'{kib(kept[k]):>12}' for k in KINDS))

    # 2. free gaps per kind, headroom
    print(f'\n{"region":14}{"bounds (offsets)":>26}{"size":>10}{"free":>10}{"gaps":>6}{"largest":>10}   largest native segment (KiB)')
    for k in KINDS:
        low, high = regions[k]
        gaps = sorted(free_gaps(regions[k], [s for s in used_now]), key=lambda g: g[0] - g[1])
        sizes = [e - s for s, e in gaps]
        biggest = max(((s['size'], short(e['bundle'])) for e in natives for s in e['segments'] if s['name'] == k), default=(0, '-'))
        report['regions'][k] = {'low': hex(low), 'high': hex(high), 'free': sum(sizes), 'gaps': len(sizes),
                                'largest_gap': sizes[0] if sizes else 0, 'largest_native': {'bytes': biggest[0], 'kext': biggest[1]},
                                'top_gaps': [{'at': hex(s), 'bytes': e - s} for s, e in gaps[:a.gaps]]}
        print(f'{SHORT[k]:14}{f"{low:#x}-{high:#x}":>26}{kib(high - low):>10}{kib(sum(sizes)):>10}{len(sizes):>6}'
              f'{kib(sizes[0] if sizes else 0):>10}   {kib(biggest[0])} {biggest[1]}')
    print('\nlargest gaps per kind (offset: KiB):')
    for k in KINDS:
        top = report['regions'][k]['top_gaps']
        print(f'  {SHORT[k]:11} ' + ', '.join(f'{g["at"]}: {kib(g["bytes"])}' for g in top))
    print('\nheadroom: the largest segment of each kind one more native kext could have now: '
          + ', '.join(f'{SHORT[k]} {kib(report["regions"][k]["largest_gap"])} KiB' for k in KINDS))

    # 3. --fit
    for item in a.fit:
        kind, _, size = item.partition('=')
        kind, size = parse_kind(kind), parse_size(size)
        at = first_fit(regions[kind], used_now, up(size))
        verdict = f'fits at {at:#x}' if at is not None else f'does NOT fit (largest gap {kib(report["regions"][kind]["largest_gap"])} KiB)'
        report.setdefault('fit', []).append({'kind': kind, 'bytes': size, 'at': hex(at) if at is not None else None})
        print(f'fit {SHORT[kind]} {size:#x} ({kib(size)} KiB): {verdict}')

    # 4. what the natives' data segments hold: sections, then the largest symbols
    print('\nnative data sections over 4 KiB (KiB; a zerofill __bss/__common takes collection space like any other):')
    for entry in natives:
        big = [(s['name'], x['name'], x['size']) for s in entry['segments'] if s['name'] in ('__DATA_CONST', '__DATA')
               for x in s['sections'] if x['size'] > 4096]
        if big:
            print(f'  {short(entry["bundle"]):20} ' + ', '.join(f'{SHORT[k]},{n} {kib(n_)}' for k, n, n_ in big))
    natives_json = a.natives or a.kc.with_suffix('.natives.json')
    if a.top and natives_json.is_file():
        symbols = json.loads(natives_json.read_text())
        sized = symbol_sizes(natives, symbols)
        report['largest_symbols'] = {}
        for kind in ('__DATA_CONST', '__DATA'):
            biggest = [x for x in sized if x[3] == kind][:a.top]
            report['largest_symbols'][kind] = [{'bytes': n, 'symbol': s, 'kext': short(b), 'section': c} for n, s, b, k, c in biggest]
            print(f'\nlargest native {SHORT[kind]} symbols ({natives_json.name}; a size runs to the next external symbol):')
            for n, s, b, k, c in biggest:
                print(f'  {kib(n):>8} KiB  {short(b):18} {c:16} {s}')
    elif a.top:
        print(f'\n({natives_json} not found: no symbol sizes)')

    # 5. --simulate
    if a.simulate:
        simulate(a, kc, regions, natives, fixed, report)
    if a.json:
        a.json.write_text(json.dumps(report, indent=1) + '\n')


def simulate(a, kc, regions, natives, fixed, report):
    kexts = [{'name': short(e['bundle']), 'sizes': {s['name']: s['size'] for s in e['segments'] if s['name'] in KINDS},
              'at': {s['name']: kc.rel(s['va']) for s in e['segments'] if s['name'] in KINDS}} for e in natives]
    # the replay must give the build's own addresses, else the model is wrong and nothing below means anything
    replay, failures, _ = place(regions, fixed, kexts, 'first-fit')
    wrong = [(k['name'], kind) for k in kexts for kind in KINDS if k['sizes'].get(kind) and replay[k['name']].get(kind) != k['at'][kind]]
    print(f'\nsimulation: first fit in link order replayed over the kernel and the kept kexts: '
          + ('the build\'s addresses, all of them' if not wrong and not failures else f'DIFFERS from the build at {wrong[:6]}'))
    report['simulation'] = {'replay_matches': not wrong and not failures, 'changes': {'drop': a.drop, 'add': a.add}}
    names = {k['name'] for k in kexts}
    for name in a.drop:
        if name not in names:
            raise SystemExit(f'--drop {name}: not a native kext of this collection ({sorted(names)})')
    kexts = [k for k in kexts if k['name'] not in a.drop]
    order = list_order(a.list)
    for item in a.add:
        name, _, spec = item.partition(':')
        sizes = {}
        for part in filter(None, spec.split(',')):
            kind, _, size = part.partition('=')
            sizes[parse_kind(kind)] = up(parse_size(size))
        if not name or not sizes:
            raise SystemExit(f'--add {item}: NAME:KIND=SIZE[,KIND=SIZE...]')
        new = {'name': name, 'sizes': sizes, 'at': {}}
        if name in order:   # its place in the kext list: before the first listed kext that comes after it
            later = set(order[order.index(name) + 1:])
            index = next((i for i, k in enumerate(kexts) if k['name'] in later), len(kexts))
        else:
            index = len(kexts)
        kexts.insert(index, new)
    if a.drop or a.add:
        print(f'  changed set: drop {a.drop or "-"}, add {[k.split(":")[0] for k in a.add] or "-"}; link order now '
              + ' '.join(k['name'] for k in kexts))
    print(f'  {"strategy":26}' + ''.join(f'{SHORT[k] + " left/largest":>26}' for k in KINDS) + '   (KiB)')
    for strategy in ('first-fit', 'largest-first first-fit', 'largest-first best-fit'):
        _, failures, used = place(regions, fixed, kexts, strategy)
        s = summary(regions, used)
        report['simulation'][strategy] = {'failures': [{'kext': n, 'kind': k, 'bytes': b, 'largest_gap': g} for n, k, b, g in failures],
                                          **{k: s[k] for k in KINDS}}
        label = 'first-fit (the builder)' if strategy == 'first-fit' else strategy
        print(f'  {label:26}' + ''.join(f'{kib(s[k]["free"]) + " / " + kib(s[k]["largest"]):>26}' for k in KINDS))
        for n, k, b, g in failures:
            print(f'      no room: {n} {SHORT[k]} {kib(b)} KiB (largest gap then {kib(g)} KiB)')


if __name__ == '__main__':
    sys.exit(main())
