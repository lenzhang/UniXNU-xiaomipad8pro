#!/bin/bash
# Host tests of package/include's shared protocols: PianoScmLock.h (the kernel-wide secure-call lock, docs/scm-lock.md),
# PianoWcnRails.h (the WCN7860 PMU's rails shared by Wi-Fi and Bluetooth, docs/drivers/bluetooth.md §7.1) and PianoMmVotes.h (the
# MXC/MMCX/MM1 votes shared by video and camera, docs/drivers/camera.md §4.3.1) and PianoSpmi.h (the SPMI transaction lock every
# kext that drives the PMIC arbiter shares, docs/spmi-lock.md: its test runs PianoWifi's, PianoPower's, PianoInput's, PianoThermal's
# and PianoTorch's own accessors at once on a model of the arbiter). The build scripts of their users (PianoA830, PianoPower,
# PianoVideo, PianoCamera, PianoBluetooth, PianoWifi, PianoThermal, PianoTorch, PianoInput, PianoDwc3, PianoUsbHost) run it before
# their own tests.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
for t in scm_lock_host wcn_rails_host mm_votes_host spmi_lock_host; do
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/$t.cpp" -o "$T/$t"
  "$T/$t"
done
# The protocol halves are plain C too.
printf '#include "PianoScmLock.h"\n#include "PianoWcnRails.h"\n#include "PianoMmVotes.h"\n#include "PianoSpmi.h"\nint main(void) { return piano_scm_lock_owner(0) == 0 && piano_wcn_rails_owners(0) == 0 && piano_mm_need(0, 0, 0) == 0 && piano_spmi_owner(0) == 0 ? 0 : 1; }\n' > "$T/c.c"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$P" "$T/c.c" -o "$T/c" && "$T/c"
