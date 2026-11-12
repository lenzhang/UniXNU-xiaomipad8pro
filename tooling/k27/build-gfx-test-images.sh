#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The test images of the login window's GPU rendering (docs/drivers/gpu-userspace.md "The login window's rendering, 2026-11-12"):
# Turnip 0007 (a sysmem pass's draws that read the attachments they render to wait for the earlier draws' writes) against the control
# without it, the wider flush, the kernel's IO-coherence test, and every render-pass barrier kept. Each is a RAM image of the GPU
# profile (PianoA830 and Turnip, vmm-present 0), the USB console (stderr-log=1: Turnip's and the plugin's lines), the USB keyboard from
# the Mac, the buttons, the fixed Power Saver CPU level (cool on the cable, unattended), and piano-reset-after as the backstop. Data:
# build-ram-images.py's store guard decides (another seed than the installed one: data-persist=0, every boot a first boot).
#
#   build-gfx-test-images.sh [NAME ...]    default: fix ctl all coh bar; out/ram-gfx-<NAME>/ and out/ram-gfx-dump/
#
#   fix  the plugin of this tree (Turnip 0001-0007, frontend 0001-0002), tu-debug "sysmem"
#   ctl  the same binaries, tu-debug "sysmem,nofbflush": 0007 off in every process (the photos' runs of 2026-11-12, same plugin)
#   all  tu-debug "sysmem,fbflushall": 0007 before every draw of the plugin's feedback-loop passes (every draw), if fix is not enough
#   coh  as fix plus piano-gpu-cohtest=1: the kernel's V9b/V9c, after which GPU-written imports (IOSurfaces) and own BOs may be mapped
#        IO-coherent (level 2): whether CPU-written surface pages the GPU reads stale matter
#   bar  as fix plus /private/var/ngpu/debug/render-barriers "all" and piano-metal-barriers=all (frontend 0002): RenderBox's
#        render-pass barriers kept in panel-sized surface passes (SecurityAgent's full-screen window included)
#   ubwc (by name only) tu-debug "sysmem,noubwc": no UBWC compression, for the seam along a full-screen draw's diagonal (a block shared
#        by its two triangles read back half-written)
#
# Each tu-debug value is its own Data seed (staged once into out/turnip-a830/stage-gfx-<name>/ by tooling/k27/stage-soft-metal.py):
# a sandboxed client that cannot read the boot line takes the seed's file, so the A/B holds for SecurityAgent too, not only for
# WindowServer. Every seed's boot job also prints WindowServer's, SecurityAgent's and loginwindow's os_log errors and faults
# (--log-errors; GFX_LOG= leaves that out).
#
# Inputs (this repository's out/ unless set): GFX_KC (out/k27/kc-k27-native.bin, built with PianoA830 linked, e.g.
# `PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh`), GFX_ADT (out/adt/piano-k27.adt), GFX_FD, GFX_INPUT_SEED
# (out/inputs/scratch-data.img), GFX_INPUT_TRUST (out/inputs/kernel.macho.trustcache), GFX_BUNDLE (out/turnip-a830/NativeAdrenoMetal.bundle,
# tooling/metal/build-turnip-a830.sh; rebuilt here when missing), GFX_EXTRA (added to every image), GFX_RESET (piano-reset-after,
# default 720), GFX_SLEEP (the test sleep, default 240 s; 0 none).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${GFX_KC:-out/k27/kc-k27-native.bin}
ADT=${GFX_ADT:-out/adt/piano-k27.adt}
FD=${GFX_FD:-out/fd/piano-uefi-A-bundled.fd}
INPUT_SEED=${GFX_INPUT_SEED:-out/inputs/scratch-data.img}
INPUT_TRUST=${GFX_INPUT_TRUST:-out/inputs/kernel.macho.trustcache}
BUNDLE=${GFX_BUNDLE:-out/turnip-a830/NativeAdrenoMetal.bundle}
LOG=${GFX_LOG-'process == "WindowServer" OR process == "SecurityAgent" OR process == "loginwindow"'}
for f in "$KC" "$ADT" "$FD" "$INPUT_SEED" "$INPUT_TRUST"; do [ -f "$f" ] || { echo "build-gfx-test-images: $f is missing" >&2; exit 1; }; done
grep -a -q com.unixnu.driver.PianoA830 "$KC" || { echo "build-gfx-test-images: $KC has no PianoA830" >&2; exit 1; }
[ -f "$BUNDLE/Contents/MacOS/libvulkan_freedreno.dylib" ] || tooling/metal/build-turnip-a830.sh > out/k27/build-turnip-a830.log 2>&1
grep -a -q nofbflush "$BUNDLE/Contents/MacOS/libvulkan_freedreno.dylib" \
  || { echo "build-gfx-test-images: $BUNDLE's Turnip has no 0007 (nofbflush): rebuild it (tooling/metal/build-turnip-a830.sh)" >&2; exit 1; }
BASE="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-input=1 piano-power=1 piano-power-ps=1"
BASE="$BASE piano-power-period=10 piano-thermal=1 piano-cpufreq=2 piano-keyboard=2 piano-backlight=2 exec-log=1 stderr-log=1"
BASE="$BASE piano-gpu-hang-ms=10000 ${GFX_EXTRA:-}"
# One test sleep (GFX_SLEEP seconds after boot completion, default 240; 0 none): the login window as first drawn, then as drawn again
# after a wake. The run of 2026-11-12 07:48 (three test sleeps) was the one boot of three with no flat boxes but polygonal glass shapes.
if [ "${GFX_SLEEP:-240}" != 0 ]; then
  BASE="$BASE piano-sleep=1 piano-sleep-state=1 piano-sleep-test=${GFX_SLEEP:-240} piano-sleep-test-count=1 piano-sleep-max=30"
fi
ORDER="fix ctl all coh bar"
image() {   # TU: the seed's tu-debug; FILES: more --file arguments; A: the image's own arguments
  TU=sysmem FILES=() A=
  case $1 in
    fix) ;;
    ctl) TU=sysmem,nofbflush ;;
    all) TU=sysmem,fbflushall ;;
    ubwc) TU=sysmem,noubwc ;;
    coh) A="piano-gpu-cohtest=1" ;;
    bar) printf 'all\n' > out/turnip-a830/render-barriers-all
         FILES=(--file out/turnip-a830/render-barriers-all:private/var/ngpu/debug/render-barriers)
         A="piano-metal-barriers=all" ;;
    *) return 1 ;;
  esac
}
stage() {   # stage NAME: the Data seed and trust cache for image NAME's tu-debug and files, once per input change
  local out=out/turnip-a830/stage-gfx-$1 stamp
  printf '%s\n' "$TU" > "out/turnip-a830/tu-debug-gfx-$1"
  stamp=$( { shasum -a 256 "$INPUT_SEED" "$INPUT_TRUST" tooling/k27/stage-soft-metal.py "out/turnip-a830/tu-debug-gfx-$1"
             find "$BUNDLE" -type f | sort | xargs shasum -a 256; echo "log $LOG ${FILES[*]+${FILES[*]}}"; } | shasum -a 256 | cut -c1-16)
  if [ "$(cat "$out/stamp" 2>/dev/null)" != "$stamp" ] || [ ! -f "$out/scratch-data.img" ]; then
    mkdir -p "$out"
    python3 tooling/k27/stage-soft-metal.py --seed "$INPUT_SEED" --trust "$INPUT_TRUST" --out "$out" ${LOG:+--log-errors "$LOG"} \
      --file "out/turnip-a830/tu-debug-gfx-$1:private/var/ngpu/debug/tu-debug" ${FILES[@]+"${FILES[@]}"} "$BUNDLE" \
      > "$out/stage.log" 2>&1 || { tail -5 "$out/stage.log" >&2; exit 1; }
    echo "$stamp" > "$out/stamp"
  fi
  SEED=$out/scratch-data.img TRUST=$out/kernel.macho.trustcache
}
for n in ${*:-$ORDER}; do
  image "$n" || { echo "build-gfx-test-images: no image $n (one of: $ORDER ubwc)" >&2; exit 2; }
  case $n in coh) stage fix ;; *) stage "$n" ;; esac
  PIANO_FAST_BOOT=1 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=${GFX_RESET:-720} PIANO_EXTRA_ARGS="$BASE $A" \
    python3 tooling/bootimg/build-ram-images.py --fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" \
      --utc "$(date +%s)" --out "out/ram-gfx-$n" | tail -1
done
image fix; stage fix
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py --fd "$FD" --fd-name A-bundled --kc "$KC" \
  --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)" --out out/ram-gfx-dump | tail -1
shasum -a 256 out/ram-gfx-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
