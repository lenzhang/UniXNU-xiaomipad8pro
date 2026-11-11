# Touch: NT36532E on GENI SPI (ShengNovatekTouch + QcomGeniSpi), keyboard cover note

Status: design, 2026-11-10. Nothing built or run. Template: `README.md`.

## 1. Purpose and gate

Multi-touch on the 3200x2136 panel as an IOHID digitizer, so the login window and the desktop work by touch. Gates
"login by touch" (after M2; plan §4(d).3). The keyboard cover (I2C HID, §11) is the earlier login path and the fallback.

Plan R6 called this the largest single uncertainty, assuming a new Darwin THP daemon. The finding of this page: the
UniXNU component for the same controller family already processes the raw matrix **in the kernel driver**
(ShengNovatekTouch: "The tracker confirms tip-down on the second scan ... A quiet matrix releases the contact"), and the
Ubuntu piano THP processing core is Apache-2.0/MIT userspace from the same `xiaomi-sheng-thp` lineage. So the piano work
is a sensor configuration and an SPI/IRQ/firmware transport for an existing component, not a new daemon.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Controller | Novatek NT36532E, host-processed (THP): the controller streams raw electrode frames, the host tracks fingers | `streamB-input-report.md` a); `device/piano-thp/README.md` | verified |
| Bus | GENI SPI `spi@a88000` (QUP1 SE2) | plan §4(d).3; `native-display-gpu-research.md` §1 | verified (DT) |
| Frame rate | 120 Hz frames over physical SPI with IRQs (supervised diagnostic DTB) | `device/piano-thp/README.md` | verified (one session) |
| Matrix | 40 x 60 electrodes | plan §4(d).3 | verified (Ubuntu THP) |
| Output space (Ubuntu) | 12 slots, X 0..31999, Y 0..21359 (10x panel pixels), MT only, INPUT_PROP_DIRECT | `streamB-input-report.md` a) | verified |
| Orientation | raw Y inverted relative to the panel (udev calibration is libinput-only on Ubuntu) | `streamB-input-report.md` c) problem 1 | verified |
| Firmware | per panel vendor; this unit BOE. Loaded by the host into the controller at probe | plan §4(d).3; `device/piano-thp/README.md` (`nt36532e-thp` transport, NTP1 contract) | verified (file name only) |
| Calibration | per unit `thp-calibration.json` (private, `private/`, never committed) | plan §4(d).3; `.gitignore` | verified |
| Power / reset / IRQ GPIOs | from `sm8750-xiaomi-piano.dts` touch node (supervised diagnostic variant enables it) | Ubuntu kernel v1.1.0 | needs reading; needs-boot |

## 3. Ubuntu as the specification

- Transport: kernel `nt36532e-thp` misc driver (experimental, GPL-2.0, in `linux-sm8750-piano` v1.1.0): SPI framing, IRQ,
  firmware download (NTP1 contract documented beside the driver). Read as behaviour only.
- Processing: `xiaomipad-8pro-mainline/device/piano-thp/src/` (Apache-2.0 and MIT SPDX headers; core from
  `xiaomi-sheng-thp`, provenance in that repository's `NOTICE`): `frame.cpp` (frame decode), `calibration.cpp`,
  `nvt_touch_core.cpp` + `nvt_finger_filter.cpp` (tracking/filtering), `orientation.hpp` (Y flip), `input.cpp` (uinput
  slots). Licence permits reuse with attribution; keep the NOTICE chain.
- Known gaps on Ubuntu (README): held contacts still break up; orientation fix not rerun; accuracy, multitouch, pen not
  validated. These carry over as open questions.

## 4. UniXNU template to build on

- **ShengNovatekTouch** 69fdd62 (`NvtProtocol.c`, `NvtTouchCore.c`; NT36532 protocol and sheng sensor configuration;
  tracker over the raw matrix). Piano delta: sensor geometry 40x60, piano electrode calibration loaded from the payload
  (private), Y inversion, output range, BOE firmware image.
- **QcomGeniSpi** eac47ba (UniXNU catalog): GENI SPI controller interface. Piano delta: SE base of `spi@a88000`, clocks
  left on by UEFI/ABL (needs-boot), GPI DMA vs FIFO mode.
- **ShengHidInput** (UniXNU-sheng submodule): the IOHID digitizer publication (`touch-hid=1`) used on macOS 27.3 sheng.

## 5. IOKit family / ABI

IOHIDFamily: an IOHIDDevice publishing a multi-touch digitizer report descriptor (as ShengHidInput does on sheng for
macOS 27.3); on k27 the VM's `DroidVMInput` touch path (Apple multitouch protocol to HSTouchHIDService, commit fb07165
of macOS-DroidVM) is the reference for what macOS 27 accepts.

## 6. Boot args

`touch=4` (ShengNovatekTouch mode, as sheng), `touch-hid=1` (publish as HID digitizer). Default off in the piano board
until the first read-only frame test passes.

## 7. Kernel archive / component

`component:ShengNovatekTouch`, `component:QcomGeniSpi`, ShengHidInput; piano sensor configuration as a board file in
`package/` (geometry, orientation, range; calibration from the payload, not compiled in).

## 8. Hardware test plan

After M2 (XNU RAM boot with the kernel log on the panel):
1. Read-only: controller ID over SPI, no firmware download; log the ID. Pass: the NT36532E ID matches Ubuntu's.
2. Firmware download from the payload, frames streamed, frame counter and one raw frame summary in the log. Pass: 120 Hz.
3. Tracker on, HID off: contact positions logged for four corner taps (expected ~(100,100), (3100,100), (100,2036),
   (3100,2036) panel pixels, as `streamB-input-report.md`'s test plan). Pass: Y grows downward.
4. `touch-hid=1`: the login window reacts to taps.
Fails safe: RAM boot, Ubuntu returns on reboot; touch off by boot arg.

## 9. Fallback

Keyboard cover (§11), then the volume/power keys in the UniXNU menu; touch disabled by `touch=0`.

## 10. Open questions

1. Does the BOE firmware offer a self-reporting (coordinate) mode at all? If yes, it bypasses the tracker. (source read of
   the Ubuntu transport + one SPI test)
2. Which GPIOs/regulators must be raised before SPI works natively, or does ABL/UEFI leave them on? (needs-boot)
3. Held-contact break-ups seen on Ubuntu: same tracker, same symptom expected; fix once for both. (replay test offline)
4. Where the private calibration lives in the payload (Data seed vs a separate FAT file) without entering any repository.

## 11. Keyboard cover (summary; its page is [keyboard.md](keyboard.md), driver `package/PianoKeyboard`)

"Xiaomi Keyboard" 15d9:00a3 + touchpad 15d9:00a1 (clickpad 3200x2136) + "Tablet Mode" switch, over GENI I2C
`i2c@a98000` through a nanosic HID bridge (MCU firmware `nanosic/piano/MCU_Upgrade.bin`, 83436 bytes, identity only)
(`streamB-input-report.md` a); `tools/build-boot.py` KBD_FIRMWARE). Darwin side: an I2C-HID transport into IOHIDFamily,
with UniXNU-sheng's WN8030 keyboard-cover driver (keyboard + pointer on macOS 27.3) as the template and Ubuntu's
`hid-nanosic` as the behavioural spec.

## 12. Licence note

Linux `nt36532e-thp` and `hid-nanosic` (GPL-2.0): specification only. `device/piano-thp` (Apache-2.0/MIT) and
ShengNovatekTouch (its own licence, check `LICENSE`/`NOTICE`) may be reused with attribution. No calibration data in
any repository.
