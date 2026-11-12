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
#                                           ReportCrash run: 15-17 crashes a boot (attend1-4, 2026-11-11/12)
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
#          - the still wallpaper (DESK_WALLPAPER=still, the default; docs/drivers/gpu-userspace.md, "Allocation limits and the video
#            wallpaper, 2026-11-12"): tooling/k27/still-wallpaper.sh and its files (tooling/k27/still-wallpaper.py), run first by the boot
#            job after the clock, give the login window (and the account's desktop, where it has no wallpaper store yet) one dark ochre
#            picture instead of the default aerial video: no software HEVC decode and no 4K conversion every frame at the login window
#            (battery). DESK_WALLPAPER=video stages none: the aerial video, drawn since frontend 0003, for testing that path.
#   quiet  desk + com.apple.audiomxd disabled too (it spins on configd without an audio device: 0.3-0.5 cores, docs/power.md 1.2);
#          media playback may need it (VM-era note), hence not in desk
#   plain  desk's profile with the stock seed (no override, no deny, no keyboard type, no wallpaper: the aerial video): the A/B
#          partner when desk misbehaves
#   diag   plain + the keyboard type + the still wallpaper (as desk's) + exec-log=1 + piano-policy=1 (the cover's decisions logged,
#          nothing acted on) + tooling/k27/desk-reports.sh in the boot job: the failing jobs by name (which job exits 78), xprotectd's own log, the
#          unified log of loginwindow, WindowServer, Dock, Finder, SecurityAgent, authorizationhost ... around the login (what the
#          wait after the password is), the busiest processes after the login; one run answers what the console cannot
#
#   desk-dev desk + the ways in from the Mac (docs/attended-desktop.md section 3): piano-usb-shell=1 with the userland stage's
#          pianousb-shell in the seed (tooling/userland/stage.py, its pmset defaults off: no display sleep after 2 min while
#          debugging), piano-usb-net=1, sshd with a key of this Mac's (out/desk-dev/id_ed25519, made here once; the public half
#          staged as each account's authorized_keys by tooling/k27/desk-dev.sh, which also sets and prints the USB network's
#          address: DESK_DEV_NET auto (default: a DHCP lease within 20 s, else the fixed 169.254.80.2), static or dhcp; the Mac's side
#          is tooling/native/desk-ssh.sh), and configd's interface database (tooling/k27/desk-dev-NetworkInterfaces.plist: en0 and the
#          USB network's en1 named at once, not after InterfaceNamer's 240 s wait for an IOKit quiet that never comes);
#          reset after 3600 s (DESK_RESET_AFTER) as desk. The owner logs in himself (or tooling/native/desk-login.sh types his
#          password from the ignored private/login.secret)
#
#   build-desktop-images.sh [NAME ...]   default: desk; out/ram-<NAME>/ and out/ram-desk-dump/ (the loop's dump image)
#
# Inputs (this checkout's out/ unless set): DESK_KC (out/k27/kc-k27-native.bin: tooling/k27/build-native-image.sh with
# K27_KERNEL=<k27 tree>/out/k27/kernel-k27-0064-native.macho PIANO_GPU=2 PIANO_VMM_PRESENT=0), DESK_ADT, DESK_FD, DESK_SEED /
# DESK_TRUST (the Turnip stage the same build made, out/turnip-a830/stage), DESK_RESET_AFTER (default 3600 s; 0: none, the image
# then runs until the owner's reset), DESK_RTC_OFFSET (piano-rtc=1 piano-rtc-offset=N: the calendar from the PMIC RTC from the start,
# k27 0060, tooling/native/rtc-offset.py), DESK_KEYBOARD_TYPE (40 ANSI, 41 ISO, 42 JIS), DESK_TOUCH (default 2: piano-touch=2; 0
# leaves the touch screen out: the fallback when it resets or its controller keeps falling into its watchdog state; any other
# value is piano-touch's), DESK_WALLPAPER (still, the default, or video: above), DESK_EXTRA (added to every image's arguments, e.g. piano-touch-idle=0: the touch screen's level-1 pace). The staged seeds go to out/desk-stage/<name>. A new seed is a new identity: the images run Data volatile
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
WALLPAPER=${DESK_WALLPAPER:-still}
case $WALLPAPER in still|video) ;; *) echo "build-desktop-images: DESK_WALLPAPER=$WALLPAPER: still or video" >&2; exit 1 ;; esac
DEVNET=${DESK_DEV_NET:-auto}
case $DEVNET in auto|static|dhcp) ;; *) echo "build-desktop-images: DESK_DEV_NET=$DEVNET: auto, static or dhcp" >&2; exit 1 ;; esac
# The collection must have the drivers (each one's own start line: another kext's text may name a driver it watches; attend5's and
# desk1's images had no PianoKeyboard at all) and the fixes the desktop needs: PianoA830's map limits (the render server's
# reserve), NativeGopDisplay's cursor layer, NativeRamBlock's volatile pool with its boot reservation (2026-11-12: refused writes
# corrupted desk-a's Data container), PianoTouch's watchdog download, drag delay and its announced-frame rule (no read without an
# edge, 2026-11-12: the watchdog loop of desk1 and desk-diag), PianoKeyboard's resting thumb, PianoA830's pooled BOs from several
# extents with the va/sizes lines (2026-11-12, desk-dev's OutOfMemory(Device): gpu-userspace.md), PianoPower's charger watchdog (the
# charger's stop at ~40 s of every boot, 2026-11-12: charging-policy.md §18) and its charging enable command with the record of the
# charger's register changes (charging-policy.md §19).
for s in 'start: piano-keyboard=%u' 'start: piano-touch=%u' 'start: piano-backlight=%u' 'start: piano-usb=%u' 'start: piano-power=%u' \
         "the render server's reserve of ranges" 'composition: layer 0 the frame, layer 1 the cursor' \
         'reserved at start (data-volatile-reserve-mib)' 'the download for the watchdog failed: again in' 'piano-touch-drag-delay' \
         'a frame read only when the controller announces it' 'piano-keyboard-resting' "per-object limit of %llu MiB (its GPU window)" \
         'va/sizes: %u:%s <1M' 'wdog: piano-power-wdog=%u (%s)' 'chgen: piano-power-chgen=%u (%s)'; do
  grep -a -q -F "$s" "$KC" || { echo "build-desktop-images: $KC lacks \"$s\": rebuild it from this tree (tooling/k27/build-native-image.sh)" >&2; exit 1; }
done
# The seed's Metal plugin must have the day's GPU fixes (docs/results-2026-11-12.md): Turnip 0007 (a sysmem draw that reads its own
# attachments waits for their writes: the login window's opaque boxes), 0008 (the per-object limit is the GPU window) and frontend
# 0003 (the 16384 px textures and the 4K YCbCr frames), 0009 (idle caches given back, every GPU process's cache asked for when a
# BoAlloc finds no memory): their messages in the staged libraries, as the seed's bytes hold them.
for s in 'an attachment feedback loop (fbflushall)' 'is past the per-object limit of' 'YCbCr10p 4:2:0' \
         'every GPU process was asked for its cached BOs'; do
  grep -a -q -F "$s" "$SEED_IN" || { echo "build-desktop-images: $SEED_IN lacks \"$s\" (Turnip 0007/0008/0009, frontend 0003): restage the Turnip seed (tooling/k27/build-native-image.sh)" >&2; exit 1; }
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
ALL="desk quiet plain diag desk-dev"

stage() {   # stage NAME [stage-boot-run.py arguments]: out/desk-stage/NAME from the input seed
  local name=$1
  shift
  python3 tooling/k27/stage-boot-run.py --seed "$SEED_IN" --trust "$TRUST_IN" --out "out/desk-stage/$name" "$@" \
    > "out/desk-stage/$name.log" 2>&1 || { tail -5 "out/desk-stage/$name.log" >&2; exit 1; }
  tail -1 "out/desk-stage/$name.log"
}
mkdir -p out/desk-stage
# The still wallpaper's files and boot command, first in each staged seed's boot job (the login window reads its picture about 7 s
# after the clock): none with DESK_WALLPAPER=video.
WALL=()
if [ "$WALLPAPER" = still ]; then
  WALL=(--file tooling/k27/still-wallpaper.sh:private/var/piano/still-wallpaper.sh)
  while read -r flag spec; do WALL+=("$flag" "$spec"); done < <(python3 tooling/k27/still-wallpaper.py out/wallpaper)
  WALL+=(--boot-run "sh /private/var/piano/still-wallpaper.sh")
fi
NAMES=${*:-$ORDER}
for n in $NAMES; do
  case " $ALL " in *" $n "*) ;; *) echo "build-desktop-images: no image $n (one of: $ALL)" >&2; exit 2 ;; esac
done
for n in $NAMES; do
  SEED=$SEED_IN TRUST=$TRUST_IN DENY= A=
  case $n in
    desk|quiet)
      X=(${WALL[@]+"${WALL[@]}"} --boot-run "$KEYBOARDS")
      for j in $LOOPS; do X+=(--disable-job "$j"); done
      DENY=$DENY_DESK
      if [ "$n" = quiet ]; then X+=(--disable-job com.apple.audiomxd); DENY=$DENY,/usr/libexec/audiomxd; fi
      stage "$n" "${X[@]}" ;;
    plain) ;;
    desk-dev)
      # the userland stage first (pianousb-shell and its trust cache entry), then desk's additions and the ways in
      python3 tooling/userland/stage.py --seed "$SEED_IN" --trust "$TRUST_IN" --out out/desk-stage/desk-dev-userland \
        --off piano-pmset-defaults > out/desk-stage/desk-dev-userland.log 2>&1 || { tail -5 out/desk-stage/desk-dev-userland.log >&2; exit 1; }
      mkdir -p out/desk-dev
      [ -f out/desk-dev/id_ed25519 ] || ssh-keygen -q -t ed25519 -N '' -C desk-dev -f out/desk-dev/id_ed25519
      # configd's interface database with en0 and the USB network's en1 by their MACs: named at once, not after the 240 s quiet timeout
      X=(${WALL[@]+"${WALL[@]}"} --boot-run "$KEYBOARDS" --file tooling/k27/desk-dev.sh:private/var/piano/desk-dev.sh
         --file out/desk-dev/id_ed25519.pub:private/var/piano/dev/authorized_keys
         --file tooling/k27/desk-dev-NetworkInterfaces.plist:Library/Preferences/SystemConfiguration/NetworkInterfaces.plist
         --boot-run "sh /private/var/piano/desk-dev.sh $DEVNET")
      for j in $LOOPS; do X+=(--disable-job "$j"); done
      DENY=$DENY_DESK
      python3 tooling/k27/stage-boot-run.py --seed out/desk-stage/desk-dev-userland/scratch-data.img \
        --trust out/desk-stage/desk-dev-userland/kernel.macho.trustcache --out out/desk-stage/desk-dev "${X[@]}" \
        > out/desk-stage/desk-dev.log 2>&1 || { tail -5 out/desk-stage/desk-dev.log >&2; exit 1; }
      tail -1 out/desk-stage/desk-dev.log
      A="piano-usb-shell=1 piano-usb-net=1" ;;
    diag)
      stage diag ${WALL[@]+"${WALL[@]}"} --file tooling/k27/desk-reports.sh:private/var/piano/desk-reports.sh \
        --boot-run "$KEYBOARDS" --boot-run "sh /private/var/piano/desk-reports.sh 40"
      A="exec-log=1 piano-policy=1" ;;
  esac
  if [ "$n" != plain ]; then SEED=out/desk-stage/$n/scratch-data.img TRUST=out/desk-stage/$n/kernel.macho.trustcache; fi
  COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
  R=$RESET P=
  env PIANO_FAST_BOOT=1 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER="$R" PIANO_DENY_EXEC="$DENY" PIANO_EXTRA_ARGS="$PROFILE $A" \
    ${P:+PIANO_DATA_PERSIST=$P} python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-$n" | tail -1
  python3 tooling/bootimg/verify-ram-images.py "out/ram-$n/build-A-bundled.json" | tail -1
done
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST_IN" --seed "$SEED_IN" --adt "$ADT" --utc "$(date +%s)")
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" \
  --out out/ram-desk-dump | tail -1
for n in $NAMES desk-dump; do shasum -a 256 "out/ram-$n/boot-piano-macos-ram-A-bundled.img"; done
