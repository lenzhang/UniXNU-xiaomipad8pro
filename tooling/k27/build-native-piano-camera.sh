#!/bin/bash
# Build package/PianoCamera (piano's two cameras; level 1 a read-only survey, level 2 each sensor powered, its chip ID read and the
# sensor powered down again; docs/drivers/camera.md) as a k27 native kext: the sensor tables' tool self-test and data/camera checked
# against its MANIFEST, the tables' C header generated into out/ (the host tests read it; the kext from level 3 on), the host tests
# (tests/camera_host.cpp: a board model with the bus gates, CCI0, bit-level sensors, PMIC/RPMh; every power-down back to the state
# found), a store check of the survey headers, the objects in out/native-kexts/PianoCamera for tooling/k27/kext/PianoCamera.json,
# the import check. Inert without the boot argument piano-camera. PianoPower's SPMI observer and PianoKeyboard's cmd-db header are
# included, not copied.
#   environment: PIANO_K27_TREE (the k27 tree, read only), K27_OBJ (default k27-native-obj), CXX (host compiler, default c++),
#                K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoCamera; O=$D/out/native-kexts/PianoCamera; G=$O/gen
INC=(-I "$P" -I "$D/package/PianoPower" -I "$D/package/PianoKeyboard")
mkdir -p "$O/host-tests" "$G"
# The tables: the tool's own test, then data/camera as pinned (a changed or truncated table stops the build), then the header.
"$D/package/include/tests/run.sh"   # the shared protocols (PianoMmVotes.h: the MXC/MMCX/MM1 votes this kext shares with PianoVideo)
python3 "$D/tooling/camera/sensor-tables.py" --self-test
python3 "$D/tooling/camera/sensor-tables.py" check "$D/data/camera" || { echo "Error: data/camera does not match its MANIFEST" >&2; exit 1; }
python3 "$D/tooling/camera/sensor-tables.py" header "$D/data/camera" "$G/CameraTables.h"
# (-O1: level 3's model writes whole 16 MiB frames)
"${CXX:-c++}" -std=c++20 -O1 -Wall -Wextra -Werror "${INC[@]}" -I "$D/package/include" -I "$G" "$P/tests/camera_host.cpp" \
  -o "$O/host-tests/camera_host"
PIANO_CAMERA_MANIFEST="$D/data/camera/MANIFEST" "$O/host-tests/camera_host" || { echo "Error: PianoCamera host tests failed" >&2; exit 1; }
# The survey headers hold no store: no str instruction and no assignment through a volatile pointer (level 1's only store is the SPMI
# observer's read command, in PianoCamera.cpp). Level 2 stores only through CameraPower.h's put()/putQuiet() (the allowlist).
if grep -nE '"str|volatile[^;]*\*[^;]*[^=!<>]=[^=]' "$P/CameraHw.h" "$P/CameraSurvey.h"; then
  echo "Error: a survey header has a store" >&2; exit 1
fi
for f in CameraPower.h CameraStream.h; do   # level 3 likewise through CameraStream.h's put()/putQuiet() (its own allowlist)
  if grep -nE 'ops\.wr(Quiet)?\(' "$P/$f" | grep -vE '^[0-9]+:    ops\.wr(Quiet)?\(pa, v(alue)?(, what)?\);$'; then
    echo "Error: $f stores outside put()/putQuiet()" >&2; exit 1
  fi
done
# The SMMU route's headers store only through the Write they are given (level 3 routes it through put()).
if grep -nE 'asm|"str[ \\]|volatile[^;]*\*[^;]*[^=!<>]=[^=]|ops\.' "$P/CameraSmmu.h" "$P/CameraSmmuWindow.h"; then
  echo "Error: an SMMU header stores other than through its Write" >&2; exit 1
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoCamera "$P/PianoCamera.cpp" "$D/tooling/k27/NativeKextGlue.cpp" "${INC[@]}" -I "$G" \
  --obj "${K27_OBJ:-k27-native-obj}"
# The camera thread's own frames stay well inside its 16 KiB kernel stack (c3rp 2026-11-13 overflowed it with 15 KiB of them; XNU's
# IOKit mapping path below needs about 4 KiB): at most 6 KiB from its entry, the function-pointer targets of Loader/Pmic/RscIo/
# PianoMmVotesOps/the SPMI Io included.
python3 "$D/tooling/k27/check-kext-stack.py" "$O/PianoCamera.o" __ZN11PianoCamera6threadEPvi --max 6144 --brief \
  --indirect loaderRd,pmicApid,pmicRead,cmdDbLookup,rscRd,rscWr,rscSleep,mmAddress,mmLevel,mmSend,mmSleep,mmReport,ioRd,ioWr,ioDelay >&2
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoA830's lock words (DRV-2, GCC votes) and the shared multimedia votes' words come from that kext (a library in
# PianoCamera.json, linked before PianoCamera in configs/k27-native-kexts.txt), resolved by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _sm8750_rpmh_drv2_owner \
  --library-prefix _sm8750_gcc_vote_owner --library-prefix _piano_mm_votes | cut -c1-160 >&2
