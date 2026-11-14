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
