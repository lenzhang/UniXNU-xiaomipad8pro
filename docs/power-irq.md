# Power: fewer wake-ups (interrupts where they can be had, adaptive polling otherwise)

Status 2026-11-11: **built, host-tested, linked into the native KC; not run on the tablet.** The owner's priority is battery
life; the native drivers woke the CPUs constantly by polling, which keeps them out of idle. This page has the interrupt facts,
the shared helper (`package/common/PianoIrq.h`), what each driver does now and by default, how to measure it (wake-ups per
minute next to PianoPower's battery current), and the change PianoKeyboard should take.

Facts are marked **verified** (read from the DT, the kernel binary, a register already used by a running driver),
**spec** (from Linux's drivers, read as the specification only: GPL, nothing copied), **Ubuntu** (what the piano kernel ran
and measured: `ubuntu-history/kernel.md`, `ubuntu-history/device.md`), **inferred** or **needs-boot**.

## 1. Summary

| Driver | Before | Now (default) | Opt-in | At rest, wake-ups/s (model) |
|---|---|---|---|---|
| PianoDwc3 (USB console, HID, net) | 1 ms after every quiet pass, forever | adaptive wait by the link's state (`UsbIdle.h`) | `piano-usb-irq=1`: the core's SPI 133 | ~1000 → 4 (no cable); 20 / 50 attached; with the IRQ 1 / 5 |
| PianoDwc3's HID thread | 100 ms deadline, forever | 100 ms only while a key/button is held, else 60 s | – | 10 → ~0 |
| PianoInput (power, vol keys) | 10 ms | 10 ms while a key is busy and 1 s after, else 40 ms | `piano-input-idle=MS` | 100 → 25 |
| PianoTouch (NT36532E) | latch polled every 1 ms, every frame read (~120/s) | every announced frame at once while busy; at rest the latch looked at once per 50 ms, a frame read only when announced (~0.7/s idling; `TouchPace.h`, 2026-11-12) | `piano-touch-irq=1`: gpio162 on the TLMM summary SPI | ~1000 → ~20 (one look per idle period, polled or by the IRQ) |
| PianoKeyboard (cover) | (another change already made it adaptive: 4 / 30 / 200 ms) | – | §6: gpio97 on the TLMM summary SPI | 33 idle → ~2 with the IRQ |
| PianoThermal (TSENS) | 1 s; 250 ms from 70 C; 10 ms near a trip or with a cluster above 2.5 GHz | unchanged | `piano-thermal-irq=1`: the four uplow SPIs 771-774, a proof at each start, at rest a 5 s base (`thermal.md` §13) | 1 (100 with a fast cluster) → 0.2 |

`piano-usb-idle=0`, `piano-input-idle=0`, `piano-touch-idle=0` restore the old loops exactly (the A of the A/B; the touch screen's
reads stay on announced frames, `piano-touch-noedge=50` adds back the reads without one).

## 2. Interrupt facts

### 2.1 What the native kernel offers

| Fact | State | Evidence |
|---|---|---|
| UniXNU's `ArmCpu` is linked into the k27 kernel (not a kext) and drives the GICv3 (dist 0x16000000, redist 0x16080000, stride 0x40000) | verified | `k27/build-kernel27.sh` (force_load libArmCpu.a); ADT `/arm-io/intc` |
| It exports `armcpu_spi_attach(intid, edge, handler, ctx, readback, router)` and `armcpu_spi_count(intid)`; **no** detach, mask, enable or disable | verified | symbol table of `kernel-k27-0059-native.macho` (`out/k27/kc-k27-native.nm`), disassembly of the attach |
| attach: INTID 32..1019 (= SPI + 32), ICENABLER, wait RWP, Group 1, priority 0xa0, ICFGR edge/level, IROUTER = the boot CPU, ISENABLER; one handler per INTID (second: `kIOReturnExclusiveAccess`), 16 slots | verified | same disassembly; ArmCpu source |
| The handler runs in ArmCpu's IRQ loop in primary interrupt context, before the EOI; an SPI with no handler is disabled at the GIC and counted (`armcpu_spi_unhandled`) | verified (source) | ArmCpu.cpp `interrupt()` |
| No SPI-level `IOInterruptController` is registered (only ArmCpu's per-CPU `IOCPUInterruptController`), so `registerInterrupt`/`IOInterruptSpecifiers` on a nub reach nothing; the Apple vma2 KC keeps no AIC/GIC kext | verified | `kc-keep.txt`; ArmDTPlatform publishes plain nubs |
| The VM-era virtio kexts (input, Ethernet, virtio block) used no interrupts at all: they polled (NO_INTERRUPT rings) | verified | the VM-era kext sources (read only) |
| No native SPI has been taken yet: PianoA830 links weak refusing `armcpu_spi_*` and polls; NativeUfs is built `NATIVE_UFS_POLL_ONLY`. PianoDwc3 `piano-usb-irq=1` is the first | verified (source) / **needs-boot** | `A830Irq.cpp`, `build-native-ufs.sh` |
| The KC links `_armcpu_spi_attach` from the kernel for our kexts (`resolve()` takes the kernel's symbol first; PianoA830's weak copy stays its own) | verified | `kc-build.py resolve()`; the KC link of this change |

### 2.2 Lines

| Line | INTID | Trigger | Used here | State |
|---|---|---|---|---|
| DWC3 `dwc_usb3` (usb@a600000): event buffer count > 0 and GEVNTSIZ.INTMASK clear | 165 (SPI 133) | level high | PianoDwc3 `piano-usb-irq=1` | DT verified; delivery **needs-boot** |
| DWC3 `pwr_event` / `hs_phy_irq` | SPI 130 / 131 | level | – | DT |
| DWC3 dp/dm/ss PHY wake | PDC pins 14/15/17 → SPI 759/760/762 | both edges / level | – | DT + PDC map |
| TLMM summary (pinctrl@f100000): any GPIO with INTR_CFG enable + target 3 and its status latched | 240 (SPI 208) | level high | PianoTouch `piano-touch-irq=1`; PianoKeyboard (§6) | DT verified; delivery **needs-boot** |
| QUP1 SE2 (touch's SPI engine) / SE6 (keyboard's I2C, bit-banged here) | SPI 355 / 363 | level | – (polled) | DT |
| TSENS "uplow" tsens0..3 (the PDC's outputs for its pins 26-29) / "critical" | 803-806 (SPI 771-774) / SPI 484-487 | level high | PianoThermal `piano-thermal-irq=1` (uplow only) | DT verified (`check-tsens-irq.py`); delivery **needs-boot** (the start's proof says) |
| SPMI arbiter `periph_irq` (all PMIC interrupts: PON keys, PMIC GPIO6) | PDC pin 1 → SPI 746 | level high | – (§2.4) | DT + PDC map |
| Gunyah: every SPI the HLOS DT names reaches the HLOS (Linux takes them all); XNU is the HLOS | – | – | – | **inferred**; nothing in `hyp-evidence` lists the HLOS's virtual interrupts |

### 2.3 TLMM GPIO interrupts (SM8750)

| Fact | State | Source |
|---|---|---|
| GPIO n at 0x0f100000 + n × 0x1000: CTL +0, IO +4, INTR_CFG +8, INTR_STATUS +0xc; 216 GPIOs; reserved (not the HLOS's, never touched) 36-39, 48-51, 74 | verified | DT (`reg`, `gpio-ranges`, `gpio-reserved-ranges`) |
| INTR_CFG: bit 0 enable, 1 polarity, 3:2 detection (0 level, 1 rising, 2 falling, 3 both; polarity set for every edge mode and level-high), 4 raw status (latch even when not enabled), 6 wakeup present, 7 wakeup enable (the PDC route), 10:8 target (3 = apps processor = the summary line); INTR_STATUS bit 0, cleared by writing 0 | spec + verified | sm8750 pin groups (spec); PianoTouch/PianoKeyboard already latch gpio162/gpio97 with this word (enable off, bit 7 as found) |
| The summary asserts while an enabled pin's status is latched; masking (enable off) drops the pin's part, and an edge still latches while masked | spec | pinctrl-msm mask/unmask comments |
| Wake map (TLMM GPIO → PDC pin → SPI): gpio11 (table hall) → 76 → 552, gpio80 (lid hall) → 122 → 633, gpio95 (keyboard status, its wake request) → 128 → 639, gpio97 (keyboard data) → 100 → 611, gpio162 (touch) → 109 → 620 | spec + DT + Ubuntu | sm8750 wake map; `qcom,pdc-ranges`; kernel.md §2.3, §3.3 |
| Linux routes those PDC-mapped GPIOs through the PDC only (`skip_wake_irqs`): INTR_CFG bit 7 (wakeup enable) set if bit 6 reads 1, the type in the PDC and the GIC, the TLMM summary unused; on release it clears bit 7. Without a PDC entry a GPIO uses the summary | spec + Ubuntu | pinctrl-msm (`reqres`, `relres`); kernel.md §2.3 |
| PDC registers (DRV at 0x0b220000): IRQ_i_CFG 0x110 + 4 i, type 2:0 (level low 0, falling 2, level high 4, rising 6, both 7), enable bit 3 from version 3.2 (VERSION 0x1000), else IRQ_ENABLE_BANK 0x10 + 4 (i / 32); the GIC side always rising or level-high | spec | qcom-pdc; kernel.md §2.3 |
| The PDC's "SPI interface" region 0x164400f0 (0x64 bytes): 1 bit per SPI below 800 (1 level, 0 edge), set by firmware; its level bits for GPIO80/95 caused early wakes in Ubuntu and the fix was withdrawn | Ubuntu | kernel.md §2.3, top finding 4 |
| GPIO97 (keyboard data) must not be a wake source (it woke the tablet right after suspending; not a wake IRQ since v1.1.1); GPIO95 wakes only while blanked with the lid open; the lid wakes only on opening | Ubuntu | kernel.md §2.9 |

Choice: the **summary line** for every GPIO here (one SPI, 240; no PDC write, no SPI-interface bit, nothing armed for wake),
with INTR_CFG bit 7 cleared (`tlmm::intrCfg`): Linux's state for a GPIO it does not take through the PDC, so the pin feeds
only the summary. That the summary carries a PDC-mapped GPIO is the TLMM's generic path (*inferred*: Ubuntu ran these pins
through the PDC); a pin that never fires shows up as missed interrupts and is polled. The direct PDC route (one SPI per pin:
611, 620, 633, 639) is the option for system sleep, when a pin must also wake the SoC; it needs the PDC type and enable, bit 7,
the GIC type, and a read of the SPI-interface bits first.

### 2.4 The PMIC keys (why PianoInput stays polled)

The keys are PMIC interrupts (pwrkey `<0 0x13 7 both>`, resin `<0 0x13 6 both>`, vol+ = PM8550 GPIO6 via gpio-keys): they reach the
CPU only through the SPMI arbiter's summary (PDC pin 1 → SPI 746), which needs the PDC pin configured and enabled, the arbiter's
per-APID accumulator enables (v7: chnls + 0x100 + 0x1000 × APID; status +0x104, clear +0x108; owner at intr + 0x1000 × EE +
4 × (APID / 32)) and PMIC writes (INT_SET_TYPE 0x11, POLARITY_HIGH/LOW 0x12/0x13, LATCHED_CLR 0x14, EN_SET/CLR 0x15/0x16;
LATCHED_STS 0x18) (spec). Four writers, all untestable offline, against a driver that runs today read-only: not this change.
PianoInput now logs the two peripherals' interrupt registers once (`irq registers PON: type [11] .., ..., pending [19] ..`), the
facts that step would start from.

## 3. The helper: `package/common/PianoIrq.h`

Header-only, internal linkage (the KC's native linker does not coalesce weak definitions). Pure parts host-tested by
`package/common/tests/run.sh` (every converted driver's build runs it):

- facts: `intid()`, the SPIs, `pdcSpi(pin)`, the wake map, `tlmm::intrCfg(found, trigger, enable)` (the encoding of §2.3: bit 7
  cleared, the bits not named kept as found), `tlmm::reserved()`;
- `Backoff` (fast while active + hold, then mid, then idle), `StormGuard`, `MissGuard`, `WakeStats` (the per-minute window).

With `KERNEL`:

- `Waiter` / `waitFor` / `kick`: a generation word; producers and handlers kick (any context, primary interrupt included); the
  thread sleeps until the generation moves or a timeout (`assert_wait_timeout_with_leeway`, 1/8 leeway from 20 ms: coalescing);
- `SpiLine`: `armcpu_spi_attach` with a storm guard (more than 2000 entries with nothing to deassert in 100 ms disable the line
  with a write-one-to-clear GICD_ICENABLER through its own mapping; then everyone polls);
- `workLoopSource()`: the stock IOKit shape for drivers built around a work loop: a provider-less `IOInterruptEventSource`
  whose `interruptOccurred()` the SPI handler calls (no new subclass, no vtable check);
- `tlmm::Pin`: one GPIO on the summary SPI shared by all kexts. The first kext whose attach succeeds owns the line and a table
  (an `OSData` published as the resource `PianoTlmmIrq`, bytes = GPIO numbers, masked configs, wake words, counters; no pointer);
  the owner first reads INTR_CFG of every HLOS GPIO and attaches nothing if one is enabled to the apps processor by someone else.
  The handler masks each claimed pin that is enabled and latched and kicks its slot. `open()` leaves the pin latched-not-enabled
  (what polling did), `arm()` enables it, the handler masks it, the client consumes the latch as before.

## 4. Per driver

**PianoDwc3** (default: adaptive polling; IRQ opt-in). The wait tier per pass (`UsbIdle.h`): busy 1 ms (an event, moved log
bytes or a kick within 200 ms, a control transfer past SETUP, bytes waiting for a free IN endpoint), open 20 ms (configured, a
port's DTR set or the NCM link up: the kernel-log pump), configured 50 ms, no host 250 ms (nothing configured and no event for 3
s, or the link in suspend/disconnected/early suspend). Writers and readers kick the thread (console, `/dev/pianousb`, HID
replies and reads, the NCM queues), so data never waits for a period. Confident as default: the busy tier is the old loop; the
only cost is that a cable plugged in after more than 3 s of no host can lose the first SETUP (the host retries the enumeration
within seconds). With `piano-usb-irq=1`: the handler masks GEVNTSIZ when events are pending and kicks; the thread unmasks before
each wait (pending events raise the line at once); periods become 10/20/200/1000 ms (bookkeeping only); a storm or three missed
interrupts in a row turn it off for good and the thread polls. Opt-in because no native SPI has been delivered yet and this is
the debug channel. The HID thread sleeps 60 s instead of 100 ms when nothing is held.

**PianoInput** (default: adaptive, idle 40 ms). 10 ms polls while a key reads down, is held or is being debounced and 1 s after;
40 ms otherwise. Host-tested at every phase: every press of 82 ms or more is reported and released, at most 82 ms late (a
deliberate press is 100 ms or more; a 30 ms tap is below the 32 ms debounce at any rate). Confident: same reads, same debounce.

**PianoTouch** (default: paced, idle 50 ms; IRQ opt-in). Every frame (1 ms latch polls) while the panel is busy (a contact, the tip
debouncing or down, the decoder learning its reference, watchdog frames) and 1.5 s after; at rest the thread sleeps until 2 ms
before the next read is due, drops the stale latch and reads on the next fresh edge. Model (host test): 20 reads and ~60
wake-ups/s instead of ~120 and ~1000; a finger is read at most 60 ms late, then every frame. Ubuntu measured touch reads 43-51 %
slow while the system idle state (4.4 ms exit; cluster 1.98 ms) was entered and fixed it with a 2000 µs CPU latency hold per
transfer (kernel.md top finding 8): so the busy state stays on 1 ms polls (short sleeps), and with `piano-touch-irq=1` only the
idle frames' edge comes by interrupt (2 wake-ups per idle read); a bad checksum after an interrupt (a late wake) makes the next
idle edge polled, three missed interrupts turn it off. When the idle work provides a latency-hold hook, the busy state can wait
for the interrupt too, holding it across each read. Default paced: the driver has not run yet, so its first run tests this
too; the IRQ is opt-in. (Ubuntu also notes the firmware idles its scanning only after the BF commands, which PianoTouch sends.)
2026-11-12, after the level-2 runs: the controller holds each frame behind its edge until it is read and scans the next one only
then, and reads inside its ~12.6 ms cycle or ~500 ms apart put it into its watchdog state. So a latched edge is never dropped after
an idle sleep any more (that was why the level-1 run saw ~0.5 edges/s), a read without an edge waits 50 ms while busy (25 ms with
the line still high), at rest the idle period + 50 ms, never more than 250 ms unread (200 ms with the display off); the watchdog is
downloaded again every 10 s, never given up; the k27 0061 latency hold covers each busy read burst when the kernel has it
(`drivers/touch.md` §13).
2026-11-12, after desk1 and desk-diag (the watchdog loop): a frame is read only when the controller announces it (an edge, or the
line still high), never by a timer, as piano's stock driver and Ubuntu read only in their gpio162 interrupt thread. Nobody
touching, the controller idles and announces a frame about every 1.5 s, so at rest the loop reads ~0.7 frames a second (was 10-20)
and looks at the latch once per idle period (20 wake-ups a second; with `piano-touch-irq=1` the interrupt, waited for one idle
period at a time); the display off, once per 200 ms. The old reads without an edge: `piano-touch-noedge=50` (A/B only).

## 5. Measuring: the A/B

Every converted loop logs, every `piano-wake-log` seconds (default 60, 0 off):

```
PianoDwc3: idle: 241 wakeups in 60 s, irq 0 (kick 3, timer 238; passes busy 2 open 0 configured 0 no-host 238; polled; link state 0, configuration 0; SPI 133 count 0)
PianoInput: idle: 1502 wakeups in 60 s, irq 0 (polled: 3 at 10 ms while a key was busy, 1499 at 40 ms idle)
PianoTouch: idle: 3600 wakeups in 60 s, irq 0 (1200 frame reads; 1200 idle sleeps; 0 reads on an interrupt's edge, 0 of them bad; 0 missed; pace idle)
```

next to PianoPower's `PianoPower: [30s] battery: <mV> mV, <mA> mA (...), ...` every 30 s (charging natively is not working yet:
the current is negative even on the Mac's cable, `charging.md`). The old loops (A) print no wake-up line; their rate is known
(PianoDwc3 ~1000/s, PianoInput 100/s).

Test images (RAM boot only, `piano-reset-after`; built 2026-11-11 from this change on main 63ed7c0: KC (14 native kexts) sha256
`8373b972095cd9d4`, kernel `kernel-k27-0059-native` `d475f824a4f234af`; the GPU profile's Data seed and trust cache from
`out/turnip-a830/stage`; `PIANO_VMM_PRESENT=0`); in each directory of the main checkout's `out/` the macOS RAM image is
`boot-piano-macos-ram-A-bundled.img` (images are never committed):

| Dir (`out/`) | Arguments added to the GPU profile | Reset after | sha256 (16) |
|---|---|---|---|
| `ram-power-A` | `piano-power=1 piano-usb=2 piano-usb-hid=1 piano-input=1 piano-usb-idle=0 piano-input-idle=0` (old loops) | 900 s | `a0bc7689da77d0a2` |
| `ram-power-B` | `piano-power=1 piano-usb=2 piano-usb-hid=1 piano-input=1` (new defaults) | 900 s | `c81433f67d035290` |
| `ram-power-C` | B + `piano-usb-irq=1` | 900 s | `e696c6126c4fbfcf` |
| `ram-touch-pace` | `piano-usb=2 piano-touch=1` | 240 s | `1519b848bca57df5` |
| `ram-touch-irq` | `piano-usb=2 piano-touch=1 piano-touch-irq=1` | 240 s | `b073dcac98371c02` |
| `ram-power-B-dump` | the dump image (reset after 1 s, `data-persist=0`) for all of them | 1 s | `4f7e672c3e476852` |

(all with `panic_on_crash_disabled=1`). Rebuild: `PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=900 PIANO_EXTRA_ARGS="..."
tooling/k27/build-native-image.sh`.

Plan (owner present, tablet in ABL fastboot, same brightness, Wi-Fi/none, the screen on at the login window, no input):

1. **No cable (the battery case).** For A, B, C: `tooling/native/native-loop.sh out/ram-power-X/boot-piano-macos-ram-A-bundled.img
   out/ram-power-B-dump/boot-piano-macos-ram-A-bundled.img`; unplug the cable once the login window shows, plug it back after the
   15 min (the loop waits for ABL fastboot up to 150 s past the reset). From `logs/native-runs/*-loop/xnu-console.txt`:
   `grep -E 'battery:|idle: [0-9]+ wakeups' ...`. Compare the mean battery current from minute 2 to minute 14 (A vs B vs C) and the
   wake-up lines: expected PianoDwc3 ~240/min no-host (B), ~60/min (C), PianoInput ~1500/min, HID thread silent.
2. **Mac cable, console open** (B and C): `tooling/native/usb-console.py`; type in the monitor (`usb` shows `wait: polled, adaptive,
   tier open` / `wait: interrupt, ...; SPI 133: attached, N handled, 0 strays, 0 missed`), `log` output flows, the Mac's keyboard and
   pointer (`piano-usb-hid=1`) feel as before. Close the terminal: tier `configured`. Expected wake-ups: ~3000/min open (20 ms), ~1200/min
   configured (50 ms), both with B; C only changes the configured tier (200 ms).
3. **C first-run lines**: `PianoDwc3: irq: SPI 133 (INTID 165, level) attached (0): readback 0xa003 (group 1 1, enabled 1, edge 0,
   priority 0xa0), router 0x...; events wake the USB thread`, then `idle:` lines with `irq` > 0 and a growing `SPI 133 count`.
   Failure signs: `irq: off (storm: ...)` (the line stays up: polled from then on), `irq: off (events pending ... no interrupt
   arrives)` (not delivered under Gunyah: polled), or a hang right after the attach line (a storm the guard did not catch: drop
   `piano-usb-irq`).
4. **Buttons** (B): press vol+, vol-, power briefly (~0.1 s) 10 times each: every press is reported (`piano-input=2` logs them).
5. **Touch** (`ram-touch-pace`, then `ram-touch-irq`; touch.md §8 for the rest): `frames at 60 s: ~20 reads/s` at rest,
   ~120 reads/s while touching; `PianoTouch: idle:` ~3600/min (pace), ~2400/min (irq) at rest; with the IRQ first `frames: gpio162
   interrupt on the TLMM summary (SPI 208, INTID 240): slot 0, ...`, failure signs `frames: gpio162 interrupt NOT used (gpioN is
   enabled to the apps processor by someone else ...)`, `... no interrupt in 3 waits in a row`, many `of them bad`.

## 6. PianoKeyboard: the change to take (another change owns the driver)

Taken 2026-11-12 (steps 1-8; `piano-keyboard-irq`, default 0; `docs/drivers/keyboard.md` §6b); 9 (gpio80 by interrupt) not
taken: the hall pin stays read only, sampled every 500 ms; 10 and 11 are for the suspend work.

Its serving loop already has the one place for this, `PianoKeyboard::waitForEdge(ms)` (adaptive 4 / 30 / 200 ms, Wn8030Piano.h
PollPlan). Route: **gpio97 on the TLMM summary (SPI 208, INTID 240)**, not its PDC line (PDC 100 → SPI 611): no PDC or
SPI-interface write, the same table as PianoTouch's gpio162, and with bit 7 cleared gpio97 is no wake source (§2.3). With the
helper:

1. `#include "../common/PianoIrq.h"`; in `tooling/k27/build-native-piano-keyboard.sh` run `"$D/package/common/tests/run.sh"`
   before the keyboard tests.
2. Boot argument `piano-keyboard-irq` (default 0 until PianoTouch's or PianoDwc3's interrupt run has shown native delivery; then 1).
3. Members: `pianoirq::tlmm::Pin fPin = {}; pianoirq::MissGuard fMiss = pianoirq::missGuard(3); bool fIrqLive = false;` and counters
   `fIrqWakes`, `fIrqMisses`.
4. End of `armIrq()` (after its own INTR_CFG and INTR_STATUS writes, which `open()` repeats with bit 7 cleared):
   `char why[160] = {}; fIrqLive = gArgs.irq && fPin.open("PianoKeyboard", 97, pinVa(kIrq), pianoirq::tlmm::Trigger::rising, why, sizeof why);`
   and log either `serving: gpio97 interrupt on the TLMM summary (SPI 208): slot N` or `... NOT used (why)`.
5. `waitForEdge(ms)`: when `fIrqLive && fPin.live() && ms`: `const uint32_t seen = fPin.gen(); fPin.arm(); const bool woke =
   fPin.wait(seen, ms); if (!woke) fPin.mask();` instead of `IOSleep(ms)` (`woke`: `++fIrqWakes; fMiss.delivered();`); then the latch
   read and clear as now. A timed-out wait with the latch set is a miss: `if (!woke && edge && fMiss.miss()) { fIrqLive = false;
   note("... no interrupt in 3 waits: polled"); }`. When `fPin.live()` turns false (the line's storm guard), set `fIrqLive = false`
   and log it once. The latch keeps working throughout; nothing else changes.
6. With the interrupt live the idle and slow periods only bound bookkeeping: use `min(fPoll.readEveryMs(mode), hallMs (500))`
   as the timeout in idle/slow mode (the hall sensor is sampled every 500 ms; the keepalive every 12 s); keep 4 ms in active
   mode only while `fReadAgain` bursts run (else the interrupt as in idle). At rest: ~2 wake-ups/s instead of 33 (idle) or 5
   (slow).
7. The wake-up line: add `irq %u` after `in %u s` (the format of the others: `PianoKeyboard: idle: N wakeups in 60 s, irq M (...)`).
8. Test: with PianoTouch also on, the second opener finds the table (`slot 1`); typing after a pause shows no lost first key.
9. gpio80 (lid hall, active low; PDC 122 → SPI 633 for a future wake on opening only) can take the same path instead of the
   500 ms samples: `fHallPin.open("PianoKeyboard", 80, hallPageVa, pianoirq::tlmm::Trigger::both, ...)`, arm it while waiting,
   read the level after each latch (the two-sample filter stays). That writes the hall pin's INTR_CFG, which the driver now
   only reads: the owner's decision. The wait's timeout can then grow to the read-every interval (1 / 5 / 10 s).
10. gpio95 (keyboard status, the MCU's wake request; PDC 128 → SPI 639) needs no interrupt while the system runs (the loop logs
   its level); it matters only for a system sleep, through the PDC, armed while blanked with the lid open (Ubuntu's rule) and
   with the SPI-interface bit for SPI 639 checked first (it caused Ubuntu's early wakes).
11. Not part of this change but a wake-up each 12 s: Ubuntu removed the community 12 s keep-alive (the stock driver keeps the
   MCU awake only during authentication; kernel.md §2.9).

Both clients share the summary line: the first `open()` owns it; up to 8 GPIOs.

## 7. Risks and fallbacks

- **A storm on CPU 0** if a level line stays up with nothing to deassert: the guard disables it at the GIC within ~2000 entries;
  a hang would show right after the attach line; drop the `-irq` argument.
- **Delivery under Gunyah** is inferred, not seen: the clients detect missed interrupts and poll.
- **GICD/TLMM mappings**: the guard maps the distributor (a second mapping beside ArmCpu's; writes are write-one-to-clear); the
  TLMM owner maps the whole block and reads INTR_CFG of every non-reserved GPIO once (Linux leaves only `gpio-reserved-ranges`
  to other owners; a GPIO protected otherwise would abort that read: needs-boot, and only with an `-irq` argument).
- **USB enumeration after a long no-host period** may lose the first SETUP (poll mode): the Mac retries.
- **Touch latency at rest**: up to one idle period (50 ms) plus a frame; `piano-touch-idle=20` halves it, `=0` is the old loop.
- **Buttons**: a press shorter than ~80 ms may be missed at rest (`piano-input-idle=10` is the old 10 ms).
- Other wakers not touched here (owned elsewhere): PianoPower (30 s), PianoA830's status rows, the net thread (1 s), NativeUfs's
  polled completions, the kernel's own timers.

## 8. Next

1. After C's run: `piano-usb-irq=1` and `piano-touch-irq=1` default on if delivered; PianoKeyboard §6.
2. NativeUfs's completion interrupt (SPI 265) now that the attach exists (its mask needs a device-side mask instead of
   `armcpu_spi_mask`).
3. The hall sensor (gpio80, both edges) on the summary line instead of 500 ms samples.
4. The PMIC keys through the SPMI arbiter (§2.4), after the read-only register survey.
5. After the uplow test (`thermal.md` §13.8): `piano-thermal-irq=1` default on if the four proofs and the test pass.
