#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-airportd-kick: airportd started once PianoWifi80211 answers the apple80211 requests (docs/drivers/wifi-menu.md section 5).
# launchd starts airportd on an IOKit match for an IO80211Controller; PianoWifi80211 (the kernel argument piano-wifi-apple80211=1)
# registers a controller of that class name only once its interface filter answers, so launchd may start airportd by itself (M1
# measures whether it does). If airportd is not running 10 s after that, this job starts it (launchctl kickstart), once.
# Without the switch it exits at once; when Wi-Fi does not come up, the controller never says it answers: the job waits 300 s
# and exits having done nothing. Its lines go to the console and the system log; nothing else is written.
say() {
  /usr/bin/logger -t piano-airportd-kick "$*"
  echo "piano-airportd-kick: $*" > /dev/console 2> /dev/null
}
# Without the kernel argument the job ends at once (review of today's merges, #7: the 300 s wait walked the registry once a
# second through every default boot's busiest minutes, for nothing).
case " $(/usr/sbin/sysctl -n kern.bootargs 2> /dev/null) " in
*" piano-wifi-apple80211="[1-9]*) ;;
*) exit 0 ;;
esac
i=0
while [ $i -lt 300 ]; do
  /usr/sbin/ioreg -l -r -c PianoWifi80211 -d 1 2> /dev/null | /usr/bin/grep -q '"piano-wifi-apple80211-answering"' && break
  sleep 1
  i=$((i + 1))
done
[ $i -lt 300 ] || exit 0
say "PianoWifi80211 answers the apple80211 requests (seen after ${i} s)"
sleep 10
state() { /bin/launchctl print system/com.apple.airportd 2> /dev/null | /usr/bin/grep -E '^[[:space:]]*(state|pid) = ' | /usr/bin/tr -s ' \t' ' '; }
if state | /usr/bin/grep -q 'state = running'; then
  say "airportd runs already (launchd's match or a client started it):$(state | /usr/bin/tr '\n' ' ')"
  exit 0
fi
say "airportd is not running 10 s after the controller:$(state | /usr/bin/tr '\n' ' ') launchctl kickstart"
/bin/launchctl kickstart -p system/com.apple.airportd > /dev/null 2>&1
rc=$?
sleep 5
say "kickstart returned $rc; airportd now:$(state | /usr/bin/tr '\n' ' ')"
exit 0
