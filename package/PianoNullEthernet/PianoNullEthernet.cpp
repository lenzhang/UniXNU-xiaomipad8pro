// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoNullEthernet: a built-in en0 with no hardware behind it, for native macOS 27 on piano until a real network exists (Wi-Fi, USB).
// A fixed locally administered MAC, link up, every packet dropped.
//
// Why it exists: IOTimeSyncClockManager derives the TimeSync clock identifier from a built-in network interface's MAC. Without one the
// clock never registers, clocksyncd waits for it, and WindowServer (QuartzCore's TimeSync clock) waits on clocksyncd, so the login
// session never shows (2026-11-11 on the tablet: with this interface logind, loginwindow and WindowServer start and WindowServer opens
// the display; without it they hang). IOTimeSyncClockManager looks once, at its start: the descriptor (tooling/k27/kext/
// PianoNullEthernet.json) gates IOTimeSyncRootService on the resource published here, once the interface is matched.
//
// The controller of the VM-era Ethernet kext without its virtio-net transport (provenance: UPSTREAM).
#include <IOKit/IOLib.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"   // the family's headers mix anonymous enums
#define __PRIVATE_SPI__   // the shipped family uses IONetworkController's reserved slots 2-5 (outputStart ... networkInterfaceNotification)
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>   // kIOReturnOutputDropped
#pragma clang diagnostic pop

namespace {
constexpr UInt64 linkSpeed = 1000 * 1000000ULL;
}

class PianoNullEthernet : public IOEthernetController {
    OSDeclareDefaultStructors(PianoNullEthernet)

    IOEthernetInterface *fInterface = nullptr;
    IOEthernetAddress fAddress = {{0x02, 0x50, 0x49, 0x41, 0x4e, 0x01}};   // locally administered, "PIAN"
    UInt64 fTxDrops = 0;

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn getHardwareAddress(IOEthernetAddress *address) override { *address = fAddress; return kIOReturnSuccess; }
    IOReturn enable(IONetworkInterface *) override { return kIOReturnSuccess; }
    IOReturn disable(IONetworkInterface *) override { return kIOReturnSuccess; }
    // IOBSD is up (our IOResourceMatch), and the family's wait for it from start() needs IOResources matched, which it is not while
    // IOResources' own matching starts us: a deadlock whenever IOBSD is published inside IOResources' first match (seen in the source).
    UInt32 getFeatures() const override { return kIONetworkFeatureNoBSDWait; }
    UInt32 outputPacket(mbuf_t packet, void *) override { ++fTxDrops; freePacket(packet); return kIOReturnOutputDropped; }
    IOReturn setPromiscuousMode(bool) override { return kIOReturnSuccess; }
    IOReturn setMulticastMode(bool) override { return kIOReturnSuccess; }
    IOReturn setMulticastList(IOEthernetAddress *, UInt32) override { return kIOReturnSuccess; }
};

OSDefineMetaClassAndStructors(PianoNullEthernet, IOEthernetController)

bool PianoNullEthernet::start(IOService *provider)
{
    if (!IOEthernetController::start(provider))
        return false;
    OSDictionary *media = OSDictionary::withCapacity(1);
    IONetworkMedium *medium = IONetworkMedium::medium(IOMediumType(kIOMediumEthernetAuto) | IOMediumType(kIOMediumOptionFullDuplex), linkSpeed);
    if (!media || !medium || !IONetworkMedium::addMedium(media, medium) || !publishMediumDictionary(media)) {
        OSSafeReleaseNULL(media);
        OSSafeReleaseNULL(medium);
        IOLog("PianoNullEthernet: cannot publish the medium\n");
        return false;
    }
    media->release();
    setSelectedMedium(medium);
    setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, medium, linkSpeed);
    medium->release();
    if (!attachInterface(reinterpret_cast<IONetworkInterface **>(&fInterface), false)) {
        IOLog("PianoNullEthernet: cannot attach the interface\n");
        return false;
    }
    // IOTimeSyncClockManager takes the clock identifier from the built-in interfaces only (the interface copies IOBuiltin from the
    // controller's provider, IOResources here): mark it built-in, register it, then release IOTimeSyncRootService's gate.
    fInterface->setProperty(kIOBuiltin, true);
    fInterface->registerService(kIOServiceSynchronous);
    publishResource("PianoNullEthernet");
    const uint8_t *m = fAddress.bytes;
    IOLog("PianoNullEthernet: en0 (built-in, no hardware: every packet dropped), MAC %02x:%02x:%02x:%02x:%02x:%02x; TimeSync released\n",
          m[0], m[1], m[2], m[3], m[4], m[5]);
    return true;
}

void PianoNullEthernet::stop(IOService *provider)
{
    if (fInterface) {
        detachInterface(fInterface);
        OSSafeReleaseNULL(fInterface);
    }
    IOEthernetController::stop(provider);
}
