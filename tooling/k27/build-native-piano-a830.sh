#!/bin/bash
# Build package/PianoA830 (the Adreno830 A830 kernel GPU driver with piano's board glue, docs/drivers/gpu.md) as a k27 native kext:
#   1. the board inputs: package/PianoA830/board/PianoGpuInputs.h must match piano's boot DT and the recorded live-DT values
#      (tooling/k27/gen-piano-gpu-inputs.py --check);
#   2. the firmware: piano's own gen80000 images, pin-checked and copied into the ignored out/native-kexts/PianoA830/firmware, embedded
#      from there (tooling/k27/stage-piano-gpu-firmware.py; PIANO_GPU_NO_FIRMWARE=1 builds without: S0 then refuses to start);
#   3. Adreno830's host tests (ASan/UBSan) with piano's firmware pins, upstream and ADRENO830_MACOS (tooling/k27/run-a830-host-tests.py;
#      PIANO_A830_HOST_TESTS=0 skips them);
#   4. the objects with the k27 kernel recipe (-DADRENO830_MACOS: WindowServer as the render server, macOS client lists, no staged
#      IOAcceleratorES), the weak definitions coalesced (tooling/k27/coalesce-weak.py) into out/native-kexts/PianoA830/objects.json,
#      and every import checked against the k27 kernel's symbols (tooling/k27/check-kext-imports.py);
#   5. out/k27/kext/PianoA830.json (tooling/k27/kext/PianoA830.json with @OUT@ filled in), printed last, for a test KC:
#      PIANO_EXTRA_KEXTS=$(tooling/k27/build-native-piano-a830.sh | tail -1) tooling/k27/build-native-image.sh
# Inert without the boot argument piano-gpu (1 read-only survey, 2 staged bring-up S1..S10 polled).
#   environment: K27_TREE (the k27 build tree, read only: compile recipes, kernel), K27_OBJ (default k27-native-obj), MACOS_SDK (the
#                SDK whose Kernel.framework has IOKit/graphics/IOAccelerator.h), PIANO_GPU_FW (the firmware source directory)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
P=$D/package/PianoA830; A=$P/adreno830; B=$P/board; K=$P/kits
O=$D/out/native-kexts/PianoA830
K27=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
mkdir -p "$O" "$D/out/k27/kext"
python3 "$D/tooling/k27/gen-piano-gpu-inputs.py" --check
python3 "$D/tooling/k27/stage-piano-gpu-firmware.py" ${PIANO_GPU_NO_FIRMWARE:+--no-firmware}
if [ "${PIANO_A830_HOST_TESTS:-1}" = 1 ]; then
  python3 "$D/tooling/k27/run-a830-host-tests.py" ${PIANO_GPU_NO_FIRMWARE:+--no-firmware} --out "$O/host-tests" \
    || { echo "Error: Adreno830 host tests failed ($O/host-tests/*.log)" >&2; exit 1; }
fi
SDK=${MACOS_SDK:-$(xcrun --sdk macosx --show-sdk-path)}
GFX=$D/out/k27/inc-gfx   # IOKit/graphics alone, so no other Kernel.framework header comes before xnu's own
mkdir -p "$GFX/IOKit"
ln -sfn "$SDK/System/Library/Frameworks/Kernel.framework/Headers/IOKit/graphics" "$GFX/IOKit/graphics"
[ -f "$GFX/IOKit/graphics/IOAccelerator.h" ] || { echo "no IOKit/graphics/IOAccelerator.h in $SDK" >&2; exit 1; }
# The kernel sources: Adreno830's archive lists (scripts/build-adreno830.py INHERITED, GEN8, PLATFORM and the two kits), without the
# iPadOS staged accelerator (NativeAdrenoAccelerator.cpp), plus the board and the kext glue.
INHERITED="NativeAdrenoPublish NativeAdrenoUserClient NativeAdrenoSubmit NativeAdrenoRing NativeAdrenoVm NativeAdrenoMemory NativeAdrenoCpu
  NativeAdrenoSmmuMemory NativeAdrenoSmmuContext NativeAdrenoGmuContext NativeAdrenoGpuContext NativeAdrenoGmuMemory NativeAdrenoTimer
  NativeAdrenoPwrup NativeAdrenoFirmware NativeAdrenoFirmwareDefaults NativeAdrenoSha256 NativeAdrenoKernelTime"
GEN8="Adreno830Status Adreno830Start A830Smmu A830GmuFirmware A830Gmu A830Votes A830Hfi A830Cp A830Zap A830Irq A830Fence A830SelfTest
  A830PowerState A830Recovery A830Dcvs"
SRC=()
for s in $INHERITED $GEN8; do SRC+=("$A/$s.cpp"); done
SRC+=("$A"/platform/sm8750/Sm8750{GpuPower,Rpmh,Scm,SocInfo,AossQmp,Llcc,Tsens}.c "$K/AdrenoPM4/AdrenoPM4.c" "$K"/ArmSmmu4K/ArmSmmu4K*.c)
SRC+=("$B/PianoA830.cpp" "$B/PianoA830Board.cpp" "$B/PianoA830Survey.cpp" "$D/tooling/k27/NativeKextGlue.cpp")
[ -f "$O/gen/PianoA830Firmware.c" ] && SRC+=("$O/gen/PianoA830Firmware.c")
rm -rf "$O/obj"
python3 "$D/tooling/k27/build-native-kext.py" PianoA830 "${SRC[@]}" -I "$A" -I "$A/platform/sm8750" -I "$K/ArmSmmu4K" -I "$K/AdrenoPM4" \
  -I "$B" -I "$O/gen" -I "$GFX" -D ADRENO830_MACOS --obj "${K27_OBJ:-k27-native-obj}" --out "$O/obj"
rm -f "$O"/*.o
python3 "$D/tooling/k27/coalesce-weak.py" "$O/obj/objects.json" "$O"
python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "${K27_KERNEL:-$K27/out/k27/kernel-k27-0059-native.macho}"
sed "s#@OUT@#$D/out#" "$D/tooling/k27/kext/PianoA830.json" > "$D/out/k27/kext/PianoA830.json"
echo "$D/out/k27/kext/PianoA830.json"
