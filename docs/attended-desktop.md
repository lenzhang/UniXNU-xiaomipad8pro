# A usable desktop: what hurt it, what changed, and the next attended session (2026-11-12)

Offline analysis of the attended runs with the owner on 2026-11-11/12 (attend1-4: `logs/usb-console/20261111-232732`,
`20261111-234057`, `20261112-000758`, `20261112-002843`), the night's unattended GPU boots (`20261112-*`, among them fast1
`041235` with `pc-sample=30`, and desk1 `081349`, the attend5 image from main, run unattended at 08:14) and the driver pages. T is
seconds from XNU's start. **Evidence** is a quoted log line or measured count; **inference** says so. Nothing here ran on the
tablet; the fixes are host-tested and built. Not covered: NativeGopDisplay's composition (the cursor layer, the fence rule, swap
completion, bands and flicker), which is analysed separately; its findings are only referred to.

## 1. Findings, by symptom

### 1.1 The wait between the password and the desktop (47-73 s)

Timed from the password's submit (`exec: pid N authorizationhos`) to the first desktop frame (NativeGopDisplay's status line
with a lit, non-grey frame):

| Run | Submit | authorizationhost exits | Dock / Finder exec | First desktop frame | Submit to desktop |
|---|---|---|---|---|---|
| attend1 (Data volatile) | 355.5 | 368.5 | 377.4 / 377.6 | 403-413 | 47-58 s |
| attend4 (Data resumed) | 175.9 | 189.0 | 193.4 / 197.2 | 239-249 | 63-73 s |
| attend2, attend3 | 1187-1206, 709-737 | | | not captured | > 74 s, > 62 s |

- **authorizationhost takes 13.0 s every time** (evidence: 12.98 s in attend1, 13.03 s in attend4); the session's agents
  (MenuBarAgent, WindowManager, WallpaperAgent) start only after it exits. attend2's stderr just before its exit:
  `aks:pid:-1,:_aks_verify_password:1956: aks connection failed`, and SecurityAgent `ACM ... err = -5`; launchd says `failed to find
  AppleKeyStore service` at boot. Inference: a fixed timeout around the absent AppleKeyStore / credential manager. The diag image's
  report prints authorizationhost's own log lines of that window (§3), which name the step; no fix is attempted without them.
- **Then 26-56 s in which WindowServer sends 2-4 frames** while the GPU is almost idle (`row0 GPU RUN 160M b2 ... 68C`, ~2 % busy,
  `row1 DCVS 160-160`), 0 frames dropped, 0 ticks stalled. In that window: every Metal process's precompiled pipelines are refused
  (`native-metal: binary archive refused ... RenderBox`: compiled from source, on the CPU), the wallpaper is the video Aerial, its
  4K frames converted on the CPU (`VTDecoderXPCServ: ... surface texture refused: 4096x2160 YCbCr format 543 converts on the CPU`,
  attend2), and the second iconservicesagent grows 64 → 163 → 362 MB of GPU memory. Inference: CPU-side work (pipeline compiles
  with cold caches, the video wallpaper, icon rendering). The resumed store (attend4) started Dock 4.4 s after the authorization
  instead of 8.9 s (attend1, volatile), so warm caches help; a still wallpaper takes the video off the path
  ([drivers/gpu-userspace.md](drivers/gpu-userspace.md) open risk 2).
- **The idle load**: PerfPowerServices spins ~1.7 cores of user time in every boot, audiomxd with configd ~0.5 cores (fast1's
  pc-sample: `task PerfPowerServices kernel 1 user 12955` of ~14 k user samples a period; `caught burning CPU` in every boot,
  [power.md](power.md) §1.2). Three jobs respawn every 10 s (§1.4). Their share of the 47-73 s is not measured.
- **Keyboard Setup Assistant** starts after every login (evidence: attend1 426.7, attend3 759.0, attend4 244.2, and
  `PianoA830: KeyboardSetupAssist ... opens the accelerator`: it draws a window): macOS asks the type of a keyboard it does not know
  (ours: vendor 0x1af4). A window in front of the new desktop.
- **attend1's first 150 s of typing did not log in**: 100 key reports by 283.7, the submit only at 355.5. With the touch screen's
  Y mirrored in that run (`orientation: flip X 0, Y 0`), tapping the password field or the user is unlikely to have worked
  (inference).

### 1.2 The black phase and the garbled phase at login

- Black: attend1 `381.911 ... last frame 0.0% non-black mean #000000` (372-382), attend2 black then an 11 %-lit frame: the stall of
  §1.1 with almost no new frames.
- Garbled (evidence): frames copied before the GPU finished them, `1 gave up after 50 ms` with a `76.4% non-black mean #7e7e7f`
  frame (attend4 ~229), an 11 %-lit one (attend2), a frozen `#aaa6a1` one (attend3); during the stall that partial frame stays on
  the panel for 10-40 s. These images had the old 50 ms copy-anyway rule; the current NativeGopDisplay never copies an unfinished
  frame (its own analysis and page).

### 1.3 Crashes and restarts of the session

| Process | Per boot | Reason (evidence) | State |
|---|---|---|---|
| watchdogd | 15-17 | `exit ... status 0x5 reason 2:5` (SIGTRAP) soon after each start; no IOWatchdog natively; KeepAlive, relaunched every 10 s, each crash a `Denying core dump` and a ReportCrashService run of ~1 s | disabled in the desk images (§2) |
| locationd | 0-1 | `status 0xb` (SIGSEGV), once, in attend2 only | left |
| WindowServer | 1 (attend4) | `BoMap -> 0xe00002be (NoResources)` at 366.4, then `status 0x6`: the session back at the login window | fixed (GPU map limits, Turnip 0006; [drivers/gpu.md](drivers/gpu.md) §8) |
| the whole system | attend1-3 | resets 1-2 min after login, attend3's console stops at ~807 | fixed (the volatile Data disk's pool, [data-store.md](data-store.md)) |

No crash of loginwindow, Dock, Finder, SystemUIServer, ControlCenter or NotificationCenter in any run. Their SIGKILL/SIGTERM exits
are attend4's logout after WindowServer's abort, and universalaccessd/MenuBarAgent at every login (the login window's agents
replaced by the user's). SetStoreUpdateService (~30 a boot) and the `com.apple.audio.*` helpers exit with SIGKILL: idle XPC
services stopped by launchd, as on any Mac.

### 1.4 The respawn loop every few seconds

Four spawns per 10 s at the idle login window, each through an xpcproxy (attend4, T 139-170, every cycle the same):

| Job | What happens | Evidence | Inference |
|---|---|---|---|
| `com.apple.watchdogd` | starts, SIGTRAP, ReportCrashService runs ~1 s | `exec: pid 350 watchdogd` ... `status 0x5 reason 2:5`, `exec: pid 351 ReportCrashServi` ... exit 1.0 s later | no IOWatchdog provider (its job matches `IOProviderClass IOWatchdog`; macOS 26.6's plist) |
| a job whose exec fails (`exit: pid N xpcproxy status 0x4e00`, exit 78) | 2.2 s after each watchdogd crash | `exit: pid 361 xpcproxy status 0` then `... status 0x4e00`, no exec in between | `com.apple.mobileactivationd`: k27's default `k27-deny-exec` (bsd/kern/kern_exec.c, k27 0035) makes its exec fail with EPERM, and a client keeps a request queued, so launchd retries every 10 s. The diag report's `failing` lines name it (`launchctl list` status 78) |
| `com.apple.security.xprotectd` | starts and exits 1 at once | `exec: pid 353 xprotectd`, `status 0x100`; nothing on its stderr | KeepAlive true (26.6's plist): its reason is in its own log (diag report) |
| `com.apple.endpointsecurity.endpointsecurityd` | exits 35 every 30-70 s | `status 0x2300`; stderr `No need to downcall, there are no early boot clients` | left (launchd's EndpointSecurity boot task) |

`droidvm-disable-jobs=com.apple.watchdogd,...` in every image changes nothing natively ([boot-time.md](boot-time.md) §4).
Also seen for every new process: `triggered unnest of range ... of DYLD shared region` (a private copy of the shared region's
page tables per process; observation only).

### 1.5 Clicks that did nothing, and the rest of the input

- **Touch screen taps read as drags** (evidence, attend1): 61 touches, 31 taps, 16 drags of which 11 under 310 ms, most pressed
  25-100 ms before the lift and ending 6-70 px from their start (`drag ends after 100 ms, 32 px from its start`, `... 424 ms, 6 px`,
  `... 84 ms, 49 px`): a lifting finger's centre moves past the 32 px slop. A drag presses where the finger landed and releases
  elsewhere: no click on a button, no double click. Holds that wobbled past 32 px became drags instead of right clicks (`drag ends
  after 1050 ms, 39 px`). Two two-finger taps of 517-526 ms were lost (500 ms limit).
- **The cover touchpad's two-finger presses are right clicks** (evidence, attend4): 9 of 31 presses had two fingers down, all with
  0 pointer moves, several with a contact at Y 1800-1914 (the edge nearest the user, a resting thumb); the owner pressed with two
  fingers from gesture 19 to 26, then switched to one. attend1: a press, then three two-finger presses within a second (a double
  click attempt turned into right clicks, inference).
- **Phantom cover taps** (evidence, attend1): all three cover taps (33-56 ms) came while a finger was on the screen, 11-16 ms before
  the screen's own click (`212.067 touch gesture 1: ... 45 ms ... TAP (left click)`, `212.081 gesture: tap -> click (116 ms)`): one
  click became two.
- **Near misses**: a 181 ms two-finger tap (limit 180), a 116 ms tap with 110 counts of travel (limit 85).
- **No report lost in transport**: no `handleReport` failure in any run, `1233 reports sent, 0 failed` (touch), `handleReport ok`
  (cover), the Mac's HID `0 send errors`.
- **Touch screen reliability** (evidence, desk1): the controller sat in its watchdog state most of the boot, 10314 watchdog blocks
  among 17296 reads by 1051 s, each download followed by `watchdog (state 0xfd) again 2233 ms after its download`. The first frame
  after a start comes ~0.85 s later (the boot's first edge after 842 ms), but the busy pace read without an edge 50 ms after a
  download (inference from the host model, which reproduces it, [drivers/touch.md](drivers/touch.md) §13). attend1's touch
  recovered after two downloads; attend2's never did.
- **Keyboard cover keys**: delivered (299 keys by T 420 in attend4, 0 rollovers). **Brightness keys**: never pressed in any run
  (`consumer 0`, PianoBacklight `steps 0`). **Caps Lock LED**: no evidence either way (only the attach-time command logged).
- **Cover close/open**: the hall sensor never changed in any run (`hall: ... cover open at start`, no later hall line): the cover
  was never closed. PianoPolicy was not enabled (no `piano-policy`), `piano-keyboard-lid=0`, `piano-backlight-lid=0`: closing the
  cover would have done nothing.
- **Display sleep at an idle login window works**: attend2's backlight went off at ~767-797 and came back with a key over the
  Mac's HID at ~1190.
- **The Mac's keyboard and pointer (PianoDwc3)**: used once (attend2), `0 send errors`.

### 1.6 The images: attend5 and desk1 have no keyboard cover

PianoKeyboard was never in `configs/k27-native-kexts.txt`; attend1-4 linked it through `PIANO_EXTRA_KEXTS`. The images built since
(`out/ram-attend5`, `ram-attend5nt`, `ram-desk1`) do not contain it (no `piano-keyboard-typing` in them, no `PianoKeyboard: start`
line in desk1's capture), although their arguments say `piano-keyboard=2`: **the owner could not type on the cover with them**,
only through the Mac (`piano-input.py`). Fixed: it is in the list now (§2).

### 1.7 Logging

`stderr-log=1` puts every daemon's stderr on the console: attend2 35577 stderr lines (contextstored 13866: CoreData errors,
dmd 4576, linkd 3475). desk1's capture (`stderr-log=1`) reached only T ≈ 195 s of a 1233 s run: the console never caught up, so
the run says nothing about the login window. `-v` draws every console line into the Device-mapped boot framebuffer until the first
frame ([boot-time.md](boot-time.md) §3.1). The desktop images have neither (§3); the diag image has `exec-log=1` only.

## 2. Changes (2026-11-12, this branch)

| Change | Where | Tested |
|---|---|---|
| Touch screen: slop 64 px (32), a drag only after 80 ms past it or past three times it (a lift before is the tap), two-finger taps up to 700 ms (500); the tap line says how far the finger went | `package/PianoTouch/TouchGesture.h`, `piano-touch-slop`, `piano-touch-drag-delay` | `touch_host`: drift taps, delayed and fast drags, a wobbling hold, a 560 ms two-finger tap |
| Touch screen: after a download and start, no read without an edge until the first frame (2 s at most) | `TouchPace.h` `StartGrace`, `PianoTouch.cpp` | `touch_host`: the model with an 850 ms start reproduces desk1 without it, recovers after one download with it |
| Cover touchpad: a resting thumb (bottom fifth, another contact above) not counted; two fingers pressing > 45 mm apart: left; taps up to 220 ms / 150 counts; a tap during a screen touch dropped; the press's button, the vetoed taps and every LED request logged; the alive line in two | `Wn8030Piano.h` `Pointer`, `PianoKeyboard.cpp`, `PianoTouchClient.h` (`PianoTouchContact`); `piano-keyboard-resting`, `-tap-ms`, `-tap-travel`, `-tap-veto` | `keyboard_host` `testRestingThumb` |
| PianoKeyboard in the kext list | `configs/k27-native-kexts.txt` | the KC links (22 kexts) |
| The desktop images: desk, quiet, plain, diag | `tooling/k27/build-desktop-images.sh` | built in a worktree (§3) |
| The diag image's boot-job report | `tooling/k27/desk-reports.sh` | `tooling/k27/test-desk-reports.py` |

The desk seed's launchd overrides (disabled, so launchd never loads them and their Mach services are not advertised; a client
gets an invalid connection at once): `com.apple.watchdogd`, `com.apple.mobileactivationd`, `com.apple.security.xprotectd`,
`com.apple.PerfPowerServices` (and `com.apple.audiomxd` in quiet), with `k27-deny-exec` for watchdogd and PerfPowerServices as well
in case a 27.0.1 label differs. And `/Library/Preferences/com.apple.keyboardtype.plist` with our two keyboards as ANSI (`20555-6900-0`
the cover, `20565-6900-0` PianoDwc3's: product-vendor-0 => 40), written by the boot job as root, so Keyboard Setup Assistant does not
ask (inference: the preference administrators pre-set for the same purpose; checklist step 3 checks it). Risk (inference): a client that needs one of those daemons fails at once instead of finding a crashing or spinning one; none
of them answered before either. The plain image is the A/B partner.

## 3. The desktop image

Built from main, after these commits (the KC with the k27 0064 kernel for the RTC calendar):

```sh
K=$(sed -n 's/^PIANO_K27_TREE=//p' ~/.config/piano/env)
K27_KERNEL=$K/out/k27/kernel-k27-0064-native.macho PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh
DESK_RTC_OFFSET=1794397906 tooling/k27/build-desktop-images.sh desk quiet plain diag
```

`build-native-image.sh` restages the Turnip seed only when its inputs changed (the same seed as attend5's otherwise). The RTC
offset is the rtc run's (`tooling/native/rtc-offset.py`, 2026-11-12, the same within 1 s across PSCI resets); without
`DESK_RTC_OFFSET` the clock starts at 1970 until `settime.sh`, as before. Every image is a RAM image, Data volatile (a new seed:
`data-persist=0`), reset after 3600 s (`DESK_RESET_AFTER`), no `-v`. Run:

```sh
tooling/native/native-loop.sh out/ram-desk/boot-piano-macos-ram-A-bundled.img out/ram-desk-dump/boot-piano-macos-ram-A-bundled.img
python3 tooling/native/usb-console.py --capture 3640     # in parallel: the console with the Mac's times
```

desk's command line as built (925 bytes; DarwinBoot takes 1023; `droidvm-utc` is the build time):

```
debug=0x14e kt_fixed_zones=120 -no_slto_panic tlto_us=0 -apfs_shared_datavolume offline-apfs-load=1 owner-staged-apps=1
mac-amfi-launch-permissive=1 union-hide-lower=1 malloc-ranges-off=1 arm64_maxoffset=0x7FFE000000 droidvm-timebase-hz=24000000
exec-env=com.apple.WebKit.WebContent:GIGACAGE_ENABLED=no rootauth=0 vm_compression_limit=1572864 droidvm-tz=Asia/Singapore
pstore-console=0x9cb00000 pstore-console-size=0x700000 piano-reset-after=3600 piano-reset-to=bootloader native-ufs-root=1
native-ufs-diagnostics=1 k27-deny-exec=/usr/libexec/mobileactivationd,/usr/libexec/watchdogd,/usr/libexec/PerfPowerServices
data-persist=0 droidvm-utc=<build time> piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-power=1 piano-usb=2
piano-usb-hid=1 piano-keyboard=2 piano-keyboard-gestures=1 piano-backlight=2 piano-backlight-keys=1 piano-gpu-hang-ms=10000
piano-touch=2 piano-rtc=1 piano-rtc-offset=1794397906 sheng-smp=1
```

Checked in an agent worktree (2026-11-12): the KC from this tree with the 0064 kernel and PianoKeyboard in the list (24 natives;
`kc-space`: `__DATA` 246.4 KiB free, largest gap 63.1), main's Turnip stage as the input seed (`f61764674739968c`, attend5's);
`verify-ram-images.py` PASS for desk, quiet, plain, diag and the dump image; desk's seed identity `1474f691e8e29bc4`, its boot job
`sleep 0; sh /private/var/settime.sh; defaults write /Library/Preferences/com.apple.keyboardtype keyboardtype -dict-add
20555-6900-0 -int 40 20565-6900-0 -int 40`. Those images are not for the session: the owner's are built in the main checkout
(main's current collection, 08:12, has neither PianoKeyboard nor the 0064 kernel).

| Image | Seed | Extra | Use |
|---|---|---|---|
| `ram-desk` | overrides + keyboard type | — | the session |
| `ram-quiet` | + audiomxd disabled | — | if desk is still slow (media playback may need audiomxd) |
| `ram-plain` | the Turnip stage's, unchanged | — | when desk misbehaves: the same drivers with the stock userland |
| `ram-diag` | keyboard type + `desk-reports.sh` in the boot job | `exec-log=1 piano-policy=1` | one boot to name what §1.1 and §1.4 leave open: `grep -a 'desk-reports:' <capture>.txt` |

Variants by environment: `DESK_TOUCH=0` (no touch screen: the fallback if it resets or its watchdog lines never stop),
`DESK_EXTRA="piano-touch-idle=0"` (the touch screen's level-1 pace: every frame, more power), `DESK_KEYBOARD_TYPE=41|42` (ISO, JIS).

The owner's own Data (persistent: a still wallpaper, warm caches and answered setup dialogs survive the reboot) is ESP v3 with the
system layer ([persistent-boot.md](persistent-boot.md) §11); its RAM test with `PIANO_DATA_PERSIST=2` keeps the installed base seed,
so no seed change: there the same quieting is the owner's, once, in Terminal (each `sudo` asks his password):
`sudo launchctl disable system/com.apple.watchdogd` (likewise `com.apple.mobileactivationd`, `com.apple.security.xprotectd`,
`com.apple.PerfPowerServices`; undone with `launchctl enable` and a reboot) and
`sudo defaults write /Library/Preferences/com.apple.keyboardtype keyboardtype -dict-add 20555-6900-0 -int 40 20565-6900-0 -int 40`.

## 4. The attended checklist (next session)

The owner types his password himself, on the cover. Console: `python3 tooling/native/usb-console.py --capture 3640` from the boot;
afterwards `python3 tooling/native/boot-timeline.py logs/usb-console/<ts>-port0.times.txt --loop <loop.log>`.

1. **Boot `ram-desk`.** No kernel text on the panel while it boots (no `-v`: the console is on the Mac only). At the login window
   (T ~110-150): the pointer visible and following the cover's touchpad; the menu bar
   clock shows today's time (the RTC calendar; a wrong date: rebuild without `DESK_RTC_OFFSET`). Console: `PianoKeyboard: start:
   piano-keyboard=2`, `touchpad: a tap at most 220 ms ...`, no `Denying core dump for pid N (watchdogd)` (before: one every
   10 s), and 3 minutes in no `caught burning CPU` for PerfPowerServices. (desk has no exec-log, so spawns are not on the console:
   the disabled jobs are checked in Terminal after the login, `launchctl print-disabled system | grep -E
   'watchdogd|mobileactivation|xprotectd|PerfPower'`, or by the diag image's report.)
2. **Touch screen at the login window**: tap the user / the password field, the four corners and the middle. Each tap lands under
   the finger (Y not mirrored), and logs `gesture: tap -> click (N ms, M px at most from the landing)`: note the largest M. Watch
   for `frames: watchdog`: after a download the line `frames: the first edge N ms after start S` should follow and the touch come
   back. If watchdog lines never stop: next boot with `DESK_EXTRA="piano-touch-idle=0"`, then `DESK_TOUCH=0`.
3. **Log in on the cover.** Note the time from Return to the desktop (stopwatch: 47-73 s before; diag's exec-log dates
   authorizationhost, 13 s before). Expected: no garbled frame (current NativeGopDisplay), no Keyboard Setup Assistant window
   (the keyboard type written by the boot job; if it still asks, answer it and note the keyboard it names).
4. **Set a still wallpaper** (System Settings › Wallpaper, a photo or a colour, not a video one) and note whether the desktop is
   more responsive (Dock magnification, opening Finder). On volatile Data it lasts this boot only.
5. **Clicks on the cover touchpad**: one-finger press with the thumb resting at the bottom edge → left click (console: `pressed
   (left)`, `a resting thumb left out`); two fingers close together pressed → right click (`pressed (right)`); one-finger tap →
   click; two-finger tap → context menu; double-click a Finder folder by tapping twice; two-finger scroll in a Finder window and
   in Safari (direction: natural); three-finger swipe up → Mission Control, left/right → Spaces.
6. **Clicks on the touch screen**: tap Dock icons, a Finder item; double-tap a folder; long-press (500 ms) → context menu; drag a
   window by its title bar (a drag starts ~80 ms after moving); two-finger scroll; two-finger tap → context menu. While a finger is
   on the screen, a touchpad tap is dropped (`tap dropped (a finger on the screen)`).
7. **Keys**: type in TextEdit or Notes (letters, Shift, numbers); Caps Lock on/off: the cover's LED follows and the console logs
   `LED request N from macOS: 0x02 (Caps Lock on)` and `command 0x2e caps lock LED = 253: ok`; the brightness keys (the top row's,
   and Right Ctrl + 1 / 2): `PianoBacklight: step +1 from the cover: code A -> B (P %)` lines and the panel's brightness changes.
8. **Ten minutes of ordinary use** (Finder, Safari's start page, System Settings): no reset, no return to the login window, no
   `gpu call failed` line. `top -o cpu` in Terminal: nothing above ~30 % at rest (PerfPowerServices gone); if audiomxd and configd
   lead, the next boot is `ram-quiet`.
9. **Cover close/open** (diag image, or desk with `DESK_EXTRA="piano-policy=1"`): close the cover onto the screen, wait 5 s, open;
   fold it behind the tablet; detach and attach. Console: `PianoKeyboard: hall: cover closed (gpio80 0, ...)` / `... open` with each, and PianoPolicy's
   `would ...` decision lines (level 1 acts on nothing). This verifies the hall polarity (keyboard.md §10.3) before anything acts
   on it; display sleep on close needs `piano-policy=2 piano-policy-lid=1` and the policy agent of the userland stage (later).
10. **Display sleep**: leave it idle (or `pmset displaysleepnow` from the root shell if the image has one): the backlight goes off;
    a key on the cover brings it back with the desktop (not the login window unless the lock screen setting asks).
11. **Once, `ram-diag`**: boot, log in as in step 3, use it for 3 minutes, then `grep -a 'desk-reports:' <capture>.txt`: the failing
    jobs (`failing (...)` lines: which label exits 78), `| ` lines with authorizationhost's log (the 13 s), xprotectd's reason, the
    phase lines of loginwindow / Dock / WindowServer, the busiest processes.

Stop and report on: a reset or a hang (the capture's last lines and the loop's dump), WindowServer exits, `BoMap` refusals, touch
watchdog lines that never stop, or clicks that still do not land (note where: screen or touchpad, which gesture line).

## 5. Open

- authorizationhost's 13 s (diag report first); the 26-56 s of CPU-side work after Dock starts (pipelines compiled from source:
  the plugin's binary archive support; a persistent store keeps caches).
- The touch screen's display-off pace: desk1 also went into the watchdog state with the display still off at boot (T 105.9, reads
  ~250 ms apart, no edges); counted only until the display returns, where the start grace now applies.
- mobileactivationd as the exit-78 job is inferred; the diag report or `launchctl print system/com.apple.mobileactivationd`
  settles it.
