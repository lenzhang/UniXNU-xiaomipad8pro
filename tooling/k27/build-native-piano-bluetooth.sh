#!/bin/bash
# Build package/PianoBluetooth (the WCN7860's Bluetooth side over QUP2 SE6 "UART14": level 1 a read-only survey, level 2 power (the
# rails shared with PianoWifi, BT_EN over SPMI), level 3 the UART, level 4 the controller set-up with the firmware read at run time)
# as a k27 native kext: the host tests first (package/include/tests/run.sh: the shared rails helper with two simulated radios;
# tests/bt_host.cpp: the survey's gates and decoders, the clock and UART plans, H4 framing, the QCA vendor codec and the set-up
# against a simulated controller; tests/bt_bringup_host.cpp: levels 2..4 whole against register models wired to the simulated
# controller, with faults at every level, and the RSC transaction), then a store check of the survey headers, then the objects in
# out/native-kexts/PianoBluetooth for tooling/k27/kext/PianoBluetooth.json. Inert without the boot argument piano-bt; the radio is
# powered only from piano-bt=2 (docs/drivers/bluetooth.md). No firmware is in the build or the repository. Included, not copied:
# PianoPower's SpmiObserver.h and FatVolume.c, PianoKeyboard's CmdDb.h.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoBluetooth; O=$D/out/native-kexts/PianoBluetooth
INC=(-I "$P" -I "$D/package/PianoPower" -I "$D/package/PianoKeyboard")
mkdir -p "$O/host-tests"
"$D/package/include/tests/run.sh"   # PianoWcnRails.h: the rails both radios share (docs/drivers/bluetooth.md section 7.1)
for t in bt_host bt_bringup_host; do
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "${INC[@]}" -I "$D/package/include" "$P/tests/$t.cpp" -o "$O/host-tests/$t"
  "$O/host-tests/$t" || { echo "Error: PianoBluetooth host tests failed ($t)" >&2; exit 1; }
done
# The level 1 survey headers stay store-free: no str instruction and no assignment through a volatile pointer (the kext's stores are
# in PianoBluetooth.cpp; levels 2..4 write only through BtBringup.h's callbacks).
if grep -nE '"str|volatile[^;]*\*[^;]*[^=!<>]=[^=]' "$P/GeniUart.h" "$P/BtHw.h"; then
  echo "Error: the survey headers have a store" >&2; exit 1
fi
# This driver never drives the other radio's pin: no TLMM write to WLAN_EN (gpio16) anywhere in the sequences.
if grep -nE 'kPinWlanEn[^;]*write|write[^;]*kPinWlanEn' "$P/BtBringup.h" "$P/PianoBluetooth.cpp"; then
  echo "Error: PianoBluetooth writes WLAN_EN" >&2; exit 1
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoBluetooth "$P/PianoBluetooth.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  "$P/BtFirmware.c" "$D/package/PianoPower/FatVolume.c" "${INC[@]}" --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoA830's lock words (DRV-2, GCC votes) and the rails helper's two words, and NativeUfs's partition reader, come from those kexts
# (libraries in PianoBluetooth.json, both linked before PianoBluetooth in configs/k27-native-kexts.txt), resolved by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _sm8750_rpmh_drv2_owner \
  --library-prefix _sm8750_gcc_vote_owner --library-prefix _piano_wcn_ --library-prefix _native_ufs_part_ | cut -c1-160 >&2
