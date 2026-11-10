#!/bin/bash
# The USB debug test KC: the native image's kext set (configs/k27-native-kexts.txt, as tooling/k27/build-native-image.sh links
# it) plus PianoDwc3, linked to out/k27/kc-k27-usb-test.bin. Only PianoDwc3 is (re)built here; the other kexts' objects are
# used as they were last built. Never writes kc-k27-native.bin or out/ram; the RAM image is a separate step
# (docs/drivers/usb-device-debug.md section 7).
#   tooling/k27/build-usb-test-kc.sh [OUTPUT.bin]
# Environment: K27_TREE (the k27 tree, only read), K27_KERNEL (default kernel-k27-0058-native.macho), PIANO_KEXT_LIST (another
# list; its entries resolve against the list's own repository), PIANO_OUT ("@OUT@", default that repository's out/),
# PIANO_EXTRA_KEXTS (more descriptor paths).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
OUT=${1:-$D/out/k27/kc-k27-usb-test.bin}
case "$(basename "$OUT")" in kc-k27-native.bin) echo "refusing to overwrite $OUT" >&2; exit 2;; esac
mkdir -p "$(dirname "$OUT")"
LOG=${OUT%.bin}.build.log
LIST=${PIANO_KEXT_LIST:-$D/configs/k27-native-kexts.txt}
ROOT=$(cd "$(dirname "$LIST")/.." && pwd)
O=${PIANO_OUT:-$ROOT/out}
PIANO_OUT=$O tooling/k27/build-piano-dwc3.sh > "$LOG" 2>&1 || { tail -20 "$LOG"; exit 1; }
DESCS=""
if [ -f "$LIST" ]; then
  for e in $(sed -e 's/#.*//' "$LIST" | awk 'NF {print $1}'); do
    case "$e" in
      droidvm:*) DESCS="$DESCS $K/${e#droidvm:}" ;;
      piano:*) DESCS="$DESCS $ROOT/${e#piano:}" ;;
      *) DESCS="$DESCS $ROOT/tooling/k27/kext/$e.json" ;;
    esac
  done
else   # the set before configs/k27-native-kexts.txt
  for k in NativeVirtioBlock NativeAES ARMPlatformStandIn DroidVMDisplay DroidVMEthernet DroidVMInput DroidVMGpu DroidVMAudio DroidVMVideo; do
    DESCS="$DESCS $K/overlay/kext/$k.json"
  done
  DESCS="$DESCS $D/tooling/k27/kext/NativeUfs.json"
fi
DESCS="$DESCS $D/tooling/k27/kext/PianoDwc3.json ${PIANO_EXTRA_KEXTS:-}"
# "@OUT@" in a descriptor's objects path: $O, else the k27 tree's out/x12377 (the layout before 2026-11-11, where the piano build
# of NativeRamBlock was "NativeRamBlockUfs"). Resolved copies go to a temporary directory; each choice is logged.
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NATIVE=$(python3 - "$TMP" "$O" "$K/out/x12377" $DESCS 2>> "$LOG" <<'PY'
import json, os, sys
tmp, out, legacy, *descs = sys.argv[1:]
paths = []
for d in descs:
    spec = json.load(open(d))
    obj = spec.get('objects', '')
    if '@OUT@' in obj:
        name = os.path.basename(os.path.dirname(obj))
        choices = [obj.replace('@OUT@', out), obj.replace('@OUT@', legacy), os.path.join(legacy, 'native-kexts', name + 'Ufs', 'objects.json')]
        found = next((c for c in choices if os.path.exists(c)), None)
        if not found:
            sys.exit(f'{spec["name"]}: no objects at {choices[0]}: build it first (tooling/k27/build-native-*.sh)')
        spec['objects'] = found
        d = os.path.join(tmp, os.path.basename(d))
        json.dump(spec, open(d, 'w'), indent=1)
        print(f'{spec["name"]}: objects {found}', file=sys.stderr)
    paths.append(d)
print(' '.join(paths))
PY
) || { tail -5 "$LOG"; exit 1; }
echo "kexts: $NATIVE" >> "$LOG"
NATIVE="$NATIVE" tooling/kc/build-kc-k27.sh "${K27_KERNEL:-$K/out/k27/kernel-k27-0058-native.macho}" "$OUT" >> "$LOG" 2>&1 \
  || { tail -20 "$LOG"; exit 1; }
grep -E "linked, header|problem|wrote" "${OUT%.bin}.log" | cut -c1-120
tail -2 "$LOG"
