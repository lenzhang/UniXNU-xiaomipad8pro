# PianoUsbHost

USB host mode and the Type-C port of piano for native macOS 27: the DWC3 at `usb@a600000` as an xHCI (its first 32 KiB), the
QMP USB3/DP combo PHY's Type-C mux, the DIO4485 SBU switch, the PMIH010x Type-C block and, later, the DSP's UCSI client and port
notifications. Separate from PianoDwc3 (the device-mode debug console keeps the controller unless a host level is asked for).

- `piano-usbhost=1`: the Type-C state (PMIC Type-C block and the eUSB2 repeater, over the SPMI observer channel), the orientation
  line gpio61 and the DIO4485's bus pins, the USB power domains and clocks, the DWC3 role (GCTL.PRTCAPDIR) and glue words, the
  xHCI capability words (only when the core is powered and clocked), the xHCI operational/port/extended-capability words (only in
  host or OTG mode), the QMP PHY's DP_COM/PCS words (only when the PHY is powered and clocked), the orientation witnesses and the
  role the policy would choose. Every MMIO access is a load; the only store is the SPMI observer's read command.
- `piano-usbhost=2`: the same, then a watch of the port (a line for each plug, unplug or orientation change).
- `piano-usbhost=5` (H5, writes): the survey, then host mode with our own minimal xHCI probe: stream 0x40 through an SMMU bypass
  bank, the DWC3 switched to host (glue stores only with the clocks re-read on), halt + HCRST, rings in a non-cacheable DMA region,
  a No-Op command, the root ports' connection (and with `-reset=1` a USB 2 port reset), a port watch; `-back=S` returns to device
  mode. Polled. Refused while PianoDwc3 owns the controller (`piano-usb>=2`), except a timed hand-off.
- `piano-usbhost=6` (H6 preparation, path A1, writes): the same switch and probe, stream 0x40 identity-mapped write-back, then
  Apple's `AppleUSBXHCI` on `PianoUsbHostNub` through the codeless personality of `tooling/k27/kext/PianoUsbHostStack.json`. Needs
  a collection built with `PIANO_USBHOST_STACK=1`; refused with `piano-usb>=2`.

Design, hardware facts, the macOS-side verdict, the bring-up ladder, the test images and their expected lines:
`docs/drivers/usb-host.md`.

| File | What |
|---|---|
| `PianoUsbHost.cpp` | the IOService on IOPlatformExpertDevice; the survey thread, its log lines, the watch, the `piano-usbhost-survey` property; levels 5 and 6 (KernelHal, DMA region, SMMU route, `PianoUsbHostNub` and its interrupt, the Apple-stack watch, `piano-usbhost-host`) |
| `UsbHostSurvey.h` | portable: what level 1 loads and under which gate (no store in the file), the decoders (DWC3, xHCI, QMP, PMIC Type-C), the orientation witnesses, the role policy |
| `XhciHost.h` | portable: the host switch and back, the xHCI probe (reset, rings, run, No-Op, ports, stop), every store through a Hal |
| `SmmuIdentity.h` | portable: stream 0x40 through an identity-mapped write-back stage 1 context bank (after QcomDwc3) |
| `UsbHostHw.h` | the facts, each block with its source |
| `tests/usbhost_host.cpp` | host tests of the survey against register models (what is and is never loaded, the gates, the decoders, the walk's bounds, the policy) |
| `tests/xhcihost_host.cpp` | host tests of level 5 against a model of the core, the glue, the GCC gates and an xHCI executing its rings (gates, order, hi-lo, No-Op, wraps, ports) |
| `tests/smmu_identity_host.cpp` | host tests of the identity map against a model of piano's apps SMMU |

Build: `tooling/k27/build-native-piano-usbhost.sh` (the three host tests, a store check of the survey headers, the objects, the import
check). PianoUsbHost is in `configs/k27-native-kexts.txt` and inert without `piano-usbhost`. It includes
`package/PianoDwc3/{PianoUsbHw.h,SmmuStream.h}`, `package/PianoPower/{SpmiObserver.h,ChargerPmic.h}` and
`package/common/PianoIrq.h` (not copied). Apple's host stack for level 6: `PIANO_USBHOST_STACK=1 tooling/k27/build-native-image.sh`
(`package/PianoUsbHostStandIn`, `tooling/k27/kext/PianoUsbHostStack.json`, `tooling/k27/check-usbhost-stack.py`). Sources and
licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
