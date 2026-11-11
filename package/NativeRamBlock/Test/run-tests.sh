#!/bin/bash
# Host tests of NativeRamBlock's disk and store code (no tablet, no kernel): build each test with the host compiler and run it.
#   package/NativeRamBlock/Test/run-tests.sh [SEED.img]   SEED: also print SeedDisk.h's identity of a packed seed (seed-identity.cpp),
#                                                          to compare with tooling/persist/common.py seed_info
# From upstream (UniXNU drivers/NativeRamBlock/Test, see ../UPSTREAM): sparse, persist (adapted to the store guard), direct, seeddisk,
# seed-identity. UniXNU-piano: guard (the store guard, replace, flatten with power cuts, self-contained stores, header compatibility),
# volatile (VolatileDisk.h). Binaries go to a temporary directory.
set -euo pipefail
T=$(cd "$(dirname "$0")" && pwd)
B=$(mktemp -d); trap 'rm -rf "$B"' EXIT
fail=0
for t in sparse persist direct seeddisk guard volatile; do
  clang++ -std=c++17 -O2 -UNDEBUG -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -o "$B/$t" "$T/$t.cpp" \
    || { echo "FAIL $t: does not build"; fail=1; continue; }
  if out=$("$B/$t" 2>&1); then echo "$t: $out"; else echo "FAIL $t: $out"; fail=1; fi
done
if [ $# -gt 0 ]; then
  clang++ -std=c++17 -O2 -Wno-unused-function -o "$B/seed-identity" "$T/seed-identity.cpp"
  echo "seed identity of $1: $("$B/seed-identity" "$1")"
fi
exit $fail
