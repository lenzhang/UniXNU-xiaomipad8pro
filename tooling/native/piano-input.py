#!/usr/bin/env python3
"""Keyboard and mouse input from this Mac into native macOS on piano, through PianoDwc3's input port (docs/drivers/usb-device-debug.md
section 12; the tablet boots with piano-usb=2 piano-usb-hid=1).

  piano-input.py type "text" [--enter]      type text on the US layout (shift for capitals and symbols)
  piano-input.py type --secret [--enter]    read the text without echo (a password: not in argv, shell history or ps)
  piano-input.py type --stdin [--enter]     read it from stdin (one trailing newline dropped)
  piano-input.py key return | tab | esc | up | cmd+space | shift+tab | ctrl+alt+f2 | volumeup ... [--repeat N]
  piano-input.py move DX DY [--steps N]     relative pointer motion (positive: right, down)
  piano-input.py click [left|right|middle] [--double]
  piano-input.py scroll DY [DX]             wheel notches (positive: up / right; macOS may invert with natural scrolling)
  piano-input.py drag DX DY                 left button held while moving
  piano-input.py release | status | keys    release everything; the tablet's counters; the key names
  piano-input.py raw "K 0 4"                one protocol line (package/PianoDwc3/HidInput.h)

Typed text is never printed, logged or written anywhere: only counts are. Only the Python standard library; the port is found as
usb-console.py finds the others (the interface "piano input"), --device overrides. Every command waits for the tablet's "ok"
(--no-ack: do not wait; --delay MS between reports, default 8).
"""
import argparse
import getpass
import importlib.util
import os
import select
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = 2

# ------------------------------------------------------------------------------------------------------------ the US layout
MODIFIERS = {'ctrl': 0x01, 'control': 0x01, 'shift': 0x02, 'alt': 0x04, 'option': 0x04, 'opt': 0x04, 'cmd': 0x08, 'command': 0x08,
             'gui': 0x08, 'meta': 0x08, 'rctrl': 0x10, 'rshift': 0x20, 'ralt': 0x40, 'roption': 0x40, 'rcmd': 0x80}
SHIFT = 0x02
US = {}   # character -> (usage, shift)
for i, ch in enumerate('abcdefghijklmnopqrstuvwxyz'):
    US[ch] = (0x04 + i, False)
    US[ch.upper()] = (0x04 + i, True)
for i, (plain, shifted) in enumerate(zip('1234567890', '!@#$%^&*()')):
    US[plain] = (0x1e + i, False)
    US[shifted] = (0x1e + i, True)
for usage, plain, shifted in ((0x2d, '-', '_'), (0x2e, '=', '+'), (0x2f, '[', '{'), (0x30, ']', '}'), (0x31, '\\', '|'),
                              (0x33, ';', ':'), (0x34, "'", '"'), (0x35, '`', '~'), (0x36, ',', '<'), (0x37, '.', '>'), (0x38, '/', '?')):
    US[plain] = (usage, False)
    US[shifted] = (usage, True)
US[' '] = (0x2c, False)
US['\n'] = (0x28, False)
US['\t'] = (0x2b, False)
KEYS = {'return': 0x28, 'enter': 0x28, 'esc': 0x29, 'escape': 0x29, 'backspace': 0x2a, 'delete': 0x2a, 'tab': 0x2b, 'space': 0x2c,
        'capslock': 0x39, 'forwarddelete': 0x4c, 'del': 0x4c, 'insert': 0x49, 'home': 0x4a, 'pageup': 0x4b, 'end': 0x4d,
        'pagedown': 0x4e, 'right': 0x4f, 'left': 0x50, 'down': 0x51, 'up': 0x52, 'minus': 0x2d, 'equal': 0x2e, 'slash': 0x38,
        'comma': 0x36, 'period': 0x37, 'grave': 0x35, 'backslash': 0x31, 'semicolon': 0x33, 'quote': 0x34}
KEYS.update({f'f{n}': 0x3a + n - 1 for n in range(1, 13)})
CONSUMER = {'volumeup': 0xe9, 'volumedown': 0xea, 'mute': 0xe2, 'playpause': 0xcd, 'next': 0xb5, 'previous': 0xb6,
            'brightnessup': 0x6f, 'brightnessdown': 0x70}
BUTTONS = {'left': 1, 'right': 2, 'middle': 4}


class InputError(Exception):
    """A user error; its message never contains typed text."""


def keyboard_line(mods, *usages):
    return 'K ' + ' '.join(f'{v:x}' for v in (mods, *usages))


def press(mods, usage):
    """The reports of one key press: modifiers first, then the key, released in the opposite order."""
    if mods:
        return [keyboard_line(mods), keyboard_line(mods, usage), keyboard_line(mods), keyboard_line(0)]
    return [keyboard_line(0, usage), keyboard_line(0)]


def text_lines(text):
    missing = sum(1 for ch in text if ch not in US)
    if missing:
        raise InputError(f'{missing} of {len(text)} characters have no key on the US layout; nothing was typed')
    lines = []
    for ch in text:
        usage, shifted = US[ch]
        lines += press(SHIFT if shifted else 0, usage)
    return lines


def key_lines(spec):
    """'cmd+shift+3', 'return', 'a', '?', 'volumeup' -> protocol lines."""
    parts = spec.split('+') if spec != '+' else ['+']
    if len(parts) > 1 and parts[-1] == '':   # 'cmd++' : the plus key
        parts = parts[:-2] + ['+']
    *mods, name = parts
    bits = 0
    for m in mods:
        if m.lower() not in MODIFIERS:
            raise InputError(f'unknown modifier "{m}" (cmd, ctrl, alt/option, shift, rcmd, rctrl, ralt, rshift)')
        bits |= MODIFIERS[m.lower()]
    lower = name.lower()
    if lower in CONSUMER:
        if bits:
            raise InputError('consumer keys take no modifiers')
        return [f'C {CONSUMER[lower]:x}', 'C 0']
    if lower in KEYS:
        return press(bits, KEYS[lower])
    if lower in MODIFIERS and not mods:   # a modifier alone (e.g. "shift")
        return [keyboard_line(MODIFIERS[lower]), keyboard_line(0)]
    if len(name) == 1 and name in US:
        usage, shifted = US[name]
        return press(bits | (SHIFT if shifted else 0), usage)
    raise InputError(f'unknown key "{name}" (piano-input.py keys lists them)')


def split(total, limit):
    """total as steps of at most limit in size, same sign."""
    steps, sign, left = [], -1 if total < 0 else 1, abs(total)
    while left:
        step = min(left, limit)
        steps.append(sign * step)
        left -= step
    return steps


def move_lines(dx, dy, steps=1, buttons=0):
    steps = max(1, steps)
    lines = []
    for i in range(steps):   # an even spread over steps, each within the report's range
        sx = dx * (i + 1) // steps - dx * i // steps
        sy = dy * (i + 1) // steps - dy * i // steps
        xs, ys = split(sx, 32767), split(sy, 32767)
        for j in range(max(len(xs), len(ys), 1)):
            x = xs[j] if j < len(xs) else 0
            y = ys[j] if j < len(ys) else 0
            lines.append(f'P {buttons:x} {x} {y}')
    return lines


def click_lines(button='left', double=False):
    if button not in BUTTONS:
        raise InputError(f'unknown button "{button}" (left, right, middle)')
    once = [f'P {BUTTONS[button]:x} 0 0', 'P 0 0 0']
    return once + once if double else once


def scroll_lines(dy, dx=0):
    ys, xs = split(dy, 127), split(dx, 127)
    return [f'P 0 0 0 {ys[j] if j < len(ys) else 0} {xs[j] if j < len(xs) else 0}' for j in range(max(len(ys), len(xs), 1))]


def drag_lines(dx, dy, steps=10):
    return ['P 1 0 0'] + move_lines(dx, dy, steps, buttons=1) + ['P 0 0 0']


# ------------------------------------------------------------------------------------------------------------ the port
class Port:
    def __init__(self, path, ack=True, delay=0.008):
        console = load_console()
        self.fd = console.open_port(path)
        self.ack, self.delay, self.buffer = ack, delay, b''
        self.drain()

    def drain(self):
        while select.select([self.fd], [], [], 0)[0]:
            if not os.read(self.fd, 4096):
                break

    def reply(self, timeout=1.0):
        deadline = time.monotonic() + timeout
        while b'\n' not in self.buffer:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                return None
            data = os.read(self.fd, 4096)
            if not data:
                return None
            self.buffer += data
        line, self.buffer = self.buffer.split(b'\n', 1)
        return line.decode(errors='replace').strip()

    def send(self, line):
        os.write(self.fd, line.encode() + b'\n')
        answer = self.reply() if self.ack else 'ok'
        if answer is None:
            raise InputError('no answer from the tablet within 1 s (is it booted with piano-usb-hid=1? --no-ack sends blind)')
        if answer.startswith('err'):
            raise InputError(f'the tablet refused a command: {answer}')
        if self.delay:
            time.sleep(self.delay)
        return answer

    def close(self):
        os.close(self.fd)


def load_console():
    spec = importlib.util.spec_from_file_location('usb_console', HERE / 'usb-console.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def find_port():
    ports = load_console().find_ports()
    if PORT not in ports:
        raise InputError('no "piano input" port on this Mac (usb-console.py --list; the tablet needs piano-usb=2 piano-usb-hid=1)')
    return ports[PORT]


def run(port, lines):
    for line in lines:
        port.send(line)
    return len(lines)


def main(argv=None):
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('--device', help='the input port\'s /dev/cu.* node instead of the ioreg lookup')
    a.add_argument('--no-ack', action='store_true', help='do not wait for the tablet\'s answers')
    a.add_argument('--delay', type=float, default=8, help='milliseconds between reports (default 8)')
    sub = a.add_subparsers(dest='command', required=True)
    t = sub.add_parser('type')
    t.add_argument('text', nargs='?')
    t.add_argument('--secret', action='store_true', help='read the text without echo')
    t.add_argument('--stdin', action='store_true', help='read the text from stdin')
    t.add_argument('--enter', action='store_true', help='press Return afterwards')
    k = sub.add_parser('key')
    k.add_argument('name')
    k.add_argument('--repeat', type=int, default=1)
    m = sub.add_parser('move')
    m.add_argument('dx', type=int)
    m.add_argument('dy', type=int)
    m.add_argument('--steps', type=int, default=1)
    c = sub.add_parser('click')
    c.add_argument('button', nargs='?', default='left')
    c.add_argument('--double', action='store_true')
    s = sub.add_parser('scroll')
    s.add_argument('dy', type=int)
    s.add_argument('dx', type=int, nargs='?', default=0)
    d = sub.add_parser('drag')
    d.add_argument('dx', type=int)
    d.add_argument('dy', type=int)
    sub.add_parser('release')
    sub.add_parser('status')
    sub.add_parser('keys')
    r = sub.add_parser('raw')
    r.add_argument('line')
    args = a.parse_args(argv)
    try:
        if args.command == 'keys':
            print('keys:', ' '.join(sorted(KEYS)), '| consumer:', ' '.join(sorted(CONSUMER)), '| modifiers:', ' '.join(sorted(MODIFIERS)),
                  '| or any US character, e.g. key ? / key cmd+a')
            return 0
        text = None
        if args.command == 'type':
            if sum(bool(x) for x in (args.text is not None, args.secret, args.stdin)) != 1:
                raise InputError('type takes exactly one of TEXT, --secret, --stdin')
            text = args.text if args.text is not None else getpass.getpass('text (not echoed): ') if args.secret else sys.stdin.read()
            if args.stdin and text.endswith('\n'):
                text = text[:-1]
            lines = text_lines(text) + (press(0, KEYS['return']) if args.enter else [])
        elif args.command == 'key':
            lines = key_lines(args.name) * max(1, args.repeat)
        elif args.command == 'move':
            lines = move_lines(args.dx, args.dy, args.steps)
        elif args.command == 'click':
            lines = click_lines(args.button, args.double)
        elif args.command == 'scroll':
            lines = scroll_lines(args.dy, args.dx)
        elif args.command == 'drag':
            lines = drag_lines(args.dx, args.dy)
        elif args.command == 'release':
            lines = ['R']
        elif args.command == 'status':
            lines = ['?']
        else:
            lines = [args.line]
        port = Port(args.device or find_port(), ack=not args.no_ack, delay=args.delay / 1000)
        try:
            if args.command == 'status':
                print(port.send('?'))
                return 0
            sent = run(port, lines)
        except BaseException:
            # Interrupted or refused half way: nothing stays pressed (the tablet also releases after piano-usb-hid-hold).
            try:
                port.ack = False
                port.send('R')
            except Exception:
                pass
            raise
        finally:
            port.close()
        if text is not None:
            print(f'typed {len(text)} characters ({sent} reports){" + Return" if args.enter else ""}')
        else:
            print(f'{args.command}: {sent} reports')
        return 0
    except InputError as e:
        print(f'piano-input: {e}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == '__main__':
    sys.exit(main())
