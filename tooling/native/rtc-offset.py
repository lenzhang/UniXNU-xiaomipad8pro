#!/usr/bin/env python3
"""The PMIC RTC's offset for piano-rtc-offset=N (docs/drivers/rtc.md): UTC = the RTC's count + N, measured against the Mac's clock.

  rtc-offset.py [TIMES ...]        usb-console.py's <time>-port0.times.txt (default: the newest in logs/usb-console): every
                                   "rtc: COUNT (" line of PianoPower's report, paired with the Mac's time T when it arrived
  rtc-offset.py --pair COUNT UTC   one pair by hand; UTC as seconds since 1970 or 2026-11-11T08:00:00Z

The count is whole seconds and a line reaches the Mac after the count was read, so every T - COUNT is at least the true offset E
(the UTC at which the count was 0) and below E + 1 + that line's delay. The smallest T - COUNT over the lines is the estimate (a
line held back in a buffer only makes its own value larger); + 0.5 centres the count's whole second; rounded. With PianoPower's
30 s report (piano-power-period=5 for a quicker measurement) a few minutes of lines give it to within about a second. The Mac's
clock must be network time (System Settings > General > Date & Time, set automatically).

Prints N, the spread of T - COUNT, what the SDAM cell said on those lines, and the build-esp.py argument.
"""
import argparse
import calendar
import math
import re
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
# PianoPower (package/PianoPower/PianoPower.cpp readRtcLine): "rtc: COUNT (DATE UTC, offset N from SOURCE), enabled E", also with
# "[Ns] " before it, and "rtc: COUNT (+ offset N from SOURCE = ..." or "rtc: COUNT (no offset: ..." when there is no date.
LINE = re.compile(r'PianoPower: (?:\[\d+s\] )?rtc: (\d+) \((.*?)\), enabled ([01])')
OFFSET = re.compile(r'offset (\d+) from (piano-rtc-offset|the SDAM cell)')
UTC_MIN, UTC_MAX = 1767225600, 4102444800   # PmicRtc.h: a date in [2026-01-01, 2100-01-01)


def parse(lines):
    """[(T, count, offset or None, source or None, enabled)] from the lines of a .times.txt file (T, a tab, the console line)."""
    out = []
    for raw in lines:
        stamp, sep, text = raw.rstrip('\n').partition('\t')
        if not sep:
            continue
        m = LINE.search(text)
        if not m:
            continue
        try:
            t = float(stamp)
        except ValueError:
            continue
        o = OFFSET.search(m.group(2))
        out.append((t, int(m.group(1)), int(o.group(1)) if o else None, o.group(2) if o else None, m.group(3) == '1'))
    return out


def estimate(samples):
    """N from [(T, count)]: min(T - count) + 0.5, rounded half up; with the min and max of T - count."""
    d = [t - c for t, c in samples]
    lo, hi = min(d), max(d)
    return math.floor((lo + 0.5) + 0.5), lo, hi


def as_u32(n):
    """The boot argument's value: UTC - count modulo 2^32, as the kernel adds it (PmicRtc.h piano_rtc_utc)."""
    if not -(1 << 31) <= n < 1 << 32:
        raise SystemExit(f'offset {n} does not fit in 32 bits')
    return n % (1 << 32)


def utc_of(text):
    if re.fullmatch(r'\d+', text):
        return int(text)
    return calendar.timegm(time.strptime(text.rstrip('Z'), '%Y-%m-%dT%H:%M:%S'))


def date(secs):
    return time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(secs))


def report(n, how):
    v = as_u32(n)
    print(f'piano-rtc-offset={v}  ({how})')
    print(f'  tooling/persist/build-esp.py --profile daily --rtc-offset {v}   (adds piano-rtc=1 piano-rtc-offset={v})')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('times', nargs='*', type=Path, help='usb-console.py .times.txt files')
    p.add_argument('--pair', nargs=2, metavar=('COUNT', 'UTC'), help='one count and the UTC it was read at')
    a = p.parse_args()
    if a.pair:
        count, utc = int(a.pair[0]), utc_of(a.pair[1])
        report(utc - count, f'{date(utc)} - count {count}')
        return 0
    files = a.times or sorted((REPO / 'logs/usb-console').glob('*-port0.times.txt'))[-1:]
    if not files:
        raise SystemExit('no .times.txt capture: run tooling/native/usb-console.py --capture 300 while PianoPower reports')
    samples = []
    for f in files:
        samples += parse(f.read_text(errors='replace').splitlines())
    if not samples:
        raise SystemExit(f'no "PianoPower: rtc: COUNT" lines in {", ".join(map(str, files))} (piano-power=1 in the boot arguments?)')
    stopped = sum(1 for s in samples if not s[4])
    if stopped:
        print(f'WARNING: {stopped} of {len(samples)} lines say "enabled 0": the counter is stopped, no offset makes it a clock')
    n, lo, hi = estimate([(t, c) for t, c, _, _, _ in samples])
    print(f'{len(samples)} rtc lines in {", ".join(f.name for f in files)}: Mac time - count from {lo:.3f} to {hi:.3f} '
          f'(spread {hi - lo:.3f} s: the lines\' delays and the count\'s whole seconds)')
    if len(samples) < 3:
        print('WARNING: fewer than 3 lines; the estimate can be up to the first line\'s delay late')
    _, c, off, source, _ = samples[-1]
    if not UTC_MIN <= (c + n) % (1 << 32) < UTC_MAX:
        print('WARNING: count + offset is outside 2026-2100, which the kernel refuses as a date: is the Mac\'s clock right?')
    if off is not None:
        delta = (off - as_u32(n) + (1 << 31)) % (1 << 32) - (1 << 31)
        print(f'last line: count {c}, the kext used offset {off} from {source} ({date((c + off) % (1 << 32))}): '
              f'{delta:+d} s from this measurement')
    report(n, f'smallest Mac time - count + 0.5 s, rounded; the last count reads {date((c + n) % (1 << 32))} with it')
    return 0


if __name__ == '__main__':
    sys.exit(main())
