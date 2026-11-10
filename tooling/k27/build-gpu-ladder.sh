#!/bin/bash
# The piano-gpu=2 bring-up ladder (docs/drivers/gpu.md section 8): one macOS RAM image per piano-gpu-stop=N of the current KC, which
# must have PianoA830 linked (PIANO_EXTRA_KEXTS=$(tooling/k27/build-native-piano-a830.sh | tail -1) tooling/k27/build-native-image.sh),
# into out/ram-gpu/s<N>/ (and out/ram-gpu/full/ without a stop). Only the kernel arguments differ between the images.
#   environment: PIANO_GPU_STOPS (default "1 2 3 4 5 6 7 8 9 full"), PIANO_GPU_ARGS (more arguments, default "piano-usb=2"),
#                PIANO_RESET_AFTER (default 45), PIANO_TRUST / PIANO_SEED / PIANO_ADT (as tooling/k27/build-native-image.sh)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
grep -q '^com.unixnu.driver.PianoA830: linked' out/k27/kc-k27-native.log \
  || { echo "out/k27/kc-k27-native.bin has no PianoA830: build it with PIANO_EXTRA_KEXTS first" >&2; exit 1; }
for s in ${PIANO_GPU_STOPS:-1 2 3 4 5 6 7 8 9 full}; do
  args="piano-gpu=2"
  [ "$s" = full ] || args="$args piano-gpu-stop=$s"
  dir=out/ram-gpu/$([ "$s" = full ] && echo full || echo "s$s")
  PIANO_EXTRA_ARGS="$args ${PIANO_GPU_ARGS-piano-usb=2}" python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-A-bundled.fd \
    --fd-name A-bundled --kc out/k27/kc-k27-native.bin --trust "${PIANO_TRUST:-out/inputs/kernel.macho.trustcache}" \
    --seed "${PIANO_SEED:-out/inputs/scratch-data.img}" --adt "${PIANO_ADT:-out/adt/piano-k27.adt}" --utc "$(date +%s)" --out "$dir" \
    > "$dir.log" 2>&1 || { tail -3 "$dir.log"; exit 1; }
  echo "$dir/boot-piano-macos-ram-A-bundled.img ($args): $(shasum -a 256 "$dir/boot-piano-macos-ram-A-bundled.img" | cut -c1-16)"
done
