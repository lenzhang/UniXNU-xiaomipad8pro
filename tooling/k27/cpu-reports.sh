#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# On the tablet, from the seed's boot job (tooling/k27/stage-boot-run.py --file / --boot-run; docs/power.md 1.2): for an unattended
# native run, print to the kernel console (/dev/console: console_write, so the pstore mailbox and the USB console get it without
# stderr-log) what the idle CPU load is made of:
#   - the launchd jobs and overrides whose labels match PATTERN (which jobs run, under which label, and which are disabled);
#   - every 5 minutes the busiest processes (ps, sorted by CPU);
#   - every CPU resource report (the kernel's "caught burning CPU" -> spindump's microstackshot report, symbolicated on the tablet
#     itself) whose file name matches PATTERN, as it appears in /Library/Logs/DiagnosticReports: its first 300 lines, folded to
#     140 characters, each line its own write. That names the function a spinning thread runs, which the console log cannot.
#
#   sh cpu-reports.sh [PATTERN] [MINUTES]        PATTERN: an extended regex (default PerfPower|audiomxd), MINUTES default 15
#
# Lines start with "cpu-reports: ". Nothing is written but the console and one list in /private/var/tmp. Data is volatile in the
# test images that carry this (data-persist=0), so a report printed here is gone after the run's reset.
# For a test on a Mac: CPU_REPORTS_OUT (a file instead of /dev/console), CPU_REPORTS_DIR (the reports' directory), CPU_REPORTS_TICK
# (seconds a "minute" lasts), CPU_REPORTS_SEEN (the list).
pattern=${1:-PerfPower|audiomxd}
minutes=${2:-15}
out=${CPU_REPORTS_OUT:-/dev/console}
dir=${CPU_REPORTS_DIR:-/Library/Logs/DiagnosticReports}
tick=${CPU_REPORTS_TICK:-60}
say() { printf 'cpu-reports: %.150s\n' "$1" >> "$out" 2> /dev/null; }
say "start $(date -u '+%Y-%m-%d %H:%M:%S') UTC: jobs, the busiest processes every 5 min, reports matching $pattern for $minutes min"
launchctl list 2> /dev/null | grep -E "$pattern" | while IFS= read -r line; do say "job: $line"; done
launchctl print-disabled system 2> /dev/null | grep -E "$pattern" | while IFS= read -r line; do say "override: $line"; done
seen=${CPU_REPORTS_SEEN:-/private/var/tmp/cpu-reports.seen}
: > "$seen"
n=0
while [ "$n" -lt "$minutes" ]; do
  sleep "$tick"
  n=$((n + 1))
  if [ $((n % 5)) -eq 0 ]; then
    ps -Aro pid,%cpu,cputime,comm 2> /dev/null | head -n 9 | while IFS= read -r line; do say "ps $n min: $line"; done
  fi
  for f in "$dir"/* "$dir"/*/*; do
    [ -f "$f" ] || continue
    name=${f##*/}
    printf '%s\n' "$name" | grep -q -E "$pattern" || continue
    grep -q -x -F "$f" "$seen" && continue
    printf '%s\n' "$f" >> "$seen"
    say "report $name ($(wc -c < "$f" | tr -d ' ') bytes)"
    head -c 60000 "$f" | tr -d '\r' | fold -w 140 | head -n 300 | while IFS= read -r line; do say "| $line"; done
  done
done
say "end after $n min"
