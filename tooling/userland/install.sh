#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
# piano-userland install.sh: puts the userland staged next to it (manifest.tsv, files/) in place on the tablet's macOS, as root
# (docs/userland.md section 5). tooling/userland/stage.py copies it into the Data seed with the files and adds
#   sh /private/var/piano/userland/install.sh --boot
# to the seed's boot job, so every boot from that seed runs it; a live update runs it with --live (docs/userland.md section 7).
#
#   install.sh [--boot|--live]   install what differs, load the jobs that are on, restart the loaded jobs whose files changed
#   install.sh --check           change nothing: every file against the manifest (sha256, owner, mode) and every job's state;
#                                exit status 1 on any difference
#
# Rules:
#   - A staged file whose sha256 is not the manifest's is not installed (a torn copy); its unit counts as failed.
#   - A file is replaced only when its bytes differ, by install -S (a new file renamed over the old one: a running program keeps
#     its pages). Owner and mode are set as the manifest says, also on an unchanged file.
#   - A job that is on (default_on in configs/userland.toml) is loaded (launchctl bootstrap) when it is not; when the owner turned
#     it off (launchctl disable), launchd refuses the bootstrap and it stays off. A job that is off has Disabled in its plist (written
#     by stage.py) and is never loaded here; `launchctl enable system/<label>` (an agent: gui/<uid>/<label>) turns it on for good.
#   - A loaded job whose files changed is restarted (kickstart -k; bootout and bootstrap when its plist changed).
#   - Agents go to the console user's session; with nobody logged in, the next login window or login loads them.
#   - A file this installer put in place earlier (the receipt) that the manifest no longer lists is removed, its job booted out,
#     when it is unchanged since; anything else is left alone.
# SIGHUP is ignored: run through the USB root shell, a restart of pianousb-shell ends that shell but not this script.
# Lines go to stdout (the boot job's: the kernel log with stderr-log=1), the system log (logger -t piano-userland) and
# /private/var/db/piano-userland/last.txt.
#
# Host tests (tooling/userland/test-stage.py): PIANO_USERLAND_ROOT is a directory standing for /, PIANO_USERLAND_NOCHOWN=1 leaves
# owners alone (a user cannot chown to root), launchctl and logger come from PATH.
set -u
trap '' HUP
MODE=${1:---boot}
case "$MODE" in --boot|--live|--check) ;; *) echo "usage: install.sh [--boot|--live|--check]" >&2; exit 2 ;; esac
R=${PIANO_USERLAND_ROOT:-}
HERE=$(cd "$(dirname "$0")" && pwd)
S=${PIANO_USERLAND_STAGE:-$HERE}
STATE=$R/private/var/db/piano-userland
NOCHOWN=${PIANO_USERLAND_NOCHOWN:-0}
TAB=$(printf '\t')
LOG=
FAILED=0
CHANGED=' '
PLIST_CHANGED=' '

say() {
  echo "piano-userland: $*"
  logger -t piano-userland "$*" 2> /dev/null || true
  LOG="$LOG$*
"
}

sum() {
  if command -v shasum > /dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
  elif command -v openssl > /dev/null 2>&1; then openssl dgst -sha256 -r "$1" | cut -d' ' -f1
  else echo unknown; fi
}

# The owner and mode a file has: "user:group mode" (mode in octal, as the manifest writes it, e.g. 0755).
has() {
  stat -f '%Su:%Sg 0%Lp' "$1" 2> /dev/null
}

domain_of() {   # domain_of KIND: system, gui/<uid> of the console user, or nothing (an agent with nobody logged in)
  if [ "$1" = daemon ]; then echo system; return; fi
  u=$(stat -f %u "$R/dev/console" 2> /dev/null || echo 0)
  [ "$u" != 0 ] && echo "gui/$u"
}

loaded() { launchctl print "$1/$2" > /dev/null 2>&1; }

disabled() { launchctl print-disabled "$1" 2> /dev/null | grep -Eq "\"$2\" => (disabled|true)"; }

[ -f "$S/manifest.tsv" ] || { say "no manifest at $S/manifest.tsv"; exit 1; }
[ "$MODE" = --check ] || mkdir -p "$STATE"

# 1. Files.
while IFS=$TAB read -r unit kind on mode owner group want staged dest label; do
  case "$unit" in ''|'#'*) continue ;; esac
  src=$S/$staged
  dst=$R$dest
  if [ "$MODE" = --check ]; then
    if [ ! -f "$dst" ]; then say "$dest: missing ($unit)"; FAILED=1; continue; fi
    got=$(sum "$dst")
    [ "$got" = "$want" ] || [ "$got" = unknown ] || { say "$dest: sha256 $got, the manifest's $want ($unit)"; FAILED=1; }
    if [ "$NOCHOWN" = 1 ]; then now=$(has "$dst" | cut -d' ' -f2); exp=$mode
    else now=$(has "$dst"); exp="$owner:$group $mode"; fi
    [ "$now" = "$exp" ] || { say "$dest: $now, the manifest's $exp ($unit)"; FAILED=1; }
    continue
  fi
  if [ ! -f "$src" ]; then say "$dest: the staged file $staged is missing: $unit not installed"; FAILED=1; continue; fi
  got=$(sum "$src")
  if [ "$got" != "$want" ] && [ "$got" != unknown ]; then
    say "$dest: the staged file's sha256 is $got, not the manifest's $want (a torn copy?): $unit not installed"
    FAILED=1
    continue
  fi
  dir=$(dirname "$dst")
  if [ ! -d "$dir" ]; then
    if [ "$NOCHOWN" = 1 ]; then install -d -m 755 "$dir"; else install -d -o root -g wheel -m 755 "$dir"; fi
  fi
  if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
    if [ "$NOCHOWN" = 1 ]; then now=$(has "$dst" | cut -d' ' -f2); exp=$mode
    else now=$(has "$dst"); exp="$owner:$group $mode"; fi
    if [ "$now" != "$exp" ]; then
      if [ "$NOCHOWN" = 1 ]; then chmod "$mode" "$dst"; else chown "$owner:$group" "$dst" && chmod "$mode" "$dst"; fi
      say "$dest: owner/mode $now -> $exp"
    fi
    continue
  fi
  if [ "$NOCHOWN" = 1 ]; then install -S -m "$mode" "$src" "$dst"; else install -S -o "$owner" -g "$group" -m "$mode" "$src" "$dst"; fi
  if [ $? != 0 ]; then say "$dest: install failed ($unit)"; FAILED=1; continue; fi
  say "$dest: installed (sha256 ${want%"${want#????????????????}"})"
  CHANGED="$CHANGED$unit "
  case "$kind" in daemon|agent) PLIST_CHANGED="$PLIST_CHANGED$unit " ;; esac
done < "$S/manifest.tsv"

# 2. Files of the previous receipt that the manifest no longer lists: removed when unchanged since, their jobs booted out first.
if [ "$MODE" != --check ] && [ -f "$STATE/receipt.tsv" ]; then
  while IFS=$TAB read -r unit kind on mode owner group want staged dest label; do
    case "$unit" in ''|'#'*) continue ;; esac
    cut -f9 "$S/manifest.tsv" | grep -qxF "$dest" && continue
    dst=$R$dest
    [ -f "$dst" ] || continue
    if [ "$(sum "$dst")" != "$want" ]; then say "$dest: no longer in the manifest, but changed since it was installed: left"; continue; fi
    if [ "$kind" != file ]; then
      d=$(domain_of "$kind")
      if [ -n "$d" ] && loaded "$d" "$label"; then launchctl bootout "$d/$label" 2> /dev/null; fi
    fi
    rm -f "$dst" && say "$dest: removed (no longer in the manifest)"
  done < "$STATE/receipt.tsv"
fi

# The receipt: what is in place now (before any job is touched, so a run cut short below still knows its files).
if [ "$MODE" != --check ]; then
  : > "$STATE/receipt.tsv.new"
  while IFS=$TAB read -r unit kind on mode owner group want staged dest label; do
    case "$unit" in ''|'#'*) continue ;; esac
    [ -f "$R$dest" ] && [ "$(sum "$R$dest")" = "$want" ] &&
      printf '%s\n' "$unit$TAB$kind$TAB$on$TAB$mode$TAB$owner$TAB$group$TAB$want$TAB$staged$TAB$dest$TAB$label" >> "$STATE/receipt.tsv.new"
  done < "$S/manifest.tsv"
  mv -f "$STATE/receipt.tsv.new" "$STATE/receipt.tsv"
fi

# 3. Jobs.
while IFS=$TAB read -r unit kind on mode owner group want staged dest label; do
  case "$kind" in daemon|agent) ;; *) continue ;; esac
  d=$(domain_of "$kind")
  if [ -z "$d" ]; then
    [ "$on" = 1 ] && say "$label: an agent; nobody is logged in: the next login window or login loads it"
    continue
  fi
  if [ "$MODE" = --check ]; then
    if loaded "$d" "$label"; then say "$label: loaded in $d"
    elif [ "$on" = 1 ] && disabled "$d" "$label"; then say "$label: on in the manifest, turned off by the owner (launchctl disable)"
    elif [ "$on" = 1 ]; then say "$label: on in the manifest but not loaded in $d"; FAILED=1
    else say "$label: off (launchctl enable $d/$label turns it on)"; fi
    continue
  fi
  case "$CHANGED" in *" $unit "*) changed=1 ;; *) changed=0 ;; esac
  if loaded "$d" "$label"; then
    [ "$changed" = 1 ] || continue
    case "$PLIST_CHANGED" in
      *" $unit "*) launchctl bootout "$d/$label" 2> /dev/null
                   if launchctl bootstrap "$d" "$R$dest" 2> /dev/null; then say "$label: reloaded in $d (its job file changed)"
                   elif [ "$on" = 0 ] || disabled "$d" "$label"; then say "$label: off now: booted out of $d"
                   else say "$label: bootstrap in $d failed after the bootout"; FAILED=1; fi ;;
      *) if launchctl kickstart -k "$d/$label" > /dev/null 2>&1; then say "$label: restarted in $d (its files changed)"
         else say "$label: kickstart -k in $d failed"; FAILED=1; fi ;;
    esac
    continue
  fi
  [ "$on" = 1 ] || continue
  if disabled "$d" "$label"; then say "$label: turned off by the owner (launchctl disable): left off"; continue; fi
  if launchctl bootstrap "$d" "$R$dest" 2> /dev/null; then say "$label: loaded in $d"
  else say "$label: launchctl bootstrap $d failed"; FAILED=1; fi
done < "$S/manifest.tsv"

result=ok
[ "$FAILED" = 0 ] || result=failed
say "$MODE done: $result (changed:${CHANGED% })"
if [ "$MODE" != --check ]; then
  { date -u '+%Y-%m-%dT%H:%M:%SZ' 2> /dev/null; printf '%s' "$LOG"; } > "$STATE/last.txt.new" && mv -f "$STATE/last.txt.new" "$STATE/last.txt"
fi
exit "$FAILED"
