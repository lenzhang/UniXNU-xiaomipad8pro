#!/bin/bash
# One command after a NativeUfs/NativeRamBlock or k27 change: native kexts -> KC, the ADT -> the macOS RAM image (FD A) in out/ram, and the
# loop's dump image in out/ram-dump (the same with piano-reset-after=1: it only lets our UEFI read the previous run's log).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
LIST=$(sed -e 's/#.*//' configs/k27-native-kexts.txt | awk 'NF {print $1}')
for k in $LIST; do   # the kexts' objects: tooling/k27/build-native-<name>.sh
  case $k in ARMPlatformStandIn) b=standin ;; NativeUfs) b=ufs ;; NativeRamBlock) b=ram-block ;; NativeGopDisplay) b=gop-display ;; PianoDwc3) b=piano-dwc3 ;; PianoInput) b=piano-input ;; NativeSoftGpu) b=soft-gpu ;; *) b= ;; esac
  [ -n "$b" ] || { echo "no build script for $k" >&2; exit 1; }
  tooling/k27/build-native-$b.sh > out/k27/build-native-$b.log 2>&1 || { grep -E "error:|Error" out/k27/build-native-$b.log | head; exit 1; }
done
python3 tooling/adt/make-piano-adt.py out/adt/piano-k27.adt --unique-chip-id "${UNIQUE_CHIP_ID:-47554e5941480001}" --data --trust > /dev/null
# The descriptors with their objects' absolute place (@OUT@ = this repository's out/), plus PIANO_EXTRA_KEXTS (JSON paths) for a test.
mkdir -p out/k27/kext; NATIVE=
for k in $LIST; do sed "s#@OUT@#$D/out#" "tooling/k27/kext/$k.json" > "out/k27/kext/$k.json"; NATIVE="$NATIVE $D/out/k27/kext/$k.json"; done
NATIVE="$NATIVE ${PIANO_EXTRA_KEXTS:-}" \
  tooling/kc/build-kc-k27.sh "${K27_KERNEL:-$K/out/k27/kernel-k27-0058-native.macho}" out/k27/kc-k27-native.bin > out/k27/build-kc.log 2>&1 || { tail -5 out/k27/build-kc.log; exit 1; }
# PIANO_TRUST / PIANO_SEED: another trust cache and Data seed for a test (e.g. tooling/k27/stage-soft-metal.py's, with a staged plugin).
COMMON=(--fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin --trust "${PIANO_TRUST:-out/inputs/kernel.macho.trustcache}"
        --seed "${PIANO_SEED:-out/inputs/scratch-data.img}" --adt "${PIANO_ADT:-out/adt/piano-k27.adt}" --utc "$(date +%s)")
PIANO_RESET_AFTER=1 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-dump > out/k27/build-ram-dump.log 2>&1 &
python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram | tail -1
wait $! || { tail -3 out/k27/build-ram-dump.log; exit 1; }
