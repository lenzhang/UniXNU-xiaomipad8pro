#!/bin/bash
# Host tests of package/PianoDwc3: the gadget core against a DWC3 model (2 and 3 ports), the SMMU planner against an SMMU model, the
# input port's command parser and its reports against PianoKeyboard's HID descriptors.
#   package/PianoDwc3/tests/run.sh [v]     v: print the gadget core's notes (the event trace)
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
for t in usb_gadget_host smmu_stream_host hid_input_host; do
  c++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -I "$P" -I "$P/../PianoKeyboard" "$P/tests/$t.cpp" -o "$T/$t"
  "$T/$t" "$@"
done
