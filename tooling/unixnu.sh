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
for m in AppleKeyStoreState ArmCpu ArmDTPlatform DarwinBoot GopFramebuffer RamBlock XnuPrelink AirNir ArmSmmu4K AdrenoFirmware AdrenoPM4; do
  [ -d "$R/$m/.git" ] && extra+=(--local-module "$m=$R/$m")
done
cd "$U"
exec ./unixnu "$cmd" -D "$D" --local-module "soc:sm8750=$R/UniXNU-sm8750-common" ${extra[@]+"${extra[@]}"} "$@"
