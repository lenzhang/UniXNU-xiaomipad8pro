# PianoUsbHostStandIn

The stand-in (`scripts/kc-build.py` `stands_in_for`) for seven Apple SoC kexts that Apple's USB host stack reaches through its
libraries and piano does not have: AppleSMC, RTBuddy, AppleA7IOP, AppleARMPMU, AppleEmbeddedTempSensor, AppleOnboardSerial and
IOSlowAdaptiveClockingFamily (IOUSBHostFamily -> AppleSMC -> the rest; IOAccessoryManager -> AppleOnboardSerial). The kept kexts refer
to them only through two metaclasses, `AppleSMCFamily` and `AppleOnboardSerialSync`, which this kext defines as classes no object
belongs to. The link report's other eleven symbols, AppleARMPlatform's `gAppleARMUSBCable*` keys, are in
`package/ARMPlatformStandIn/UsbCableKeys.cpp`, compiled into ARMPlatformStandIn for the same collection only.

Linked only with `PIANO_USBHOST_STACK=1 tooling/k27/build-native-image.sh` (descriptor `tooling/k27/kext/PianoUsbHostStandIn.json`,
build `tooling/k27/build-native-usbhost-standin.sh`), next to the codeless `tooling/k27/kext/PianoUsbHostStack.json`; loaded only when
the stack loads, which only `piano-usbhost=6` (PianoUsbHost) lets happen. Design and the link measurements: `docs/drivers/usb-host.md`
sections 5.2 and 8 (H6). Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
