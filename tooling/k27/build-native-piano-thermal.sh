#!/bin/bash
# Build package/PianoThermal (SoC temperatures from the four TSENS controllers, the thermal protection policy, the CPU clusters'
# frequency over SCMI) as a k27 native kext: the host tests first (tests/thermal_host.cpp: the TSENS v2 decode and the sensor map,
# the poll against a register model, the policy state machine with its hysteresis, the SCMI transport and perf protocol against a
# model of the CPUCP, the governor), then the objects in out/native-kexts/PianoThermal for tooling/k27/kext/PianoThermal.json.
# Inert without the boot arguments piano-thermal / piano-cpufreq (docs/drivers/thermal.md). When the piano DT (PIANO_DTB, default
# out/inputs/piano-boot.dtb) and dtc are there, Tsens.h's sensor map is also checked against its thermal zones
# (tooling/k27/check-thermal-map.py). B6 (the board thermistors on the PMIC ADC, piano-thermal-adc): tests/adc_host.cpp (the NTC
# curve, the ADC5 gen3 conversion and its error paths against a model of the SPMI arbiter, SDAM and PBS, the board-temperature
# API), and with the DT AdcChannels.h's map against the ADC node (tooling/k27/check-adc-map.py); PianoThermalAdc.cpp is the kext's
# second object. The uplow interrupt (piano-thermal-irq, docs/drivers/thermal.md section 13): package/common's PianoIrq.h tests,
# tests/uplow_host.cpp (TsensIrq.h against a model of the controllers and a level line), with the DT TsensIrq.h's SPI table against
# the TSENS nodes (tooling/k27/check-tsens-irq.py); PianoThermalIrq.cpp is the third object.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoThermal; O=$D/out/native-kexts/PianoThermal
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/thermal_host.cpp" -o "$O/host-tests/thermal_host"
"$O/host-tests/thermal_host" || { echo "Error: PianoThermal host tests failed" >&2; exit 1; }
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/adc_host.cpp" -o "$O/host-tests/adc_host"
"$O/host-tests/adc_host" || { echo "Error: PianoThermal ADC host tests failed" >&2; exit 1; }
# piano-thermal-irq (TsensIrq.h, PianoThermalIrq.cpp): PianoIrq.h's guards, then the uplow interrupt against a controller model
"$D/package/common/tests/run.sh" >&2
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/uplow_host.cpp" -o "$O/host-tests/uplow_host"
"$O/host-tests/uplow_host" || { echo "Error: PianoThermal uplow interrupt host tests failed" >&2; exit 1; }
DTB=${PIANO_DTB:-$D/out/inputs/piano-boot.dtb}
if [ -f "$DTB" ] && command -v dtc > /dev/null; then
  "$O/host-tests/thermal_host" --map > "$O/host-tests/map.txt"
  python3 "$D/tooling/k27/check-thermal-map.py" "$DTB" "$O/host-tests/map.txt" \
    || { echo "Error: Tsens.h's sensor map differs from the DT's thermal zones" >&2; exit 1; }
  "$O/host-tests/adc_host" --map > "$O/host-tests/adc-map.txt"
  python3 "$D/tooling/k27/check-adc-map.py" "$DTB" "$O/host-tests/adc-map.txt" \
    || { echo "Error: AdcChannels.h's map differs from the DT's ADC node" >&2; exit 1; }
  "$O/host-tests/uplow_host" --map > "$O/host-tests/uplow-map.txt"
  python3 "$D/tooling/k27/check-tsens-irq.py" "$DTB" "$O/host-tests/uplow-map.txt" \
    || { echo "Error: TsensIrq.h's interrupt table differs from the DT's TSENS nodes" >&2; exit 1; }
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoThermal "$P/PianoThermal.cpp" "$P/PianoThermalAdc.cpp" "$P/PianoThermalIrq.cpp" \
  "$D/tooling/k27/NativeKextGlue.cpp" -I "$P" --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# The GPU hooks (adreno830_dcvs_park, adreno830_thermal_state) come from PianoA830, a library of the descriptor, resolved by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _adreno830_dcvs_park \
  --library-prefix _adreno830_thermal_state --library-prefix _piano_spmi_words | cut -c1-160 >&2
