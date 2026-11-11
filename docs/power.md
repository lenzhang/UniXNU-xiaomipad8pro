# Power: the battery picture, CPU idle (k27 0061) and suspend-to-idle (k27 0062)

Status 2026-11-11: **built, host-tested, linked into a KC with the current kext set; not run on the tablet.** Everything here is
off unless its boot argument is set. The owner's priority is battery life; this page is the whole picture (what each piece saves,
who owns it) and, in detail, the kernel/platform part: CPU idle states, a system sleep that is the closest equivalent of Linux's
s2idle with its wake sources, and lid / power-key sleep.

Facts are marked **verified** (read from source, the running system, a binary or a register a running driver already uses),
**spec** (Linux or Gunyah read as the specification only: Linux is GPL, nothing copied; Gunyah is BSD-3-Clause, read only),
**Ubuntu** (measured on this tablet by the Ubuntu project: `ubuntu-history/kernel.md`, `ubuntu-history/device.md`),
**inferred** or **needs-boot**.

## 1. The battery picture

The native system runs from two packs (9800 mAh, about 38 Wh). The one native reading so far, early in a boot with the GPU
coming up, was `battery: 4440 mV, -860 mA (discharging)`, about 3.8 W (`drivers/charging.md` §4): a working day is not
possible at that rate. Where it goes, and who is cutting it:

| Piece | What it saves (estimate, **inferred** until measured) | Owner | State | Page |
|---|---|---|---|---|
| Polling → interrupts, adaptive polling | the 1 kHz loops (USB, touch) and 100 Hz ones kept every CPU out of idle: thousands of wake-ups/s down to tens | the interrupts work | merged, needs-boot | `power-irq.md` |
| Backlight off with the display asleep | the backlight boost, the largest single load with the screen on (0.5-1.5 W) | PianoBacklight | merged, needs-boot | `drivers/backlight.md` |
| GPU idle (slumber, IFPC, votes) | GX off between frames; CX/MX and the DDR vote at boot levels today (`drivers/gpu.md`) | PianoA830 | in flight | `drivers/gpu.md` |
| CPU DVFS + thermal | the cores no longer at the clocks UEFI left; the Balanced caps | PianoThermal | merged, needs-boot | `drivers/thermal.md` |
| **CPU idle states** (here, §2) | idle cores in C4 retention instead of WFI clock gating, a cluster in CL5, the CPU subsystem in SS3: 0.1-0.3 W at idle with eight cores | k27 0061 | built | this page |
| **Suspend-to-idle** (here, §3) | no thread runs, no poll fires; CPUs parked in their deepest state; every driver taken to its off state | k27 0062 + every driver (§4) | built | this page |
| RPMh votes dropped → CX collapse, DDR self-refresh | the floor of a suspended SoC: from hundreds of mW (rails at active votes) to tens (Ubuntu since v0.8.0) | every driver (§4), then a kernel RSC flush | not started; contract written | §3.6, §4 |
| Charging control | not a saving: the battery has to fill | PianoPower | level 2 in progress | `drivers/charging.md` |
| Panel sleep (DCS), zero-copy scanout, 60/120 Hz | the panel and the CPU copy (~39 ms a frame) | display work (B1) | design | `BACKLOG-hardware.md` |

Battery life follows from the average power: at 3 W about 13 h, at 1 W 38 h; suspended at 0.5 W (no CX collapse) about three days,
at 30 mW (Ubuntu's CX-off) over a month. §6.4 is the measurement that replaces these estimates.

## 2. CPU idle (k27 0061, `piano-cpuidle=N`)

### 2.1 Facts

| Fact | State | Source |
|---|---|---|
| Gunyah traps the HLOS's WFI (HCR_EL2.TWI) and blocks the vCPU; the physical CPU then runs Gunyah's idle thread, which executes a plain `wfi` (clock gating) | spec | gunyah-hypervisor `hyp/vm/vcpu/aarch64/src/aarch64_init.c`, `wfi.c`, `hyp/core/idle/aarch64/src/idle.c` |
| Gunyah calls the firmware's CPU_SUSPEND only from its idle thread and only when no vCPU of that CPU is active; a vCPU blocked in WFI is active. So **XNU's WFI never reaches a retention or collapse state**: the idle loop must call CPU_SUSPEND | spec | `psci_vpm_active_vcpus_is_zero` (hyp/vm/psci) |
| With GICv4 Gunyah may stop trapping WFI of a vCPU that is alone on its CPU (`VCPU_IDLE_IN_EL1`); then WFI runs natively: still only clock gating | spec | `hyp/vm/vgic/build.conf` |
| A pending, enabled virtual interrupt wakes a vCPU even with PSTATE.I masked; while a vCPU is blocked its EL1 timers run on hypervisor timers | spec | `hyp/vm/vgic/src/deliver.c`, `hyp/vm/arm_vm_timer` |
| A vCPU's CPU_SUSPEND is a vote: per physical CPU the shallowest CPU-level request, for clusters and the system the shallowest across all VPM groups; in OS-initiated mode a cluster/system request while a sibling runs is DENIED (-3); invalid states -2 | spec | `hyp/vm/psci/src/psci_common.c`, `psci_osi`, `psci_pc`, `vpm_base` |
| Retention requests (StateType bit 30 clear) return SUCCESS to the caller; a power-down request restarts the vCPU at the entry point with the MMU off (`vcpu_warm_reset`); with a wake-up already pending a power-down returns SUCCESS too | spec | `psci_common.c` |
| Extended StateID format only; PSCI_VERSION 0x10001; FEATURES(CPU_SUSPEND) 3 for the HLOS (OSI) in the OSI build, else 2; SET_SUSPEND_MODE and SYSTEM_SUSPEND only in the OSI build; PSCI_STAT_* not implemented | spec; **needs-boot** (probe line) | `hyp/interfaces/psci/psci.tc`, `psci.ev` |
| piano's states (its Linux DT): CPU `ret` 0x00000004, cores 0-5 93/129/560 µs (entry/exit/min-residency), cores 6-7 172/130/686 µs; cluster `cl5` 0x01000054 2150/1983/9144 µs; system `ss3` 0x0200c354 2800/4400/10150 µs. None has the power-down bit, none `local-timer-stop` | verified | `out/inputs/piano-boot.dtb`, `ubuntu-history/kernel.md` §2.1 |
| Linux uses OS-initiated mode with these as genpd domain states: the last CPU of a cluster sends the cluster's param in place of its own, the last of all the system's; it entered the system state at run time on this tablet | spec; Ubuntu | `cpuidle-psci-domain`; K:322fd1bfbda9 |
| ArmCpu (UniXNU's CPU driver, linked into k27) registers no idle callbacks (`processor_idle`, `idle_timer` NULL) and only ever calls CPU_ON; XNU's idle loop ends in WFI (`cpu_idle`) | verified | ArmCpu.cpp; `osfmk/arm64/cpu.c` |
| Entering ss3 (4400 µs exit) stalls GPI DMA: Linux holds a 2000 µs CPU latency limit around each GPI SPI message (touch frames) | Ubuntu | K:322fd1bfbda9 |
| The AOP keeps RPMh sleep counters at 0x0c3f0048: records `aosd`, `cxsd`, `ddr` {count, last entered, last exited, accumulated, 19.2 MHz ticks}; cxsd/ddr accumulating with the time asleep is CX collapse and DDR self-refresh | spec; Ubuntu | `qcom_stats`; K:1d5861e2ee37 |

### 2.2 Design

- **Where:** `cpu_idle()`'s WFI (interrupts masked) calls `piano_cpuidle_enter()`; nothing else in the idle path changes
  (`osfmk/arm64/piano_pm.c`, hook in `cpu.c`).
- **Choice** (`piano_pm_policy.h`, host-tested): the CPU state when `rtcPop - now` (XNU's next timer deadline for this CPU, what
  the decrementer was just programmed with) is at least its min-residency and its exit latency fits every latency hold, else WFI.
  Domain states as Linux sends them: with `N >= 2`, the CPU that completes its cluster (all its CPUs in CPU_SUSPEND or entering
  it) asks for `cl5` when every CPU of the cluster has 9144 µs before its deadline; with `N = 3`, the one that completes the
  system asks for `ss3` (10150 µs), only when the LLCC island is reserved (§3.6). An idle mask and per-CPU deadlines are the
  bookkeeping; a DENIED (a sibling woke) retries at once with the CPU state; a state rejected as invalid 16 times in a row is
  turned off for that CPU and the summary says so, and so is one that returns at once with nothing pending 64 times in a row
  (`last error 1`: a CPU_SUSPEND that does not suspend would make the idle loop a spin of SMCs; WFI then blocks again).
- **Mode:** OS-initiated, set once at boot from the boot CPU before any CPU_SUSPEND (Gunyah's mode switch checks vCPU 0) when
  FEATURES(CPU_SUSPEND) has bit 0 and N >= 2; `piano-cpuidle-pc=1` keeps platform-coordinated (domain states as votes).
- **Power-down states:** none on this platform (Gunyah hides any physical collapse; the guest's context survives). For the test of
  XNU's path anyway, `piano-cpuidle-pd=1` asks for `ret` with the power-down bit: Gunyah then restarts the vCPU at the reset vector,
  which takes `resume_idle_cpu → start_cpu → arm_init_idle_cpu → cpu_idle_exit(TRUE)` (XNU's power-gated idle path), where
  `piano_cpuidle_reset_resume()` restores what ArmCpu set up once (the GIC CPU interface: SRE, PMR, BPR1, CTLR, IGRPEN1) and
  reprograms the decrementer.
- **Latency holds:** `piano_cpuidle_latency_hold(max_exit_us)` / `_release(handle)` (8 slots, any context); PianoTouch holds 2000
  around a DMA read, which keeps ss3 out and leaves cl5 (1983 µs).
- **Counters:** per CPU and state, entries and time (counter ticks); `sysctl machdep.piano_cpuidle` (all of it, with the PSCI
  answers and the RPMh line), `machdep.piano_cpuidle_level` (read-write, 0 .. N: the A/B switch without a reboot); every
  `piano-cpuidle-period` s (60) one console line per CPU and the RPMh line.

### 2.3 Boot arguments

| Argument | Default | Meaning |
|---|---|---|
| `piano-cpuidle=N` | 0 | 0 WFI as before; 1 `ret`; 2 + `cl5`; 3 + `ss3` (refused unless the LLCC island is reserved) |
| `piano-cpuidle-period=S` | 60 | the console summary's period; 0 none (the daily profile: 0 or 600) |
| `piano-cpuidle-pc=1` | 0 | no OS-initiated mode (domain states as platform-coordinated votes) |
| `piano-cpuidle-pd=1` | 0 | experimental: `ret` as a power-down, the reset-vector path |
| `piano-pm-probe=1` | 0 | only the probe lines (PSCI, RSC, RPMh, LLCC), nothing changes |
| `piano-llcc-island-ok=1` | 0 | ss3 even with the island not reserved: an experiment that can reset the SoC |

### 2.4 Lines

```
piano-pm: PSCI version 0x10001; FEATURES: CPU_SUSPEND 3 (extended StateID, OS-initiated mode supported), SYSTEM_SUSPEND 0, SET_SUSPEND_MODE 0, CPU_OFF 0, STAT_COUNT -1
piano-pm: APPS RSC DRV2 id 0x..., solver config 0x... (hardware solver on|off); RPMh sleep: aosd N entries S s, cxsd N entries S s, ddr N entries S s
piano-pm: LLCC ISLAND1 (SCID 12): reserved ways 0x7fffffff, bonus ways 0: reserved, as CX collapse needs       (or: NOT reserved ...; ss3 stays off)
piano-pm: PSCI_SET_SUSPEND_MODE(OS-initiated) = 0: the last CPU of a cluster or of the system asks for its domain's state
piano-cpuidle: level 2 (ret, cl5, OS-initiated), the deepest state that fits before the next timer, else WFI; summary every 60 s
piano-cpuidle: cpu3 state wfi entries 812 residency 1.2%, state ret entries 4410 residency 71.0%, state cl5 entries 120 residency 18.3% (denied 4, rejected 0, last error 0)
piano-cpuidle: RPMh sleep aosd 0 entries 0.000 s, cxsd 0 entries 0.000 s, ddr 0 entries 0.000 s
```
(The values in the first line are what the Gunyah source predicts; the probe is there to find out.)

## 3. Suspend-to-idle (k27 0062, `piano-sleep=1`)

### 3.1 Why not macOS's own sleep path

| Fact | State | Source |
|---|---|---|
| IOPMrootDomain sleeps through `IOPlatformExpert::sleepKernel → IOCPUSleepKernel` (this board has no `USE_APPLEARMSMP`): `haltCPU()` per CPU, then `processor_sleep`; the boot CPU ends in `ml_arm_sleep → arm64_prepare_for_sleep`, which on this board is a WFI loop (a Mac's SoC powers off there and comes back through the reset vector) | verified | `iokit/Kernel/IOCPU.cpp`, `osfmk/arm64/cpu.c`, `machine_routines_asm.s` |
| ArmCpu's `haltCPU` runs on the caller: it masks DAIF and spins in WFI. The first `haltCPU` (for a secondary, on the boot CPU) would hang the tablet. Its `initCPU(false)` panics ("secondary resume is unsupported"); `quiesceCPU` returns, which ends in "zombie processor" | verified | ArmCpu.cpp, the copy linked into k27 (`droidvm-build/macOS-DroidVM/out/k27/src/platform/ArmCpu`) |
| Gunyah's SYSTEM_SUSPEND is DENIED unless every other vCPU is CPU_OFF (ArmCpu cannot bring one back); Linux on this tablet never uses PSCI system suspend either: s2idle (`SuspendState=freeze`) | spec; Ubuntu | `psci_osi`; device.md §2.1 |
| Every native ADT says `/defaults sleep-disabled = 1`; IOPMrootDomain reads it at boot completion and refuses every sleep (`kPMConfigPreventSystemSleep`) | verified | `configs/k27-adt.toml`, `IOPMrootDomain.cpp` |

So the platform hook is IOCPUSleepKernel itself (the least invasive place: everything above it, IOPMrootDomain's notifications
and the drivers' power tree, runs as on a Mac; nothing below it is used), and `sleep-disabled` is overridden **kernel-side**,
only with `piano-sleep=1` (the ADT stays `sleep-disabled = 1` in every image: an image without the argument can never sleep).

### 3.2 The sequence

```
IOPMrootDomain (idle / Apple menu / lid / power key / piano-sleep-test)
  └ kIOMessageSystemWillSleep … every driver setPowerState(off) (§4) … powerChangeDone
     └ sleepKernel → IOCPUSleepKernel → piano branch (IOCPU.cpp)
        platform actions (PreSleep), trace points, sleep/wake watchdog stopped
        piano_s2idle_enter (piano_pm.c):
          baseline of the wake sources; Gunyah watchdog armed (30 s bark, 32 s bite)
          1 park: a bound MAXPRI_KERNEL thread per other CPU: preemption off, interrupts off, its virtual timer masked,
            PSCI vote (its cluster's last CPU: cl5) until released; IPIs taken in short windows while others still park
            (a CPU that does not park within 1 s aborts the suspend: nothing suspended, a normal wake)
          2 freeze: interrupts off on the boot CPU; a handshake IPI, after which every parked CPU takes no interrupt
            (it acknowledges IPIs by hand); kQueueQuiesce actions; mach_absolute_time noted
          3 loop on the boot CPU: read the armed sources; ping the watchdog every 5 s; virtual timer = poll (100 ms);
            the deepest allowed state (the last CPU of all: ss3 if allowed, else cl5/ret); on return take the poll timer
            or an IPI by hand; any other interrupt (a driver's SPI) ends the loop and is left pending for its handler
          4 back: the power key's release awaited (≤ 3 s); mach_absolute_time moved back by the time asleep; kQueueActive;
            the parked CPUs released (IPI) and their decrementers resynced; interrupts on
        platform actions (PostResume), watchdog started
  └ clock_wakeup_calendar: the calendar + the time asleep (piano_s2idle_take_wake_utc)
  └ full wake: wake reason "piano.<source>" claimed, requestUserActive; every driver setPowerState(on)
```
Nothing of a driver runs between the freeze and the release: all their polling threads stop with it (§4 tells them what that
means for them).

### 3.3 Wake sources

Read by the kernel between polls, with no driver code, through registers the drivers already read (read only):

| Bit (`piano-sleep-wake`) | Source | Read | Rule |
|---|---|---|---|
| 1 (default) | power key | PMK8550 PON (SID 0, PPID 0x013) INT_RT_STS 0x10 bit 7, SPMI observer channel | a press (released → pressed); the key held at entry (the press that asked for sleep) does not wake until it is released and pressed again |
| 2 | cover opening | TLMM gpio80 IO bit 0 (low = closed) | armed only when the cover reads closed at entry; two open readings in a row (as Ubuntu: opening wakes, closing never) |
| 4 | USB-C attach change | the charger PMIC's Type-C block +0x0b bit 0 (located as PianoPower does) | any change of the attach bit |
| 8 | volume-down | PON INT_RT_STS bit 6 | a press |
| 16 | keyboard | TLMM gpio95 INTR_STATUS (rising edges latched by PianoKeyboard, §4) | armed only with the cover open and the latch configured and clear at entry; never gpio97 |
| — | timer | `piano-sleep-max=S` | a test's automatic wake |
| — | a peripheral interrupt | any SPI pending at the boot CPU (drivers with `armcpu_spi_attach` lines) | ends the suspend; its handler runs after the wake |

Poll cost: a wake every `piano-sleep-poll` ms (100) for a few SPMI reads (µs). At `cl5`/`ss3` each poll also pays the state's exit
(2-4.4 ms); with those states `piano-sleep-poll=200..250` is the better trade (a power-key press must last a poll to be seen).
The interrupt route (SPMI arbiter SPI 746 through the PDC, the TLMM wake lines; `power-irq.md`) removes the polling later.

### 3.4 Time across a suspend

`mach_absolute_time()` does not count the suspend (as on a Mac: idle timers, HID idle time and every driver's timeout see only
awake time); the architected counter runs through it, so the time asleep is exact and the calendar is advanced by it in
`clock_wakeup_calendar` (this kernel has no continuous hardware clock, and the RTC answers only with `piano-rtc=1`, at 1 s). The
freeze handshake makes the rebase safe: from the moment the time is noted until the release, no XNU code runs anywhere and nothing
is stamped (recount and the scheduler's snapshots all lie before it).

### 3.5 Wake reliability (reboot rather than hang)

- The Gunyah watchdog (SMCCC vendor-hypervisor calls, as 0056) is armed at entry, pinged by the loop every 5 s, and pinged by a
  thread call for 240 s after the wake (past IOPMrootDomain's own 180 s wake watchdog, whose panic then becomes a reset), then
  turned off again unless `piano-reset-after` owns it (test images: 0056's pings resume after the wake). A hang anywhere in
  entry, loop or wake resets the tablet; RAM test images land in ABL fastboot (0057). Gunyah freezes this watchdog only for an
  OSI system suspend (spec), so in ret/cl5 it counts: pinged it must be.
- A CPU that does not park in 1 s, or does not stop taking interrupts in 100 ms: the suspend is aborted (a normal wake follows).
  A CPU that does not come back from the park in 0.5 s: panic, so the watchdog reboots.
- `piano-sleep-wdt=0` disables the watchdog part (not for daily use).

### 3.6 What this does not reach yet: CX collapse and DDR self-refresh

Linux's s2idle on this SoC reaches CX off and DDR self-refresh since Ubuntu v0.8.0. What it took (Ubuntu; kernel.md §2.1, §2.2):
every block dropping its RPMh/clock votes (the APPS RSC, DRV2 at 0x16520000, replays its sleep set only when the CPU subsystem
goes down, and any active vote left in place persists through sleep), the **LLCC ISLAND1 slice (SCID 12) with reserved ways
0x7fffffff and no bonus ways** (else *every* suspend reset the SoC), PDC wake routing for interrupt wakes. In this kernel:

- the CPU side is done (ss3 is requested by the last CPU, OS-initiated, as Linux does);
- the LLCC check is done (read only: `0x26841300` ATTR0 and `0x26841308` ATTR1 through the broadcast base; ss3 is refused while
  it is not reserved; programming the slice, e.g. with PianoA830's `Sm8750Llcc` v6 writer, is the next step if the probe shows the
  upstream table);
- the RPMh counters are logged at every suspend entry and wake: the proof, when the votes are gone;
- the votes are the drivers' (§4); a kernel `rpmh_flush` equivalent (sleep set 0 / wake set restore in the SLEEP/WAKE TCSes,
  refused while an active TCS is busy) is only needed for votes a driver must keep while awake, and only if DRV2's solver bit is
  clear (probe line).

## 4. The driver contract (`package/include/PianoPowerContract.h`)

Every piano driver: two IOKit power states (off, on), `pianopower::join(this)` in `start()` (PMinit, the platform expert's
`PMRegisterDevice`, `registerPowerDriver`, `changePowerStateTo(on)`, as NativeGopDisplay does), `setPowerState` synchronous
(return `kIOPMAckImplied` within 1 s, or a time in µs and `acknowledgeSetPowerState()` within 30 s), `PMstop()` in `stop()`.
In **off**: no activity that costs power, every RPMh / bandwidth vote, clock and GDSC it holds released or at its floor, no access
to a block whose clocks it gated, GIC lines it attached masked at their source (ArmCpu has no detach or mask) unless they are its
wake line. In **on**: back as before, hardware state read again (the cover, USB-C, the charger may have changed while frozen).
While suspended nothing of the driver runs and `mach_absolute_time()` stands still. The kernel offers
`piano_cpuidle_latency_hold/_release`, `piano_sleep_enabled()`, `piano_s2idle_last_reason()` (linked by name: a kext calling
them needs a 0061/0062 kernel, which `build-native-image.sh` pins with `K27_KERNEL`).

What each driver adds (their owners' work; nothing here changes their packages):

| Driver | off | on | Also |
|---|---|---|---|
| NativeGopDisplay | already registered: stop the VBL timer (it re-arms itself every frame, even with the display off) | restart it | panel DCS off/sleep-in later (display work) |
| PianoBacklight | already: BL_EN off through NativeGopDisplay's power-state hook (system sleep included); keep it so | already | never HWEN low |
| PianoA830 | GMU slumber, GX/CX GDSC off, the boot bandwidth vote and the CX/MX/DDR votes dropped; mask GMU interrupts | resume (slumber exit) | the ADSP is PianoPower's: no LPASS votes |
| PianoTouch | stop scanning (controller idle or panel-follower off), the latch's INTR_CFG enable off | restart asynchronously (Ubuntu: ~0.3 s faster wake) | `piano_cpuidle_latency_hold(2000)` around each DMA frame read |
| PianoKeyboard | 0x25 = 0 and the MCU sleep line (gpio3) as on panel blank; gpio97's enable off; with the cover open and `piano-sleep-wake` bit 16: gpio95 INTR_CFG raw status + rising edge (enable off), INTR_STATUS cleared | sleep line released, ≤100 ms for gpio97, drain, 0x25 = 1; gpio95 INTR_STATUS cleared; the hall read again and reported | lid → clamshell already (`piano-keyboard-lid=2`) |
| PianoInput | nothing to power; its thread just stops | read the keys again (the kernel swallowed the waking press) | with `piano_sleep_enabled()`: a short press (< 1.5 s, on release) → `receivePowerNotification(kIOPMSleepNow)` instead of the HID Power usage; a long press as today |
| PianoDwc3 | with a host attached and `piano-sleep-usb=1` (theirs to add): nothing (the console survives; logs flow after the wake); else link to U3/suspend; **never touch QSCRATCH with the glue clocks off** (Ubuntu: SoC reset) | restore | a cable to a host keeps the HS PHY out of L2: measure without it |
| PianoPower | hand charging back as Ubuntu v1.1.0 (the source type's 5 V limits, CP stopped) — level 2 only; the gauges' I2C idle | re-send the full charger state (#18), read the port again | level 1 (read only): nothing |
| PianoWifi | PCIe link down / root port D3, no RF clock vote (RPMH_RF_CLK1) | bring back | — |
| NativeUfs | hibern8, then every UFS clock and the GDSC (no extra references, no ICE) | exit hibern8 | in-flight I/O finishes after the wake (times do not advance) |
| PianoThermal | tsens lower-threshold interrupts masked (when interrupts are used); no SCMI requests | the first zone update re-arms them | cpufreq: nothing |
| PianoAudio | release LPASS_HW_MACRO / DCODEC votes when idle (Linux: 100 ms autosuspend) — **only after the LLCC island is confirmed reserved** | — | the island reset is about exactly this |
| PianoTorch | torch off (four SPMI writes, synchronous in `setPowerState(kOff)`; also `systemWillShutdown`) | stays off (a new request lights it) | level 1: nothing |
| NativeRamBlock, PianoNullEthernet, ARMPlatformStandIn | nothing | nothing | — |

## 5. Policy defaults

| What | Default | How |
|---|---|---|
| System sleep possible | off | `piano-sleep=1` (kernel; images built with it). Daily profile: only after §6 S1-S4 passed |
| Display sleep after idle | macOS's default | powerd; PianoBacklight turns the backlight off with it |
| System sleep after idle on battery | 5 min (Ubuntu's v0.8.0 choice); none on external power (stock and Ubuntu keep it awake plugged in) | `sudo pmset -b sleep 5 displaysleep 2 hibernatemode 0 standby 0 powernap 0 tcpkeepalive 0; sudo pmset -c sleep 0 displaysleep 10` (no hibernation natively: hibernatemode 0) |
| Lid close | sleep, only after the hall polarity is verified (`drivers/keyboard.md` §10.3); cover-open wake | `piano-keyboard=2 piano-keyboard-lid=2` (PianoKeyboard tells IOPMrootDomain the clamshell; macOS sleeps on close) + `piano-sleep-wake` bit 2. Ubuntu's "closed on external power only locks" needs PianoKeyboard to send kIOPMDisableClamshell on external power (its owner, with PianoPower's source state) |
| Power key short press | sleep / wake | wake: the kernel (bit 1, default); sleep: PianoInput (§4), until then the Apple menu or `pmset sleepnow` |
| Volume-down, keyboard, USB-C as wake sources | off | `piano-sleep-wake` bits 8, 16, 4 |
| CPU idle | off; target `piano-cpuidle=2` (ret + cl5) in the daily profile after §6 I1-I2 | `piano-cpuidle=N` |

Boot arguments of 0062:

| Argument | Default | Meaning |
|---|---|---|
| `piano-sleep=1` | 0 | system sleep is suspend-to-idle; overrides the ADT's sleep-disabled |
| `piano-sleep-state=N` | 1 | the deepest level while suspended: 0 WFI, 1 ret, 2 cl5, 3 ss3 (LLCC island required) |
| `piano-sleep-poll=MS` | 100 | the wake sources' poll period (20..1000) |
| `piano-sleep-wake=MASK` | 1 | 1 power key, 2 cover open, 4 USB-C, 8 volume-down, 16 keyboard |
| `piano-sleep-max=S` | 0 (30 with a test) | automatic wake after S s |
| `piano-sleep-wdt=0` | 1 | no Gunyah watchdog across the suspend |
| `piano-sleep-test=S` | 0 | kIOPMSleepNow S s after boot completed and after each wake ... |
| `piano-sleep-test-count=N` | 1 | ... N times |

Lines (0062): `piano-sleep: system sleep is suspend-to-idle: CPUs down to ret while suspended (OS-initiated), wake mask 0x1 (...),
poll 100 ms`; at boot completion `piano-sleep: the ADT's sleep-disabled is overridden (piano-sleep=1): system sleep is
suspend-to-idle`; then per suspend:

```
piano-sleep: test sleep 1 of 3 (piano-sleep-test): asking IOPMrootDomain to sleep now
IOCPUSleepKernel enter (piano: suspend-to-idle)
piano-sleep: wake sources: power key and volume-down PON APID <a>, cover gpio80 IO 0x.., keyboard gpio95 INTR_CFG 0x.., Type-C APID <t>   (first suspend)
piano-sleep: suspend 1: CPUs down to ret, wake on power-key timer, poll 100 ms, watchdog armed (32 s bite)
piano-sleep: automatic wake after 30 s (piano-sleep-max)
piano-sleep: suspend 1 entry: RPMh sleep aosd .., cxsd .., ddr ..
piano-sleep: suspend 1: woke after 30.0xx s by timer; ~300 polls, 0 IPIs, N spurious
piano-sleep: suspend 1 wake: RPMh sleep aosd .., cxsd .., ddr ..
IOCPUSleepKernel exit (piano: timer)
```
and `sysctl machdep.piano_sleep`: `on; state 1, poll 100 ms, wake mask 0x1, max 30 s; suspends 3 (aborted 0), last 30012 ms by
timer, total 90040 ms; wakes: power-key 0 lid 0 usb 0 volume-down 0 timer 3 irq 0 keyboard 0; LLCC island ...; rpmh ...`.

## 6. Test plan (order of risk)

Every test is a RAM boot (nothing written to the tablet; `build-ram-images.py`'s store guard gives other seeds `data-persist=0`)
with `piano-reset-after` as the backstop and every reset into ABL fastboot (0057), the USB console captured on the Mac
(`tooling/native/usb-console.py --capture S`, or the mailbox through `tooling/native/native-loop.sh IMAGE out/ram-pm-dump/...`).
A stuck tablet: power + volume-down for 15-20 s.

**Build** (main checkout; the k27 tree at 0060):
```sh
# kernel: in droidvm-build/macOS-DroidVM/out/k27/xnu: git am <this repo>/patches/k27/0061-*.patch <this repo>/patches/k27/0062-*.patch
K27_VARIANT=native ../droidvm-build/macOS-DroidVM/build-k27-piano.sh      # then copy .../mach.development.bcm2837.unstripped
                                                                           #   to out/k27/kernel-k27-0062-native.macho there
tooling/k27/test-piano-pm.sh                                               # host tests (add --kext with K27_KERNEL=... for the contract)
K27_KERNEL=$K/out/k27/kernel-k27-0062-native.macho PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh   # KC + ADT
tooling/k27/build-pm-test-images.sh            # out/ram-pm-{base,probe,idle1,idle2,idle1pd,idle3,sleep0,sleep1,sleep2,sleepkey,measure}
```
(`$K` = `droidvm-build/macOS-DroidVM`. Built offline here from an APFS clone of the k27 tree with its own object directory: kernel
sha256 `24940c6dd10bee7d…`, KC with the 14 native kexts `f0a92af1f3696fc4…`, layout check passed; images `ram-pm-probe`
`6581b37722ae39d9…`, `ram-pm-idle2` `f0332cca1569821f…`, `ram-pm-sleep1` `c6e3492ac15a9597…` — rebuild in the main checkout before a
run. For `sleepkey` with the cover: `PIANO_EXTRA_KEXTS=$PWD/out/k27/kext/PianoKeyboard.json` in the KC build.)

| # | Image | Risk | Run | Pass | On failure |
|---|---|---|---|---|---|
| P | `ram-pm-probe` (240 s) | read only (PSCI queries; RSC, AOP and LLCC reads: a fault would be an abort at boot) | unattended | the three `piano-pm:` lines; values recorded in §2.1/§3.6 | a missing line after `piano-pm: PSCI` names the region that faulted: drop that read |
| I1 | `ram-pm-idle1` (900 s) | low: retention returns like WFI | unattended, then 10 min idle at the login window, no cable for the measurement | per-CPU `ret` residency high at rest (> 60 %), `rejected 0`; desktop as usual | `rejected` > 0 with `last error -2`: the param is wrong for Gunyah: back to WFI (`machdep.piano_cpuidle_level=0`) and report |
| I2 | `ram-pm-idle2` (900 s) | low-medium: cluster votes, OSI | as I1 | `PSCI_SET_SUSPEND_MODE(OS-initiated) = 0`; `cl5` entries at rest; few `denied` | `= -3/-1`: rerun with `piano-cpuidle-pc=1` |
| I3 | `ram-pm-idle1pd` (300 s) | medium: the reset-vector path | unattended | `ret` entries with `N through the reset vector` growing, no hang | a hang → reset at 300 s: the resume path is incomplete (keep pd off; it is no saving) |
| I4 | `ram-pm-idle3` (300 s) | medium-high: CPU subsystem state | only if P showed the island reserved | `ss3` entries; RPMh `aosd`/`cxsd` may start counting | reset or stall: keep N = 2 |
| S1 | `ram-pm-sleep0` (600 s) | medium: the whole suspend path, CPUs in WFI | unattended: 3 cycles, 30 s each | three `woke after 30.0 s by timer`, `IOCPUSleepKernel exit`, the desktop back (USB console prompt, login window clock right), `sysctl machdep.piano_sleep` | `aborted: CPUs ... did not park`: the stuck CPU's thread (USB console `ps`); a hang: the watchdog resets in ≤ 32 s, the mailbox has the last line |
| S2 | `ram-pm-sleep1` (600 s) | as S1, in ret | unattended | as S1; fewer spurious returns | as S1 |
| S3 | `ram-pm-sleep2` (600 s) | cl5 while suspended + idle level 2 | unattended | as S1 | stay at S2 |
| S4 | `ram-pm-sleepkey` (1800 s) | attended wake sources | owner: after each test sleep press power (wake within a poll), then close and open the cover (with PianoKeyboard), plug/unplug USB-C, press volume-down | `woke ... by power-key / lid / usb / volume-down`, no immediate re-sleep after the power key, no spurious HID power press | a missed short press: raise `piano-sleep-poll` resolution (lower value) |
| M | `ram-pm-measure` (2400 s) | low (S2-S3 passed) | no cable: unplug at the login window, wait 40 min, plug back, capture | §6.4 | — |

### 6.4 The measurement (A/B)

PianoPower's line every 10 s (`piano-power=1 piano-power-period=10`, all images): `[Ns] battery: V mV, I mA (discharging), P %
(R/F mAh), ...`; the current is the two packs' sum, negative while discharging. With the cable out (a host keeps the HS PHY out
of L2 and charges or supplies):

1. **Idle desktop, display on** (login window, no input, same brightness): `base` vs `idle1` vs `idle2`, the mean current from
   minute 2 to 9. Also `machdep.piano_cpuidle_level` 0 → 2 → 0 in one boot from the USB console (one line per change), which
   removes the boot-to-boot noise.
2. **Display off**: `pmset displaysleepnow` in each, the same window. With PianoBacklight (`PM_EXTRA=piano-backlight=2`) this
   is the backlight's share.
3. **Suspended**: `measure`: the lines stop during the suspend; the average current asleep is the remaining-capacity difference of
   the last line before and the first after, divided by the time asleep (`woke after S s`): `ΔmAh × 3600 / S` mA. 30 min at
   300 mA is 150 mAh, well above the gauge's resolution; with CX collapse (§3.6) a longer window is needed.

Expected (inferred; to be replaced by the numbers): idle with the display on, `idle2` 5-15 % below `base` (more once DVFS and the
interrupt work cut the wake-ups); display off another 0.5-1.5 W; suspended, a fraction of the display-off idle (no thread wakes,
CPUs parked), still hundreds of mW while the rails keep their votes; with §3.6 done, tens of mW and `cxsd`/`ddr` accumulating with
the time asleep.

## 7. Risks and limits

- **A broken idle or suspend path hangs the tablet**: everything is opt-in; the watchdog covers the suspend path; test images
  reset on their own. The idle path has no watchdog of its own: a CPU that never returns from CPU_SUSPEND shows as a stall until
  `piano-reset-after`'s reset (0056's watchdog bites if the timer stops).
- Gunyah facts are from its public source; the Qualcomm parts (StateID validation, the watchdog handler, the production
  configuration) are not public: the probe and I1/I2 settle them.
- The poll-based wake misses a power-key press shorter than a poll period; PianoInput's 32 ms debounce means it never saw such
  presses as presses either.
- A driver that keeps polling hardware it powered off, or touches a gated block on wake before it is clocked, can fault: each
  driver's `on` must re-enable before access (§4).
- The SPMI observer channel of the PON (and the Type-C block) is shared with PianoInput (PianoPower): the kernel waits for a
  command in flight before its own; during the freeze it is the only reader. A read racing PianoPower at the suspend entry can
  give one wrong value in a log line.
- Time: an RTC alarm wake (Linux's rtcwake) is not offered: `piano-sleep-max` is the timer.
- Not done: RPMh sleep/wake sets (kernel), LLCC slice programming, PDC wake routing, PSCI SYSTEM_SUSPEND (needs CPU_OFF/ON in
  ArmCpu), macOS dark wakes (every wake is full).

## 8. Files

`patches/k27/0061-*.patch` (`osfmk/arm64/piano_pm.c`, `piano_pm.h`, `piano_pm_policy.h`; hooks in `cpu.c`, `arm_init.c`;
`machdep.piano_cpuidle*` in `bsd/dev/arm64/sysctl.c`), `patches/k27/0062-*.patch` (the suspend in `piano_pm.c`; `IOCPU.cpp`,
`IOPMrootDomain.cpp`, `clock.c`, `machdep.piano_sleep`), `tooling/k27/piano-pm/` (the policy header, its host test, a contract
check driver), `tooling/k27/test-piano-pm.sh`, `tooling/k27/build-pm-test-images.sh`, `package/include/PianoPowerContract.h`
(on every kext's include path through `build-native-kext.py`).
