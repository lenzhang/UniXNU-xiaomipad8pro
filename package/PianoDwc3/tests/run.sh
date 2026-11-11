#!/bin/bash
# Host tests of package/PianoDwc3: the gadget core against a DWC3 model (2 and 3 ports, each without and with the CDC-NCM network
# function; the ACM-only descriptors against their bytes from before it), NTB16 framing, the frame queue and the TX FIFO plan, the
# SMMU planner against an SMMU model, the input port's command parser and its reports against PianoKeyboard's HID descriptors, the
# USB thread's wait tiers and the interrupt handler's decision (UsbIdle.h), the system sleep's steps and the QSCRATCH guard (UsbSleep.h),
# the controller hand-off's decisions (UsbHandoff.h).
#   package/PianoDwc3/tests/run.sh [v]     v: print the gadget core's notes (the event trace)
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
for t in usb_gadget_host ncm_host smmu_stream_host hid_input_host usb_idle_host usb_sleep_host usb_handoff_host; do
  c++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -I "$P" -I "$P/../PianoKeyboard" "$P/tests/$t.cpp" -o "$T/$t"
  "$T/$t" "$@"
done
