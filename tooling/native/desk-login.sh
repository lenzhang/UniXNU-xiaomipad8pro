#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Log the owner in on a desktop image (desk, desk-dev: docs/attended-desktop.md) from this Mac, over PianoDwc3's keyboard
# (piano-usb=2 piano-usb-hid=1, tooling/native/piano-input.py), with the password he left in the ignored private/login.secret. The
# password is handed to piano-input.py on stdin (`type --stdin`): never echoed, never in argv or ps, never written anywhere; this
# script prints only what it waited for and when.
#
#   desk-login.sh [--console FILE] [--timeout SECS] [--secret FILE] [--now]
#
# It waits (up to --timeout, default 600 s) in the running USB console capture (FILE, default the newest
# logs/usb-console/*-port0.txt written to in the last 2 minutes) for the login window: SecurityAgent (the login window's UI) opening
# the accelerator ("PianoA830: SecurityAgent opens the accelerator"), then NativeGopDisplay's next frame line with a lit frame, then
# 10 s. A "Finder opens the accelerator" first means someone is logged in already: nothing is typed. Then the password and Return
# (the login window has the account selected and its password field focused, as on the owner's 2026-11-12 17:02 login), and up to
# 180 s for the Finder to open the accelerator: the desktop (in that login's capture the Finder opened it, the Dock did not).
# --now types at once (the login window seen on the webcam). DESK_LOGIN_INPUT replaces the piano-input.py command for a dry run.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
CONSOLE= TIMEOUT=600 SECRET=private/login.secret NOW=0
while [ $# -gt 0 ]; do
  case $1 in
    --console) CONSOLE=$2; shift ;;
    --timeout) TIMEOUT=$2; shift ;;
    --secret) SECRET=$2; shift ;;
    --now) NOW=1 ;;
    *) echo "desk-login: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
[ -s "$SECRET" ] || { echo "desk-login: $SECRET is missing or empty" >&2; exit 2; }
case $(cd "$(dirname "$SECRET")" && pwd -P)/ in "$(pwd -P)/private/"*|"$(pwd -P)/out/"*) ;; *)
  echo "desk-login: $SECRET is not under this repository's private/ or out/ (both ignored)" >&2; exit 2 ;; esac
read -r -a INPUT <<< "${DESK_LOGIN_INPUT:-python3 tooling/native/piano-input.py}"   # (a stand-in for a dry run)
start=$(date +%s)
left() { echo $(( TIMEOUT - ($(date +%s) - start) )); }
since() { echo $(( $(date +%s) - start )); }
after() {   # after LINE PATTERN: does a line after line LINE match PATTERN? (grep -c reads to the end: no SIGPIPE for pipefail)
  tail -n +"$(( $1 + 1 ))" "$CONSOLE" | grep -a -c -E "$2" > /dev/null
}
line_of() {   # line_of PATTERN: the number of the last line matching it (0: none)
  grep -a -n -E "$1" "$CONSOLE" | tail -n 1 | cut -d: -f1 || true
}

if [ "$NOW" = 0 ] || [ -n "$CONSOLE" ]; then
  if [ -z "$CONSOLE" ]; then
    while [ "$(left)" -gt 0 ]; do
      CONSOLE=$(ls -t logs/usb-console/*-port0.txt 2> /dev/null | head -n 1 || true)
      [ -n "$CONSOLE" ] && [ $(( $(date +%s) - $(stat -f %m "$CONSOLE") )) -lt 120 ] && break
      CONSOLE=
      sleep 5
    done
    [ -n "$CONSOLE" ] || { echo "desk-login: no running USB console capture (logs/usb-console/*-port0.txt) within ${TIMEOUT}s" >&2; exit 2; }
  fi
  echo "desk-login: watching $CONSOLE"
fi
if [ "$NOW" = 0 ]; then
  sa=0
  while [ "$(left)" -gt 0 ]; do
    if grep -a -q -E 'PianoA830: Finder opens the accelerator' "$CONSOLE"; then
      echo "desk-login: the Finder runs already (someone is logged in): nothing typed"; exit 0
    fi
    sa=$(line_of 'PianoA830: SecurityAgent opens the accelerator')
    [ "${sa:-0}" -gt 0 ] && break
    sleep 3
  done
  [ "${sa:-0}" -gt 0 ] || { echo "desk-login: no login window (SecurityAgent) within ${TIMEOUT}s: nothing typed" >&2; exit 2; }
  echo "desk-login: the login window's SecurityAgent after $(since)s; waiting for a lit frame"
  until after "$sa" 'NativeGopDisplay: .*last frame ([5-9][0-9]|100)\.[0-9]% non-black'; do
    [ "$(left)" -gt 0 ] || { echo "desk-login: no lit frame after the login window within ${TIMEOUT}s: nothing typed" >&2; exit 2; }
    sleep 3
  done
  sleep 10
fi
until "${INPUT[@]}" status > /dev/null 2>&1; do   # PianoDwc3's input port
  [ "$(left)" -gt 0 ] || { echo "desk-login: the tablet's input port did not answer within ${TIMEOUT}s" >&2; exit 2; }
  sleep 5
done
typed=$(since)
"${INPUT[@]}" type --stdin --enter < "$SECRET" > /dev/null
echo "desk-login: the password (not shown) and Return typed at $(since)s"
[ -n "$CONSOLE" ] || exit 0
mark=$(wc -l < "$CONSOLE" | tr -d ' ')
for i in $(seq 1 60); do
  if after "$mark" 'PianoA830: Finder opens the accelerator'; then
    echo "desk-login: the desktop: the Finder opened the accelerator $(( $(since) - typed ))s after the password"; exit 0
  fi
  sleep 3
done
echo "desk-login: no Finder within 180 s of the password (a wrong field or password? the webcam; --now to type again)" >&2
exit 1
