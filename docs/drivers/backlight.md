# Panel backlight: two KTZ8866 over software I2C (PianoBacklight)

Status, 2026-11-11: built, host-tested and in the kext list (inert without `piano-backlight`); not run on the tablet. Sources:
`package/PianoBacklight` (design and boot arguments at the top of `PianoBacklight.cpp`, register facts and the state machine in
`Ktz8866.h`, the interface for other drivers in `PianoBacklightClient.h`, provenance in `UPSTREAM`). Template: `README.md`.

## 1. Purpose and gate

The panel never went dark and its brightness was whatever the boot stages left: bad for a tablet in daily use. PianoBacklight
gives (1) a moderate brightness at boot, (2) the backlight off while macOS has the display asleep and back on at wake, (3)
brightness steps from keys, (4) optionally the backlight off while the keyboard cover is closed. It gates "daily use on battery"
(plan M5); nothing else depends on it.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| ICs | 2x Kinetic KTZ8866 (6-channel LED boost + LCD bias), 7-bit address 0x11 each, the same brightness on both | inventory §1; boot DT `kinetic,ktz8866`; datasheet p.24 | verified (sources) |
| Primary | `backlight@11` on `i2c@98c000` (`qcom,geni-i2c-master-hub`, I2C hub SE3), 400 kHz, `enable-gpios` TLMM 2, `current-num-sinks` 6, `kinetic,secondary-backlight` | boot DT | verified |
| Primary pins | gpio70 SDA, gpio71 SCL, function `i2chub0_se3`, pull-up, 2 mA (`hub-i2c3-data-clk-state`) | boot DT | verified; SDA-first order *inferred* (as gpio56/57 for the cover) |
| Secondary | `backlight@11` on `i2c@880000` (`qcom,geni-i2c`, QUP2 SE0), 400 kHz, `kinetic,secondary` | boot DT | verified |
| Secondary pins | gpio0 SDA, gpio1 SCL, function `qup2_se0`, pull-up, 2 mA (`qup-i2c8-data-clk-state`) | boot DT | verified; order *inferred* |
| HWEN | TLMM gpio2 (active high; low = shutdown, registers reset, no I2C). Read only here | panel XML "ktz8866 hwen: gpio 2"; datasheet p.23 | verified (sources) |
| Panel bias | the panel's AVDD/AVEE are the KTZ8866's OUTP/OUTN, enabled by its ENP/ENN pins = TLMM gpio117/118 ("VSP/VSN EN"); registers 0x09-0x0e | panel XML; datasheet Table 4 | verified (sources). Never touched |
| Brightness | 11-bit code: 0x04 bits 2:0 (LSB), 0x05 (8 MSBs); LSB first, the MSB write applies it; code 0 = boost and sinks off | datasheet Tables 9-10 | verified (datasheet) |
| Enable | 0x08 BL_EN: bit 7 software reset (all registers to defaults), bit 6 backlight enable, bits 5:0 the six sinks | datasheet Table 11 | verified (datasheet) |
| Mapping | 0x02 bit 3: 1 linear (reset value), 0 exponential | datasheet Table 7 | *needs-boot* (what UEFI set) |
| Ramps | 0x14: on 7:4, off 3:0 (0 = 512 us, n = 2^(n-1) ms; reset 8 ms); 0x03 bits 6:3 the ramp between codes (reset 2 ms) | datasheet Tables 8, 23 | verified (datasheet); values *needs-boot* |
| FLAG | 0x0f latched faults, cleared by reading: read only with `piano-backlight-flags=1` | datasheet Table 18 | verified (datasheet) |
| Found level | stock firmware's lowest reads 374 on both ICs, highest 2047; Ubuntu/GNOME ran 1135-1256 | Project-SunUEFI display-depth.md, sensors.md | *inferred* for the UEFI handover: *needs-boot* |
| TLMM | pin n at 0x0f100000 + n*0x1000: CTL (pull 1:0, function 5:2, drive 8:6, OE 9, eGPIO 11/12), IO (0 pad, 1 out); none of gpio0/1/2/70/71 in `gpio-reserved-ranges` (36-39, 48-51, 74) | boot DT; PianoKeyboard (ran) | verified |

Power: BL_EN cleared stops the backlight boost and the six sinks, the dominant consumer; it is the datasheet's own off. Code 0 also
stops them, but BL_EN is what the Linux driver and the datasheet use and it keeps the code for the wake. HWEN low would save the
chip's standby current too, but it also drops the LCD bias, i.e. the panel's supply: not available. The panel keeps scanning
(DSI video mode, DPU): panel sleep (DCS display-off/sleep-in) is a separate, later step (`display.md` §8).

## 3. Ubuntu as the specification

Linux `drivers/video/backlight/ktz8866.c` (GPL-2.0) as piano's kernels run it, patched by Project-SunUEFI
(`patches/linux/7.3/0002-display.patch`; behaviour only, nothing copied):
- the secondary (`kinetic,secondary`) has no backlight device of its own; every register write to the primary is repeated on it;
- `update_status`: on → BL_BRT_LSB, BL_BRT_MSB, then BL_EN bit 6 set; off → BL_EN bit 6 cleared, then the fade waited for (0x14's off
  ramp: 512 us or 2^(n-1) ms);
- probe: the level the boot loader left is kept when BL_EN is set and the code is non-zero (`ktz8866_get_initial_brightness`);
- `max_brightness` 2047, linear scale.
PianoBacklight does the same on both buses and adds a floor (never below `piano-backlight-min` while lit) and the read-back of every
write. It does not run Linux's `ktz8866_init` writes (sinks, ramps, bias): UEFI already configured the ICs, and they are left so.

## 4. UniXNU template

UniXNU-sheng's `ShengBacklight` (`package/AppleARMBacklight.cpp`, `docs/drivers/sheng-backlight.md`): the same two KTZ8866 on another
Xiaomi tablet, the 11-bit code on both, the found level kept, requests handed to the driver's own thread. Its GENI I2C core, GCC votes
and interrupts are not used: piano drives both buses in software on their TLMM pins (`BitI2c.h`, PianoKeyboard's, which ran on
piano), so there is no clock, firmware or vote, and no PianoA830 lock is needed. Details in `UPSTREAM`.

## 5. IOKit family / ABI, and macOS's own brightness path

`PianoBacklight : IOService` on IOPlatformExpertDevice, registered (found by class). Interface (`PianoBacklightClient.h`), through
`IOService::callPlatformFunction` (no link dependency), each only recording a request and waking the driver's thread:

| Function | Caller | Parameters |
|---|---|---|
| `PianoBacklightDisplay` | NativeGopDisplay's `do_power_state_change_gated` (with `piano-backlight=2`) | on, system power state, WindowServer's API power state |
| `PianoBacklightStep` | key producers (`piano-backlight-keys=1`): PianoDwc3's HID port now; PianoKeyboard can call it the same way | signed steps, a source name |

Watched properties: NativeGopDisplay's `piano-display-power` (boolean, set with each change; read every second, so a call missed while
PianoBacklight was not yet published is caught up) and PianoKeyboard's `piano-keyboard-hall` (1 open, 0 closed; every 250 ms with
`piano-backlight-lid=1`). Published: `piano-backlight-code`, `-percent`, `-lit`, `-found-code`. `setProperties` (administrator only):
`piano-backlight-percent` (0-100) and `piano-backlight-step` (signed).

**Display sleep hook.** On this stack macOS powers a display off through IOMobileFramebuffer: WindowServer's display sleep calls
`setAPIPowerState`, system sleep `setPowerState`; the family stores both and calls the driver's `do_power_state_change_gated`, where
NativeGopDisplay computes on = both non-zero (AppleParavirtDisplay's rule) and tells WindowServer (`powerstate_notify`). That is the
hook. An "off" before the first "on" (the display not yet taken by WindowServer during boot) is ignored, so the boot screen stays lit.

**Idle dim: not reachable.** k27 is built with `NO_KERNEL_HID`, so IOPMrootDomain's display wrangler (`DISPLAY_WRANGLER_PRESENT`) and
its dim state do not exist; on Apple silicon the dim before display sleep is CoreBrightness's, for a built-in panel with a backlight
service. The display power reaching the kernel is IOMFB's on/off. PianoBacklight logs the raw system/API values of every call, so a
boot shows whether WindowServer ever uses an intermediate API state (then a dim step can follow it).

**Brightness keys and macOS's own control.** NativeGopDisplay publishes its display as an external one (`external = true`, as the VM's
display: the proven path). macOS's brightness keys and slider act on built-in panels through CoreBrightness's backlight service and on
Apple displays; an external third-party display gets no brightness control from macOS (the keys show the HUD and do nothing). The
iPadOS route of UniXNU-sheng (a service answering the AppleARMBacklight class with `backlight-control`) was seen to freeze the screen
there when CoreBrightness did not take it (sheng int-236: QuartzCore waited for a libEDR server that never started), and changing the
display to built-in changes WindowServer's behaviour: neither is done. The working path is therefore kernel-side: the producers of HID
reports see the brightness usages (consumer 0x6F up, 0x70 down) or the combo **Right Ctrl + 1 (darker) / Right Ctrl + 2 (brighter)**
(the F1/F2 places of a Mac keyboard) and call `PianoBacklightStep`; the reports still go to macOS. Done for PianoDwc3 (the Mac's
keyboard: `piano-input.py key brightnessup|brightnessdown|rctrl+1|rctrl+2`). PianoKeyboard (the cover) is owned elsewhere and not
edited: its integration is three lines (`PianoBacklightClient.h` header comment) in its report dispatch. Whether the cover has
brightness keys, and which usages they send, shows in a PianoKeyboard level-1 log (`consumer N: 06 ... (usage 0x..)`).

Steps: linear mapping: full scale divided by 2^(k/3) (three steps per doubling, ~26 % each): 128, 161, 203, 256, 322, 406, 512, 645,
812, 1024, 1290, 1625, 2047 with the default floor; exponential mapping: 16 equal steps.

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-backlight` | 0 | 1: survey (pins, both ICs read, the found level), writes nothing; 2: control (NativeGopDisplay calls out too) |
| `piano-backlight-level=P` | 45 at level 2 | boot brightness, P % of the 11-bit range: given, it is set whatever was found; by default a found level above 45 % is lowered and a lower one kept; 0: as found |
| `piano-backlight-min=CODE` | 128 | floor of every step and level while lit (32..1024); dark is BL_EN, never code 0 |
| `piano-backlight-lid=1` | 0 | cover closed (PianoKeyboard's hall, `piano-keyboard>=1`) → backlight off; open → on (if the display is on). Until the hall polarity is verified (`keyboard.md` §10.3) |
| `piano-backlight-keys=1` | 0 | read by the producers: brightness usages and Right Ctrl + 1/2 step the backlight |
| `piano-backlight-flags=1` | 0 | the survey reads FLAG (0x0f; clears its latched faults) |
| `piano-backlight-halfbit=US` | 5 | I2C half-bit time (~100 kHz; 2..50) |
| `piano-backlight-swap=B` | 0 | SDA/SCL swapped, bit 0 primary bus, bit 1 secondary (only if reads fail with "no ack (address)" on idle-high pins) |

## 7. Build, kernel collection, RAM image

`tooling/k27/build-native-piano-backlight.sh`: the host tests (`tests/backlight_host.cpp`: BitI2c on a bit-level KTZ8866 model with
auto-increment and clock stretching; steps, levels, log limit; the controller's sequences on register models: survey writes nothing,
boot level, display off/on order, steps and floor, lid, partial failures and retries, giving up after 8 failed attempts, a missing
or foreign IC, only 0x04/0x05/0x08 written, BL_EN bit 7 never, bits 5:0 kept; the whole stack; the key watch), then the objects and
the import check. PianoBacklight is in `configs/k27-native-kexts.txt`, so every `build-native-image.sh` links it.

## 8. Hardware test plan (owner present, cable to the Mac, RAM boots only)

Build each image, copy it away (each build rewrites `out/ram`):

```sh
# (a) level 1: the survey alone
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-backlight=1" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-bl1 && cp -R out/ram out/ram-bl1
# (b) level 2: boot level, display sleep/wake, the Mac's brightness keys, a root shell for pmset
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=900 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-usb-shell=1 piano-backlight=2 piano-backlight-keys=1" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-bl2 && cp -R out/ram out/ram-bl2
# (c) level 2 with the cover and the lid (after keyboard.md's hall test showed the right polarity)
KB=$(tooling/k27/build-native-piano-keyboard.sh | tail -1)
PIANO_EXTRA_KEXTS=$KB PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=900 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-keyboard=2 piano-backlight=2 piano-backlight-lid=1" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-bl3 && cp -R out/ram out/ram-bl3
```

Each run: `python3 tooling/native/usb-console.py --capture 1000`, `tooling/native/native-loop.sh out/ram-bl1/boot-piano-macos-ram-A-bundled.img`,
then `grep -a "PianoBacklight\|NativeGopDisplay: display" logs/usb-console/<newest>-port0.txt`.

**(a)** Expected, within seconds of the kext's start, nothing written, the panel unchanged:
```
PianoBacklight: start: piano-backlight=1 (read-only survey); boot level 45 % (default: only lowers a higher found level), floor 128, ...
PianoBacklight: step 1: survey (read only): pins, then both ICs
PianoBacklight: pins: gpio70 primary SDA (i2c@98c000, I2C hub SE3)    CTL 000002xx (function N, pull 3, drive 2 mA, input) IO 00000001 (pad 1, out 0)
PianoBacklight: pins: gpio2  KTZ8866 HWEN (read only, never written) CTL ... IO 0000000x (pad 1, ...)
PianoBacklight: bus primary: SDA gpio70, SCL gpio71: used (software I2C, pins given back after each burst)
PianoBacklight: survey primary: ID 0x2? (KTZ8866, device field 4); BL_CFG1 0x..: OVP .. V, ..., linear|exponential mapping, ...; code NNNN (P %, ...); BL_EN 0x7f: backlight ON, sinks 0x3f
PianoBacklight: survey primary: LCD bias 0x09 0x.. (LCD_EN 1, ...) ... ramps 0x14 0x.. (on .. us, off .. us); 0x15 0x.. (full scale ..)
PianoBacklight: (the same two lines for the secondary)
PianoBacklight: found: primary answered, code NNNN (P %) lit, secondary answered, code NNNN (P %) lit; both KTZ8866
PianoBacklight: level 1: survey done, nothing written (bus accesses: 18 + 18 reads, 0 + 0 errors, pins restored 1 + 1 times)
```
Record the found code, the mapping, the ramps and the full-scale current: they decide the floor and the steps.

**(b)** Expected: `boot level: found code NNNN (P %), set 921 (45 %, ...)` with two `write primary|secondary: BL_BRT_LSB ... read back ok`
lines (the panel dims visibly), or `... not above 921 (45 %): kept`; then `control: ON, linear mapping, 13 steps from 128 to 2047, ...`.
When WindowServer starts: `NativeGopDisplay: display on` and `PianoBacklight: display power: system 1, WindowServer 1: on` (possibly
preceded by an ignored "off ... before the first on"). Then:
- on the Mac: `python3 tooling/native/piano-input.py key brightnessdown` (and `brightnessup`, `rctrl+1`, `rctrl+2`) →
  `step -1 from the Mac's keyboard (PianoDwc3 HID): code 921 -> 812 (40 %)` + two `write ...: read back ok` lines, the panel changes;
- display sleep: in the root shell (`python3 tooling/native/usb-console.py --port 1`; the helper in the root image,
  `usb-device-debug.md` §6) `pmset displaysleepnow` →
  `NativeGopDisplay: display off`, `display power: system 1, WindowServer 0: off`, two `write ...: BL_EN 0x08 = 0x3f (was 0x7f): backlight
  OFF, sinks 0x3f kept; read back ok`: the panel goes black (it still scans);
- wake: a key from the Mac (`piano-input.py key shift`) → `display on`, `BL_EN 0x08 = 0x7f (was 0x3f): backlight ON` on both, the
  picture back at the same level; also leave it idle past the display-sleep timer (`pmset -g` shows it) once.
- every 10 minutes `alive: controlling, code ..., lit; N writes, 0 failed accesses; calls: display N, steps N`.

**(c)** Close the folio: `lid closed`, both `BL_EN ... backlight OFF`; open: `lid open`, both `backlight ON`.

Failure lines and what they mean:
- `bus ...: NOT used: a line is low as found` → the bus is held (the QUP engine mid-transfer, or no pull-up): nothing is tried on it.
- `bus ...: NOT used: a pin is routed to the other TLMM (eGPIO)` → the pins belong to another processor: no software I2C there.
- `read primary 0x01: no ack (address)` with idle-high pins → SDA/SCL the other way round: `piano-backlight-swap=1` (or 2 for the
  secondary); `clock stretch timeout` → SCL held low by the IC or the wrong pin as SCL.
- `survey ...: ID 0x.. (NOT a KTZ8866 ...)` → another device at 0x11, or a misread: control refuses to start.
- `control: OFF, the secondary did not answer ...` → level 2 stays read only (one IC alone would light half the panel).
- `pins of the ... bus: CTL read back differs after the restore` → the pin configuration did not return as found: stop and report.
- `write ...: READ BACK DIFFERS` / `WRITE FAILED` → retried every second; `control: OFF after 8 failed attempts in a row` → given up,
  the ICs stay as they are (a reboot restores everything: HWEN and the UEFI's display init).
- No `display power` line after `NativeGopDisplay: display on` → the call did not arrive: the 1 s property poll still applies it
  (`display power from NativeGopDisplay's property: ...`); if neither appears, `piano-backlight=2` did not reach NativeGopDisplay.
- `REFUSED write ...` → a bug: the guard stopped a write outside 0x04/0x05/0x08 bit 6 or under the floor.

## 9. Fallback

Without `piano-backlight` the service does not probe and NativeGopDisplay's callout is off: the backlight stays as the boot stages
left it. A hang or a bad state is a reset (`piano-reset-after`, the Gunyah watchdog): ABL's power-on sequence raises HWEN and UEFI
programs the ICs again (*inferred*: a warm reset resets TLMM, so HWEN drops and the registers reset; confirm once by rebooting with
the display asleep).

## 10. Open questions

1. The found state (code, mapping, ramps, full scale, BL_EN): (a).
2. Does WindowServer's display sleep reach `do_power_state_change_gated`, and with which API values (dim?): (b) with `pmset displaysleepnow` and an idle timeout.
3. Is the floor (128) visible but dim enough? Lower it with `piano-backlight-min` after (b).
4. The cover's brightness keys (usages) and the PianoKeyboard integration: a PianoKeyboard level-1 log, then its owner.
5. The hall polarity before `piano-backlight-lid=1` (`keyboard.md` §10.3).
6. A restart with the display asleep: does the next boot come up lit (§9)?
7. Panel sleep (DCS display-off/sleep-in) for more savings while dark: `display.md` §8, later.

## 11. Licence note

Linux's ktz8866 driver and Project-SunUEFI's patch to it (GPL-2.0) were read as the specification only; nothing of them is in this
repository. Register facts come from the public KTZ8866 datasheet. PianoBacklight is BSD-2-Clause-Patent
(`package/PianoBacklight/LICENSE`), its I2C engine PianoKeyboard's (same licence).
