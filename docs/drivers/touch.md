# Touch: NT36532E on GENI SPI (`PianoTouch`)

Status 2026-11-11: **level 1 ran on the tablet** (`package/PianoTouch`, listed in `configs/k27-native-kexts.txt`, inert without
`piano-touch=N`): GENI SPI loopback passed, NT36532 identified, the BOE image downloaded with its CRC, scanning, the unit's
calibration loaded, frames with good checksums. Facts still marked *inferred* or *needs-boot* below wait for a run with a finger.
**Level 2** (`piano-touch=2`): the touch screen for macOS, which has no touch screen interface of its own, as what it understands
from a HID pointer (§14): tap = click, hold = right click, drag = left drag, two-finger scroll (fine, natural), two-finger tap =
right click, palms rejected, a pinch recognised (sent only on request). Host-tested on synthetic frames, **not run yet**: the
attended test list for the next session is §15. Multi-contact tracking (§14.2) feeds it; Apple's multitouch path is not used.
Battery (2026-11-11): at rest the frame loop reads one frame per 50 ms instead of every frame and a 1 ms poll (§13,
[`../power-irq.md`](../power-irq.md)); `piano-touch-irq=1` takes those frames' edge from the TLMM interrupt. The first run saw
almost no gpio162 edges at rest, so a frame is now also read without one (§13): a touch at rest is read within ~50 ms instead of
200, and under a finger the loop reads ~80 frames a second even if the edge never comes.
Display rotation (2026-11-11, host-tested, default off): `piano-touch-rotate=1` maps each contact into the rotated desktop before
the gestures (§5, [sensors.md](sensors.md) §5a.3).

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
| Orientation | X along the 60 columns (3200 px), Y along the 40 rows (row-major matrix); BOE: panel Y = processor Y (no flip), CSOT: reversed; X never flipped | piano-thp `frame.cpp`, `orientation.hpp`; this BOE unit ran so on Ubuntu | verified (Ubuntu); the verification step confirms it natively |
| Calibration | per unit, private: electrode centres `row_map[40]`, `column_map[60]` (52-54 px steps, not uniform), maxima 21360 / 32000 (pixels x 10), edge band 600 (60 px), gain 4350 / base 2700; `project_vendor_id` 26 = BOE | the unit's Ubuntu file (§7) | verified (file); its arrays are never written down here |
| Frames at rest | nobody touching: ~0.5 gpio162 edges a second, frame type 3, every read with or without an edge had a good checksum (1074 of 1074 in 210 s), peak delta 9-12 at rest | the first level-1 run, 2026-11-11 | verified at rest; under a finger **needs-boot** (§15) |

## 3. Specification sources

Behaviour only, nothing copied from GPL code: piano's Ubuntu (`nt36532e-thp` kernel transport, `device/piano-thp` processing)
as described in the local plan `touch-native-plan.md` (not published); Project-SunUEFI's piano touch producer, which ran this
controller natively in UEFI over GPIO SPI (firmware loaded, idle baseline; touching broke the checksums: GPIO SPI is too slow).

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
| `piano-touch-mode=V` | 2 | operating-mode command value (65536: not sent) |
| `piano-touch-baseline=0` | 1 | no idle-baseline command |
| `piano-touch-threshold=T` | 200 | contact threshold of the decoder |
| `piano-touch-flip=B` | auto | auto: by the panel (BOE none, CSOT Y reversed; the calibration's panel wins over gpio100 here, its `invert_y` over both); 0..3: bit 0 X, bit 1 Y flipped |
| `piano-touch-poll=MS` | 1 | edge polling interval while the panel is busy |
| `piano-touch-idle=MS` | 50 | at rest one frame per MS (20..500); 0: every frame, the old loop (§13) |
| `piano-touch-irq=1` | 0 | the idle frames' edge by the TLMM summary interrupt instead of 1 ms polls (§13) |
| `piano-wake-log=S` | 60 | the `idle: N wakeups in S s` line every S seconds (0 none; shared) |
| `piano-touch-trace=1` | 0 | more frame headers; contact positions also at level 2 |
| `piano-touch-rotate=1` | 0 | follow the display's rotation (§5): `PianoTouchRotation` and `piano-touch-rotation` taken, contacts mapped from their touch-down |
| `piano-touch-noedge=MS` | 12 | while busy, a frame is read anyway MS after the last one when no gpio162 edge came (4..200); at rest the idle period + 10 ms, or the idle period itself once 3 reads in a row had no edge (§13). With `piano-touch-idle=0`: 200, as before |
| `piano-touch-gestures=0` | 1 | level 2 without the gestures: the first contact is the tip (down presses, moves drag, up releases), as before this change |
| `piano-touch-hold=MS` | 500 | a finger held this long within the slop: right click (200..3000; 0: never, and a long press is a click) |
| `piano-touch-slop=PX` | 32 | panel pixels (~2.4 mm) a finger may move and still tap or hold; past it a drag, a scroll or a pinch (4..200) |
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
0xa0..0xaf`; `step 6`, `start: scanning ...; payload 5192 ...; ... calibrated ...; baseline command taken, mode 2 taken`; `step 7`, `frames: first gpio162 edge`, three
`frame N ... checksum ok` lines, `orientation: flip X 0, Y 0 (BOE panel: as processor Y; panel BOE by the calibration),
position from the unit's electrode maps and edge mapping`, `frames: without a gpio162 edge a frame is read anyway 12 ms after the
last one while busy, 60 ms at rest`, `matrix: reference of 32 frames ...`, `frames at 5 s: ~120 reads/s, ~120 edges/s ...` every 5 s
for 30 s then every 60 s (at rest, once the reference is learned and 1.5 s after the last contact: ~20 reads/s, and the edges/s the
loop takes, ~20; while touching ~120, or ~80 reads/s with no edges at all), and per contact `contact N down at (x, y) px, peak P,
area A nodes, frame F` / `contact N up at (x, y) px after T ms (F frames, peak up to P, area up to A nodes)` with the
`gesture: ...` lines of §14.4; every minute
`idle: N wakeups in 60 s, irq M (R frame reads; S idle sleeps; ...)` (~3600 at rest polled, ~2400 with `piano-touch-irq=1`). Pass: checksum ok >= 99 %;
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
| `baseline command NOT taken` / `mode 2 NOT taken (continuing)` | the firmware did not clear the command byte; frames may still come (SunUEFI sends no baseline command): look at the frame lines |
| `geni: transfer N: the engine is still busy` | a command still running 2 ms after the previous DMA completed: the engine hangs |
| no `first gpio162 edge`, ~0 edges/s, ~20 reads/s at rest and ~80 while touching | GPIO162 does not latch (or, at rest, the controller idles its scanning): frames are read without it (§13); good checksums mean they are whole. Before this change the same case read 5 frames a second |
| many `checksum BAD` | reads not synced to the edge, or slower than a frame: `piano-touch-khz=19200` |
| `N watchdog frames in a row`, `watchdog (state S): the download again` | controller firmware crashed: idled first when S is 0xfe (swrst <- 0xaa), then downloaded again (at most 3 times) |
| `FAILED at step 7: frames: 50 frame reads in a row failed` | the transport died mid-run |
| level 2 without `event service ... published` | IOHIDEventDriver did not attach (personality/IDs) |
| `contact N down` lines but no pointer movement in macOS | the reports go nowhere: `HID: handleReport` failures, or the descriptor not taken (compare with `piano-touch-gestures=0`, the plain tip) |
| every tap a `contact N is a palm` | the finger's blob is larger than `piano-touch-palm` (the `area A nodes` of its down line): raise it |
| taps logged `gesture: drag` | the centroid moves more than the slop during a tap: `piano-touch-slop=48` |

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
6. Charger noise (USB cable always in): the Ubuntu driver sends a charger state command; not done.
7. Finger peaks and blob sizes on the real panel (the defaults: fast confirmation at 400, palm at 48 nodes) are guesses until
   §15's `contact N down ... peak P, area A nodes` lines show them; nobody touched the panel in the level-1 run.
8. Does the controller idle its scanning at rest (the first run's ~0.5 edges/s) and return to 120 Hz edges under a finger?
9. Does the TDDI keep its firmware across a display-off and a suspend (§16's probe says which, per event)?

## 11. Keyboard cover

Its own page: [keyboard.md](keyboard.md) (`package/PianoKeyboard`, WN8030 over software I2C, `piano-keyboard=N`).

## 12. Licence note

New code under BSD-2-Clause-Patent; reused pieces under their own licences as listed in `package/PianoTouch/UPSTREAM` (one
Apache-2.0 source, NOTICE kept). Linux `nt36532e`/`spi-geni-qcom` (GPL-2.0) were not read for this work; K90's code is facts only.
The panel images and the unit's calibration stay out of every repository: the calibration is read at build time from the path in
`PIANO_TOUCH_CALIBRATION` and only its panel and an 8-digit hash prefix are logged.

## 13. Battery: the frame pace (`piano-touch-idle`, `piano-touch-irq`)

The controller streams a frame about every 8.3 ms whether or not a finger is on the panel. The loop now reads every frame
(1 ms latch polls, as before) only while the panel is busy: a contact in the last frame, the tip confirming or down, the decoder
still learning its type or reference, watchdog frames, and for 1.5 s after the last of these. At rest (`TouchPace.h`) it sleeps
until 2 ms before the next read is due (one per `piano-touch-idle`, default 50 ms), drops the latch (that edge came mid-sleep,
mid-frame) and reads on the next fresh edge. A finger is read at most one idle period and a frame late, then every frame. The
host test's model: ~20 reads and ~60 wake-ups a second at rest instead of ~120 and ~1000. SPI by hand is not paced (it reads at
most 10 frames a second anyway).

With `piano-touch-irq=1` the wait for that fresh edge is the TLMM interrupt: gpio162 on the summary line SPI 208
(`package/common/PianoIrq.h` `tlmm::Pin`, the table shared with PianoKeyboard), enabled only for that wait (the handler masks
it), with INTR_CFG's PDC bit cleared. Ubuntu measured touch reads 43-51 % slow while the system idle state (4.4 ms exit) was
entered and held a 2000 µs CPU latency limit per transfer (`../ubuntu-history/kernel.md` top finding 8): the busy state therefore
stays on 1 ms polls, and a bad checksum on a read after an interrupt (a late wake) makes the next idle edge polled. Three
timed-out waits in a row with the latch set turn the interrupt off (`no interrupt in 3 waits in a row`); a storm on the line
(its guard) too. Lines: `frames: gpio162 interrupt on the TLMM summary (SPI 208, INTID 240): slot N, INTR_CFG armed ... masked
...` or `... NOT used (why)`. Test images and the A/B: `../power-irq.md` §5.

**Without an edge** (2026-11-11, after the first run). The level-1 run on the tablet saw about one gpio162 edge in two seconds with
nobody touching (perhaps the controller idling its scanning at rest; open question 8), and every frame read without one had a
good checksum. The old loop then read a frame only every 200 ms (5 a second; a touch at rest up to 200 ms late, and under a
finger 5 frames a second if the edge stayed away). Now (`TouchPace.h` `noEdgeMs`): while busy a frame is read anyway 12 ms after
the last one (`piano-touch-noedge`; ~80 frames a second; with edges every frame comes on its edge first); at rest the idle period
plus 10 ms, and once three reads in a row came without an edge the rest is *blind*: the thread sleeps to the due time itself and
reads (one wake-up a read) until an edge latches during a sleep again. Host model with no edges at all: 198 reads and 297 wake-ups
in 10 s at rest (with edges, polled: 201 and 600), a touch read at most 49 ms after it landed, then 32 or more reads in 400 ms.
`piano-touch-idle=0` (the old loop, the A of the battery A/B) keeps 200 ms.

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
| **tap**: lifted within 500 ms, moved at most 32 px (~2.4 mm) | **left click** where it landed | pressed and released when the lift is seen (3 frames, ~25 ms, without the contact). A tap within 500 ms and 48 px of the previous one clicks exactly where that one did, so macOS counts a double click |
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

From the finger's landing to the first report: at rest up to one idle period (50 ms) and a frame read (~5 ms), with or without
edges (§13); then the pointer moves in the contact's first frame read when its peak is strong, else in the second (8-12 ms
later). Under a finger every frame is read (8.3 ms with edges, 12 ms without). A tap's click: 3 frames (~25 ms) after the lift;
a hold's right click 500 ms after the landing (host test: 504 ms). `piano-touch-idle=20` halves the rest latency for 50 reads a
second instead of 20; `piano-touch-irq=1` changes only how the idle edge is waited for (≤ 1 ms).

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
drag directions at 90/180/270. The descriptor: two variants, six resolutions, the report bytes. The pace with and without edges.
The burst hold. The power states and the probe (§16).

## 15. Attended test list (the next tablet session)

Images (RAM boot, `piano-reset-after=900`, back in ABL fastboot, the installed system untouched; GPU profile; built 2026-11-11 from
this change on main 471197c, sha256 of each `boot-piano-macos-ram-A-bundled.img` in the table; `out/` is never committed):

| Image (`out/`) | Kernel | Arguments added to the GPU profile | sha256 (16) |
|---|---|---|---|
| A `ram-touch-g` | 0059 (no latency hold) | `panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-touch=2 piano-touch-trace=1` | `9a9b7dd2709ffc5c` |
| B `ram-touch-g-fast` | 0059 | A + `piano-touch-idle=20 piano-touch-irq=1` | `59d2318506f0b502` |
| C `ram-touch-g-sleep` | 0062 (latency hold compiled in) | `panic_on_crash_disabled=1 piano-power=1 piano-power-period=10 piano-usb=2 piano-usb-hid=1 piano-input=1 piano-sleep=1 piano-sleep-state=1 piano-sleep-test=150 piano-sleep-test-count=3 piano-sleep-max=30 piano-touch=2 piano-touch-trace=1` (the suspend test's set, the Mac's keyboard and pointer over USB, and touch) | `c0bdd714217f7c2e` |
| D `ram-touch-g-display` (optional) | 0059 | A + `piano-backlight=2` | `d7f2e831f39a89e0` |

Each has its dump image in `out/ram-<name>-dump`. KC (16): `58c18824f3425025` (0059, A, B, D), `f29b076e05a7b0ff` (0062, C); 21
native kexts, the native kext rule passed, `PianoTouchHID` 0 problem slots. Data runs volatile (the A830 stage's seed is not the
installed one). Order: A, then B, then C; D only after the backlight's own first run.

Rebuild any of them (`K27_KERNEL` only for C):

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=900 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-touch=2 piano-touch-trace=1" \
  tooling/k27/build-native-image.sh                                                 # A (out/ram, out/ram-dump)
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img out/ram-dump/boot-piano-macos-ram-A-bundled.img
grep 'PianoTouch:' logs/native-runs/*-loop/xnu-console.txt | tail -200            # or live: tooling/native/usb-console.py
```

**A, at boot** (no finger yet): `gestures: tap = click, held 500 ms = right click, moved past 32 px = drag, two-finger tap = right
click, two-finger scroll 4 px a count (declared 45/inch, the content follows the fingers under natural scrolling), pinch
recognised, nothing sent`; `contacts: up to 8 followed; one counts after 2 frames, at once from a peak of 400, ...`; `latency: no
CPU latency hold (built for a kernel without piano_cpuidle_latency_hold)`; `power: joined the power tree: ...`; `HID device:
touch screen pointer (absolute X/Y, 3 buttons, wheel and AC Pan at 45 counts/inch), ... 89-byte descriptor`; `event service
IOHIDEventDriver published on the touch screen`; after step 7 `frames: without a gpio162 edge a frame is read anyway 12 ms after
the last one while busy, 60 ms at rest` and `matrix: reference of 32 frames ...`.

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
read as drags → `piano-touch-slop=48`; holds too eager → `piano-touch-hold=700`.

**B** (lowest latency at rest): steps 1-2 again; the `contact 1 down` line comes at most ~20 ms after the touch (one idle period of
20 ms); `frames: gpio162 interrupt on the TLMM summary ...` or `... NOT used (why)`; `frames at 60 s: ~50 reads/s` at rest.

**C** (suspend, the 0062 kernel: a sleep 150 s after the boot completed and 150 s after each wake, three times, each woken after
30 s; log in before the first with the Mac's keyboard over USB, keep the cable in): at start `latency: while the panel is busy no
CPU idle state slower than 2000 us to leave (piano_cpuidle_latency_hold)`; at each suspend `power:
system off (suspend N): frames stopped, contacts released, CPU latency hold released, gpio162 interrupt masked` and `power:
system off taken, frames stopped after M ms` (M under 600); at each wake `power: the controller kept its state | lost its state:
firmware info ...`, (if lost) `firmware: download N done ...`, `power: system on: touch back after T ms`, then `matrix: reference
of 32 frames ...` again. After each wake: a tap and a two-finger scroll work as in A (steps 2 and 7). A touch while the
tablet is suspended wakes nothing (touch is no wake source).

**D** (optional, after backlight.md's own first run: it needs `piano-backlight=2`, with which NativeGopDisplay publishes
`piano-display-power`): at start `display: following NativeGopDisplay's piano-display-power (on now)`. Let the display sleep
(System Settings, Lock Screen: turn the display off on power adapter when inactive, for 1 minute): `display off: touch muted
(nothing reported), one frame per 500 ms ...`; touch and tap the dark panel: no `gesture:` lines, the display stays off; wake it
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
  reported, one frame per 500 ms (the slowest tier; decoded only so that the reference keeps following the drift), never busy (no
  latency hold), read errors and watchdog frames only counted (a TDDI stops scanning with its panel: Ubuntu started the THP stream
  after panel enable and stopped it at disable, `../ubuntu-history/device.md` §2.10). On: the same probe; kept, nothing more
  (the reference was followed); lost, the download and the reference.
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
