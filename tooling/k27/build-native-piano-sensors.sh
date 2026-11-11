#!/bin/bash
# Build package/PianoSensors (the ADSP sensor hub: level 1 a read-only survey, level 2 the FastRPC reverse file service from the
# owner's sensor pack on the ESP, level 3 the SNS client's SUID lookups and attributes) as a k27 native kext: the host tests first
# (tests/run-host-tests.py: the protocol codecs, the guard, the policies, the client against a simulated hub, the pack loader on an
# mtools FAT32 image, the kernel's no-FP/no-libc compile), the copies checked against their originals, then the objects in
# out/native-kexts/PianoSensors for tooling/k27/kext/PianoSensors.json. Inert without the boot argument piano-sensors
# (docs/drivers/sensors.md). No pack, registry or vendor file is in the build: level 2 reads the owner's pack from the ESP at run time.
#   environment: K27_TREE (the k27 build tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CC (host compiler
#                for the tests, default cc), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
P=$D/package/PianoSensors; O=$D/out/native-kexts/PianoSensors
mkdir -p "$O"
python3 "$P/tests/run-host-tests.py" > "$O/host-tests.log" 2>&1 \
  || { tail -20 "$O/host-tests.log" >&2; echo "Error: PianoSensors host tests failed" >&2; exit 1; }
tail -1 "$O/host-tests.log"
# The copies stay byte for byte their originals (but for their provenance lines), which their owners never have to change.
for f in FatVolume.c FatVolume.h; do
  diff <(tail -n +4 "$P/$f") <(tail -n +2 "$D/package/PianoPower/$f") > /dev/null \
    || { echo "Error: package/PianoSensors/$f has drifted from package/PianoPower/$f" >&2; exit 1; }
done
diff <(tail -n +4 "$P/SmmuStream.h" | sed 's/namespace pianosensors/namespace pianousb/') <(tail -n +2 "$D/package/PianoDwc3/SmmuStream.h") \
  > /dev/null || { echo "Error: package/PianoSensors/SmmuStream.h has drifted from package/PianoDwc3/SmmuStream.h" >&2; exit 1; }
python3 "$D/tooling/k27/build-native-kext.py" PianoSensors "$P/PianoSensors.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  "$P/SensorsRpc.c" "$P/SensorsPack.c" "$P/SensorsSsc.c" "$P/SensorsPb.c" "$P/SensorsClient.c" "$P/SensorsGuard.c" \
  "$P/SensorsPolicy.c" "$P/SensorsFixed.c" "$P/SensorsOrient.c" "$P/SensorsLoad.c" "$P/FatVolume.c" -I "$P" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoPower's client contract (PianoAdspContract.h) and NativeUfs's partition reader come from those kexts (libraries in
# PianoSensors.json, both linked before PianoSensors in configs/k27-native-kexts.txt), resolved by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _piano_adsp_ \
  --library-prefix _piano_qrtr_ --library-prefix _native_ufs_part_ | cut -c1-160 >&2
