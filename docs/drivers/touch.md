# Touch: NT36532E on GENI SPI (`PianoTouch`)

Status 2026-11-11: **level 1 ran on the tablet** (`package/PianoTouch`, listed in `configs/k27-native-kexts.txt`, inert without
`piano-touch=N`): GENI SPI loopback passed, NT36532 identified, the BOE image downloaded with its CRC, scanning, the unit's
calibration loaded, frames with good checksums. Facts still marked *inferred* or *needs-boot* below wait for a run with a finger.
**Level 2** (`piano-touch=2`): the touch screen for macOS, which has no touch screen interface of its own, as what it understands
from a HID pointer (§14): tap = click, hold = right click, drag = left drag, two-finger scroll (fine, natural), two-finger tap =
right click, palms rejected, a pinch recognised (sent only on request). Multi-contact tracking (§14.2) feeds it; Apple's
multitouch path is not used. **Ran 2026-11-11** (§15.1): contacts, taps, drags and two-finger scrolling worked (12238 frames,
1 bad checksum); the Y axis was reversed on this BOE unit (now `PIANO_TOUCH_INVERT_Y=1` at build, §5); a second run lost touch
after ~5 minutes to the controller's watchdog state (0xfd) that three downloads in a row did not clear.
**The watchdog loop (2026-11-12, desk1 and desk-diag; fixed, host-tested, not run, §13)**: nobody touching, the controller went
into its watchdog state ~2 s after every download and every minute or two at rest, so the firmware was downloaded again every
~10 s (desk-diag: 25 downloads in its first 10 minutes). Cause (evidence: every watchdog onset in every run followed reads the
controller had not announced; inference: those reads are the trigger): the loop read frames by timers without an edge, and an
idling controller (op mode 2, nobody touching) announces only one frame every ~1.5 s. Fix: **a frame is read only when the
controller announces it** (an edge, or the line still high), as piano's stock driver and its Ubuntu read only in their gpio162
interrupt thread; the reference of a quiet controller from its few frames; the stock start sequence (stylus scanning off, the idle
baseline updated with 1, op mode 2). The 20-minute unattended proof image is §15.2. The attended list for the next session is §15.
Battery: at rest an idling controller is read ~0.7 times a second (one look at the latch per 50 ms; §13,
[`../power-irq.md`](../power-irq.md)); `piano-touch-irq=1` takes the edge from the TLMM interrupt.
Display rotation (2026-11-11, host-tested, default off): `piano-touch-rotate=1` maps each contact into the rotated desktop before
the gestures (§5, [sensors.md](sensors.md) §5a.3).
Taps (2026-11-12, host-tested, not run): attend1's short taps became drags (11 of its 16 drags under 310 ms, most pressed 25-100 ms
before the lift and ending 6-70 px from their start: a lifting finger's centre moves on this sensor, whose nodes are 53 px apart).
The slop is 64 px now (32 before) and a drag starts only after the finger stayed past it for 80 ms (`piano-touch-drag-delay`), or
went past three times the slop: lifted before, the touch is the tap at its landing. Holds that wobbled past 32 px (drags of 700-1050
ms ending 20-52 px away) stay right clicks. A two-finger tap may take 700 ms (500 before: two of attend1's took 517-526 ms). The tap
line says how far the finger got (`gesture: tap -> click (N ms, M px at most from the landing[: past the slop, lifted before the
drag])`). The service answers `PianoTouchContact` (`PianoTouchClient.h`): the last frame with a finger, which PianoKeyboard asks
before it sends a touchpad tap ([keyboard.md](keyboard.md) §6a).

## 1. Purpose and gate

The panel as an input device, so the login window and the desktop work by touch ("login by touch", plan §4(d).3). The keyboard
cover ([keyboard.md](keyboard.md)) and the Mac's keyboard and mouse over USB (`piano-usb-hid=1`) are the other input paths.

The NT36532E is host-processed ("THP"): it streams raw 40 x 60 electrode matrices and reports no coordinates. The matrix to
contact work runs **in the kext** (`TouchMatrix.h`, Project-SunUEFI's decoder for this panel); no userspace daemon.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Controller | NT36532E, a TDDI (drives the panel too), THP: no flash for the touch firmware, no coordinate mode | DT `touchscreen@0` `novatek,nt36532e` with `panel = <&panel>`; `touch-native-plan.md` §1 | verified |
| Bus | QUP1 SE2 `spi@a88000` (0x4000), CS0, `spi-max-frequency` 19.2 MHz, mode 0 | DT | verified |
| SE firmware | SPI protocol, FIFO interface disabled ("GSI only"): SE DMA or GPI DMA, no FIFO | SunUEFI `docs/devel/boot/uefi-console.md`; plan §1 | verified elsewhere; read again in step 1 |
| SE DMA with the FIFO off | works on SM8750 QUP1 SE4 (OnePlus 13T) | UniXNU-Odin3 `PaganiInput.cpp` | **needs-boot** on SE2 |
| Pins | GPIO40 MISO, 41 MOSI, 42 CLK, 43 CS; function `qup1_se2` (1), 6 mA, no bias | DT `qup-spi2-*-state` | verified (function number inferred) |
| IRQ | GPIO162 rising edge = frame ready (latched in TLMM INTR_STATUS; with `piano-touch-irq=1` also on the TLMM summary SPI 208, INTID 240, while an idle frame's edge is waited for). Its PDC line (PDC 109 → SPI 620) is not used | DT `interrupts = <162 1>`; `../power-irq.md` §2.3 | verified (DT); summary delivery needs-boot |
| Panel ID | GPIO100 (`novatek,lcd-id-gpios`): 0 CSOT, 1 BOE; this unit is BOE | DT; plan §1 | verified (DT) / needs-boot (level) |
| Power, reset | none of its own: powered with the panel (lit by ABL/UEFI); reset and boot over SPI | DT (no supply, no reset GPIO); SunUEFI | verified (DT) |
| Clocks | GCC vote 0x52008 bits 18-21 (QUP1 wrapper) + 24 (`wrap1_s2`); CBCRs 0x23140/0x23144/0x23148/0x2315c, SE2 0x1827c; rate: QSPI-ref RCG 0x188c0 through the divider 0x1828c | K90 (same SE, same SoC), 13T QUP1 S4 code, mainline names | **inferred** |
| SMMU | QUP1 stream 0xa3 (SE DMA); GPI is 0xb6 (unused here) | DT `geniqup@ac0000 iommus` | verified |
| Protocol | trim ID `0e .. .. 32 65 03`; cascade: event buffer 0x11c400, poll info 0x1093d8; reset state 0xa0..0xaf, calibrated 0xa3 | SunUEFI, ShengNovatekTouch | verified on piano by SunUEFI (download, idle frames) |
| Frame | 257-byte transport block + THP packet (checksum, 40 x 60 int16); BOE payload 5192 B (frame 5449 B); 120 Hz | plan §1; Ubuntu | verified (Ubuntu) |
| Images | `novatek_nt36532_piano_fw_{boe,csot}.bin`, 245,760 B each (sha256 `b7999389…` / `14b30bb7…`) | xiaomipad-8pro-mainline `firmware/piano-input.json` | verified; both download byte-exact into the host model |
| Orientation | X along the 60 columns (3200 px), Y along the 40 rows (row-major matrix); BOE: panel Y = processor Y (no flip), CSOT: reversed; X never flipped. **This BOE unit: Y reversed** (the owner at level 2, 2026-11-11; piano's Ubuntu needed a per-device invert_y on it too) | piano-thp `frame.cpp`, `orientation.hpp`; attend1 | verified on the tablet: `PIANO_TOUCH_INVERT_Y=1` (§5) |
| Calibration | per unit, private: electrode centres `row_map[40]`, `column_map[60]` (52-54 px steps, not uniform), maxima 21360 / 32000 (pixels x 10), edge band 600 (60 px), gain 4350 / base 2700; `project_vendor_id` 26 = BOE | the unit's Ubuntu file (§7) | verified (file); its arrays are never written down here |
| Frames at rest | nobody touching: ~0.5 gpio162 edges a second, frame type 3, every read with or without an edge had a good checksum (1074 of 1074 in 210 s), peak delta 9-12 at rest | the first level-1 run, 2026-11-11 | verified; the few edges were the loop's own doing (next row) |
| Frame handshake | active (a finger, a start): gpio162 rises when a frame is ready and the frame waits until the host reads it; the next one is scanned only then: edges = reads, at most ~79 a second under a finger (a 5.1 ms read, ~7.5 ms scan). Idling (op mode 2, nobody touching; about 12 frames after a start): one frame announced every ~1.5 s (39 edges a minute), the event buffer rewritten by the controller in between (the counter advances, reads catch half-written frames). The watchdog state (transport 0xfd/0xfe, the line rising all the time, ~150 a second, no frames until a download) began after reads the controller had not announced, in every run (§13) | attend1, attend2 (2026-11-11), desk1, desk-diag (2026-11-12) | counts verified; the cause **inferred**; §15.2's run tests it |

## 3. Specification sources

Behaviour only, nothing copied from GPL code: piano's Ubuntu (`nt36532e-thp` kernel transport, `device/piano-thp` processing)
as described in the local plan `touch-native-plan.md` (not published) and, since 2026-11-12, as its transport's source and README
and piano's stock driver (MiCode `p81/nt36532`) behave (§13, `package/PianoTouch/UPSTREAM`); Project-SunUEFI's piano touch producer,
which ran this controller natively in UEFI over GPIO SPI (firmware loaded, idle baseline; touching broke the checksums: GPIO SPI is
too slow).

## 4. Components reused (provenance in `package/PianoTouch/UPSTREAM`)

| Piece | From | How |
|---|---|---|
| `GeniSpi.c/.h`, `tests/geni_spi_host.c` | NativeGeniSpi b5399b5 (QcomGeniSpi lineage), BSD-2-Clause-Patent | unchanged |
| bring-up order, image layout, chip tables, matrix decoder, SPI by hand | Project-SunUEFI `touch-product`, BSD-2-Clause-Patent | re-expressed (`Nt36532.h`, `TouchMatrix.h`, `Se2Bus.h`) |
| SE DMA through stream 0xa3, cache maintenance, vote bits | UniXNU-Odin3 `PaganiInput` (13T), UniXNU-sm8750-common `PeripheralIO.h` | re-expressed |
| start sequence (fw info, idle baseline 0x19, host-command handshake) | ShengNovatekTouch 69fdd62, Apache-2.0 (NOTICE kept) | re-written |
| bypass context bank | `package/PianoDwc3/SmmuStream.h` | included |
| HID class, publication, personality | `package/PianoKeyboard` / the VM-era HID class | same pattern |
| calibrated position, edge mapping, calibration check | piano-thp `nvt_touch_core.cpp` (Apache-2.0, modified from xiaomi-sheng-thp by ianchb), `calibration.cpp`, `orientation.hpp`, `panel.cpp` (MIT) | re-written (`TouchMatrix.h`, `gen-touch-calibration.py`); the centroid's window widened (§14.2) |
| scroll resolution declared through the Wheel's physical extents | `package/PianoKeyboard/HidDescriptors.h` (`makeTouchpadDescriptor`, [keyboard.md](keyboard.md) §6a) | the same formula (`TouchHid.h` `makeTouchDescriptor`) |
| multi-contact tracker, palm rejection, gestures | new for piano | `TouchTrack.h`, `TouchGesture.h` |

Not used: the multi-contact trackers of ShengNovatekTouch and piano-thp, and piano-thp's filters (`TouchTrack.h` is written anew).

## 5. IOKit family / ABI

`PianoTouch : IOService` on `IOPlatformExpertDevice`, a kernel thread, polled; in the power tree (`pianopower::join`, §16). At
level 2 a `PianoTouchHID : IOHIDDevice` (vendor 0x1af4, product 0x5054), matched by a codeless `IOHIDEventDriver` personality; the
334-slot vtable is checked at the KC link. Its descriptor (`TouchHid.h` `makeTouchDescriptor`, 89 bytes): mouse usage, 3 buttons,
absolute X/Y 0..32767 (the tablet shape that logged in by tap in the piano VM), and the Wheel and AC Pan as relative bytes with
physical extents that declare 45 counts per inch (Physical -2822..2822, Unit inch, Unit Exponent -3: IOHIDEventService's
quotient 254000 / 5644 = 45; PianoKeyboard's touchpad formula, [keyboard.md](keyboard.md) §6a). Report: 7 bytes {buttons, X LE16,
Y LE16, wheel, AC Pan}, no report ID. With `piano-touch-pinch=1|2` (124 bytes) a second application collection with the eight
keyboard modifiers: report ID 1 the pointer (8 bytes), report ID 2 the modifiers (2 bytes).

**Display rotation** (`piano-touch-rotate=1`; without it both inputs are refused as before and the reports are unchanged). The
rotation is CGDisplayRotation's (degrees clockwise, 90 = the panel's left edge up), told by the rotation agent `piano-rotated`
([sensors.md](sensors.md) §5a.2) after it read it back:
- `callPlatformFunction("PianoTouchRotation", false, (void *)(uintptr_t)degrees, (void *)source, 0, 0)` for a kernel caller, and
  the property `piano-touch-rotation` through `setProperties` (0, 90, 180, 270) from root or the console user (the agent runs as
  the logged-in user; xnu's `"console"` privilege). Both only record the request (`kIOReturnBadArgument` for another value,
  `kIOReturnNotPrivileged` for another caller, `kIOReturnUnsupported` without the argument). `piano-touch-rotation` is also
  published: the rotation in use.
- A new rotation takes effect when a gesture begins (its first contact while no gesture runs), never in the middle of one
  (`pat_latch`). Every contact is mapped into the rotated desktop (`package/PianoAuto/TouchRotation.h`, `pat_rotate`) **before** the
  gestures, so taps land under the finger and every direction (drag, the scroll's wheel up / AC Pan right, the pinch) is the
  desktop's; each report is scaled over the rotated desktop's size. At 0 the bytes are `tabletReport`'s. One line per change:
  `rotation: 90 degrees from this touch on (was 0; from setProperties, change 1)`.

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-touch=N` | 0 (inert: probe declines, nothing mapped) | 1: steps 1-7, statistics, contacts and the gestures they would make in the log, nothing for macOS; 2: the same plus the HID pointer and its gestures (§14). At 2 positions are not logged (taps on an on-screen keyboard at the login window would spell a password) unless `piano-touch-trace=1` |
| `piano-touch-stop=N` | 0 | stop before step N (2: survey only; 4: bus up and self-tested, controller untouched) |
| `piano-touch-bus=N` | 0 | 0 GENI, else SPI by hand; 1 GENI only; 2 SPI by hand only (no SMMU access then) |
| `piano-touch-khz=K` | 9600 | GENI SPI rate, 1000..19200 |
| `piano-touch-seirq=N` | 2 | engine interrupt outputs while polled: 2 on (13T and K90 keep them on, their status bits follow them; ArmCpu keeps unused SPIs disabled at the GIC); 1 off; 0 off, then on if the DMA loopback times out |
| `piano-touch-smmu=0` | 1 | do not write the SMMU (read only) |
| `piano-touch-loopback=0` | 1 | skip the engine's loopback self-test |
| `piano-touch-mode=V` | 2 | operating-mode command value (65536: not sent); 2 is the stock driver's idle mode (1 its active one) |
| `piano-touch-baseline=0` | 1 | no idle-baseline command (default: sent with 1 as the stock driver and Ubuntu; 0 until 2026-11-12) |
| `piano-touch-stylus=1` | 0 | stylus scanning left on (default: off, host command 0x04 = 0 first, as the stock driver without a pen and Ubuntu) |
| `piano-touch-threshold=T` | 200 | contact threshold of the decoder |
| `piano-touch-flip=B` | auto | auto: this unit's Y from the build (`PIANO_TOUCH_INVERT_Y`, §7) if given, else by the panel (BOE none, CSOT Y reversed; the calibration's panel wins over gpio100 here, its `invert_y` over both); 0..3: bit 0 X, bit 1 Y flipped (wins over all of them) |
| `piano-touch-poll=MS` | 1 | edge polling interval while the panel is busy |
| `piano-touch-idle=MS` | 50 | at rest the latch looked at once per MS (20..200): an announced frame waits at most that long (§13); 0: every frame at once, never at rest |
| `piano-touch-irq=1` | 0 | at rest the edge by the TLMM summary interrupt (waited for one idle period at a time) instead of a look per idle period (§13) |
| `piano-wake-log=S` | 60 | the `idle: N wakeups in S s` line every S seconds (0 none; shared) |
| `piano-touch-trace=1` | 0 | more frame headers; contact positions also at level 2 |
| `piano-touch-rotate=1` | 0 | follow the display's rotation (§5): `PianoTouchRotation` and `piano-touch-rotation` taken, contacts mapped from their touch-down |
| `piano-touch-noedge=MS` | 0 | A/B only: also read without an announced frame, as before 2026-11-12 (busy MS after the last read, 25..200; at rest the idle period + 50 ms; the display off every 200 ms; never in the 2 s after a start until its first edge). 0: never (§13) |
| `piano-touch-gestures=0` | 1 | level 2 without the gestures: the first contact is the tip (down presses, moves drag, up releases), as before this change |
| `piano-touch-hold=MS` | 500 | a finger held this long within the slop: right click (200..3000; 0: never, and a long press is a click) |
| `piano-touch-slop=PX` | 64 | panel pixels (~4.7 mm) a finger may move and still tap or hold; past it a drag, a scroll or a pinch (4..200; 32 before 2026-11-12) |
| `piano-touch-drag-delay=MS` | 80 | past the slop this long before a drag starts (or past three times the slop at once); a lift before is the tap (0..500; 0: at once, as before 2026-11-12) |
| `piano-touch-scroll=PX` | 4 | finger travel per wheel / AC Pan count (1..64): smaller is finer and faster |
| `piano-touch-scroll-res=N` | 45 | the scroll resolution the descriptor declares, counts per inch (10..900): macOS's scroll acceleration works in it, so higher is slower; 45 gives a finger's millimetre the wheel travel of PianoKeyboard's touchpad (§14.3) |
| `piano-touch-scroll-invert=1` | 0 | both scroll axes reversed (natural scrolling is macOS's setting, one for every device) |
| `piano-touch-pinch=N` | 0 | 0: a pinch is recognised and nothing is sent; 1: Command held while the wheel turns (apart: up); 2: Control (Accessibility Zoom's modifier). 1 and 2 add the keyboard modifiers to the descriptor (§5) |
| `piano-touch-palm=N` | 48 | a blob of N nodes or more (at half the threshold) is a palm (8..2400; 0: no palm rejection by size or peaks) |
| `piano-touch-confirm=N` | 2 | frames a new contact needs before it counts (1..5) |
| `piano-touch-fast=P` | 2 x threshold | a first peak of P or more counts in its first frame (the first report a frame sooner; 0: never) |
| `piano-touch-cpu-hold=US` | 2000 | while the panel is busy no CPU idle state slower than US to leave (`piano_cpuidle_latency_hold`, compiled in only for a kernel that has it, §7); 0: none |
| `piano-touch-pm=0` | 1 | not in the power tree: system sleep and restart do not stop the frames (§16) |
| `piano-touch-display=0` | 1 | ignore NativeGopDisplay's `piano-display-power`: touch reports with the display off as well (§16) |

## 7. Build and kernel collection

`tooling/k27/build-native-piano-touch.sh`: host tests (GeniSpi; the NT36532E model over whole transfers and bit by bit; both
real images downloaded into the model with its CRC check; the probe after a suspend; decoder, tip debounce, the multi-contact
points, tracker, palms and every gesture on synthetic frames (§14.5), the HID descriptor and reports, the rotated report and the
gestures under rotation, clocks, the frame pace with and without edges, the burst hold, the power states), the pinned images staged
into the ignored `out/native-kexts/PianoTouch/firmware` and `.incbin`'d, the objects. `tooling/k27/build-native-image.sh` builds it
with the list; PianoTouch links after PianoA830 (it takes A830's `sm8750_gcc_vote_owner` around the 0x52008 write). Without the
images the build warns and step 5 stops on the target ("no usable image"). The CPU latency hold is compiled in only when the
kernel the KC will link (`K27_KERNEL`, default the 0059 one) exports `piano_cpuidle_latency_hold` (k27 0061+): the KC linker takes
no weak imports. The build prints `PianoTouch: CPU latency hold compiled in (kernel-k27-0062-native.macho exports ...)` or `no CPU
latency hold (...)`.

**Calibration.** The unit's Ubuntu touchscreen calibration is a private JSON file (schema 1), made by xiaomipad-8pro-mainline
`tools/extract-thp-calibration.py` from the vendor `piano_nova_<panel>_thp_config.ini`. Its path is `PIANO_TOUCH_CALIBRATION`, in
the environment or `~/.config/piano/env`; no path, hash or value of it is in this repository. The build passes it through
`tooling/k27/gen-touch-calibration.py`, which checks it as piano-thp does (40 x 60, 10 units per pixel, strictly increasing maps
inside their maxima, edge band below half an axis) and writes `out/native-kexts/PianoTouch/gen/PianoTouchCalibration.h` (ignored).
The build prints `calibration: unit file (panel BOE, sha256 xxxxxxxx), electrode maps, edge 60 px`, or `calibration: none (...):
linear map` when the file is absent, unset (`PIANO_TOUCH_CALIBRATION=` in the environment) or invalid (with the reason). With it, a
contact's position is piano-thp's: the electrode positions weighted by max(delta - 25, 0) over the peak's 3 x 3 nodes (widened toward a stronger neighbour, §14.2), then the
edge stretch, then the orientation, then / 10 to pixels. Without it, the linear map over node centres.

**This unit's Y orientation** (`PIANO_TOUCH_INVERT_Y`, in the environment or `~/.config/piano/env`: `1` Y reversed, `0` as the
processor gives it; private like the calibration, never committed). The first level-2 run showed this BOE unit's Y reversed under
the panel's default (no flip); piano's Ubuntu needed a per-device invert_y on it as well. The generator writes it as
`kUnitInvertYOverride` (with or without a calibration file) and `piano-touch-flip=auto` takes it before the file's `invert_y` and
the panel; an explicit `piano-touch-flip=0..3` still wins. The build prints `...; Y reversed by PIANO_TOUCH_INVERT_Y=1`, the boot
`orientation: this unit's Y reversed by the build (PIANO_TOUCH_INVERT_Y=1): ...` and `orientation: flip X 0, Y 1 (this unit: Y
reversed (PIANO_TOUCH_INVERT_Y=1 at build); ...)`. For this unit: add `PIANO_TOUCH_INVERT_Y=1` to `~/.config/piano/env`.

## 8. Hardware test plan

Fails safe: RAM boot, `piano-reset-after=180`, back in ABL fastboot, the installed system untouched. Images as the installed
`gpu` profile, so the login window comes up:

```sh
PIANO_VMM_PRESENT=0 PIANO_GPU=2 PIANO_RESET_AFTER=180 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-touch=1" tooling/k27/build-native-image.sh   # level 1
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
grep 'PianoTouch:' logs/native-runs/*-loop/xnu-console.txt | tail -80      # or live: tooling/native/usb-console.py
```

During the run, once the login window shows (verification, not calibration: the unit's maps are built in): press a fingertip as
far into each corner as it goes (top-left, top-right, bottom-left, bottom-right), then once in the middle, then drag left to right.
Level 2: the images and the attended list of §15 (tap, hold, drag, two-finger scroll and tap, palm, the display off, a suspend).
Level 1 ran on 2026-11-11 (§2 "Frames at rest"): everything up to the frames passed, nobody touched the panel.

Level 1, expected in order: `start: piano-touch=1 ... flip auto ...`; `calibration: unit file (panel BOE, sha256 xxxxxxxx),
electrode maps, edge 60 px`; `step 1` with six `pins:` lines, `gcc: vote 0x52008 = ...`,
`gcc: SE2 clock: ...` (a hardware-gated AHB branch reads `hw-gated`, which counts as running), `se2: FW_REVISION ... (protocol 1: SPI) IF_DISABLE 00000001 (FIFO disabled: SE DMA)` (or `registers not
read (a QUP1/SE2 clock is off)`), two `firmware CSOT|BOE: 245760 bytes ... parsed`; `step 2`, `smmu: ... bypass set`; `step 3`,
`geni: votes ... (set under the lock | already set) ... branches on`, `geni: SE2 clock N Hz ... divider ...`, `geni: SE DMA through ...`,
`geni: init (SE DMA, engine interrupt outputs on): 0; ... DELAY 00000000`,
`geni: loopback PASSED`, `geni: pins qup1_se2`; `step 4`, `identify (GENI): NT36532 ... id 0e .. .. 32 65 03, table 0 (E),
cascade, event buffer 0x11c400`; `step 5`, `firmware: lcd-id gpio100 = 1: BOE panel`, `firmware: download 1 done ... reset state
0xa0..0xaf`; `step 6`, `start: scanning ...; payload 5192 ...; ... calibrated ...; stylus off taken, baseline command (1) taken,
mode 2 taken`; `step 7`, `frames: gpio162 CTL ... (rising edge latched since step 5, not enabled), level 0, latched 0|1`,
`orientation: flip X 0, Y 0 (BOE panel: as processor Y; panel BOE by the calibration), position from the unit's electrode maps
and edge mapping`, `frames: a frame read only when the controller announces it (a gpio162 edge, or the line still high after 25
ms), none without one: ...`, `frames: first gpio162 edge after N ms`, three `frame N ... checksum ok` lines, `frames: the
controller quiet (no edge for 400 ms) 1: N frames since the last start ...` (about 12 after a start), `matrix: reference of N frames
...` (formed once the controller went quiet, or of 32), `frames at 5 s: ...` every 5 s for 30 s then every 60 s (nobody touching,
the controller idles: ~1 read/s, ~1 edge/s; while touching ~80 of each), each with `frames: reads N: E on an edge, L on the line
still high, B without either` (B stays 0), and per contact `contact N down at (x, y) px, peak P,
area A nodes, frame F` / `contact N up at (x, y) px after T ms (F frames, peak up to P, area up to A nodes)` with the
`gesture: ...` lines of §14.4; every minute
`idle: N wakeups in 60 s, irq M (R frame reads; S idle sleeps; ...)` (~1200 at rest, R ~40). Pass: checksum ok >= 99 %;
the corner presses land within about 10 px of the panel's corners, (0, 0), (3199, 0), (0, 2135), (3199, 2135) (the edge stretch
reaches the border), the middle one near (1600, 1068), Y growing downward. A corner on the wrong side means the orientation is
wrong (`piano-touch-flip=2` reverses Y, `=1` X, `=3` both); corners 20-60 px short of the border mean the edge mapping is not in
use (`calibration: none` in the log). Level 2 adds `HID device: touch screen pointer (absolute X/Y, 3 buttons, wheel and AC Pan at
45 counts/inch) ...` and `event service IOHIDEventDriver published on the touch screen`; the pointer lands under the fingertip
(within about 10 px, under a millimetre) and the gestures of §14 work (§15 lists the checks).

| Line | Meaning, next step |
|---|---|
| no `PianoTouch: start` | KC without PianoTouch, or no `piano-touch` argument |
| `calibration: none (linear map)` / `... INVALID: none` | the KC was built without the unit's file (or with an invalid one): positions are linear, the edges fall short |
| `WARNING: lcd-id gpio100 says X but the calibration's project_vendor_id N is Y` | the pin and the file disagree on the panel: orientation follows the file, the firmware image the pin; one of them is wrong (check the panel, re-extract the file) |
| `smmu: no bypass ...` / `... did not take` | SE DMA impossible this boot; GENI falls back to SPI by hand |
| `geni: ... branches NOT all on` | the 0x52008 bits do not start the SE2 clocks (inferred bit/branch wrong): fallback |
| `geni: the SE2 clock rate is unknown` | RCG source not decoded (raw values in the line before): fallback |
| `geni: the engine holds no SPI firmware` / `a command is running` | SE2 not usable as SPI here: fallback |
| `geni: loopback FAILED (rc -110 ...) ... moved 0/0` | the DMA never completed: SE DMA does not run on SE2 this way (try `piano-touch-seirq=0`/`1` once; else GPI DMA is the next step); fallback |
| `geni: loopback FAILED (rc 0 ...): k/64 bytes back` | bytes move but wrong: packing or cache maintenance |
| `bus: GENI not usable: SPI by hand ...` | the fallback ran: steps 4-6 still answer the controller questions; frames then ~10/s, checksums break while touching |
| `identify (GENI): NO NT36532 ... id ff ff ...` (or `00 ...`) then `identify (by hand)` | the engine does not reach the controller (pins, rate, CS) |
| `FAILED at step 4: identify ...` | no answer by hand either: controller unpowered or not on these pins |
| `firmware: lcd-id gpio100 = 0: CSOT` | unexpected for this unit: check the pin before trusting the image choice |
| `firmware: download N FAILED: attempt 3, stage S (rc R)` | stage 1/2/4 with rc -5: transfers fail; 5: auto copy never ends; 8: reset state never 0xa0..0xaf (CRC mismatch, data corrupted: try `piano-touch-khz=4800`) |
| `stylus off NOT taken` / `baseline command (1) NOT taken` / `mode 2 NOT taken (continuing)` | the firmware did not clear the command byte; frames may still come (SunUEFI sends no baseline command): look at the frame lines |
| `geni: transfer N: the engine is still busy` | a command still running 2 ms after the previous DMA completed: the engine hangs |
| no `first gpio162 edge`, `0 on an edge`, no frames read at all, also under a finger | GPIO162 does not latch: nothing announces a frame, so nothing is read (§13). Every run so far latched its edges; `piano-touch-noedge=50` reads without them (the rules that ended in the watchdog loop) |
| `frames: no frame for N ms with a contact or a gesture on (K times): taken as the lift` | an active controller stopped announcing under a finger (an edge lost?): the contact ended as a lift, the touch comes back with the next edge |
| many `checksum BAD` | reads not synced to the edge, or slower than a frame: `piano-touch-khz=19200`; reads without an announced frame (B > 0) catch half-written frames |
| `frames: 11 watchdog frames in a row (state S); the first read X, T ms after the read before it (...); N frames since the last start, M ms ago`, then `watchdog (state S, N frames in a row): the download again (K for the watchdog...)` | the controller's watchdog state (0xfd; 0xfe: idled first, swrst <- 0xaa): downloaded again at once, then every 10 s while it comes back, never given up. The first line says what preceded it (X: on an edge, on the line still high, or without either): `without either` means a read the controller had not announced (§13, B > 0: `piano-touch-noedge` set?); `on an edge` with B = 0 would refute §13's inference (record the line, the `frames: reads` line and what was going on) |
| `geni: a QUP1/SE2 clock branch is off ...` | someone cleared PianoTouch's 0x52008 votes or the branch stopped: voted again, that transfer skipped (an SE2 access then would have stalled the bus) |
| `FAILED at step 7: frames: 50 frame reads in a row failed` | the transport died mid-run |
| level 2 without `event service ... published` | IOHIDEventDriver did not attach (personality/IDs) |
| `contact N down` lines but no pointer movement in macOS | the reports go nowhere: `HID: handleReport` failures, or the descriptor not taken (compare with `piano-touch-gestures=0`, the plain tip) |
| every tap a `contact N is a palm` | the finger's blob is larger than `piano-touch-palm` (the `area A nodes` of its down line): raise it |
| taps logged `gesture: drag` | the centroid moves more than the slop for longer than the drag delay: `piano-touch-slop=96` or `piano-touch-drag-delay=150`; the tap lines' `M px at most` say how far taps go |

## 9. Fallback

`piano-touch=0` (the default) removes the driver from the boot; `piano-touch-bus=2` keeps the controller questions answerable
without SE DMA. Input meanwhile: the keyboard cover, or the Mac's keyboard and mouse over USB.

## 10. Open questions

1. Does SE DMA run on SE2 with the FIFO disabled, and do its status bits latch with the interrupt outputs off? (step 3's loopback)
2. Are the inferred clock facts right (vote bit 24, branch 0x1827c, the rate)? (step 3 lines)
3. The orientation and the edge mapping natively (the verification step): Ubuntu's BOE no-flip and edge stretch, not yet seen here.
4. Frame rate and checksum rate at 9.6 vs 19.2 MHz; CPU cost of the polled thread.
5. Multi-touch: contacts, palms and gestures through the HID pointer are done (§14, not run). Apple's own path (frames to
   HSTouchHIDService, as the VM-era HID kext fb07165 did) would give macOS's trackpad gestures; not attempted.
6. Charger noise (USB cable always in): the stock driver and Ubuntu send the external-power state after every start (host
   command 0x53 online / 0x51 offline, `{0x53, 0x00}` at event + 0x50, no command-byte handshake); not done (PianoPower knows the
   USB input's state). The first suspect if §15.2's run still shows a watchdog after announced reads only.
7. Finger peaks and blob sizes on the real panel (the defaults: fast confirmation at 400, palm at 48 nodes) are guesses until
   §15's `contact N down ... peak P, area A nodes` lines show them; nobody touched the panel in the level-1 run.
8. Does the controller idle its scanning at rest and return to edges on every frame under a finger? The counts say it idles
   (one announced frame every ~1.5 s, §13); its wake latency under a finger (the model's 30 ms) and the scan modes (4 active, 2
   idling: `piano-touch-trace=1`'s frame headers) are for §15.2's run and the attended one. Whether the stylus command and the
   baseline value 1 change the idle behaviour: open (they came with the announced-frame rule, as the stock driver sends them).
9. Does the TDDI keep its firmware across a display-off and a suspend (§16's probe says which, per event)?

## 11. Keyboard cover

Its own page: [keyboard.md](keyboard.md) (`package/PianoKeyboard`, WN8030 over software I2C, `piano-keyboard=N`).

## 12. Licence note

New code under BSD-2-Clause-Patent; reused pieces under their own licences as listed in `package/PianoTouch/UPSTREAM` (one
Apache-2.0 source, NOTICE kept). Linux `spi-geni-qcom` (GPL-2.0) was not read for this work; K90's code is facts only. piano's
stock `nt36xxx` driver and Ubuntu's `nt36532e-thp` transport (GPL-2.0) were read on 2026-11-12 for their behaviour only (§13: the
read pace, the recovery, the start sequence); no code was taken (`package/PianoTouch/UPSTREAM`).
The panel images and the unit's calibration stay out of every repository: the calibration is read at build time from the path in
`PIANO_TOUCH_CALIBRATION` and only its panel and an 8-digit hash prefix are logged.

## 13. The frame pace: a frame read only when announced (`piano-touch-idle`, `piano-touch-irq`, `piano-touch-noedge`)

**The rule** (2026-11-12, after desk1 and desk-diag; `TouchPace.h` `readOn`, host-tested, not run): a frame is read only when the
controller announced it, by a rising edge of gpio162 latched in TLMM (raw status, configured from step 5 on) or by the line still
high 25 ms after the last read started (an edge missed). Never by a timer:
- busy (a contact, a gesture, watchdog frames, the decoder still learning while the controller is active) and 1.5 s after: the
  latch looked at every 1 ms, every announced frame read at once (~79 a second under a finger);
- at rest: the latch looked at once per idle period (`piano-touch-idle`, 50 ms; with `piano-touch-irq=1` the TLMM interrupt is
  waited for, one idle period at a time): an announced frame waits at most that long; an idling controller is not read at all;
- the display off: the latch looked at once per 200 ms;
- a latched edge is never dropped: it is cleared when taken, and once after each download (the edges before it were the old
  firmware's or its watchdog state's; the display's return and a resume no longer clear it, which desk-diag's display-on restart did:
  `no edge within 2000 ms of start 1`);
- the controller **quiet** (no edge for 400 ms; active again after 3 edges each within 400 ms of the one before): `frames: the
  controller quiet (no edge for 400 ms) N: M frames since the last start T ms ago; read only when it announces a frame`. A reference
  still being learned is then formed from the frames so far, at least 4 for the type and 4 samples (`TouchMatrix.h` `matrixSettle`;
  piano-thp takes the first idle frame as its reference): `matrix: reference of N frames of type 3 (40 x 60), formed once the
  controller went quiet`. Without it the 48 frames of the type probe and the reference would take ~70 s at one per 1.5 s;
- a contact or a gesture without a frame for 250 ms (an active controller announces one every ~12.6 ms): taken as the lift (an
  empty frame through the tracker and the gestures, so a short touch still clicks), anything still going on released (`frames: no
  frame for N ms with a contact or a gesture on (K times): taken as the lift`; piano-thp releases a stalled stream after 100 ms);
- `piano-touch-noedge=MS` (25..200; default 0): also the reads without an announced frame of before (busy MS after the last read,
  at rest the idle period + 50 ms, the display off every 200 ms, none in a start's first 2 s before its first edge), for an A/B
  only; gpio162 not mapped (no latch): one read per 200 ms, the level-1 loop. SPI by hand: at most one announced frame per 100 ms.

**Why** (the runs; evidence is a count or a quoted line, inference says so):

| Observation | Source | Kind |
|---|---|---|
| Active (a finger, a start), the controller announces every frame and scans the next one after the read: edges = reads. attend1's touch session read every frame on its edge (`79 reads/s, 79 edges/s`, `39/39`, `24/24` at 209-329 s, the counter gaps unchanged at 1113): no watchdog in those 300 s | attend1 `20261111-232732` | evidence |
| Idling (op mode 2, nobody touching) it announces one frame every ~1.5 s: desk-diag at rest 39 edges and ~574 reads without either a minute at the 100 ms rest pace (206-266 s); desk1 16 edges in 30 s at the 200 ms display-off pace; level 1 0.6 edges/s. Its event buffer still changes in between: the packet counter advances ~0.9 per read without an edge | desk-diag `20261112-093853`, desk1 `20261112-081349`, `20261111-203446` | evidence |
| After a download it announces about 12 frames, then idles: +51 edges in the minute with download 3 (+39 without); the reference (48 frames) took 2.3-2.4 s, i.e. the busy pace's 50 ms reads without an edge supplied it | desk-diag 86-266 s, `matrix: reference ... after N good frames` 2.4 s after each start | evidence (counts); which reads: inference |
| Every watchdog onset followed reads the controller had not announced. desk-diag: 1.8-2.2 s after a start (start 4 at 289.3 s, `11 watchdog frames in a row` at 291.1; 6 at 309.3, 311.1; 8 at 338.8, 340.6; download 17 at 446.4, 448.5), while the busy pace read every 50 ms without edges; at the 100 ms rest pace after 51-107 s (129.8, 181.7, 288.9 s); with the display off (200 ms reads) after 34 s at boot and 39 s after the display went off (647.4 → 686.5 s); desk1 the same at ~35 s; attend2 and the 01:32 run at once under 12 ms reads without edges | the runs above, `20261111-234057`, `20261112-013224` | evidence (the coincidence, every onset) |
| Reads of an idling controller catch half-written frames: 42 bad checksums among 6429 data frames; transport blocks `ff fd fd fd fd fd ff fd` with an advancing packet counter (159, 443, 501, 672, 858: not a watchdog run) | desk-diag frames 352-1130 | evidence |
| In the watchdog state the line rises all the time: every 200 ms display-off read on an edge (`4 reads/s, 4 edges/s` from 746 s), 1209 blocks in 7.8 s at the busy pace | desk-diag | evidence |
| piano's stock driver (MiCode `vendor_xiaomi_proprietary_touch-driver` `p81/nt36532/nt36xxx.c`) and its Ubuntu (`nt36532e-thp` `core.c`) read a frame only in their gpio162 interrupt thread. Stock downloads again after more than ten 0xfd/0xfe blocks in a row (0xfe: idled first); its ESD check (the 0x77 handshake and a 1.5 s IRQ timer) is compiled out (`NVT_TOUCH_ESD_PROTECT 0`): no periodic host command or heartbeat. Ubuntu has no recovery at all, and it is the touch path of its daily-use releases | the drivers' sources, read for behaviour only | evidence (their code's behaviour) |
| Their start sequence: stylus scanning off (BF04 = 0), the idle baseline updated (BF19 = 1), op mode 2, which the stock driver's THP mode 53 calls idle (0 active: op mode 1; 1 idle: op mode 2). PianoTouch sent BF19 = 0 (sheng's) and no BF04 | stock `nt36xxx.c` 4114-4152, `nt36xxx_ext_proc.c`; Ubuntu `README.rst` | evidence; whether they mattered here: open |
| The level-1 run read without edges every 200 ms for 300 s with no watchdog and no bad checksum | `20261111-203446` | the counterexample: that boot ran without PianoPower, PianoBacklight and the cover; why its controller tolerated the reads is open |

Inference: a read the controller did not announce, while it idles, collides with its own work on the event buffer and sometimes ends
in its watchdog reset (the touch firmware lost: the ROM's 0xfd/0xfe blocks and edges until a download), right after a start's idle
entry almost always. The 2026-11-11 explanations (a frame starved for ~0.5 s; reads inside the scan cycle) are kept in the model,
but every case they explained read without an announced frame too. Neither the idle pace (reading too rarely: the stock driver
does not read an idling controller at all), a missing heartbeat (none in the stock driver), the SPI clock (9.6 MHz in every run,
good checksums on announced frames) nor a misread idle state (the 0xfd runs are the stock driver's watchdog pattern, recovered only
by a download) fits as well.

Host model (`tests/touch_host.cpp` `FeedModel`, `simulateFeed`, stepped every millisecond, the loop's passes at its own times; the
loop's decision is `TouchPace.h` `readOn` itself): active as before (a frame ready 6.5-11 ms after the previous read, waiting; a read
before it or a frame unread for 450 ms: 0xfd); idling from 300 ms after a start or the last finger (one frame announced every
1.5 s; a read it did not announce: 0xfd with odds of 1 in 6 in the first second of idling, 1 in 700 later, desk-diag's); the watchdog
state raises the line every scan. Results: 20 minutes at rest, desk-diag's rules: 117 downloads, 1175 reads without an edge; the
default rules: 0 watchdogs, 0 reads without an announced frame, 798 reads (every announced frame), the reference after 0.7 s
(settled), ~22 wake-ups a second; a touch on the idling controller read at most 85 ms after it landed (30 ms its wake, one idle
period, the read), 471 frames in 5 s under a finger, the lift seen in the frames; the attend1/attend2 cases (the attend rules:
1 starvation; 4 collisions, 3 downloads) and desk1's (without the grace: 4 downloads, never back) still reproduced, the default
rules in all of them never at 0xfd; edges always, every third lost, none at all (then the line's level, which the tablet's line
never showed: a pulse), cycles of 11.6 and 16 ms, the display off: an announced frame waits at most 193 ms.

History. 2026-11-11: one frame per idle period with the latched edge dropped after each sleep, 12 ms reads without an edge while
busy (attend1/attend2). 2026-11-12 morning ("keeping the controller fed"): edges never dropped, reads without an edge after 50 ms
busy, the idle period + 50 ms at rest, every 200 ms with the display off, never more than 250 ms unread, and `StartGrace` (no read
before a start's first edge, desk1): desk-diag ran them and looped all the same.

With `piano-touch-irq=1` the wait for the edge (when it is not latched at the idle period's end) is the TLMM interrupt: gpio162 on the summary line SPI 208
(`package/common/PianoIrq.h` `tlmm::Pin`, the table shared with PianoKeyboard), enabled only for that wait (the handler masks
it), with INTR_CFG's PDC bit cleared. Ubuntu measured touch reads 43-51 % slow while the system idle state (4.4 ms exit) was
entered and held a 2000 µs CPU latency limit per transfer (`../ubuntu-history/kernel.md` top finding 8): the busy state therefore
stays on 1 ms polls, and a bad checksum on a read after an interrupt (a late wake) makes the next idle edge polled. Three
timed-out waits in a row with the latch set turn the interrupt off (`no interrupt in 3 waits in a row`); a storm on the line
(its guard) too. Lines: `frames: gpio162 interrupt on the TLMM summary (SPI 208, INTID 240): slot N, INTR_CFG armed ... masked
...` or `... NOT used (why)`. Test images and the A/B: `../power-irq.md` §5.

**The watchdog** (`WatchdogRecovery`): eleven watchdog transport blocks in a row (0xfd, or 0xfe: idled first, swrst <- 0xaa) and the
download again at once; while it comes back sooner than 10 s after a download, the next one 10 s after it, and so on, never given
up (2026-11-11: three downloads in a row, then `FAILED at step 7` and no touch for the boot). With the display off it is only
counted (the blocks are read as the controller announces them); at the display's return the probe reads the firmware info and the
reset state (§16), which survive the watchdog state, and the frame loop downloads after eleven blocks. Lines: `frames: 11 watchdog
frames in a row (state 0xfd); the first read on an edge|on the line still high|without either, T ms after the read before it
(...); N frames since the last start, M ms ago` (what preceded it, §8), `frames: watchdog (state 0xfd, 11 frames in a row): the
download again (N for the watchdog[; it came back: every 10 s from now on, never given up])`, `frames: watchdog (state 0xfd) again
T ms after its download: the next download in T ms`, `frames: the download for the watchdog failed: again in 10 s`. With every
stats line: `frames: reads N: E on an edge, L on the line still high, B without either; gpio162 now 0|1; watchdog frames W,
downloads for it D`: B is 0 unless `piano-touch-noedge` is set.

**SE2's clocks before each transfer.** Every GENI register access needs the QUP1 SE2 clock branches; an access with one off stalls
the bus, which can hang a CPU until the watchdog resets the tablet (two desktop sessions with touch reset, cause unknown). Before
each transfer the five branches are read in GCC (always safe); one found off: PianoTouch's votes in 0x52008 again (bits only set,
under PianoA830's lock word) and the transfer skipped (`geni: a QUP1/SE2 clock branch is off (CBCR ...; vote ... -> ...; N
times): voted again, the transfer skipped`). Audit of the touch paths for the resets: nothing masks interrupts; the waits are bounded
(the GENI polls: 300 µs spinning, then 1 ms sleeps, ~110 ms at most a frame; the engine-idle wait 2 ms; the lock word 50 ms, at
bring-up only; the SMMU's TLB sync 100 ms, at step 2 only; `setPowerState(off)` 600 ms of 1 ms sleeps); no other driver clears the
0x52008 bits (PianoA830 votes in 0x52000, PianoWifi only sets bits under the lock); the latency hold is not in the 0059 images. The
decoder's and the tracker's scratch (~4.5 KiB) moved from the 16 KiB kernel stack into their structures.

**The CPU latency hold** (`piano-touch-cpu-hold`, default 2000 µs; `TouchPace.h` `BurstHold`). Ubuntu held a 2000 µs CPU latency
limit around each GPI SPI transfer (reads 43-51 % slow without it). Here the hold covers the read burst: taken when the panel turns
busy, released 1.5 s after it was last busy (so the idle frames never hold it), and always at a mute, a suspend or the loop's end.
`piano_cpuidle_latency_hold` is k27 0061's: compiled in only for a kernel that exports it (§7); with `piano-cpuidle` below 3 its
2000 µs keeps nothing out (cl5 leaves in 1983 µs), with 3 it keeps the CPU subsystem state (4400 µs) out during touches.

## 14. Level 2: the touch screen on macOS (2026-11-11, host-tested, not run)

macOS has no touch screen interface: no touch events, no gesture recogniser for a panel, no on-screen tap targets of its own. What
it takes from HID is a pointer, so PianoTouch turns touches into what a pointer can say (`TouchGesture.h`), from contacts it follows
itself (`TouchTrack.h`) over the matrix's peaks (`TouchMatrix.h` points).

### 14.1 What a touch does

| Touch | macOS gets | Notes |
|---|---|---|
| a finger lands | the pointer moves there, no button | the first report of a touch: in the contact's first frame when its peak reaches 400 (`piano-touch-fast`), else its second |
| **tap**: lifted within 500 ms, moved at most 32 px (~2.4 mm) | **left click** where it landed | pressed and released when the lift is seen (3 frames without the contact, ~38 ms at the ~80 frames a second the controller gives). A tap within 500 ms and 48 px of the previous one clicks exactly where that one did, so macOS counts a double click |
| **hold**: 500 ms within 32 px | **right click** where it landed, at 500 ms | the context menu opens under the finger; the finger then only moves the pointer (past the slop), its lift sends nothing |
| **drag**: moved past 32 px before 500 ms | **left drag**: pressed where it landed, the pointer follows the finger, released at the lift | selecting text, moving windows and icons, sliders; a second finger during a drag changes nothing |
| two fingers (the second seen in 2 frames while the first was undecided) | the pointer moves between them | the window under it is the one that scrolls |
| **two-finger tap**: every finger lifted within 500 ms of the first landing, neither their middle nor their spread moved past 32 px | **right click** between them | |
| **two-finger scroll**: their middle moved past 32 px | **Wheel / AC Pan**, one count per 4 px of the middle's travel, declared at 45 counts per inch | fingers up = wheel up and right = AC Pan right, so under macOS's natural scrolling (its default) the content follows the fingers as on a Mac trackpad; the axis locks at the start when one is at least twice the other; the pointer stays put; it ends when a finger lifts |
| **pinch**: the spread changed past 32 px and more than the middle moved | nothing by default; `piano-touch-pinch=1`: Command held while the wheel turns, `=2`: Control (Accessibility Zoom's modifier), one count per 16 px of spread, apart = up = zoom in | recognised either way, so a pinch never scrolls; the modifier is released when it ends (or anything is cancelled) |
| three or more fingers | nothing until every finger is up | whatever was going on ends first (a held button released) |
| a **palm** (a blob of 48+ nodes or 3+ peaks), or anything where a palm was in the last 500 ms | nothing | a finger elsewhere still taps and drags beside a resting palm; a followed contact that turns out a palm ends its gesture (released, never clicked) |
| the display off (NativeGopDisplay's `piano-display-power`) | nothing | §16 |
| `piano-touch-gestures=0` | the plain tip of before: down presses, moves drag, up releases | |

Every press is released and every modifier let go on every path: a lift, three fingers, a palm, frames lost (a read error or the
watchdog's re-download: `gesture: cancelled (frames lost): released`), the display going off, a suspend. The host tests check that
each sequence ends with no button and no modifier held.

### 14.2 Contacts

- **Peaks** (`TouchMatrix.h`): every local maximum down to 60 % of the threshold (120); one at the threshold (200) may start a
  contact, a weaker one only continues one (the hysteresis). Two peaks two nodes apart with no dip between them (the node between
  at 80 % of the weaker or more) are one contact; three nodes apart (fingertips side by side) stay two.
- **Size**: each peak's blob, the 8-connected nodes at half the threshold, gives its area (nodes), its peak count and its bounding
  box. A fingertip in the synthetic frames is 6-8 nodes, a palm 90 or more; the real values come with §15's lines.
- **Position**: the centroid of the peak's 3 x 3 nodes, widened on each axis by one node toward the strictly stronger neighbour
  when that node is at most half of the neighbour (this contact's own tail, not another's flank). Found by the host test: a finger
  resting exactly between two electrodes made its peak node flicker between them with the noise, and the plain 3 x 3 window jumped
  15 px each time (a drag ended in a 15 px shimmer); the widened window is the same in both cases, so the position stays put (the
  test: within 3 px over 40 frames, and monotonic over a sweep). With the unit's calibration the same window weighs piano-thp's
  electrode positions (piano-thp's domainContact is the 3 x 3).
- **Tracking** (`TouchTrack.h`): up to 8 contacts, IDs 1..255; each frame the pairs of a contact and a point within 400 px of where
  the contact was heading (its last motion carried on: two fingers passing each other keep their IDs), nearest first. A new contact
  counts after 2 frames (or at once from a peak of 400); it ends after 3 frames without a point, or at the first one once it was
  last seen 40 ms ago (slow frames). A shorter dropout keeps it, its ID and its place. Its position moves only past 2 px (a
  resting finger sends nothing).
- **Palms**: a blob of 48 nodes or more, or with 3 peaks or more, never starts a contact, and marks a palm zone (its box and 2
  nodes around) for 500 ms; a contact whose point turns into a palm, or that starts in a zone, is a palm until it lifts. So a palm
  that lands (small, then large) or lifts (large, then a fingertip-sized remnant) never clicks.

### 14.3 Scroll resolution

As PianoKeyboard's touchpad ([keyboard.md](keyboard.md) §6a): macOS takes the scroll resolution from the Wheel's physical extents
and has no Resolution Multiplier, so fine scrolling is fine counts declared at a correspondingly higher resolution. A count is
4 px of finger travel, about 0.3 mm on this ~345 px/inch panel: ~86 counts per inch of finger, declared at 45 per inch. That is
the touchpad's ratio (173 counts per inch of finger declared at 90): a finger's millimetre turns the wheel as far as it does on the
cover's touchpad, and macOS's scroll acceleration applies on top. Faster: `piano-touch-scroll-res=30`; slower: `=60`; finer
steps: `piano-touch-scroll=2`.

### 14.4 Latency and the log

From the finger's landing to the first report: at rest the idling controller's wake (its first announced frame; the model
assumes 30 ms, the run tells) and up to one idle period (50 ms) and a frame read (~5 ms; the model: at most 85 ms, §13); then the
pointer moves in the contact's first frame read when its peak is strong, else in the second (~12.6 ms later: under a finger
every frame is read on its edge, ~80 a second). A tap's click: 3 frames (~38 ms) after the lift;
a hold's right click 500 ms after the landing (host test: 504 ms). `piano-touch-idle=20` shortens the rest latency by 30 ms for 50
looks at the latch a second instead of 20; `piano-touch-irq=1` takes the edge at once (≤ 1 ms).

Lines, at level 1 and with `piano-touch-trace=1` at level 2 (without it at level 2 the positions are left out): per contact
`contact N down at (x, y) px, peak P, area A nodes[ (a palm's)], frame F`, `contact N up at (x, y) px after T ms (F frames, peak
up to P, area up to A nodes)`, `contact N is a palm (area A nodes, K peaks in its blob): no gesture from it` (panel pixels); per
gesture (desktop pixels) `gesture: tap at (x, y) -> click (T ms)`, `gesture: held T ms at (x, y) -> right click`, `gesture: drag
at (x, y), left button down (T ms after the touch)`, `gesture: drag ends at (x, y) after T ms, D px from its start`, `gesture:
two-finger tap at (x, y) -> right click (T ms)`, `gesture: two-finger scroll at (x, y), vertical|horizontal|both axes`,
`gesture: scroll ends after T ms: wheel W, AC Pan P counts`, `gesture: pinch at (x, y): recognised, nothing sent|Command +
wheel|Control + wheel`, `gesture: pinch ends after T ms: spread D px, Z zoom counts[ (not sent)]`, `gesture: three or more
fingers: nothing until every finger is up`, `gesture: cancelled (a contact turned out a palm); ...`, `gesture: cancelled
(frames lost): released`. The first 200 of each, then about every 20th. With the stats: `gestures at N s: T taps, R right
clicks, D drags, S scrolls, P pinches, C cancelled; contacts N (palms ...), M merged peaks; K reports sent, F failed; CPU
latency holds H`.

### 14.5 Host tests (`tests/touch_host.cpp`, synthetic frames through the matrix, the tracker and the gestures)

Points: two fingers, fingertips side by side, a ridge merged, a palm's size and peaks, a weak peak, a palm's peaks not crowding
out a finger, the flicker between electrodes, a sweep. Tracker: two fingers crossing keep their IDs, a 2-frame dropout kept and
a 3-frame one ended, 200 ms frames ending at once, the hysteresis, the fast confirmation, palms by size and by peaks, the lifting
palm's remnant, a finger flattening into a palm, a palm beside a finger. Gestures: tap (first report in the first frame; without
the fast confirmation in the second), no jitter while resting, hold (504 ms), hold then move, drag (pressed where it landed,
monotonic, released), a second finger during a drag, two-finger tap (also staggered), two fingers held (nothing), scroll up /
down / right / left / diagonal / inverted / coarser, pinch (nothing; Command; Control), three fingers, a palm alone, a palm and a
finger, a palm landing and lifting, one-frame noise, double-tap snapping (and too late, too far), frames lost during a drag and
a pinch, the plain tip, the corners, every gesture at 12 ms frames. Rotation: taps at 0/90/180/270 under the finger, scroll and
drag directions at 90/180/270. The descriptor: two variants, six resolutions, the report bytes. The pace with and without edges,
the read decision (`readOn`), the idling controller model (desk-diag's loop and its absence), the quiet reference (`matrixSettle`).
The burst hold. The power states and the probe (§16).

## 15. Attended test list (the next tablet session)

### 15.1 Results of 2026-11-11

- **attend1** (`piano-touch=2`, `piano-touch-flip` auto: BOE, no flip): the display read off at boot (NativeGopDisplay's
  `piano-display-power` before WindowServer), touch muted at ~2 reads a second, and the controller went to its watchdog state
  (0xfd) while muted; at the display's return the probe said "kept its state" (the firmware info survives 0xfd), two downloads for
  the watchdog, then edges at 24-79 a second and touch worked: contacts, taps, drags, two-finger scrolling (12238 frames, 1 bad
  checksum). **The Y axis was reversed** (as on Ubuntu, which needed a per-device invert_y).
- **attend2** (the same with `piano-touch-flip=2`): muted at boot without 0xfd, then at rest without a single edge counted (the
  latch dropped after each idle sleep), at the first touch (183 s) 0xfd at once, three downloads each into 0xfd at once (the 12 ms
  edge-less reads inside the cycle), `FAILED at step 7`.
- Two desktop sessions with touch enabled reset the tablet (no panic in the log; the RAM images' Gunyah watchdog bites after
  32 s of a hang); cause unknown, a run without touch is the comparison. §13 has the touch paths' audit and the clock guard.

Fixed 2026-11-12 (§13, §7): the frame pace, the watchdog's back-off, the probe's frame read, SE2's clock guard, this unit's Y.

### 15.2 The next run: 20 minutes at rest, unattended (the watchdog loop gone)

One unattended boot of the desk image ([../attended-desktop.md](../attended-desktop.md) §3: the desktop profile, its seed, no
`-v`) with this change, nobody touching, reset after 1500 s (the login window from T ~110-150 s, then more than 20 minutes at rest,
the display going to sleep by itself at ~650 s as in desk-diag). Built from this tree (the KC with the k27 0064 kernel; this unit's
Y and calibration from `~/.config/piano/env`, never committed); `out/` is never committed:

```sh
K=$(sed -n 's/^PIANO_K27_TREE=//p' ~/.config/piano/env)
K27_KERNEL=$K/out/k27/kernel-k27-0064-native.macho PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh
DESK_RTC_OFFSET=1794397906 DESK_RESET_AFTER=1500 DESK_EXTRA="piano-touch-trace=1" tooling/k27/build-desktop-images.sh desk
tooling/native/native-loop.sh out/ram-desk/boot-piano-macos-ram-A-bundled.img out/ram-desk-dump/boot-piano-macos-ram-A-bundled.img
python3 tooling/native/usb-console.py --capture 1540     # in parallel
```

Its arguments: desk's of [../attended-desktop.md](../attended-desktop.md) §3 with `piano-reset-after=1500` and `piano-touch-trace=1`
added (the first 40 frame headers: the scan mode of the start's frames, 4 while active, 2 once idling; no finger, so no position is
logged); 945 bytes. `build-desktop-images.sh` refuses a KC without the line `a frame read only when the controller announces it`.
Checked in an agent worktree (2026-11-12, on 49b69bd with this change): KC `b8c3d658927d09e3` (the k27 0064 kernel, 25 native
kexts, the native kext rule passed, `PianoTouchHID` 0 problem slots), main's Turnip stage as the seed; `ram-desk`
`c25b01f028fdfaa3`, its dump image `cbd51b750e9eca64`, `verify-ram-images.py` PASS for both.

What to count, in the capture and in the dump's `xnu-console.txt` (`grep -a 'PianoTouch:'`):

| Line | Pass | desk-diag (before) |
|---|---|---|
| `firmware: download N done` | only `download 1` | 25 downloads in its first 10 minutes |
| `watchdog frames in a row`, `frames: watchdog (state` | none | every 10-100 s |
| the last `frames: reads N: E on an edge, L on the line still high, B without either; ...; watchdog frames W, downloads for it D` | B = 0, W = 0, D = 0; E = N (L = 0) | B 5653 of 17936, W 11903, D 23 by 626 s |
| `start: scanning ...; stylus off taken, baseline command (1) taken, mode 2 taken` | once | (no stylus command, baseline 0) |
| `frames: the controller quiet (no edge for 400 ms) 1: N frames since the last start ...` | one line (N about 12), at most a few more | (none: the line is new) |
| `matrix: reference of N frames ... (formed once the controller went quiet ...)` or `of 32 frames` | once, within seconds of the start | |
| `frames at N s: R reads/s, E edges/s` at rest | R ≈ E ≈ 1 (the idling controller's one frame per ~1.5 s) | 10 reads/s, 0 edges/s |
| `idle: N wakeups in 60 s` at rest | ~1200-1400 (a look at the latch per 50 ms) | 21000-25700 |
| `display off: touch muted ...` (~650 s), then the stats lines | W stays 0 (desk-diag: 0xfd 39 s later) | `11 watchdog frames in a row` at 686 s |
| `no frame for N ms with a contact or a gesture on` | none (nobody touches) | |

Fails and what they mean: a download beyond the first with `the first read on an edge ...` in its `11 watchdog frames in a row`
line and B = 0: the announced-frame inference of §13 is wrong (the controller resets by itself: the next suspects are the
environment, the charger state command the stock driver sends, and the A/B `DESK_EXTRA="piano-touch-trace=1 piano-touch-stylus=1
piano-touch-baseline=0"`); B > 0: a read path still reads without an edge (a bug); no edges at all (`0 on an edge`): the latch.
Optional A/B partner (the same image with `DESK_EXTRA="piano-touch-trace=1 piano-touch-noedge=50"`): the old reads with the new
start sequence, which should loop again if §13 is right.

Then, attended (§15.3 and [../attended-desktop.md](../attended-desktop.md) §4): the corners and Y (`orientation: flip X 0, Y 1 (this
unit: Y reversed ...)`), the first touch after minutes at rest (the idling controller's wake: the pointer within ~0.1 s), taps that
click after a pause, 20 minutes of use with no `watchdog` line, no reset before `piano-reset-after`.

### 15.3 The steps (the list of 2026-11-11, still to be done in full)

**At boot** (no finger yet): `gestures: tap = click, held 500 ms = right click, moved past 32 px = drag, two-finger tap = right
click, two-finger scroll 4 px a count (declared 45/inch, the content follows the fingers under natural scrolling), pinch
recognised, nothing sent`; `contacts: up to 8 followed; one counts after 2 frames, at once from a peak of 400, ...`; `latency: no
CPU latency hold (built for a kernel without piano_cpuidle_latency_hold)`; `power: joined the power tree: ...`; `HID device:
touch screen pointer (absolute X/Y, 3 buttons, wheel and AC Pan at 45 counts/inch), ... 89-byte descriptor`; `event service
IOHIDEventDriver published on the touch screen`; after step 7 the `frames: every frame on its gpio162 edge while busy ...` line of §15.2
and `matrix: reference of 32 frames ...`.

Then, in this order (each step: what to do, the lines that prove it, what to watch on the screen):

1. **First touch, hold** (login window, an empty area): rest one fingertip still for 2 s, lift. `contact 1 down at (x, y) px, peak
   P, area A nodes, frame F`: **write down P and A** (they decide `piano-touch-fast` and `-palm`); the pointer appears under the
   fingertip at once; `gesture: held 50x ms at (x, y) -> right click`; `contact 1 up ...`. The `frames at` line after it: reads/s
   while touching (~120 with edges, ~80 without) and edges/s (open question 8).
2. **Tap** the user icon (or the password field) once. `gesture: tap at (x, y) -> click (T ms)`, T well under 500; the target
   reacts. The pointer within ~10 px of the fingertip. Log in (the cover's keyboard or the Mac's over USB).
3. **Double tap** a folder on the desktop or in a Finder window: two `gesture: tap` lines with the **same** (x, y); the folder
   opens.
4. **Hold** on the desktop 1 s: `gesture: held ... -> right click`; the desktop's context menu opens and stays after the lift; tap
   an empty area to close it (a `gesture: tap` line).
5. **Drag** a Finder window by its title bar about 5 cm, then a desktop icon: `gesture: drag at (x, y), left button down (T ms after
   the touch)` with T well under 500, then `gesture: drag ends at (x, y) after T ms, D px from its start`; the window follows the
   finger without lag or shimmer. Select text in a TextEdit document by dragging across it.
6. **Two-finger tap** on the desktop: `gesture: two-finger tap at (x, y) -> right click (T ms)`; the context menu between the fingers.
7. **Two-finger scroll** in a long Finder list or a TextEdit document: fingers up 5 cm slowly, then down, then left and right in a
   wide document. `gesture: two-finger scroll at (x, y), vertical` ... `gesture: scroll ends after T ms: wheel +N, AC Pan 0 counts`
   (5 cm is ~680 px: N about 170); the content moves **with** the fingers (up = up) if natural scrolling is on (System Settings,
   Mouse or Trackpad). Note whether it scrolls about as far as the fingers travel: if not, the next image takes
   `piano-touch-scroll-res=` lower (further) or higher (less far).
8. **Pinch** apart and together on the desktop: `gesture: pinch at (x, y): recognised, nothing sent`, `gesture: pinch ends ...
   (not sent)`; nothing scrolls or zooms.
9. **Three fingers** on the panel, moved: `gesture: three or more fingers: nothing until every finger is up`; nothing moves.
10. **Palm**: rest the heel of the hand on the lower half for 2 s, lift; then rest it again and tap a desktop icon with a finger of
    the other hand. Lines: no `gesture: tap` for the palm (`contact N is a palm ...` or `contact N down ... (a palm's)`, or no
    contact at all), one `gesture: tap` for the finger; nothing clicks under the palm.
11. **Corners**: tap each corner as far as it goes: the tap lines' (x, y) within ~10 px of (0, 0), (3199, 0), (0, 2135),
    (3199, 2135) (the desktop at rotation 0 is the panel).
12. **At rest** 2 minutes without touching: `frames at 60 s: ~20 reads/s`, `idle: N wakeups in 60 s` (~1800 blind, ~3600 polled on
    edges), and a `gestures at N s: ...` line with the counts of steps 1-10.

Rotation is not in these images (`piano-touch-rotate=1` needs the rotation agent, [sensors.md](sensors.md) §5a); its mapping into
the gestures is host-tested (§14.5) and checked with that agent's own test.

Fallbacks for the next image: no pointer movement although `contact` lines come → `piano-touch-gestures=0` (the plain tip) tells
whether the descriptor or the gestures are at fault; every tap a palm → `piano-touch-palm=` above the `area` of step 1; taps
read as drags → `piano-touch-slop=96` (64 since 2026-11-12, with an 80 ms drag delay); holds too eager → `piano-touch-hold=700`.

**With `piano-touch-idle=20 piano-touch-irq=1` added** (the lowest latency at rest): steps 1-2 again; the `contact 1 down` line comes at most ~20 ms after the touch (one idle period of
20 ms); `frames: gpio162 interrupt on the TLMM summary ...` or `... NOT used (why)`; `frames at 60 s: ~50 reads/s` at rest.

**Suspend** (`K27_KERNEL=<the 0062 kernel>`, the suspend test's arguments `piano-power=1 piano-power-period=10 piano-input=1 piano-sleep=1 piano-sleep-state=1 piano-sleep-test=150 piano-sleep-test-count=3 piano-sleep-max=30` added: a sleep 150 s after the boot completed and 150 s after each wake, three times, each woken after
30 s; log in before the first with the Mac's keyboard over USB, keep the cable in): at start `latency: while the panel is busy no
CPU idle state slower than 2000 us to leave (piano_cpuidle_latency_hold)`; at each suspend `power:
system off (suspend N): frames stopped, contacts released, CPU latency hold released, gpio162 interrupt masked` and `power:
system off taken, frames stopped after M ms` (M under 600); at each wake `power: the controller kept its state | lost its state:
firmware info ...`, (if lost) `firmware: download N done ...`, `power: system on: touch back after T ms`, then `matrix: reference
of 32 frames ...` again. After each wake: a tap and a two-finger scroll work as in A (steps 2 and 7). A touch while the
tablet is suspended wakes nothing (touch is no wake source).

**The display off** (optional, `piano-backlight=2` added, after backlight.md's own first run: it needs `piano-backlight=2`, with which NativeGopDisplay publishes
`piano-display-power`): at start `display: following NativeGopDisplay's piano-display-power (on now)`. Let the display sleep
(System Settings, Lock Screen: turn the display off on power adapter when inactive, for 1 minute): `display off: touch muted
(nothing reported), the latch looked at once per 200 ms ...`; touch and tap the dark panel: no `gesture:` lines, the display stays off; wake it
with a key: `display: the controller kept its state ...`, `display on: touch back after T ms`; a tap works at once.

## 16. Power states: suspend and the display off (2026-11-11, host-tested, not run)

The touch part of the driver contract (`package/include/PianoPowerContract.h`, `../power.md` §4), in `TouchPower.h` and
`PianoTouch::setPowerState` / `powerStep`:

- **Joined** at start (`pianopower::join`; `piano-touch-pm=0` leaves it out): IOPMrootDomain sends `setPowerState(off)` before a
  system sleep (k27 0062, `piano-sleep=1`), a restart or a shutdown, and `on` after a wake.
- **Off**: recorded and the frame thread kicked (an idle wait ends at once); the thread ends every contact and gesture (released),
  releases the CPU latency hold, masks gpio162 at the pin (INTR_CFG enable off; touch is no wake source, as in Ubuntu, and its PDC
  bit stays clear) and parks; `setPowerState` waits for that at most 600 ms and acknowledges (`kIOPMAckImplied`, within the
  contract's 1 s). The facts give the NT36532E no sleep command of its own (the host commands known are 0x19 and 0x01), so off is
  "stop reading frames": no SPI traffic, no DMA, nothing of the driver runs; Ubuntu stopped the touch transfers before the SPI bus
  suspended (`../ubuntu-history/kernel.md`, K:e98efd360100). PianoTouch holds no RPMh or bandwidth vote and no GDSC; its GCC
  branch votes for QUP1 SE2 (0x52008 bits 18-21 and 24) stay set in off, as they are only ever set (the register is shared under
  PianoA830's lock word): dropping them belongs to the CX-collapse work (`../power.md` §3.6), with the other drivers' votes.
- **On**: returns at once; the thread restarts asynchronously (Ubuntu: touch restarting asynchronously made the screen wake ~0.3 s
  faster). It probes the controller by reads only (`Nt36532.h` `probeState`: the firmware info valid with the version and counts
  `start()` saw, the reset state in 0xa0..0xaf; a probe whose read fails sets the GENI engine up again once, clocks and loopback
  included): kept, the frames go on and the reference is learned again (the time asleep is
  unknown: ~0.4 s without touch); lost, the download again (at most twice), then the reference; still nothing, the frames stay
  stopped until the next wake. The latch, the contacts and the gestures start afresh.
- **The display off** (`piano-touch-display`, default 1): NativeGopDisplay's `piano-display-power` (published with
  `piano-backlight=2`; without it the display counts as on), read every 250 ms. Off: every contact and gesture ended, nothing
  reported, the latch looked at once per 200 ms and an announced frame read (§13; decoded only so that the reference keeps following the drift), never busy (no
  latency hold), read errors and watchdog frames only counted (a TDDI stops scanning with its panel: Ubuntu started the THP stream
  after panel enable and stopped it at disable, `../ubuntu-history/device.md` §2.10). On: the same probe; kept, nothing more
  (the reference was followed); lost, the download and the reference.
- The probe (both returns) reads the firmware info and the reset state only, no frame (§13: a frame is read when the controller
  announces it; 2026-11-11 to 12 it read one frame too). Both survive the watchdog state (attend1's "kept its state" followed by 0xfd
  frames): a controller in it announces its blocks all the time, and the frame loop downloads after eleven of them (§13).
- Lines: `power: joined the power tree: ...`, `power: system off (suspend N): ...`, `power: system off taken, frames stopped after
  M ms` (or `NOT taken yet`), `power: the controller kept|lost its state: firmware info V/C (started with S), reset state R`,
  `power: system on: touch back after T ms` (or `the controller did not come back: frames stopped until the next resume`),
  `display: following NativeGopDisplay's piano-display-power (on now)`, `display off: touch muted ...`, `display: the controller
  ...`, `display on: touch back after T ms`.
- Host tests: the state machine (mute, suspend from either, resume into a dark display, an unknown display as on,
  `piano-touch-display=0`), the restart plan (kept, relearn, two downloads, give up), the probe on the chip model (kept after the
  start, a lost SRAM detected, the download bringing it back, another image's version refused) and a drag released at a mute.
- Risks: the power tree's `off` also comes at a restart (harmless: the frames stop); a controller that loses its firmware with the
  panel is re-downloaded at each display return (~1 s without touch); NativeGopDisplay publishes the display's state only with
  `piano-backlight=2` (a one-line change in its owner's package would publish it always).
