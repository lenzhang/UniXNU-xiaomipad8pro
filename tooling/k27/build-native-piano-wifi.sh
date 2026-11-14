#!/bin/bash
# Build package/PianoWifi (the WCN7860 Wi-Fi on PCIe0: level 1 a read-only survey, level 2 the power sequence, level 3 the PCIe0
# root complex and the link, level 4 the firmware over MHI, level 5 the firmware's QMI service, level 6 the copy engine, the host
# transport and a passive scan) as a k27 native kext: the host tests first (tests/wifi_host.cpp: the level 1 read gates, the
# decoders, the SMMU scan, the cmd-db lookup and the PMIC survey; tests/bringup_host.cpp: the rail votes, the pin and PHY sequence
# order, the link polls and their timeouts, the configuration space and BAR sizing, the MHI boot state machine;
# tests/qmi_host.cpp: the QMI codec, the router, the MHI channels and the WLAN firmware's service; tests/wmi_host.cpp: the copy
# engine's registers and rings, the transport's handshake and credits, the firmware's message interface, the radio's set-up
# before a virtual device (the hardware mode, the parameters, the regulatory event and the channel list: docs §10.27), the scan
# and its list; tests/assoc_host.cpp: level 7's messages, built and read back the way the firmware reads them (the regulatory
# rules, the channel list, a started station, a peer, its association, the keys, a management frame) —
# each with its injected faults), then the objects in out/native-kexts/PianoWifi for tooling/k27/kext/PianoWifi.json. The macOS
# side of level 6 (en1) is its own kext: tooling/k27/build-native-piano-wifi-ethernet.sh. Inert without the boot argument
# piano-wifi (docs/drivers/wifi.md). No firmware is in the build: levels 4 and 5 read it at run time from the tablet's own
# modem_a partition.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoWifi; O=$D/out/native-kexts/PianoWifi
mkdir -p "$O/host-tests"
"$D/package/include/tests/run.sh"   # PianoWcnRails.h: the rails shared with PianoBluetooth (docs/drivers/bluetooth.md section 7)
# WifiQrtr.c (the copy of PianoPower's QRTR codec) is a C source: the level 5 test links its object.
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/WifiQrtr.c" -o "$O/host-tests/WifiQrtr.o"
for t in wifi_host bringup_host qmi_host wmi_host assoc_host profile_host sta_host dp_host net_host; do
  EXTRA=; [ "$t" = qmi_host ] && EXTRA="$O/host-tests/WifiQrtr.o"
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/$t.cpp" $EXTRA -o "$O/host-tests/$t"
  "$O/host-tests/$t" || { echo "Error: PianoWifi host tests failed ($t)" >&2; exit 1; }
done
# FatVolume.c must stay the byte-for-byte copy of PianoPower's (but for its provenance line), which PianoPower never has to change.
for f in FatVolume.c FatVolume.h; do
  diff <(tail -n +4 "$P/$f") <(tail -n +2 "$D/package/PianoPower/$f") > /dev/null \
    || { echo "Error: package/PianoWifi/$f has drifted from package/PianoPower/$f" >&2; exit 1; }
done
python3 "$D/tooling/k27/build-native-kext.py" PianoWifi "$P/PianoWifi.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  "$P/WifiFirmware.c" "$P/FatVolume.c" "$P/WifiQrtr.c" "$P/WifiProfileFile.c" -I "$P" --obj "${K27_OBJ:-k27-native-obj}"
# The ladder's own frames stay well inside its 16 KiB kernel stack (boots 10c, 10d and 10d-ctl, 2026-11-14, overflowed it as level
# 6 began, with about 16 KiB of them: whole-object resets inlined into level 6, now renew() in place): at most 6 KiB from each
# thread's entry, the function-pointer targets of the I/O tables, the hooks and en0's callbacks included (an over-estimate).
IND=spmiRd,spmiWr,spmiDelay,hwRead,hwWrite,hwUdelay,hwMsleep,hwNow,hwVoteLock,hwVrm,hwPmic,hwStep,barRd,barWr,chipIoRead,chipIoWrite
IND=$IND,linkNote,staNote,staReport,staHook,wlfwClaim,wlfwSendMessage,wlfwFileSize,wlfwFileRead,wlfwMemory,wlfwStepNote,wlfwEventNote
for entry in __ZN9PianoWifi6threadEPvi __ZN9PianoWifi10stopThreadEPvi __ZN9PianoWifi10parkThreadEPvi deferPrinter portSend; do
  python3 "$D/tooling/k27/check-kext-stack.py" "$O/PianoWifi.o" "$entry" --max 6144 --brief --indirect "$IND" >&2
done
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoA830's lock words and NativeUfs's partition reader come from those kexts (libraries in PianoWifi.json, both linked before
# PianoWifi in configs/k27-native-kexts.txt), resolved by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _sm8750_rpmh_drv2_owner \
  --library-prefix _sm8750_gcc_vote_owner --library-prefix _piano_wcn_ --library-prefix _native_ufs_part_ --library-prefix _piano_spmi_words | cut -c1-160 >&2
