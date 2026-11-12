// PianoDiskIdentity.h: what diskutil, Disk Utility and System Information show for piano's own disks (NativeUfs's root windows,
// NativeRamBlock's Data disk).
//
// Without these properties an IOBlockStorageDevice is "external, physical" with no protocol and no medium type: DiskArbitration
// takes a disk's location from the "Protocol Characteristics" dictionary its IOMedia finds among its parents ("Physical
// Interconnect Location" anything but External: internal), the protocol from "Physical Interconnect", the model, vendor, revision and
// "Solid State" from "Device Characteristics"; IOMedia picks its icon (Internal.icns) from the same location. A Mac's internal SSD
// publishes the pair on its block device: {"Physical Interconnect"="Apple Fabric","Physical Interconnect Location"="Internal"} and
// {"Product Name"="APPLE SSD ...","Vendor Name"="","Product Revision Level"=...,"Medium Type"="Solid State"}. The IOMedia's name is
// IOBlockStorageDriver's "<vendor string> <product string> Media", so the drivers' getVendorString/getProductString follow it too.
//
// Cosmetic only: matching, the root and Data lookups go by volume UUID, never by these names. piano-disk-identity=0 keeps the old
// identity (vendor "Native", no characteristics: "external, physical") for an A/B.
#pragma once

#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

namespace pianodisk {

constexpr const char *kVendor = "Xiaomi";
constexpr const char *kInterconnect = "UFS";   // no constant of Apple's names UFS; diskutil prints the string as the Protocol

inline bool enabled()
{
    uint32_t value = 1;
    PE_parse_boot_argn("piano-disk-identity", &value, sizeof value);
    return value != 0;
}

inline bool putString(OSDictionary *dict, const char *key, const char *value)
{
    OSString *string = OSString::withCString(value);
    const bool ok = string && dict->setObject(key, string);
    if (string) string->release();
    return ok;
}

// On the block device (the IOBlockStorageDevice) before registerService, so the IOMedia IOBlockStorageDriver makes finds them.
inline bool publish(IOService *device, const char *product, const char *revision)
{
    if (!device || !product || !enabled()) return false;
    OSDictionary *protocol = OSDictionary::withCapacity(2), *characteristics = OSDictionary::withCapacity(4);
    bool ok = protocol && characteristics &&
              putString(protocol, "Physical Interconnect", kInterconnect) &&
              putString(protocol, "Physical Interconnect Location", "Internal") &&
              putString(characteristics, "Vendor Name", kVendor) &&
              putString(characteristics, "Product Name", product) &&
              putString(characteristics, "Product Revision Level", revision ? revision : "1.0") &&
              putString(characteristics, "Medium Type", "Solid State");
    if (ok) ok = device->setProperty("Protocol Characteristics", protocol) && device->setProperty("Device Characteristics", characteristics);
    OSSafeReleaseNULL(protocol);
    OSSafeReleaseNULL(characteristics);
    return ok;
}

}  // namespace pianodisk
