#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
# piano-pmset-defaults: the recommended power timers (docs/policy.md section 7) set once, for each power source, unless the owner
# already set that source's timers. Installed as /usr/local/libexec/piano-pmset-defaults and run by the one-shot LaunchDaemon
# com.unixnu.piano-pmset-defaults (RunAtLoad, LaunchOnlyOnce: once per boot, and it does nothing once both sources are settled).
#
#   Battery Power: sleep 5 displaysleep 2 hibernatemode 0 standby 0 powernap 0 tcpkeepalive 0   (5 min idle sleep, no hibernation)
#   AC Power:      sleep 0 displaysleep 10                                                    (plugged in never sleeps)
#
# "Already set by the owner": that source's dictionary in /Library/Preferences/com.apple.PowerManagement*.plist (powerd's settings,
# written on the first change from System Settings or pmset) holds a System Sleep Timer or Display Sleep Timer that this script did
# not write (the files' sha256 after its own write is kept in the state file). Such a source is never touched, now or later.
# A source pmset does not offer yet (no "Battery Power" in pmset -g custom until PianoPower publishes the battery) is tried again at
# the next boot; a key pmset does not list for the source (e.g. standby on this model) is left out and said so.
# Sleep itself stays impossible until the kernel allows it (k27 0062's piano-sleep=1; the ADT keeps sleep-disabled), so on today's
# profile the battery's sleep timer is inert and only the display timers act.
#
# State: /private/var/db/piano-userland/pmset-defaults (lines "battery applied|skipped <why>", "ac ...", "prefs <sha256>").
# Lines go to the system log: log show --last 1h --predicate 'process == "logger"' | grep piano-pmset-defaults
# Host tests (tooling/userland/test-stage.py): PIANO_USERLAND_ROOT stands for /, pmset and logger come from PATH.
set -u
R=${PIANO_USERLAND_ROOT:-}
STATE=$R/private/var/db/piano-userland
MARK=$STATE/pmset-defaults
PREFS=$R/Library/Preferences
BATTERY='sleep 5 displaysleep 2 hibernatemode 0 standby 0 powernap 0 tcpkeepalive 0'
AC='sleep 0 displaysleep 10'

say() {
  echo "piano-pmset-defaults: $*"
  logger -t piano-pmset-defaults "$*" 2> /dev/null || true
}

prefs_sum() {   # the sha256 of powerd's settings files together, or "none"
  set -- "$PREFS"/com.apple.PowerManagement*.plist
  [ -e "$1" ] || { echo none; return; }
  cat "$@" | shasum -a 256 | cut -d' ' -f1
}

timers_set() {   # timers_set "Battery Power": a sleep timer in powerd's settings for that source
  for f in "$PREFS"/com.apple.PowerManagement*.plist; do
    [ -e "$f" ] || continue
    for k in 'System Sleep Timer' 'Display Sleep Timer'; do
      plutil -extract "$1.$k" raw -o - "$f" > /dev/null 2>&1 && return 0
    done
  done
  return 1
}

state() { [ -f "$MARK" ] && grep "^$1 " "$MARK" | tail -1 | cut -d' ' -f2; }

mkdir -p "$STATE"
[ "$(state battery)" != '' ] && [ "$(state ac)" != '' ] && exit 0
ours=$(state prefs)
now=$(prefs_sum)
custom=$(pmset -g custom 2> /dev/null)

for source in battery ac; do
  [ "$(state $source)" = '' ] || continue
  case $source in battery) name='Battery Power' flag=-b list=$BATTERY ;; ac) name='AC Power' flag=-c list=$AC ;; esac
  # the settings changed since this script last wrote them (or it never did), and the owner has timers for this source
  if [ "$now" != "${ours:-none}" ] && timers_set "$name"; then
    echo "$source skipped the owner's $name timers were there first" >> "$MARK"
    say "$name: the owner's timers are set: left as they are"
    continue
  fi
  section=$(printf '%s\n' "$custom" | awk -v s="$name:" '$0 == s {on = 1; next} /^[^ ]/ {on = 0} on')
  if [ -z "$section" ]; then
    say "$name: not offered by pmset yet (pmset -g custom): tried again at the next boot"
    continue
  fi
  done_keys= left=
  set -- $list
  while [ $# -ge 2 ]; do
    if printf '%s\n' "$section" | grep -Eq "^ +$1 "; then
      if pmset "$flag" "$1" "$2" > /dev/null 2>&1; then done_keys="$done_keys $1 $2"; else left="$left $1(failed)"; fi
    else
      left="$left $1"
    fi
    shift 2
  done
  echo "$source applied$done_keys" >> "$MARK"
  say "$name: set$done_keys${left:+; left out (not offered here):$left}"
done
echo "prefs $(prefs_sum)" >> "$MARK"
exit 0
