# Halt and restart: the 30 s "Halt/Restart Timed Out"

Status (2026-11-14): every restart or shutdown of the tablet ends in `Halt/Restart Timed Out @IOPlatformExpert.cpp:884` after
30 s, then the panic's own reboot (`MACH Reboot`). The cause is not known yet; k27 patch 0068 (`piano-halt-trace=1`) names it in
one boot. Probe: `ram-k273-halt1` (`private/host-checks/20261114-halt/CHECKLIST-halt1.txt`).

## What times out

All sources are the k27 tree's xnu (xnu-12377 + the k27 series), `$PIANO_K27_TREE/out/k27/xnu`.

- `PEHaltRestartInternal()` (`iokit/Kernel/IOPlatformExpert.cpp`, ~998) arms a 30 s thread call
  (`IOShutdownNotificationsTimedOut`; the ADT's `/defaults halt-restart-timeout`, or `halt_restart_timeout` on a development
  kernel) and runs `IOPMrootDomain::handlePlatformHaltRestart()` synchronously; only after it returns does it call the platform's
  `haltRestart()`, and `halt_all_cpus()` (`osfmk/arm64/machine_routines.c`) then issues PSCI `SYSTEM_RESET`/`SYSTEM_OFF` (smc on the
  tablet) and spins. The timer is never cancelled: a halt that has not reset the CPU within 30 s panics.
- On ARM the timeout only panics (`panic("Halt/Restart Timed Out")`, line 884); x86 calls `panicWithShutdownLog()`, which names the
  phase. That is why the tablet's panic says nothing more.
- `handlePlatformHaltRestart()` (`iokit/Kernel/IOPMrootDomain.cpp`, ~6112) runs, in order: `PESyncNVRAM()`; the priority clients
  (`gIOPriorityPowerStateInterest`, each handler inline); for a halt the File Server Mode setting; the power-plane drivers'
  `systemWillShutdown()` (`notifySystemShutdown`, each acknowledged through `PMShutdown`); the halt/restart platform actions
  (`IOCPURunPlatformHaltRestartActions`, each a synchronous `callPlatformFunction`); the PM quiesce (`quiescePowerTree` on the root,
  then an unbounded `IOLockSleep` until its callback). None of these waits has its own deadline.
- What is already excluded (PianoWifi's halt watch, `drivers/wifi.md`): at +8 s and +20 s no service holds `PMShutdown` (no
  `systemWillShutdown()` left unacknowledged, which covers the four project overrides: PianoPower, PianoWifi, PianoBluetooth,
  PianoTorch) and none of 15 power-plane services has a power change in progress. That leaves a priority client, the halt workers'
  coordination after the acknowledgements, a halt/restart platform action (no project kext registers one; an Apple class could),
  a request ahead of the PM quiesce (a queued request need not show as a power change), the platform's `haltRestart`, or PSCI.

## k27 0068: `piano-halt-trace=1`

Default 0: nothing changes. With it, `piano-halt: ...` lines on the console (the pstore mailbox keeps them):

| Marker | Where |
|---|---|
| `PEHaltRestart type T at t=MS ms (timeout S s): handlePlatformHaltRestart ->`, `handlePlatformHaltRestart returned after MS ms` | `PEHaltRestartInternal` |
| `Restart: NVRAM sync ->`, `NVRAM sync returned at t=...` | `handlePlatformHaltRestart` |
| `phase 'P' at t=...` for each of the four phases | `handlePlatformHaltRestart` |
| `priority client N handler H ->`, `priority client N returned after MS ms` | `platformHaltRestartApplier` |
| `notifySystemShutdown returned at t=...` | `handlePlatformHaltRestart` |
| `halt/restart actions: N registered, message M ->`, `IOPlatformHaltRestartAction <- SERVICE returned R`, `halt/restart actions done` | `IOPlatformActions.cpp` (the kernel logs each entry, `IOPlatformHaltRestartAction -> SERVICE`) |
| `quiescePowerTree returned R; waiting for the callback` | `handlePlatformHaltRestart` |
| `quiesce request Q: the work queue (N services)`, `quiesce blocker [(late)] SERVICE type T request R`, `quiesce blocker retired SERVICE type T request R`, `quiesce request retired` | `IOServicePM.cpp` (`attachQuiesceRequest`, `queuePMRequest`, `checkRequestQueue`) |
| `PM quiesce callback`, `PM quiesced` | `IOPMrootDomain.cpp` |
| `CLASS::haltRestart(T) -> (PE_halt_restart set/NULL)`, `haltRestart returned R` | `PEHaltRestartInternal` |
| `PSCI SYSTEM_RESET (0x84000009) through smc`, `PSCI SYSTEM_RESET returned R` (signed) | `halt_all_cpus` |
| `timed out after 30 s at t=...`, then `Restart timed out in phase 'P'. Total 30000 ms` | the 30 s thread call (`panicWithShutdownLog`) |

`tooling/native/halt-trace-verdict.py MAILBOX` reads the last halt in a log and prints the verdict (`--self-test` for its cases).

## Reading one boot

| Last relevant evidence | Waiter |
|---|---|
| phase 'Notifying priority clients', a client `->` without its return | that priority client's handler (the address names it) |
| phase 'Notifying power plane drivers', no `notifySystemShutdown returned` | a `systemWillShutdown()` or the halt workers (with `PMShutdown` already zero: the coordination) |
| phase 'Notifying HaltRestart action handlers', `IOPlatformHaltRestartAction -> X` without `<- X` | X's `callPlatformFunction` |
| phase 'Quiescing PM', a blocker X/type T attached, not retired | X's IOPM request of type T, and what it waits for |
| phase 'Quiescing PM', no blocker open, no callback | IOPM work-loop/request bookkeeping, not a driver's power change |
| `handlePlatformHaltRestart returned`, `haltRestart ->` without its return | the platform expert's `haltRestart` (`PE_halt_restart` or the class's own) |
| PSCI marker, then `PSCI ... returned R` | the firmware refused (R a PSCI error); not a driver |
| PSCI marker and nothing after | the SMC did not return and did not reset: the conduit/firmware path |

Source walk and the probe's design: `private/host-checks/20261114-halt/REPORT.md` (local). Patch: `patches/k27/0068-*`.
