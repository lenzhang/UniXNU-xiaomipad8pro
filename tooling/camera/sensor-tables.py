#!/usr/bin/env python3
"""piano's camera sensor register tables (data/camera/*.tbl): extract, check, and emit the C header PianoCamera builds from.

The tables are hardware data, register addresses and values with the modes' timing parameters. They come from the Ubuntu piano
kernel's sensor drivers at a pinned commit (data/camera/README.md: which project, driver, commit and file hash; the owner's
decision of 2026-11-12, docs/BACKLOG-hardware.md section 4). Only the data is carried: no function, comment or structure of the
drivers. The few register writes those drivers make outside their arrays (a stream-start preamble, the stream on/off bit) are
written below as data with the function they were read from.

    sensor-tables.py extract SRC OUT       parse SRC (a checkout of the pinned kernel, tooling/camera/fetch-ubuntu-camera-sources.sh)
                                           and write OUT/<sensor>.tbl
    sensor-tables.py check DIR [--src SRC] DIR/*.tbl against DIR/MANIFEST (each file's SHA-256 and every section's entry count) and
                                           the format's rules; with --src, also re-extract from SRC and require the same bytes
    sensor-tables.py header DIR OUT.h      the C header (one packed array of writes, one section index, the mode parameters)
    sensor-tables.py manifest DIR          print the MANIFEST lines for DIR's tables (after a deliberate re-extraction)
    sensor-tables.py --self-test           the parser, the checker and the header against synthetic sources

Table format (text, one item a line, '#' starts a comment):
    sensor NAME                  first line that is not a comment
    source PATH SHA256           the driver file the data was read from
    [KIND NAME]                  a section: KIND is start | init | mode | on | off; NAME is the mode's WxH for a mode, else KIND
    p KEY VALUE                  a mode parameter (decimal): width, height, hts, vts, exposure, margin, line_length, frame_length, ...
    w BITS ADDR VALUE            a register write: BITS 8, 16 or 24; ADDR and VALUE hexadecimal (0x...)
    d MIN_US MAX_US              a delay before the next write
Sections are written in this order when a stream starts: start, init, the mode, (the driver's own controls), on; off stops it.
"""
import hashlib
import re
import sys
import tempfile
from pathlib import Path

PINNED_COMMIT = '7f8d6bedde479a01f4a05d6d0581b43c7fd20a01'   # yzddmr6/linux-sm8750-piano, piano/dev (v1.2.0 merged)
KINDS = ('start', 'init', 'mode', 'on', 'off')

# Per sensor: the driver file, which array is which section, the mode-parameter struct array and the fields kept, and the writes the
# driver makes outside its arrays (data, with the function they come from).
SENSORS = {
    's5kjn1': {
        'file': 'drivers/media/i2c/s5kjn1.c',
        'arrays': {
            'init_array_setting': ('init', 'init'),
            's5kjn1_4080x3072_30fps_mode': ('mode', '4080x3072'),
            's5kjn1_8160x6144_10fps_mode': ('mode', '8160x6144'),
        },
        'modes': ('s5kjn1_supported_modes', ('width', 'height', 'hts', 'vts', 'exposure', 'exposure_margin')),
        'mode_tables': {'s5kjn1_4080x3072_30fps_mode': '4080x3072', 's5kjn1_8160x6144_10fps_mode': '8160x6144'},
        # s5kjn1_enable_streams: before init_array_setting; then the mode list, the controls, and CTRL_MODE 0x0100 bit 0.
        'start': [('w', 16, 0x6028, 0x4000), ('w', 16, 0x0000, 0x0003), ('w', 16, 0x0000, 0x38e1), ('w', 16, 0x001e, 0x0007),
                  ('w', 16, 0x6028, 0x4000), ('w', 16, 0x6010, 0x0001), ('d', 5000, 6000), ('w', 16, 0x6226, 0x0001),
                  ('d', 10000, 11000)],
        'on': [('w', 8, 0x0100, 0x01)],    # s5kjn1_enable_streams, last write
        'off': [('w', 8, 0x0100, 0x00)],   # s5kjn1_disable_streams
    },
    'ov32d40': {
        'file': 'drivers/media/i2c/ov32d40.c',
        'arrays': {
            'ov32d40_init_regs': ('init', 'init'),
            'ov32d40_6528x4896_regs': ('mode', '6528x4896'),
            'ov32d40_3264x2448_regs': ('mode', '3264x2448'),
            'ov32d40_3264x1836_regs': ('mode', '3264x1836'),
            'ov32d40_streamon_regs': ('on', 'on'),
            'ov32d40_streamoff_regs': ('off', 'off'),
        },
        'modes': ('supported_modes', ('width', 'height', 'line_length', 'frame_length', 'sensor_frame_length')),
        'mode_tables': {'ov32d40_6528x4896_regs': '6528x4896', 'ov32d40_3264x2448_regs': '3264x2448',
                        'ov32d40_3264x1836_regs': '3264x1836'},
        # __ov32d40_start_stream: before ov32d40_init_regs.
        'start': [('w', 8, 0x1001, 0x04), ('d', 500, 1000)],
    },
}


class Bad(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


# ------------------------------------------------------------------------------------------------------------- extraction
def defines(src):
    """Register-name macros of the form NAME CCI_REGn(0x....)."""
    out = {}
    for m in re.finditer(r'^#define\s+(\w+)\s+CCI_REG(8|16|24)\((0x[0-9a-fA-F]+)\)', src, re.M):
        out[m.group(1)] = (int(m.group(2)), int(m.group(3), 16))
    return out


def reg_arrays(src):
    """Every 'static const struct cci_reg_sequence NAME[] = { ... };' as (name, body)."""
    return [(m.group(1), m.group(2)) for m in
            re.finditer(r'static\s+const\s+struct\s+cci_reg_sequence\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\n\};', src, re.S)]


def entries(body, names):
    out = []
    for m in re.finditer(r'\{\s*([A-Za-z_][\w]*(?:\(\s*0x[0-9a-fA-F]+\s*\))?)\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*\}', body):
        key, value = m.group(1), int(m.group(2), 0)
        c = re.fullmatch(r'CCI_REG(8|16|24)\(\s*(0x[0-9a-fA-F]+)\s*\)', key)
        if c:
            bits, addr = int(c.group(1)), int(c.group(2), 16)
        elif key in names:
            bits, addr = names[key]
        else:
            raise Bad(f'unknown register name {key}')
        out.append(('w', bits, addr, value))
    # Every non-blank, non-comment line of the body must have been an entry.
    stripped = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    lines = [ln.strip() for ln in stripped.split('\n') if ln.strip() and not ln.strip().startswith('//')]
    if len(lines) != len(out):
        raise Bad(f'{len(lines)} lines but {len(out)} entries')
    return out


def mode_params(src, array, fields, tables):
    m = re.search(r'static\s+const\s+struct\s+\w+\s+' + re.escape(array) + r'\s*\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
    if not m:
        raise Bad(f'no mode array {array}')
    body = m.group(1)
    modes = {}
    # Each mode is a brace group at depth 1 of the array body.
    depth, start = 0, None
    for i, ch in enumerate(body):
        if ch == '{':
            depth += 1
            if depth == 1:
                start = i
        elif ch == '}':
            depth -= 1
            if depth == 0 and start is not None:
                group = body[start:i + 1]
                params = {}
                for f in fields:
                    fm = re.search(r'\.' + f + r'\s*=\s*(\d+)\s*,', group)
                    if fm:
                        params[f] = int(fm.group(1))
                rm = re.search(r'\.regs\s*=\s*(\w+)', group)
                if not rm or rm.group(1) not in tables:
                    raise Bad(f'{array}: a mode without a known register list')
                if set(params) != set(fields):
                    raise Bad(f'{array}: mode {tables[rm.group(1)]} lacks {sorted(set(fields) - set(params))}')
                modes[tables[rm.group(1)]] = params
                start = None
    return modes


def extract_sensor(name, src_text, src_sha):
    cfg = SENSORS[name]
    names = defines(src_text)
    arrays = dict(reg_arrays(src_text))
    if set(arrays) != set(cfg['arrays']):
        raise Bad(f'{name}: arrays {sorted(arrays)} differ from the expected {sorted(cfg["arrays"])}')
    modes = mode_params(src_text, cfg['modes'][0], cfg['modes'][1], cfg['mode_tables'])
    sections = []
    if cfg.get('start'):
        sections.append(('start', 'start', {}, cfg['start']))
    for array, (kind, sec) in cfg['arrays'].items():
        if kind == 'init':
            sections.append((kind, sec, {}, entries(arrays[array], names)))
    for array, (kind, sec) in cfg['arrays'].items():
        if kind == 'mode':
            sections.append((kind, sec, modes[sec], entries(arrays[array], names)))
    for kind in ('on', 'off'):
        for array, (k, sec) in cfg['arrays'].items():
            if k == kind:
                sections.append((kind, sec, {}, entries(arrays[array], names)))
        if cfg.get(kind):
            sections.append((kind, kind, {}, cfg[kind]))
    return render(name, cfg['file'], src_sha, sections)


def render(name, path, src_sha, sections):
    out = [f'# piano camera sensor register table: {name}. Hardware data (addresses, values, mode timing), no code.',
           '# Written by tooling/camera/sensor-tables.py extract; provenance and format: data/camera/README.md. Do not edit by hand.',
           f'sensor {name}', f'source {path} {src_sha}']
    for kind, sec, params, items in sections:
        out.append(f'[{kind} {sec}]')
        for k, v in params.items():
            out.append(f'p {k} {v}')
        for it in items:
            if it[0] == 'w':
                digits = {8: 2, 16: 4, 24: 6}[it[1]]
                out.append(f'w {it[1]} 0x{it[2]:04x} 0x{it[3]:0{digits}x}')
            else:
                out.append(f'd {it[1]} {it[2]}')
    return '\n'.join(out) + '\n'


def cmd_extract(src, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    for name, cfg in SENSORS.items():
        raw = (src / cfg['file']).read_bytes()
        text = extract_sensor(name, raw.decode(), sha256(raw))
        (outdir / f'{name}.tbl').write_text(text)
        print(f'{outdir / (name + ".tbl")}: {summary(parse(text))}')


# ----------------------------------------------------------------------------------------------------------------- parsing
def parse(text):
    t = {'sensor': None, 'source': None, 'sections': []}
    cur = None
    for n, raw in enumerate(text.split('\n'), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        f = line.split()
        if t['sensor'] is None:
            if f[0] != 'sensor' or len(f) != 2:
                raise Bad(f'line {n}: the first item must be "sensor NAME"')
            t['sensor'] = f[1]
            continue
        if f[0] == 'source':
            if len(f) != 3 or not re.fullmatch(r'[0-9a-f]{64}', f[2]):
                raise Bad(f'line {n}: "source PATH SHA256"')
            t['source'] = (f[1], f[2])
        elif line.startswith('['):
            m = re.fullmatch(r'\[(\w+) ([\w-]+)\]', line)
            if not m or m.group(1) not in KINDS:
                raise Bad(f'line {n}: bad section {line}')
            kind, name = m.groups()
            if kind == 'mode' and not re.fullmatch(r'\d+x\d+', name):
                raise Bad(f'line {n}: a mode is named WxH')
            if kind != 'mode' and name != kind:
                raise Bad(f'line {n}: section {kind} must be named {kind}')
            if any(s['kind'] == kind and s['name'] == name for s in t['sections']):
                raise Bad(f'line {n}: section {kind} {name} twice')
            cur = {'kind': kind, 'name': name, 'params': {}, 'items': []}
            t['sections'].append(cur)
        elif f[0] == 'p':
            if cur is None or cur['kind'] != 'mode' or len(f) != 3 or not f[2].isdigit() or cur['items']:
                raise Bad(f'line {n}: a parameter belongs at the top of a mode section: "p KEY DECIMAL"')
            cur['params'][f[1]] = int(f[2])
        elif f[0] == 'w':
            if cur is None or len(f) != 4 or f[1] not in ('8', '16', '24'):
                raise Bad(f'line {n}: "w 8|16|24 ADDR VALUE" inside a section')
            bits, addr, value = int(f[1]), int(f[2], 16), int(f[3], 16)
            if not f[2].startswith('0x') or not f[3].startswith('0x') or addr > 0xffff or value >= 1 << bits:
                raise Bad(f'line {n}: address or value out of range')
            cur['items'].append(('w', bits, addr, value))
        elif f[0] == 'd':
            if cur is None or len(f) != 3 or not (f[1].isdigit() and f[2].isdigit()) or int(f[1]) > int(f[2]) or int(f[2]) > 100000:
                raise Bad(f'line {n}: "d MIN_US MAX_US" (at most 100 ms)')
            cur['items'].append(('d', int(f[1]), int(f[2])))
        else:
            raise Bad(f'line {n}: unknown item {f[0]}')
    if t['sensor'] is None or t['source'] is None:
        raise Bad('no sensor or source line')
    kinds = [s['kind'] for s in t['sections']]
    for need in ('init', 'mode', 'on', 'off'):
        if need not in kinds:
            raise Bad(f'no {need} section')
    order = [KINDS.index(k) for k in kinds]
    if order != sorted(order):
        raise Bad('sections out of order (start, init, modes, on, off)')
    for s in t['sections']:
        if not s['items'] or s['items'][0][0] != 'w':
            raise Bad(f'section {s["kind"]} {s["name"]} is empty or starts with a delay')
        if s['kind'] == 'mode':
            w, h = (int(x) for x in s['name'].split('x'))
            if s['params'].get('width') != w or s['params'].get('height') != h:
                raise Bad(f'mode {s["name"]}: its width/height parameters disagree with its name')
    return t


def summary(t):
    return ', '.join(f'{s["kind"]} {s["name"]}: {sum(1 for i in s["items"] if i[0] == "w")}'
                     if s['kind'] == 'mode' else f'{s["kind"]}: {sum(1 for i in s["items"] if i[0] == "w")}'
                     for s in t['sections'])


def manifest_lines(d):
    lines = []
    for p in sorted(d.glob('*.tbl')):
        raw = p.read_bytes()
        t = parse(raw.decode())
        counts = ' '.join(f'{s["kind"]}:{s["name"]}={sum(1 for i in s["items"] if i[0] == "w")}' for s in t['sections'])
        lines.append(f'{p.name} {sha256(raw)} {counts}')
    return lines


def cmd_check(d, src=None, quiet=False):
    man = d / 'MANIFEST'
    if not man.is_file():
        raise Bad(f'{man}: missing (put the tables at {d}: tooling/camera/fetch-ubuntu-camera-sources.sh, then extract)')
    want = [ln for ln in man.read_text().split('\n') if ln.strip() and not ln.startswith('#')]
    files = sorted(p.name for p in d.glob('*.tbl'))
    if sorted(ln.split()[0] for ln in want) != files:
        raise Bad(f'{d}: tables {files} do not match MANIFEST {sorted(ln.split()[0] for ln in want)}')
    got = manifest_lines(d)
    for w, g in zip(sorted(want), got):
        if w != g:
            raise Bad(f'MANIFEST mismatch:\n  pinned  {w}\n  present {g}')
    for p in sorted(d.glob('*.tbl')):
        t = parse(p.read_text())
        if t['sensor'] != p.stem or t['sensor'] not in SENSORS:
            raise Bad(f'{p.name}: sensor {t["sensor"]} is not this file\'s')
        if t['source'][0] != SENSORS[t['sensor']]['file']:
            raise Bad(f'{p.name}: source {t["source"][0]} is not {SENSORS[t["sensor"]]["file"]}')
        if src is not None:
            raw = (src / t['source'][0]).read_bytes()
            if sha256(raw) != t['source'][1]:
                raise Bad(f'{p.name}: {src / t["source"][0]} is not the pinned file (SHA-256 {sha256(raw)}, pinned {t["source"][1]})')
            if extract_sensor(t['sensor'], raw.decode(), sha256(raw)) != p.read_text():
                raise Bad(f'{p.name}: does not equal a fresh extraction from {src}')
        if not quiet:
            print(f'{p.name}: ok ({summary(t)})')


# ------------------------------------------------------------------------------------------------------------------ header
def cmd_header(d, out):
    tables = [parse(p.read_text()) for p in sorted(d.glob('*.tbl'))]
    words, index, modes = [], [], []
    for t in tables:
        for s in t['sections']:
            first = len(words)
            for it in s['items']:
                if it[0] == 'w':
                    # Two words: (bytes << 30 | address), bytes 1..3 (bits 29:16 zero), then the value (up to 24 bits).
                    words.append((it[1] // 8) << 30 | it[2])
                    words.append(it[3])
                else:
                    words.append(0xffff0000 | min(it[1] // 100, 0xffff))   # a delay: 0xffff << 16 | min in 100 us units
                    words.append(it[2])
            index.append((t['sensor'], s['kind'], s['name'], first, len(words) - first))
            if s['kind'] == 'mode':
                modes.append((t['sensor'], s['name'], s['params']))
    lines = ['// Generated by tooling/camera/sensor-tables.py header from data/camera (provenance: data/camera/README.md). Do not edit.',
             '#pragma once', '#include <stdint.h>', '',
             '// A write is two words: (bytes << 30 | register address) and the value; a delay is (0xffff << 16 | min / 100 us) and max us.',
             f'static const uint32_t kCameraTableWords[{len(words)}] = {{']
    for i in range(0, len(words), 8):
        lines.append('  ' + ', '.join(f'0x{w:08x}' for w in words[i:i + 8]) + ',')
    lines += ['};', '', 'struct CameraTableSection {', '  const char sensor[8], kind[8], name[12];', '  uint32_t first, words;', '};',
              f'static const CameraTableSection kCameraTableSections[{len(index)}] = {{']
    for sensor, kind, name, first, n in index:
        lines.append(f'  {{"{sensor}", "{kind}", "{name}", {first}, {n}}},')
    lines += ['};', '', 'struct CameraTableMode {', '  const char sensor[8], name[12];', '  uint32_t width, height, a, b, c, d;', '};',
              '// a..d: s5kjn1 hts, vts, exposure, exposure_margin; ov32d40 line_length, frame_length, sensor_frame_length, 0.',
              f'static const CameraTableMode kCameraTableModes[{len(modes)}] = {{']
    for sensor, name, p in modes:
        rest = [v for k, v in p.items() if k not in ('width', 'height')] + [0, 0, 0, 0]
        lines.append(f'  {{"{sensor}", "{name}", {p["width"]}, {p["height"]}, {rest[0]}, {rest[1]}, {rest[2]}, {rest[3]}}},')
    lines += ['};', '']
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('\n'.join(lines))


# --------------------------------------------------------------------------------------------------------------- self-test
SYNTH_S5K = r'''
#define S5KJN1_REG_VTS		CCI_REG16(0x0340)
static const struct cci_reg_sequence init_array_setting[] = {
	{ CCI_REG16(0x6028), 0x2400 },
	{ CCI_REG16(0x602a), 0x1354 },
};
static const struct cci_reg_sequence s5kjn1_4080x3072_30fps_mode[] = {
	{ S5KJN1_REG_VTS,    0x10c0 },

	{ CCI_REG8(0x0100), 0x00 },
};
static const struct cci_reg_sequence s5kjn1_8160x6144_10fps_mode[] = {
	{ CCI_REG16(0x0342), 0x21f0 },
};
static const struct s5kjn1_mode s5kjn1_supported_modes[] = {
	{
		.width = 4080, .height = 3072, .hts = 4352, .vts = 4288, .exposure = 3840, .exposure_margin = 22,
		.reg_list = { .regs = s5kjn1_4080x3072_30fps_mode, .num_regs = 2, },
	},
	{
		.width = 8160, .height = 6144, .hts = 8688, .vts = 6400, .exposure = 6144, .exposure_margin = 44,
		.reg_list = { .regs = s5kjn1_8160x6144_10fps_mode, .num_regs = 1, },
	},
};
'''


def self_test():
    text = extract_sensor('s5kjn1', SYNTH_S5K, '0' * 64)
    t = parse(text)
    kinds = [(s['kind'], s['name']) for s in t['sections']]
    assert kinds == [('start', 'start'), ('init', 'init'), ('mode', '4080x3072'), ('mode', '8160x6144'), ('on', 'on'),
                     ('off', 'off')], kinds
    m = t['sections'][2]
    assert m['params'] == {'width': 4080, 'height': 3072, 'hts': 4352, 'vts': 4288, 'exposure': 3840, 'exposure_margin': 22}
    assert m['items'] == [('w', 16, 0x340, 0x10c0), ('w', 8, 0x100, 0)], m['items']
    for bad, why in [(SYNTH_S5K.replace('{ CCI_REG16(0x602a), 0x1354 },', '{ CCI_REG16(0x602a) 0x1354 },'), 'a malformed entry'),
                     (SYNTH_S5K.replace('S5KJN1_REG_VTS,', 'S5KJN1_REG_HTS,'), 'an unknown register name'),
                     (SYNTH_S5K.replace('.vts = 4288, ', ''), 'a mode parameter missing')]:
        try:
            extract_sensor('s5kjn1', bad, '0' * 64)
        except Bad:
            continue
        raise AssertionError(f'{why} was accepted')
    for bad in [text.replace('w 16 0x6028 0x2400', 'w 16 0x6028 0x12400'), text.replace('[on on]', '[on stream]'),
                text.replace('p width 4080', 'p width 4096'), text.replace('d 5000 6000', 'd 6000 5000'),
                text.replace('[off off]\nw 8 0x0100 0x00\n', '')]:
        try:
            parse(bad)
        except Bad:
            continue
        raise AssertionError('a malformed table was accepted')
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / 's5kjn1.tbl').write_text(text)
        try:
            cmd_check(d)
            raise AssertionError('a directory without MANIFEST was accepted')
        except Bad:
            pass
        (d / 'MANIFEST').write_text('\n'.join(manifest_lines(d)) + '\n')
        try:
            cmd_check(d, quiet=True)   # the synthetic source line names the right file, so this passes
        except Bad as e:
            raise AssertionError(f'a consistent directory was refused: {e}')
        (d / 's5kjn1.tbl').write_text(text.replace('0x1354', '0x1355'))
        try:
            cmd_check(d)
            raise AssertionError('a changed table passed its MANIFEST')
        except Bad:
            pass
        (d / 's5kjn1.tbl').write_text(text)
        cmd_header(d, d / 'CameraTables.h')
        h = (d / 'CameraTables.h').read_text()
        assert '{"s5kjn1", "mode", "4080x3072", ' in h and '0x80000340, 0x000010c0' in h and '0x40000100, 0x00000000' in h, h
    print('sensor-tables.py: self-test passed')


def main(argv):
    try:
        if argv[1:] == ['--self-test']:
            self_test()
        elif len(argv) == 4 and argv[1] == 'extract':
            cmd_extract(Path(argv[2]), Path(argv[3]))
        elif len(argv) in (3, 5) and argv[1] == 'check':
            src = None
            if len(argv) == 5:
                if argv[3] != '--src':
                    raise Bad('check DIR [--src SRC]')
                src = Path(argv[4])
            cmd_check(Path(argv[2]), src)
        elif len(argv) == 4 and argv[1] == 'header':
            cmd_header(Path(argv[2]), Path(argv[3]))
        elif len(argv) == 3 and argv[1] == 'manifest':
            print("# The tables PianoCamera accepts: file, SHA-256, every section's write count (tooling/camera/sensor-tables.py check).")
            print('# Regenerated only after a deliberate re-extraction: sensor-tables.py manifest data/camera > data/camera/MANIFEST')
            print('\n'.join(manifest_lines(Path(argv[2]))))
        else:
            print(__doc__, file=sys.stderr)
            return 2
    except Bad as e:
        print(f'sensor-tables.py: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
