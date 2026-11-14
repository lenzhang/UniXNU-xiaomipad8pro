# Halt and restart: the 30 s "Halt/Restart Timed Out"

Status (2026-11-14): every restart or shutdown of the tablet ended in `Halt/Restart Timed Out @IOPlatformExpert.cpp:884` after
30 s, then the panic's own reboot (`MACH Reboot`). The cause of the USB console's reset is **confirmed on the tablet** (boot 10p,
`PianoDwc3: reset: PEHaltRestart returned -1 after 242 ms`): the shutdown notifications finish in a quarter of a second and the
platform has no restart, so nothing resets until the timer panics. halt1 (`piano-halt-trace=1`, confirmed on the tablet):
`handlePlatformHaltRestart` returned after 38 ms (4 priority clients at 0 ms, no halt/restart action, an empty PM quiesce), then
`ArmDTPlatform::haltRestart(1) -> (PE_halt_restart NULL)` and `haltRestart returned -1`, nothing until `timed out after 30 s`; the
panic's restart then ended in `PSCI SYSTEM_RESET (0x84000009) through smc`. Fix: k27 0069 `piano-psci-halt` (default on, section below),
with the boot guard's restart-reason fix; probe `ram-k273-halt2` (`private/host-checks/20261114-halt/CHECKLIST-halt2.txt`). Tracing:
k27 0068 `piano-halt-trace=1`, probe `ram-k273-halt1`.

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

## k27 0069: `piano-psci-halt`, the restart and the power-off through PSCI

Why -1 (source, and the 10p line): the platform expert is `ArmDTPlatform`, an `IODTPlatformExpert` with no `haltRestart` of its
own; `IOPlatformExpert::haltRestart()` (`IOPlatformExpert.cpp:339-368`) answers `kPEPanicSync` and `kPEHangCPU` itself and
otherwise calls the function pointer `PE_halt_restart` if it is set, else returns -1. Nothing sets it here: Apple's platform
expert of `AppleARMPlatform` does, and it is not in the native KC; ARMPlatformStandIn (our stand-in for its classes) has no
platform expert. So `PEHaltRestart(kPERestartCPU)` and `PEHaltRestart(kPEHaltCPU)` both return -1 after the notifications:

- PianoDwc3's `reset` / `reset fastboot` (a thread calling `PEHaltRestart(kPERestartCPU)`) returned, the system ran on, and the
  30 s timer panicked; the panic restart then went through `halt_all_cpus()`'s PSCI `SYSTEM_RESET`.
- `shutdown -r` / `-h` (`reboot_kernel` -> `halt_all_cpus()`) got the -1 too, and `halt_all_cpus()` issues its own PSCI call right
  after `PEHaltRestart` returns; whether that path also waits out the timer is what halt1/halt2 show (inferred: it does not).

The fix, in the kernel next to the boot guard (`osfmk/arm64/machine_routines.c`; a kernel patch because the guard and the PSCI
conduit live there and ArmDTPlatform is not our code):

| | |
|---|---|
| `piano-psci-halt=1` (default) | at EARLY_BOOT, natively (the SMC conduit) and only when `PE_halt_restart` is unset, it becomes a handler: `kPERestartCPU` -> PSCI `SYSTEM_RESET` (0x84000009), `kPEHaltCPU` -> PSCI `SYSTEM_OFF` (0x84000008), through the conduit of `piano_psci_reset` (the resets of `piano-reset-after` and the guard). The PMIC restart reason is not touched: the one in place (the guard's, `piano-reset-to`'s, PianoDwc3's) decides where ABL goes. Every other type answers -1 as before (`kPEPanicRestartCPU` among them); the panic restart's `halt_all_cpus(TRUE)` then gets the same `SYSTEM_RESET` from the handler, the call `halt_all_cpus()` would make next |
| `piano-psci-halt=0` | off: `PE_halt_restart` stays unset, exactly the course before (the 30 s timeout) |
| `sysctl machdep.piano_psci_halt=0/1` | the same switch at run time (root); reads 1 while the handler is in force |
| `sysctl machdep.piano_restart_started` | 1 once a restart or power-off has begun (below); writing 1 marks it |

Lines: `piano-psci-halt: on: a restart is PSCI SYSTEM_RESET, a power-off PSCI SYSTEM_OFF, through smc (piano-psci-halt=0: off)` at
boot; at a restart `piano-psci-halt: PE_halt_restart(1) at N ms: PSCI SYSTEM_RESET (0x84000009) through smc, the restart reason as
it is`, and only if the firmware refuses, `piano-psci-halt: PSCI SYSTEM_RESET returned R` (PEHaltRestart then returns R and the
caller goes on as before).

**The boot guard's restart reason** (`boot-guard.md` §3): on 2026-11-14 a `reset fastboot` sent at 170 s of an installed boot set
bootloader, the restart hung in the 30 s timeout, the guard's health decision at 180 s set the reason back to normal during the
hang, and the panic restart booted the installed system (`private/host-checks/20261114-a2run/WHY.md`). Since 0069 a restart word
(`osfmk/arm64/piano_halt_policy.h` = `tooling/k27/piano-halt/piano_halt_policy.h`, host-tested by `tooling/k27/test-piano-halt.sh`,
a two-thread race included) is marked at `PEHaltRestart`'s entry for the halt/restart types and by
`machdep.piano_restart_started=1`, which PianoDwc3's `reset` writes **before** its own reason (`kernel_sysctlbyname`; an older
kernel answers ENOENT and PianoDwc3 says so). Once marked, the guard's decision does not write the reason
(`piano-boot-guard: healthy after 180 s, but a restart began at N ms (machdep.piano_restart_started): the restart reason is left as it
is`); a marker that writes a reason of its own waits (200 ms at most) for a guard write already in flight, so its write comes last.

The callers whose outcome changes (every other caller is `halt_all_cpus()`, which made the same PSCI call right after): PianoDwc3's
`reset`, and IOPMrootDomain's sleep/wake watchdog (`takeStackshot`: with `swd_panic=0` a restart; after a repeated failure a
power-off). Those are Apple's intended outcomes; the power-off needs the previous boot's `SleepWakeFailurePanic` NVRAM key, which
this NVRAM does not keep (no NVRAM controller, inferred).

What 0069 does not do: the console `reset` still restarts without a file-system sync (as before: it calls `PEHaltRestart` directly;
`shutdown -r` over ssh syncs and unmounts first). `SYSTEM_OFF` was not run on the tablet before (inferred to power it off like the
panic path's PSCI calls work; an unattended test cannot press the power key afterwards, so the halt2 checklist leaves it out).

Source walk and the probe's design: `private/host-checks/20261114-halt/REPORT.md` (local). Patches: `patches/k27/0068-*`, `0069-*`.
