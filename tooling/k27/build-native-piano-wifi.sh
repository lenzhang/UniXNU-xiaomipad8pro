#!/bin/bash
# Build package/PianoWifi (the WCN7860 Wi-Fi on PCIe0: level 1 a read-only survey, level 2 the power sequence, level 3 the PCIe0
# root complex and the link, level 4 the firmware over MHI) as a k27 native kext: the host tests first (tests/wifi_host.cpp: the
# level 1 read gates, the decoders, the SMMU scan, the cmd-db lookup and the PMIC survey; tests/bringup_host.cpp: the rail votes,
# the pin and PHY sequence order, the link polls and their timeouts, the configuration space and BAR sizing, the MHI boot state
# machine, each with its injected faults), then the objects in out/native-kexts/PianoWifi for tooling/k27/kext/PianoWifi.json.
# Inert without the boot argument piano-wifi (docs/drivers/wifi.md). No firmware is in the build: level 4 reads it at run time from
# the tablet's own modem_a partition.
#   environment: K27_TREE (the k27 build tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
P=$D/package/PianoWifi; O=$D/out/native-kexts/PianoWifi
mkdir -p "$O/host-tests"
for t in wifi_host bringup_host; do
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/$t.cpp" -o "$O/host-tests/$t"
  "$O/host-tests/$t" || { echo "Error: PianoWifi host tests failed ($t)" >&2; exit 1; }
done
# FatVolume.c must stay the byte-for-byte copy of PianoPower's (but for its provenance line), which PianoPower never has to change.
for f in FatVolume.c FatVolume.h; do
  diff <(tail -n +4 "$P/$f") <(tail -n +2 "$D/package/PianoPower/$f") > /dev/null \
    || { echo "Error: package/PianoWifi/$f has drifted from package/PianoPower/$f" >&2; exit 1; }
done
python3 "$D/tooling/k27/build-native-kext.py" PianoWifi "$P/PianoWifi.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  "$P/WifiFirmware.c" "$P/FatVolume.c" -I "$P" --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" | cut -c1-160 >&2
