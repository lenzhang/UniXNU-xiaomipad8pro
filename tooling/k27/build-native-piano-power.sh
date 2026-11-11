#!/bin/bash
# Build package/PianoPower (battery and charging: the fuel gauges, the charger PMIC's port state, the audio DSP's charger firmware) as a
# k27 native kext: the host tests first (tests/power_host.cpp: the PMIC RTC readout and its date math (PmicRtc.h, RtcReadout.h), the
# charger PMIC discovery over an SPMI model, the software I2C bus against a bq27z561 model, the MCA codec and charging policy against a
# model of the DSP), then the objects in out/native-kexts/PianoPower
# for tooling/k27/kext/PianoPower.json. Inert without the boot argument piano-power (docs/drivers/charging.md).
#   environment: K27_TREE (the k27 build tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CC/CXX (host compilers
#                for the tests, default cc/c++)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
P=$D/package/PianoPower
T=$D/out/native-kexts/PianoPower/host-tests; mkdir -p "$T"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/PianoMca.c" -o "$T/PianoMca.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/QcomQrtr.c" -o "$T/QcomQrtr.o"   # the QMI helpers Servreg.h uses
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/power_host.cpp" "$T/PianoMca.o" "$T/QcomQrtr.o" -o "$T/power_host"
"$T/power_host" || { echo "Error: PianoPower host tests failed" >&2; exit 1; }
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/battery_host.cpp" -o "$T/battery_host"   # BatteryMap.h (piano-power-ps)
"$T/battery_host" || { echo "Error: PianoPower battery host tests failed" >&2; exit 1; }
# The kernel's calendar uses the same arithmetic: k27 0060 adds PmicRtc.h unchanged as osfmk/arm64/piano_rtc.h.
for p in "$D"/patches/k27/0060-*.patch; do
  [ -f "$p" ] || continue
  awk '/^\+\+\+ b\/osfmk\/arm64\/piano_rtc.h/{f=1;next} f&&/^@@/{next} f&&/^(diff --git|-- $)/{f=0} f{sub(/^\+/,"");print}' "$p" \
    | cmp -s - "$P/PmicRtc.h" || { echo "Error: $(basename "$p")'s osfmk/arm64/piano_rtc.h is not package/PianoPower/PmicRtc.h" >&2; exit 1; }
done
python3 "$D/tooling/k27/build-native-kext.py" PianoPower "$P/PianoPower.cpp" "$P/PianoAdsp.cpp" "$P/PianoBattery.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  "$P/PianoMca.c" "$P/PianoQrtr.c" "$P/PianoAdspFirmware.c" "$P/QcomSmem.c" "$P/QcomSmp2p.c" "$P/QcomGlink.c" "$P/QcomQmp.c" \
  "$P/QcomRpmh.c" "$P/QcomMdt.c" "$P/QcomQrtr.c" "$P/FatVolume.c" "$P/Sm8750Scm.c" \
  -I "$P" --obj "${K27_OBJ:-k27-native-obj}"
