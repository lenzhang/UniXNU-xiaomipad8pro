#!/bin/bash
# Build package/PianoThermal (SoC temperatures from the four TSENS controllers, the thermal protection policy, the CPU clusters'
# frequency over SCMI) as a k27 native kext: the host tests first (tests/thermal_host.cpp: the TSENS v2 decode and the sensor map,
# the poll against a register model, the policy state machine with its hysteresis, the SCMI transport and perf protocol against a
# model of the CPUCP, the governor), then the objects in out/native-kexts/PianoThermal for tooling/k27/kext/PianoThermal.json.
# Inert without the boot arguments piano-thermal / piano-cpufreq (docs/drivers/thermal.md). When the piano DT (PIANO_DTB, default
# out/inputs/piano-boot.dtb) and dtc are there, Tsens.h's sensor map is also checked against its thermal zones
# (tooling/k27/check-thermal-map.py).
#   environment: K27_TREE (the k27 build tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
P=$D/package/PianoThermal; O=$D/out/native-kexts/PianoThermal
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/thermal_host.cpp" -o "$O/host-tests/thermal_host"
"$O/host-tests/thermal_host" || { echo "Error: PianoThermal host tests failed" >&2; exit 1; }
DTB=${PIANO_DTB:-$D/out/inputs/piano-boot.dtb}
if [ -f "$DTB" ] && command -v dtc > /dev/null; then
  "$O/host-tests/thermal_host" --map > "$O/host-tests/map.txt"
  python3 "$D/tooling/k27/check-thermal-map.py" "$DTB" "$O/host-tests/map.txt" \
    || { echo "Error: Tsens.h's sensor map differs from the DT's thermal zones" >&2; exit 1; }
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoThermal "$P/PianoThermal.cpp" "$D/tooling/k27/NativeKextGlue.cpp" -I "$P" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# The GPU hooks (adreno830_dcvs_park, adreno830_thermal_state) come from PianoA830, a library of the descriptor, resolved by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _adreno830_dcvs_park \
  --library-prefix _adreno830_thermal_state | cut -c1-160 >&2
