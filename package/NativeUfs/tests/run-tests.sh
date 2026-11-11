#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host tests of NativeUfs's command, queue and power code (no tablet, no kernel): the production headers (RootCommands.h, RootQueue.h,
# RootPower.h) built with the host compiler against the register model (UfsModel.h), with the SM8750 link handling and the
# development build's writable root windows as the kext builds them, each once plain and once with ASan/UBSan.
#   package/NativeUfs/tests/run-tests.sh          UFS_TEST_VERBOSE=1 also prints RootPower.h's console lines
set -euo pipefail
T=$(cd "$(dirname "$0")" && pwd)
B=$(mktemp -d); trap 'rm -rf "$B"' EXIT
FLAGS=(-std=c++17 -UNDEBUG -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
       -DNATIVE_UFS_SM8750=1 -DNATIVE_UFS_UNVERIFIED_ROOT=1)
fail=0
for t in RootCommands RootPower; do
  for mode in plain san; do
    extra=(-O2); [ $mode = san ] && extra=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
    if ! clang++ "${FLAGS[@]}" "${extra[@]}" -o "$B/$t-$mode" "$T/$t.cpp" 2> "$B/$t-$mode.log"; then
      echo "FAIL $t ($mode): does not build"; cat "$B/$t-$mode.log"; fail=1; continue
    fi
    if out=$("$B/$t-$mode" 2>&1); then echo "$t ($mode):"; echo "$out" | sed 's/^/  /'; else echo "FAIL $t ($mode):"; echo "$out"; fail=1; fi
  done
done
exit $fail
