#!/bin/bash
# Build package/PianoPower (battery and charging: the fuel gauges, the charger PMIC's port state, the audio DSP's charger firmware) as a
# k27 native kext: the host tests first (tests/power_host.cpp: the PMIC RTC readout and its date math (PmicRtc.h, RtcReadout.h), the
# charger PMIC discovery, the charger watchdog's gates, pets and clear (ChargerWdog.h) and the charging enable command's gates, set,
# pulse and register record (ChargerEnable.h) over an SPMI model, the software I2C bus against a bq27z561 model, the MCA codec and charging policy against a
# model of the DSP, and ChargePolicy wired to the codec (ChargeLink.h, piano-power-policy=1) against the same model; tests/policy_host.cpp:
# ChargePolicy.c alone against a firmware and source model; tests/port_events_host.cpp: the USB-C port events' fan-out), then the
# objects in out/native-kexts/PianoPower for tooling/k27/kext/PianoPower.json. Inert without the boot argument piano-power
# (docs/drivers/charging.md).
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CC/CXX (host compilers
#                for the tests, default cc/c++)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
P=$D/package/PianoPower
T=$D/out/native-kexts/PianoPower/host-tests; mkdir -p "$T"
"$D/package/include/tests/run.sh"   # PianoScmLock.h: the secure-call lock around every SCM request and PAS sequence (docs/scm-lock.md)
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/PianoMca.c" -o "$T/PianoMca.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/ChargePolicy.c" -o "$T/ChargePolicy.o"   # the charging policy (charging-policy.md)
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/policy_host.cpp" "$T/ChargePolicy.o" -o "$T/policy_host"
"$T/policy_host" || { echo "Error: PianoPower charging-policy host tests failed" >&2; exit 1; }
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/QcomQrtr.c" -o "$T/QcomQrtr.o"   # the QMI helpers Servreg.h uses
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$D/package/include" -c "$P/QrtrPorts.c" -o "$T/QrtrPorts.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/QcomGlink.c" -o "$T/QcomGlink.o"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/power_host.cpp" "$T/PianoMca.o" "$T/ChargePolicy.o" \
  "$T/QcomQrtr.o" -o "$T/power_host"
"$T/power_host" || { echo "Error: PianoPower host tests failed" >&2; exit 1; }
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/battery_host.cpp" -o "$T/battery_host"   # BatteryMap.h (piano-power-ps)
"$T/battery_host" || { echo "Error: PianoPower battery host tests failed" >&2; exit 1; }
# The DSP client contract (package/include/PianoAdspContract.h): the QRTR port table against the router codec, the health pieces and
# the quiet flag (QrtrPorts.c, AdspHealth.h), the charger codec's "configured", GLINK's late registration (QcomGlink.c).
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/ports_host.cpp" "$T/QrtrPorts.o" "$T/QcomQrtr.o" \
  "$T/PianoMca.o" "$T/QcomGlink.o" -o "$T/ports_host"
"$T/ports_host" || { echo "Error: PianoPower client-contract host tests failed" >&2; exit 1; }
# The USB-C port events' read-only fan-out (PortEvents.c, the contract's "USB-C port events"; PianoDp's D2): registration, delivery,
# and the charger codec's side of the hook.
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DPIANO_PORT_EVENTS_TEST -I "$D/package/include" -c "$P/PortEvents.c" -o "$T/PortEvents.o"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/port_events_host.cpp" "$T/PortEvents.o" \
  "$T/PianoMca.o" -o "$T/port_events_host"
"$T/port_events_host" || { echo "Error: PianoPower port-event host tests failed" >&2; exit 1; }
# The USB-C relay (package/include/PianoUsbcContract.h, UsbcRelay.c): the requests a client may send, the queue, the ready transitions,
# whose a received message is, and PianoPower's order of the relay and the charger codec.
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$D/package/include" -c "$P/UsbcRelay.c" -o "$T/UsbcRelay.o"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/usbc_host.cpp" "$T/UsbcRelay.o" "$T/PianoMca.o" \
  -o "$T/usbc_host"
"$T/usbc_host" || { echo "Error: PianoPower USB-C relay host tests failed" >&2; exit 1; }
# The kernel's calendar uses the same arithmetic: k27 0060 adds PmicRtc.h unchanged as osfmk/arm64/piano_rtc.h.
for p in "$D"/patches/k27/0060-*.patch; do
  [ -f "$p" ] || continue
  awk '/^\+\+\+ b\/osfmk\/arm64\/piano_rtc.h/{f=1;next} f&&/^@@/{next} f&&/^(diff --git|-- $)/{f=0} f{sub(/^\+/,"");print}' "$p" \
    | cmp -s - "$P/PmicRtc.h" || { echo "Error: $(basename "$p")'s osfmk/arm64/piano_rtc.h is not package/PianoPower/PmicRtc.h" >&2; exit 1; }
done
python3 "$D/tooling/k27/build-native-kext.py" PianoPower "$P/PianoPower.cpp" "$P/PianoAdsp.cpp" "$P/PianoBattery.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  "$P/PianoMca.c" "$P/ChargePolicy.c" "$P/PianoQrtr.c" "$P/PianoAdspFirmware.c" "$P/QcomSmem.c" "$P/QcomSmp2p.c" "$P/QcomGlink.c" "$P/QcomQmp.c" \
  "$P/QcomRpmh.c" "$P/QcomMdt.c" "$P/QcomQrtr.c" "$P/QrtrPorts.c" "$P/PortEvents.c" "$P/FatVolume.c" "$P/Sm8750Scm.c" "$P/UsbcRelay.c" \
  "$P/PianoUsbcRelay.cpp" \
  -I "$P" --obj "${K27_OBJ:-k27-native-obj}"
# The imports the KC resolves from the libraries PianoPower.json names: NativeUfs's partition reads, PianoA830's DRV-2 and secure-call
# lock words (package/include/PianoScmLock.h, docs/scm-lock.md); anything else the kernel must define.
. "$D/tooling/k27/k27-tree.sh"
KERNEL=${K27_KERNEL:-$PIANO_K27_TREE/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$D/out/native-kexts/PianoPower/objects.json" "$KERNEL" \
  --library-prefix _native_ufs_part_ --library-prefix _sm8750_rpmh_drv2_owner --library-prefix _sm8750_scm_owner --library-prefix _piano_spmi_words | cut -c1-160 >&2
