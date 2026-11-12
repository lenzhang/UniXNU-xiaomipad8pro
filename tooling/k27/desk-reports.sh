#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# On the tablet, from the seed's boot job (tooling/k27/stage-boot-run.py --file / --boot-run; the diag image of
# tooling/k27/build-desktop-images.sh, docs/attended-desktop.md): what the console log cannot say about the desktop session, printed
# to the kernel console (/dev/console: console_write, so the pstore mailbox and the USB console get it without stderr-log):
#   - at the start: launchd's overrides and jobs of the respawn loops (watchdogd, mobileactivationd, xprotectd, PerfPowerServices,
#     audiomxd, endpointsecurityd, ReportCrash);
#   - the jobs whose last exit was not 0 (`launchctl list`: a status 78 is a spawn that failed, a negative one the signal of a
#     crash), at the start and 1, 3 and 10 minutes after the login, with `launchctl print` of each such job's state lines;
#   - the login: when /dev/console changes owner (the login window is root's), the unified log of loginwindow, WindowServer, Dock,
#     Finder, SystemUIServer, SecurityAgent, opendirectoryd, securityd, coreauthd and KeyboardSetupAssistant from 30 s before to
#     2 minutes after, filtered to the lines that date the phases (launched, started, shown, finished, waiting, timed out, giving
#     up, error, fault); and authorizationhost's lines unfiltered (it took 13.0 s after every password in attend1/attend4);
#   - xprotectd's own log lines (it exits 1 at every start natively, the reason is not on stderr);
#   - the busiest processes 3 and 10 minutes after the login.
#
#   sh desk-reports.sh [MINUTES]     MINUTES (default 40): how long to wait for a login before giving up
#
# Lines start with "desk-reports: ". Nothing is written but the console. The unified log can hold the user's short name (the
# capture stays on the Mac, logs/ is not committed); no password or key is ever logged by these processes or by this script.
# For a test on a Mac: DESK_REPORTS_OUT (a file instead of /dev/console), DESK_REPORTS_TICK (seconds between login checks, default
# 5), DESK_REPORTS_SCALE (seconds a "minute" lasts, default 60), DESK_REPORTS_CONSOLE (the file whose owner is the console user),
# DESK_REPORTS_TMP (a scratch file, default /private/var/tmp/desk-reports.failing).
minutes=${1:-40}
out=${DESK_REPORTS_OUT:-/dev/console}
tick=${DESK_REPORTS_TICK:-5}
scale=${DESK_REPORTS_SCALE:-60}
console=${DESK_REPORTS_CONSOLE:-/dev/console}
tmp=${DESK_REPORTS_TMP:-/private/var/tmp/desk-reports.failing}
jobs='watchdogd|mobileactivationd|xprotectd|PerfPowerServices|audiomxd|endpointsecurity|ReportCrash'
say() { printf 'desk-reports: %.150s\n' "$1" >> "$out" 2> /dev/null; }
each() {   # each PREFIX: every line of stdin as its own report line
  while IFS= read -r line; do say "$1$line"; done
}
failing() {   # failing WHEN: the jobs whose last exit status is not 0, then each one's state lines
  say "failing ($1): PID STATUS LABEL"
  launchctl list 2> /dev/null | awk 'NR > 1 && $2 != "0" && $2 != "-"' | head -n 40 > "$tmp"
  each "failing ($1): " < "$tmp"
  awk '{print $3}' "$tmp" | head -n 12 | while IFS= read -r label; do
    launchctl print "system/$label" 2> /dev/null \
      | grep -E '^[[:space:]]*(state|runs|last exit code|last exit reason|spawn type|program|properties) =' \
      | head -n 8 | each "job $label: "
  done
}
cpu() {   # cpu WHEN
  ps -Aro pid,%cpu,cputime,comm 2> /dev/null | head -n 9 | each "ps ($1): "
}
stamp() { date '+%Y-%m-%d %H:%M:%S'; }
say "start $(date -u '+%Y-%m-%d %H:%M:%S') UTC: the respawn loops' jobs, failing jobs, the login's phases (waits up to $minutes min)"
launchctl print-disabled system 2> /dev/null | grep -E "$jobs" | each "override: "
launchctl list 2> /dev/null | grep -E "$jobs" | each "job: "
failing start
# The login: the console's owner leaves root (the login window) for the user.
waited=0 limit=$((minutes * scale))
user=$(stat -f %u "$console" 2> /dev/null || echo 0)
while [ "${user:-0}" = 0 ] && [ "$waited" -lt "$limit" ]; do
  sleep "$tick"
  waited=$((waited + tick))
  user=$(stat -f %u "$console" 2> /dev/null || echo 0)
done
if [ "${user:-0}" = 0 ]; then
  say "no login within $minutes min"
  failing "no login"
  cpu "no login"
  exit 0
fi
login=$(stamp)
say "login: the console's owner is uid $user at $login (local time), $waited s after the start"
sleep "$scale"
failing "login + 1 min"
start=$(date -j -v-30S -f '%Y-%m-%d %H:%M:%S' "$login" '+%Y-%m-%d %H:%M:%S' 2> /dev/null || printf '%s' "$login")
sleep $((scale * 2))
end=$(stamp)
say "log: loginwindow, WindowServer, Dock, Finder, SystemUIServer, SecurityAgent, opendirectoryd, securityd, coreauthd, KeyboardSetupAssistant from $start to $end"
log show --style compact --start "$start" --end "$end" \
  --predicate 'process == "loginwindow" OR process == "WindowServer" OR process == "Dock" OR process == "Finder" OR process == "SystemUIServer" OR process == "SecurityAgent" OR process == "opendirectoryd" OR process == "securityd" OR process == "coreauthd" OR process == "KeyboardSetupAssistant"' 2> /dev/null \
  | grep -i -E 'launch|start|shown|finish|ready|wait|timed? ?out|giving up|time ?out|error|fault|fail|deny|denied' \
  | head -n 250 | cut -c 1-300 | fold -w 140 | each "| "
say "log: authorizationhost from $start to $end"
log show --style compact --start "$start" --end "$end" --predicate 'process == "authorizationhost"' 2> /dev/null \
  | head -n 120 | cut -c 1-300 | fold -w 140 | each "| "
failing "login + 3 min"
cpu "login + 3 min"
say "log: xprotectd (last 10 min)"
log show --style compact --last 10m --predicate 'process == "xprotectd"' 2> /dev/null | tail -n 40 | cut -c 1-300 | fold -w 140 \
  | each "| "
sleep $((scale * 7))
failing "login + 10 min"
cpu "login + 10 min"
say "end"
