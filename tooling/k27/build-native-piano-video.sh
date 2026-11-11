#!/bin/bash
# Build package/PianoVideo (the video core, SM8750 Iris: level 1 a read-only survey; levels 2-4 the ladder's V1-V3: power on/off at
# 240 MHz, the firmware through PAS 9, the SMMU bank and the HFI handshake) as a k27 native kext: the host tests first
# (tests/video_host.cpp: level 1's gate and what is and is never loaded, the decoders, the SMMU scan; tests/bringup_host.cpp:
# levels 2-4 against one SoC model (VIDEO_CC, the core, DRV-2, cmd-db, the secure world, the SMMU, a firmware reached through our
# table) with every wait made to fail, every secure call refused and the stop argument at every step; tests/esp_host.cpp: the ESP
# read on mtools FAT32 images, skipped without mtools), the copies checked against their originals, then the objects in
# out/native-kexts/PianoVideo for tooling/k27/kext/PianoVideo.json. Inert without the boot
# argument piano-video; nothing is written below piano-video=2 (docs/drivers/video.md). No firmware is in the build: level 3 reads
# the owner's vpu35_4v.mbn from the ESP at run time. When the owner's copy is on this Mac (PIANO_VIDEO_FW, default
# ../xiaomipad-8pro-mainline/firmware/qcom/sm8750/vpu35_4v.mbn next to this repository), the host tests also check it against the
# pin and parse its headers (it is only read).
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX/CC (host
#                compilers for the tests, default c++/cc), K27_KERNEL (for the import check; skipped when absent), PIANO_VIDEO_FW
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoVideo; O=$D/out/native-kexts/PianoVideo
mkdir -p "$O/host-tests"
"$D/package/include/tests/run.sh"   # PianoScmLock.h: the secure-call lock around every SCM request and the PAS 9 sequence (docs/scm-lock.md)
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
# The ESP read (VideoFirmware.h espRead over FatVolume.c) on FAT32 images formatted as build-esp.py formats the ESP: a stand-in of
# the pinned size at the kernel's path (never the real file), the file missing, a directory in its place, an empty one, no FAT.
if command -v mformat > /dev/null && command -v mmd > /dev/null && command -v mcopy > /dev/null; then
  E=$(mktemp -d); trap 'rm -rf "$E"' EXIT
  export MTOOLS_SKIP_CHECK=1
  F=EFI/UniXNU/firmware/qcom/sm8750/vpu35_4v.mbn
  head -c 2409160 /dev/urandom > "$E/stand-in.bin"; : > "$E/empty.bin"; head -c 1048576 /dev/zero > "$E/notfat.img"
  for i in ok missing directory empty; do
    mformat -C -F -S 5 -M 4096 -T 70000 -h 1 -s 1 -c 1 -i "$E/$i.img" ::
    mmd -i "$E/$i.img" ::/EFI ::/EFI/UniXNU ::/EFI/UniXNU/firmware ::/EFI/UniXNU/firmware/qcom ::/EFI/UniXNU/firmware/qcom/sm8750
  done
  mcopy -i "$E/ok.img" "$E/stand-in.bin" "::/$F"
  mmd -i "$E/directory.img" "::/$F"
  mcopy -i "$E/empty.img" "$E/empty.bin" "::/$F"
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/FatVolume.c" -o "$O/host-tests/FatVolume.o"
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/esp_host.cpp" "$O/host-tests/FatVolume.o" -o "$O/host-tests/esp_host"
  "$O/host-tests/esp_host" "$E/ok.img" "$E/stand-in.bin" "$E/missing.img" "$E/directory.img" "$E/empty.img" "$E/notfat.img" \
    || { echo "Error: PianoVideo host tests failed (the ESP read)" >&2; exit 1; }
else
  echo "PianoVideo ESP host tests: no mtools (brew install mtools): skipped"
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoVideo "$P/PianoVideo.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  "$P/FatVolume.c" "$P/QcomMdt.c" "$P/Sm8750Scm.c" -I "$P" --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoA830's lock words (DRV-2, the secure-call lock) and NativeUfs's partition reader come from those kexts (libraries in
# PianoVideo.json, both linked before PianoVideo in configs/k27-native-kexts.txt), resolved by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _sm8750_rpmh_drv2_owner \
  --library-prefix _sm8750_scm_owner --library-prefix _native_ufs_part_ | cut -c1-160 >&2
