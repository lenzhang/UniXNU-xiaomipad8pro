# Touch: NT36532E on GENI SPI (`PianoTouch`)

Status 2026-11-11: **built, host-tested and linked** (`package/PianoTouch`, listed in `configs/k27-native-kexts.txt`, inert
without `piano-touch=N`). **Not run on the tablet**: every hardware fact marked *inferred* or *needs-boot* below is open until
the first run. First deliverable: a **single-touch absolute pointer** (tap = click, drag = drag). Multi-touch is not done.

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
| IRQ | GPIO162 rising edge = frame ready (latched in TLMM INTR_STATUS, never routed to the GIC) | DT `interrupts = <162 1>` | verified (DT) |
| Panel ID | GPIO100 (`novatek,lcd-id-gpios`): 0 CSOT, 1 BOE; this unit is BOE | DT; plan §1 | verified (DT) / needs-boot (level) |
| Power, reset | none of its own: powered with the panel (lit by ABL/UEFI); reset and boot over SPI | DT (no supply, no reset GPIO); SunUEFI | verified (DT) |
| Clocks | GCC vote 0x52008 bits 18-21 (QUP1 wrapper) + 24 (`wrap1_s2`); CBCRs 0x23140/0x23144/0x23148/0x2315c, SE2 0x1827c; rate: QSPI-ref RCG 0x188c0 through the divider 0x1828c | K90 (same SE, same SoC), 13T QUP1 S4 code, mainline names | **inferred** |
| SMMU | QUP1 stream 0xa3 (SE DMA); GPI is 0xb6 (unused here) | DT `geniqup@ac0000 iommus` | verified |
| Protocol | trim ID `0e .. .. 32 65 03`; cascade: event buffer 0x11c400, poll info 0x1093d8; reset state 0xa0..0xaf, calibrated 0xa3 | SunUEFI, ShengNovatekTouch | verified on piano by SunUEFI (download, idle frames) |
| Frame | 257-byte transport block + THP packet (checksum, 40 x 60 int16); BOE payload 5192 B (frame 5449 B); 120 Hz | plan §1; Ubuntu | verified (Ubuntu) |
| Images | `novatek_nt36532_piano_fw_{boe,csot}.bin`, 245,760 B each (sha256 `b7999389…` / `14b30bb7…`) | xiaomipad-8pro-mainline `firmware/piano-input.json` | verified; both download byte-exact into the host model |
| Orientation | X along the 60 columns (3200 px), Y along the 40 rows, raw Y runs up the panel (flipped by default) | SunUEFI touch-view port; Ubuntu udev flip | inferred: the corner test settles it |

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

Not used in this pass: ShengNovatekTouch's multi-contact tracker, the per-unit calibration (`thp-calibration.json`, private).

## 5. IOKit family / ABI

`PianoTouch : IOService` on `IOPlatformExpertDevice`, a kernel thread, polled. At level 2 a `PianoTouchHID : IOHIDDevice`
(vendor 0x1af4, product 0x5054, 56-byte descriptor: mouse usage, 3 buttons, absolute X/Y 0..32767; the tablet shape that logged in
by tap in the piano VM), matched by a codeless `IOHIDEventDriver` personality; the 334-slot vtable is checked at the KC link.

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-touch=N` | 0 (inert: probe declines, nothing mapped) | 1: steps 1-7, statistics and contacts in the log, nothing for macOS; 2: the same plus the HID tablet pointer |
| `piano-touch-stop=N` | 0 | stop before step N (2: survey only; 4: bus up and self-tested, controller untouched) |
| `piano-touch-bus=N` | 0 | 0 GENI, else SPI by hand; 1 GENI only; 2 SPI by hand only (no SMMU access then) |
| `piano-touch-khz=K` | 9600 | GENI SPI rate, 1000..19200 |
| `piano-touch-seirq=N` | 0 | engine interrupt outputs while polled: 0 off, on if the DMA loopback times out; 1 off; 2 on (13T) |
| `piano-touch-smmu=0` | 1 | do not write the SMMU (read only) |
| `piano-touch-loopback=0` | 1 | skip the engine's loopback self-test |
| `piano-touch-mode=V` | 2 | operating-mode command value (65536: not sent) |
| `piano-touch-baseline=0` | 1 | no idle-baseline command |
| `piano-touch-threshold=T` | 200 | contact threshold of the decoder |
| `piano-touch-flip=B` | 2 | bit 0 X, bit 1 Y flipped |
| `piano-touch-poll=MS` | 1 | edge polling interval |
| `piano-touch-trace=1` | 0 | more frame headers; contact positions also at level 2 |

## 7. Build and kernel collection

`tooling/k27/build-native-piano-touch.sh`: host tests (GeniSpi; the NT36532E model over whole transfers and bit by bit; both
real images downloaded into the model with its CRC check; decoder, tip debounce, HID, clocks), the pinned images staged into
the ignored `out/native-kexts/PianoTouch/firmware` and `.incbin`'d, the objects. `tooling/k27/build-native-image.sh` builds it
with the list; PianoTouch links after PianoA830 (it takes A830's `sm8750_gcc_vote_owner` around the 0x52008 write). Without the
images the build warns and step 5 stops on the target ("no usable image").

## 8. Hardware test plan

Fails safe: RAM boot, `piano-reset-after=180`, back in ABL fastboot, the installed system untouched. Images as the installed
`gpu` profile, so the login window comes up:

```sh
PIANO_VMM_PRESENT=0 PIANO_GPU=2 PIANO_RESET_AFTER=180 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-touch=1" tooling/k27/build-native-image.sh   # level 1
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
grep 'PianoTouch:' logs/native-runs/*-loop/xnu-console.txt | tail -80      # or live: tooling/native/usb-console.py
```

During the run, once the login window shows: tap ~1 cm in from the top-left, top-right, bottom-left, bottom-right corners, then
drag left to right. Level 2: the same build with `piano-touch=2`; tap the user icon, drag; do not tap Sleep/Restart/Shut Down.

Level 1, expected in order: `start: piano-touch=1 ...`; `step 1` with six `pins:` lines, `gcc: vote 0x52008 = ...`,
`gcc: SE2 clock: ...`, `se2: FW_REVISION ... (protocol 1: SPI) IF_DISABLE 00000001 (FIFO disabled: SE DMA)` (or `registers not
read (a QUP1/SE2 clock is off)`), two `firmware CSOT|BOE: 245760 bytes ... parsed`; `step 2`, `smmu: ... bypass set`; `step 3`,
`geni: votes ... branches on`, `geni: SE2 clock N Hz ... divider ...`, `geni: SE DMA through ...`, `geni: init ...: 0`,
`geni: loopback PASSED`, `geni: pins qup1_se2`; `step 4`, `identify (GENI): NT36532 ... id 0e .. .. 32 65 03, table 0 (E),
cascade, event buffer 0x11c400`; `step 5`, `firmware: lcd-id gpio100 = 1: BOE panel`, `firmware: download 1 done ... reset state
0xa0..0xaf`; `step 6`, `start: scanning ...; payload 5192 ...; ... calibrated ...`; `step 7`, `frames: first gpio162 edge`, three
`frame N ... checksum ok` lines, `matrix: reference of 32 frames ...`, `frames at 5 s: ~120 reads/s, ~120 edges/s ...` every 5 s
for 30 s then every 60 s, and per touch `touch N down at (x, y) px` / `touch N up ... after T ms`. Pass: checksum ok >= 99 %,
corners about 125 px (1 cm) in from each edge: near (125, 125), (3075, 125), (125, 2010), (3075, 2010) px, Y growing downward
(Y upside down: `piano-touch-flip=0`; X mirrored: `=3`; both: `=1`). Level 2 adds `HID device: tablet pointer ...` and
`event service IOHIDEventDriver published on the touch screen`; the pointer follows the finger and taps click.

| Line | Meaning, next step |
|---|---|
| no `PianoTouch: start` | KC without PianoTouch, or no `piano-touch` argument |
| `smmu: no bypass ...` / `... did not take` | SE DMA impossible this boot; GENI falls back to SPI by hand |
| `geni: ... branches NOT all on` | the 0x52008 bits do not start the SE2 clocks (inferred bit/branch wrong): fallback |
| `geni: the SE2 clock rate is unknown` | RCG source not decoded (raw values in the line before): fallback |
| `geni: the engine holds no SPI firmware` / `a command is running` | SE2 not usable as SPI here: fallback |
| `geni: loopback FAILED (rc -110 ...) ... moved 0/0`, then a retry | DMA status never latched with the outputs off; the retry turns them on (13T). Both fail: SE DMA does not run on SE2 (GPI DMA is the next step) |
| `geni: loopback FAILED (rc 0 ...): k/64 bytes back` | bytes move but wrong: packing or cache maintenance |
| `bus: GENI not usable: SPI by hand ...` | the fallback ran: steps 4-6 still answer the controller questions; frames then ~10/s, checksums break while touching |
| `identify (GENI): NO NT36532 ... id ff ff ...` (or `00 ...`) then `identify (by hand)` | the engine does not reach the controller (pins, rate, CS) |
| `FAILED at step 4: identify ...` | no answer by hand either: controller unpowered or not on these pins |
| `firmware: lcd-id gpio100 = 0: CSOT` | unexpected for this unit: check the pin before trusting the image choice |
| `firmware: download N FAILED: attempt 3, stage S (rc R)` | stage 1/2/4 with rc -5: transfers fail; 5: auto copy never ends; 8: reset state never 0xa0..0xaf (CRC mismatch, data corrupted: try `piano-touch-khz=4800`) |
| `start: FAILED (rc -110)` | a host command was never taken: the firmware is not running properly |
| no `first gpio162 edge`, ~5 reads/s, 0 edges/s | GPIO162 does not latch; frames are polled every 200 ms, torn |
| many `checksum BAD` | reads not synced to the edge, or slower than a frame: `piano-touch-khz=19200` |
| `N watchdog frames in a row` | controller firmware crashed: the driver downloads again (at most 3 times) |
| `FAILED at step 7: frames: 50 frame reads in a row failed` | the transport died mid-run |
| level 2 without `event service ... published` | IOHIDEventDriver did not attach (personality/IDs) |

## 9. Fallback

`piano-touch=0` (the default) removes the driver from the boot; `piano-touch-bus=2` keeps the controller questions answerable
without SE DMA. Input meanwhile: the keyboard cover, or the Mac's keyboard and mouse over USB.

## 10. Open questions

1. Does SE DMA run on SE2 with the FIFO disabled, and do its status bits latch with the interrupt outputs off? (step 3's loopback)
2. Are the inferred clock facts right (vote bit 24, branch 0x1827c, the rate)? (step 3 lines)
3. Y (and X) orientation, edge accuracy without the private calibration maps (the corner test).
4. Frame rate and checksum rate at 9.6 vs 19.2 MHz; CPU cost of the polled thread.
5. Multi-touch (Apple's path frames to HSTouchHIDService, DroidVMInput fb07165) and palm rejection: later.
6. Charger noise (USB cable always in): the Ubuntu driver sends a charger state command; not done.

## 11. Keyboard cover

Its own page: [keyboard.md](keyboard.md) (`package/PianoKeyboard`, WN8030 over software I2C, `piano-keyboard=N`).

## 12. Licence note

New code under BSD-2-Clause-Patent; reused pieces under their own licences as listed in `package/PianoTouch/UPSTREAM` (one
Apache-2.0 source, NOTICE kept). Linux `nt36532e`/`spi-geni-qcom` (GPL-2.0) were not read for this work; K90's code is facts only.
The panel images and any calibration stay out of every repository.
