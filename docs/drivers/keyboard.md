# Keyboard cover: Nanosic WN8030 over bit-banged I2C (PianoKeyboard), and the folio hall sensor

Status, 2026-11-11: built and linked (inert without `piano-keyboard`). Level 1 ran once on the tablet (240 s, nobody typed):
MCU booted, cover attached, commands acknowledged. No key or touchpad report seen yet; level 2, the hall sensor and the
DRV-2 lock not run. 2026-11-12 (offline, host-tested, not run): the touchpad tuned for macOS (§6a) and the battery work (§6b).
Sources: `package/PianoKeyboard` (design and boot arguments at the top of `PianoKeyboard.cpp`, the codec in `Wn8030Piano.h`, the
descriptors in `HidDescriptors.h`, provenance in `UPSTREAM`). Template: `README.md`.

## 1. Purpose and gate

The magnetic keyboard cover with touchpad as a macOS keyboard (with consumer keys) and pointer: the quickest way to type the
login password natively (plan §4(d).3; touch, `touch.md`, comes later). It gates "login typed on the tablet itself". The USB
HID from the Mac (`usb-device-debug.md` §12) is the fallback in the same image.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Controller | Nanosic WN8030 MCU, I2C client 0x4c on `i2c@a98000` (QUP1 SE6); driven here by software I2C on its pins, QUP not touched | boot DT `keyboard@4c` (`nanosic,wn8030-piano`) | verified (ran) |
| Pins | SDA gpio56, SCL gpio57, data/IRQ gpio97 (rising edge per frame), status gpio95, reset gpio188 and sleep gpio3 (active low) | boot DT; Project-SunUEFI PianoPogoBoard.c | verified (ran) |
| Supplies | `ldob11` 1.8 V (vddio) then `ldob14` 3.3 V (dvdd), RPMh active votes from apps DRV-2 (cmd-db 0x40a00 / 0x41900 on this unit) | boot DT; capture | verified (ran) |
| Firmware | `MCU_Upgrade.bin` (stock 303, 83,436 bytes, pinned SHA-256, never committed) into RAM at each power-up: 101 blocks, ~7 s | build script; capture | verified (ran) |
| Frames | 68 bytes from register 0x4c: `57 seq type` (0x39/0x4a/0x5b/0x6c), reports packed from byte 3, ID 0 ends; byte 67 = sum of bytes 2..66 | v1.1.0 module; capture | verified (capture), the sum *inferred* |
| Reports | 05 keyboard 9 B, 06 consumer 5 B (3 used), 02 mouse 8 B, 19 touchpad 27 B (buttons, 3 x {flags, id, -, X, Y}, count), 22/23 vendor 16/32 B, 24/26 to the frame end | v1.1.0 module, newer port's descriptors | key/touch *needs-boot* |
| Touchpad | X 0..3199 over 94 mm, Y 0..2135 over 42 mm (34 / 51 counts per mm); flags bit 0 tip, 1 in range, 2 confidence; Windows-8 digitizer | newer port's descriptor | *inferred* (Ubuntu works) |
| Vendor | `{id, 31, 38, 80, kind, len, payload, sum}`; a2 attach (byte 9 bit 0 = on; seen 0x23), 64 accelerometer, 24/31/32 auth, f0 `{cmd, status}` ack, 21/25/2e `{value}` state, a4/28/01 unknown | v1.1.0 module; capture | verified (capture), meanings of f0/21/25/2e *inferred* |
| Commands | `32 00 4e 31 80 38 cmd 01 value sum` to register 0x4c: 25 keyboard power (repeated every 12 s), 2e LEDs (fd/fc Caps, f7/f3 mic), 21 touchpad on, 23 backlight 0..100 | v1.1.0 module; capture (acks) | verified (ran) |
| Hall sensor | folio lid switch, TLMM gpio80, gpio-keys `switch-lid`, SW_LID, active low (low = closed); separate from the attach bit | v1.1.0 `piano-boot.dtb` | *needs-boot* |

## 3. Ubuntu as the specification

`drivers/hid/hid-nanosic-wn8030.c` (GPL-2.0), read as behaviour only: the v1.1.0 module piano's Ubuntu runs (`UPSTREAM`
pins it), and the newer port with the touchpad made at attach (embedded in Project-SunUEFI's 7.3 patch set). Per path:
- **read**: `read_frame` checks byte 0 (& 0x7f = 0x57), the type, then packs reports by ID; read again while a frame had
  reports. PianoKeyboard does the same, polling gpio97's latched edge (no GIC interrupt).
- **attach** (`dispatch` kind a2): bit 0 on → keyboard device, then the input-state work: touchpad on (21), Caps LED, mic LED,
  backlight; keyboard power (25) from the 12 s worker. Bit 0 off → devices removed (here: everything released). Tablet mode
  is its inverse.
- **keyboard**: passed through (v1.1.0); Linux's HID core ignores an ErrorRollOver array, so `Keys` keeps the previous keys.
  The newer port maps Right Ctrl + 1..0 - = to F1..F12; here opt-in (`piano-keyboard-fnrow=1`). Mic key 0x6f passes.
- **touchpad**: given to hid-multitouch, which reads the first `contact count` slots and scales by the physical size; here
  `Pointer` does that and makes a relative mouse (macOS has no driver for a third-party touchpad's finger collections).
  Linux sends no mode command but the touchpad enable (21); its transport has no feature reports.
- **auth**: 24 (request, value 00/01 first, 64 repeated) → init "MIAUTH" only with the helper open; 31 (UID) → offline s3t1;
  32 (challenge) → the helper's token. Ubuntu runs without the helper, so nothing is answered by default.
- **hall**: Linux turns input off while SW_LID is closed (touchpad 0, LEDs off, reports dropped). Not done here.

## 4. UniXNU template

UniXNU-sheng's WN8030 driver (`Wn8030.c`, `KeyboardCover.cpp`) for the boot ROM protocol and the touchpad-as-pointer design;
PianoInput's IOHIDDevice pattern; PianoPower's software I2C and RPMh vote; PianoA830's DRV-2 lock. Details in `UPSTREAM`.

## 5. IOKit family / ABI

Two `PianoKeyboardHID` (IOHIDDevice) under the service, vendor 0x1af4 product 0x504b: "Piano Keyboard Cover" (keyboard report 5
+ consumer report 6, LED output report 5 → Caps LED; also the gestures' Control + arrow) and "Piano Keyboard Touchpad" (7-byte
relative mouse: three buttons, X, Y, wheel, AC Pan, with physical extents that declare its resolutions, §6a).
`tooling/k27/kext/PianoKeyboard.json`'s codeless IOHIDEventDriver personality binds them. The DRV-2 owner word comes from PianoA830
(a library of the descriptor). PianoDwc3's USB keyboard/pointer share `HidDescriptors.h`'s keyboard and plain pointer descriptors,
which are unchanged.

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-keyboard` | 0 | 1: steps 1-6, reports logged raw and decoded, nothing to macOS; 2: also the HID devices (key contents not logged) |
| `piano-keyboard-stop=N` | 0 | stop before step N (2: the read-only survey; the hall sensor is still polled) |
| `piano-keyboard-trace=1` | 0 | level 2 logs contents too (types the password into the log: never at the login) |
| `piano-keyboard-auth=1` | 0 | answer the authentication as Linux with its helper (init, s3t1); UID and challenge never logged |
| `piano-keyboard-fnrow=1` | 0 | Right Ctrl + 1..0 - = are F1..F12 |
| `piano-keyboard-hall=0` | 1 | do not read gpio80 |
| `piano-keyboard-lid=1` / `2` | 0 | tell IOPMrootDomain the hall state as a clamshell: 1 with clamshell sleep disabled first, 2 with it allowed |
| `piano-keyboard-touchpad=V` | 1 | touchpad command value on attach (2: not sent) |
| `piano-keyboard-dpi=N` | 400 | pointer resolution declared to macOS (120..4000); lower = faster |
| `piano-keyboard-divide=D` | 1 | touchpad counts per pointer count |
| `piano-keyboard-scroll=C` | 5 | touchpad counts per wheel / AC Pan count; higher = slower scrolling |
| `piano-keyboard-scroll-res=R` | 90 | scroll resolution declared to macOS (10..900; 0 none: macOS's 9) |
| `piano-keyboard-scroll-invert=1` | 0 | scroll against macOS's natural setting |
| `piano-keyboard-gestures=1` | 0 | three-finger swipes as Control + arrow (Mission Control, App Exposé, Spaces) |
| `piano-keyboard-tap=0` | 1 | no tap-to-click (on: 1 finger left, 2 right, 3 middle; no drag lock) |
| `piano-keyboard-typing=MS` | 250 | touches starting within MS of a key going down are ignored (0: off) |
| `piano-keyboard-backlight=P` | 0 | keyboard backlight on attach, 0..100 (over 100: not sent) |
| `piano-keyboard-backlight-idle=S` | 0 | backlight off after S s without a key or touch, back at the next |
| `piano-keyboard-poll=MS` / `-idle-poll=MS` / `-slow-poll=MS` | 4 / 30 / 200 | latch poll while reports flow / idle / detached or closed |
| `piano-keyboard-raw=0`, `-invert=B`, `-mouse=0`, `-halfbit=US`, `-rails=0` | | see `PianoKeyboard.cpp` |

## 6a. The touchpad on macOS (2026-11-12)

macOS (IOHIDFamily, read as a reference) takes a HID pointer's resolutions from the descriptor: `IOHIDEventService::
determineResolution` divides the relative X element's and the Wheel's logical range by their physical range (counts per inch;
400 and 9 when no extents are given), and the HID event system's pointer and scroll acceleration work in those units.
`IOHIDEventDriver` reads Wheel and AC Pan as whole counts and does not implement the Resolution Multiplier feature. So:
- **Speed.** One pointer count per touchpad count (865 per inch of finger travel) declared as 400 dpi: macOS's mouse
  acceleration with 2.2 times a mouse's travel, for a 94 mm pad. System Settings' tracking speed applies; boot-time tuning is
  `piano-keyboard-dpi` (lower = faster).
- **Scrolling.** Two fingers; one wheel count per 5 touchpad counts (~0.15 mm) declared at 90 per inch: macOS's former line
  step comes in ten parts at the same speed (smooth, not line-by-line). Direction: fingers up = wheel up, which under macOS's
  "natural scrolling" (on by default, one setting for every device) moves content with the fingers, as a Mac trackpad does; with
  natural scrolling off it is the traditional direction, again as on a Mac. `piano-keyboard-scroll-invert=1` flips both axes. The
  axis locks after 1.2 mm when one is at least twice the other (straight vertical scrolling, no sideways drift); a diagonal start
  stays free. No inertia (the risk of a runaway scroll without a way to stop it).
- **Clicks.** Taps on: one finger left, two right, three middle; a press is left, or right when it starts with two fingers; the
  pad's second button right. No drag lock. 50 ms without motion after a press changes (a clickpad moves under the press).
- **Typing.** A touch that starts within 250 ms of a key going down is ignored (palm or thumb while typing); its press still clicks.
- **Gestures.** Three fingers moving 9 mm along one axis, once per touch: up Mission Control (Control+Up), down App Exposé
  (Control+Down), left the Space to the right (Control+Right), right the Space to the left (Control+Left), sent through the
  keyboard device (Control, Control+arrow, Control, release). **Off by default**: the touchpad has sent no frame yet, so whether
  its contact count is reliable is only inferred (Linux's hid-multitouch needs it to be). With gestures off three fingers
  scroll, and every touch line still names a recognised swipe (`three-finger swipe up (...)`): when tonight's (a) shows three
  fingers counted cleanly, `piano-keyboard-gestures=1` (and later the default) is safe. Four fingers: not done (three slots).

## 6b. Battery (2026-11-12)

The serving loop has no interrupt; it sleeps, then reads gpio97's latch, which keeps an edge that comes during the sleep. It
sleeps 4 ms while reports flow (2 s after the last frame or edge), 30 ms while the cover is attached and open (the first
keystroke after a pause waits at most that), 200 ms while it is detached or the hall sensor says closed; the insurance read
without an edge goes to 5 s / 10 s idle / slow. A minute of the model with 10 s of typing: ~4,600 wakeups instead of 15,000;
detached: 300. `PianoKeyboard: idle: N wakeups in 60 s (...)` every minute for five minutes, then every tenth. The wait is one
function (`waitForEdge`), so a GPIO interrupt (a shared TLMM helper) replaces only it. A read without an edge now needs gpio97
seen low at the previous wakeup (a line that idles high made every 4 ms poll an I2C read). The hall sensor is sampled every
500 ms. The backlight is off unless `piano-keyboard-backlight=P` (Linux's `nanosic::kbd_backlight`, 0..100); the cover's
keyboard-brightness keys, if it sends consumer 0x79/0x7a/0x7c, step it by 10; `piano-keyboard-backlight-idle=S` turns it off when
idle. Display sleep is not used (natively only NativeGopDisplay knows it). The mic-mute LED stays off (nothing in macOS drives it).

## 7. Build, kernel collection, RAM image

`tooling/k27/build-native-piano-keyboard.sh` runs the host tests (`tests/keyboard_host.cpp`: the captured frames, key/touch/
attach/auth frames of the same shape, the pointer, the I2C engine, cmd-db, descriptors), stages the pinned image, builds the
objects and prints `out/k27/kext/PianoKeyboard.json` for `PIANO_EXTRA_KEXTS` (after the kext list, so PianoA830 links first).

## 8. Hardware test plan (tonight, owner present, cable to the Mac, RAM boots only)

Build all three images first (each `build-native-image.sh` rewrites `out/ram` and `out/ram-dump`, so copy each away):

```sh
KB=$(tooling/k27/build-native-piano-keyboard.sh | tail -1)
# (a) level 1: keys and swipes logged raw, nothing to macOS
PIANO_EXTRA_KEXTS=$KB PIANO_RESET_AFTER=600 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-keyboard=1" \
  tooling/k27/build-native-image.sh && python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
rm -rf out/ram-kbd-raw && cp -R out/ram out/ram-kbd-raw
# (b) the login: GPU, Mac HID, PianoPower level 1, the cover as macOS keyboard and pointer
PIANO_EXTRA_KEXTS=$KB PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=1200 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-power=1 piano-keyboard=2" \
  tooling/k27/build-native-image.sh && python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
rm -rf out/ram-kbd-login && cp -R out/ram out/ram-kbd-login
# (c) the same with the authentication answered
PIANO_EXTRA_KEXTS=$KB PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=1200 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-power=1 piano-keyboard=2 piano-keyboard-auth=1" \
  tooling/k27/build-native-image.sh && python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
rm -rf out/ram-kbd-auth && cp -R out/ram out/ram-kbd-auth
# (d) the login image with the three-finger gestures on (2026-11-12 touchpad and battery work)
PIANO_EXTRA_KEXTS=$KB PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=1200 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-power=1 piano-keyboard=2 piano-keyboard-gestures=1" \
  tooling/k27/build-native-image.sh && python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
rm -rf out/ram-kbd-touchpad && cp -R out/ram out/ram-kbd-touchpad
```

Each run: terminal 1 `python3 tooling/native/usb-console.py --capture 1500` (live log, saved in `logs/usb-console/`);
terminal 2 `tooling/native/native-loop.sh out/ram-kbd-raw/boot-piano-macos-ram-A-bundled.img` (then `ram-kbd-login`,
`ram-kbd-auth`, `ram-kbd-touchpad`). Afterwards `grep -a PianoKeyboard logs/usb-console/<newest>-port0.txt`.

**(a) level 1.** After "cover ATTACHED" (~10 s after the kext starts): type `a`, Shift+`a`, `1`, Caps Lock, a volume/brightness
key, Right Ctrl+`1`; then one-finger swipes left/right/up/down, a tap, a two-finger tap, a two-finger scroll, a press; close the
folio and open it again; detach the cover and attach it again. Proof:
- keys: `key 1: 05 00 00 04 00 00 00 00 00 -> mods 00 keys 04`, then `key 2: ... -> mods 00 keys none`; Shift `mods 02`;
  a media key `consumer N: 06 e9 00 ... (usage 0xe9)`.
- touchpad: `touchpad detected (first report 0x19)`, `touch N: 19 00 07 ...` lines (flags 07, byte 26 the count), and per
  touch `touch gesture N: 1 finger, 400 ms, (x0,y0) -> (x1,y1), travel T, M moves, 0 scrolls`; a tap ends `, TAP (left click)`,
  a two-finger tap `, TAP (right click)`, a scroll `2 fingers ... 0 moves, S scrolls`.
- `command ack N: command 0x21 status 0` and `command state N: command 0x21 = 01` (touchpad on taken).
- hall: `hall: gpio80 ...: cover open at start`, then `hall: cover closed (gpio80 0, ...)` and `hall: cover open (...)`.
- detach: `attach message: state byte 0x.. (bit 0 0 ...)`, `cover detached (attach message)`; re-attach: `cover ATTACHED`
  and the five commands again.

**(b) login.** At the login window type the password on the cover and Return; move the pointer, click. Proof:
`HID device Piano Keyboard Cover: vendor 0x1af4 product 0x504b ...` and `... Touchpad ...`, `event service IOHIDEventDriver
published on Piano Keyboard Cover: macOS has it` (and on the Touchpad), `key report 1 to macOS: handleReport ok`,
`pointer report 1 to macOS: handleReport ok`, `touch gesture` lines, and the desktop appearing. With the GPU: `rails: ldob11:
DRV-2 lock taken (tag 'KBRD') after N waits` (a wait for `0x41383330 ('A830')` is fine).

**(c) auth.** `auth request (answered as Linux with its helper)`, `auth: sent auth init (MIAUTH): ok`; if the cover goes on:
`auth init ... (not logged)`, `auth: sent auth s3t1 ...`, `auth challenge ...`, `auth: the cover's challenge ...: not sent`.
Pass = keys and pointer still work after it (the exchange ends unanswered).

**(d) the touchpad features** (in (b), or an image with `piano-keyboard-gestures=1` added to (b)'s `PIANO_EXTRA_ARGS`): the
start line `touchpad: 400 dpi declared (1 counts per pointer count), scroll 5 counts per wheel count at 90 per inch, natural
scrolling, taps on, typing guard 250 ms, ...`; a two-finger scroll moves page content with the fingers in small steps; three
fingers up/down/left/right give `touch gesture N: 3 fingers, ... three-finger swipe up (Mission Control, Control+Up)` and, with
gestures on, `gesture N: three-finger swipe up (...): sent` and Mission Control on the panel; a tap clicks, two fingers right-click;
touching while typing gives `ignored (typing)`. Battery: `idle: N wakeups in 60 s (... idle at 30 ms ...)` with N about 2,000
while idle and about 300 with the cover detached or closed (15,000 before).

Failure lines and what they mean:
- `rails: ldob11: the DRV-2 lock stayed held by 0x... ('A830') for 2 s` → the GPU kept the lock (its own hang); run (a) first.
- `boot ROM: id 0xffffffff` / `nack address` → MCU unpowered or the bus wrong (rails, reset/sleep levels in the `pins:` lines).
- `firmware: header not taken` / `block N ... nack` → bus errors: try `piano-keyboard-halfbit=4`.
- `frame N: trailer checksum ...` (many) → bus corruption: `piano-keyboard-halfbit=4`. `bad frame type` / `unknown report id` /
  `report past the frame` → a frame layout not seen yet: keep the raw line.
- Typing gives no `key` line but frames arrive → check `command state ... 0x25 = 01` and the hall state (closed covers may
  stop the keyboard); no frames at all → `edges` stays 0 in `alive` lines (gpio97 not rising).
- `mouse N: 02 ...` instead of `touch` → the MCU sends its own mouse reports: the pointer still works from them.
- `touch` lines but gestures with `0 moves` → contact count or flags differ from the descriptor: keep the raw `touch` lines.
- `HID device: init/attach/IOHIDDevice::start` (step -6), or no `event service ... published` → the HID personality did not
  bind; `handleReport 0x...` → IOHIDFamily refused the report.
- `key report ... handleReport ok` but nothing typed → macOS side (HID event system); the Mac's keyboard (`piano-usb-hid=1`)
  tells whether the login field takes input at all.
- `hall: cover closed` with the folio open → polarity; never run `piano-keyboard-lid=1` before that is right.

## 9. Fallback

Without `piano-keyboard` the service does not probe. The Mac's keyboard and pointer over USB (`piano-usb-hid=1`), then the
volume/power keys (PianoInput). A hang is a reset (`piano-reset-after`, the Gunyah watchdog) back to ABL.

## 10. Open questions

1. Key and touchpad reports on the wire (shape, type byte, contact count): tonight's (a).
2. Whether the cover needs authentication for anything (backlight?): (c).
3. The hall sensor's level and polarity, and whether a closed folio presses keys: (a).
4. Kinds a4, 28, 01 and bit 1 of the attach byte: more captures.
5. Clamshell (`piano-keyboard-lid`) and Linux's input-off-while-closed policy: after native sleep exists.
6. Whether 400 dpi / 5 counts per wheel count feel right, and the scroll direction under natural scrolling: (d), then the
   defaults move.
7. Whether the contact count is clean enough for gestures on by default: (a)'s three-finger lines.
8. Whether the cover sends keyboard-brightness consumer usages at all: (a)'s `consumer` lines.

## 11. Licence note

The Linux driver (GPL-2.0) is a specification only; nothing of it is in this repository. PianoKeyboard is BSD-2-Clause-Patent
(`package/PianoKeyboard/LICENSE`); the MCU image is Xiaomi/Nanosic's, never committed.
