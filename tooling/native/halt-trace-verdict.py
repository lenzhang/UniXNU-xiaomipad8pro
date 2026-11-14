#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Name the waiter of a halt or restart from a boot log with k27 0068's `piano-halt: ...` markers (piano-halt-trace=1).

    halt-trace-verdict.py LOG [LOG ...]       a mailbox dump or console capture; the last PEHaltRestart in it is read
    halt-trace-verdict.py --self-test

Prints the markers of the last halt/restart in order (with their uptimes) and the verdict of docs/halt.md's decision table:
the last phase entered and what it was waiting for (a priority client, a power-plane driver, a halt/restart platform action, a
quiesce-blocking IOPM request, the platform's haltRestart, PSCI). The mailbox can carry bit flips: lines are matched loosely and a
line that matches nothing is ignored. Exit 0 when a verdict was reached, 1 when the log has no PEHaltRestart marker.
"""
import re
import sys

START = re.compile(r'piano-halt: PEHaltRestart type (\d+) at t=(\d+) ms')


def read(paths):
    lines = []
    for path in paths:
        with open(path, 'rb') as f:
            lines += f.read().decode('utf-8', 'replace').split('\n')
    return lines


def verdict(lines):
    """(markers, verdict) for the last PEHaltRestart in lines; verdict None when there is none."""
    starts = [i for i, line in enumerate(lines) if START.search(line)]
    if not starts:
        return [], None
    tail = lines[starts[-1]:]
    markers = [line[line.index('piano-halt:'):].strip() for line in tail if 'piano-halt:' in line]
    timed_out = any('Halt/Restart Timed Out' in line or 'timed out in phase' in line for line in tail)
    phase_panic = next((m.group(0) for line in tail for m in [re.search(r"timed out in phase '([^']*)'", line)] if m), None)
    joined = '\n'.join(markers)
    phases = re.findall(r"phase '([^']*)'", joined)
    phase = phases[-1] if phases else None

    def last(pattern):
        found = [m for m in (re.search(pattern, x) for x in markers) if m]
        return found[-1] if found else None

    # PSCI and the platform's haltRestart: after handlePlatformHaltRestart returned
    if 'handlePlatformHaltRestart returned' in joined:
        psci = last(r'PSCI (\S+) returned (-?\d+)')
        if psci:
            return markers, 'PSCI %s returned %s: the firmware refused the reset/power-off; not a kext acknowledgement' % (
                psci.group(1), psci.group(2))
        if last(r'PSCI (\S+) \(0x[0-9a-f]+\) through'):
            return markers, 'PSCI call made and never returned: the conduit/firmware path (or it succeeded without the effect)'
        hr = last(r'(\S+)::haltRestart\((\d+)\) -> \(PE_halt_restart (\S+)\)')
        if hr and not last(r'haltRestart returned'):
            return markers, 'the platform\'s haltRestart: %s::haltRestart (PE_halt_restart %s) never returned' % (hr.group(1), hr.group(3))
        if last(r'haltRestart returned (-?\d+)'):
            return markers, 'haltRestart returned %s and no PSCI marker followed: halt_all_cpus not reached' % (
                last(r'haltRestart returned (-?\d+)').group(1))
        return markers, 'handlePlatformHaltRestart returned and no haltRestart marker followed'
    if phase == 'Quiescing PM':
        attached = [m.group(1, 2, 3) for m in (re.search(r'quiesce blocker(?: \(late\))? (\S+) type (0x[0-9a-f]+) request (\S+)', x)
                                                 for x in markers) if m]
        retired = {m.group(3) for m in (re.search(r'quiesce blocker retired (\S+) type (0x[0-9a-f]+) request (\S+)', x)
                                        for x in markers) if m}
        open_ = [a for a in attached if a[2] not in retired and 'retired' not in a[0]]
        if 'PM quiesced' in joined:
            return markers, 'PM quiesced; handlePlatformHaltRestart did not return after it (its logging/checkShutdownTimeout)'
        if open_:
            return markers, 'PM quiesce blocked by %s' % ', '.join('%s (request type %s)' % (s, t) for s, t, _ in open_)
        if 'quiesce request retired' in joined or 'PM quiesce callback' in joined:
            return markers, 'the quiesce request retired/called back but the waiter did not wake (gPMHaltLock/IOLockSleep)'
        if 'quiescePowerTree returned' not in joined:
            return markers, 'quiescePowerTree itself did not return (the PM work loop)'
        return markers, 'PM quiesce: no blocker attached or retired and no callback: IOPM work-loop/request bookkeeping'
    if phase == 'Notifying HaltRestart action handlers':
        entries = [m.group(1) for m in (re.search(r'IOPlatformHaltRestartAction -> (\S+)', line) for line in tail) if m]
        returns = [m.group(1) for m in (re.search(r'IOPlatformHaltRestartAction <- (\S+) returned', x) for x in markers) if m]
        pending = [e for e in entries if returns.count(e) < entries.count(e)]
        if pending:
            return markers, 'the halt/restart platform action of %s never returned (its callPlatformFunction)' % pending[-1]
        if 'halt/restart actions done' in joined:
            return markers, 'the platform actions finished; the quiesce phase was not entered'
        return markers, 'the halt/restart action walk did not end, no action entry seen'
    if phase == 'Notifying power plane drivers':
        if 'notifySystemShutdown returned' in joined:
            return markers, 'notifySystemShutdown returned; the platform-action phase was not entered'
        return markers, 'a power-plane driver\'s systemWillShutdown (PMShutdown) or the halt workers\' coordination (notifySystemShutdown)'
    if phase == 'Notifying priority clients':
        client = last(r'priority client (\d+) handler (\S+) ->')
        done = last(r'priority client (\d+) returned')
        if client and (not done or done.group(1) != client.group(1)):
            return markers, 'the priority client %s handler %s never returned' % (client.group(1), client.group(2))
        return markers, 'the priority clients returned; the power-plane phase was not entered (the File Server Mode setting)'
    if 'NVRAM sync ->' in joined and 'NVRAM sync returned' not in joined:
        return markers, 'PESyncNVRAM never returned'
    note = ' (the timeout fired)' if timed_out else ''
    return markers, 'no phase marker after PEHaltRestart%s%s' % (note, (': ' + phase_panic) if phase_panic else '')


SELF_TESTS = [
    (['piano-halt: PEHaltRestart type 3 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Notifying priority clients' at t=101 ms",
      'piano-halt: priority client 0 handler 0xfffffff00a000000 ->'], 'the priority client 0 handler 0xfffffff00a000000 never returned'),
    (['piano-halt: PEHaltRestart type 3 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Notifying priority clients' at t=101 ms",
      "piano-halt: phase 'Notifying power plane drivers' at t=102 ms"], "a power-plane driver's systemWillShutdown"),
    (['piano-halt: PEHaltRestart type 3 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Notifying HaltRestart action handlers' at t=103 ms",
      'piano-halt: halt/restart actions: 1 registered, message 0x3 ->',
      'IOPlatformHaltRestartAction -> AppleFoo'], 'the halt/restart platform action of AppleFoo never returned'),
    (['piano-halt: PEHaltRestart type 3 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Quiescing PM' at t=104 ms",
      'piano-halt: quiesce request 0x1: the work queue (2 services) at t=104 ms',
      'piano-halt: quiesce blocker NativeUfsBlock type 0x8 request 0xa',
      'piano-halt: quiesce blocker IODisplay type 0x2 request 0xb',
      'piano-halt: quiescePowerTree returned 0x0; waiting for the callback',
      'piano-halt: quiesce blocker retired IODisplay type 0x2 request 0xb',
      'Halt/Restart Timed Out'], 'PM quiesce blocked by NativeUfsBlock (request type 0x8)'),
    (['piano-halt: PEHaltRestart type 3 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Quiescing PM' at t=104 ms",
      'piano-halt: quiesce request 0x1: an empty work queue (0 services) at t=104 ms',
      'piano-halt: quiescePowerTree returned 0x0; waiting for the callback'], 'IOPM work-loop/request bookkeeping'),
    (['piano-halt: PEHaltRestart type 3 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      'piano-halt: handlePlatformHaltRestart returned after 50 ms',
      'piano-halt: ArmDTPlatform::haltRestart(3) -> (PE_halt_restart NULL) at t=151 ms'],
     "the platform's haltRestart: ArmDTPlatform::haltRestart (PE_halt_restart NULL) never returned"),
    (['piano-halt: PEHaltRestart type 3 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      'piano-halt: handlePlatformHaltRestart returned after 50 ms',
      'piano-halt: ArmDTPlatform::haltRestart(3) -> (PE_halt_restart NULL) at t=151 ms',
      'piano-halt: haltRestart returned -1 at t=151 ms',
      'piano-halt: PSCI SYSTEM_RESET (0x84000009) through smc',
      'piano-halt: PSCI SYSTEM_RESET returned -2'], 'PSCI SYSTEM_RESET returned -2: the firmware refused'),
    (['piano-halt: PEHaltRestart type 3 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      'piano-halt: handlePlatformHaltRestart returned after 50 ms',
      'piano-halt: haltRestart returned -1 at t=151 ms',
      'piano-halt: PSCI SYSTEM_RESET (0x84000009) through smc'], 'PSCI call made and never returned'),
    # only the last PEHaltRestart counts; a corrupted line is ignored
    (['piano-halt: PEHaltRestart type 3 at t=1 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Quiescing PM' at t=2 ms",
      'piano-halt: PEHaltRestart type 3 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      'pi\x01no-halt: ph@se garbage',
      "piano-halt: phase 'Notifying priority clients' at t=101 ms",
      'piano-halt: priority client 0 handler 0x5 ->',
      'piano-halt: priority client 0 returned after 1 ms',
      "piano-halt: phase 'Notifying power plane drivers' at t=102 ms",
      'piano-halt: notifySystemShutdown returned at t=103 ms'], 'notifySystemShutdown returned; the platform-action phase'),
    (['nothing here'], None),
]


def self_test():
    for i, (lines, want) in enumerate(SELF_TESTS):
        _, got = verdict(lines)
        if want is None:
            assert got is None, (i, got)
        else:
            assert got and want in got, (i, got, want)
    print('halt-trace-verdict: self-test PASS (%d cases: priority client, power plane, platform action, quiesce blocker, quiesce '
          'bookkeeping, platform haltRestart, PSCI refused, PSCI silent, last-of-two with a damaged line, no marker)' % len(SELF_TESTS))


def main():
    if sys.argv[1:] == ['--self-test']:
        self_test()
        return 0
    if not sys.argv[1:]:
        print(__doc__)
        return 2
    markers, result = verdict(read(sys.argv[1:]))
    if result is None:
        print('halt-trace-verdict: no "piano-halt: PEHaltRestart" marker (piano-halt-trace=1 missing, or no halt/restart in the log)')
        return 1
    for m in markers:
        print('  ' + m)
    print('verdict: ' + result)
    return 0


if __name__ == '__main__':
    sys.exit(main())
