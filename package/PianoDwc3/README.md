# PianoDwc3

A USB device-mode debug channel for native macOS 27 on piano: the DWC3 controller behind the USB-C port, taken over from
the UEFI in device mode, enumerates on the Mac as two CDC-ACM serial ports.

- port 0, the kernel console: the console log streamed live (replayed from boot when the Mac opens the port) and a kernel
  monitor (`help`, `usb`, `ioreg`, `peek`, `args`, `reset`, `panic`, ...);
- port 1, the user pipe `/dev/pianousb` (`piano-usb-shell=1`), for `tooling/native/pianousb-shell` (a shell) or `log stream`.

Design, hardware facts, boot arguments, test plan and the Mac-side commands: `docs/drivers/usb-device-debug.md`.

| File | What |
|---|---|
| `PianoDwc3.cpp` | the IOService on IOPlatformExpertDevice; read-only probe, takeover, log tail, monitor, `/dev/pianousb` |
| `Dwc3Gadget.h` | portable DWC3 gadget core: event buffer, EP0 (chapter 9, CDC-ACM requests), bulk transfers |
| `UsbDescriptors.h` | composite device, two ACM functions (IAD), USB 2.0 high/full speed |
| `SmmuStream.h` | SID 0x40 through a VA64 bypass context bank of the apps SMMU |
| `Dwc3Regs.h`, `PianoUsbHw.h` | register facts, with their sources |
| `tests/` | host tests of the gadget core and the SMMU planner against register models |

Build: `tooling/k27/build-piano-dwc3.sh` (host tests, then `build-native-kext.py PianoDwc3`); test KC:
`tooling/k27/build-usb-test-kc.sh`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
