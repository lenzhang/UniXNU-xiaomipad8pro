#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The desktop images (docs/attended-desktop.md, 2026-11-12): RAM images for using the macOS desktop on the tablet, from this
# checkout's kernel collection, ADT and Turnip stage, each a profile with every input driver and nothing that slows the session:
#
#   desk   the usable desktop: the A830 GPU, PianoPower level 1, the USB console with the Mac's keyboard and pointer, the keyboard
#          cover (keys, touchpad, three-finger swipes), the touch screen, the backlight with the brightness keys; no verbose console
#          (PIANO_FAST_BOOT=1: the boot screen draws no kernel text), no exec-log, no stderr-log. Its seed (a copy of the Turnip
#          stage's) has:
#          - the launchd overrides of the jobs that only loop natively (disabled: launchd never loads them, so none runs or
#            advertises its Mach services; a client gets an invalid connection at once):
#            com.apple.watchdogd            SIGTRAP at every start (no IOWatchdog natively), relaunched every 10 s, each time a
#                                           ReportCrash run: 15-21 crashes a boot (attend1-4, 2026-11-11/12)
#            com.apple.mobileactivationd    exec denied by the kernel (k27-deny-exec's default), so launchd's spawn fails (xpcproxy
#                                           exit 78) every 10 s for as long as a client keeps its request queued
#            com.apple.security.xprotectd   KeepAlive, exits 1 at once at every start: one spawn every 10 s (25 a boot)
#            com.apple.PerfPowerServices    powerlog: 1.7 cores of user time at the idle login window in every boot
#                                           (docs/power.md 1.2, fast1's pc-sample)
#            watchdogd and PerfPowerServices are also denied by k27-deny-exec, in case 27.0.1 names a job differently (then the
#            job's spawn fails every 10 s instead of crashing or spinning);
#          - the keyboard type of our two HID keyboards (the cover, vendor 0x1af4 product 0x504b; the Mac's keyboard through
#            PianoDwc3, product 0x5055) as ANSI (40, DESK_KEYBOARD_TYPE) in /Library/Preferences/com.apple.keyboardtype.plist, written
#            by the boot job as root: Keyboard Setup Assistant asks for an unknown keyboard's type, and it came up after every login
#            of attend1-4 (a window in front of the new desktop).
#   quiet  desk + com.apple.audiomxd disabled too (it spins on configd without an audio device: 0.3-0.5 cores, docs/power.md 1.2);
#          media playback may need it (VM-era note), hence not in desk
#   plain  desk's profile with the stock seed (no override, no deny, no keyboard type): the A/B partner when desk misbehaves
#   diag   plain + the keyboard type + exec-log=1 + piano-policy=1 (the cover's decisions logged, nothing acted on) +
#          tooling/k27/desk-reports.sh in the boot job: the failing jobs by name (which job exits 78), xprotectd's own log, the
#          unified log of loginwindow, WindowServer, Dock, Finder, SecurityAgent, authorizationhost ... around the login (what the
#          wait after the password is), the busiest processes after the login; one run answers what the console cannot
#
#   build-desktop-images.sh [NAME ...]   default: desk; out/ram-<NAME>/ and out/ram-desk-dump/ (the loop's dump image)
#
# Inputs (this checkout's out/ unless set): DESK_KC (out/k27/kc-k27-native.bin: tooling/k27/build-native-image.sh with
# K27_KERNEL=<k27 tree>/out/k27/kernel-k27-0064-native.macho PIANO_GPU=2 PIANO_VMM_PRESENT=0), DESK_ADT, DESK_FD, DESK_SEED /
# DESK_TRUST (the Turnip stage the same build made, out/turnip-a830/stage), DESK_RESET_AFTER (default 3600 s; 0: none, the image
# then runs until the owner's reset), DESK_RTC_OFFSET (piano-rtc=1 piano-rtc-offset=N: the calendar from the PMIC RTC from the start,
# k27 0060, tooling/native/rtc-offset.py), DESK_KEYBOARD_TYPE (40 ANSI, 41 ISO, 42 JIS), DESK_TOUCH (default 2: piano-touch=2; 0
# leaves the touch screen out: the fallback when it resets or its controller keeps falling into its watchdog state; any other
# value is piano-touch's), DESK_EXTRA (added to every image's arguments, e.g. piano-touch-idle=0: the touch screen's level-1 pace). The staged seeds go to out/desk-stage/<name>. A new seed is a new identity: the images run Data volatile
# (data-persist=0), the owner's store is not touched.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${DESK_KC:-out/k27/kc-k27-native.bin}
ADT=${DESK_ADT:-out/adt/piano-k27.adt}
FD=${DESK_FD:-out/fd/piano-uefi-A-bundled.fd}
SEED_IN=${DESK_SEED:-out/turnip-a830/stage/scratch-data.img}
TRUST_IN=${DESK_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
RESET=${DESK_RESET_AFTER:-3600}
KBTYPE=${DESK_KEYBOARD_TYPE:-40}
for f in "$KC" "$ADT" "$FD" "$SEED_IN" "$TRUST_IN"; do [ -f "$f" ] || { echo "build-desktop-images: $f is missing" >&2; exit 1; }; done
case $KBTYPE in 40|41|42) ;; *) echo "build-desktop-images: DESK_KEYBOARD_TYPE=$KBTYPE: 40 (ANSI), 41 (ISO) or 42 (JIS)" >&2; exit 1 ;; esac
# The collection must have the drivers (each one's own start line: another kext's text may name a driver it watches; attend5's and
# desk1's images had no PianoKeyboard at all) and the fixes the desktop needs: PianoA830's map limits (the render server's
# reserve), NativeGopDisplay's cursor layer, NativeRamBlock's volatile pool, PianoTouch's watchdog download, drag delay and start
# grace, PianoKeyboard's resting thumb.
for s in 'start: piano-keyboard=%u' 'start: piano-touch=%u' 'start: piano-backlight=%u' 'start: piano-usb=%u' 'start: piano-power=%u' \
         "the render server's reserve of ranges" 'composition: layer 0 the frame, layer 1 the cursor' \
         '(data-volatile-mib), in 2 MiB chunks' 'the download for the watchdog failed: again in' 'piano-touch-drag-delay' \
         'no read without one before it' 'piano-keyboard-resting'; do
  grep -a -q -F "$s" "$KC" || { echo "build-desktop-images: $KC lacks \"$s\": rebuild it from this tree (tooling/k27/build-native-image.sh)" >&2; exit 1; }
done
if [ -n "${DESK_RTC_OFFSET:-}" ]; then
  [[ $DESK_RTC_OFFSET =~ ^[0-9]+$ ]] || { echo "build-desktop-images: DESK_RTC_OFFSET=$DESK_RTC_OFFSET is not a number" >&2; exit 1; }
  grep -a -q 'the calendar is not set from the RTC' "$KC" \
    || { echo "build-desktop-images: DESK_RTC_OFFSET needs k27 0060 in $KC's kernel (K27_KERNEL=...-0064-native.macho)" >&2; exit 1; }
fi

# The desktop profile: every argument a driver level that ran on the tablet (docs/attended-desktop.md section 3).
PROFILE="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-power=1 piano-usb=2 piano-usb-hid=1"
PROFILE="$PROFILE piano-keyboard=2 piano-keyboard-gestures=1 piano-backlight=2 piano-backlight-keys=1 piano-gpu-hang-ms=10000"
TOUCH=${DESK_TOUCH:-2}
[[ $TOUCH =~ ^[0-9]+$ ]] || { echo "build-desktop-images: DESK_TOUCH=$TOUCH is not a level" >&2; exit 1; }
[ "$TOUCH" = 0 ] || PROFILE="$PROFILE piano-touch=$TOUCH"
[ -z "${DESK_RTC_OFFSET:-}" ] || PROFILE="$PROFILE piano-rtc=1 piano-rtc-offset=$DESK_RTC_OFFSET"
PROFILE="$PROFILE ${DESK_EXTRA:-}"
LOOPS="com.apple.watchdogd com.apple.mobileactivationd com.apple.security.xprotectd com.apple.PerfPowerServices"
DENY_DESK=/usr/libexec/watchdogd,/usr/libexec/PerfPowerServices
# The cover (0x504b = 20555) and PianoDwc3's keyboard (0x5055 = 20565), vendor 0x1af4 = 6900: "product-vendor-0" => type.
KEYBOARDS="defaults write /Library/Preferences/com.apple.keyboardtype keyboardtype -dict-add 20555-6900-0 -int $KBTYPE 20565-6900-0 -int $KBTYPE"
ORDER="desk"
ALL="desk quiet plain diag"

stage() {   # stage NAME [stage-boot-run.py arguments]: out/desk-stage/NAME from the input seed
  local name=$1
  shift
  python3 tooling/k27/stage-boot-run.py --seed "$SEED_IN" --trust "$TRUST_IN" --out "out/desk-stage/$name" "$@" \
    > "out/desk-stage/$name.log" 2>&1 || { tail -5 "out/desk-stage/$name.log" >&2; exit 1; }
  tail -1 "out/desk-stage/$name.log"
}
mkdir -p out/desk-stage
NAMES=${*:-$ORDER}
for n in $NAMES; do
  case " $ALL " in *" $n "*) ;; *) echo "build-desktop-images: no image $n (one of: $ALL)" >&2; exit 2 ;; esac
done
for n in $NAMES; do
  SEED=$SEED_IN TRUST=$TRUST_IN DENY= A=
  case $n in
    desk|quiet)
      X=(--boot-run "$KEYBOARDS")
      for j in $LOOPS; do X+=(--disable-job "$j"); done
      DENY=$DENY_DESK
      if [ "$n" = quiet ]; then X+=(--disable-job com.apple.audiomxd); DENY=$DENY,/usr/libexec/audiomxd; fi
      stage "$n" "${X[@]}" ;;
    plain) ;;
    diag)
      stage diag --file tooling/k27/desk-reports.sh:private/var/piano/desk-reports.sh \
        --boot-run "$KEYBOARDS" --boot-run "sh /private/var/piano/desk-reports.sh 40"
      A="exec-log=1 piano-policy=1" ;;
  esac
  if [ "$n" != plain ]; then SEED=out/desk-stage/$n/scratch-data.img TRUST=out/desk-stage/$n/kernel.macho.trustcache; fi
  COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
  PIANO_FAST_BOOT=1 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=$RESET PIANO_DENY_EXEC=$DENY PIANO_EXTRA_ARGS="$PROFILE $A" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-$n" | tail -1
done
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST_IN" --seed "$SEED_IN" --adt "$ADT" --utc "$(date +%s)")
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" \
  --out out/ram-desk-dump | tail -1
for n in $NAMES desk-dump; do shasum -a 256 "out/ram-$n/boot-piano-macos-ram-A-bundled.img"; done
