#!/bin/bash
# Run the pinned UniXNU checkout's CLI on this device with the pinned local checkouts of unixnu.lock (no network fetch).
#   tooling/unixnu.sh <unixnu command> [args...]     e.g. tooling/unixnu.sh build firmware
# Environment: UNIXNU_RESEARCH (default ~/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research), the directory holding the
# checkouts named in unixnu.lock (UniXNU, UniXNU-sm8750-common, Mu-Silicium-piano, piano-uefi).
set -euo pipefail
D=$(cd "$(dirname "$0")/.." && pwd)
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
U=$R/UniXNU
cmd=$1; shift
extra=()
case $cmd in
  build|firmware|bootimg|release) extra=(--uefi "$R/Mu-Silicium-piano" --edk2 "$U/out/feeds/mu_basecore-8f10084c") ;;
esac
# UniXNU's own submodule worktrees are empty in this checkout (only their .git files survived the 2026-11-10 history
# rewrite); the standalone checkouts next to it are the same commits (unixnu.lock), so they stand in as local modules.
for m in AppleKeyStoreState ArmCpu ArmDTPlatform GopFramebuffer RamBlock XnuPrelink AirNir ArmSmmu4K AdrenoFirmware AdrenoPM4; do
  [ -d "$R/$m/.git" ] && extra+=(--local-module "$m=$R/$m")
done
# DarwinBoot with this repository's deltas (firmware/patches/darwinboot/: the RAM window up to 13 GiB): a copy of the pinned
# checkout in out/deps/DarwinBoot with the patches applied, remade from the checkout on every run (replaced only when its content
# changes, so EDK II rebuilds it only then); the shared checkout itself stays the pin of unixnu.lock.
if [ -d "$R/DarwinBoot/.git" ]; then
  db=$D/out/deps/DarwinBoot
  pin=$(python3 -c "import tomllib;print(tomllib.load(open('$D/unixnu.lock','rb'))['pin']['DarwinBoot']['commit'])")
  head=$(cat "$R/DarwinBoot/.git/$(sed -n 's/^ref: //p' "$R/DarwinBoot/.git/HEAD")" 2>/dev/null || cat "$R/DarwinBoot/.git/HEAD")
  [ "$head" = "$pin" ] || echo "tooling/unixnu.sh: $R/DarwinBoot is at $head, unixnu.lock pins $pin" >&2
  rm -rf "$db.new"; mkdir -p "$db.new"
  (cd "$R/DarwinBoot" && tar --exclude .git -cf - .) | tar -xf - -C "$db.new"
  for p in "$D"/firmware/patches/darwinboot/*.patch; do
    [ -e "$p" ] || continue
    patch -p1 -s -N -d "$db.new" -i "$p" || { echo "tooling/unixnu.sh: $p does not apply to DarwinBoot $head" >&2; exit 1; }
  done
  if [ -d "$db" ] && diff -r -q "$db" "$db.new" > /dev/null; then rm -rf "$db.new"; else rm -rf "$db"; mv "$db.new" "$db"; fi
  extra+=(--local-module "DarwinBoot=$db")
fi
cd "$U"
exec ./unixnu "$cmd" -D "$D" --local-module "soc:sm8750=$R/UniXNU-sm8750-common" ${extra[@]+"${extra[@]}"} "$@"
