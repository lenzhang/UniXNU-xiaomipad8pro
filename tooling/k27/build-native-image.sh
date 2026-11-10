#!/bin/bash
# One command after a NativeUfs/NativeRamBlock or k27 change: native kexts -> KC -> the macOS RAM image (FD A) in out/ram, and the
# loop's dump image in out/ram-dump (the same with piano-reset-after=1: it only lets our UEFI read the previous run's log).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
tooling/k27/build-native-ufs.sh > out/k27/build-native-ufs.log 2>&1 || { grep error: out/k27/build-native-ufs.log | head; exit 1; }
tooling/k27/build-native-ram-block.sh > out/k27/build-native-ram-block.log 2>&1 || { grep error: out/k27/build-native-ram-block.log | head; exit 1; }
# The linked set: configs/k27-native-kexts.txt, plus PIANO_EXTRA_KEXTS (absolute JSON paths) for a test.
NATIVE=$(sed -e 's/#.*//' configs/k27-native-kexts.txt | awk 'NF {print $1}' | sed -e "s#^droidvm:#$K/#" -e "s#^piano:#$D/#" | tr '\n' ' ')
NATIVE="$NATIVE ${PIANO_EXTRA_KEXTS:-}" \
  tooling/kc/build-kc-k27.sh "${K27_KERNEL:-$K/out/k27/kernel-k27-0058-native.macho}" out/k27/kc-k27-native.bin > out/k27/build-kc.log 2>&1 || { tail -5 out/k27/build-kc.log; exit 1; }
COMMON=(--fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin --trust out/inputs/kernel.macho.trustcache
        --seed out/inputs/scratch-data.img --adt "${PIANO_ADT:-out/adt/piano-k27.adt}" --utc "$(date +%s)")
PIANO_RESET_AFTER=1 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-dump > out/k27/build-ram-dump.log 2>&1 &
python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram | tail -1
wait $! || { tail -3 out/k27/build-ram-dump.log; exit 1; }
