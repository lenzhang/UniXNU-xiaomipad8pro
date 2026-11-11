#!/bin/bash
# Host tests of package/common/PianoIrq.h (the INTR_CFG encoding, the PDC map, the adaptive-poll and guard state machines). The
# drivers' build scripts run it before their own tests.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter "$P/tests/irq_host.cpp" -o "$T/irq_host"
"$T/irq_host"
