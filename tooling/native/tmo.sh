#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# tmo: a command on the tablet with a time limit, for the probe checklists (docs/drivers/wifi-menu.md). macOS has no timeout(1)
# and a tablet command that never returns has held a whole session's ssh before (wm1b, 2026-11-14: CoreWLAN's scan).
#   tmo.sh SECONDS command [args...]      -> the command's own status, or 124 when the limit ran out
# Copy it to the tablet (or paste the function) and prefix every command of a checklist with it.
s=${1:?seconds}
shift
[ $# -gt 0 ] || { echo "tmo: no command" >&2; exit 2; }
"$@" &
p=$!
i=0
while kill -0 "$p" 2> /dev/null && [ "$i" -lt "$s" ]; do
  sleep 1
  i=$((i + 1))
done
if kill -0 "$p" 2> /dev/null; then
  kill -9 "$p" 2> /dev/null
  wait "$p" 2> /dev/null
  echo "tmo: gave up after ${s}s: $*" >&2
  exit 124
fi
wait "$p"
