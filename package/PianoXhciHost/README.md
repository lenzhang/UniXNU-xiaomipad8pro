# PianoXhciHost

Path **A2** of `docs/drivers/usb-host.md` §5: the smallest subclass of the kernel collection's generic Apple xHCI driver that
piano's Qualcomm DWC3 controller needs. No instance field, and exactly three of the class's 427 virtual slots:

| Slot | What | Why |
|---:|---|---|
| 292 | the desired/default power state: 3 (on) | without Apple's Type-C transport an idle root port would let the controller fall to a state nothing wakes it from; our role manager creates this controller only while a peripheral is on the cable |
| 419 | the 64-bit register read: two 32-bit loads, low half first | symmetry with the write; the controller does not promise to answer one 8-byte access |
| 421 | the 64-bit register write: the high half first, each store followed by a barrier | Linux's `write-64-hi-lo-quirk`, which every DWC3 host carries. In this collection the base class's 64-bit write reaches a **4-byte** helper, so a ring above 4 GiB would be latched with half an address. `regOr64` / `regAnd64` dispatch through slots 419 and 421, so these two fix them as well |

Everything else stays outside the class, in `package/PianoUsbHost`: the register window, the interrupt, the DWC3 role switch,
the PHY, VBUS, the clocks, the SMMU route for stream 0x40, and the halted and reset controller Apple's `start()` finds.
Apple's generic ARM layer (`AppleUSBXHCIARM`) and its SoC leaves are deliberately not the base class: they want
`AppleARMIODevice` resources, Apple's Type-C PHY, mapper and clock sessions, and they do not override the 64-bit accessors
anyway.

- The base-class declarations are generated at build time by `tooling/k27/xhci-abi.py` from
  `data/usb-xhci/abi-26C5035r-mac17j.json` (slot numbers, PAC diversities and names; opaque private storage). Generated
  headers and thunks are build outputs under `out/native-kexts/PianoXhciHost/`, never committed.
- `Reg64.h` holds the two accesses, templated on a register file, so the same code runs in the kext and in
  `tests/reg64_host.cpp` (the hi-lo order, the barriers, addresses above 4 GiB, the round trips).
- The build (`tooling/k27/build-native-piano-xhcihost.sh`) **fails closed on ABI drift**: the gate self-test, the host test,
  the collection check (SHA-256, all 427 slot targets and diversities, IA + address diversification, the instance size
  `0xf88`), then the compiled object's own vtable slot by slot, with only these three entries ours.
- Linked only with `PIANO_USBHOST_A2=1 tooling/k27/build-native-image.sh` (descriptor `tooling/k27/kext/PianoXhciHost.json`,
  which names `com.apple.driver.usb.AppleUSBXHCI` and `com.apple.iokit.IOUSBHostFamily`; that collection carries
  `PianoUsbHostStackA2` instead of `PianoUsbHostStack`, so path A1's codeless personality is **not** in it and only one
  automatic client can match the nub). It matches nothing until `piano-usbhost=6 piano-usbhost-a2=1` makes
  `PianoUsbHostNub`.

Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent). Design, the first tablet checklist and the results:
`docs/drivers/usb-host.md` §5.4 and §8.4.
