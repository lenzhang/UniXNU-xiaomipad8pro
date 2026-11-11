# Forensics, 2026-11-11 evening: the "power1" reset and the daily ESP's silent first boot

Status: an offline reading of the logs; nothing has run on the tablet since. Two failures:

- **(A) power1** (RAM boot, an image built at 09:41: `piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-power=1
  piano-usb=2 piano-usb-hid=1`, `data-persist=2`) reset about 56 s after XNU started. Its console ends in PianoA830's S1 trace at
  `t g1.7 GPUCC r 3d99010 =8000000`.
- **(B) the daily ESP** (`esp-daily2-macos`, written at 20:51) brought up no USB console on its first boot (5+ minutes). Its RAM
  equivalent (`out/ram-daily2`: the same kernel collection and seed, `data-persist=0`, a reset after 300 s) had passed 10 minutes
  earlier.

**Update 2026-11-12.** (B) is settled as §6's first row. The installed ESP's console log, read back after the night, shows the
system running for 2.4 h, while PianoDwc3 never saw a host (`no-host`, `link state 3, configuration 0`). The ESP stays. The fix is
PianoDwc3's cold start (§7; `docs/drivers/usb-device-debug.md` §17), built and host-tested. It is to be proven from ABL fastboot by
the RAM images C0-C8 (§7.3) before the ESP is rebuilt.

Short answer: (A) is **not** a hang on the GPU_CC read. The bring-up had already returned successfully; the GPU_CC lines are a
logger backlog. The system stopped about 1 s after the bring-up, at a place the asynchronous trace cannot show. For (B) the leading
suspect is new: this was the first boot ever of PianoDwc3 and PianoPower on the normal (non-fastboot) boot path. The diagnostic
code of §4 and the test order of §5 settle both in one session. The decision table is §6.

Sources: `logs/native-runs/20261111-201656-loop/` (power1's loop log and its console from the mailbox),
`logs/usb-console/20261111-201656-port0{,.times}.txt` (the same console over USB, each line stamped with the Mac's arrival time),
the passing runs `logs/native-runs/20261111-073205-loop`, `-073448-loop`, `logs/usb-console/20261111-202237-*` (G2), `-202842-*`,
`-203446-*`, `-204211-*` (daily2 as a RAM image), the install session's record and ESP build records in the persist checkout
(`out/persistent/`). Specification only: Linux `drivers/clk/qcom/gpucc-sm8750.c` (GPL-2.0, read for register facts, nothing
copied). Code: Adreno830's `platform/sm8750/Sm8750GpuPower.c` and `A830Trace.h` (MIT), the k27 tree's XNU sources (timer queues,
console path), k27 0055/0056.

## 1. The register named last: GPU_CC 0x3d99010

GPU_CC is at 0x3d90000, so 0x3d99010 is offset 0x9010. In Linux's SM8750 GPU_CC description that is the CBCR of
`gpu_cc_demet_clk`: halt and enable register 0x9010, enable bit 0, `BRANCH_HALT_VOTED`, no parent clock. It is not one of the six
critical CBCRs (0x9004 RSCC_XO_AON, 0x9008 CXO_AON, 0x9064 GX_AHB_FF, 0x90cc SLEEP, 0x93a4 CB, 0x93a8 RSCC_HUB_AON). It is not in
the GMU's clock list in piano's DT either. Sm8750GpuPower.c enables it as the first of its waited branches ("kept from the
author's tested sequence").

The step is `Sm8750GpuStepBranches` (the trace's `g1.7`). The six critical CBCRs get their enable bit, `gpu_cc_ahb_clk` (0x90bc)
gets its enable with a 10 us delay, then 0x9010 is read, written with the enable bit, and polled until CLK_OFF (bit 31) clears,
500 us at most. In G2 and daily2 the lines after the read are:

```
t g1.7 GPUCC r 3d99010 =8000000
t g1.7 GPUCC w 3d99010 8000001
t g1.7 GPUCC r 3d99010 #2 =8000000      CLK_OFF clear: on
t g1.7 GPUCC r 3d990d4                  gpu_cc_cx_gmu_clk next
```

In power1 **the read completed**: both its lines are on the console (`r 3d99010` before the access, `r 3d99010 =8000000` after
it), with the passing runs' value. An access that never completes leaves only its first line.

## 2. (A) power1

### 2.1 The bring-up had finished

PianoA830's report lines do not go through the trace ring: `piano_a830_note` calls IOLog directly, from the starter thread (the
accelerator's line from IOKit's matching). They arrived at 20:17:47.714, 1.05 s after the bring-up began:

```
PianoA830: board: adreno830_start returned 0
PianoA830: Metal accelerator AGXAccelerator on NativeAdrenoGpu (MetalPluginName NativeAdrenoMetal)
PianoA830: adreno830_start: 0; stage 15 (running) state 2 step 0 reason 0 ioreturn 0 reg 0 value 0 expect 0
PianoA830: stage S0 inputs: 8562 us ... S3 SMMU: 997294 us ... S7 ZAP: 17915 us
PianoA830: stage S8 fence: 11516 us
```

These are G2's numbers within 1 % (for example S3: 990327 us, S8: 11525 us). **S1..S10 passed in power1, GPU_CC 0x9010 included.**

The `t g1.x` lines after them are the logger thread's backlog. The orchestrator pushes a whole bring-up's trace into the ring in
about 1 s: G2 logged `g done 0 1046938us lines 1921 drops 0`. The logger prints that backlog through the verbose console at 10-30 ms
a line, and G2's last trace line came 40 s after its bring-up had returned. When power1's console stopped, the logger was about 170
lines into S1. The board's comment claimed that a hung access is always named on the console. That holds only once the logger has
printed everything before the hung access; the comment is corrected in this change.

### 2.2 Where the console stopped

k27 0055 puts a line into the mailbox when its writer enters `console_write`, before the console ring and the drain that draws it
on the panel. The mailbox order is therefore the order in which each writer entered `console_write`:

1. The **starter thread** stopped first. Its last line is `stage S8 fence: 11516 us`. The next lines (`stage S9 Metal`,
   `stage S10 DCVS`, the four status rows, `bring-up done: the GPU runs`, the SMMU route lines) come within the same millisecond in
   every other GPU run (G2, daily2, 202842, 203446, 073448), and they never came. Between those two IOLogs the thread touches no
   hardware.
2. The **logger** wrote about 150 more backlog lines. Its last is `t g1.7 GPUCC r 3d99010 =8000000`.
3. **PianoDwc3** handled four control requests from the Mac (SET_LINE_CODING) at 20:17:53.06 and stopped in the fifth, with its
   data stage pending.

The mailbox dump and the USB stream end on the same line. No writer entered `console_write` after it, and there is no panic text.
Arrival times are only upper bounds, because the USB console streams the mailbox in batches (nothing arrived from 20:17:47.714 to
20:17:50.401).

### 2.3 The reset

The reset came at about 20:18:07 ± 1 s: the tablet was back in ABL fastboot at 20:18:15, and the PSCI resets of 202237, 202842 and
203446 reached ABL fastboot 7-8 s after their `300 s reached` line. k27 0056 pets the Gunyah watchdog every 10 s, and the watchdog
bites 32 s after the last pet. Three runs put the pets at 3.24-3.28 s of uptime plus multiples of 10 s, measured from the
`300 s reached` line against PianoA830's clock. In power1, uptime 0 is 20:17:20.1 (from `g start ... t=26558819` at 20:17:46.660),
so the pets fall at about 20:17:23.4, :33.4, :43.4 and :53.4.

- A bite 32 s after the 20:17:43.4 pet would bring ABL fastboot at about 20:18:22, later than observed.
- A bite 32 s after the 20:17:33.4 pet would bring it at about 20:18:12-13, which fits within the measurement.

So either the pet due at uptime ≈23.3 s did not take effect, 3 s before the bring-up began, or the reset was not the 32 s bite. The
other possibilities are a reaction of the firmware or hypervisor to a stalled CPU, or a panic whose text never reached the mailbox.
The run cannot tell these apart.

In both readings a CPU stopped serving its interrupts. On arm64 XNU every timer sits on the queue of the CPU that armed it
(`timer_queue_assign`, local CPU; there is no long-term queue on arm64), and timers move only when a CPU is taken offline
(`ml_cpu_down` → `timer_queue_shutdown`). One CPU stuck in an access that never completes, or in an SMC that never returns,
therefore strands the timers it holds (0056's pet among them) while the other CPUs run on.

### 2.4 What differs from the passing runs

**Register state.** Before the stop the logger printed 173 S1 accesses. They match G2 and daily2 access for access, values
included. The one exception is 0x3d990cc (`gpu_cc_sleep_clk`, critical) bit 31: clear in power1 and daily2, set in G2. Nothing in
what S1 reads shows a state left by the previous boot (a long-running installed system, then a button reset): the CX GDSC is
collapsed at entry and comes up with the same values, PLL0 is configured from standby, and the RCGs, GPLL0 and the DRV-2 TCS votes
for CX/MX complete with the same values. S1 does not read GX, the GMU, or RPMh's aggregate rail levels from other masters.

**Configuration.** power1 is the only GPU run of the day that combines the Data store in direct mode (`data-persist=2`, store
resumed) with the USB console. It is also the oldest build among tonight's runs:

| Run | GPU | USB console | PianoPower | Data store | Build | Result |
|---|---|---|---|---|---|---|
| 073205 (and 073448, whose store line decayed) | 2 | no | no | direct (resumed) | morning | passed |
| G2 (202237) | 2 | yes | 2 (no writes) | off | 15:10 | passed |
| daily2 (204211) | 2 | yes + HID | 1 | off | 20:26 | passed |
| power1 (201656) | 2 | yes + HID | 1 | direct (resumed) | 09:41 | stopped ~1 s after the bring-up |
| ESP daily2 (B) | 2 | yes + HID | 1 | direct (replaced) | 20:26 | no USB console |

The 09:41 build's PianoDwc3 still polls every millisecond; the wait on the link state came at 14:33.

### 2.5 Candidates for (A)

1. **A CPU stall right after the bring-up**, around uptime 27.6 s. GPU-side activity starts exactly then: S10's DCVS, the first
   status-row refresh, the accelerator's registration (AGXAccelerator started; NativeGopDisplay's fence lookups follow). An
   access to a collapsed or unclocked block would stall its CPU this way. That is a candidate; this log cannot place it.
2. **The verbose console's flush.** Every IOLog can make its caller the console's flusher, and the flusher draws the backlog into the
   27 MB boot framebuffer, which is Device memory (`docs/boot-time.md` §3.1). A starter stopped between two IOLogs fits a flusher
   that never came back from drawing. So does the rest of the pattern: the others kept writing about 14 KB, and writers block only
   once the console ring is full. A store that never completes would stall that CPU as in 1.
3. **Direct-mode Data I/O and the USB console together**: this is the combination §2.4 singles out, and (B) shares it. Both drivers
   program the apps SMMU (NativeUfs redirects stream 0x60; PianoDwc3 keeps 0x40 as the firmware left it), and NativeUfs issues FUA
   writes for every changed Data page while the boot runs.
4. **The previous boot's state**: no evidence in the registers S1 reads (§2.4). Not excluded for blocks S1 does not read.
5. **PianoPower level 1**: it was idle at the time. Its first report (I2C gauges, SPMI) was printed before 20:17:33, and the next was
   due at about uptime 41 s. Evidence against.
6. **CX/MX rail level**: the S1 votes completed with G2's values, and the bring-up passed after them. A rail loss is not supported
   by anything in the log.

## 3. (B) the daily ESP's first boot

### 3.1 What is known

- `esp-daily2-macos`: kernel collection `3850545b` (21 native kexts), seed `8f333110`, menu timeout 3 s, default `macos`. Its
  arguments are the daily profile (GPU, PianoPower 1, USB console + HID) plus `data-persist=2 data-store-replace=0xaced5bf9`, with
  no `piano-reset-after` and no `piano-reset-to`.
- It was written through the maintenance image and read back (partition and LUN 0) at 20:51-20:53. The first boot was a
  `fastboot reboot` at 20:54:02: ABL's normal path → boot_a → FD → UniXNU menu → the ESP's payload.
- `ram-daily2` (20:42) had the same collection and seed, `data-persist=0`, a reset after 300 s and menu timeout 0, booted with
  `fastboot boot`. It passed until the monitor's `reset fastboot`.
- **The ESP installed before it was `esp-gpu-macos`** (09:05): 9 native kexts, PianoA830 only, no `piano-usb`, no `piano-power`,
  `data-persist=2`, seed `aced5bf9`. It reached the login screen on its own. `esp-daily-macos` (10:30) was a dry run only and
  never written. So the daily2 ESP's boot was the first time PianoDwc3 and PianoPower ran on the normal boot path. Every USB
  console so far had followed ABL fastboot, which leaves the controller, PHY and repeater running.
- The checkout has no USB capture of that boot (no `logs/usb-console` file after 20:45), and no observation of the panel was
  recorded. Whether the Mac saw the gadget enumerate at all is unknown.
- Without `piano-reset-after`, k27 0056 does not arm the Gunyah watchdog. Any stall then parks the tablet until a button reset, and
  no dump image reads the mailbox. (Since this ESP was built, `build-esp.py --profile daily` adds k27 0063's
  `piano-boot-guard=180` ([boot-guard.md](boot-guard.md)), so a rebuilt ESP on a 0063 kernel resets a stalled boot instead; not yet
  run on the tablet.)
- The store at LBA 68119455 belonged to seed `aced5bf9` (generation 4 at the install's check; power1 resumed it at 20:17). With the
  store guard and `data-store-replace=0xaced5bf9`, the first boot to reach the store's set-up starts it over: state 3, generation 5,
  seed `8f333110`. The header on the medium therefore records whether the ESP's boot got that far (R0, §5).

### 3.2 Suspects, ranked

| # | Suspect | For | Against |
|---|---|---|---|
| 1 | **The USB console on the normal boot path**: (a) the takeover refuses (stage −10: the core not clocked) or the link never comes up, and macOS runs without a console; (b) an access to a block whose clock the probe does not check stalls a CPU and the tablet parks. The core and QSCRATCH are read only after the GCC check, but the M31 PHY's registers are read whenever the core's clocks are on, and the takeover then writes the PHY and resets the core | never run on this path; `docs/drivers/usb-device-debug.md` §10 lists "UEFI powered USB down at ExitBootServices" and an unimplemented cold path; everything else in this boot had already worked on this path (§3.1) or with this collection (ram-daily2) | the FD's own USB fastboot may set up the controller on both paths (not verified on the normal path) |
| 2 | **PianoPower level 1 on the normal path** (SPMI reads, bit-banged I2C on TLMM pins) | first time on this path | read only; its failures are lines ("no ack", refusals), and it ran in every RAM boot tonight |
| 3 | **The store's start-over**: the guard code's first run on hardware; 96 FUA bitmap writes, a sync, the header, a sync | new code on the boot path; shares direct-mode I/O with (A) | the I/O sequence is the old code's (the old kernels ran this initialisation four times at this window through NativeUfs: generation 4); the decision is host-tested (`Test/guard.cpp`); a stall that holds only the publish thread would leave the USB console (its own thread) running |
| 4 | **(A) again** (a stall about 1 s after the GPU bring-up, uptime ≈ 27-28 s) | the same GPU arguments as power1, plus direct-mode Data and the USB console; no watchdog to reset | the USB console is normally up by uptime 13-14 s, so an attached reader would have shown lines first (none was recorded) |
| 5 | **The ESP boot path** (UniXNU reads the payload from the FAT ESP on UFS; 3 s menu) | differs from RAM boots | the same path booted `esp-gpu-macos` to the login screen |
| 6 | **The 12 natives added since that ESP** | more code | each is inert without its argument; the same collection passed as ram-daily2 |
| 7 | **The clock argument** (the ESP's build time, 28 min old) | differs | earlier ESPs carried hours-old values; it only seeds the calendar |
| — | **No `piano-reset-after`** | — | not a cause: it turns any stall into a parked tablet and removes the automatic mailbox read |

### 3.3 What settles it

- The panel, before any reset. The verbose console draws every line until WindowServer's first frame, so a stall shows its last
  lines there. A login screen or desktop means "no console, system up" (1a).
- The store header (R0). Seed `8f333110`, generation 5, with present pages means the boot reached the Data set-up and wrote Data,
  so the system ran well past the USB takeover. Seed `aced5bf9`, generation 4, means it stopped before the set-up.
- R1/R1e against R2: the ESP's arguments on the fastboot path, with and without the store.
- R4: the installed boot again, observed, with a capture running (§5).

## 4. Diagnostic code (default off)

**PianoA830, `piano-gpu-trace=3`, synchronous trace.** Adreno830Start.cpp's `traceAccess` now hands the tracer's lock back and then
calls a board hook, `boot_progress_trace_sync` (a weak no-op upstream, recorded in `package/PianoA830/UPSTREAM`), before the access
is made. The board drains the ring there with IOLog, from the thread making the access, and only when interrupts are on and
preemption is enabled; elsewhere the line waits for the logger as before. Each line carries the uptime of its push,
`PianoA830: [27.612] t g1.7 GPUCC w 3d99010 8000001`, so the console's last `t` line is the access in flight or the one just made.
The driver itself is handed `piano-gpu-trace=1` (runs counted; 2 stays the verbatim mode). The logger adds a heartbeat once a
second, every 30 s from 120 s after the bring-up:

```
PianoA830: hb 31.204 s: bring-up thread past line 812 for 3020 ms: its access (t g5.3 GMU w ...); starter: bring-up for 4645 ms; lines pushed 830, printed 812 (790 by the bring-up thread), sync prints skipped 0, backlog 18
```

The heartbeat shows whether the rest of the system still runs, whether the bring-up thread is printing (the console) or past its
print (the access), which phase the starter is in, and whether it is inside an IOLog and for how long (power1's stop).

Cost: with `-v` every line is drawn, so the bring-up takes tens of seconds instead of one, and accesses are milliseconds apart.
A failure that only shows up when the bring-up is slow fails here too. A pass does not clear (A), because the timing changed.

The per-step timeout guard was not added. Every poll in the bring-up is already bounded, by iterations (Sm8750GpuPower) or by
time (A830Gmu, A830Hfi, A830Cp, A830Smmu, Sm8750Scm). What no software can bound is a single access, or an SMC, that never returns;
the heartbeat names that one instead.

**NativeRamBlock, `native-ram-trace=1`.** It prints a line before and after each step of the store's set-up: the waits for IOBSD
and for NativeUfs's root layout, the GPT check, the window, the header read (described: seed, generation, flags), the bitmap read or
clear (first block, every 32nd, last), each sync, the header write, the disk's registration. Each line carries milliseconds since
the publish thread started (`NativeRamBlock: store +1234 ms: clearing the bitmap (96 blocks, one FUA write each)`). The set-up's
I/O is unchanged. The argument is not a `data-*` name, so the tooling's guards accept it in `PIANO_EXTRA_ARGS` and
`build-esp.py --arg`.

## 5. The test session, in this order

The exact commands are in the session's queue file (not published). The images are in the main checkout's `out/ram-fx-*`. They
use a collection (`abd30a00`, kernel 0059 as the ESP's) built from the ESP's sources plus the two diagnostic commits (the kexts of
`a40b611` with `piano-gpu-trace=3` and `native-ram-trace=1`), and the ESP's seed, trust cache and ADT. `ram-fx-R1e` uses the ESP's
own collection.

| Step | Image | What it does | Expect | Meaning |
|---|---|---|---|---|
| 0 | (none) | look at the panel of the parked ESP boot and photograph it; `usb-console.py --list`; then power + volume-down; then a dump-only read of the mailbox | login screen / desktop, or frozen verbose text | (B) 1a, or the last lines name the step. The mailbox usually does not survive a button reset (`docs/bringup-debugging.md`); a readable tail is a bonus |
| R0 | `ram-fx-R0` | read-only store probe (`data-persist=1`), no GPU, no USB, 90 s | `store +… ms: header read: SHNGDATA v1, 3145728 pages, seed 8f333110, generation 5, flags 0`, then `Data store state 6`, `present P` | the ESP boot reached the Data set-up; P > 0: it ran on and wrote Data (the system was up: (B) 1a likely) |
| | | | `seed aced5bf9, generation 4`, state 7, present > 0 | the ESP boot stopped before the Data set-up at uptime ≈12 s (the USB takeover, PianoPower's start, NativeUfs; not the GPU, which starts ≈14 s later) |
| | | | `seed aced5bf9`, present 0 | the start-over cleared the bitmap (written first) and stopped before or in the header write (written last): (B) 3 |
| R1 | `ram-fx-R1` | the ESP's exact arguments + `piano-reset-after=420 piano-reset-to=bootloader native-ram-trace=1` | the store lines to `set-up done` (state 3, generation 5; or 2 if R0 showed generation 5), `PianoDwc3: stage 20 running`, the GPU's `bring-up done`, the reset at 420 s | (B)'s payload, arguments and store are fine on the fastboot path |
| R1e | `ram-fx-R1e` | the same with the ESP's own collection, no trace argument | as R1 without the `store +` lines | only if R1 passes: confirms the installed collection itself |
| R2 | `ram-fx-R2` | R1 with `data-persist=0` | as ram-daily2: `data-persist=0: no store to open`, console up, GPU up, reset at 420 s | the reference: if R1 stops and R2 passes, the store's direct mode (with the USB console) is the cause |
| R3 | `ram-fx-R3` | power1's arguments with `piano-gpu-trace=3 native-ram-trace=1` (store direct, as in power1), 300 s | `[<uptime>] t …` lines before each access, `hb …` every second, `bring-up done`, `SMMU routes`, heartbeats until the reset | a stall: the last `[<uptime>] t` line and the last `hb` say where and whether the rest of the system ran (§4) |
| R4 | (installed) | from ABL fastboot: `usb-console.py --capture 600` running, then `fastboot reboot`; watch the panel | the gadget enumerates and the console streams, or not; the panel shows the login screen or frozen text | the normal boot path itself: (B) 1 against everything the RAM images covered |

Each RAM run lands in ABL fastboot by itself (`piano-reset-to=bootloader`), and `tooling/native/native-loop.sh` reads its mailbox.
R4 needs a person for the panel, and a button reset if it parks.

Store note: R0 writes nothing. R1, R1e and R3 open the installed store's window (`data-persist=2`). If the window still holds seed
`aced5bf9`'s store they start it over for seed `8f333110`, which is what the ESP itself does. That store holds no user data yet.

## 6. Decision table: fix or roll back the installed ESP

"Rebuild" means `build-esp.py` from the current tree, which now also adds the boot guard (§3.1); "roll back" means
`maint-session.py … esp-final` with the persist checkout's `esp-gpu-macos.img` (a NAS backup first, as always).

| Outcome | Reading | Installed ESP |
|---|---|---|
| Step 0 or R4: login screen / desktop, no gadget on the Mac; R1 passes | (B) 1a: no USB console on the normal path, system fine | **keep** `esp-daily2-macos`; fix PianoDwc3's cold path (PHY, repeater, clocks after a normal boot) and test it on the normal path |
| Step 0 / R4: frozen verbose text ending in PianoDwc3's probe or takeover | (B) 1b: the takeover stalls on the normal path | **roll back** to `esp-gpu-macos` (`maint-session.py … esp-final`), or rebuild daily2 without `piano-usb`; fix PianoDwc3 (check the PHY's clocks before its reads, the normal path's cold start) |
| Frozen text ending in PianoPower's lines | (B) 2 | rebuild without `piano-power` (or roll back); fix PianoPower's normal-path state |
| R1 (and R1e) stop in the `store +` lines, R2 passes | (B) 3: the store's start-over or direct mode | **roll back** to `esp-gpu-macos` (build-esp.py gives the macos entries `data-persist=2` always: a volatile-Data ESP needs an option there first); fix NativeRamBlock/NativeUfs at the step named |
| R1 stops after the GPU bring-up, R2 passes | (A) and (B) share the direct-mode store + USB console + GPU combination | rebuild the ESP without `piano-usb`, or roll back; R3's heartbeat names what to fix |
| R1 and R2 both stop at the same place | the collection or the arguments, not the store | roll back to `esp-gpu-macos`; fix at the place named |
| R3 stops with a `[<uptime>] t` access in flight for seconds, heartbeats going on | (A): a hung GPU access, now named | GPU fix at that step; keep any ESP without `piano-gpu=2` until then |
| R3 stops with heartbeats stopping at the same time and the starter "inside IOLog" | (A): a system-level stall in or under the console path | console or system fix (`-v` off as a test: `PIANO_FAST_BOOT=1`); the ESP decision follows R1/R2 |
| Everything passes (R1, R2, R3) and R4 shows the login screen | (B) 1a | keep; schedule the PianoDwc3 cold path |

Result (2026-11-11, late): the first row. The installed system ran; only the console was missing (`results-2026-11-11.md`, "Later the
same night"). The ESP is kept, and the cold path is §7.

## 7. The fix (2026-11-12): PianoDwc3's cold start, proven from ABL fastboot

### 7.1 What changed

PianoDwc3 no longer trusts the state ABL fastboot happens to leave. Design, steps, lines and failure signs are in
`docs/drivers/usb-device-debug.md` §17; the code is `package/PianoDwc3/UsbCold.h`, host-tested by `tests/usb_cold_host.cpp`.

- **Inspection** (read only, every boot, every mode). Before the takeover the USB thread reads GCC (GDSC, branches, RCG rates,
  GPLL0), the USB2 reference clock, the core (its role, and whether the firmware ever ran it as a device), the M31 PHY, the eUSB2
  repeater, the three USB rails over SPMI, and the Type-C orientation (GPIO61; the PMIC's Type-C block only without PianoPower). It
  prints `cold: inherited ...` lines and a verdict. On the next installed boot these lines answer what the normal path leaves.
- **Decision** (`piano-usb-cold=auto`, the default): the cold start runs when the inherited state is not a running device
  controller (11 named reasons). Every check is derived from the fastboot path's probe values, so on the fastboot path the verdict
  is "a running device controller" and the takeover of before runs unchanged.
- **Cold start** c1-c5: power (GDSC, GPLL0's vote under PianoA830's GCC vote lock, branches, RCGs, reference clock), stop (a running
  device controller or xHCI; an opt-in block reset), rails (`ldod2`, `ldob15`, `ldob5` through RPMh DRV-2 under PianoA830's DRV-2
  lock), the repeater (enable, the board's DT tuning when it was off, device mode), and the M31 PHY (reset plus the stock init). Then
  the old takeover runs (UTMI as PIPE, soft reset, device role, VBUS/session override, RUN_STOP). The ADSP, the QMP PHY, the DIO4485
  and the shared CX/MX and bandwidth votes are not touched.
- **No hang, retries**: every poll is bounded. A failed takeover gets another cold start 2 s later, up to 3 per boot. The link
  watch runs a cold start when no host resets the device within 15 s of RUN_STOP (then 30 and 60 s).
- **Test hook and monitor**: `piano-usb-cold-sim=MASK` leaves the hardware the way the normal path may leave it before the decision
  is taken. `usb cold [sim MASK]` does the same live from the console.
- Arguments: `piano-usb-cold=0|1|auto` (auto), `piano-usb-cold-steps=MASK` (0x17), `piano-usb-cold-wait=S` (15),
  `piano-usb-cold-sim=MASK` (0). `piano-usb-cold=0` is the takeover of before, plus the read-only `cold:` lines.

### 7.2 The suspects on the normal path, and what covers each

| # | Suspect | Evidence | Covered by | Reproduced from fastboot by |
|---|---|---|---|---|
| 1 | The repeater's rails (`vreg_l5b_3p1` drives the USB 2 side, `vreg_l15b_1p8`) and the M31's `vreg_l2d_0p88` never voted when ABL does not run USB | link state 3: the device sees an idle bus and the Mac never resets it, which is how a pull-up that does not reach the connector looks; the takeover passed the soft reset, so the M31 had a clock | c3 (votes, read back), then c4, c5 | `sim=16` |
| 2 | The repeater disabled, or left in host mode (its 19.2 MHz clock forced) | as 1 | c4 | `sim=1` |
| 3 | The M31 PHY not initialised (VBUS-valid/suspend bits) | a PHY without its UTMI clock would have failed stage 16; a configured clock with other bits off would not | c5 | `sim=2` |
| 4 | The core left in the host role | the old takeover sets PRTCAPDIR itself: not the cause alone | reported (reason), c2 stops a running xHCI | `sim=4` |
| 5 | Clocks or the GDSC off | excluded for this boot (stage 20 needs them); the worst case anyway | c1 | `sim=8` |
| 6 | The DIO4485 mux, the EUD, the cable | not readable without an I2C transaction (writes to pins and QUP) | not covered: the link watch's `given up` line points here | — |

### 7.3 The test session (RAM boots from ABL fastboot; nothing is written to any partition)

Build the collection once (this tree), then one image per row. Data is volatile (`PIANO_DATA_PERSIST=0`), there is no GPU, and
each image resets to ABL fastboot after 300 s (`piano-reset-to=bootloader` is added by `build-ram-images.py`):

```sh
tooling/k27/build-native-image.sh                     # out/k27/kc-k27-native.bin with this PianoDwc3 (out/ram itself is not used)
cold_img() {   # cold_img NAME "ARGUMENTS"
  PIANO_EXTRA_ARGS="piano-usb=2 piano-usb-hid=1 $2" PIANO_DATA_PERSIST=0 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300 \
  python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin \
    --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt --utc "$(date +%s)" \
    --out "out/ram-cold-$1"
}
cold_img C0  "piano-usb-cold=0"
cold_img C1  ""
cold_img C2a "piano-usb-cold=0 piano-usb-cold-sim=1"
cold_img C2b "piano-usb-cold=0 piano-usb-cold-sim=16"
cold_img C3  "piano-usb-cold-sim=1"
cold_img C4  "piano-usb-cold-sim=16"
cold_img C5  "piano-usb-cold-sim=2"
cold_img C6  "piano-usb-cold-sim=31"
cold_img C7  "piano-usb-cold=1 piano-usb-cold-steps=31"
# each run: the capture first, then the loop (it reads the mailbox after the reset)
python3 tooling/native/usb-console.py --capture 300 &
tooling/native/native-loop.sh out/ram-cold-C3/boot-piano-macos-ram-A-bundled.img
```

| Step | Arguments (besides `piano-usb=2 piano-usb-hid=1`) | Expect | Meaning |
|---|---|---|---|
| C0 | `piano-usb-cold=0` | `cold: verdict (piano-usb-cold=0): ... a running device controller (none): taken as it is`, `cold: no link watch (piano-usb-cold=0)`, then enumeration as before | no regression, and the fastboot path's inherited values (the rails' PPIDs and set points, the repeater's `force_en_5`, GPIO61) recorded for the comparison with the normal path |
| C1 | none (`auto`) | verdict `none`, the old takeover, enumeration, `link watch: a host reset the device: the watch ends (0 cold starts this boot)` | the default changes nothing on the fastboot path |
| C2a | `piano-usb-cold=0 piano-usb-cold-sim=1` | `stage 20 running`, then no host: `idle: ... no-host ...; link state 3 (or 4), configuration 0` | the control: does a disabled repeater reproduce the installed ESP's symptom? |
| C2b | `piano-usb-cold=0 piano-usb-cold-sim=16` | no host as in C2a, or `FAILED at takeover: core soft reset` (no M31 vdd); or enumeration if another master keeps the rails on (then the `inherited rail` lines read on) | the control for the rails |
| C3 | `piano-usb-cold-sim=1` | verdict `the repeater is not enabled/ready; the repeater has the host-mode clock forced`, `cold c4 repeater: ... the board's tuning (DT) ... ready`, enumeration | the repeater step works |
| C4 | `piano-usb-cold-sim=16` | verdict `a rail is off` (when nothing else votes them), `cold c3 rail ...: on` for all three, enumeration | the rails step works |
| C5 | `piano-usb-cold-sim=2` | verdict `the USB2 reference clock is off`, `cold c5 PHY ...: ready`, `soft reset done`, enumeration | the PHY step works |
| C6 | `piano-usb-cold-sim=31` | every reason; `cold c1 ...: the USB30 GDSC is collapsed`, `(5 switched on)`, both RCGs updated; c3-c5 ready; enumeration | the whole cold start from the worst state |
| C7 | `piano-usb-cold=1 piano-usb-cold-steps=31` | `block reset (USB30_PRIM_BCR) done`, then the takeover and enumeration | the opt-in block reset works (the step for a controller the normal path left wedged) |
| C8 | boot C1, then on the Mac `python3 tooling/native/usb-console.py --send 'usb cold sim 31' --capture 120` | the link drops, the Mac enumerates again within seconds, the replay has `cold restart (asked on the console)` and the c-lines | the live restart path, which the link watch also takes |

C0-C1 are unattended. C2-C7 too: if an image does not enumerate, it still resets at 300 s, and `native-loop.sh` reads its mailbox.
C8 needs the console attached. A hang in any of them shows the step: each c-line is printed before that step's writes.

### 7.4 Decision after the session

| Outcome | Next |
|---|---|
| C0, C1 enumerate; C3-C7 enumerate; C2a or C2b shows "no host" | the cause is reproduced and fixed: rebuild the daily ESP from this tree (`build-esp.py --profile daily`, `USING.md` §5), install it, and on its first boot run `usb-console.py --capture 600` (as R4). Expect `verdict ... NOT a running device controller (...)` or the link watch's cold start, then enumeration. Its `cold: inherited` lines record the normal path's state |
| C0, C1, C3-C7 enumerate; neither C2 reproduces | the normal path's cause is not modelled. Rebuild anyway: the cold start and the watch cover more than before, and the installed boot's `cold: inherited` lines and verdict will name its state. If the console still fails there, the watch's `given up` line points at the DIO4485 / EUD path (§7.2 row 6) |
| C1 fails where C0 enumerates | the auto decision or the watch misfires on the fastboot path; its verdict line names the reason. Fix that before any ESP |
| C3-C7: a `cold cN FAILED` or `NOT ready`, or no enumeration | fix that step first. The installed ESP stays as it is (its system runs; only the console is missing) |
| an image stops early (no reset at 300 s, a parked tablet) | button reset; the last `cold` line names the step and its access. Images go back to `piano-usb-cold=0` until it is fixed |
