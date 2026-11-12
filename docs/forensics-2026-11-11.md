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

**Update 2026-11-12, after queue7** (§7.5). C2a and C2b reproduce the installed ESP's symptom, and C3 and C4 cure it. C0, C1, C5 and
C7 pass. C6 (every leftover at once) stopped the whole system: the cold start left the core in the host role, and the takeover
soft-reset it there before switching it to device. The fix is in this tree (§7.6: the role switch in c2, the takeover's gate, the
role rule). The ESP waits for the re-test images C6b and C6c (§7.7).

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
- **Cold start** c1-c5: power (GDSC, GPLL0's vote under PianoA830's GCC vote lock, branches, RCGs, reference clock), stop and the
  device role (a running device controller halted; in the host role the xHCI halted, then the programming model's switch, or the
  block reset when that switch is not shown done or c1 found the GDSC collapsed: §7.6), rails (`ldod2`, `ldob15`, `ldob5` through
  RPMh DRV-2 under PianoA830's DRV-2 lock), the repeater (enable, the board's DT tuning when it was off, device mode), and the M31
  PHY (reset plus the stock init). Then the takeover runs: its gate checks the device role first (in every mode), then UTMI as PIPE,
  soft reset, high speed, VBUS/session override, RUN_STOP. The ADSP, the QMP PHY, the DIO4485 and the shared CX/MX and bandwidth votes
  are not touched.
- **The role rule** (since C6, §7.6): no device register is read or written while GCTL reads another role, no xHCI register outside
  the host role. The cold start, the gadget core and the kernel's register access each enforce it.
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
| 4 | The core left in the host role | not the installed ESP's symptom (that boot reached stage 20). But C6 showed that the old takeover's order on a host-role core stops the whole system (§7.6) | c2's role switch and the takeover's gate (§7.6) | `sim=4` (C6c); with the collapse `sim=31` (C6b) |
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

The session ran on 2026-11-12 (queue7; results in §7.5):

- C0 and C1 enumerate.
- C2a and C2b reproduce the installed ESP's "no host".
- C3 and C4 cure it; C5 and C7 enumerate.
- **C6 stopped the whole system.**

So the cause is reproduced and the cure works, but the cold start was not safe on a core left in the host role. This tree fixes that
(§7.6). C6b and C6c (§7.7) must pass before the daily ESP gets `piano-usb-cold=auto`; until then images and the ESP stay at
`piano-usb-cold=0`. **Later on 2026-11-12 both passed** (each one cold start after 15 s, then the planned reset), so ESP v3
(`persistent-boot.md` §11) has the default `piano-usb-cold=auto`, and its daily profile refuses a collection without the role rule.

| Outcome of C6b / C6c | Next |
|---|---|
| both enumerate (C6b with `block reset c1 found the GDSC collapsed`, C6c with `soft switch PROVEN` or with the block reset) | rebuild the daily ESP from this tree (`build-esp.py --profile daily`, `USING.md` §5) with the default `piano-usb-cold=auto`, install it, and capture its first boot with `usb-console.py --capture 600` (as R4). Expect `verdict ... NOT a running device controller (...)` or the link watch's cold start, then enumeration. The `cold: inherited` lines record the normal path's state |
| C6b enumerates; C6c stops the system after `soft switch PROVEN` | the proof is not enough on this core, so the soft switch is not to be trusted. Until the code takes the block reset for every role switch, the ESP carries `piano-usb-cold-steps=0x1f` (C7's: the block reset always, proven from the device role by C7 and from the host role by C6b). Report C6c's last lines |
| C6b stops the system | the block reset of a core that was a host is not safe either. Keep `piano-usb-cold=0` on the ESP and report the last `cold c2 role:` line. The installed ESP's own symptom (C2a's and C2b's) does not need the host-role path, so the next candidate is a build whose c2 refuses the host role (no takeover, a line saying so) |
| `cold c2 FAILED: the xHCI does not halt within 100 ms` | by design the controller is left alone (no reset under a DMA master that may still run); report the `cold c2 role:` line's USBSTS |
| `FAILED at takeover: the device role` | the gate refused; its `takeover: role:` line says why |
| an image stops early (no reset at 300 s) | button reset; the last `cold` line names the step and its access |

### 7.5 The session's results (queue7, 2026-11-12)

Logs: `logs/usb-console/20261112-07*-port0.txt` and `logs/native-runs/20261112-07*-loop/`. C2a, C2b and C6 have no USB console
stream; their lines are in the loop's mailbox dump. All runs booted from ABL fastboot without the GPU: WindowServer exits without an
accelerator, as expected.

| Step | Result | Key lines |
|---|---|---|
| C0 (`piano-usb-cold=0`) | PASS: enumerates | inherited state: GDSC on, role 2 (device), M31 ready, repeater ready and enabled, `ldod2` 880 mV / `ldob15` 1800 mV / `ldob5` 3104 mV on, Type-C CC1 attached with VBUS (sink); verdict `... a running device controller (none): taken as it is`; `no link watch`; SET_CONFIGURATION 1 |
| C1 (`auto`, the default) | PASS: enumerates | the same state and verdict; the watch armed, then `a host reset the device: the watch ends (0 cold starts this boot)` |
| C2a (`=0`, `sim=1`) | the control: **the installed ESP's symptom reproduced** | `NOT a running device controller (the repeater is not enabled/ready; the repeater has the host-mode clock forced): taken as it is anyway`, then `no-host ...; link state 3, configuration 0` for the whole run |
| C2b (`=0`, `sim=16`) | the control: no host, as in C2a | the three DRV-2 votes withdrawn (TCS 2, 1 ms each). Only `ldob5` (the repeater's vdd3, 3104 mV) went off: `ldod2` and `ldob15` have another voter. Repeater `NOT ready/enabled, status 00`; `no-host ..., link state 3, configuration 0` |
| C3 (`sim=1`) | PASS: the repeater step cures C2a's state | `cold c4 repeater: ... the board's tuning (DT), device mode: written; after status 80 enable 80 force 00/00 tune 0e 01 07 03 06: ready` (ABL had `[51] 0a`, `[57] 03`); 1 cold start; enumerates |
| C4 (`sim=16`) | PASS: the rails step cures C2b's state | `cold c3 rail vreg_l5b_3p1 ... before status 03 enable 00 (OFF); voted 3104 mV, mode 7, enable: done; after ... on`. c4 then found the repeater ready again (tuning kept); enumerates |
| C5 (`sim=2`) | PASS: the PHY step | verdict `the USB2 reference clock is off`; `cold c5 PHY ...: before ... [54] 49 ..., after 01 00 6b 07 00: ready`; `soft reset done`; enumerates |
| C6 (`sim=31`) | **FAIL: the whole system stopped**; the Gunyah watchdog reset it (ABL after 72 s, not at the planned 300 s) | every reason in the verdict; c1 `the USB30 GDSC is collapsed`, `RCG usb30_master ... (updated)`, 5 branches switched on; **`cold c2 stop: ... was role 1 (GCTL 00101005): nothing ran; no block reset ...; now GCTL 00101005`**; c3 `ldob5` OFF to on, c4 ready (DT tuning), c5 ready; `soft failures 0`; `soft reset done ...: GCTL 00101005`; last line `stage takeover: run`. See §7.6 |
| C7 (`=1`, `steps=31`) | PASS: the opt-in block reset works | `cold c2 stop: ... was role 2 (GCTL 00102005): nothing ran; block reset (USB30_PRIM_BCR) done`; afterwards GCTL `00102004`, TX FIFOs `00000043 0043030d ...`, RX `00000313`; c4 and c5 ready; enumerates |

C8 (the live `usb cold sim 31`, attended) was not part of queue7. It takes C6's path, so it waits for the fixed build and C6b.

### 7.6 C6: why the system stopped, and the fix

**What the run shows.** Sources: `logs/native-runs/20261112-072600-loop/xnu-console.txt` (the mailbox dump), compared with C5
(`...071956...`) and C7 (`...073140...`).

1. **The core came up in a mixed state.** The test hook set GCTL to the host role (bit 4), then collapsed the GDSC with RETAIN_FF
   set (bit 8). After c1 powered it up again, the core read `GCTL 00101005` and `GSTS 7e800001` (CURMOD host).
   - Retained as the firmware left them: GUCTL1, DCFG, DCTL, DEVTEN, DALEPENA and GEVNTSIZ.
   - Not retained: GEVNTADR read 0. The TX FIFO sizes read `00000025 00258082 00a700c4 016b8103 026e028b ...` and RX `00000028`.
     Every other run shows `00000043 0043030d 03500083 ...` and RX `00000313`, which C7 shows are the reset values after the block
     reset. These are not device sizes: TXFRAMNUM (bit 15 on DWC_usb31) is set on FIFOs 1, 3, 5 and 7, and FIFOs 6 and 7 have no
     depth.
2. **c2 left the host role.** It read role 1 and the xHCI's run bit 0 (`nothing ran`). The block reset was opt-in, so it took none.
3. **The takeover inverted the switch.** In this order, all in the host role:
   - the halt: a DCTL write and DSTS polls;
   - the soft reset: DCTL.CSFTRST, `soft reset done in 20000 us: GCTL 00101005`;
   - the TX FIFO readout, still `00000025 00258082 ...`.

   Only stage 17, after the soft reset, wrote PRTCAPDIR = device (in the gadget's init). DCFG and about 16 endpoint commands followed
   (DEPSTARTCFG, DEPCFG, SETTRANSFRESOURCE). All of them completed: a timeout or a command error ends the stage with
   `FAILED at takeover: endpoint commands`, and stage 18 printed instead. Stage 18's QSCRATCH session override printed its readback.
4. **The log stops inside a bounded stage.** The last line of the whole log is `PianoDwc3: stage takeover: run` (stage 19). Stage 19
   cannot run on: STARTTRANSFER on EP0 waits at most 10 ms, then RUN_STOP, then DSTS is polled for at most 50 ms, then
   `stage 20 running` goes out through the note thread. Neither that line nor a timeout note appeared, and no other thread printed
   anything. In C7 the root mount's `dev_init` and `nx_mount` lines follow `stage takeover: run` in the same burst; in C6 they never
   came.
5. **The watchdog reset it.** The tablet was back in ABL fastboot 72 s after `fastboot boot`, not after the planned PSCI reset at
   300 s. 0056's watchdog pings had stopped, and the Gunyah watchdog bit 32 s after the last one.

**What that rules out.**
- DMA from a running xHCI: c2 read its run bit 0, and the test hook never starts it.
- A device-register access in the host role that stalls the bus by itself: the halt, the soft reset, the probe's DCFG, DCTL, DSTS
  and DEVTEN reads, and c2's own DCTL and DSTS reads were all made in the host role, and all of them returned.
- An endpoint command in the host role: none was issued before PRTCAPDIR read device, and the ones that followed completed.
- A bounded poll that failed: it would have returned, and its note would have been printed.

**The cause (an inference: the log cannot say which access it was).** The device side was started in a core that was switched from
host to device after its soft reset, not before. So it had no device soft reset in the device role, it still had the host role's
FIFO sizes, and it kept whatever internal state the collapse with retention left.

The DWC3 programming model orders the host-to-device switch like this: halt the xHCI, soft-reset the core where the core needs it,
set PRTCAPDIR = device, then DCTL.CSFTRST, then the device init. Linux's dwc3 core (read as the behaviour reference) follows that
order. It added these resets after someone reported a lockup without them on another SoC. C6 inverted the last two steps.

Stage 19 is where the device controller first does DMA: STARTTRANSFER makes it fetch EP0's TRB, and RUN_STOP starts the link and the
event writes. One of these, or a CPU read behind them, stopped the interconnect for every CPU: no thread printed again, and the pings
stopped. §2.3's caveat applies, since one stuck CPU strands its own timers, the pet's included. Here, though, the other threads went
silent too (the root mount was in progress), which points at more than one CPU.

**The fix.** In this tree: `package/PianoDwc3/UsbCold.h` (`toDevice()` and `ensureDevice()`), `Dwc3Gadget.h` and `PianoDwc3.cpp`.
It is host-tested and not yet run on the tablet.
- **c2 always leaves the device role.**
  1. In the host role it first stops the xHCI: it clears RS and the interrupt enable, then waits at most 100 ms for HCHalted. If the
     xHCI does not report halted, c2 fails and writes nothing more, so nothing is reset under a DMA master that may still run.
     HCHalted is 1 by default and 0 only while the xHCI still executes.
  2. Then it switches as the programming model orders: both PHYs awake, then GCTL.CORESOFTRESET where the core needs it (DWC_usb3,
     and DWC_usb31 before 1.90a, as in Linux and PianoUsbHost; piano's 2.00a does not), then PRTCAPDIR = device. It waits up to
     10 ms for GSTS.CURMOD, and only then sets DCTL.CSFTRST (250 ms bound, 50 ms settling).
- **The switch counts only when it is shown.** Every check must pass:
  - PRTCAPDIR and CURMOD read device;
  - CSFTRST clears;
  - DEVCTRLHLT is set;
  - TX FIFOs 0-6 are a device layout: each has a depth, they are contiguous from word 0 and inside the TX RAM, TXFRAMNUM is clear,
    and the RX FIFO has a depth. C7's sizes pass this check; C6's fail it.

  Otherwise c2 takes the block reset (`USB30_PRIM_BCR`). It also takes it when the xHCI's registers do not answer, when the role is
  neither host nor device, when c1 found the GDSC collapsed (C6's mixed retained state), and with steps bit 3. After the block reset
  the core comes up in the device role (C7: `00102004`); if it does not, c2 writes PRTCAPDIR. c2 fails if GCTL does not read the
  device role at the end.
- **The takeover's gate.** `ensureDevice` runs before stage 12's first device register, in every mode: with `piano-usb-cold=0` and
  after a hand-off's reclaim too. A core that does not read the device role gets the same switch there. If it still does not, the
  takeover stops with `FAILED at takeover: the device role` and touches no device register.
- **The role rule everywhere.**
  - The cold start's inspection, the test hook and c2 read the device registers only in the device role, and the xHCI's only in the
    host role.
  - The gadget's halt, soft reset, init and run refuse outside the device role, and its init no longer switches the role.
  - The kernel's `coreRd` and `coreWr` remember the role from every GCTL access ("unknown" after a hand-off's release). They refuse a
    device-register access outside the device role, and an xHCI access outside the host role, and count each refusal:
    `role rule: N core accesses refused` appears in the takeover's notes and in `usb`.
- **Tests** (`package/PianoDwc3/tests/usb_cold_host.cpp`):
  - The model now has piano's reset values (C7), C6's retention, and the host role's FIFO sizes. It flags every device-register
    access outside the device role, an xHCI access outside the host role, a role change under a running xHCI, a block reset of a
    running controller, and RUN_STOP with the host role's FIFO sizes.
  - sim 4, sim 31 and host boot paths (the xHCI halted or running), against every combination of the two unknown FIFO behaviours,
    all end in the device role. The gadget then replays the takeover to RUN_STOP on the same model.
  - The edges: an xHCI that does not halt, registers that do not answer, the OTG role and no role, and DWC_usb31 1.80a with
    GCTL.CORESOFTRESET.
  - The gate, with and without a cold start.
  - C6's own order is flagged, and a mutant without the role switch fails 83 checks, one that reads the device registers in any role 64.

### 7.7 Re-test images C6b and C6c (the same base as §7.3)

Use the `cold_img` function of §7.3 with a collection built from this tree:

```sh
cold_img C6b "piano-usb-cold-sim=31"
cold_img C6c "piano-usb-cold-sim=4"
```

The images' PianoDwc3 arguments are then `piano-usb=2 piano-usb-hid=1 piano-usb-cold-sim=31` (C6b) and
`piano-usb=2 piano-usb-hid=1 piano-usb-cold-sim=4` (C6c), with `PIANO_DATA_PERSIST=0 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300` as
before. For each run, start `python3 tooling/native/usb-console.py --capture 300 &`, then
`tooling/native/native-loop.sh out/ram-cold-C6b/boot-piano-macos-ram-A-bundled.img` (C6c alike).

**C6b: expected lines**, in order:
1. C6's verdict and c1 lines.
2. `cold c2 role: found role 1 (host, GCTL 00101005); xHCI halted already (...); soft switch not tried (...); block reset c1 found the GDSC collapsed (the core kept only its retention flops); now GCTL 00102004 (role 2: device)`
3. `cold c2 role: TX FIFOs 0-6 after the block reset 00000043 0043030d ...: a device layout`
4. `cold c2 stop: ... was role 1 (GCTL 00101005): nothing ran (the xHCI was halted); block reset (USB30_PRIM_BCR) done; now GCTL 00102004 (role 2) ...`
5. c3-c5 ready; `cold: done: ..., block reset; soft failures 0`.
6. No `takeover: role:` line; `soft reset done ...: GCTL 0010200x`; `stage 20 running`.
7. Enumeration; the watch ends with 1 cold start.

C6b answers whether the block reset gets a console from a collapsed host-role core: C6's state.

**C6c: expected lines**, in order:
1. Verdict `NOT a running device controller (the core is not in the device role)`.
2. `cold: inherited core ... (role 1: host) ...; the device registers are not read (not the device role); xHCI halted: ...`
3. Either `cold c2 role: found role 1 ...; soft switch PROVEN (... CURMOD device 1, CSFTRST cleared 1 ..., halted 1); block reset not taken; now GCTL 0010200x (role 2: device)` with the FIFO line `after the soft switch ...: a device layout`, or `soft switch NOT proven: ...` followed by the block reset.
4. The rest as in C6b, then enumeration.

C6c answers whether the programming model's switch works on this core without the block reset. If the switch is not proven, it
shows which check failed and what FIFO sizes a DCTL.CSFTRST leaves.

Optional, after both pass: C6d, `cold_img C6d "piano-usb-cold=0 piano-usb-cold-sim=4"`. It shows the takeover's gate on its own:
`takeover: role: found role 1 ...`, then enumeration without a cold start.
