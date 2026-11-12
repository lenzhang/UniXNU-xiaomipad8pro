#!/bin/bash
# Everything the persistent boot writes, built offline into out/persistent (docs/persistent-boot.md). Nothing is sent to the tablet.
#   FD=1      first rebuild out/fd/piano-uefi-A-bundled.fd with firmware/patches/unixnu/0001 (a boot image's RAM payload wins over
#             the UFS ESP): tooling/firmware/setup-mu.sh + tooling/unixnu.sh build firmware, as tooling/build-slice1.sh does
#   ESP_PROFILE=base|softgpu (default base; build-esp.py PROFILES), ESP_ARGS="--arg NAME=VALUE ..." (more kernel arguments),
#   ESP_FROM=DIR (a checkout whose out/ holds the KC, ADT, trust cache and seed; default this one), ESP_UTC (the clock; default now)
#   RAM=1     also the dev and dump RAM images (out/ram, out/ram-dump) with this FD and the profile's inputs; once the ESP exists,
#             every RAM image must carry this FD (an older one boots the ESP's configuration instead of its own payload)
# Then: tooling/persist/sign.sh (signing host), tooling/persist/maint-session.py --dry-run ... (docs/persistent-boot.md section 5).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
M=$R/Mu-Silicium-piano
O=out/persistent
PROFILE=${ESP_PROFILE:-base}
UTC=${ESP_UTC:-$(date +%s)}
FROM=${ESP_FROM:-$D}
mkdir -p "$O" out/fd logs/fd
FD_FILE=out/fd/piano-uefi-A-bundled.fd
if [ "${FD:-0}" = 1 ]; then
  # setup-mu.sh, the build, the checks of firmware/patches/unixnu/0001 and darwinboot/0001, the stamps, the old FD as .prev
  tooling/firmware/build-fd.sh
fi
[ -f "$FD_FILE" ] || { echo "no $FD_FILE: FD=1 $0" >&2; exit 1; }
[ "$(cat "$FD_FILE.ram-payload-first" 2>/dev/null)" = "$(shasum -a 256 "$FD_FILE" | cut -c1-64)" ] \
  || { echo "$FD_FILE is not a build with firmware/patches/unixnu/0001 (no matching $FD_FILE.ram-payload-first): FD=1 $0" >&2; exit 1; }
python3 tooling/persist/build-boot.py --fd "$FD_FILE" --out "$O"
# shellcheck disable=SC2086
python3 tooling/persist/build-esp.py --from "$FROM" --profile "$PROFILE" --default macos-diag --name "esp-$PROFILE-diag" --utc "$UTC" ${ESP_ARGS:-} --out "$O"
# shellcheck disable=SC2086
python3 tooling/persist/build-esp.py --from "$FROM" --profile "$PROFILE" --default macos --name "esp-$PROFILE-macos" --utc "$UTC" ${ESP_ARGS:-} --out "$O"
python3 tooling/persist/build-gpt.py --out "$O"
if [ "${RAM:-0}" = 1 ]; then
  # the profile's inputs, as build-esp.py resolved them
  IN=$(python3 -c "import json;d=json.load(open('$O/esp-$PROFILE-diag.json'))['inputs'];print(' '.join(d[k]['path'] for k in ('kc','trust','seed','adt')))")
  set -- $IN
  COMMON=(--fd "$FD_FILE" --fd-name A-bundled --kc "$1" --trust "$2" --seed "$3" --adt "$4" --utc "$UTC")
  python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram | tail -1
  PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-dump | tail -1
fi
(cd "$O" && shasum -a 256 boot-piano-*.img kernel-field.bin "esp-$PROFILE-"*.img gpt-head.bin gpt-tail.bin) > "$O/SHA256SUMS"
cat "$O/SHA256SUMS"
