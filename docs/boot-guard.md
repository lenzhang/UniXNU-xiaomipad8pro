# Boot guard (k27 0063, `piano-boot-guard=SECS`)

Status 2026-11-12: **G1-G4 ran on the tablet and behaved as designed** (no pings, a hang and no healthy exec each returned to ABL
fastboot; a healthy boot was judged healthy at 60 s, restart reason normal, the watchdog fed 6 times, longest gap 10.0 s). G5 (the
late hang into the installed system) and G6 (sleep) have not run. Off without its argument; `tooling/persist/build-esp.py
--profile daily` adds `piano-boot-guard=180`: ESP v3 (`persistent-boot.md` §11) is the first installed system with it.

Facts are marked **verified** (read from source, a binary, the running system or a run's log), **spec** (Gunyah or Linux read as
the specification only; nothing copied), **inferred** or **needs-boot**.

## 1. Why

The installed system boots from the ESP without `piano-reset-after`, and nothing else arms the Gunyah watchdog. On 2026-11-11 a
hang during a GPU boot (one boot in three; the console showed stage S1, which with the asynchronous trace is the logger's
backlog rather than the hang: `forensics-2026-11-11.md` §2) left the tablet stuck on the console until someone held power +
volume-down for 15-20 s. The RAM test images never had that problem: 0056 pings the watchdog and resets on time, and 0057 sends
every reset to ABL fastboot. The boot guard gives the installed system the same backstop, but with an end: once the boot is
healthy the restart reason goes back to normal, so a later hang restarts macOS rather than parking the tablet in fastboot.

## 2. Facts

| Fact | State | Source |
|---|---|---|
| The hypervisor watchdog is a Gunyah SMCCC vendor-hypervisor service: CONTROL 0x86000005 (3 enable, 2 disable), STATUS 0x86000006, PING 0x86000007, SET_TIME 0x86000008 (bark, bite in ms, at most 0x8000) | verified (source) | `el1-hyp-notes.md` §1 |
| It is not armed when the HLOS starts | inferred | `el1-hyp-notes.md` §1; every installed boot without `piano-reset-after` that hung stayed hung |
| A bite resets the tablet: a secure-world stall during a test with 0056 armed ended in a reset about 30 s later, and so did the evening's "power1" stall in a RAM test (first read as a GPU_CC read at S1.7; the bring-up had in fact returned about 1 s before the system stopped, and the reset's timing fits a bite only if a ping 3 s before the bring-up already failed: `forensics-2026-11-11.md` §2.3): the whole system stopped, 0056's pings included | verified (runs) | `drivers/charging.md` §6; `results-2026-11-11.md` |
| The restart reason is bits 1-7 of byte 0x48 of the SID 0 SDAM (PPID 0x071), 2 = bootloader; after 0057 wrote 2, every PSCI reset of a RAM test landed in ABL fastboot | verified (runs) | k27 0057; `bringup-debugging.md` §3 |
| That the byte survives a watchdog bite (not a PSCI reset) and ABL honours it then | **needs-boot** | §7 test `ping0` |
| `fastboot reboot` from ABL fastboot boots the installed system | verified (a run) | `persistent-boot.md` (session of 09:05) |
| XNU counts every IRQ and FIQ a CPU takes in `cpu_stat.irq_ex_cnt` (the shared interrupt prologue in `locore.s`); `cpu_signal(SIGPnop)` is a bare IPI | verified (source) | `osfmk/arm64/locore.s` BEGIN_INTERRUPT_HANDLER, `osfmk/arm/cpu_common.c` |
| launchd's exec comes before 45 s with the verbose console (its `hello` at 47.8-50.2 s, up to 67.5 s with several USB drivers), the login window at about 90-110 s, the login screen at about 150 s | verified (logs) | `boot-time.md` §2, §3.1 |
| `mach_absolute_time()` does not advance across k27 0062's suspend: the guard counts awake time | verified (source) | `power.md` §3.4 |

## 3. Design

**One owner of the watchdog** (`machine_routines.c`, `piano_watchdog_hold/release/ping/enabled`). Before 0063 three pieces
programmed the watchdog on their own: 0056 for a test run, 0062 around a suspend, and now the guard. 0062 turned it off 240 s
after a wake unless `piano-reset-after` was set, which would have silently ended the guard. Now each is a holder (0056 and the
guard for the whole boot, 0062 from a suspend's entry to the end of its wake window). The first holder enables the watchdog
(STATUS, SET_TIME bark 30 s / bite 32 s, CONTROL 3); the last one out disables it. Anyone may ping it. 0062's suspend loop pings
it every 5 s whoever holds it, since every thread call, the guard's included, is frozen during a suspend.

**The guard** (`piano-boot-guard=SECS`, `STARTUP(EARLY_BOOT)`):

1. At EARLY_BOOT: the restart reason becomes bootloader (2) and the guard takes its hold on the watchdog (32 s bite). A thread
   call (kernel-high priority) ticks every 10 s and pings.
2. Each tick also checks the CPUs. Every other running CPU (processor state idle or running, cpu_data started) gets a nop IPI,
   and its interrupt count must have moved by the next tick. A CPU that took no interrupt across two ticks in a row is stuck:
   interrupts masked forever, or a load stalled on an unclocked bus, which is what a bring-up hang on one CPU looks like while
   the others still run. The guard then stops pinging and resets through PSCI (if PSCI refuses, the unpinged watchdog bites).
   A whole-system hang needs no check: there are no more ticks, so the watchdog bites 32 s after the last ping.
3. At SECS of uptime the health decision runs, in the same thread call, so it also says that the pinger still runs. The boot is
   **healthy** when the exec of the milestone process has been seen by then: `launchd` unless `piano-boot-guard-exec=` names
   another, or `none` for the uptime alone. A kernel-visible event: every exec's `p_comm` is compared until the first match,
   next to `exec-log`. Healthy: the restart reason goes back to normal (0), and the pings and CPU checks go on for the rest of
   the boot. **Not healthy** (no such exec by SECS, such as a root that never mounted): a PSCI reset into ABL fastboot, because a
   boot that never reached userland should not sit there.
4. Deferral: with `piano-reset-after=N` (N > 0) the guard stays off, since that run owns the watchdog and the restart reason as
   before. With `piano-reset-to=bootloader` alone the guard runs but leaves the reason at bootloader when healthy, so the
   argument's promise (every reset into ABL) holds.
5. A restart that has begun keeps its reason (k27 0069, `halt.md`): once `PEHaltRestart` was entered for a restart or
   power-off, or PianoDwc3's `reset` wrote `machdep.piano_restart_started=1` (it does so before its own reason), the health
   decision does not write the reason. Before 0069 a `reset fastboot` sent at 170 s set bootloader, the restart hung for its
   30 s, the decision at 180 s set normal during the hang and the panic restart booted the installed system
   (2026-11-14). With `piano-psci-halt` (0069, default on) the restart no longer hangs, so the window was the 0.25 s of the
   shutdown notifications; the word closes it too (`tooling/k27/test-piano-halt.sh`).

Why launchd and not a later process: the decision must never reset a sound boot into fastboot, which only the Mac can undo, and
launchd's exec comes well before 180 s in every boot logged. The CPU check covers what the milestone does not: a bring-up hang
on one CPU while launchd goes on. `piano-boot-guard-exec=loginwindow` is a stricter choice for a test (the login window at about
90-110 s).

What each event lands in:

| Event | Before the decision (reason bootloader) | After healthy (reason normal) |
|---|---|---|
| Whole-system hang (no tick) | bite within 32 s of the last ping → ABL fastboot | bite → the installed system boots again |
| One CPU stuck (masked or stalled) | 2 ticks, then a PSCI reset → ABL fastboot | the same → the installed system |
| Panic (restarts: no `panic-spin`) | → ABL fastboot | → the installed system |
| No milestone by SECS | PSCI reset → ABL fastboot | — |
| A restart from macOS, or the USB console's `reset` | → ABL fastboot (`fastboot reboot` goes on to macOS) | → the installed system |
| The USB console's `reset fastboot` | → ABL fastboot (since 0069 also when it falls on the decision: the decision leaves the reason) | → ABL fastboot (PianoDwc3 writes 2 itself) |
| A userland hang with the kernel well (ticks on time, every CPU takes interrupts, launchd ran) | not caught | not caught |

## 4. Boot arguments

| Argument | Default | Meaning |
|---|---|---|
| `piano-boot-guard=SECS` | off | the guard; SECS is the uptime of the health decision. The daily ESP profile: 180 (`build-esp.py --boot-guard`, 0 for none; under 60 refused there) |
| `piano-boot-guard-exec=NAME` | `launchd` | the milestone: the first exec whose process name (`p_comm`, its first 16 characters) is NAME; `none`: the uptime alone |
| `piano-boot-guard-cpus=0` | 1 | skip the per-CPU interrupt check |
| `piano-boot-guard-ping=0` | 1 | test: arm and never ping (the bite about 32 s later) |
| `piano-boot-guard-test=hang-at=N` | — | test: at N s of uptime every CPU masks interrupts and spins (`cpu_broadcast_xcall`), refused while the watchdog is off |
| `piano-reset-after=N` (0056) | — | N > 0: the guard stays off (`deferred`) |
| `piano-reset-to=bootloader` (0057) | — | without `piano-reset-after`: the guard keeps the reason at bootloader when healthy |
| `piano-sleep-wdt=0` (0062) | 1 | 0062 takes no hold of its own; its loop still pings whatever another holder armed |

## 5. Lines and `sysctl machdep.piano_boot_guard`

```
piano-boot-guard: SDAM restart reason 0x00 -> 0x04 (APID n)
piano-boot-guard: armed: restart reason bootloader until healthy, Gunyah watchdog armed (32 s bite), pinged every 10 s, CPU check on; health at 180 s: the exec of launchd, else a PSCI reset into ABL fastboot
piano-boot-guard: exec of launchd at 31 s (the milestone)
piano-boot-guard: SDAM restart reason 0x04 -> 0x00 (APID n)
piano-boot-guard: healthy after 180 s, restart reason normal
piano-boot-guard: healthy; restart reason normal; health at 180 s (decided at 180 s): exec of launchd at 31 s; watchdog on (32 s bite), holders guard; 18 pings, longest gap 10.0 s
```

The last line is also what `sysctl machdep.piano_boot_guard` prints at any time (`armed; ...` before the decision, `holders guard
sleep` inside a wake window of 0062, `deferred: piano-reset-after=N owns ...` in a test run, `off` without the argument). The
failures:

```
piano-boot-guard: not healthy after 60 s (no exec of piano-nosuch yet): PSCI reset into ABL fastboot
piano-boot-guard: CPU 3 took no interrupt across 2 ticks (a nop IPI each): hung; no more pings, PSCI reset into ABL fastboot
piano-boot-guard: test: no pings (piano-boot-guard-ping=0): the watchdog bites about 32 s from now, into ABL fastboot
piano-boot-guard: test: every CPU stops now with interrupts masked (piano-boot-guard-test=hang-at=40): the watchdog resets into ABL fastboot within 32 s
```

A restart begun before the decision (k27 0069; the first line only while the guard is armed):

```
piano-boot-guard: a restart began at 171234 ms (machdep.piano_restart_started): the health decision will leave the restart reason alone
piano-boot-guard: healthy after 180 s, but a restart began at 171234 ms (machdep.piano_restart_started): the restart reason is left as it is
```

0062's wake window now ends with `piano-sleep: the wake went through: the Gunyah watchdog stays on (held by piano-reset-after or
piano-boot-guard)` when another holder keeps it.

## 6. Build

```sh
tooling/k273/prepare-k27-tree.sh 0063                             # current 27.3 kernel and tree
tooling/k27/test-piano-guard.sh                                   # host tests (the policy header, and the patch carries it)
python3 tooling/persist/test-data-store.py                        # build-esp.py's --boot-guard among its checks
K273_KERNEL=<prepared kernel-k273-0063-...-native.macho> PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k273/build-k273-image.sh
tooling/k27/build-guard-test-images.sh                            # out/ram-guard-{ping0,hang,nohealth,healthy,latehang,sleep}
```

`build-esp.py --profile daily` refuses a collection whose kernel lacks 0063 (unless `--boot-guard 0`).

Built offline on 2026-11-11 from an APFS clone of the k27 tree with its own object directory: kernel sha256 `8e945149325a0272…`;
the KC with the 21 kexts of the list `2af25c3377b8ac8f…` (layout check passed; Turnip's staged seed and trust cache taken as they
were, `PIANO_A830_METAL=0`); images `ram-guard-ping0` `705ccd9a75814526…`, `-hang` `3d6631ae84e9bb89…`, `-nohealth`
`c5b26888f56a561c…`, `-healthy` `66f609bb57f1227a…`, `-latehang` `9616d20a9a7884ee…`, `-sleep` `c44f6407a0b856f6…`; a daily ESP
from that KC carries `piano-boot-guard=180` on macos and macos-diag, none on fastboot. Rebuild in the main checkout before a run.

## 7. RAM test procedure

Every image is a RAM boot of the GPU profile with the USB console (`piano-usb=2`), `PIANO_FAST_BOOT=1`, Data volatile
(`data-persist=0`), and **neither `piano-reset-after` nor `piano-reset-to`** (`PIANO_RESET_AFTER=0`): the guard is the only thing
that resets the tablet, as on the installed system. Start each from ABL fastboot: `fastboot boot out/ram-guard-<name>/boot-piano-
macos-ram-A-bundled.img`, with `python3 tooling/native/usb-console.py --capture 300` running (the log also stays in the pstore
mailbox: `tooling/native/native-loop.sh` with `out/ram-guard-dump`). Order of risk:

| # | Image | Arguments (besides the profile) | Pass | On failure |
|---|---|---|---|---|
| G1 | `ram-guard-ping0` | `piano-boot-guard=60 piano-boot-guard-ping=0` | the `armed` and `test: no pings` lines, then **back in ABL fastboot** about 32-40 s after XNU started, by itself | the tablet boots the installed system instead: the byte did not survive the bite or ABL ignores it after one: keep the guard off the ESP and report; stuck on the console: the watchdog did not bite (STATUS/SET_TIME refused: the `armed` line says "not available") |
| G2 | `ram-guard-hang` | `piano-boot-guard=60 piano-boot-guard-test=hang-at=40` | `test: every CPU stops now ...` at 40 s, nothing after it, **ABL fastboot** by about 75 s | as G1 |
| G3 | `ram-guard-nohealth` | `piano-boot-guard=60 piano-boot-guard-exec=piano-nosuch` | `not healthy after 60 s (no exec of piano-nosuch yet): PSCI reset into ABL fastboot`, ABL fastboot within a few seconds | — |
| G4 | `ram-guard-healthy` | `piano-boot-guard=60` | `exec of launchd at N s`, then **`piano-boot-guard: healthy after 60 s, restart reason normal`**, the state line `healthy; restart reason normal; ...` (the text of `sysctl machdep.piano_boot_guard`; a shell needs `GUARD_EXTRA=piano-usb-shell=1` and the root image's helper, `usb-device-debug.md`), the login screen as usual. Then `reset fastboot` on the console | `not healthy after 60 s`: launchd came later than 60 s in this boot (the line's time says when): G4 with `piano-boot-guard=90` |
| G5 | `ram-guard-latehang` | `piano-boot-guard=60 piano-boot-guard-test=hang-at=100` | `healthy after 60 s, restart reason normal`, the hang at 100 s, and then the **installed system** boots (not fastboot): the normal reason survived the bite | ABL fastboot instead: the write of 0 did not take (the `SDAM restart reason 0x04 -> 0x00` line), or ABL keeps a reason across boots |
| G6 | `ram-guard-sleep` (after `power.md` S1 passed) | `piano-boot-guard=60 piano-sleep=1 piano-sleep-state=1 piano-sleep-test=90 piano-sleep-test-count=3 piano-sleep-max=30` | three `woke after 30.0 s by timer`; 240 s after the last wake `the Gunyah watchdog stays on (held by ...)`; the sysctl line keeps `watchdog on`, its pings count on; no reset. Then `reset fastboot` | a reset during a suspend: 0062's loop did not ping (report the last mailbox lines) |

After G5 the installed system is running; `python3 tooling/native/usb-console.py --send 'reset fastboot' --capture 5` takes it
back to ABL for the next image. The installed ESP gets the guard only after G1, G2, G4 and G5 passed (then `build-esp.py
--profile daily` as usual: the default is 180 s).

## 8. Risks and limits

- **The bite path is not proven on this tablet** with the SDAM reason: G1 settles it. If ABL treats a watchdog reset differently
  (a crash-dump mode, or the reason cleared), the guard still ends the hang, but the tablet may come back in the installed system
  or in a dump screen instead of fastboot.
- **A restart within the window lands in ABL fastboot**, including a deliberate one from macOS or the USB console's `reset` in the
  first SECS seconds (180 s: about half a minute after the login screen). `fastboot reboot` goes on. Telling a deliberate restart
  from a panic in the reboot path would need the USB console's `reset fastboot` to say its intent; not done.
- **A boot loop after health** (a hang that comes every time after 180 s) restarts macOS over and over, as a panic loop does today;
  power + volume-down still ends it.
- **A userland hang with a healthy kernel** (WindowServer waiting forever on a stuck driver thread while every CPU takes
  interrupts) is not caught: nothing kernel-visible says the desktop came up. A kext-set milestone (PianoA830's bring-up done,
  NativeGopDisplay's first frame) is the next step if such hangs show up.
- The CPU check reads one counter per CPU and sends one IPI each every 10 s (about 0.7 wake-ups a second in all). A CPU masked
  for more than two ticks in a sound system would be reset as stuck; nothing in XNU does that on purpose (0062's frozen CPUs run
  no tick at all). `piano-boot-guard-cpus=0` turns the check off.
- The SDAM byte is also written by PianoDwc3's `reset fastboot`; a guard decision in the same 200 ms as that command could undo
  it (one reset into macOS instead of fastboot).
- The watchdog's bark (30 s) is not handled by XNU, as before.

## 9. Files

`patches/k27/0063-*.patch` (`osfmk/arm64/machine_routines.c`: the watchdog owner, 0056/0057 as holders, the guard;
`osfmk/arm64/piano_guard_policy.h`; `piano_pm.c`/`piano_pm.h`: 0062 as a holder; `bsd/kern/kern_exec.c`: the milestone hook;
`bsd/dev/arm64/sysctl.c`: `machdep.piano_boot_guard`), `tooling/k27/piano-guard/` (the policy header and its host test),
`tooling/k27/test-piano-guard.sh`, `tooling/k27/build-guard-test-images.sh`, `tooling/persist/build-esp.py` (`--boot-guard`),
`tooling/bootimg/build-ram-images.py` (`PIANO_RESET_AFTER=0` drops both reset arguments).
