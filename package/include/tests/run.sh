#!/bin/bash
# Host tests of package/include's shared protocols: PianoScmLock.h (the kernel-wide secure-call lock, docs/scm-lock.md) and
# PianoWcnRails.h (the WCN7860 PMU's rails shared by Wi-Fi and Bluetooth, docs/drivers/bluetooth.md §7.1). The build scripts of their
# users (PianoA830, PianoPower, PianoVideo; PianoBluetooth, and PianoWifi once it takes the rails helper) run it before their own tests.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
for t in scm_lock_host wcn_rails_host; do
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/$t.cpp" -o "$T/$t"
  "$T/$t"
done
# The protocol halves are plain C too.
printf '#include "PianoScmLock.h"\n#include "PianoWcnRails.h"\nint main(void) { return piano_scm_lock_owner(0) == 0 && piano_wcn_rails_owners(0) == 0 ? 0 : 1; }\n' > "$T/c.c"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$P" "$T/c.c" -o "$T/c" && "$T/c"
