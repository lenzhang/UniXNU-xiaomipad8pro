#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host test of tooling/k27/desk-reports.sh (the diag desktop image's boot-job reporter, docs/attended-desktop.md) on this Mac, with
stand-ins for launchctl and log on PATH and the "minute" scaled to 0 s:

  python3 tooling/k27/test-desk-reports.py

Checks: the overrides and jobs of the respawn loops; the failing jobs (a status 78 and a crash's negative status, not the 0 and "-"
lines) with each one's launchctl print state lines; the login found from the console file's owner; the unified log filtered to the
phase lines and folded; xprotectd's lines; the busiest processes; the "no login" path when the console stays root's; every line
prefixed and at most 150 characters after the prefix.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

D = Path(__file__).resolve().parents[2]
SCRIPT = D / 'tooling/k27/desk-reports.sh'

LAUNCHCTL = r'''#!/bin/sh
case "$1" in
  list) printf 'PID\tStatus\tLabel\n'
        printf -- '-\t0\tcom.apple.fine\n'
        printf '123\t-\tcom.apple.running\n'
        printf -- '-\t78\tcom.apple.mobileactivationd\n'
        printf -- '-\t-5\tcom.apple.watchdogd\n'
        printf -- '-\t1\tcom.apple.security.xprotectd\n' ;;
  print-disabled) printf 'disabled services = {\n\t"com.apple.PerfPowerServices" => disabled\n\t"com.apple.other" => disabled\n}\n' ;;
  print) printf '%s = {\n\tactive count = 0\n\tstate = not running\n\tprogram = /usr/libexec/x\n\truns = 15\n' "$2"
         printf '\tlast exit code = 78: EX_CONFIG\n\tspawn type = daemon (3)\n\tsomething else = 1\n}\n' ;;
esac
'''
LOG = r'''#!/bin/sh
case "$*" in
  *xprotectd*) echo 'xprotectd: [com.apple.xprotect] Exiting: no payload' ;;
  *'"authorizationhost"'*) echo 'authorizationhost: [com.apple.Authorization] mechanism builtin:authenticate took 13 s' ;;
  *) echo '2026-11-12 10:00:01.000 E loginwindow[104:1] -[LoginApp start] Login Window Application Started'
     echo '2026-11-12 10:00:02.000 Df WindowServer[120:2] uninteresting line'
     echo '2026-11-12 10:00:03.000 E Dock[400:3] giving up waiting for match - runloop result is 3'
     i=0; while [ $i -lt 400 ]; do echo "2026-11-12 10:00:04.000 E Finder[500:4] error $i"; i=$((i + 1)); done ;;
esac
'''


def run(console, minutes='1'):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bin_dir = tmp / 'bin'
        bin_dir.mkdir()
        for name, body in (('launchctl', LAUNCHCTL), ('log', LOG)):
            (bin_dir / name).write_text(body)
            (bin_dir / name).chmod(0o755)
        out = tmp / 'console.txt'
        env = dict(os.environ, PATH=f'{bin_dir}:{os.environ["PATH"]}', DESK_REPORTS_OUT=str(out), DESK_REPORTS_TICK='1',
                   DESK_REPORTS_SCALE='0', DESK_REPORTS_CONSOLE=str(console), DESK_REPORTS_TMP=str(tmp / 'failing'))
        subprocess.run(['/bin/sh', str(SCRIPT), minutes], env=env, check=True, timeout=60)
        return out.read_text().splitlines()


def main():
    failures = []

    def check(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            failures.append(what)

    with tempfile.NamedTemporaryFile() as console:   # owned by this user (not 0): a login at once
        lines = run(console.name)
    check(all(l.startswith('desk-reports: ') for l in lines), 'every line has the prefix')
    check(all(len(l) <= len('desk-reports: ') + 150 for l in lines), 'every line at most 150 characters after the prefix')
    text = '\n'.join(lines)
    check('override: \t"com.apple.PerfPowerServices" => disabled' in text and 'com.apple.other' not in text,
          'the loops\' overrides only')
    check('job: -\t78\tcom.apple.mobileactivationd' in text, 'the loops\' jobs')
    failing = [l for l in lines if l.startswith('desk-reports: failing (start): ') and 'PID STATUS' not in l]
    check(len(failing) == 3 and all(('78' in l or '-5' in l or '\t1\t' in l) for l in failing),
          'failing jobs: status 78, -5 and 1; not 0 or "-"')
    check('job com.apple.mobileactivationd: \tlast exit code = 78: EX_CONFIG' in text, 'a failing job\'s state lines')
    check('something else' not in text, 'only the state lines of launchctl print')
    check(any(l.startswith('desk-reports: login: the console\'s owner is uid') for l in lines), 'the login found')
    check('Login Window Application Started' in text and 'giving up waiting for match' in text, 'the phase lines of the log')
    check('uninteresting line' not in text, 'other log lines left out')
    check(sum(1 for l in lines if 'Finder[500:4] error' in l) <= 250, 'the log is capped')
    check('Exiting: no payload' in text, 'xprotectd\'s lines')
    check('mechanism builtin:authenticate took 13 s' in text, 'authorizationhost\'s lines, unfiltered')
    check(any(l.startswith('desk-reports: ps (login + 3 min): ') for l in lines), 'the busiest processes after the login')
    check(lines[-1] == 'desk-reports: end', 'the end line')

    lines = run('/etc/hosts', minutes='0')   # root's: no login
    text = '\n'.join(lines)
    check('desk-reports: no login within 0 min' in text, 'no login: said')
    check(any(l.startswith('desk-reports: failing (no login): ') for l in lines), 'no login: the failing jobs still')
    check('log: loginwindow' not in text, 'no login: no log window')

    print(f'{len(failures)} failed' if failures else 'all passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
