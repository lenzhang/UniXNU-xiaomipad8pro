# Touch: NT36532E on GENI SPI (`PianoTouch`)

Status 2026-11-11: **built, host-tested and linked** (`package/PianoTouch`, listed in `configs/k27-native-kexts.txt`, inert
without `piano-touch=N`). **Not run on the tablet**: every hardware fact marked *inferred* or *needs-boot* below is open until
the first run. First deliverable: a **single-touch absolute pointer** (tap = click, drag = drag). Multi-touch is not done.
Battery (2026-11-11): at rest the frame loop reads one frame per 50 ms instead of every frame and a 1 ms poll (§13,
[`../power-irq.md`](../power-irq.md)); `piano-touch-irq=1` takes those frames' edge from the TLMM interrupt.
Display rotation (2026-11-11, host-tested, default off): `piano-touch-rotate=1` maps each contact into the rotated desktop (§5,
[sensors.md](sensors.md) §5a.3).

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

## 3. Specification sources

Behaviour only, nothing copied from GPL code: piano's Ubuntu (`nt36532e-thp` kernel transport, `device/piano-thp` processing)
as described in `droidvm-build/piano-patches/touch-native-plan.md`; Project-SunUEFI's piano touch producer, which ran this
controller natively in UEFI over GPIO SPI (firmware loaded, idle baseline; touching broke the checksums: GPIO SPI is too slow).

## 4. Components reused (provenance in `package/PianoTouch/UPSTREAM`)

| Piece | From | How |
|---|---|---|
| `GeniSpi.c/.h`, `tests/geni_spi_host.c` | NativeGeniSpi b5399b5 (QcomGeniSpi lineage), BSD-2-Clause-Patent | unchanged |
| bring-up order, image layout, chip tables, matrix decoder, SPI by hand | Project-SunUEFI `touch-product`, BSD-2-Clause-Patent | re-expressed (`Nt36532.h`, `TouchMatrix.h`, `Se2Bus.h`) |
| SE DMA through stream 0xa3, cache maintenance, vote bits | UniXNU-Odin3 `PaganiInput` (13T), UniXNU-sm8750-common `PeripheralIO.h` | re-expressed |
| start sequence (fw info, idle baseline 0x19, host-command handshake) | ShengNovatekTouch 69fdd62, Apache-2.0 (NOTICE kept) | re-written |
| bypass context bank | `package/PianoDwc3/SmmuStream.h` | included |
| HID class, publication, personality | `package/PianoKeyboard` / DroidVMInput | same pattern |
| calibrated position, edge mapping, calibration check | piano-thp `nvt_touch_core.cpp` (Apache-2.0, modified from xiaomi-sheng-thp by ianchb), `calibration.cpp`, `orientation.hpp`, `panel.cpp` (MIT) | re-written (`TouchMatrix.h`, `gen-touch-calibration.py`) |

Not used in this pass: the multi-contact trackers (ShengNovatekTouch's, piano-thp's) and piano-thp's filters.

## 5. IOKit family / ABI

`PianoTouch : IOService` on `IOPlatformExpertDevice`, a kernel thread, polled. At level 2 a `PianoTouchHID : IOHIDDevice`
(vendor 0x1af4, product 0x5054, 56-byte descriptor: mouse usage, 3 buttons, absolute X/Y 0..32767; the tablet shape that logged in
by tap in the piano VM), matched by a codeless `IOHIDEventDriver` personality; the 334-slot vtable is checked at the KC link.

**Display rotation** (`piano-touch-rotate=1`; without it both inputs are refused as before and the reports are unchanged). The
rotation is CGDisplayRotation's (degrees clockwise, 90 = the panel's left edge up), told by the rotation agent `piano-rotated`
([sensors.md](sensors.md) §5a.2) after it read it back:
- `callPlatformFunction("PianoTouchRotation", false, (void *)(uintptr_t)degrees, (void *)source, 0, 0)` for a kernel caller, and
  the property `piano-touch-rotation` through `setProperties` (0, 90, 180, 270) from root or the console user (the agent runs as
  the logged-in user; xnu's `"console"` privilege). Both only record the request (`kIOReturnBadArgument` for another value,
  `kIOReturnNotPrivileged` for another caller, `kIOReturnUnsupported` without the argument). `piano-touch-rotation` is also
  published: the rotation in use.
- A new rotation takes effect at the next touch-down, never in the middle of a contact (`pat_latch`); each report goes through
  `rotatedTabletReport` (`TouchHid.h`): the contact in the rotated desktop (`package/PianoAuto/TouchRotation.h`, `pat_rotate`), scaled
  over its size. At 0 the bytes are `tabletReport`'s. One line per change: `rotation: 90 degrees from this touch on (was 0; from
  setProperties, change 1)`.

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-touch=N` | 0 (inert: probe declines, nothing mapped) | 1: steps 1-7, statistics and contacts in the log, nothing for macOS; 2: the same plus the HID tablet pointer |
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

## 7. Build and kernel collection

`tooling/k27/build-native-piano-touch.sh`: host tests (GeniSpi; the NT36532E model over whole transfers and bit by bit; both
real images downloaded into the model with its CRC check; decoder, tip debounce, HID, the rotated report (identical bytes at 0
over the panel and beyond, the corners at 90/180/270, the latch), clocks), the pinned images staged into
the ignored `out/native-kexts/PianoTouch/firmware` and `.incbin`'d, the objects. `tooling/k27/build-native-image.sh` builds it
with the list; PianoTouch links after PianoA830 (it takes A830's `sm8750_gcc_vote_owner` around the 0x52008 write). Without the
images the build warns and step 5 stops on the target ("no usable image").

**Calibration.** The unit's Ubuntu touchscreen calibration is a private JSON file (schema 1), made by xiaomipad-8pro-mainline
`tools/extract-thp-calibration.py` from the vendor `piano_nova_<panel>_thp_config.ini`. Its path is `PIANO_TOUCH_CALIBRATION`, in
the environment or `~/.config/piano/env`; no path, hash or value of it is in this repository. The build passes it through
`tooling/k27/gen-touch-calibration.py`, which checks it as piano-thp does (40 x 60, 10 units per pixel, strictly increasing maps
inside their maxima, edge band below half an axis) and writes `out/native-kexts/PianoTouch/gen/PianoTouchCalibration.h` (ignored).
The build prints `calibration: unit file (panel BOE, sha256 xxxxxxxx), electrode maps, edge 60 px`, or `calibration: none (...):
linear map` when the file is absent, unset (`PIANO_TOUCH_CALIBRATION=` in the environment) or invalid (with the reason). With it, a
contact's position is piano-thp's: the electrode positions weighted by max(delta - 25, 0) over the peak's 3 x 3 nodes, then the
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
Level 2: the same build with `piano-touch=2`; tap the user icon and small targets, drag; do not tap Sleep/Restart/Shut Down.

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
position from the unit's electrode maps and edge mapping`, `matrix: reference of 32 frames ...`, `frames at 5 s: ~120 reads/s, ~120 edges/s ...` every 5 s
for 30 s then every 60 s (at rest, once the reference is learned and 1.5 s after the last contact: ~20 reads/s, and the edges/s the
loop takes, ~20; while touching ~120), and per touch `touch N down at (x, y) px` / `touch N up ... after T ms`; every minute
`idle: N wakeups in 60 s, irq M (R frame reads; S idle sleeps; ...)` (~3600 at rest polled, ~2400 with `piano-touch-irq=1`). Pass: checksum ok >= 99 %;
the corner presses land within about 10 px of the panel's corners, (0, 0), (3199, 0), (0, 2135), (3199, 2135) (the edge stretch
reaches the border), the middle one near (1600, 1068), Y growing downward. A corner on the wrong side means the orientation is
wrong (`piano-touch-flip=2` reverses Y, `=1` X, `=3` both); corners 20-60 px short of the border mean the edge mapping is not in
use (`calibration: none` in the log). Level 2 adds `HID device: tablet pointer ...` and `event service IOHIDEventDriver published on
the touch screen`; the pointer lands under the fingertip (within about 10 px, under a millimetre) and taps click.

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
| no `first gpio162 edge`, ~5 reads/s, 0 edges/s | GPIO162 does not latch; frames are polled every 200 ms, torn |
| many `checksum BAD` | reads not synced to the edge, or slower than a frame: `piano-touch-khz=19200` |
| `N watchdog frames in a row`, `watchdog (state S): the download again` | controller firmware crashed: idled first when S is 0xfe (swrst <- 0xaa), then downloaded again (at most 3 times) |
| `FAILED at step 7: frames: 50 frame reads in a row failed` | the transport died mid-run |
| level 2 without `event service ... published` | IOHIDEventDriver did not attach (personality/IDs) |

## 9. Fallback

`piano-touch=0` (the default) removes the driver from the boot; `piano-touch-bus=2` keeps the controller questions answerable
without SE DMA. Input meanwhile: the keyboard cover, or the Mac's keyboard and mouse over USB.

## 10. Open questions

1. Does SE DMA run on SE2 with the FIFO disabled, and do its status bits latch with the interrupt outputs off? (step 3's loopback)
2. Are the inferred clock facts right (vote bit 24, branch 0x1827c, the rate)? (step 3 lines)
3. The orientation and the edge mapping natively (the verification step): Ubuntu's BOE no-flip and edge stretch, not yet seen here.
4. Frame rate and checksum rate at 9.6 vs 19.2 MHz; CPU cost of the polled thread.
5. Multi-touch (Apple's path frames to HSTouchHIDService, DroidVMInput fb07165) and palm rejection: later.
6. Charger noise (USB cable always in): the Ubuntu driver sends a charger state command; not done.

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
