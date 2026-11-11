#!/usr/bin/env python3
"""The boot's milestones from a native run's console (docs/boot-time.md), with their time from XNU's start when the log has times.

  boot-timeline.py logs/usb-console/<time>-port0.times.txt [--xnu-start HH:MM:SS | --loop logs/native-runs/<ts>-loop/loop.log]
  boot-timeline.py logs/native-runs/<ts>-loop/xnu-console.txt          (order only; launchd's 1970 stamps where -v printed them)
  ... --execs                                                          (every exec and exit too)

T (seconds from XNU's start) comes from usb-console.py's .times.txt (the Mac's clock when each line arrived), anchored on the run's
own `piano-reset-after: N s reached` line (printed at mach time N, which counts from XNU's start), else on the loop's "XNU started"
time (loop.log, local HH:MM:SS, +-1 s). Lines from before the Mac opened the port arrive in one burst when it does (PianoDwc3 replays
the console mailbox from its start): their T is only an upper bound, marked '<='. Every reconnect replays the mailbox again, so only
the first arrival of a line counts. A log without times (the decoded mailbox, a .txt capture) gives the order and line numbers only;
launchd's own lines carry the 1970 calendar, which starts at XNU's uptime (no RTC), so with -v they read as T directly.
The decoded mailbox has DRAM bit damage: names can be garbled, so a missing milestone there is not proof of absence.
"""
import argparse
import datetime
import re
import sys
from pathlib import Path

MILESTONES = [
    ('kernel', r'Kernel boot args:'),
    ('store', r'NativeRamBlock: Data store state \d'),
    ('root device', r'BSD root: '),
    ('launchd exec', r'exec: pid 1 launchd\b'),
    ('launchd hello (-v only)', r'hello, launchd'),
    ('early boot complete (-v only)', r'Early boot complete'),
    ('GPU up', r'PianoA830: bring-up done'),
    ('logind', r'exec: pid \d+ logind\b'),
    ('WindowServer', r'exec: pid \d+ WindowServer\b'),
    ('WindowServer Metal device', r'NativeAdrenoMetal: WindowServer: device '),
    ('loginwindow', r'exec: pid \d+ loginwindow\b'),
    ('first frame (console released)', r'NativeGopDisplay: first frame'),
    ('authd', r'exec: pid \d+ authd\b'),
    ('settime (boot job)', r'settime: '),
    ('SecurityAgent', r'exec: pid \d+ SecurityAgent\b'),
    ('VTDecoderXPCService (wallpaper video)', r'exec: pid \d+ VTDecoderXPCServ'),
    ('login screen (lit frame)', r'NativeGopDisplay: .*(?:last frame|thumb frame \d+[^,]*,).* (?:[5-9]\d|100)\.\d% non-black'),
    ('reset', r'piano-reset-after: \d+ s reached'),
]
COUNTS = [
    ('crashes (core dump denied)', r'Denying core dump for pid \d+ \(([^)]+)\)'),
    ('WindowServer exits', r'exit: pid \d+ WindowServer\b'),
    ('watchdogd execs', r'exec: pid \d+ watchdogd\b'),
    ('ReportCrash execs', r'exec: pid \d+ ReportCrash'),
    ('xpcproxy exit 78 (a denied or failed spawn)', r'exit: pid \d+ xpcproxy status 0x4e00'),
]
CAL = re.compile(r'1970-01-01 07:(\d\d):(\d\d)\.(\d+)')   # launchd's stamps in Asia/Singapore's 1970 zone (+7:30)
RESET = re.compile(r'piano-reset-after: (\d+) s reached')


def read(path):
    rows = []   # (arrival or None, text)
    for raw in Path(path).read_bytes().decode('utf-8', 'replace').split('\n'):
        stamp, tab, text = raw.partition('\t')
        if tab and re.fullmatch(r'\d+\.\d{3}', stamp):
            rows.append((float(stamp), text))
        else:
            rows.append((None, raw))
    return rows


def xnu_start(rows, a):
    for t, text in rows:
        m = RESET.search(text)
        if t is not None and m:
            return t - int(m.group(1)), 'the reset line'
    loop = a.xnu_start
    if a.loop:
        m = re.search(r'^(\d\d:\d\d:\d\d) XNU started', Path(a.loop).read_text(), re.M)
        loop = m and m.group(1)
    if loop and rows and rows[0][0] is not None:
        first = datetime.datetime.fromtimestamp(rows[0][0])
        hh, mm, ss = map(int, loop.split(':'))
        start = first.replace(hour=hh, minute=mm, second=ss, microsecond=0)
        if start.timestamp() > rows[0][0] + 600:
            start -= datetime.timedelta(days=1)
        return start.timestamp(), 'loop.log "XNU started"'
    return None, None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('log', type=Path)
    p.add_argument('--xnu-start', metavar='HH:MM:SS', help="the loop's local time of \"XNU started\"")
    p.add_argument('--loop', type=Path, help='the run\'s loop.log (its "XNU started" line)')
    p.add_argument('--execs', action='store_true', help='list every exec and exit')
    a = p.parse_args()
    rows = read(a.log)
    timed = any(t is not None for t, _ in rows)
    t0, source = xnu_start(rows, a) if timed else (None, None)
    burst = next((t for t, _ in rows if t is not None), None)

    def when(t, text):
        cal = CAL.search(text)
        if t0 is not None and t is not None:
            mark = '<=' if burst is not None and t - burst < 2 else '  '
            return f'{mark}{t - t0:7.1f}'
        if cal:
            return f'~{(int(cal.group(1)) - 30) * 60 + int(cal.group(2)) + float("0." + cal.group(3)):7.1f}'
        return ' ' * 9

    print(f'{a.log}: {len(rows)} lines, ' + (f'T from {source}' if t0 is not None else
          'no times (order and line numbers only; ~T: launchd\'s calendar)'))
    seen = {}
    for i, (t, text) in enumerate(rows):
        for label, rx in MILESTONES:
            if label not in seen and re.search(rx, text):
                seen[label] = (i, t, text)
    for label, rx in MILESTONES:
        if label in seen:
            i, t, text = seen[label]
            print(f'{when(t, text)}  line {i + 1:6d}  {label:38s} {text.strip()[:110]}')
        else:
            print(f'{"":9s}  {"-":>11s}  {label:38s} (not in the log)')
    print('counts (first arrival of each line):')
    first = {}
    for t, text in rows:
        first.setdefault(text, t)
    for label, rx in COUNTS:
        hits = [m for text in first if (m := re.search(rx, text))]
        detail = ''
        if hits and hits[0].groups():
            names = {}
            for m in hits:
                names[m.group(1)] = names.get(m.group(1), 0) + 1
            detail = ' (' + ', '.join(f'{n} {c}' for n, c in sorted(names.items(), key=lambda x: -x[1])) + ')'
        print(f'  {label}: {len(hits)}{detail}')
    samples = [(t, text) for text, t in first.items() if re.search(r'pc-sample: \d+ idle, \d+ user, \d+ kernel', text)]
    if samples:
        print('pc-sample periods (idle/user/kernel samples):')
        for t, text in sorted(samples, key=lambda x: (x[0] is None, x[0] or 0)):
            print(f'{when(t, text)}  {text.strip()[:100]}')
    if a.execs:
        print('execs and exits:')
        for i, (t, text) in enumerate(rows):
            if re.search(r'\b(exec|exit): pid \d+ ', text) and first.get(text) == t:
                print(f'{when(t, text)}  line {i + 1:6d}  {text.strip()[:110]}')


if __name__ == '__main__':
    sys.exit(main())
