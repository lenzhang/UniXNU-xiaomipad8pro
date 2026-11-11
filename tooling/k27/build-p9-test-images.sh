#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The idle-load test images of docs/power.md 1.2 and 6.5 (2026-11-12: P11 of that night's queue, PerfPowerServices and audiomxd
# spinning a core in every boot, and P9, the governor that followed them to 3532 MHz): RAM images of pm-idle1's profile (the GPU
# profile, PianoPower's battery lines, the USB console, piano-cpuidle=1 for the per-CPU idle residency lines) at the idle login
# window for 900 s, all with the same diagnostics, so each differs from the baseline by one change:
#
#   base  the reference: today's userland, both daemons running
#   off   com.apple.PerfPowerServices and com.apple.audiomxd disabled in the seed's launchd overrides (stage-boot-run.py
#         --disable-job): the fallback fix; the A/B partner of base
#   rtc   base with the calendar from the PMIC RTC from the start (piano-rtc=1 piano-rtc-offset=$P9_RTC_OFFSET, k27 0060), so no
#         daemon sees settime's jump from 1970 to droidvm-utc: the test of the clock-jump hypothesis for PerfPowerServices
#   deny  base with both executables denied (PIANO_DENY_EXEC, k27-deny-exec): the argument-only disable an ESP can take
#   gov   off with PianoThermal's governor (piano-thermal=1 piano-cpufreq=3, its 2026-11-12 fixes): the governor at an idle login
#         window without the spinners
#   diag  base with iokit-log and sc-trace for both daemons (600 s): what they look up and which calls fail; may flood the console
#         if a spin is made of such calls, so it is not a measurement image
#
#   build-p9-test-images.sh [NAME ...]   default: base off; out/ram-p9-<NAME>/ and out/ram-p9-dump/
#
# Every image's seed has tooling/k27/cpu-reports.sh in its boot job (stage-boot-run.py): it prints the jobs and overrides of both
# daemons, the busiest processes every 5 minutes, and the CPU resource reports (spindump, symbolicated on the tablet) written for
# them, to the console. Diagnostics on the command line: pc-sample=30 (the busiest tasks per 30 s, from the existing timer interrupts:
# no extra wakeups), exec-log=1 (a denied spawn shows as `exit: pid N xpcproxy status 0x4e00`), sample-name for both daemons every
# 300 s (each thread's state, pc and frames).
#
# Inputs (this repository's out/ unless set): P9_KC (out/k27/kc-k27-native.bin, a kernel with 0061: build-native-image.sh with
# K27_KERNEL=<kernel-k27-0064-native.macho> PIANO_GPU=2 PIANO_VMM_PRESENT=0), P9_ADT, P9_FD, P9_SEED / P9_TRUST (the Turnip stage,
# out/turnip-a830/stage: the images then boot to the A830 login window as pm-idle1 did), P9_RTC_OFFSET (rtc only:
# tooling/native/rtc-offset.py), P9_EXTRA (added to every image). The staged seeds go to out/p9-stage/{report,off}.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${P9_KC:-out/k27/kc-k27-native.bin}
ADT=${P9_ADT:-out/adt/piano-k27.adt}
FD=${P9_FD:-out/fd/piano-uefi-A-bundled.fd}
SEED_IN=${P9_SEED:-out/turnip-a830/stage/scratch-data.img}
TRUST_IN=${P9_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
for f in "$KC" "$ADT" "$FD" "$SEED_IN" "$TRUST_IN"; do [ -f "$f" ] || { echo "build-p9-test-images: $f is missing" >&2; exit 1; }; done
grep -a -q piano_cpuidle_enter "$KC" || { echo "build-p9-test-images: $KC has no piano_cpuidle_enter: its kernel lacks k27 0061" >&2; exit 1; }
# The two jobs (their labels and executables as on macOS 26.6's /System/Library/LaunchDaemons; cpu-reports.sh's job/override lines
# and the reports' Path lines show 27.0.1's own) and the reporter's pattern.
JOBS="com.apple.PerfPowerServices com.apple.audiomxd"
EXECS="/usr/libexec/PerfPowerServices,/usr/libexec/audiomxd"
PATTERN='PerfPower|audiomxd'
# pm-idle1's profile (build-pm-test-images.sh BASE + idle1) and the diagnostics, the same in every image
BASE="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-power=1 piano-power-period=10 piano-usb=2 piano-input=1"
BASE="$BASE piano-cpuidle=1 pc-sample=30 exec-log=1 sample-name=PerfPowerServices,audiomxd sample-period=300 ${P9_EXTRA:-}"
ORDER="base off"
ALL="base off rtc deny gov diag"

stage() {   # stage NAME [--disable-job LABEL ...]: out/p9-stage/NAME from the input seed, with the reporter in its boot job
  local name=$1
  shift
  python3 tooling/k27/stage-boot-run.py --seed "$SEED_IN" --trust "$TRUST_IN" --out "out/p9-stage/$name" \
    --file tooling/k27/cpu-reports.sh:private/var/piano/cpu-reports.sh \
    --boot-run "sh /private/var/piano/cpu-reports.sh '$PATTERN' 16" "$@" > "out/p9-stage/$name.log" 2>&1 \
    || { tail -5 "out/p9-stage/$name.log" >&2; exit 1; }
  tail -1 "out/p9-stage/$name.log"
}
mkdir -p out/p9-stage
NAMES=${*:-$ORDER}
need_report=0 need_off=0
for n in $NAMES; do
  case " $ALL " in *" $n "*) ;; *) echo "build-p9-test-images: no image $n (one of: $ALL)" >&2; exit 2 ;; esac
  case $n in off|gov) need_off=1 ;; *) need_report=1 ;; esac
done
[ "$need_report" = 0 ] || stage report
if [ "$need_off" = 1 ]; then
  OFF=()
  for j in $JOBS; do OFF+=(--disable-job "$j"); done
  stage off "${OFF[@]}"
fi

for n in $NAMES; do
  SEED=out/p9-stage/report R=900 A= DENY=
  case $n in
    base) ;;
    off)  SEED=out/p9-stage/off ;;
    rtc)  [ -n "${P9_RTC_OFFSET:-}" ] || { echo "build-p9-test-images: rtc needs P9_RTC_OFFSET (tooling/native/rtc-offset.py): skipped" >&2; continue; }
          grep -a -q "piano-rtc-offset" "$KC" || { echo "build-p9-test-images: rtc needs k27 0060 in $KC's kernel" >&2; exit 1; }
          A="piano-rtc=1 piano-rtc-offset=$P9_RTC_OFFSET" ;;
    deny) DENY=$EXECS ;;
    gov)  SEED=out/p9-stage/off A="piano-thermal=1 piano-cpufreq=3"
          grep -a -q "left out of busy" "$KC" || { echo "build-p9-test-images: gov needs PianoThermal's 2026-11-12 governor in $KC" >&2; exit 1; } ;;
    diag) A="iokit-log=PerfPowerService,audiomxd sc-trace=PerfPowerService,audiomxd" R=600 ;;
  esac
  COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$SEED/kernel.macho.trustcache" --seed "$SEED/scratch-data.img" --adt "$ADT"
          --utc "$(date +%s)")
  PIANO_FAST_BOOT=1 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=$R PIANO_DENY_EXEC=$DENY PIANO_EXTRA_ARGS="$BASE $A" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-p9-$n" | tail -1
done
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST_IN" --seed "$SEED_IN" --adt "$ADT" --utc "$(date +%s)")
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-p9-dump \
  | tail -1
shasum -a 256 out/ram-p9-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
