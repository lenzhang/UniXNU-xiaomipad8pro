# PianoDp

piano's USB-C DisplayPort Alt Mode for native macOS 27 (backlog B8): the DP controller at `af54000`, the DP half of the QMP USB3/DP
combo PHY, HPD and the alt-mode state from the DSP's USB-C port notifications, the DPU's second CTL/LM path. Built so far: the first
slices, all read only, inert without `piano-dp`.

- `piano-dp=1`, **D1**: a read-only survey. PianoDpu's display gate first (nothing of DISPCC without GCC's display AHB, nothing of
  MDSS/DPU without its GDSC, AHB and MDP clocks); DISPCC's DP branches and RCGs; the DP controller block by block, each only with
  its own clocks on (ahb + aux with the AUX clock, link with LINK + LINK_INTF, p0 with PIXEL0; the AUX FIFO and the HPD acknowledge
  register never); the QMP PHY's DP_COM and DP half (revision, power-down, mode, status, AUX configuration as found) only with the
  PHY's power domain and clocks on; the DPU's DP side (every CTL and LM, MERGE_3D, INTF0/INTF3, the TOP words) and what of it is
  free; the orientation and DIO4485 pins. Every MMIO access is a load.
- `piano-dp=1`, **D2**: the port events. Registered with PianoPower's client contract (`piano_port_events`,
  `package/include/PianoAdspContract.h`; the fan-out is `package/PianoPower/PortEvents.c`), every port notification the DSP sends
  (PianoPower at level 2) is decoded and its changes logged per port: attach, detach, DisplayPort entry and exit, pin assignment,
  orientation, HPD up/down, IRQ_HPD, and what a plug would program (logged, never written). Event-driven: the thread sleeps until a
  notification arrives.
- **D6**: the macOS-side probe (`DpFake.h`): NativeGopDisplay publishes a second, RAM-backed display on its nub with `piano-dp=1
  piano-dp-fake=S` (or `piano-dp-fake-boot=1`), hot-plugged on a timer, to see whether WindowServer extends the desktop. No hardware;
  this kext only logs the configuration.

Design, facts, the expected lines and the tablet test list: `docs/drivers/displayport.md`.

| File | What |
|---|---|
| `PianoDp.cpp` | the IOService on IOPlatformExpertDevice: the thread, MMIO pages mapped on demand (loads only), the report, the port-event client and its ring, the per-port log, the registry properties |
| `DpHw.h` | the facts, each with its source (DP controller blocks and registers, DISPCC's DP clocks, the QMP DP half, the notification layout, the DIO4485 words, the link cap) |
| `DpSurvey.h` | portable: D1's reads behind their gates, the decoders, the DPU path verdict; no store |
| `DpPort.h` | portable: D2's notification decoder, the pin-assignment table, the link budget, the HPD state machine, the plug plan |
| `DpFake.h` | portable: D6's arguments and plug schedule, shared with NativeGopDisplay |
| `tests/dp_host.cpp` | host tests: the gates on a register model, the decoders, the path verdict, the notification decoder, the pin table and budget, the state machine on plug sequences, D6's configuration and schedule |

Build: `tooling/k27/build-native-piano-dp.sh` (host tests, a store check of the portable headers, the objects, the import check).
PianoDp is in `configs/k27-native-kexts.txt` after PianoPower, whose client functions it links. It includes PianoDpu's, PianoUsbHost's
and PianoDwc3's fact and survey headers (not copied). Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
