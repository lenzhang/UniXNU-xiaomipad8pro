#!/usr/bin/env python3
"""Host test of pianousb-shell.c (the root shell on PianoDwc3's user pipe, docs/drivers/usb-device-debug.md section 6), on the Mac.

The device is a pseudo-terminal standing in for /dev/pianousb, with what made the tablet's helper deaf (desk-dev, 2026-11-12 17:27):
a shim (DYLD_INSERT_LIBRARIES, interposed poll()) answers poll() on that device with POLLNVAL at once, which is what XNU's poll()
does for a character device whose cdevsw is not kqueue-capable (spec_kqfilter, filt_specattach: EINVAL, poll_nocancel: POLLNVAL).
Checked: the shim does that (a probe program), the helper still relays a command and its output both ways, and it does not spin
while idle (its CPU time over a quiet second and a half).

    python3 tooling/native/pianousb-shell/test_pianousb_shell.py
"""
import os
import select
import subprocess
import sys
import tempfile
import termios
import time
import tty
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent

SHIM = r'''
#include <poll.h>
#include <stdlib.h>
#include <sys/stat.h>
// poll() as XNU answers it for /dev/pianousb before PianoDwc3 marked its cdevsw kqueue-capable: the device's registration fails,
// it is reported POLLNVAL at once and nothing is waited for.
static int noKqueue(int fd)
{
  const char *path = getenv("PIANOUSB_TEST_NO_KQUEUE");
  struct stat a, b;
  return path && fd >= 0 && fstat(fd, &a) == 0 && stat(path, &b) == 0 && S_ISCHR(a.st_mode) && a.st_rdev == b.st_rdev;
}
static int shimPoll(struct pollfd *fds, nfds_t n, int timeout)
{
  int bad = 0;
  for (nfds_t i = 0; i < n; ++i) bad += noKqueue(fds[i].fd);
  if (!bad) return poll(fds, n, timeout);
  for (nfds_t i = 0; i < n; ++i) fds[i].revents = noKqueue(fds[i].fd) ? POLLNVAL : 0;
  return bad;
}
__attribute__((used, section("__DATA,__interpose"))) static struct { const void *replacement, *original; } interpose[] = {
  {(const void *)shimPoll, (const void *)poll},
};
'''

PROBE = r'''
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
int main(int argc, char **argv)
{
  const int fd = argc > 1 ? open(argv[1], O_RDWR | O_NOCTTY) : -1;
  struct pollfd p = {.fd = fd, .events = POLLIN};
  const int n = poll(&p, 1, 2000);
  printf("%d %x\n", n, p.revents);
  return 0;
}
'''


def cc(out, *args):
    subprocess.run(['xcrun', 'clang', '-arch', 'arm64', '-mmacosx-version-min=14.0', '-O2', '-Wall', '-Wextra', '-Werror', *args,
                    '-o', str(out)], check=True)


def cpu_seconds(pid):
    text = subprocess.run(['ps', '-o', 'time=', '-p', str(pid)], capture_output=True, text=True).stdout.strip()
    minutes, seconds = text.split(':') if ':' in text else ('0', text or '0')
    return int(minutes) * 60 + float(seconds)


class PianoUsbShellTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='pianousb-shell-test-')
        d = Path(cls.tmp.name)
        cls.helper, cls.shim, cls.probe = d / 'pianousb-shell', d / 'shim.dylib', d / 'probe'
        cc(cls.helper, str(HERE / 'pianousb-shell.c'))
        (d / 'shim.c').write_text(SHIM)
        cc(cls.shim, '-dynamiclib', str(d / 'shim.c'))
        (d / 'probe.c').write_text(PROBE)
        cc(cls.probe, str(d / 'probe.c'))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        # The Mac's end of the cable is the master; the device the helper opens is the slave, raw both ways like the USB pipe (no
        # echo, no newline translation), kept open here so that its settings stay (a pty's first open resets them).
        self.master, self.slave = os.openpty()
        self.device = os.ttyname(self.slave)
        tty.setraw(self.slave)
        attrs = termios.tcgetattr(self.slave)
        attrs[3] &= ~termios.ECHO
        termios.tcsetattr(self.slave, termios.TCSANOW, attrs)
        self.env = dict(os.environ, DYLD_INSERT_LIBRARIES=str(self.shim), PIANOUSB_TEST_NO_KQUEUE=self.device)
        self.proc = None

    def tearDown(self):
        if self.proc:
            self.proc.kill()
            self.proc.wait()
            self.proc.stderr.close()
        os.close(self.slave)
        os.close(self.master)

    def read_until(self, needle, seconds):
        got, end = b'', time.monotonic() + seconds
        while needle not in got and time.monotonic() < end:
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if ready:
                got += os.read(self.master, 4096)
        return got

    def test_shim_answers_pollnval(self):
        out = subprocess.run([str(self.probe), self.device], env=self.env, capture_output=True, text=True, timeout=10).stdout.split()
        self.assertEqual(out, ['1', '%x' % select.POLLNVAL], 'the shim must answer poll() on the device as XNU did for /dev/pianousb')

    def test_relays_both_ways_without_spinning(self):
        self.proc = subprocess.Popen([str(self.helper), self.device, '/bin/sh'], env=self.env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.assertIn(b'a new shell on the tablet', self.read_until(b'(exit starts another)', 10))
        os.write(self.master, b"printf 'piano%sok\\n' -relay-\n")   # the output, piano-relay-ok, is not the command's text
        self.assertIn(b'piano-relay-ok', self.read_until(b'piano-relay-ok', 10), 'no output of the command came back')
        time.sleep(0.5)
        before = cpu_seconds(self.proc.pid)
        time.sleep(1.5)
        self.assertLess(cpu_seconds(self.proc.pid) - before, 0.5, 'the helper spins while idle')
        os.write(self.master, b'exit\n')   # a new shell starts: the banner again
        self.assertIn(b'a new shell on the tablet', self.read_until(b'(exit starts another)', 10))
        self.assertIsNone(self.proc.poll(), 'the helper exited')


if __name__ == '__main__':
    if sys.platform != 'darwin':
        print('test_pianousb_shell: macOS only (XNU poll semantics, xcrun clang)')
        sys.exit(0)
    unittest.main()
