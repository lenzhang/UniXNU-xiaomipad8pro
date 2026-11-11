# RTC: macOS's calendar from the PMIC clock (k27 0060, PianoPower's readout)

Status 2026-11-11: built offline and host-tested; the kernel (K27_VARIANT=native, clean objects) and a KC with PianoPower link,
`check-native-kexts.py` passes; a RAM test image is built. **Not run on the tablet.** Inert without `piano-rtc=1` (kernel) and
`piano-power=N` (readout).

## 0. The answer

- **Today** the native kernel has no clock: the calendar starts at 1970 + uptime, and the OS image's `settime.sh` boot job sets it to
  `droidvm-utc`, the time the RAM image or ESP was built. So every boot of the installed system starts at the ESP's build time.
- **piano's RTC** is the PMK8550's seconds counter (SID 0, `rtc@6100`), running on the battery. Every stack reads it the same way:
  UTC = count + an offset kept elsewhere; nobody sets the count. We read it only (no PMIC write anywhere in this design).
- **Design: (a), a kernel patch.** k27 0060 makes `PEGetUTCTimeOfDay` read the counter when the platform answers 0 and
  `piano-rtc=1`; `clock_initialize_calendar` (bsd_init, before launchd) then starts the calendar at count + offset. The offset is
  `piano-rtc-offset=N`, measured once against the Mac's clock, else the SDAM cell Linux keeps it in. `settime.sh` then finds a set
  clock and does nothing; `droidvm-utc` stays as the fallback for a boot where the RTC gives no date.
- **Readout:** PianoPower (level 1 is in the installed `daily` profile) logs `rtc: COUNT (DATE UTC, offset N from SOURCE), enabled E`
  with every report from the first one after IOBSD is published (so never at the same time as the kernel's read), with the
  current kernel too. Those lines plus the Mac's arrival times give N
  (`tooling/native/rtc-offset.py`).

## 1. Purpose and gate

A wall clock that survives reboots (the login window showed 7:55 for hours). No milestone depends on it, but TLS, Keychain, file
times and logs all want a real date, and there is no network time natively yet.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| RTC | `pmic@0` (SID 0, the PMK8550: `pmk8550_xo_therm` is on it) `rtc@6100`, `qcom,pmk8350-rtc`, `reg = <0x6100 0x6200>` ("rtc", "alarm"), alarm interrupt SID 0 periph 0x62 | `out/inputs/piano-boot.dtb` (Ubuntu's DT) | verified (DT) |
| Writable? | no `allow-set-time`: the count is not set, the date is count + the nvmem cell `offset` | same | verified (DT) |
| Offset cell | `nvram@7100` (`qcom,spmi-sdam`) `rtc-offset@72`, 4 bytes; the same SDAM holds `reboot-reason@48` (k27 0057 writes that byte) | same | verified (DT) |
| RTC_RW registers | +0x46 EN_CTL (bit 7 = running), +0x48..+0x4b RDATA (count, LE), +0x40 WDATA (never used here); ALARM at 0x6200 | Linux `rtc-pm8xxx.c` pmk8350 register set | inferred (source), needs-boot |
| Consistent read | 4 bytes, then the low byte again; smaller than the first means a carry ran through: read the 4 again | `rtc-pm8xxx.c:pm8xxx_rtc_read_raw` | inferred (source) |
| Date | count + offset modulo 2^32 (the driver adds them as u32) | `rtc-pm8xxx.c:pm8xxx_rtc_read_time` | inferred (source) |
| Android's offset | `persist/time/ats_2`, 8 bytes LE milliseconds; UTC = count + ats_2 / 1000 (stock `time_daemon`, measured on piano) | `unixnu-research/Project-SunUEFI/docs/devel/hardware/sleep.md` | verified (theirs) |
| SDAM cell content | Ubuntu v0.8.0: "the offset is stored in the PMIC ... correct across reboots and Android"; later Ubuntu docs: "the RTC cannot be written", `fake-hwclock` + chrony | `xiaomipad-8pro-mainline/packaging/release/changes/v0.8.0.md`, `docs/AGENT-GUIDE.en.md`, `.../50-piano-no-rtc.conf` | contradictory: needs-boot |
| SPMI | arbiter v7 (0x70020000), 896 APIDs, observer channels at 0x0C440000 + 0x20 * APID | PianoPower level 1 log, 2026-11-11 | verified (boot) |
| RTC/SDAM APIDs | SDAM 0x071 has an EE 0 APID (0057 needs and uses it); RTC_RW 0x061 not yet seen | 0057 runs; PianoPower's new line | needs-boot |
| No IORTC wait | `IOKitInitializeTime` waits for IORTC only `#ifndef BCM2837`; our machine config is BCM2837 (`IOStartIOKit.cpo` has no `waitForService`) | k27 tree and objects | verified (object) |
| No platform clock | `ArmDTPlatform` (an `IODTPlatformExpert`) does not override `getGMTTimeOfDay`, the base answers 0; AppleARMPlatform (whose PMU RTC Apple uses) is not in the KC | `macOS-DroidVM/out/k27/src/platform/ArmDTPlatform`, `configs/k27-native-kexts.txt` | verified (source) |
| Today's calendar | launchd stamps `1970-01-01 ...` in native runs | `logs/native-runs/*/xnu-console.txt` | verified (boot) |

## 3. Ubuntu as the specification

- Kernel `drivers/rtc/rtc-pm8xxx.c` (read as the specification only): probe takes `allow-set-time` and the nvmem cell "offset"; reads
  are count + offset; without `allow-set-time` a set computes offset = new time - count and writes the cell (SDAM, through
  `nvmem/qcom-spmi-sdam.c`); alarms through ALARM_RW. We use only the read path, never the set.
- `drivers/spmi/spmi-pmic-arb.c` v7: reads through the EE's observer channel (command = opcode 1 << 27 | register << 4 | bytes - 1,
  status bit 0 done, bits 1-3 errors), writes only through the owner EE's channel. Same facts as `package/PianoPower/SpmiObserver.h`.
- Userspace: chrony with `nocerttimecheck` "for computers without an RTC" and `fake-hwclock`; so the cell may hold an offset from
  v0.8-v1.0, zero, or an older value. The readout shows which (§8 step 3).
- Project-SunUEFI (BSD) hands Android's `ats_2` to Linux as a read-only nvmem cell only when the DT has no `nvmem-cells`: Ubuntu's DT
  has one, so on our boots the cell is the SDAM's.

## 4. What it builds on

No UniXNU component has a piano/SM8750 RTC (UniXNU-sheng, ArmDTPlatform: none). The SPMI access is k27 0057's (map core and cnfg,
APID by PPID with the EE 0 owner preferred, observer read) and PianoPower's `SpmiObserver.h`. The arithmetic is one plain C header,
`package/PianoPower/PmicRtc.h`, which 0060 adds unchanged as `osfmk/arm64/piano_rtc.h` (`build-native-piano-power.sh` checks it).

## 5. Where the calendar is set: options

| Option | Verdict |
|---|---|
| (a) kernel: 0060 in `PEGetUTCTimeOfDay` | **chosen.** Runs in `clock_initialize_calendar` at bsd_init, before any user process: boot time, file times and every log are right from the start; no OS image change. Also answers wake-time calls (no sleep natively yet) |
| (b) a kext publishing IORTC / a platform function | does nothing here: the IORTC wait is compiled out (BCM2837) and the platform expert never asks an RTC; it would need ArmDTPlatform changed (outside the k27 tree) or a kext setting the calendar after launchd started (no KPI, a jump) |
| (c) `settime.sh` from an ioreg property | changes the OS image, sets the time late (a jump after launchd), and needs the readout kext anyway |

`settime.sh` (`droidvm-build/piano-patches/settime.sh`) sets `droidvm-utc` only while `date +%s` is below 100000000; with the
calendar from the RTC it exits, so `build-esp.py` keeps `droidvm-utc` as the fallback. Without `droidvm-utc` it only sets the
time zone. `PESetUTCTimeOfDay` stays the platform's no-op: a time set in macOS lasts until the reboot, the RTC is never written.

## 6. Boot arguments

| Argument | Default | Effect |
|---|---|---|
| `piano-rtc=1` | absent: the kernel never touches the RTC | k27 0060: the calendar starts at count + offset when that is a date in [2026, 2100) and EN_CTL bit 7 is set |
| `piano-rtc-offset=N` | absent: the SDAM cell 0x72 | the offset (UTC - count, mod 2^32), for the kernel and PianoPower's readout |
| `piano-power=1` | 0 | PianoPower level 1, which logs the RTC lines |
| `piano-power-period=S` | 30 | report (and RTC line) interval; 5 for the measurement |

## 7. Components

`patches/k27/0060-*.patch` (apply in the k27 tree with `git am`, then `K27_VARIANT=native build-k27-piano.sh`);
`package/PianoPower` (`PmicRtc.h`, `RtcReadout.h`, `readRtcLine`); `tooling/native/usb-console.py` (every line also in
`<time>-port<N>.times.txt` behind the Mac's arrival time); `tooling/native/rtc-offset.py`; `tooling/persist/build-esp.py --rtc-offset`.
Host tests: `tooling/k27/build-native-piano-power.sh` (dates against `gmtime_r` over the whole 32-bit range, the carry re-read, the
arbiter model, no write but observer commands) and `python3 tooling/native/test_rtc_offset.py`.

## 8. Hardware test plan (USB-C cable to the Mac; nothing written to the PMIC; a RAM boot, back in ABL after 300 s)

Built offline 2026-11-11 (rebuild in the main checkout before a run; kernel sha256 `c4c55f1a589c82a0…`, KC `b95af75c161aeebc…`,
RAM image `d4815251a89fcb77…`, verify-ram-images PASS):

```sh
# the kernel: in droidvm-build/macOS-DroidVM/out/k27/xnu: git am <this repo>/patches/k27/0060-*.patch, then
K27_VARIANT=native ../droidvm-build/macOS-DroidVM/build-k27-piano.sh   # copy .../mach.development.bcm2837.unstripped to kernel-k27-0060-native.macho
K27_KERNEL=$K/out/k27/kernel-k27-0060-native.macho tooling/k27/build-native-image.sh
PIANO_EXTRA_ARGS="piano-usb=2 piano-power=1 piano-power-period=5 piano-rtc=1" PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300 \
  python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --out out/ram-rtc \
  --kc out/k27/kc-k27-native.bin --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt
```

1. The Mac's clock on network time. `tooling/native/usb-console.py --capture 280` in one terminal, then
   `fastboot boot out/ram-rtc/boot-piano-macos-ram-A-bundled.img`.
2. Expected on port 0 (the kernel's lines come with the replayed boot log):
   ```
   piano-rtc: RTC_RW APID <a> (owner EE <e>), EN_CTL 0x80, offset <cell> from the SDAM cell
   piano-rtc: count <C> + offset <cell> = <U>, 2026-11-.. ..:..:.. UTC: the calendar starts there
         (or: ... = <U> (year <y>) is not a date: the calendar is not set from the RTC   -> settime.sh's droidvm-utc as today)
   PianoPower: [<s>s] rtc: <C> (2026-11-.. ..:..:.. UTC, offset <cell> from the SDAM cell), enabled 1   (the first report after IOBSD)
   PianoPower: rtc: RTC_RW APID <a> (owner EE <e>) type <t> subtype <s> EN_CTL 0x80; SDAM APID <b> (owner EE 0), offset cell reads <cell> (0x...)
   PianoPower: [<s+5>s] rtc: <C+5> (...), enabled 1     every 5 s
   ```
   Pass: the count grows 5 per line, `enabled 1`. `rtc: not read (RTC_RW (PPID 0x061) is not in the arbiter's map)` or
   `EN_CTL read failed` means the RTC is not reachable from EE 0 (open question 1); `enabled 0` means the counter is stopped.
3. `python3 tooling/native/rtc-offset.py` (newest capture): prints `piano-rtc-offset=N`, the spread, and how far the SDAM cell is
   from the measurement (`last line: ... the kext used offset <cell> from the SDAM cell (...): +X s`).
4. The installed system: a KC on the 0060 kernel (`PIANO_VMM_PRESENT=0 K27_KERNEL=$K/out/k27/kernel-k27-0060-native.macho
   tooling/k27/build-native-image.sh`, `$K` = `droidvm-build/macOS-DroidVM`; the kext list has PianoA830, PianoPower and PianoDwc3),
   then `tooling/persist/build-esp.py --profile daily --rtc-offset N --default macos` (it refuses a KC without 0060), installed as in
   `docs/persistent-boot.md`. `--rtc-offset sdam` instead only if step 3 showed the cell within 1-2 s. Expected:
   `piano-rtc: count ... the calendar starts there`, launchd stamps of today, the login window's clock right, and right again after a
   reboot some minutes later (`unixnu.toml` says `clock: the PMIC RTC + offset N (k27 0060 piano-rtc=1), else droidvm-utc ...`).

## 9. Fallback

No `piano-rtc`: today's behaviour. The RTC gives no date (stopped, unreadable, sum outside 2026-2100): the kernel says why once and
leaves the calendar at 1970, and `settime.sh` sets `droidvm-utc` as today.

## 10. How the offset lasts

N is valid as long as the count is neither reset nor written. Nothing here writes it; Android (`ats_2`) and Ubuntu (the SDAM cell)
keep their own offsets and do not set the count either (read-only DT, `time_daemon`), so switching OS does not disturb N, and
measuring N does not disturb theirs. The counter runs from the battery (inferred: that is how all three stacks keep time across
power-off). A fully drained or disconnected battery stops or restarts it: then EN_CTL reads 0 or the sum falls before 2026, the
kernel refuses it and `droidvm-utc` takes over; the readout shows it, and N is measured again (step 1-3). Drift is the 32 kHz
crystal's (typically some tens of ppm, a minute or two a month at worst; inferred): re-measure when the clock is visibly off, since
macOS has no network time here yet.

## 11. Open questions

1. Is RTC_RW (PPID 0x061) in the arbiter map with an observer channel for EE 0, and what are its TYPE/SUBTYPE? (needs-boot, step 2)
2. What does the SDAM cell hold: Ubuntu's good offset, zero, or stale? (needs-boot, step 3; decides `sdam` vs N)
3. The drift over days. (needs-boot: two measurements a week apart)
4. Could the offset come from Android's `ats_2` instead (the maintenance image reading `persist` read-only, `ats_2 / 1000` as N)?
   Saves the measurement when Android last synced; not built.

## 12. Licence note

All code here is new (BSD-2-Clause-Patent). Linux's `rtc-pm8xxx.c`, `qcom-spmi-sdam.c` and `spmi-pmic-arb.c` (GPL-2.0) were read as
the specification only; nothing is copied. Project-SunUEFI (BSD-2-Clause-Patent) is cited for its `ats_2` measurement only.
