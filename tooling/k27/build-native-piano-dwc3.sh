#!/bin/bash
# Build package/PianoDwc3 (USB device-mode debug channel: CDC-ACM console, user pipe, with piano-usb-hid=1 the input port and its
# keyboard/pointer HID devices, with piano-usb-net=1 the CDC-NCM network function and its PianoUsbNet Ethernet controller;
# docs/drivers/usb-device-debug.md) as a k27 native kext: the host tests first (package/PianoDwc3/tests/run.sh), then the objects in
# out/native-kexts/PianoDwc3 for tooling/k27/kext/PianoDwc3.json. Inert without the boot argument piano-usb.
#   environment: K27_TREE (the k27 build tree, read only: compile recipes, out/x12377/inc-hid, the IOKit/hid headers PianoInput and
#                PianoKeyboard build with, and out/x12377/inc-net-piano, the IOKit/network headers PianoNullEthernet builds with),
#                K27_OBJ (default k27-native-obj), K27_KERNEL (for the import check)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
HID=$K/out/x12377/inc-hid NET=$K/out/x12377/inc-net-piano
[ -f "$HID/IOKit/hid/IOHIDDevice.h" ] || { echo "Error: no IOKit/hid headers at $HID" >&2; exit 1; }
[ -f "$NET/IOKit/network/IOEthernetController.h" ] || { echo "Error: no IOKit/network headers at $NET" >&2; exit 1; }
"$D/package/common/tests/run.sh"      # PianoIrq.h: the interrupt helpers' facts and state machines
"$D/package/PianoDwc3/tests/run.sh"   # host tests: gadget core (+ NCM), NTB16, TX FIFO plan, SMMU planner, input parser, wait tiers
python3 "$D/tooling/k27/build-native-kext.py" PianoDwc3 "$D/package/PianoDwc3/PianoDwc3.cpp" "$D/package/PianoDwc3/PianoUsbNet.cpp" \
  "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$D/package/PianoDwc3" -I "$D/package/PianoKeyboard" -I "$D/package/PianoBacklight" -I "$HID" -I "$NET" -I "$K/out/x12377/inc-gfx" -I "$K/out/x12377/storage-abi/include" \
  -D U:TARGET_OS_OSX -D TARGET_OS_OSX=1 --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# IOHIDDevice's and IONetworkingFamily's symbols come from IOHIDFamily and IONetworkingFamily (libraries of the descriptor), resolved
# by the KC link.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$D/out/native-kexts/PianoDwc3/objects.json" "$KERNEL" \
  --library-prefix __ZN11IOHIDDevice --library-prefix __ZNK11IOHIDDevice --library-prefix __ZTV11IOHIDDevice \
  --library-prefix __ZN20IOEthernetController --library-prefix __ZNK20IOEthernetController --library-prefix __ZTV20IOEthernetController \
  --library-prefix __ZN19IONetworkController --library-prefix __ZNK19IONetworkController --library-prefix __ZN15IONetworkMedium \
  --library-prefix __ZN18IONetworkInterface --library-prefix __ZN19IOEthernetInterface | cut -c1-160
# The KC builder checks one vtable per descriptor ("vtable": PianoUsbHID's, at every link). The descriptor's "extra_vtables"
# (PianoUsbNet against IOEthernetController) are checked here, each by a link of PianoDwc3 alone (a few seconds) into
# out/k27/kext-check, removed again; a problem slot fails the build. Skipped without the kernel or the KC base.
if [ -f "$KERNEL" ] && [ -f "${KC_BASE:-$K/out/kc/kc27.bin}" ]; then
  C=$D/out/k27/kext-check
  mkdir -p "$C"
  for cls in $(python3 - "$D" "$C" <<'EOF'
import json, sys
from pathlib import Path
repo, out = Path(sys.argv[1]), Path(sys.argv[2])
spec = json.loads((repo / 'tooling/k27/kext/PianoDwc3.json').read_text().replace('@OUT@', str(repo / 'out')))
for extra in spec.get('extra_vtables', []):
    (out / f'PianoDwc3-{extra["class"]}.json').write_text(json.dumps(dict(spec, vtable=extra), indent=1))
    print(extra['class'])
EOF
  ); do
    NATIVE="$C/PianoDwc3-$cls.json" "$D/tooling/kc/build-kc-k27.sh" "$KERNEL" "$C/kc-$cls.bin" > "$C/kc-$cls.out" 2>&1 || { tail -5 "$C/kc-$cls.out"; exit 1; }
    line=$(grep -h "vtable check $cls vs" "$C/kc-$cls.log" "$C/kc-$cls.out" | head -1)
    rm -f "$C/kc-$cls.bin" "$C/kc-$cls.nm"
    echo "${line:-vtable check $cls: no result}"
    case $line in *": 0 problem slots") ;; *) grep -h "vtable slot" "$C/kc-$cls.log" | head; exit 1 ;; esac
  done
fi
