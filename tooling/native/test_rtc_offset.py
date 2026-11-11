#!/usr/bin/env python3
"""Tests of tooling/native/rtc-offset.py (the PMIC RTC's offset from the Mac's clock) and of usb-console.py's line stamps:
simulated RTC lines with delays and a held-back backlog, the exact line formats PianoPower prints, the 32-bit wrap, --pair.

    python3 tooling/native/test_rtc_offset.py
"""
import importlib.util
import io
import math
import random
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ro = load('rtc_offset', 'rtc-offset.py')
uc = load('usb_console', 'usb-console.py')


def line(t, count, rest='2026-11-11 08:00:00 UTC, offset 1794270000 from the SDAM cell', enabled=1, period=None):
    tag = f'[{period}s] ' if period is not None else ''
    return f'{t:.3f}\tPianoPower: {tag}rtc: {count} ({rest}), enabled {enabled}'


class Estimate(unittest.TestCase):
    def simulate(self, epoch, n, seed, backlog=0):
        """n reads at random phases: count = floor(t_read - epoch), the line arriving 2-300 ms later; backlog lines 40 s late."""
        rng = random.Random(seed)
        out, t = [], 1794300000.0 + rng.random()
        for i in range(n):
            t += 30 + rng.random()
            delay = 40.0 if i < backlog else rng.uniform(0.002, 0.3)
            out.append((t + delay, math.floor(t - epoch)))
        return out

    def test_within_a_second(self):
        for seed in range(50):
            epoch = 1794270000 + random.Random(seed).random() * 1000
            n, lo, hi = ro.estimate(self.simulate(epoch, 12, seed, backlog=2))
            # The kernel sets count + N at a moment whose true UTC is in [count + epoch, count + epoch + 1).
            self.assertLessEqual(abs(n - (epoch + 0.5)), 1.0, (seed, n, epoch))
            self.assertGreaterEqual(lo, epoch)
            self.assertGreater(hi, lo)

    def test_backlog_does_not_pull_it(self):
        samples = self.simulate(1794270000.25, 10, 7, backlog=5)
        n_all = ro.estimate(samples)[0]
        n_live = ro.estimate(samples[5:])[0]
        self.assertEqual(n_all, n_live)

    def test_rounding(self):
        self.assertEqual(ro.estimate([(1000.0, 0)])[0], 1001)    # 1000.0 + 0.5 -> 1000.5 -> 1001 (half up)
        self.assertEqual(ro.estimate([(1000.49, 0)])[0], 1001)
        self.assertEqual(ro.estimate([(999.49, 0)])[0], 1000)

    def test_u32(self):
        self.assertEqual(ro.as_u32(-256), (1 << 32) - 256)   # count ahead of UTC
        self.assertEqual(ro.as_u32(1794270000), 1794270000)
        with self.assertRaises(SystemExit):
            ro.as_u32(1 << 32)


class Parse(unittest.TestCase):
    def test_formats(self):
        text = [
            line(1794355200.120, 85432100),
            line(1794355230.050, 85432130, period=30),
            line(1794355260.0, 85432160, rest='+ offset 0 from the SDAM cell = 85432160, year 1972: not a date, measure piano-rtc-offset'),
            line(1794355290.0, 85432190, rest='no offset: the SDAM cell does not read and there is no piano-rtc-offset', enabled=0),
            line(1794355320.0, 85432220, rest='2026-11-11 08:02:00 UTC, offset 1708923100 from piano-rtc-offset'),
            '1794355321.000\tPianoPower: rtc: not read (EN_CTL read failed)',
            '1794355322.000\tPianoPower: rtc: RTC_RW APID 97 (owner EE 0) type 0xb subtype 0x7 EN_CTL 0x80; SDAM APID 113',
            'no tab here rtc: 5 (x), enabled 1',
        ]
        got = ro.parse(text)
        self.assertEqual([g[1] for g in got], [85432100, 85432130, 85432160, 85432190, 85432220])
        self.assertEqual(got[0][2:], (1794270000, 'the SDAM cell', True))
        self.assertEqual(got[2][2:4], (0, 'the SDAM cell'))
        self.assertEqual(got[3][2:], (None, None, False))
        self.assertEqual(got[4][2:4], (1708923100, 'piano-rtc-offset'))
        self.assertAlmostEqual(got[1][0], 1794355230.050)


class Main(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(HERE / 'rtc-offset.py'), *args], capture_output=True, text=True)

    def test_pair(self):
        r = self.run_tool('--pair', '85432100', '2026-11-11T08:00:00Z')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f'piano-rtc-offset={1794384000 - 85432100}', r.stdout)
        self.assertIn('--rtc-offset', r.stdout)

    def test_file(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'x-port0.times.txt'
            f.write_text('\n'.join(line(1794355200.3 + 30 * i, 85432100 + 30 * i) for i in range(6)) + '\n')
            r = self.run_tool(str(f))
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('6 rtc lines', r.stdout)
            self.assertIn(f'piano-rtc-offset={1794355200 - 85432100 + 1}', r.stdout)   # 0.3 + 0.5 rounds up
            self.assertIn('the kext used offset 1794270000 from the SDAM cell', r.stdout)

    def test_no_lines(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'x-port0.times.txt'
            f.write_text('1.0\thello\n')
            r = self.run_tool(str(f))
            self.assertNotEqual(r.returncode, 0)


class Stamps(unittest.TestCase):
    def test_lines_and_partials(self):
        out = io.StringIO()
        s = uc.Stamper(out)
        s.feed(b'PianoPower: rtc: 12', 100.0)
        s.feed(b'3 (x), enabled 1\r\nsecond\nthi', 101.25)
        s.feed(b'rd\n', 102.5)
        self.assertEqual(out.getvalue(), '101.250\tPianoPower: rtc: 123 (x), enabled 1\n101.250\tsecond\n102.500\tthird\n')
        self.assertEqual([g[1] for g in ro.parse(out.getvalue().splitlines())], [123])


if __name__ == '__main__':
    unittest.main()
