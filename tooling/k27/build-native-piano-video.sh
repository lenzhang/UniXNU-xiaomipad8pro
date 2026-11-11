#!/bin/bash
# Build package/PianoVideo (the video core, SM8750 Iris: level 1 a read-only survey; levels 2-4 the ladder's V1-V3: power on/off at
# 240 MHz, the firmware through PAS 9, the SMMU bank and the HFI handshake) as a k27 native kext: the host tests first
# (tests/video_host.cpp: level 1's gate and what is and is never loaded, the decoders, the SMMU scan; tests/bringup_host.cpp:
# levels 2-4 against one SoC model (VIDEO_CC, the core, DRV-2, cmd-db, the secure world, the SMMU, a firmware reached through our
# table) with every wait made to fail, every secure call refused and the stop argument at every step; the shared firmware loader's
# tests), the copies checked against their originals, the firmware's pin, then the objects in out/native-kexts/PianoVideo for
# tooling/k27/kext/PianoVideo.json. Inert without the boot argument piano-video; nothing is written below piano-video=2
# (docs/drivers/video.md).
# The firmware (docs/kc-space.md §6-7): tooling/persist/firmware.py stage pins PianoVideo/vpu35_4v.mbn (2409160 bytes, its
# SHA-256: xiaomipad-8pro-mainline firmware/piano-iris.json) in the kext and, when the owner's copy is on this Mac (PIANO_VIDEO_FW,
# default ../xiaomipad-8pro-mainline/firmware/qcom/sm8750/vpu35_4v.mbn next to this repository), stages it in the ignored
# out/firmware/PianoVideo/, from where build-ram-images.py puts it into a RAM image's addon and build-esp.py onto the ESP; never
# in the kext (no --embed) or in this repository. Level 3 loads it at run time (piano_fw_load). With the copy here the host tests
# also parse its headers (it is only read).
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX/CC (host
#                compilers for the tests, default c++/cc), K27_KERNEL (for the import check; skipped when absent), PIANO_VIDEO_FW
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoVideo; O=$D/out/native-kexts/PianoVideo
mkdir -p "$O/host-tests" "$O/gen"
"$D/package/include/tests/run.sh"   # PianoScmLock.h: the secure-call lock around every SCM request and the PAS 9 sequence (docs/scm-lock.md)
python3 "$D/package/common/tests/run-firmware-tests.py"   # the shared firmware loader (docs/kc-space.md §6)
# The copies stay byte for byte their originals (but for their provenance lines), which their owners never have to change.
for f in FatVolume.c FatVolume.h QcomMdt.c QcomMdt.h; do
  diff <(tail -n +4 "$P/$f") <(tail -n +2 "$D/package/PianoPower/$f") > /dev/null \
    || { echo "Error: package/PianoVideo/$f has drifted from package/PianoPower/$f" >&2; exit 1; }
done
for f in Sm8750Scm.c Sm8750Scm.h; do
  diff <(tail -n +4 "$P/$f") <(tail -n +2 "$D/package/PianoPower/$f") > /dev/null \
    || { echo "Error: package/PianoVideo/$f has drifted from package/PianoPower/$f" >&2; exit 1; }
done
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/video_host.cpp" -o "$O/host-tests/video_host"
"$O/host-tests/video_host" || { echo "Error: PianoVideo host tests failed (level 1)" >&2; exit 1; }
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/QcomMdt.c" -o "$O/host-tests/QcomMdt.o"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/bringup_host.cpp" "$O/host-tests/QcomMdt.o" -o "$O/host-tests/bringup_host"
FW=${PIANO_VIDEO_FW:-$D/../xiaomipad-8pro-mainline/firmware/qcom/sm8750/vpu35_4v.mbn}
[ -f "$FW" ] || FW=$HOME/Downloads/XiaomiPad8Pro-Ubuntu/xiaomipad-8pro-mainline/firmware/qcom/sm8750/vpu35_4v.mbn   # a worktree
VIDEO_HOST_FW=$([ -f "$FW" ] && echo "$FW" || true) "$O/host-tests/bringup_host" \
  || { echo "Error: PianoVideo host tests failed (levels 2-4)" >&2; exit 1; }
# The pin (VideoHw.h kFirmwareBytes/kFirmwareSha256 hold the same two facts; the host tests run the kernel's check with them).
python3 "$D/tooling/persist/firmware.py" stage --kext PianoVideo --header "$O/gen/PianoVideoFirmwarePins.h" \
  --source "$O/gen/PianoVideoFirmware.c" "$FW=2409160:95c5882f9b66b93ad2635604b7db563cc310481e017fa6b82e0ce6cbb9eae01b"
python3 "$D/tooling/k27/build-native-kext.py" PianoVideo "$P/PianoVideo.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  "$P/FatVolume.c" "$P/QcomMdt.c" "$P/Sm8750Scm.c" "$D/package/common/PianoFirmware.cpp" "$D/package/common/PianoFirmwareCore.c" \
  "$D/package/common/PianoFirmwareZlib.c" "$O/gen/PianoVideoFirmware.c" -I "$P" -I "$D/package/common" -I "$O/gen" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoA830's lock words (DRV-2, the secure-call lock) and NativeUfs's partition reader (the firmware loader's ESP source) come from
# those kexts (libraries in PianoVideo.json, both linked before PianoVideo in configs/k27-native-kexts.txt), resolved by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _sm8750_rpmh_drv2_owner \
  --library-prefix _sm8750_scm_owner --library-prefix _native_ufs_part_ | cut -c1-160 >&2
