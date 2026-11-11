// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano: AppleARMPlatform's USB cable-type keys, compiled into ARMPlatformStandIn only for the collection that keeps Apple's USB
// host stack (PIANO_USBHOST_STACK=1, tooling/k27/build-native-standin.sh; docs/drivers/usb-host.md section 5.2). IOUSBHostFamily and
// IOAccessoryManager refer to them by pointer; the link report (kc-build27-piano.py, "stand-ins lack:") names them and places them in
// AppleARMPlatform, whose stand-in this is (a stood-in kext has one stand-in). Without the stack nothing refers to them, so the
// default build leaves this file out and ARMPlatformStandIn stays as it was (ARMPlatformStandIn.cpp is unchanged).
// Their type is taken to be that of AppleARMPlatform's other gAppleARM... keys here (const OSSymbol *); their values are ours, from the
// names and from QcomDwc3's published behaviour (BSD-2-Clause-Patent: the resource property AppleUSBCableType = "USBHost" while a USB
// host is on the cable, i.e. in device mode, removed otherwise): inferred, not read from Apple's code. PianoUsbHost level 6 logs what
// the resource service holds.
#include <libkern/c++/OSSymbol.h>

extern "C" {
const OSSymbol *gAppleARMUSBCableDetectResourceKey;
const OSSymbol *gAppleARMUSBCableTypeResourceKey;
const OSSymbol *gAppleARMUSBCableTypeDetached;
const OSSymbol *gAppleARMUSBCableTypeUSBHost;
const OSSymbol *gAppleARMUSBCableTypeUSB3Host;
const OSSymbol *gAppleARMUSBCableTypeUSBHostAltConfig;
const OSSymbol *gAppleARMUSBCableTypeUSB3HostAltConfig;
const OSSymbol *gAppleARMUSBCableTypeUSBDevice;
const OSSymbol *gAppleARMUSBCableTypeUSB3Device;
const OSSymbol *gAppleARMUSBCableTypeUSBDeviceNeedsAuthentication;
const OSSymbol *gAppleARMUSBCableTypeUSB3DeviceNeedsAuthentication;
}

namespace {
struct CableKeys {
  CableKeys()
  {
    gAppleARMUSBCableDetectResourceKey = OSSymbol::withCStringNoCopy("AppleUSBCableDetect");
    gAppleARMUSBCableTypeResourceKey = OSSymbol::withCStringNoCopy("AppleUSBCableType");
    gAppleARMUSBCableTypeDetached = OSSymbol::withCStringNoCopy("Detached");
    gAppleARMUSBCableTypeUSBHost = OSSymbol::withCStringNoCopy("USBHost");
    gAppleARMUSBCableTypeUSB3Host = OSSymbol::withCStringNoCopy("USB3Host");
    gAppleARMUSBCableTypeUSBHostAltConfig = OSSymbol::withCStringNoCopy("USBHostAltConfig");
    gAppleARMUSBCableTypeUSB3HostAltConfig = OSSymbol::withCStringNoCopy("USB3HostAltConfig");
    gAppleARMUSBCableTypeUSBDevice = OSSymbol::withCStringNoCopy("USBDevice");
    gAppleARMUSBCableTypeUSB3Device = OSSymbol::withCStringNoCopy("USB3Device");
    gAppleARMUSBCableTypeUSBDeviceNeedsAuthentication = OSSymbol::withCStringNoCopy("USBDeviceNeedsAuthentication");
    gAppleARMUSBCableTypeUSB3DeviceNeedsAuthentication = OSSymbol::withCStringNoCopy("USB3DeviceNeedsAuthentication");
  }
} gCableKeys;   // OSKext::start runs the constructors before any kext that lists this one as a library starts
}  // namespace
