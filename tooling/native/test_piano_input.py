#!/usr/bin/env python3
"""Tests of tooling/native/piano-input.py: the US layout, key names, pointer splitting, the protocol lines (each one also parsed by the
kernel's own parser, package/PianoDwc3/HidInput.h, through tests/hid_input_host when a C++ compiler is there), and a run against a
fake tablet on a pseudo-terminal that answers like PianoDwc3's HID thread. Typed text must never reach the output.

    python3 tooling/native/test_piano_input.py
"""
import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import tty
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
spec = importlib.util.spec_from_file_location('piano_input', HERE / 'piano-input.py')
pi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pi)

HEX = r'[0-9a-f]+'
PROTOCOL = re.compile(rf'^(K {HEX}( {HEX}){{0,6}}|C {HEX}|P [0-7] -?\d+ -?\d+( -?\d+( -?\d+)?)?|R|\?)$')


def valid(line):
    if not PROTOCOL.match(line):
        return False
    f = line.split()
    if f[0] == 'K':
        return all(int(x, 16) <= 0xff for x in f[1:])
    if f[0] == 'C':
        return int(f[1], 16) <= 0x3ff
    if f[0] == 'P':
        v = [int(x) for x in f[2:]]
        return all(-32767 <= x <= 32767 for x in v[:2]) and all(-127 <= x <= 127 for x in v[2:])
    return True


class Layout(unittest.TestCase):
    def test_every_printable_ascii_has_a_key(self):
        for code in range(0x20, 0x7f):
            self.assertIn(chr(code), pi.US, repr(chr(code)))

    def test_shifted_symbols(self):
        cases = {'a': (0x04, False), 'Z': (0x1d, True), '1': (0x1e, False), '!': (0x1e, True), '0': (0x27, False), ')': (0x27, True),
                 '?': (0x38, True), '/': (0x38, False), '"': (0x34, True), '_': (0x2d, True), '|': (0x31, True), '~': (0x35, True),
                 ' ': (0x2c, False), '@': (0x1f, True), '{': (0x2f, True), '>': (0x37, True)}
        for ch, want in cases.items():
            self.assertEqual(pi.US[ch], want, ch)

    def test_text_reports(self):
        self.assertEqual(pi.text_lines('aB'), ['K 0 4', 'K 0', 'K 2', 'K 2 5', 'K 2', 'K 0'])
        self.assertEqual(pi.text_lines(''), [])

    def test_unsupported_characters_are_not_named(self):
        with self.assertRaises(pi.InputError) as e:
            pi.text_lines('pässwörd')
        self.assertNotIn('ä', str(e.exception))
        self.assertNotIn('pässwörd', str(e.exception))
        self.assertIn('2 of 8', str(e.exception))


class Keys(unittest.TestCase):
    def test_names_and_combinations(self):
        self.assertEqual(pi.key_lines('return'), ['K 0 28', 'K 0'])
        self.assertEqual(pi.key_lines('cmd+space'), ['K 8', 'K 8 2c', 'K 8', 'K 0'])
        self.assertEqual(pi.key_lines('shift+tab'), ['K 2', 'K 2 2b', 'K 2', 'K 0'])
        self.assertEqual(pi.key_lines('ctrl+alt+f2'), ['K 5', 'K 5 3b', 'K 5', 'K 0'])
        self.assertEqual(pi.key_lines('cmd+shift+3'), ['K a', 'K a 20', 'K a', 'K 0'])
        self.assertEqual(pi.key_lines('?'), ['K 2', 'K 2 38', 'K 2', 'K 0'])
        self.assertEqual(pi.key_lines('+'), ['K 2', 'K 2 2e', 'K 2', 'K 0'])
        self.assertEqual(pi.key_lines('cmd++'), ['K a', 'K a 2e', 'K a', 'K 0'])
        self.assertEqual(pi.key_lines('Up'), ['K 0 52', 'K 0'])
        self.assertEqual(pi.key_lines('volumeup'), ['C e9', 'C 0'])
        self.assertEqual(pi.key_lines('shift'), ['K 2', 'K 0'])
        for bad in ('hyper+a', 'nosuchkey', 'cmd+volumeup'):
            with self.assertRaises(pi.InputError):
                pi.key_lines(bad)


class Pointer(unittest.TestCase):
    def test_split(self):
        self.assertEqual(pi.split(70000, 32767), [32767, 32767, 4466])
        self.assertEqual(pi.split(-300, 127), [-127, -127, -46])
        self.assertEqual(pi.split(0, 127), [])

    def test_move(self):
        self.assertEqual(pi.move_lines(10, -5), ['P 0 10 -5'])
        lines = pi.move_lines(100, 7, steps=4)
        self.assertEqual(len(lines), 4)
        self.assertEqual(sum(int(l.split()[2]) for l in lines), 100)
        self.assertEqual(sum(int(l.split()[3]) for l in lines), 7)
        big = pi.move_lines(-70000, 10)
        self.assertEqual(sum(int(l.split()[2]) for l in big), -70000)
        self.assertEqual(pi.move_lines(0, 0), ['P 0 0 0'])

    def test_click_scroll_drag(self):
        self.assertEqual(pi.click_lines('right'), ['P 2 0 0', 'P 0 0 0'])
        self.assertEqual(len(pi.click_lines('left', double=True)), 4)
        self.assertEqual(pi.scroll_lines(3), ['P 0 0 0 3 0'])
        self.assertEqual(pi.scroll_lines(-200, 5), ['P 0 0 0 -127 5', 'P 0 0 0 -73 0'])
        drag = pi.drag_lines(30, 0, steps=3)
        self.assertEqual(drag[0], 'P 1 0 0')
        self.assertEqual(drag[-1], 'P 0 0 0')
        self.assertTrue(all(l.startswith('P 1 ') for l in drag[1:-1]))
        with self.assertRaises(pi.InputError):
            pi.click_lines('fourth')


def all_kinds():
    lines = pi.text_lines(''.join(chr(c) for c in range(0x20, 0x7f)) + '\n\t')
    for name in list(pi.KEYS) + list(pi.CONSUMER) + ['cmd+space', 'ctrl+alt+rshift+rcmd+delete', 'shift']:
        lines += pi.key_lines(name)
    lines += pi.move_lines(-70000, 70000, steps=3) + pi.click_lines('middle', True) + pi.scroll_lines(-1000, 1000) + pi.drag_lines(5, 5)
    return lines + ['R', '?']


class Protocol(unittest.TestCase):
    def test_every_line_is_in_the_protocol(self):
        for line in all_kinds():
            self.assertTrue(valid(line), line)

    def test_the_kernel_parser_takes_every_line(self):
        cxx = shutil.which('c++')
        if not cxx:
            self.skipTest('no C++ compiler')
        with tempfile.TemporaryDirectory() as tmp:
            binary, lines = Path(tmp) / 'hid_input_host', Path(tmp) / 'lines.txt'
            pkg = REPO / 'package/PianoDwc3'
            subprocess.run([cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-I', str(pkg), '-I',
                            str(REPO / 'package/PianoKeyboard'), str(pkg / 'tests/hid_input_host.cpp'), '-o', str(binary)], check=True)
            lines.write_text('\n'.join(all_kinds()) + '\n')
            result = subprocess.run([str(binary), str(lines)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)


class FakeTablet(threading.Thread):
    """The tablet's end of the input port on a pseudo-terminal: answers each line as PianoDwc3's HID thread does."""

    def __init__(self):
        super().__init__(daemon=True)
        self.master, slave = os.openpty()
        tty.setraw(self.master)
        self.path = os.ttyname(slave)
        self.slave = slave
        self.lines, self.count = [], 0

    def run(self):
        buffer = b''
        while True:
            try:
                data = os.read(self.master, 4096)
            except OSError:
                return
            if not data:
                return
            buffer += data
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                line = line.decode()
                self.lines.append(line)
                if line == '?':
                    answer = f'status keyboard 1 pointer 1 commands {self.count} errors 0'
                elif valid(line):
                    self.count += 1
                    answer = f'ok {self.count}'
                else:
                    answer = 'err verb'
                os.write(self.master, answer.encode() + b'\n')


class EndToEnd(unittest.TestCase):
    def run_main(self, tablet, *args):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = pi.main(['--device', tablet.path, '--delay', '0', *args])
        return code, out.getvalue(), err.getvalue()

    def test_type_key_move_status(self):
        tablet = FakeTablet()
        tablet.start()
        secret = 'Hunter2!'
        code, out, err = self.run_main(tablet, 'type', secret, '--enter')
        self.assertEqual(code, 0, err)
        self.assertNotIn(secret, out + err)
        self.assertIn('typed 8 characters', out)
        self.assertEqual(tablet.lines, pi.text_lines(secret) + pi.press(0, 0x28))
        before = len(tablet.lines)
        self.assertEqual(self.run_main(tablet, 'key', 'cmd+space')[0], 0)
        self.assertEqual(tablet.lines[before:], ['K 8', 'K 8 2c', 'K 8', 'K 0'])
        self.assertEqual(self.run_main(tablet, 'move', '10', '-20')[0], 0)
        self.assertEqual(tablet.lines[-1], 'P 0 10 -20')
        code, out, _ = self.run_main(tablet, 'status')
        self.assertEqual(code, 0)
        self.assertIn('status keyboard 1 pointer 1', out)

    def test_refused_command_releases_and_reports(self):
        tablet = FakeTablet()
        tablet.start()
        code, _, err = self.run_main(tablet, 'raw', 'Z 1')
        self.assertEqual(code, 1)
        self.assertIn('refused', err)
        self.assertEqual(tablet.lines[-1], 'R')   # nothing left pressed

    def test_unmapped_text_sends_nothing(self):
        tablet = FakeTablet()
        tablet.start()
        code, out, err = self.run_main(tablet, 'type', 'naïve')
        self.assertEqual(code, 1)
        self.assertEqual(tablet.lines, [])
        self.assertNotIn('naïve', out + err)


if __name__ == '__main__':
    unittest.main(verbosity=1)
