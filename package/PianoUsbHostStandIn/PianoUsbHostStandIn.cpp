// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stand-in for seven Apple SoC kexts Apple's USB host stack reaches through its libraries (scripts/kc-build.py "stands_in_for";
// docs/drivers/usb-host.md section 5.2): IOUSBHostFamily -> AppleSMC -> RTBuddy / AppleA7IOP / AppleARMPMU /
// AppleEmbeddedTempSensor, IOAccessoryManager -> AppleOnboardSerial, and IOSlowAdaptiveClockingFamily. They drive Apple's own SoC
// blocks (the SMC, its RTKit coprocessor, the PMU, the serial ports), which piano does not have. The link report (kc-build27-piano.py
// with the stack kept, "stand-ins lack:") names what the kept kexts reference in these seven: two metaclasses, by pointer only (no
// branch into them). So the stand-in gives what an absent device gives: classes no object belongs to (an OSDynamicCast against them
// fails, a personality on them never matches). The report's other eleven symbols, the gAppleARMUSBCable* keys, are AppleARMPlatform's:
// ARMPlatformStandIn defines them in the stack build (package/ARMPlatformStandIn/UsbCableKeys.cpp). Nothing here runs.
// Linked only with PIANO_USBHOST_STACK=1 (tooling/k27/build-native-image.sh), loaded only when the stack loads (piano-usbhost=6).
#include <IOKit/IOService.h>

class AppleSMCFamily : public IOService {
  OSDeclareDefaultStructors(AppleSMCFamily)
};
OSDefineMetaClassAndStructors(AppleSMCFamily, IOService)

class AppleOnboardSerialSync : public IOService {
  OSDeclareDefaultStructors(AppleOnboardSerialSync)
};
OSDefineMetaClassAndStructors(AppleOnboardSerialSync, IOService)
