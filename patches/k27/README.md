# k27 patches for native piano

k27 = the k27 tree: xnu-12377.121.6 + the baseline series 0001-0052 (XNU 27.0.1, first brought up under a hypervisor). These
patches on top make the same kernel boot natively on piano at EL1 (each inert unless its boot argument is set):

| Patch | What |
| --- | --- |
| 0053 | One kernel for the VM and the HLOS: the PSCI conduit comes from the ADT's `/psci` method; `XNU_VM_GUEST=0` (K27_VARIANT=native) only changes the fallback to SMC |
| 0054 | SM8750 MPIDRs: Affinity2 in the reset handler's and `ml_get_cpu_number`'s CPU lookups (prime cores 0x10000/0x10100) |
| 0055 | `pstore-console=<phys>`: the console mirrored into a RAM mailbox that survives a reset (11 copies, majority on read; `tooling/native/xnu-log.py`) |
| 0056 | `piano-reset-after=<s>`: PSCI reset after N seconds, the Gunyah watchdog (32 s bite) as the backstop |
| 0057 | `piano-reset-to=bootloader`: the PMIC SDAM restart reason (SPMI arbiter v7, SID 0 byte 0x48 = 2) set at boot, so every reset lands in ABL fastboot |
| 0058 | pstore-console header stored 11 times too ("DBGR": signature, then 11 {start, size} pairs; the reader takes their majority): one decayed bit of the single size word had cut the log's tail |
| 0059 | On `!BSD` (IOBSD matching not finished after 30 s), every busy IOService with its clients and their state goes to the console: who holds the root up |
| 0060 | `piano-rtc=1` (`piano-rtc-offset=N`): the calendar starts from the PMIC RTC (PMK8550 RTC_RW, SID 0 PPID 0x061, read through the SPMI observer channel) plus the offset, else the SDAM offset cell 0x72; read only, `PEGetUTCTimeOfDay` asks it when ArmDTPlatform answers 0 (`docs/drivers/rtc.md`). Adds `osfmk/arm64/piano_rtc.h` = `package/PianoPower/PmicRtc.h` |

| 0061 | `piano-cpuidle=N`: cpu_idle's WFI becomes a PSCI CPU_SUSPEND vote when the next timer leaves its min-residency (1 ret, 2 + cluster cl5, 3 + CPU subsystem ss3; OS-initiated mode for the domain states, last-man bookkeeping; ss3 only with the LLCC island slice reserved), exit-latency holds (`piano_cpuidle_latency_hold`), per-CPU counters (`machdep.piano_cpuidle`, a console summary every 60 s with the AOP's RPMh sleep counters), a PSCI/RSC/RPMh/LLCC probe (`piano-pm-probe=1`). Adds `osfmk/arm64/piano_pm.c`, `piano_pm.h` and `piano_pm_policy.h` = `tooling/k27/piano-pm/piano_pm_policy.h` (host-tested: `tooling/k27/test-piano-pm.sh`) (`docs/power.md` §2) |
| 0062 | `piano-sleep=1`: macOS's system sleep as suspend-to-idle: IOCPUSleepKernel parks the other CPUs in PSCI states, freezes them, and the boot CPU idles polling the wake sources (power key, cover, keyboard, USB-C, a timer); mach_absolute_time stops across it, the calendar is advanced; full wake with the reason claimed; the Gunyah watchdog across the whole path; the ADT's sleep-disabled overridden only with the argument; `piano-sleep-test` for unattended cycles (`docs/power.md` §3) |
| 0063 | `piano-boot-guard=SECS`: the installed system's boot guarded. At EARLY_BOOT the restart reason bootloader and the Gunyah watchdog held (32 s bite), pinged every 10 s by a thread call that also checks every CPU still takes interrupts (a nop IPI each; one quiet for two ticks: PSCI reset); at SECS healthy when the exec of `piano-boot-guard-exec` (launchd) was seen: the reason back to normal, the pings go on, so a later hang restarts macOS; else a reset into ABL fastboot. Off with `piano-reset-after` (0056 keeps both); `piano-reset-to=bootloader` keeps the reason. The watchdog gets one owner (`piano_watchdog_hold/release/ping`; 0056, the guard and 0062 are holders, so 0062's wake window no longer turns it off under the guard). Tests `piano-boot-guard-ping=0`, `piano-boot-guard-test=hang-at=N`; `machdep.piano_boot_guard`. Adds `osfmk/arm64/piano_guard_policy.h` = `tooling/k27/piano-guard/piano_guard_policy.h` (host-tested: `tooling/k27/test-piano-guard.sh`) (`docs/boot-guard.md`) |

Build: `K27_VARIANT=native "$PIANO_K27_TREE/build-k27-piano.sh"` in the k27 tree with these patches applied to its `out/k27/xnu`
(see `docs/bringup-debugging.md` §4).
