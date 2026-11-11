#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-policy-agent: the userland half of PianoPolicy (docs/policy.md section 4), for the native macOS on piano. Two things the
# kernel cannot see or do on its own:
#   1. macOS Low Power Mode lives in userland only: powerd keeps it as the notify name com.apple.system.lowpowermode (state 0/1)
#      and sends nothing to the kernel. Each change is written to the kernel: sysctl machdep.piano_policy_lpm=<0|1>.
#   2. No kernel call puts the display to sleep (k27 has no display wrangler: WindowServer owns display power). When the cover is
#      closed and closing does not sleep (PianoPolicy: on external power, or no system sleep), IOPMrootDomain says so
#      (AppleClamshellState Yes, AppleClamshellCausesSleep No) and powerd posts com.apple.system.powermanagement.clamshellstate:
#      then "pmset displaysleepnow" here, and the session locks (System Settings > Lock Screen: require the password immediately).
# Event driven: one notifyutil process waits on both names (no polling); stock tools only (notifyutil, sysctl, ioreg, pmset,
# logger). Runs as root. Lines go to the system log under piano-policy-agent (log show --predicate 'process == "logger"').
#
# Installed by the userland pipeline (configs/userland.toml, tooling/userland/stage.py, docs/userland.md) as
# /usr/local/libexec/piano-policy-agent with its LaunchDaemon com.unixnu.piano-policy-agent.plist (next to this file).
# By hand (root shell), without the pipeline:
#   launchctl submit -l com.unixnu.piano-policy-agent -- /bin/sh /path/to/piano-policy-agent.sh
# and launchctl remove com.unixnu.piano-policy-agent to stop it.
set -u
LPM=com.apple.system.lowpowermode
LID=com.apple.system.powermanagement.clamshellstate

say() { logger -t piano-policy-agent "$*"; }

lpm() {
  s=$(notifyutil -g "$LPM" | awk '{print $2}')
  case "$s" in 0|1) ;; *) s=0 ;; esac
  if sysctl -w machdep.piano_policy_lpm="$s" > /dev/null 2>&1; then
    say "Low Power Mode $s: machdep.piano_policy_lpm=$s"
  else
    say "Low Power Mode $s: machdep.piano_policy_lpm not there (PianoPolicy inert: boot with piano-policy=N)"
  fi
}

lid() {
  r=$(ioreg -r -c IOPMrootDomain -d 1 | grep -E '"AppleClamshell(State|CausesSleep)"')
  case "$r" in *'"AppleClamshellState" = Yes'*) ;; *) return ;; esac
  case "$r" in *'"AppleClamshellCausesSleep" = Yes'*) say "cover closed: the system sleeps"; return ;; esac
  pmset displaysleepnow && say "cover closed without sleep: display sleep (the screen locks)"
}

say "started (pid $$): Low Power Mode -> machdep.piano_policy_lpm; a cover closed without sleep -> display sleep"
lpm
notifyutil -w "$LPM" -w "$LID" | while read -r key _; do
  case "$key" in
    "$LPM") lpm ;;
    "$LID") lid ;;
  esac
done
say "notifyutil ended: exiting (launchd starts the agent again)"
