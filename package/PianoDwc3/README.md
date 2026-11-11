# PianoDwc3

A USB device-mode debug channel for native macOS 27 on piano: the DWC3 controller behind the USB-C port, taken over from
the UEFI in device mode, enumerates on the Mac as CDC-ACM serial ports.

- port 0, the kernel console: the console log streamed live (replayed from boot when the Mac opens the port) and a kernel
  monitor (`help`, `usb`, `ioreg`, `peek`, `args`, `reset`, `panic`, ...);
- port 1, the user pipe `/dev/pianousb` (`piano-usb-shell=1`), for `tooling/native/pianousb-shell` (a shell) or `log stream`;
- port 2, only with `piano-usb-hid=1`: input from the Mac (`tooling/native/piano-input.py`) into a keyboard and a pointer HID
  device (`PianoUsbHID`, PianoKeyboard's report descriptors).

Design, hardware facts, boot arguments, test plan and the Mac-side commands: `docs/drivers/usb-device-debug.md`.

| File | What |
|---|---|
| `PianoDwc3.cpp` | the IOService on IOPlatformExpertDevice; read-only probe, takeover, log tail, monitor, `/dev/pianousb`, the HID devices and their command thread |
| `Dwc3Gadget.h` | portable DWC3 gadget core: event buffer, EP0 (chapter 9, CDC-ACM requests), bulk transfers, EP0 trace and watchdog |
| `UsbDescriptors.h` | composite device, two or three ACM functions (IAD), USB 2.0 high/full speed |
| `HidInput.h` | the input port's line protocol (K/C/P/R/?) and line framing, portable |
| `SmmuStream.h` | SID 0x40 through a VA64 bypass context bank of the apps SMMU |
| `Dwc3Regs.h`, `PianoUsbHw.h` | register facts, with their sources |
| `tests/` | host tests: the gadget core (2 and 3 ports) and the SMMU planner against register models, the input parser against PianoKeyboard's descriptors (`tests/run.sh`) |

Build: `tooling/k27/build-native-piano-dwc3.sh` (host tests, the objects, the import check). PianoDwc3 is in
`configs/k27-native-kexts.txt` and inert without `piano-usb`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
