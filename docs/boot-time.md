# Boot time to the login screen (native k27, 2026-11-11)

Status: offline analysis of the native runs' logs, a build switch (`PIANO_FAST_BOOT`, off by default) and a timeline tool. **Not
run on the tablet.** Every saving below is an estimate until the test in §6 has run.

## 0. The answer

- The 07:44 login run reached the login screen at about T 140-150 s (T: seconds from XNU's start, which comes 20 s after `fastboot
  boot`). It was also the first boot of a new Data seed (`NativeRamBlock: Data store state 3`, fresh), so every cache was cold.
- About 40-45 s go before launchd runs and 17 s to launchd's boot tasks. WindowServer starts at about 66 s, loginwindow at about
  90-110 s, the first frame comes at about 105-113 s, SecurityAgent at about 140 s, and the lit login frame follows.
- **The largest cost found is the verbose console.** With `-v`, XNU draws every console line as text into the 3200x2136 boot
  framebuffer until WindowServer's first frame. This configuration maps that framebuffer as Device memory, so every scroll
  repaints the screen's changed cells with uncached stores, synchronously, in whichever thread printed the line. Two measurements
  over the logs show the cost: before launchd, `hello` = 26.5 s + 75 ms per console line printed before it (41 runs); launchd's
  boot tasks take 28-37 s with `exec-log=1 stderr-log=1` and 20 s without them. `PIANO_FAST_BOOT=1` drops `-v`; the pstore mailbox
  and the USB console still get every line. Estimated saving: 15-40 s.
- **loginwindow waits 5.0 s** in MobileGestalt for an IOKit match that never comes ("giving up waiting for match"), on every
  launch (measured in the 05:40 run's unified log). The fix needs the matched service, which is still unknown (§3.3).
- **`droidvm-disable-jobs` does nothing natively.** Its reader was the iPadOS line's kernel launch policy (VM-era sources, not in this repository). The k27
  kernel and the KC contain no reader (0 matches), and bluetoothd, locationd and watchdogd run in every native run. The native way
  to keep a daemon from running is k27's `k27-deny-exec=PATH[,PATH...]`. Its default is `/usr/libexec/mobileactivationd`, and
  setting the argument replaces that default. `PIANO_FAST_BOOT=2` adds watchdogd to it, which ends watchdogd's crash loop (about 8
  crashes and 10 ReportCrash runs per 300 s).
- No 30 s or 60 s timeout shows on the login's critical path in any console log. TimeSync/clocksyncd, configd, opendirectoryd,
  securityd, the Data volume and logd are not waiting (§3.5).

## 1. Sources and clocks

| Source | What it gives | Caveat |
|---|---|---|
| `logs/native-runs/20261111-074440-loop/dump.bin` decoded with `tooling/native/xnu-log.py decode` (5653 lines) | the login run: exec-log, stderr-log, GPU trace, display lines | heavy DRAM decay (vote threshold 3 of 11): names were matched fuzzily, with decay-aware matching (a bit 1 → 0 costs less); a decayed digit can change a pid |
| `logs/usb-console/20261111-0{65641,71439,72646}-port0.txt`, the mailboxes of the 04:30-06:29 runs | clean exec order (exec-log=1), launchd's boot tasks | no Mac-side times yet (`usb-console.py` stamps lines only since the evening of 2026-11-11). The USB console replays the mailbox from its start on every (re)open, so a capture holds several copies |
| launchd's own lines (`com.apple.xpc.launchd|1970-01-01 07:MM:SS`, verbose boot only) | event times | the calendar starts at XNU's uptime when there is no RTC (`clock_initialize_calendar` takes the larger of the RTC and the uptime), so 1970 07:30:00 (the 1970 Asia/Singapore offset) is T 0. launchd writes these lines in batches, so a line's position in the log says nothing |
| PianoA830's trace (`t=` µs) and its meter lines (one 60-period window each, from S10 at T 28.4) | the GPU's times; the first window's `rs 2` dates WindowServer's first jobs before T ~88 | printed by PianoA830's logger thread, behind the events |
| `settime: <date>` (the seed's boot job) and `droidvm-utc` | an exact T: `date - droidvm-utc` (07:44: 23:46:45 UTC - 1794354270 = T 135.0) | the boot job runs late (§3.3) |
| fsck_apfs's "current time (N)" in its stderr | T 113.35 at the moment it printed | |
| the loop (`loop.log`) | "XNU started" (our UEFI's fastboot gone) and the reset; `piano-reset-after` resets at mach time N, which counts from XNU's start (reset to ABL fastboot measured at 11-16 s in all runs) | ±1 s polling |

## 2. Timeline of the 07:44 login run

Image: `PIANO_GPU=2` (`piano-gpu-delay=20`), vmm-present 0, `panic_on_crash_disabled=1 exec-log=1 stderr-log=1`, no USB console,
reset at 300 s. The Data store was fresh (a new seed).

| T (s) | Event | How it is dated |
|---|---|---|
| -20 | `fastboot boot` (242 MB, 5.6 s download), ABL, our UEFI, DarwinBoot | loop.log |
| 0 | XNU starts | loop.log |
| ~7 | the KC's kexts start (PianoA830, NativeUfs, NativeGopDisplay, ...) | A830 trace: bring-up at 27.3 = start + `piano-gpu-delay` 20 |
| 27.3-28.4 | the A830 bring-up S0-S10 (S3's 512 MiB pool 1.0 s). The accelerator is published, and the host CX/MX and DDR votes go up (gpu.md) | trace `t=` |
| < 45 | root mounted (disk5s1), Data mounted by init, `exec: pid 1 launchd` | log order; the 45 s run 071155 got as far as launchd's exec but not its first line |
| 48.0 | launchd `hello` (47.8-50.2 s in every run without the USB console) | launchd stamp |
| 48.0-65.4 | launchd's boot tasks. In the clean 07:26 log the slow ones are `fips` (cc_fips_test) 6.3 s, `dirhelper` 5.9 s, `dirs_cleaner` → `xpcroleaccountd` 4.7 s and `restore-datapartition` 3.2 s | launchd stamps |
| 65.4 | `Early boot complete. Continuing system boot.` | launchd stamp |
| ~66 | Installer Progress (pid 63, the boot progress UI, RunAtLoad), logind (88), keybagd, amfid, opendirectoryd, powerd, securityd, syslogd, then WindowServer (102), runningboardd, airportd, coreservicesd, ... | log order right after the early boot |
| < 88 | WindowServer: MetalPluginName, two 0x4147 opens, `device Adreno (TM) 830`, its first 2 GPU jobs | the first meter window (`rs 2`) |
| ~70-90 | timed, mDNSResponder, trustd, sysextd, syspolicyd, the date/mount helpers (pids 144-183) | order |
| ~90-110 | **loginwindow** (pid 186) | after the T 88 meter line and before the first frame |
| ~105-113 | `NativeGopDisplay: first frame (swap 1); boot console released from the screen` (a black frame). The verbose console stops drawing here | fsck_apfs prints T 113.35 seventy lines later |
| ~113 | fsck_apfs / diskarbitrationd of the other volumes; watchdogd's third crash; ReportCrash | fsck's clock |
| ~130 | the seed's boot job (`org.droidvm.run.0`, which runs settime.sh; its shell is pid 278) | order |
| 135.0 | `settime:` sets the clock from 1970 to 2026 | settime and droidvm-utc |
| ~138-145 | **SecurityAgent** (pid 389), the login window's UI, after the boot job ends | after the settime line, before the T 148 meter line |
| ~145 | VTDecoderXPCService (the wallpaper video, decoded in software) | order |
| ~140-150 | the first lit frame: `last frame 100.0% non-black mean #9f9f9f`, the login screen | after settime; gpu-userspace.md read about 150-160 |
| 300 | `piano-reset-after` | |

Crash and respawn loops (whole run): watchdogd 9 execs, about 8 SIGTRAP exits (`exit reason namespace 2 subcode 0x5`), each
followed by a ReportCrashService run (about 10). xprotectd ran 11 times (`exit ... status 0x100`, exit 1 on demand, not a crash;
16 in the 07:14/07:26 runs). endpointsecurity exited 35 (`0x2300`) or 33 (`0x2100`) several times. airportd asserted once
(`AssertMacros: !eachConnection.isLocal`), and contextstored printed a burst of about 150 CoreData errors (one failing store
open). A spindump ran near the end. mobileactivationd never shows up: k27's default `k27-deny-exec` denies it. The `exit ...
xpcproxy status 0x4e00` lines (exit 78) are such failed spawns.

## 3. Where the time goes

### 3.1 The verbose console (T 0 to the first frame)

What the code does:
- `-v` puts XNU's console in text mode (`pexpert/arm/pe_init.c` PE_create_console: "the ARM graphics path otherwise ignores -v").
  `initialize_screen` maps the boot framebuffer `VM_WIMG_IO` unless `HAS_UCNORMAL_MEM || APPLEVIRTUALPLATFORM`, and BCM2837 has
  neither, so the 27 MB framebuffer is Device memory.
- `console_write` (osfmk/console/serial_console.c) mirrors each write into the pstore mailbox first (k27 0055). Then, unless
  `disableConsoleOutput`, it queues the write into the console ring and drains the ring in the calling thread. A writer whose
  chunk does not fit spins, draining the ring, until it fits.
- Draining draws glyphs, and every line at the bottom scrolls. `gc_scroll_up` repaints every character cell that changed, which on
  a screen full of long lines is nearly all of them.
- NativeGopDisplay calls `PE_initialize_console(nullptr, kPEDisableScreen)` at WindowServer's first frame. That sets
  `disableConsoleOutput` and ends the drawing. In graphics mode it is set from the start, by `gc_enable(FALSE)`.

What the logs show (`logs/native-runs/*/xnu-console.txt`, every run with a launchd `hello`):
- Kernel phase: over 41 runs without the GPU, launchd's `hello` = 26.5 s + 75 ms × the console lines before launchd's exec. Plain
  runs have about 290 of those lines and say `hello` at 47.8-50.2 s. `piano-usb=2` adds 45-185 lines (51.7-60.9 s), and
  `piano-keyboard=1` brings it to 515-546 lines (66.0-67.5 s). The added drivers also do real work, so the slope is an upper bound
  per line.
- launchd's boot tasks (hello → early boot complete) take 19.5-21.0 s in 16 plain runs (179-183 lines each time). With
  `stderr-log=1` they take 25.6 s, and with `exec-log=1 stderr-log=1` 28-37 s. The boot tasks run one after another in launchd,
  and launchd writes its own verbose lines to `/dev/console`, so each of its lines and each task's stderr is drawn before it goes
  on (inferred from the code path).
- The GPU runs are faster after T 28: 8.4 and 15.1 s plain, 17.4 s with exec+stderr-log. The A830's bring-up raises CX/MX and DDR
  votes, and Device-memory stores and page-ins speed up with them (inferred). vmm-present 0 alone does not do this: the soft-Metal
  runs took 37 s.

Estimate for `PIANO_FAST_BOOT=1`: 10-20 s in the kernel phase (at 40-75 ms per line, the slope of the plain runs to that of all
runs), 0-10 s in launchd's boot tasks, and an unknown share of the 40 s from WindowServer to the first frame, where about 1200
lines are printed. Total: 15-40 s.

### 3.2 launchd: exec to `hello`, and the boot tasks

launchd's first line (`hello`) comes at 48 s in every plain run, whatever the GPU printed in between. The 45 s run (071155) had
launchd's exec but not yet its `hello`, so launchd's own start-up takes a few seconds after a kernel phase of about 40-45 s.
dyld's "result from check_np(): -1, errno 12" is the first process mapping the shared cache (normal). The boot tasks cannot be
disabled: they are launchd's own and part of the OS image. The test's `pc-sample=30` (k27 0019) tells whether this phase is CPU
bound.

### 3.3 WindowServer → loginwindow → SecurityAgent → the lit frame (T 66 to ~150)

- logind (pid 88) starts the console session, and launchd spawns the `loginwindow console` instance on its request
  (the local bring-up notes, `loginwindow-launch.md` §2, not published). In the 07:44 run loginwindow came 84 pids after
  WindowServer. In the 05:22 run it came 4 pids after. The trigger is not in doubt; what delays the request is not visible in the
  console. The diagnostic run (§6, run D) samples logind and loginwindow.
- **loginwindow's MobileGestalt wait** (05:40 run, `logs/native-runs/20261111-054010-loop`, its log loop printed loginwindow's
  unified log). For loginwindow 616, a warm relaunch:
  - 39.347 `Login Window Application Started`; 39.509 `WindowServerConnectionMade`;
  - 39.554 `Unable to read diagnostic data from service`, then **5.0 s** of nothing;
  - 44.555 `copySyscfgDictionary(void) enumeration of <private> failed` and `giving up waiting for match - runloop result is 3`
    (kCFRunLoopRunTimedOut);
  - 44.578 `FinishLaunchingStart`; 44.788 `Login Window Started Security Agent`; 51.541 `LoginwindowUIShown`.
- So each loginwindow launch loses 5 s before SecurityAgent: the next one (646) logged the same `Unable to read diagnostic data`
  at its start. MobileGestalt wants a syscfg/diagnostic-data service that this platform lacks; the log hides its name
  (`<private>`). Next step, with no OS image change: find the service name (private-data logging for one boot, or a
  `sample-name=loginwindow` stack in that window), then publish that service from a kext, or set the property its match needs. Not
  done here.
- **A cold first boot.** The 07:44 run started a fresh store, so the dyld caches, the Metal shader caches
  (`com.apple.metal/.../m2v`, about 40 s cold in the VM) and the first-launch work of every agent were all cold. A boot with the
  store resumed (`Data store state 2`) is the steady state; compare only those. A changed seed (staging inputs) starts the store
  over (gpu-userspace.md).
- The seed's boot job (settime) runs late, at about T 130. SecurityAgent followed it in the five soft-Metal and GPU runs whose log
  has both, but in the 05:22 run SecurityAgent (pid 236) started long before settime (line 1531 against 2793). So settime does not
  gate the login. Once `piano-rtc=1` (docs/drivers/rtc.md) is in use the clock is right from the start, and the job's jump goes
  away.
- WindowServer's frames start at about 105-113 s. From there to the lit frame, SecurityAgent's start and the wallpaper video
  (software HEVC decode in VTDecoderXPCService) remain. A still wallpaper (gpu-userspace.md, open risk 2) takes the video off the
  path.

### 3.4 Respawn loops

- watchdogd: no IOWatchdog device natively (vmm-present 0). It traps soon after it starts (SIGTRAP). Its job (as in macOS 26.6's
  plist) is `KeepAlive {SuccessfulExit = false}`, `_PanicOnCrash {PanicOnConsecutiveCrash}`, launched on IOKit matching of
  IOWatchdog, and launchd relaunches it about every 30 s. Every crash is a core-dump denial plus a ReportCrashService run. About 4
  of these cycles fall before the login screen. Cost: a few CPU seconds (estimated 2-5 s of wall time if the boot is CPU bound).
- xprotectd (exit 1 on each of its 11-16 on-demand runs) and endpointsecurity (exit 33/35) also respawn. They are left alone:
  their clients (syspolicyd, the Gatekeeper checks of the non-platform Metal plugin, launchd's EndpointSecurity boot task) may
  wait on them.

### 3.5 Suspects checked and cleared

| Suspect | Finding |
|---|---|
| TimeSync / clocksyncd | PianoNullEthernet's built-in en0 releases TimeSync (`TimeSync released`), and WindowServer gets its device and composites. No wait |
| configd, opendirectoryd, securityd, keybagd | start at T ~66 with the system bootstrap. No "timed out" or "waiting" line in any run's console. The only waits printed are NativeUfs `waiting for IOBSD`, dlil's thread start and DockChannel's dext (`waiting for server`), all in the kernel phase or harmless |
| mDNSResponder | one start, one exit right after (status unreadable in the decay); no loop |
| the Data volume | mounted by init in the kernel phase (`mounting volume Data, requested by: init`); the store resumes (state 2) or starts fresh (state 3) without a wait line |
| coreservicesd / launchservicesd | loginwindow's `Process unable to access CoreServicesUIAgent` is an immediate lookup failure, not a wait |
| logd | runs; the log loop reads it |
| airportd | one assertion, no respawn |
| settime | late, but not gating (§3.3) |
| launchd throttle | only watchdogd's crash loop (§3.4) |

## 4. droidvm-disable-jobs and watchdogd

- Reader: the iPadOS line's kernel launch policy (`XNU_BOARD_LAUNCH_POLICY`; VM-era sources, not in this repository)
  writes the listed labels into launchd's `disabled.plist` before launchd starts. k27's patches (0001-0060) have no such hook:
  `droidvm-disable-jobs` has 0 matches in `kernel-k27-0059-native.macho`, `kernel-k27-0060-native.macho` and
  `out/k27/kc-k27-native.bin`. The only userland that touches it is settime.sh's `sysctl kern.bootargs | tr` (seen in stderr-log).
- Effect: bluetoothd (07:44 pid 94), locationd and watchdogd run in every native run checked (06:56, 07:14, 07:26, 07:44, and the
  earlier ones of loginwindow-launch.md §7). So the argument is inert, and `panic_on_crash_disabled=1` is what keeps watchdogd's
  exits from panicking.
- What does work natively:
  - the seed's own `disabled.plist`: stage-piano.sh's `--disable-job` (AppSSODaemon, cryptexd, fskitd, mds index/scan, msrpc).
    None of those run. It is an OS image (seed) change, and a new seed starts the store over;
  - `k27-deny-exec=PATH[,PATH...]` (k27 0035, in the native kernel: `k27-deny-exec` and `/usr/libexec/mobileactivationd` are in
    its strings, a 256-byte value). exec fails with EPERM, so launchd's spawn fails and the job's xpcproxy exits 78 (inferred: the
    `exit ... xpcproxy status 0x4e00` lines). mobileactivationd is denied by default and never runs. **Setting the argument
    replaces the default**, so it must list mobileactivationd again. Since 2026-11-12 `PIANO_DENY_EXEC=PATH[,PATH]`
    (`build-ram-images.py`) and `build-esp.py --deny-exec PATH` add paths and keep the default (and `PIANO_FAST_BOOT=2`'s watchdogd)
    in the one argument (`power.md` §1.2: PerfPowerServices and audiomxd);
  - a test seed's override database: `tooling/k27/stage-boot-run.py --disable-job LABEL` (2026-11-12) adds the label to the copy's
    `disabled.plist`, as stage-piano.sh did; the seed's identity changes, so such images run Data volatile.
- `PIANO_FAST_BOOT=2` denies watchdogd. It refuses to build without `panic_on_crash_disabled=1`, because the job is
  `_PanicOnCrash` and the failed spawn may count as a crash. Risk: clients that register with watchdogd
  (`com.apple.watchdogd.optin.registration`: daemons that opt in to the userspace watchdog) find its port without a server. With
  the crash loop they found one some of the time. If one waits for a reply without a timeout, it hangs. Hence level 2 is a
  separate test (§6, run C), not part of level 1.

## 5. Changes, ranked by expected saving

| # | Change | How | Expected saving | Risk | State |
|---|---|---|---|---|---|
| 1 | no `-v`: graphics console, no text drawn into the Device-mapped framebuffer | `PIANO_FAST_BOOT=1` (build-ram-images.py), `build-esp.py --fast-boot 1` | 15-40 s | no kernel text on the panel (no panic photo; the mailbox and the USB console keep everything); launchd prints no verbose lines, so its boot-task stamps go (`pc-sample` and the USB console's times stand in); XNU may draw its boot progress image | built, not run |
| 2 | compare boots with the store resumed; keep the staged seed unchanged between tests | measurement rule (`Data store state 2` in the log) | the cold first boot's extra (unknown; first-use shader translation alone was about 40 s in the VM) | none | doc |
| 3 | loginwindow's 5 s MobileGestalt match timeout | find the service it waits for (§3.3), then provide it from a kext or the ADT | 5 s per loginwindow launch | depends on the service | open |
| 4 | the GPU's CX/MX/DDR votes earlier: `PIANO_GPU_DELAY=0` (gpu.md's own "try it once") | the existing switch | unknown: launchd's boot tasks ran 1.2-2.4x faster after the votes; with 0 the votes come at ~T 8 instead of 28 | the bring-up runs during the kernel's root phase (untested); see also the race in §6 | test E |
| 5 | watchdogd's crash loop | `PIANO_FAST_BOOT=2` (`k27-deny-exec=/usr/libexec/mobileactivationd,/usr/libexec/watchdogd`) | 2-5 s | watchdogd's clients (§4) | built, not run |
| 6 | the USB console's and the keyboard's console lines (before launchd in text mode: +4-13 s with `piano-usb=2`, +18-20 s with the keyboard too) | comes with #1 (their lines are no longer drawn) | included in #1 | none | — |
| 7 | `droidvm-disable-jobs` | dropped by `PIANO_FAST_BOOT>=1`; drop it from K27_ARGS once measured | 0 s (frees 147 of the 1023 bytes) | none (inert) | built |
| — | `exec-log=1 stderr-log=1` | test images only; after #1 they cost little (one pstore copy per line) | 8-17 s of the boot tasks in text mode (§3.1) | — | — |

Not changed: TimeSync, opendirectoryd, securityd, configd, WindowServer and loginwindow are needed. The seed's disabled jobs stay
as they are (an OS image change). bluetoothd and locationd keep running: denying a daemon that is up and answering could make its
clients wait.

## 6. The test

Build and run. build-native-image.sh stages the seed again only when its inputs change; with the seed the tablet booted last, the
store resumes (check point 1 below). Record the USB console with the Mac's times:

```sh
# run B (the change); run A is the same with PIANO_FAST_BOOT=0 (the baseline on a resumed store)
PIANO_FAST_BOOT=1 PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=600 \
PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 exec-log=1 stderr-log=1 piano-gpu-hang-ms=10000 pc-sample=30" \
  K273_KERNEL=<prepared kernel-k273-...-native.macho> tooling/k273/build-k273-image.sh  # 27.3 only; root-layout caveat in k273-line.md §4
tooling/native/usb-console.py --capture 720 &          # logs/usb-console/<time>-port0.txt and .times.txt
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
python3 tooling/native/boot-timeline.py logs/usb-console/<time>-port0.times.txt --loop logs/native-runs/<ts>-loop/loop.log
python3 tooling/native/boot-timeline.py logs/native-runs/<ts>-loop/xnu-console.txt      # the mailbox, order only
```

Command lines (with droidvm-utc and sheng-smp=1): A 827 bytes, B 693, C 761 (DarwinBoot passes 1023). `pc-sample=30` prints the
idle/user/kernel split, the busiest tasks and the hottest kernel pcs every 30 s, about 60 lines per period. Over 600 s with
exec+stderr-log the mailbox (667 KB per copy) can wrap; the USB capture keeps everything.

| Run | Switch | Purpose |
|---|---|---|
| A | `PIANO_FAST_BOOT=0` | baseline on a resumed store (07:44 was a fresh one) |
| B | `PIANO_FAST_BOOT=1` | change 1 |
| C | `PIANO_FAST_BOOT=2` | + watchdogd denied (change 5) |
| D (diagnostic) | B + `sample-name=logind,loginwindow sample-period=10` in `PIANO_EXTRA_ARGS` | what logind and loginwindow wait on (k27 0014/0036: each thread's wait and user pc/lr with the image) |
| E | B with `PIANO_GPU_DELAY=0` | change 4, and the cure if B shows the race below |

What run B must show (boot-timeline.py prints each line's T):
1. `Kernel boot args:` without `-v` and without `droidvm-disable-jobs`; `NativeRamBlock: Data store state 2` (resumed). State 3
   means a fresh store: repeat the run before comparing. The panel shows no text until the first frame, then the login screen as
   before.
2. `BSD root: disk5s1` and `exec: pid 1 launchd` clearly earlier than run A's. Expected (an estimate): launchd's exec by T 30
   (07:44: < 45). No `com.apple.xpc.launchd|` lines: launchd is not verbose without `-v`. That is expected, not a failure.
3. `PianoA830: bring-up done` (T ~28) **before** `exec: pid N WindowServer`. If WindowServer comes first it has no accelerator and
   aborts about 30 s later (`exit: pid N WindowServer status 0x6`), and the relaunch costs those 30 s. Then run E.
4. `NativeAdrenoMetal: WindowServer: device Adreno (TM) 830`, `exec: pid N loginwindow`, `NativeGopDisplay: first frame (swap 1)`,
   `exec: pid N SecurityAgent`, and a `NativeGopDisplay: ... 100.0% non-black` status line (the login screen). Target: the lit
   frame by T ~110-135 (07:44: ~140-150; run A gives the resumed-store baseline).
5. `pc-sample:` lines every 30 s. A high idle share before the login screen means waits; a low one means CPU (then the
   busiest-task lines name who).
6. No panic, no new `Denying core dump` names, and WindowServer exits only as in run A.

Run C in addition: no `exec: pid N watchdogd` and no `Denying core dump for pid N (watchdogd)`; repeated `exit: pid N xpcproxy
status 0x4e00` instead (launchd retrying the spawn, throttled); WindowServer and loginwindow as in run B. If WindowServer or
loginwindow stops progressing while watchdogd is denied (no first frame, or no SecurityAgent), a client is waiting for watchdogd:
drop level 2.

Reading a failure: if nothing follows `exec: pid 1 launchd`, run A (with `-v`) tells whether launchd depends on verbose mode (not
expected: in the xnu tree only PE_create_console reads `-v`). A panic's text is in the mailbox and the USB capture; the panel may
stay dark.

## 7. Making it the default

After runs A and B (and C) have measured the saving: take `-v` and `droidvm-disable-jobs` out of `K27_ARGS` (build-ram-images.py;
build-esp.py's profiles follow, and `--fast-boot` stays for level 2). `-v` stays available for a bring-up image through
`PIANO_EXTRA_ARGS="-v"` or `build-esp.py --arg=-v`. Correct docs/bringup-debugging.md's panic-photo note at the same time. The
installed ESP keeps its arguments until it is rebuilt (`build-esp.py --profile daily [--fast-boot 1]`).

## 8. Open

- loginwindow's MobileGestalt service (§3.3); the logind → loginwindow delay (run D).
- The ~40-45 s kernel phase without the console's share (run B shows how much is left): the kexts' start (NativeUfs's stages,
  PianoDwc3's takeover) and DDR at the firmware's level until the GPU votes. An early bandwidth vote is driver work
  (PianoPower/PianoA830, not changed here).
- launchd's boot tasks: fips (cc_fips_test) and dirhelper take about 6 s each in text mode. They are part of the OS image.
- CPU frequency: XNU has no CPU DVFS natively, so the cores run at whatever the firmware left. Not measured.

## 9. The 27.3 dyld stretch: the native 4K code-signing pre-hash of the compressed shared cache (2026-11-14)

**Measured (bt1 18:20, ufs1 19:36, RAM boots of the installed line).** Between launchd's exec and `dyld cache mapped
system-wide` the root windows hand up ~2.4 GB, mostly 512 KiB reads (the cluster layer's SSD read-ahead size). With the old read
path (bt1) that took until iolog second 39; with `native-ufs-async=8 -bounce=1024 -maxio=1024 -nap=25` (ufs1) until second 36:
the device then delivers 1.5-1.9 GB/s while busy but is busy only 4-6 % of each second (`qd 0.1`). The disk is not the limit;
launchd is (confirmed on the tablet). The 2026-11-12 `pc-sample=30` boot (27.0.1 line) had launchd at 1955 kernel and 0 user
samples in that stretch, its hottest kernel pcs in corecrypto's SHA-256 block function (ARMv8 `sha256h`/`sha256h2`, located with
`tooling/native/pc-sample-symbolize.py`).

**What the kernel does (our source, xnu-12377 + k27, `XNU_NATIVE_CS_HASH_CACHE=1` in the k27 tree's `build-k27-piano.sh`):**
- The native kernel runs 4 KiB pages (`el1-native-plan.md`); Apple's arm64e signatures hash 16 KiB pages. `ubc_cs_blob_add`
  (`bsd/kern/ubc_subr.c:4753`) calls `cs_native4k_prepare` (`:4988`) before publishing a signature: `:5876-5970` read the whole
  signed range with `vn_rdwr` 16 KiB at a time (`:5913-5927`), SHA-256 each 16 KiB block against the signature, then SHA-256 each of
  its four 4 KiB pages again (`:5948`) into a table that the fault path's `cs_validate_hash` (`:6036-6062`) uses, one 4 KiB hash per
  page-in. Counters: `cs_native4k_full_blocks`, `_ready_blobs`, `_page_checks` (`:5864-5866`).
- dyld adds each shared-cache file's signature with `fcntl(F_ADDFILESIGS*)` (`bsd/kern/kern_descrip.c:4066-4069` -> `ubc_cs_blob_add` at `:4198`) before it maps them,
  so the whole cache is read and hashed twice in launchd's thread, one file after the other, before `dyld cache mapped`.
- The mapping and the slide are lazy: `vm_shared_region_auth_remap` (`osfmk/vm/vm_shared_region.c:1259`) maps a pager over the
  auth sections and pages are slid on fault (`vm_shared_region_pager.c`); `shared_region_map_and_slide_2_np`'s own checks
  (`bsd/vm/vm_unix.c:1634-1700`) read only the signature blob and the trust cache.
- **The cache is compressed.** All 87 `dyld_shared_cache_arm64e*` files of the 27.3 SystemOS are `UF_COMPRESSED`, decmpfs type
  14 (LZBITMAP, data in the resource fork): 6,919 MiB of file content in 2,444 MiB of resource forks
  (`tooling/k273/apfs-file-flags.py systemos.img dyld_shared_cache`; Apple's own SystemOS cryptex `141-22145-021.dmg` is the same,
  so this is how 27.3 ships, not our `make-systemos.sh`). Each `vn_rdwr` of a compressed file goes through
  `decmpfs_read_compressed` (`bsd/kern/decmpfs.c:1649`, the compressed data lock taken shared, `:1677`), which decompresses in the
  caller. So the stretch is: 2.4 GB read (the 512 KiB reads), 6.9 GB LZBITMAP-decompressed, 13.8 GB SHA-256 (two passes), in
  ~440,000 `vn_rdwr` calls of 16 KiB, on one CPU. At ~37 s that is ~187 MiB/s of file content, ~84 us per call.
- On a 16 KiB-page Mac none of this runs at map time: each page is validated (and decompressed) when it is first touched.

**Where exactly the ~84 us per 16 KiB goes** (decompression, the two hash passes, the per-call VFS/decmpfs overhead, waits) is the
one open number. Probe **ufs3** (`private/host-checks/20261114-ufs1/CHECKLIST-ufs3.txt`): ufs1's switches plus
`pc-sample=2 pc-sample-hz=500 sample-name=launchd sample-period=2` (the k27 profilers, `osfmk/kern/exception.c:273-771`: the CPU
split per task, the hottest kernel pcs, launchd's threads and their kernel stacks every 2 s) and NativeUfs's third perf line, the
`cs native4k` counters per second (16 KiB blocks pre-hashed and their MiB: compare with the iolog's MiB, x2.8 for the compression).
`tooling/native/pc-sample-symbolize.py LOG --kc out/k273-ufs3/kernel.macho --kernel <the 0066 kernel>
--natives out/k273-ufs3/kernel.macho.natives.json` names the pcs (kernel symbols, kext + offset, `[sha256 insns]`).

**Decision 2026-11-14 (owner): skip the eager pre-hash on this development tablet.** Switch `cs-native4k-skip=1` (k27 0067, default
0 = verification as before). Trade-off: the system boots without the ~37 s pre-hash, and in exchange the contents of the pages of
every file signed with 16 KiB pages (all of Apple's arm64e code, the dyld cache included) are no longer checked against their
signatures on this 4 KiB-page kernel; each signature's own checks (code directory, cdhash, trust cache) still run. To turn the
verification back on: remove `cs-native4k-skip=1` from the line (or set `cs-native4k-skip=0`); `cs-native4k-threads=6` keeps it on
and parallel instead. The boot says `code-signature pre-hash skipped (cs-native4k-skip=1): N files, M MiB not hashed (P 4 KiB pages
accepted without a hash so far)` once, 120 s after the first skip (`cs-native4k-skip-report=S`).

| Kernel argument (k27 0067) | Default | Effect |
|---|---|---|
| `cs-native4k-skip=1` | 0 | no pre-hash; pages of 16 KiB-page signatures accepted unhashed (above) |
| `cs-native4k-skip-report=S` | 120 | seconds from the first skip to the one summary line |
| `cs-native4k-threads=N` | 1 | the pre-hash on N participants (1..16, at most the CPUs); 1: the original loop |
| `cs-native4k-read-kib=K` | 0 | the parallel pass's read size (0: 1024 KiB); any value takes the parallel path |
| `cs-native4k-log=MiB` | 0 | one line per pre-hashed signature of at least MiB (`cs_native4k: NAME: ... in T ms, N threads ...`) |

ufs3 (2026-11-14 20:3x, the measurement probe) confirmed the attribution: `perf ... cs native4k` ~12,300 16 KiB blocks (~192 MiB
of decompressed content) a second on one CPU, 0 bad hashes, against ~80 MiB/s on the windows; `pc-sample`: launchd ~996 kernel
samples per 2 s (one CPU busy), its hottest pcs corecrypto's SHA-256 (~87 %), then the LZBITMAP decompressor and memmove.

**Fixes, in order:**
1. **Built: k27 0067, `cs-native4k-threads=N`** (the fallback that keeps verification on) (`patches/k27/0067-*`, `patches/k27/README.md`): `cs_native4k_prepare` on the
   caller and N-1 kernel threads, each claiming 1 MiB chunks (`cs-native4k-read-kib`) and reading each with one `vn_rdwr` (decmpfs
   serves concurrent readers of one file: its compressed-data lock is taken shared), verifying the chunk's 16 KiB blocks and
   filling their 4 KiB slots; the first error stops everyone and is returned; publication only after every helper finished without
   error, as before. Default 1: the original loop. `cs-native4k-log=MiB` logs each signature's time. The work split is host-tested
   with real threads (`tooling/k27/test-cs-native4k.sh`: the same table as one thread for 1-16 threads, every block verified once,
   bad blocks and read errors fail the pass; plain, ASan/UBSan, TSan). Inferred ~37 s -> ~7-10 s with 6 threads (decompression and
   hashing spread over the CPUs; the disk has 15x headroom). Probe ufs4 (`private/host-checks/20261114-ufs1/CHECKLIST-ufs4.txt`).
2. **Design note only: validate lazily, per 16 KiB block, at fault time** (what a 16 KiB-page kernel does per page). Today the 4 KiB
   table must exist before the signature is published because the fault path (`vm_fault_enter` -> `vm_page_validate_cs` ->
   `cs_validate_hash`) runs with the VM object locked and the page busy, where no vnode I/O is allowed. The lazy form:
   - publish the signature with no table; `cs_validate_hash` for a 4 KiB page P looks up P's three siblings of its 16 KiB block
     in the same VM object; when all are resident and not busy, map them, hash the 16 KiB against the signature and mark the block
     validated (a per-blob bitmap, written once, read without locks), then answer P from that;
   - make the vnode pager's page-ins 16 KiB aligned and at least 16 KiB (the cluster already is for sequential reads), so P's
     siblings arrive with P in one UPL and are resident when P is validated;
   - when a sibling is missing (reclaimed between page-in and validation), never taint P: return a retry to `vm_fault` (it already
     restarts faults after dropping its locks), read the 16 KiB block through `vn_rdwr` outside the object lock into a private
     buffer, verify it, set the block's bit, and retry the fault.
   It removes the whole eager pass (the ~37 s stretch, and the same pre-hash at every exec after launchd: every Apple binary's
   signature is authenticated when it is added): only touched blocks are decompressed and hashed, at their first fault. Risks:
   vm_fault's busy-page and lock ordering, the pageout race on siblings, a page modified through write() on a writable volume
   (the 16 KiB kernels' tainting rules apply per block). A larger change, after 0067's numbers.
3. Not an option without the owner: skipping the verification, or a cross-boot cache of derived hashes on the writable Data volume.

`native-ufs-fast` alone gave ~3 s (dyld mapped at iolog 36 vs 39, `Early boot complete` T 38.7 vs 41.8 s) and nothing after
launchd (loginwindow at iolog 70 vs 73: the same ~31 s after `Early boot complete`; the windows busy 5-30 % of each second there).

### 9.1 The ufs4 boots: measured milestones, the seconds saved, and what to install (2026-11-14; `private/host-checks/20261114-ufs1/SCORE-ufs4.md`)

Three single boots of the probes on the tablet — ufs3 (baseline), ufs4a (`cs-native4k-skip=1`), ufs4b
(`cs-native4k-threads=6 cs-native4k-log=1`). All figures below are quoted from the console captures
(`ufs3-console.out` / `ufs4a-console.out` / `ufs4b-console.out`); this batch's pstore mailboxes were bit-flipped (~81 % of
bytes majority-corrected in the run logs) and are not quoted. Times are uptime T; the EBC / exclaves / launchd `hello`
numbers are tick-CONFIRMED on all three boots, the rest are bracket-inferred at ±0.5 s (iolog ≈ T − 0.3±0.3).

| Milestone (uptime T s) | ufs3 | ufs4a skip | ufs4b threads=6 |
|---|---|---|---|
| dyld cache mapped | ≈37.2 | **≈1.5** | **≈8.7** |
| Early boot complete | 38.86 (tick) | 3.84 (tick) | 10.36 (tick) |
| WindowServer opens | ≈40.8 | ≈6.8 | ≈12.8 |
| first lit frame | ≈69.8 | ≈34.8 | ≈41.8 |
| loginwindow opens | ≈71.8 | ≈35.8 | ≈42.8 |
| Finder (desktop launch) | not measured | ≈85.8 | ≈95.8 |

Saving to loginwindow: **≈36 s with the skip, ≈29 s with 6 threads** (`Early boot complete` tick-confirmed on all three
boots). Nothing else moved before dyld's map — GPU bring-up ≈1.8 s on every boot, and EBC→loginwindow is 33.0 / 32.0 /
32.4 s across ufs3 / ufs4a / ufs4b: the same post-EBC launch wave every time, which is the next boot-time target
(structural reading, **inferred** from the three boots).

Rate facts: ufs3 hashed 455,833 16 KiB blocks at a mean **12,297 blocks/s on one CPU** (perf lines, whole boot); ufs4b
hashed 472,393 blocks at a mean **≈60,160 blocks/s over seconds 2–8 on 6 threads** (≈4.9× — attribution to the hash
threads is **inferred**), every `cs_native4k:` line ending `ok 0`, zero refused signatures; ufs4a's pre-hash Σ = 0 and its
4 KiB demand-page checks stayed 0 all boot (fact — pages were accepted unhashed; whether verification is deferred to
exec-time is **inferred**). ufs4b's burst read 6,608 MiB of content against the iolog's `2381 MiB so far` at second 8:
ratio ×2.8, the predicted compression (CONFIRMED); the flash was no longer the pacer there (inferred).

Code-signing failures seen: **none on any of the three boots** — `KERN_CODESIGN_ERROR`, `cs_invalid_page`, `CODE SIGNING`,
`Code Signature Invalid`, `taskgated`, `Killed: 9` (plus `AMFI denial` on the two 0067 boots): 0 hits each (fact, console
scans). The only real `FATAL` line is the same pre-existing locationd crash that is already in `ufs3-console.out` — not
introduced by either switch. Health on both 0067 boots: `NativeUfs: FAILED` 0 hits, `Data store state 0`, desk-login rc=0
with the Finder launch confirmed by its console line (post-login the webcam showed the clock/lock face ~40-45 s later on
both boots — same on both, so not introduced by either switch; fast auto-lock or desktop-not-drawn yet, **inferred**).

Recommendation, **inferred** (the score's): put `cs-native4k-threads=6` without the skip in the installed ESP's boot line
— ≈28.5 s saved to the login window while keeping full verification (472k blocks verified, every file `ok 0`); leave
`cs-native4k-log` off in the installed profile. `cs-native4k-skip=1` saves ~7 s more but is a verification-off window
(300,772 pages accepted without a hash, zero demand checks all boot) — keep it for dev/test RAM boots only, drop it after
any install. The two switches are orthogonal. And before arguing over the last 7 s between a and b, chase the ~32 s
post-EBC launch wave.
## 10. The halt/restart timeout: result of probe halt1 (2026-11-14, confirmed on the tablet)

This belongs here because every console `reset fastboot` pays for the halt bug: the restart hangs 30 s, panics, and the panic
reboot reaches ABL only seconds later (halt1: 159 s from the reset command to fastboot). On the installed system it can also
land in macOS instead of fastboot (§10.3).

### 10.1 What halt1 answered

Probe halt1 (k27 0068, boot-arg-gated `piano-halt-trace=1`, RAM boot 21:38:05, `reset fastboot` sent ~21:40:05) traced the
whole of `PEHaltRestartInternal()`. **Confirmed on the tablet: the 30 s are the platform's own `haltRestart`, not a driver or a
power-tree wait.** The sequence:

- IOKit's halt phases finish in **38 ms** (t=99650 → 99688 ms):
  - NVRAM sync returns at once — it fails (`e00002d8`: `serializeVariables` failed, no `_nvramController`) but takes 0 ms and
    does not block (inferred: expected without an NVRAM controller driver);
  - phase 'Notifying priority clients': **4 clients, each returned after 0 ms**;
  - phase 'Notifying power plane drivers': `notifySystemShutdown` returned after 34 ms;
  - phase 'Notifying HaltRestart action handlers': **0 registered** actions;
  - phase 'Quiescing PM': one quiesce request against an **empty work queue (0 services)**, retired at once; PM quiesce
    callback and PM quiesced at t=99687.
- Then `piano-halt: ArmDTPlatform::haltRestart(1) -> (PE_halt_restart NULL)` and `haltRestart returned -1` at t=99688 —
  **no restart was attempted**, and `IOPlatformExpert::haltRestart()` returns -1 when `PE_halt_restart` is NULL
  (`iokit/Kernel/IOPlatformExpert.cpp:339-368`). Nothing happened afterwards until the 30 s timer fired at t=129651
  (`piano-halt: timed out after 30 s`; the phase-named line `Restart timed out in phase 'Notifications completed'.
  Total 30000 ms @IOPMrootDomain.cpp:6289` survives only in the run log's corrected copy). The panic path called
  `haltRestart` again (-1) and then reached `piano-halt: PSCI SYSTEM_RESET (0x84000009) through smc` — the last marker, with
  no return line — and that one **did** take the reset: the tablet was back in ABL fastboot 159 s after the command
  (confirmed: `logs/native-runs/20261114-213805-loop/loop.log`). Inferred: the SMC reset wins the race with the return print.
- PianoWifi's halt watch at +8 s and +20 s still read `0 service(s) with the shutdown not acknowledged` and
  `0 of 15 power-plane service(s) with a power change in progress`: nothing was pending on the driver side while it hung.

Evidence: the clean copy of the marker sequence is the dump boot's console replay
`logs/usb-console/20261114-214008-port0.txt:15-83`, which cuts off at `haltRestart returned -1`; the tail (the 30 s timeout,
the second attempt, the PSCI line) is in `private/host-checks/20261114-halt/halt1-mailbox.txt:4480-4597` with single-character
bit flips (batch-wide), repaired, and in its majority-corrected copy `logs/native-runs/20261114-213805-loop/loop.log`. The
live capture `halt1-console.out` ended before the reset and holds no halt markers.

### 10.2 The same shape in 10p — the mystery it resolves

10p's clean console line (`logs/usb-console/20261114-211416-port0.txt:92`):
`PianoDwc3: reset: PEHaltRestart returned -1 after 242 ms: the shutdown notifications are done and the platform did not
restart`, then the bare `Halt/Restart Timed Out @IOPlatformExpert.cpp:884`. Same -1, same "platform did not restart", same
30 s timeout and panic reboot to fastboot — only the notifications' share differed (242 ms vs halt1's 38 ms). 10p could only
place the blocker downstream of the notifications; halt1 pins it to `ArmDTPlatform::haltRestart` (the class method) answering
-1 without trying, with `PE_halt_restart` NULL. 10p's hypothesis: **confirmed on the tablet.**

### 10.3 The guard race during the hang (inferred)

During a hang of this kind on the **installed** boot, the boot guard's 180 s health check can still run (the kernel is alive)
and reset the restart reason back to normal, so the eventual panic reboot lands in macOS, not ABL fastboot. Inferred from the
console: the 2026-11-14 20:39:50 `reset fastboot` on installed boot #1 reached the halt hang, and `piano-boot-guard: SDAM
restart reason 0x04 -> 0x00 ... healthy after 180 s, restart reason normal` printed after the shutdown handoff lines
(`logs/usb-console/20261114-203949-port0.txt:4915-4917`); the follow-on boot's launchd at 20:41:31
(`logs/usb-console/20261114-210211-port0.txt:1292-1296`) shows it booted the installed system instead. Full analysis:
`private/host-checks/20261114-a2run/WHY.md`. Practical rule (already in `boot-guard.md`): on the installed boot, send
`reset fastboot` only after the guard has printed `healthy after N s`.

### 10.4 Fix direction

k27 0069 `piano-psci-halt`: give `PE_halt_restart` a real handler that performs PSCI `SYSTEM_RESET` itself (logging the
reason), so `haltRestart` resets instead of answering -1. Target: `reset fastboot` reaches ABL fastboot without the 30 s
hang and panic, which also removes the guard race of §10.3.
## 11. The second half after the skip: launchd to the desktop (2026-11-14)

With `cs-native4k-skip=1` the kernel part is done at 3.8 s (`Early boot complete`, confirmed tick in ufs4a); what is left is
userland. Sources: ufs4a's console (`private/host-checks/20261114-ufs1/ufs4a-console.out`, RAM boot, `data-persist=0`), ufs3's
`pc-sample=2` boot (the same RAM userland with the pre-hash, its CPU split per task every 2 s), the installed boots of
`private/host-checks/20261114-flash/desktop-time.md` (v4b6, v5b3: persistent Data).

**Timeline, uptime in s** (ufs4a; the GUI lines bracketed by the per-second iolog, +-0.5 s; launchd lines carry their wall clock):

| Milestone | ufs4a (RAM, skip) | ufs3 (RAM, pre-hash) | v5b3 (installed, pre-hash) |
|---|---|---|---|
| launchd `hello` | 1.7 | 37.4 | 40.0 |
| `Early boot complete` | 3.8 | 38.9 | 41.7 |
| Installer Progress launched (AMFI line) | 3 | 38 | right after it (no per-second marker there) |
| WindowServer opens the GPU | 6.8 | 40.5 | 43.0 |
| ViewBridgeAuxiliary launched | 28 | 64 | ~49 |
| Installer Progress opens the GPU | 33.5 | 69.5 | 49.4 |
| first frame (black), SecurityAgent, loginwindow | 34.5-35.8 | 69.5-71.5 | 49.6-55.9 |
| first lit frame (login window) | <= 44.5 (the first period line after the first frame says lit) | ~69.8 | 65.6 |
| password typed (desk-login: lit frame + 10 s settle + polling) | ~60.8 | — | 76.6-83.4 |
| Finder opens the GPU (the desktop) | 85.8 (25 s after the password) | — | 92.4 (~9 s after) |

**Where it goes, ranked** (confirmed = read from those lines; inferred = the attribution):

1. **The clocks (inferred to be the largest lever, every boot).** No profile sets `piano-cpufreq`, so the clusters stay where the
   boot stages left them: PianoThermal's clock line (30/60/90 s of ufs4a, confirmed) reads cpu0-5 at ~1996 MHz (of 3532.8) and
   cpu6-7, the ADT's P cores where the scheduler puts foreground work, at ~1016 MHz (of 4320; sustained 4089.6). ufs3's pc-sample
   shows the second half as single threads at a core each (lsd, ReportCrashService, audiomxd ~0.8, configd ~0.5) with 40-55 % of
   the samples idle: CPU-bound on slow cores, not waiting on the disk (NativeUfs busy 5-30 % of each second after 15 s, queue depth
   0.0-0.3, ~150 us a command). Fix: **`piano-cpufreq-boost=S`** (PianoThermal, 2026-11-14, default 0): every cluster at its
   sustained level until S s of uptime, PianoThermal's 90 C cluster ceiling on top in every mode, then the profile's own level
   (without `piano-cpufreq`: the boot stages' level again). Host-tested (`thermal_host` testBoost). Probe ufs5.
2. **The LaunchServices database, on RAM boots only (confirmed timing, inferred cause).** In ufs3, lsd runs at one full core from
   44 to 66 s (pc-sample) and Installer Progress opens the GPU at 69 s, ~31 s after its launch; ufs4a the same 30 s (3 -> 33 s); on
   the installed boots 7.7 s. A RAM test boot starts from the seed's Data volume, which has no LaunchServices database (and no
   font registry, icon cache, ...): lsd rebuilds it every boot, and an AppKit launch waits for lsd. The installed system keeps it in
   its store. Fix candidates, not done: a seed made after one desktop boot (the caches copied from a running RAM boot, e.g. over
   ssh, into the seed: a new seed identity, the RAM images' only), or a test-only persistent store window. Every RAM desktop test
   would gain ~20-25 s.
3. **locationd's crash, on RAM boots (confirmed, cause open).** `ERROR: [locationd:78] FATAL exception: type=0x1 reason=0x1c
   code=0x105` at ~8 s of ufs4a and ~45 s of ufs3, not on the installed boots; ReportCrashService then runs at one core for ~20 s
   (ufs3 pc-sample; ufs4a: two instances, 120 MB high-water marks, killed at 28.5 s). Competes with lsd on the slow cores.
4. **audiomxd and configd, every boot (confirmed in ufs3's pc-sample, `power.md` §1.2):** ~0.8 and ~0.5 core for the whole boot and
   after it (no CoreAudio output device). Power more than boot time; `power.md`'s fallback (the jobs off) stands.
5. **password -> Finder: 25 s on the RAM boot, ~9 s installed** (the user's first-login caches on the seed; iconservicesagent and
   others at a core each, ufs3 76 s). Item 2's seed would cover it.
6. **SecurityAgent -> visible login window**: <= 10 s here, 10-16 s installed (desktop-time.md F1). NativeGopDisplay's whole-frame
   copy costs ~21.5 ms, 17.5 ms of it reading the surface uncached (ufs4a: `read 17456 us`). Copy mode bit 1 (cacheable reads,
   DC CIVAC first) exists but never ran on the tablet: **`native-gop-display-copy-ab=1`** (2026-11-14) runs both on one boot, one
   report period each, and the period line gives each mode's read and copy means.

**Markers added for the next boots** (2026-11-14): PianoA830's `X opens the accelerator, type T: R (at N ms)` (every GUI process's
GPU open with its uptime), NativeGopDisplay's `first frame (swap N) ... (at N ms)` and `first lit frame: frame N, P% non-black,
mean #RRGGBB, at N ms (N ms after the first frame)` (once, at the first copied frame at least half lit).

**ufs5** (`private/host-checks/20261114-ufs1/CHECKLIST-ufs5.txt`): ufs4a's switches + `piano-cpufreq-boost=120` + `pc-sample=2
pc-sample-hz=500` + `native-gop-display-copy-ab=1`, kernel 0070 (0069's restart path, 0070's M4 hardening). The control is ufs4a
(the same image line without the three); `ufs5b` (the same without the boost) separates the boost from the rest if needed.

### 10.1 A warm RAM test seed (tooling/k27/warm-seed.py, 2026-11-14)

Item 2 above, built: the RAM images' seed (seed-200, identity `9bd6f902`) has an empty `private/var/folders`, no `private/var/db/lsd`,
an empty `Library/Caches` and a home whose `Library` holds only Compositions, Favorites, FontCollections, Input Methods and Preferences
(`warm-seed.py inspect`, confirmed on the host): every RAM boot rebuilds LaunchServices' databases and every first login the home's
caches. The installed system keeps them in its store and needs none of this.

- **collect** (one desktop RAM boot, the USB network up, the Finder open and a minute idle): `tooling/k27/warm-seed.py collect --out
  out/warm-k273-N` runs, as root on the tablet (`sudo -S` with the debug account's password from `private/login.secret` on stdin;
  never in argv), an xz tar with numeric owners of `private/var/folders` (without the temporary `T` folders), `private/var/db/lsd`,
  `Library/Caches` and the homes' `Library` (without Keychains and Logs), streamed over `tooling/native/desk-ssh.sh` to
  `warm.tar.xz`; `collect.json` lists the largest parts.
- **build**: `tooling/k27/warm-seed.py build --pack out/warm-k273-N/warm.tar.xz --out out/warm-k273-N/seed [--include GLOB]... [--all]`:
  a copy of the seed (default `/Volumes/ExtSSD/k273/v2/seed-200.img`, trust cache the v4 layer's) with the pack, filtered (default:
  LaunchServices' databases, lsd's, the font registry and the icon store; every parent directory and AppleDouble entry of a kept
  member kept, so each folder keeps the tablet's owner and mode), at `private/var/piano/warm/warm.tar.xz`, and a root-owned
  `private/etc/rc.server`: launchd's `rc.server` boot task (its log line `(rc.server) Doing boot task`, then `optional boot task not
  present` today) runs it as root before any daemon; it extracts the pack (`tar -x -p --numeric-owner`) and prints
  `warm-seed: the caches of pack P extracted before the daemons (tar rc N)` on the console. A pack whose system directories are not
  root's (made anywhere but `collect`) is refused. The seed is mounted by hdiutil alone with owners honoured (`diskutil mount`
  hangs on these images here some days, as `stage.py` found); `build` needs `sudo -n` on the build Mac for its two root-owned files.
- The output seed is a **new identity**: `build-ram-images.py` gives its images `data-persist=0` (Data volatile, the tablet's store
  untouched); `out/warm-seeds.json` registers it and `verify-ram-images.py` fails an image of it with any other mode.
- **Budget**: `build-ram-images.py` refuses images over 250 MB (`IMAGE_MAX_BYTES`); today's images are 237.3 MB, so the pack has
  ~12 MB (xz-compressed). `stage.json`'s `grew_bytes` is the seed's growth; narrow `--include` if it does not fit.
- **Regenerate** after a new seed, a new userland layer or a system update: collect on a desktop boot of the new image, build again.
- Dry run on the Mac (no tablet content): a tar of a dummy tree, `build --test-pack` (skips the owner check), then
  `build-ram-images.py --seed <out>/scratch-data.img --trust <out>/kernel.macho.trustcache ...`, then `verify-ram-images.py`; remove
  the dummy's entry from `out/warm-seeds.json` afterwards. Host test: `tooling/k27/test-warm-seed.py`.
- Open (the first warm boot answers both): that launchd runs `rc.server` (the console line, before `Early boot complete`), and that
  lsd keeps the collected database (Installer Progress's `opens the accelerator (at N ms)` near the installed boots' ~8 s after
  `Early boot complete`, lsd's pc-sample share small).
