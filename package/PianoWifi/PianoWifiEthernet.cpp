// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifiEthernet: the macOS side of the Wi-Fi path as docs/drivers/wifi.md section 5.2 B settles it — **an Ethernet-like
// interface, not a Wi-Fi one**. This is the skeleton of it: an IOEthernetController that publishes `en1` (built in, not the
// primary interface) with its **link down**, carrying the networks PianoWifi's level 6 scan found as the registry property
// `piano-wifi-scan`. No frame flows through it yet: a station has to associate first, which is levels 7 and 8.
//
// Why a separate kext: the controller needs IONetworkingFamily, and PianoWifi — which owns the chip, the rails and the DMA
// window — needs nothing of it. This one has no hardware of its own: it matches the resource `PianoWifiLink`, which PianoWifi
// publishes only when level 6 came up, so without `piano-wifi=6` (or when the level failed) macOS never sees the interface.
// Its shape is PianoNullEthernet's, which is already vtable-checked in this collection (tooling/k27/kext/
// PianoWifiEthernet.json carries the same check).
//
// The address: this unit's own is in the `persist` partition (never logged, never committed), and the firmware's ready event
// reports zeros on this chip. Until the read-at-run-time path of section 13.1 is built, the interface takes the locally
// administered address below, which is piano's own made-up one and is not the unit's. When PianoWifi does publish an address
// (the property `piano-wifi-mac`, from the firmware's ready event when it is not all zeros), this driver uses that instead.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"   // the family's headers mix anonymous enums
#define __PRIVATE_SPI__   // the shipped family uses IONetworkController's reserved slots 2-5, as PianoNullEthernet does
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>   // kIOReturnOutputDropped
#pragma clang diagnostic pop

namespace {
constexpr UInt64 kLinkSpeed = 150 * 1000000ULL;      // what a 2.4 GHz link of this chip reaches; the link is down regardless
constexpr uint64_t kWaitNs = 2ULL * 1000 * 1000 * 1000;

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoWifiEthernet: %s\n", line);
}
}  // namespace

class PianoWifiEthernet : public IOEthernetController {
  OSDeclareDefaultStructors(PianoWifiEthernet)

  IOEthernetInterface *fInterface = nullptr;
  // Locally administered, "PIAN"; not the unit's own. The same as WifiHw.h kLocalMac, the virtual device's address when the
  // firmware reports none: en1 must carry the station's address (a frame from it with another source address is the
  // firmware's to drop). PianoWifi publishes the address the device really took as piano-wifi-mac, which wins.
  IOEthernetAddress fAddress = {{0x02, 0x50, 0x49, 0x41, 0x4e, 0x01}};
  UInt64 fTxDrops = 0;

  void takeScanList(IOService *wifi);

 public:
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
  IOReturn getHardwareAddress(IOEthernetAddress *address) override
  {
    *address = fAddress;
    return kIOReturnSuccess;
  }
  IOReturn enable(IONetworkInterface *) override { return kIOReturnSuccess; }
  IOReturn disable(IONetworkInterface *) override { return kIOReturnSuccess; }
  UInt32 getFeatures() const override { return kIONetworkFeatureNoBSDWait; }
  // Nothing is associated, so nothing can be carried: every frame is dropped and counted (the counter is in the registry).
  UInt32 outputPacket(mbuf_t packet, void *) override
  {
    ++fTxDrops;
    freePacket(packet);
    return kIOReturnOutputDropped;
  }
  IOReturn setPromiscuousMode(bool) override { return kIOReturnSuccess; }
  IOReturn setMulticastMode(bool) override { return kIOReturnSuccess; }
  IOReturn setMulticastList(IOEthernetAddress *, UInt32) override { return kIOReturnSuccess; }
};

OSDefineMetaClassAndStructors(PianoWifiEthernet, IOEthernetController)

// What level 6 found, copied onto this driver so that `ioreg -c PianoWifiEthernet` (and tooling/native/piano-wifi) finds the
// list on the interface's own driver as well as on PianoWifi. The addresses are whole, as the list a later join needs; a log
// line never carries them (docs/drivers/wifi.md section 11).
void PianoWifiEthernet::takeScanList(IOService *wifi)
{
  if (!wifi) return;
  if (OSObject *scan = wifi->copyProperty("piano-wifi-scan")) {
    setProperty("piano-wifi-scan", scan);
    if (OSArray *list = OSDynamicCast(OSArray, scan)) note("the scan list has %u network(s)", list->getCount());
    scan->release();
  }
  if (OSObject *summary = wifi->copyProperty("piano-wifi-scan-summary")) {
    setProperty("piano-wifi-scan-summary", summary);
    summary->release();
  }
  if (OSObject *mac = wifi->copyProperty("piano-wifi-mac")) {
    OSData *bytes = OSDynamicCast(OSData, mac);
    if (bytes && bytes->getLength() == sizeof fAddress.bytes) {
      memcpy(fAddress.bytes, bytes->getBytesNoCopy(), sizeof fAddress.bytes);
      note("the interface takes the virtual device's address (its first three bytes %02x:%02x:%02x)", fAddress.bytes[0],
           fAddress.bytes[1], fAddress.bytes[2]);
    }
    mac->release();
  }
}

bool PianoWifiEthernet::start(IOService *provider)
{
  if (!IOEthernetController::start(provider)) return false;
  // PianoWifi is the service that published the resource this driver matched: its properties are the scan's.
  OSDictionary *matching = IOService::serviceMatching("PianoWifi");
  IOService *wifi = matching ? IOService::waitForMatchingService(matching, kWaitNs) : nullptr;
  OSSafeReleaseNULL(matching);
  takeScanList(wifi);
  OSSafeReleaseNULL(wifi);

  OSDictionary *media = OSDictionary::withCapacity(1);
  IONetworkMedium *medium = IONetworkMedium::medium(IOMediumType(kIOMediumEthernetAuto) | IOMediumType(kIOMediumOptionFullDuplex),
                                                    kLinkSpeed);
  if (!media || !medium || !IONetworkMedium::addMedium(media, medium) || !publishMediumDictionary(media)) {
    OSSafeReleaseNULL(media);
    OSSafeReleaseNULL(medium);
    note("cannot publish the medium");
    return false;
  }
  media->release();
  setSelectedMedium(medium);
  // The interface exists and is valid, but there is no link: macOS shows it down and routes nothing over it until a later
  // level associates (and then this is where setLinkStatus gains kIONetworkLinkActive).
  setLinkStatus(kIONetworkLinkValid, medium, 0);
  medium->release();
  if (!attachInterface(reinterpret_cast<IONetworkInterface **>(&fInterface), false)) {
    note("cannot attach the interface");
    return false;
  }
  // Built in, but not the primary interface: PianoNullEthernet stays en0 for TimeSync (wifi.md section 5.3), so this is en1.
  fInterface->setProperty(kIOBuiltin, true);
  fInterface->registerService(kIOServiceSynchronous);
  note("en%u published (built in, link down: the station of levels 7 and 8 is what brings it up); the scan list is the "
       "property piano-wifi-scan (tooling/native/piano-wifi prints it)",
       fInterface->getUnitNumber());
  return true;
}

void PianoWifiEthernet::stop(IOService *provider)
{
  if (fTxDrops) note("%llu frame(s) were dropped: nothing is associated yet", fTxDrops);
  if (fInterface) {
    detachInterface(fInterface);
    OSSafeReleaseNULL(fInterface);
  }
  IOEthernetController::stop(provider);
}
