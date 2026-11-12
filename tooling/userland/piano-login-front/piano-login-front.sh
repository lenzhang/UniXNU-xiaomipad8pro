#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-login-front: once at login, the Finder brought to the front. On the owner's Data the session could start with
# AccessibilityUIServer frontmost (an agent without menus), which keeps the menu bar off the screen until another app is activated
# (seen on the tablet 2026-11-13 01:38: the menu bar's windows offscreen with AccessibilityUIServer frontmost; activating the Finder
# put the menu bar back). Activating the Finder at login is what a normal login ends with anyway. LaunchServices only: no Apple
# events, so no automation prompt.
i=0
while [ $i -lt 90 ] && ! /usr/bin/pgrep -xq Finder; do sleep 1; i=$((i + 1)); done
sleep 6
/usr/bin/open -a Finder && /usr/bin/logger -t piano-login-front "the Finder activated after login (the menu bar's frontmost app)"
exit 0
