#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
# install-piano-rotated.sh [DIR]: on the tablet, as root (the USB root shell, or the Data seed's boot job through
# tooling/k27/stage-soft-metal.py --boot-run). Puts the agent from DIR (default /private/var/piano/rotated, where stage-soft-metal.py
# --file stages it) at /usr/local/libexec/piano-rotated and its launchd job in /Library/LaunchAgents, both owned by root (launchd
# refuses a job file that is not), then loads it into the console user's session if someone is logged in; otherwise the next login
# window or login takes it. The agent still does nothing without the kernel boot argument piano-rotate (docs/drivers/sensors.md 5a).
# Undo: launchctl bootout gui/<uid>/com.unixnu.piano-rotated; rm /Library/LaunchAgents/com.unixnu.piano-rotated.plist
#       /usr/local/libexec/piano-rotated
set -eu
S=${1:-/private/var/piano/rotated}
JOB=/Library/LaunchAgents/com.unixnu.piano-rotated.plist
[ "$(id -u)" = 0 ] || { echo "install-piano-rotated.sh: run as root" >&2; exit 1; }
install -d -o root -g wheel -m 755 /usr/local/libexec
install -o root -g wheel -m 755 "$S/piano-rotated" /usr/local/libexec/piano-rotated
install -o root -g wheel -m 644 "$S/com.unixnu.piano-rotated.plist" "$JOB"
U=$(stat -f %u /dev/console 2>/dev/null || echo 0)
if [ "$U" != 0 ]; then
  launchctl bootout "gui/$U/com.unixnu.piano-rotated" 2>/dev/null || true
  launchctl bootstrap "gui/$U" "$JOB" 2>/dev/null || echo "install-piano-rotated.sh: launchctl bootstrap gui/$U failed (it loads at the next login)" >&2
fi
echo "install-piano-rotated.sh: installed; $(/usr/local/libexec/piano-rotated --probe | head -1)"
