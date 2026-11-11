#!/bin/bash
# Host tests of package/include's shared protocols: PianoScmLock.h (the kernel-wide secure-call lock, docs/scm-lock.md). The build
# scripts of its users (PianoA830, PianoPower, PianoVideo) run it before their own tests.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/scm_lock_host.cpp" -o "$T/scm_lock_host"
"$T/scm_lock_host"
# The protocol half is plain C too.
printf '#include "PianoScmLock.h"\nint main(void) { return piano_scm_lock_owner(0) == 0 ? 0 : 1; }\n' > "$T/c.c"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$P" "$T/c.c" -o "$T/c" && "$T/c"
