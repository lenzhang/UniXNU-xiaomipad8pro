// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifiEthernet: the macOS side of the Wi-Fi path as docs/drivers/wifi.md section 5.2 B settles it — **an Ethernet-like
// interface, not a Wi-Fi one**: an IOEthernetController that publishes the interface (en0 on the 27.3 line, boot 10b; en1 where
// PianoNullEthernet takes en0), built in, carrying the networks PianoWifi's level 6 scan found as the registry property
// `piano-wifi-scan`. With PianoWifi's level 8 it carries the station's frames (802.3 both ways) through PianoWifi's network
// boundary (PianoWifiNet.h): it attaches with piano_wifi_net_attach, its output goes to piano_wifi_net_send, PianoWifi's thread
// hands it the received frames and the link's state (up once the port is authorized, down when the association ends). EAPOL
// frames never come here: they are the 802.1X port's (docs §13.7).
//
// Why a separate kext: the controller needs IONetworkingFamily, and PianoWifi — which owns the chip, the rails and the DMA
// window — needs nothing of it. This one has no hardware of its own: it matches the resource `PianoWifiLink`, which PianoWifi
// publishes only when level 6 came up, so without `piano-wifi=6` (or when the level failed) macOS never sees the interface.
// It links against PianoWifi (a library dependency) for the three boundary functions. Its shape is PianoNullEthernet's, which
// is already vtable-checked in this collection (tooling/k27/kext/PianoWifiEthernet.json carries the same check).
//
// The address: the interface takes the virtual device's (PianoWifi's property `piano-wifi-mac`), which the station's frames
// carry as their source; until that is there it keeps the locally administered address below, piano's own made-up one and
// not the unit's (the unit's own is in the `persist` partition, never logged, never committed).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <sys/errno.h>
#include <sys/kpi_mbuf.h>
#include <net/kpi_interface.h>
#include <net/if.h>   // IFF_UP, IFF_RUNNING
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"   // the family's headers mix anonymous enums
#define __PRIVATE_SPI__   // the shipped family uses IONetworkController's reserved slots 2-5, as PianoNullEthernet does
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>   // kIOReturnOutputDropped
#pragma clang diagnostic pop
#include "PianoWifiNet.h"

namespace {
constexpr UInt64 kLinkSpeed = 150 * 1000000ULL;      // what a 2.4 GHz link of this chip reaches; macOS shows it, nothing more
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
  IONetworkMedium *fMedium = nullptr;
  // Locally administered, "PIAN"; not the unit's own. The same as WifiHw.h kLocalMac, the virtual device's address when the
  // firmware reports none. PianoWifi publishes the address the device really took as piano-wifi-mac, which wins.
  IOEthernetAddress fAddress = {{0x02, 0x50, 0x49, 0x41, 0x4e, 0x01}};
  bool fAttached = false, fLinkUp = false;
  UInt64 fTxFrames = 0, fTxDrops = 0, fRxFrames = 0, fRxDrops = 0, fLinkChanges = 0;
  // after boot 10f-c (the port open, en0 "link up", and not one frame of en0's ever handed to PianoWifi): why each output was
  // dropped, whether the stack brought the interface up at all, and the first frames each way (length, type, group bit)
  UInt64 fTxDown = 0, fTxSize = 0, fTxRefused = 0, fEnables = 0, fDisables = 0;
  int fLastRefusal = 0;
  unsigned fTxLogged = 0, fRxLogged = 0;
  void counters(const char *why);
  uint8_t fTxBuffer[PIANO_WIFI_NET_FRAME_MAX];   // one output at a time (the family serializes outputPacket)

  void takeScanList(IOService *wifi);
  static void netInput(void *ctx, const uint8_t *frame, uint32_t length);
  static void netLink(void *ctx, int up, const uint8_t *mac);

 public:
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
  IOReturn getHardwareAddress(IOEthernetAddress *address) override
  {
    *address = fAddress;
    return kIOReturnSuccess;
  }
  IOReturn enable(IONetworkInterface *) override
  {
    ++fEnables;
    note("the interface is brought up (enable %llu): the stack may send now%s", fEnables, fLinkUp ? "" : " (the link is down)");
    return kIOReturnSuccess;
  }
  IOReturn disable(IONetworkInterface *) override
  {
    ++fDisables;
    note("the interface is taken down (disable %llu)", fDisables);
    return kIOReturnSuccess;
  }
  UInt32 getFeatures() const override { return kIONetworkFeatureNoBSDWait; }
  UInt32 outputPacket(mbuf_t packet, void *) override;
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

void PianoWifiEthernet::counters(const char *why)
{
  const uint32_t flags = fInterface ? uint32_t(ifnet_flags(fInterface->getIfnet())) : 0;
  note("%s: interface flags %#x (up %u, running %u), enables %llu; out %llu sent, dropped %llu (link down %llu, size %llu, "
       "refused %llu, last %d); in %llu, dropped %llu",
       why, flags, (flags & IFF_UP) ? 1u : 0u, (flags & IFF_RUNNING) ? 1u : 0u, fEnables, fTxFrames, fTxDrops, fTxDown, fTxSize,
       fTxRefused, fLastRefusal, fRxFrames, fRxDrops);
}

// A received frame, on PianoWifi's thread (one at a time): into an mbuf of the family's and up the stack.
void PianoWifiEthernet::netInput(void *ctx, const uint8_t *frame, uint32_t length)
{
  auto *self = static_cast<PianoWifiEthernet *>(ctx);
  if (!self || !self->fInterface || !frame || length < 14 || length > PIANO_WIFI_NET_FRAME_MAX) return;
  mbuf_t m = self->allocatePacket(length);
  if (!m) {
    ++self->fRxDrops;
    return;
  }
  if (mbuf_copyback(m, 0, length, frame, MBUF_DONTWAIT) != 0) {
    self->freePacket(m);
    ++self->fRxDrops;
    return;
  }
  self->fInterface->inputPacket(m, length);
  ++self->fRxFrames;
  if (self->fRxLogged < 6) {
    ++self->fRxLogged;
    note("in: frame %llu, %u bytes, type %#06x, %s", self->fRxFrames, length, unsigned(frame[12] << 8 | frame[13]),
         (frame[0] & 1) ? "group addressed" : "to us");
  }
  if ((self->fRxFrames & 255) == 0) self->counters("every 256 frames in");
}

// The link's state, on PianoWifi's thread (and once from the attach).
void PianoWifiEthernet::netLink(void *ctx, int up, const uint8_t *mac)
{
  auto *self = static_cast<PianoWifiEthernet *>(ctx);
  if (!self) return;
  const bool now = up != 0;
  if (mac && now) {
    bool same = true;
    for (unsigned i = 0; i < 6; ++i) same = same && mac[i] == self->fAddress.bytes[i];
    if (!same) note("the station's address is not the interface's: its frames would carry the wrong source (the interface "
                    "keeps its own; docs §13.7)");
  }
  if (now == self->fLinkUp) return;
  self->fLinkUp = now;
  ++self->fLinkChanges;
  self->setLinkStatus(now ? (kIONetworkLinkValid | kIONetworkLinkActive) : kIONetworkLinkValid, self->fMedium,
                      now ? kLinkSpeed : 0);
  note("the link is %s (PianoWifi's port %s)", now ? "up" : "down", now ? "is authorized" : "closed: the association ended");
  self->counters(now ? "at the link's up" : "at the link's down");
}

UInt32 PianoWifiEthernet::outputPacket(mbuf_t packet, void *)
{
  size_t length = 0;
  for (mbuf_t at = packet; at; at = mbuf_next(at)) length += mbuf_len(at);
  int rc = EMSGSIZE;
  if (fLinkUp && length >= 14 && length <= sizeof fTxBuffer && mbuf_copydata(packet, 0, length, fTxBuffer) == 0)
    rc = piano_wifi_net_send(fTxBuffer, uint32_t(length));
  else if (!fLinkUp)
    rc = ENETDOWN;
  freePacket(packet);
  if (fTxLogged < 6 && length >= 14) {
    ++fTxLogged;
    note("out: frame %llu, %zu bytes, type %#06x, %s: %s (%d)", fTxFrames + fTxDrops + 1, length,
         unsigned(fTxBuffer[12] << 8 | fTxBuffer[13]), (fTxBuffer[0] & 1) ? "group addressed" : "unicast",
         rc ? "dropped" : "handed to PianoWifi", rc);
  }
  if (rc) {
    ++fTxDrops;
    if (!fLinkUp) ++fTxDown;
    else if (rc == EMSGSIZE) ++fTxSize;
    else ++fTxRefused;
    fLastRefusal = rc;
    return kIOReturnOutputDropped;
  }
  ++fTxFrames;
  if ((fTxFrames & 255) == 0) counters("every 256 frames out");
  return kIOReturnOutputSuccess;
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
  fMedium = medium;   // kept for the link's changes (released in stop)
  // The interface exists and is valid, but there is no link until PianoWifi's port is authorized.
  setLinkStatus(kIONetworkLinkValid, medium, 0);
  if (!attachInterface(reinterpret_cast<IONetworkInterface **>(&fInterface), false)) {
    note("cannot attach the interface");
    return false;
  }
  fInterface->setProperty(kIOBuiltin, true);
  fInterface->registerService(kIOServiceSynchronous);
  // PianoWifi's network boundary: from here its thread may hand frames and the link's state to this driver.
  piano_wifi_net_client client;
  bzero(&client, sizeof client);
  client.version = PIANO_WIFI_NET_VERSION;
  client.ctx = this;
  client.input = &PianoWifiEthernet::netInput;
  client.link = &PianoWifiEthernet::netLink;
  const int rc = piano_wifi_net_attach(&client);
  fAttached = rc == 0;
  note("en%u published (built in, link down until the station's port is authorized); PianoWifi's data path %s; the scan list "
       "is the property piano-wifi-scan (tooling/native/piano-wifi prints it)",
       fInterface->getUnitNumber(), fAttached ? "attached" : rc == ENODEV ? "not there (below level 6)" : "refused the attach");
  return true;
}

void PianoWifiEthernet::stop(IOService *provider)
{
  if (fAttached) piano_wifi_net_detach(this);   // returns when no callback of ours runs
  fAttached = false;
  counters("at the stop");
  if (fInterface) {
    detachInterface(fInterface);
    OSSafeReleaseNULL(fInterface);
  }
  OSSafeReleaseNULL(fMedium);
  IOEthernetController::stop(provider);
}
