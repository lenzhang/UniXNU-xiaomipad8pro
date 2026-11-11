# PianoDwc3

A USB device-mode debug channel for native macOS 27 on piano: the DWC3 controller behind the USB-C port, taken over from
the UEFI in device mode, enumerates on the Mac as CDC-ACM serial ports.

- port 0, the kernel console: the console log streamed live (replayed from boot when the Mac opens the port) and a kernel
  monitor (`help`, `usb`, `ioreg`, `peek`, `args`, `reset`, `panic`, ...);
- port 1, the user pipe `/dev/pianousb` (`piano-usb-shell=1`), for `tooling/native/pianousb-shell` (a shell) or `log stream`;
- port 2, only with `piano-usb-hid=1`: input from the Mac (`tooling/native/piano-input.py`) into a keyboard and a pointer HID
  device (`PianoUsbHID`, PianoKeyboard's report descriptors);
- only with `piano-usb-net=1`: a CDC-NCM network function after the ports, an Ethernet link to the Mac (the Mac's own NCM
  driver; on the tablet `PianoUsbNet`, an IOEthernetController, not built-in: en0 stays PianoNullEthernet's).

Design, hardware facts, boot arguments, test plan and the Mac-side commands: `docs/drivers/usb-device-debug.md`.

| File | What |
|---|---|
| `PianoDwc3.cpp` | the IOService on IOPlatformExpertDevice; read-only probe, takeover, log tail, monitor, `/dev/pianousb`, the HID devices and their command thread |
| `PianoUsbNet.cpp`, `UsbNet.h` | the network function's macOS side: the IOEthernetController, its frame queues and receive thread (`piano-usb-net=1`) |
| `Dwc3Gadget.h` | portable DWC3 gadget core: event buffer, EP0 (chapter 9, CDC-ACM and CDC-NCM requests), bulk transfers, NTBs, EP0 trace and watchdog |
| `UsbDescriptors.h` | composite device, two or three ACM functions (IAD) and optionally the NCM function, USB 2.0 high/full speed |
| `Ncm.h` | NTB16 writer and parser, NTB parameters, NCM request codes, the frame queue, portable |
| `TxFifoPlan.h` | the TX FIFO layout of the IN endpoints in use (with the network function), portable |
| `HidInput.h` | the input port's line protocol (K/C/P/R/?) and line framing, portable |
| `SmmuStream.h` | SID 0x40 through a VA64 bypass context bank of the apps SMMU |
| `UsbIdle.h` | the USB thread's wait tiers (busy / open / configured / no host) and the core interrupt handler's decision, portable (`docs/power-irq.md`) |
| `Dwc3Regs.h`, `PianoUsbHw.h` | register facts, with their sources |
| `tests/` | host tests: the gadget core (2 and 3 ports, without and with the network function; the ACM-only descriptors against their pre-network bytes), NTB16 and the TX FIFO plan, the SMMU planner, the input parser, the wait tiers (`tests/run.sh`) |

Build: `tooling/k27/build-native-piano-dwc3.sh` (host tests, the objects, the import check, the PianoUsbNet vtable check). PianoDwc3 is in
`configs/k27-native-kexts.txt` and inert without `piano-usb`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
Battery: the USB thread's wait adapts to the link (`piano-usb-idle=0`: the old 1 ms loop); `piano-usb-irq=1` adds the core's
interrupt, SPI 133, through `package/common/PianoIrq.h` (`docs/drivers/usb-device-debug.md` section 15, `docs/power-irq.md`).
