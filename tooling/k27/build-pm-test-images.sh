#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The power test images of docs/power.md §6 (k27 0061 CPU idle, 0062 suspend-to-idle; the ufs* images: the storage side,
# docs/drivers/ufs.md §12.8; island, gsleep*, gmeasure: the GPU's part, L1, G1-G3, GM), in the order of risk, from a KC whose
# kernel has 0061 and 0062. Each is a RAM image of the GPU profile (as the installed system boots: PianoA830 and Turnip,
# vmm-present 0) plus PianoPower's battery lines every 10 s, the USB console and the buttons, with piano-reset-after as the
# backstop and every reset into ABL fastboot (build-ram-images.py's K27_ARGS). Data: build-ram-images.py's store guard decides
# (another seed than the installed one: data-persist=0).
#
#   build-pm-test-images.sh [NAME ...]        default: all; out/ram-pm-<NAME>/ and one dump image out/ram-pm-dump/
#
# Inputs (this repository's out/ unless set): PM_KC (out/k27/kc-k27-native.bin, built by
# `K27_KERNEL=<kernel-k27-0062-native.macho> PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh`), PM_ADT
# (out/adt/piano-k27.adt, the ADT that build wrote), PM_FD, PM_SEED, PM_TRUST (the Turnip stage). The KC's kernel is checked
# for the 0061/0062 symbols.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
KC=${PM_KC:-out/k27/kc-k27-native.bin}
ADT=${PM_ADT:-out/adt/piano-k27.adt}
FD=${PM_FD:-out/fd/piano-uefi-A-bundled.fd}
SEED=${PM_SEED:-out/turnip-a830/stage/scratch-data.img}
TRUST=${PM_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
for f in "$KC" "$ADT" "$FD" "$SEED" "$TRUST"; do [ -f "$f" ] || { echo "build-pm-test-images: $f is missing" >&2; exit 1; }; done
for sym in piano_cpuidle_enter piano_s2idle_enter; do
  grep -a -q "$sym" "$KC" || { echo "build-pm-test-images: $KC has no $sym: its kernel lacks k27 0061/0062" >&2; exit 1; }
done
# The GPU profile (tooling/persist/build-esp.py 'gpu') and the measurement's companions, plus PM_EXTRA for every image (e.g.
# "piano-backlight=2" once docs/drivers/backlight.md's test passed). PIANO_FAST_BOOT=1 drops -v and droidvm-disable-jobs, which
# keeps the command line under DarwinBoot's 1023 bytes with the power arguments.
BASE="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-power=1 piano-power-period=10 piano-usb=2 piano-input=1 ${PM_EXTRA:-}"
CYCLES="piano-sleep-test=150 piano-sleep-test-count=3 piano-sleep-max=30"
ORDER="base probe idle1 idle2 idle1pd idle3 sleep0 sleep1 sleep2 sleepkey measure ufssleep0 ufssleep1 ufsahit ufsahitsleep ufsmeasure"
ORDER="$ORDER island gsleep1 gsleep2 gsleep3 gmeasure"
# The storage side (docs/drivers/ufs.md §12): native-ufs-suspend=1 (NativeUfs's link into Hibern8 for each sleep, the Data disk held
# and synced first when its store is open read-write) and native-ufs-ahit=MS (auto-hibernate at run time). The store follows
# build-ram-images.py's guard like every image here: with another seed than the installed one Data is volatile and only the root
# windows take part; with the installed seed (data-persist=2) NativeRamBlock takes part too.
UFS="native-ufs-suspend=1"
# The GPU's part (docs/drivers/gpu.md "System suspend", "The LLCC island"; power.md §6 L1, G1-G3, GM): PianoA830 with the
# piano-gpu-suspend / piano-llcc-island arguments on top of the same base (every GPU image prints the island's read-only line, L0).
# G3 (the rails' votes gone) also takes the UFS link down in its own kOff ($UFS): the GPU's host votes are UFS's CX/MX too.
GSUS="piano-gpu-suspend=1"
# image NAME: sets A (its arguments) and R (its piano-reset-after, s)
image() {
  case $1 in
    base)     A="";                                             R=900 ;;    # the A of every A/B: today's behaviour
    probe)    A="piano-pm-probe=1";                             R=240 ;;    # PSCI, RSC, RPMh, LLCC: read only
    idle1)    A="piano-cpuidle=1";                              R=900 ;;    # ret
    idle2)    A="piano-cpuidle=2";                              R=900 ;;    # + cl5 (OS-initiated)
    idle1pd)  A="piano-cpuidle=1 piano-cpuidle-pd=1";           R=300 ;;    # ret as power-down: the reset-vector path
    idle3)    A="piano-cpuidle=3";                              R=300 ;;    # + ss3 (refused unless the LLCC island is reserved)
    sleep0)   A="piano-sleep=1 piano-sleep-state=0 $CYCLES";    R=600 ;;    # suspend-to-idle, CPUs in WFI
    sleep1)   A="piano-sleep=1 piano-sleep-state=1 $CYCLES";    R=600 ;;    # ... in ret
    sleep2)   A="piano-cpuidle=2 piano-sleep=1 piano-sleep-state=2 $CYCLES"; R=600 ;;   # ... cl5, idle level 2 between
    sleepkey) A="piano-sleep=1 piano-sleep-state=1 piano-sleep-wake=31 piano-sleep-test=120 piano-sleep-test-count=4"
              A="$A piano-sleep-max=600 piano-keyboard=2 piano-keyboard-lid=2"; R=1800 ;;   # attended (power key, cover, USB-C,
              # vol-, keyboard); the cover's close needs PianoKeyboard in the KC: PIANO_EXTRA_KEXTS=$PWD/out/k27/kext/PianoKeyboard.json
    measure)  A="piano-cpuidle=2 piano-sleep=1 piano-sleep-state=2 piano-sleep-test=240 piano-sleep-test-count=1"
              A="$A piano-sleep-max=1800";                      R=2400 ;;   # no cable: 30 min suspended between battery lines
    ufssleep0) A="piano-sleep=1 piano-sleep-state=0 $CYCLES $UFS"; R=600 ;;   # S1 + the UFS link in Hibern8 (CPUs in WFI)
    ufssleep1) A="piano-sleep=1 piano-sleep-state=1 $CYCLES $UFS"; R=600 ;;   # S2 + the same
    ufsahit)   A="native-ufs-ahit=1000";                          R=900 ;;    # auto-hibernate after 1 s idle; A/B with base
    ufsahitsleep) A="native-ufs-ahit=1000 piano-sleep=1 piano-sleep-state=1 $CYCLES $UFS"; R=600 ;;   # both: AHIT off/on per sleep
    ufsmeasure) A="piano-cpuidle=2 piano-sleep=1 piano-sleep-state=2 piano-sleep-test=240 piano-sleep-test-count=1"
              A="$A piano-sleep-max=1800 $UFS";                 R=2400 ;;   # measure's A/B: the same with the link in Hibern8
    island)   A="piano-pm-probe=1 piano-llcc-island=1";        R=300 ;;    # L1: ISLAND1's ways reserved, read back (DSP stopped)
    gsleep1)  A="piano-sleep=1 piano-sleep-state=1 $CYCLES $GSUS"; R=600 ;; # G1: the GPU down, its votes at NOM_L1 / the display floor
    gsleep2)  A="piano-sleep=1 piano-sleep-state=1 $CYCLES $GSUS piano-dpu=1 piano-dpu-panel=1"; R=600 ;;   # G2: + panel sleep: DDR 0
    gsleep3)  A="piano-cpuidle=2 piano-sleep=1 piano-sleep-state=3 $CYCLES $GSUS piano-gpu-suspend-rails=0 piano-dpu=1"
              A="$A piano-dpu-panel=1 piano-llcc-island=1 piano-llcc-island-ok=1 $UFS"; R=600 ;;   # G3 (attended): rails 0, ss3, island
    gmeasure) A="piano-cpuidle=2 piano-sleep=1 piano-sleep-state=2 piano-sleep-test=240 piano-sleep-test-count=1"
              A="$A piano-sleep-max=1800 $GSUS";                R=2400 ;;   # GM: M with the GPU's part
    *) return 1 ;;
  esac
}
COMMON=(--fd "$FD" --fd-name A-bundled --kc "$KC" --trust "$TRUST" --seed "$SEED" --adt "$ADT" --utc "$(date +%s)")
for n in ${*:-$ORDER}; do
  image "$n" || { echo "build-pm-test-images: no image $n (one of: $ORDER)" >&2; exit 2; }
  PIANO_FAST_BOOT=1 PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=$R PIANO_EXTRA_ARGS="$BASE $A" \
    python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out "out/ram-pm-$n" | tail -1
done
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-pm-dump \
  | tail -1
shasum -a 256 out/ram-pm-*/boot-piano-macos-ram-A-bundled.img | sed "s#  $D/#  #"
