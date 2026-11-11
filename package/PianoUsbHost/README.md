# PianoUsbHost

USB host mode and the Type-C port of piano for native macOS 27: the DWC3 at `usb@a600000` as an xHCI (its first 32 KiB), the
QMP USB3/DP combo PHY's Type-C mux, the DIO4485 SBU switch, the PMIH010x Type-C block and, later, the DSP's UCSI client and port
notifications. **Level 1 only**: a read-only survey, separate from PianoDwc3 (the device-mode debug console keeps the controller).

- `piano-usbhost=1`: the Type-C state (PMIC Type-C block and the eUSB2 repeater, over the SPMI observer channel), the orientation
  line gpio61 and the DIO4485's bus pins, the USB power domains and clocks, the DWC3 role (GCTL.PRTCAPDIR) and glue words, the
  xHCI capability words (only when the core is powered and clocked), the xHCI operational/port/extended-capability words (only in
  host or OTG mode), the QMP PHY's DP_COM/PCS words (only when the PHY is powered and clocked), the orientation witnesses and the
  role the policy would choose. Every MMIO access is a load; the only store is the SPMI observer's read command.
- `piano-usbhost=2`: the same, then a watch of the port (a line for each plug, unplug or orientation change).

Design, hardware facts, the macOS-side verdict, the bring-up ladder and the test plan: `docs/drivers/usb-host.md`.

| File | What |
|---|---|
| `PianoUsbHost.cpp` | the IOService on IOPlatformExpertDevice; the survey thread, its log lines, the watch, the `piano-usbhost-survey` property |
| `UsbHostSurvey.h` | portable: what level 1 loads and under which gate (no store in the file), the decoders (DWC3, xHCI, QMP, PMIC Type-C), the orientation witnesses, the role policy |
| `UsbHostHw.h` | the facts, each block with its source |
| `tests/usbhost_host.cpp` | host tests against register models (what is and is never loaded, the gates, the decoders, the walk's bounds, the policy) |

Build: `tooling/k27/build-native-piano-usbhost.sh` (host tests, a store check of the survey headers, the objects, the import check).
PianoUsbHost is in `configs/k27-native-kexts.txt` and inert without `piano-usbhost`. It includes `package/PianoDwc3/PianoUsbHw.h`
and `package/PianoPower/{SpmiObserver.h,ChargerPmic.h}` (not copied). Sources and licences: `UPSTREAM`, `LICENSE`
(BSD-2-Clause-Patent).
