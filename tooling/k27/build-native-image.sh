#!/bin/bash
# One command after a NativeUfs/NativeRamBlock or k27 change: native kexts -> KC, the ADT -> the macOS RAM image (FD A) in out/ram, and the
# loop's dump image in out/ram-dump (the same with piano-reset-after=1: it only lets our UEFI read the previous run's log).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
LIST=$(sed -e 's/#.*//' "${PIANO_KEXT_LIST:-configs/k27-native-kexts.txt}" | awk 'NF {print $1}')
for k in $LIST; do   # the kexts' objects: tooling/k27/build-native-<name>.sh
  case $k in ARMPlatformStandIn) b=standin ;; NativeUfs) b=ufs ;; NativeRamBlock) b=ram-block ;; NativeGopDisplay) b=gop-display ;; PianoDwc3) b=piano-dwc3 ;; PianoInput) b=piano-input ;; PianoNullEthernet) b=null-ethernet ;; PianoPower) b=piano-power ;; NativeSoftGpu) b=soft-gpu ;; PianoA830) b=piano-a830 ;; *) b= ;; esac
  [ -n "$b" ] || { echo "no build script for $k" >&2; exit 1; }
  tooling/k27/build-native-$b.sh > out/k27/build-native-$b.log 2>&1 || { grep -E "error:|Error" out/k27/build-native-$b.log | head; exit 1; }
done
# PIANO_VMM_PRESENT=0|1: the ADT's /defaults vmm-present for this build (else configs/k27-adt.toml's; docs/drivers/soft-metal.md §9).
python3 tooling/adt/make-piano-adt.py out/adt/piano-k27.adt --unique-chip-id "${UNIQUE_CHIP_ID:-47554e5941480001}" --data --trust \
  ${PIANO_VMM_PRESENT:+--vmm-present "$PIANO_VMM_PRESENT"} > /dev/null
# The software Metal path (docs/drivers/soft-metal.md): when NativeSoftGpu is linked (the list or PIANO_EXTRA_KEXTS; PIANO_SOFT_METAL=0
# leaves this out, =1 forces it), the NativeSoftMetal plugin is built and staged into a copy of the Data seed and the trust cache
# (tooling/k27/stage-soft-metal.py, out/soft-metal), which the image then takes unless PIANO_SEED / PIANO_TRUST name others. The staging
# is redone only when its inputs change (out/soft-metal/stamp): a new seed is a new seed identity, and data-persist=2 starts its store
# over for one. PIANO_SOFT_METAL_LOG=PREDICATE adds the error/fault log loop (stage-soft-metal.py --log-errors) to the seed's boot job.
# Reduce transparency is staged too (--reduce-transparency: clients draw no backdrop layers, which Core Animation's software renderer
# asserts on); PIANO_REDUCE_TRANSPARENCY=0 leaves it out.
SOFT_METAL=${PIANO_SOFT_METAL:-}
if [ -z "$SOFT_METAL" ]; then
  case " $LIST ${PIANO_EXTRA_KEXTS:-} " in *NativeSoftGpu*) SOFT_METAL=1 ;; *) SOFT_METAL=0 ;; esac
fi
if [ "$SOFT_METAL" = 1 ]; then
  tooling/k27/build-soft-metal-plugin.sh > out/k27/build-soft-metal-plugin.log 2>&1 || { grep -E "error" out/k27/build-soft-metal-plugin.log | head; exit 1; }
  STAMP=$( { shasum -a 256 out/inputs/scratch-data.img out/inputs/kernel.macho.trustcache tooling/k27/stage-soft-metal.py
             find out/soft-metal/NativeSoftMetal.bundle -type f | sort | xargs shasum -a 256; echo "log ${PIANO_SOFT_METAL_LOG:-} rt ${PIANO_REDUCE_TRANSPARENCY:-1}"; } | shasum -a 256 | cut -c1-16)
  if [ "$(cat out/soft-metal/stamp 2>/dev/null)" != "$STAMP" ] || [ ! -f out/soft-metal/scratch-data.img ]; then
    RT=; [ "${PIANO_REDUCE_TRANSPARENCY:-1}" = 1 ] && RT=--reduce-transparency
    python3 tooling/k27/stage-soft-metal.py ${PIANO_SOFT_METAL_LOG:+--log-errors "$PIANO_SOFT_METAL_LOG"} $RT > out/k27/stage-soft-metal.log 2>&1 \
      || { tail -5 out/k27/stage-soft-metal.log; exit 1; }
    echo "$STAMP" > out/soft-metal/stamp
  fi
  PIANO_SEED=${PIANO_SEED:-out/soft-metal/scratch-data.img}
  PIANO_TRUST=${PIANO_TRUST:-out/soft-metal/kernel.macho.trustcache}
fi
# The A830 GPU path (docs/drivers/gpu-userspace.md): when PianoA830 is linked (the list or PIANO_EXTRA_KEXTS; PIANO_A830_METAL=0
# leaves this out, =1 forces it), the VM's NativeAdrenoMetal.bundle with Turnip rebuilt on Adreno830's ABI v2
# (tooling/metal/build-turnip-a830.sh) is staged into a copy of the Data seed and the trust cache (out/turnip-a830/stage), which the
# image then takes unless PIANO_SEED / PIANO_TRUST name others; redone only when its inputs change (out/turnip-a830/stage/stamp).
# PIANO_A830_TU_DEBUG (default sysmem: no GMEM for the first bring-up) is staged as /private/var/ngpu/debug/tu-debug, Turnip's
# TU_DEBUG in every Metal process without one; PIANO_A830_TU_DEBUG= stages none. NativeSoftGpu and PianoA830 are exclusive: Metal
# takes the first accelerator it finds, and SkyLight the first device's port.
A830_METAL=${PIANO_A830_METAL:-}
if [ -z "$A830_METAL" ]; then
  case " $LIST ${PIANO_EXTRA_KEXTS:-} " in *PianoA830*) A830_METAL=1 ;; *) A830_METAL=0 ;; esac
fi
if [ "$A830_METAL" = 1 ]; then
  case "$SOFT_METAL $LIST ${PIANO_EXTRA_KEXTS:-} " in 1*|*NativeSoftGpu*)
    echo "NativeSoftGpu (or PIANO_SOFT_METAL=1) and PianoA830 together: Metal takes the first accelerator; leave NativeSoftGpu out" >&2
    exit 1 ;; esac
  tooling/metal/build-turnip-a830.sh > out/k27/build-turnip-a830.log 2>&1 \
    || { grep -E "error|FAILED|^no |not ABI" out/k27/build-turnip-a830.log | head; exit 1; }
  A830_TU_DEBUG=${PIANO_A830_TU_DEBUG-sysmem}
  STAMP=$( { shasum -a 256 out/inputs/scratch-data.img out/inputs/kernel.macho.trustcache tooling/k27/stage-soft-metal.py
             find out/turnip-a830/NativeAdrenoMetal.bundle -type f | sort | xargs shasum -a 256; echo "tu-debug $A830_TU_DEBUG"; } | shasum -a 256 | cut -c1-16)
  if [ "$(cat out/turnip-a830/stage/stamp 2>/dev/null)" != "$STAMP" ] || [ ! -f out/turnip-a830/stage/scratch-data.img ]; then
    FILES=()
    if [ -n "$A830_TU_DEBUG" ]; then
      printf '%s\n' "$A830_TU_DEBUG" > out/turnip-a830/tu-debug
      FILES=(--file out/turnip-a830/tu-debug:private/var/ngpu/debug/tu-debug)
    fi
    python3 tooling/k27/stage-soft-metal.py --out out/turnip-a830/stage ${FILES[@]+"${FILES[@]}"} out/turnip-a830/NativeAdrenoMetal.bundle \
      > out/k27/stage-turnip-a830.log 2>&1 || { tail -5 out/k27/stage-turnip-a830.log; exit 1; }
    echo "$STAMP" > out/turnip-a830/stage/stamp
  fi
  PIANO_SEED=${PIANO_SEED:-out/turnip-a830/stage/scratch-data.img}
  PIANO_TRUST=${PIANO_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
fi
# The descriptors with their objects' absolute place (@OUT@ = this repository's out/), plus PIANO_EXTRA_KEXTS (JSON paths) for a test.
mkdir -p out/k27/kext; NATIVE=
for k in $LIST; do sed "s#@OUT@#$D/out#" "tooling/k27/kext/$k.json" > "out/k27/kext/$k.json"; NATIVE="$NATIVE $D/out/k27/kext/$k.json"; done
NATIVE="$NATIVE ${PIANO_EXTRA_KEXTS:-}" \
  tooling/kc/build-kc-k27.sh "${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}" out/k27/kc-k27-native.bin > out/k27/build-kc.log 2>&1 || { tail -5 out/k27/build-kc.log; exit 1; }
# PIANO_TRUST / PIANO_SEED: another trust cache and Data seed for a test (e.g. tooling/k27/stage-soft-metal.py's, with a staged plugin).
COMMON=(--fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin --trust "${PIANO_TRUST:-out/inputs/kernel.macho.trustcache}"
        --seed "${PIANO_SEED:-out/inputs/scratch-data.img}" --adt "${PIANO_ADT:-out/adt/piano-k27.adt}" --utc "$(date +%s)")
# The dump image only lets our UEFI read the previous run's log; its XNU never touches the Data store (data-persist=0), so a dump
# image with another seed cannot reset the store of the installed system (docs/persistent-boot.md).
PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-dump > out/k27/build-ram-dump.log 2>&1 &
python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram | tail -1
wait $! || { tail -3 out/k27/build-ram-dump.log; exit 1; }
