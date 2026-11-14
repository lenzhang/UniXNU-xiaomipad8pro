#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Name the waiter of a halt or restart from a boot log with k27 0068's `piano-halt: ...` markers (piano-halt-trace=1)
and k27 0069's `piano-psci-halt: ...` lines (the PSCI reset/power-off of PE_halt_restart).

    halt-trace-verdict.py LOG [LOG ...]       a mailbox dump or console capture; the last PEHaltRestart in it is read
    halt-trace-verdict.py --self-test

Prints the markers of the last halt/restart in order (with their uptimes) and the verdict of docs/halt.md's decision table:
the last phase entered and what it was waiting for (a priority client, a power-plane driver, a halt/restart platform action, a
quiesce-blocking IOPM request, the platform's haltRestart, PSCI). The waiter is read from the markers before the 30 s timeout
("timed out after"); what follows it is the panic's own restart. The mailbox carries bit flips ("aT t=", "priorIty", "quieqce",
"retupned", "piano)halt:"): a line's tag may differ in 2 characters, words are matched without case and with 1 (4-6 letters) or
2 (7 and more) characters flipped, and a line that matches nothing is ignored. Exit 0 when a verdict was reached, 1 when the log
has no PEHaltRestart marker.
"""
import re
import sys

# k27 0068's markers, and k27 0069's: piano-psci-halt's PSCI call, the boot guard leaving a started restart's reason alone
TAGS = ('piano-halt:', 'piano-psci-halt:', 'piano-boot-guard:')
# every word of the markers' formats (0068, 0069) that a pattern below relies on, for the flipped-character repair
VOCAB = '''PEHaltRestart type at ms timeout handlePlatformHaltRestart returned after Restart Halt NVRAM sync phase priority client
clients handler notifySystemShutdown halt restart actions done registered message quiescePowerTree waiting for the callback
quiesce request retired blocker late PM quiesced work queue empty services haltRestart PE_halt_restart NULL set PSCI SYSTEM_RESET
SYSTEM_OFF through smc hvc timed out Notifying power plane drivers HaltRestart action handlers Quiescing IOPlatformHaltRestartAction
began healthy left Notifications completed'''.split()
WORDS = {}
for w in VOCAB:
    WORDS.setdefault(len(w), []).append(w)


def flips(a, b):
    return sum(1 for x, y in zip(a.lower(), b.lower()) if x != y)


def fix_word(m):
    word = m.group(0)
    allowed = 0 if len(word) <= 3 else 1 if len(word) <= 6 else 2
    best, best_d, tie = None, allowed + 1, False
    for w in WORDS.get(len(word), ()):
        d = flips(word, w)
        if d < best_d:
            best, best_d, tie = w, d, False
        elif d == best_d:
            tie = tie or w != best
    return best if best is not None and not tie else word


def tagged(line):
    """The marker text of LINE (its tag repaired, its words too), or None."""
    low = line.lower()
    for tag in TAGS:
        i = low.find(tag)
        if i < 0 and len(line) >= len(tag) and flips(line[:len(tag)], tag) <= 2:
            i = 0
        if i >= 0:
            text = tag + re.sub(r'[A-Za-z_]+', fix_word, line[i + len(tag):]).rstrip()
            if tag == 'piano-boot-guard:' and not re.search(r'restart began|healthy after', text, re.I):
                return None
            return text
    return None


START = re.compile(r'piano-halt: PEHaltRestart type (\d+) at t=(\d+) ms', re.I)
TIMEOUT = re.compile(r'piano-halt: timed out after (\d+) s at t=(\d+) ms', re.I)


def read(paths):
    lines = []
    for path in paths:
        with open(path, 'rb') as f:
            lines += f.read().decode('utf-8', 'replace').split('\n')
    return lines


def verdict(lines):
    """(markers, verdict) for the last PEHaltRestart in lines; verdict None when there is none."""
    texts = [tagged(line) for line in lines]
    starts = [i for i, t in enumerate(texts) if t and START.search(t)]
    if not starts:
        return [], None
    raw_tail = lines[starts[-1]:]
    markers = [t for t in texts[starts[-1]:] if t]
    cut = next((i for i, m in enumerate(markers) if TIMEOUT.search(m)), None)
    before = markers if cut is None else markers[:cut]
    timed_out = cut is not None or any(re.search(r'Halt/Restart Timed Out|timed out in phase', x, re.I) for x in raw_tail)
    phase_panic = next((m.group(0) for x in raw_tail for m in [re.search(r"timed out in phase '([^']*)'", x, re.I)] if m), None)
    joined = '\n'.join(before)
    phases = re.findall(r"phase '([^']*)'", joined, re.I)
    phase = phases[-1].lower() if phases else None

    def last(pattern):
        found = [m for m in (re.search(pattern, x, re.I) for x in before) if m]
        return found[-1] if found else None

    def has(text):
        return re.search(re.escape(text), joined, re.I) is not None

    # PSCI and the platform's haltRestart: after handlePlatformHaltRestart returned
    if has('handlePlatformHaltRestart returned'):
        handler = last(r'piano-psci-halt: PE_halt_restart\((\d+)\) at (\d+) ms: PSCI (\S+)')
        if handler:
            refused = last(r'piano-psci-halt: PSCI (\S+) returned (-?\d+)')
            if refused:
                return markers, 'piano-psci-halt (k27 0069): PSCI %s returned %s: the firmware refused; PEHaltRestart returned it' % (
                    refused.group(1), refused.group(2))
            return markers, 'piano-psci-halt (k27 0069): PSCI %s issued at %s ms and it did not return: the reset/power-off was taken%s' % (
                handler.group(3), handler.group(2), '; but the 30 s timeout fired after it' if timed_out else '')
        psci = last(r'PSCI (\S+) returned (-?\d+)')
        if psci:
            return markers, 'PSCI %s returned %s: the firmware refused the reset/power-off; not a kext acknowledgement' % (
                psci.group(1), psci.group(2))
        if last(r'PSCI (\S+) \(0x[0-9a-f]+\) through'):
            return markers, 'PSCI call made and never returned: the conduit/firmware path (or it succeeded without the effect)'
        hr = last(r'(\S+)::haltRestart\((\d+)\) -> \(PE_halt_restart (\S+)\)')
        ret = last(r'haltRestart returned (-?\d+)')
        if hr and not ret:
            return markers, 'the platform\'s haltRestart: %s::haltRestart (PE_halt_restart %s) never returned' % (hr.group(1), hr.group(3))
        if ret:
            who = ' (%s, PE_halt_restart %s)' % (hr.group(1), hr.group(3)) if hr else ''
            if timed_out:
                return markers, ('the platform\'s haltRestart returned %s%s: no restart was made, the caller went on and the 30 s timer '
                                 'fired (no PE_halt_restart: k27 0069 piano-psci-halt)' % (ret.group(1), who))
            return markers, 'haltRestart returned %s%s and no PSCI marker followed: halt_all_cpus not reached' % (ret.group(1), who)
        return markers, 'handlePlatformHaltRestart returned and no haltRestart marker followed'
    if phase == 'quiescing pm':
        attached = [m.group(1, 2, 3) for m in (re.search(r'quiesce blocker(?: \(late\))? (\S+) type (0x[0-9a-f]+) request (\S+)', x, re.I)
                                                 for x in before) if m]
        retired = {m.group(3) for m in (re.search(r'quiesce blocker retired (\S+) type (0x[0-9a-f]+) request (\S+)', x, re.I)
                                        for x in before) if m}
        open_ = [a for a in attached if a[2] not in retired and a[0].lower() != 'retired']
        if has('PM quiesced'):
            return markers, 'PM quiesced; handlePlatformHaltRestart did not return after it (its logging/checkShutdownTimeout)'
        if open_:
            return markers, 'PM quiesce blocked by %s' % ', '.join('%s (request type %s)' % (s, t) for s, t, _ in open_)
        if has('quiesce request retired') or has('PM quiesce callback'):
            return markers, 'the quiesce request retired/called back but the waiter did not wake (gPMHaltLock/IOLockSleep)'
        if not has('quiescePowerTree returned'):
            return markers, 'quiescePowerTree itself did not return (the PM work loop)'
        return markers, 'PM quiesce: no blocker attached or retired and no callback: IOPM work-loop/request bookkeeping'
    if phase == 'notifying haltrestart action handlers':
        entries = [m.group(1) for m in (re.search(r'IOPlatformHaltRestartAction -> (\S+)', x, re.I) for x in raw_tail) if m]
        returns = [m.group(1) for m in (re.search(r'IOPlatformHaltRestartAction <- (\S+) returned', x, re.I) for x in before) if m]
        pending = [e for e in entries if returns.count(e) < entries.count(e)]
        if pending:
            return markers, 'the halt/restart platform action of %s never returned (its callPlatformFunction)' % pending[-1]
        if has('halt/restart actions done'):
            return markers, 'the platform actions finished; the quiesce phase was not entered'
        return markers, 'the halt/restart action walk did not end, no action entry seen'
    if phase == 'notifying power plane drivers':
        if has('notifySystemShutdown returned'):
            return markers, 'notifySystemShutdown returned; the platform-action phase was not entered'
        return markers, 'a power-plane driver\'s systemWillShutdown (PMShutdown) or the halt workers\' coordination (notifySystemShutdown)'
    if phase == 'notifying priority clients':
        client = last(r'priority client (\d+) handler (\S+) ->')
        done = last(r'priority client (\d+) returned')
        if client and (not done or done.group(1) != client.group(1)):
            return markers, 'the priority client %s handler %s never returned' % (client.group(1), client.group(2))
        return markers, 'the priority clients returned; the power-plane phase was not entered (the File Server Mode setting)'
    if has('NVRAM sync ->') and not has('NVRAM sync returned'):
        return markers, 'PESyncNVRAM never returned'
    note = ' (the timeout fired)' if timed_out else ''
    return markers, 'no phase marker after PEHaltRestart%s%s' % (note, (': ' + phase_panic) if phase_panic else '')


# halt1 (2026-11-14, a console "reset" with piano-halt-trace=1): the mailbox's markers as they came, bit flips and all
HALT1 = '''piano-halt: PEHaltRestart type 1 aT t=99650 ms (timeout 30 s): handlePlatformHaltRestart ->
piano-`alt: Restart: LVRAM sync ->
[System Event] [2391606986] [ANFO] [Subsystem: nvram] [Event: sync] triggered
piano-halt: NVRAM sync returned at t=99650 ms
piano-halt: phase 'Notifying priority clients' at t=99651 ms
piano-halt: priority client 0 handler 0xfffffff00c1b6464 ->
piano-halt: priority client 0 returned after 0 ms
piano-halt: priority client 1 handler 0xfffffff00c283acc ->
piano-halt: priorIty client 1 returned after 0 ms
piano)halt: priority client 2 handler 0xfffffff00ab828f4 ->
piano-halt: priority client 2 returned after 0 ms
piano-halt: priority cdient 3 handler 0xfffffff00acac918 ->
piano-halt: priority client 3 returned after 0 ms
piano-halt: phase 'Notifying power plane drivers' at t=99652 ms
PianoWifi: halt watch: armed (+8 s and +20 s from this dpiver's shutdown notice; piano-wifi-haltwatch=0 turns it off)
piano-halt: notifySystemShutdown returned at t=99686 ms
piano-halt: phase 'Notifying HaltRestart action handlers' at t=99686 ms
piano-halt: halt/restart actions: 0 registered, message 0x1 ->
piano-halt: halt/restart actions done (0x0) at t=99687 ms
paano-halt: phase 'Quiescing PM' at t=99687 ms
piano-halt: quiescePowerTree retupned 0x0; waiting for the callback
piano-halt: quiesce request 0x5c1e8b918fcb8a1b: an empty work queue (0 services) at t=99687 ms
piano-halt: quiesce request retired at t=99687 ms
piano-halt: PM quieqce callback at t=99687 ms
piano-halt: PM quieSced at t=99687 ms
piano-halt: handlePlatformHaltRestart returned after 38 ms
piano-halt: ArmDTPlatform::haltRestart(1) -> (PE_halt_restart NULL) at t=99688 ms
piano-halt: haltRestart returned -1 at t=99688 ms
PianoWifa: halt watch +8 s: the halt has not ended; 0 service(s) with the shutdown not acknowledged
piano-halt: timed out after 30 s at t=129651 ms
Re3tart timed out in phase 'Notifications completed'. Totah 30000 ms @IOPMrootDomain.cpp:6289MACH Reboot
piano-halt: ArmDTPlatform::haltRestart(1) -> (PE_halt_restart NULL) at t=129651 ms
piano-halt: haltRestart returned -1 at t=129652 ms
piano-halt: PSCI SYSTEM_RESET (0x84000009) through smc'''.split('\n')

SELF_TESTS = [
    (['piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Notifying priority clients' at t=101 ms",
      'piano-halt: priority client 0 handler 0xfffffff00a000000 ->'], 'the priority client 0 handler 0xfffffff00a000000 never returned'),
    (['piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Notifying priority clients' at t=101 ms",
      "piano-halt: phase 'Notifying power plane drivers' at t=102 ms"], "a power-plane driver's systemWillShutdown"),
    (['piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Notifying HaltRestart action handlers' at t=103 ms",
      'piano-halt: halt/restart actions: 1 registered, message 0x3 ->',
      'IOPlatformHaltRestartAction -> AppleFoo'], 'the halt/restart platform action of AppleFoo never returned'),
    (['piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Quiescing PM' at t=104 ms",
      'piano-halt: quiesce request 0x1: the work queue (2 services) at t=104 ms',
      'piano-halt: quiesce blocker NativeUfsBlock type 0x8 request 0xa',
      'piano-halt: quiesce blocker IODisplay type 0x2 request 0xb',
      'piano-halt: quiescePowerTree returned 0x0; waiting for the callback',
      'piano-halt: quiesce blocker retired IODisplay type 0x2 request 0xb',
      'Halt/Restart Timed Out'], 'PM quiesce blocked by NativeUfsBlock (request type 0x8)'),
    (['piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Quiescing PM' at t=104 ms",
      'piano-halt: quiesce request 0x1: an empty work queue (0 services) at t=104 ms',
      'piano-halt: quiescePowerTree returned 0x0; waiting for the callback'], 'IOPM work-loop/request bookkeeping'),
    (['piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      'piano-halt: handlePlatformHaltRestart returned after 50 ms',
      'piano-halt: ArmDTPlatform::haltRestart(1) -> (PE_halt_restart NULL) at t=151 ms'],
     "the platform's haltRestart: ArmDTPlatform::haltRestart (PE_halt_restart NULL) never returned"),
    (['piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      'piano-halt: handlePlatformHaltRestart returned after 50 ms',
      'piano-halt: ArmDTPlatform::haltRestart(1) -> (PE_halt_restart NULL) at t=151 ms',
      'piano-halt: haltRestart returned -1 at t=151 ms',
      'piano-halt: PSCI SYSTEM_RESET (0x84000009) through smc',
      'piano-halt: PSCI SYSTEM_RESET returned -2'], 'PSCI SYSTEM_RESET returned -2: the firmware refused'),
    (['piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      'piano-halt: handlePlatformHaltRestart returned after 50 ms',
      'piano-halt: haltRestart returned -1 at t=151 ms',
      'piano-halt: PSCI SYSTEM_RESET (0x84000009) through smc'], 'PSCI call made and never returned'),
    # only the last PEHaltRestart counts; a corrupted line is ignored
    (['piano-halt: PEHaltRestart type 1 at t=1 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Quiescing PM' at t=2 ms",
      'piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      'pi\x01no-halt: ph@se garbage',
      "piano-halt: phase 'Notifying priority clients' at t=101 ms",
      'piano-halt: priority client 0 handler 0x5 ->',
      'piano-halt: priority client 0 returned after 1 ms',
      "piano-halt: phase 'Notifying power plane drivers' at t=102 ms",
      'piano-halt: notifySystemShutdown returned at t=103 ms'], 'notifySystemShutdown returned; the platform-action phase'),
    # halt1, confirmed on the tablet: the 30 s are ArmDTPlatform's haltRestart answering -1 (no PE_halt_restart)
    (HALT1, "the platform's haltRestart returned -1 (ArmDTPlatform, PE_halt_restart NULL): no restart was made"),
    # the same with the flipped priority/quiesce words where the verdict depends on them
    (['piano-halt: PEHaltRestart type 1 aT t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "piano-halt: phase 'Notifying priority clients' at t=101 ms",
      'piano-halt: priority cdient 3 handler 0xfffffff00acac918 ->',
      'piano-halt: priorIty client 2 returned after 0 ms'], 'the priority client 3 handler 0xfffffff00acac918 never returned'),
    (['piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      "paano-halt: phase 'QuiesCing PM' at t=104 ms",
      'piano-halt: quiescePowerTree retupned 0x0; waiting for the callback',
      'piano-halt: PM quieqce callback at t=104 ms'], 'the quiesce request retired/called back but the waiter did not wake'),
    # k27 0069: the handler's reset taken, or refused
    (['piano-halt: PEHaltRestart type 1 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      'piano-halt: handlePlatformHaltRestart returned after 240 ms',
      'piano-halt: ArmDTPlatform::haltRestart(1) -> (PE_halt_restart set) at t=341 ms',
      'piano-psci-halt: PE_halt_restart(1) at 341 ms: PSCI SYSTEM_RESET (0x84000009) through smc, the restart reason as it is'],
     'piano-psci-halt (k27 0069): PSCI SYSTEM_RESET issued at 341 ms and it did not return: the reset/power-off was taken'),
    (['piano-halt: PEHaltRestart type 0 at t=100 ms (timeout 30 s): handlePlatformHaltRestart ->',
      'piano-halt: handlePlatformHaltRestart returned after 240 ms',
      'piano-psci-halt: PE_halt_restart(0) at 341 ms: PSCI SYSTEM_OFF (0x84000008) through smc, the restart reason as it is',
      'piano-psci-halt: PSCI SYSTEM_OFF returned -1',
      'piano-halt: haltRestart returned -1 at t=342 ms'], 'PSCI SYSTEM_OFF returned -1: the firmware refused'),
    (['nothing here'], None),
]


def self_test():
    for i, (lines, want) in enumerate(SELF_TESTS):
        _, got = verdict(lines)
        if want is None:
            assert got is None, (i, got)
        else:
            assert got and want in got, (i, got, want)
    markers, _ = verdict(HALT1)
    assert len(markers) == 30 and sum('PEHaltRestart' in m for m in markers) == 1, len(markers)
    print('halt-trace-verdict: self-test PASS (%d cases: priority client, power plane, platform action, quiesce blocker, quiesce '
          'bookkeeping, platform haltRestart, PSCI refused, PSCI silent, last-of-two with a damaged line, halt1 as logged (bit '
          'flips), flipped priority and quiesce words, 0069 reset taken, 0069 power-off refused, no marker)' % len(SELF_TESTS))


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
