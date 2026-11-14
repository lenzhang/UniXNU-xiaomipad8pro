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
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOBSD.h>   // kIOBSDNameKey
#include <pexpert/pexpert.h>
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
  bool fTakeEn0 = false;   // piano-wifi-en0=1: take over PianoNullEthernet's built-in en0 (its address, primary)
  bool takeOverEn0();
  // A work loop of its own (PianoUsbNet's pattern). Boot 10g: without one the controller had its provider's (IOResources,
  // none), the family's command gate never ran, and en0 was UP and RUNNING in ifconfig while enable() was never called and
  // not one frame reached outputPacket.
  IOWorkLoop *fWorkLoop = nullptr;
  // Locally administered, "PIAN"; not the unit's own. The same as WifiHw.h kLocalMac, the virtual device's address when the
  // firmware reports none; never PianoNullEthernet's 0x01 (boot 10h: an interface with en0's address got no name of its own).
  // PianoWifi publishes the address the device really took as piano-wifi-mac, which wins.
  IOEthernetAddress fAddress = {{0x02, 0x50, 0x49, 0x41, 0x4e, 0x04}};
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

 protected:
  bool createWorkLoop() override
  {
    fWorkLoop = IOWorkLoop::workLoop();
    return fWorkLoop != nullptr;
  }
  void free() override
  {
    OSSafeReleaseNULL(fWorkLoop);
    IOEthernetController::free();
  }

 public:
  IOWorkLoop *getWorkLoop() const override { return fWorkLoop; }
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
  ifnet_t ifp = fInterface ? fInterface->getIfnet() : nullptr;
  const uint32_t flags = ifp ? uint32_t(ifnet_flags(ifp)) : 0;
  OSString *name = fInterface ? OSDynamicCast(OSString, fInterface->getProperty(kIOBSDNameKey)) : nullptr;
  note("%s: %s (%s), interface flags %#x (up %u, running %u), enables %llu; out %llu sent, dropped %llu (link down %llu, "
       "size %llu, refused %llu, last %d); in %llu, dropped %llu",
       why, name ? name->getCStringNoCopy() : "no BSD name yet", ifp ? "attached to the network stack" : "not attached",
       flags, (flags & IFF_UP) ? 1u : 0u, (flags & IFF_RUNNING) ? 1u : 0u, fEnables, fTxFrames, fTxDrops, fTxDown, fTxSize,
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
  const size_t length = mbuf_pkthdr_len(packet);
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

// piano-wifi-en0=1: the Wi-Fi interface becomes the built-in en0. PianoNullEthernet publishes en0 at boot (TimeSync takes
// its clock identifier from a built-in interface's address once, at its start, and WindowServer waits for that); once level
// 6 is up and this driver starts, PianoNullEthernet is terminated, its interface leaves the network stack, and this one
// registers with the same address (02:50:49:41:4e:01, which PianoWifi then gives the firmware's virtual device too), built
// in and primary — so configd's saved naming gives it en0 again. Off by default: the interface takes its own address and the
// next free unit (boot 10i). Returns true once no interface is named en0 any more.
// The stack's own view of en0: an ifnet of that name (ifnet_find_by_name), and an interface in the registry that carries it.
static bool ifnetNamed(const char *name)
{
  ifnet_t ifp = nullptr;
  if (ifnet_find_by_name(name, &ifp) != 0 || !ifp) return false;
  ifnet_release(ifp);
  return true;
}

static bool registryNamed(const char *name)
{
  OSDictionary *named = IOService::serviceMatching("IONetworkInterface");
  OSDictionary *props = OSDictionary::withCapacity(1);
  OSString *bsd = OSString::withCString(name);
  IOService *found = nullptr;
  if (named && props && bsd) {
    props->setObject(kIOBSDNameKey, bsd);
    named->setObject(gIOPropertyMatchKey, props);
    found = IOService::copyMatchingService(named);
  }
  OSSafeReleaseNULL(named);
  OSSafeReleaseNULL(props);
  OSSafeReleaseNULL(bsd);
  if (!found) return false;
  found->release();
  return true;
}

// The null controller's own interface (its child in the service plane), retained, or null once it is gone.
static IONetworkInterface *nullInterface(IOService *null)
{
  IONetworkInterface *found = nullptr;
  if (OSIterator *children = null->getChildIterator(gIOServicePlane)) {
    while (OSObject *child = children->getNextObject()) {
      IONetworkInterface *iface = OSDynamicCast(IONetworkInterface, child);
      if (iface && !iface->isInactive()) {
        found = iface;
        found->retain();
        break;
      }
    }
    children->release();
  }
  return found;
}

bool PianoWifiEthernet::takeOverEn0()
{
  // PianoNullEthernet never calls registerService() on itself (only its interface is registered), so matching does not find
  // it (boot 10j: "no PianoNullEthernet to stand down"): it is looked for among IOResources' children by its class.
  IOService *null = nullptr;
  if (IOService *resources = IOService::getResourceService()) {
    if (OSIterator *children = resources->getChildIterator(gIOServicePlane)) {
      while (OSObject *child = children->getNextObject()) {
        IOService *svc = OSDynamicCast(IOService, child);
        if (svc && svc->getMetaClass() && strcmp(svc->getMetaClass()->getClassName(), "PianoNullEthernet") == 0) {
          null = svc;
          null->retain();
          break;
        }
      }
      children->release();
    }
  }
  if (!null) {
    note("en0: no PianoNullEthernet to stand down (ifnet en0 %s)", ifnetNamed("en0") ? "exists" : "does not exist");
    return !ifnetNamed("en0") && !registryNamed("en0");
  }
  // Before: whether the stack has named anything en0 yet (configd names the interfaces; at this point of the boot it may not
  // have), and the null's interface with its BSD name.
  IONetworkInterface *theirs = nullInterface(null);
  OSString *theirName = theirs ? OSDynamicCast(OSString, theirs->getProperty(kIOBSDNameKey)) : nullptr;
  note("en0: before the stand-down: ifnet en0 %s, an interface named en0 in the registry %s; PianoNullEthernet's interface %s%s",
       ifnetNamed("en0") ? "exists" : "does not exist", registryNamed("en0") ? "exists" : "does not exist",
       theirs ? "is there, BSD name " : "is not there", theirs ? (theirName ? theirName->getCStringNoCopy() : "none yet") : "");
  OSSafeReleaseNULL(theirs);
  // Boots 10j and 10k-en0: terminate() left en0 the null's (its interface stayed attached and named, ours never got a name).
  // The null detaches its interface itself, synchronously; terminate() is only the fallback for a null without the call.
  const OSSymbol *standDown = OSSymbol::withCString("PianoNullEthernetStandDown");
  const IOReturn rc = standDown ? null->callPlatformFunction(standDown, true, nullptr, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
  OSSafeReleaseNULL(standDown);
  note("en0: PianoNullEthernet is asked to stand down (piano-wifi-en0=1): %s (%#x)",
       rc == kIOReturnSuccess ? "its interface was detached" : "no stand-down call: the controller is terminated instead", rc);
  if (rc != kIOReturnSuccess) null->terminate();
  // Free means the stack's real state: no ifnet named en0, no interface in the registry named en0, and the null's interface
  // gone from the registry; the times at which each came are logged.
  uint64_t ifnetGoneMs = ~0ull, nameGoneMs = ~0ull, ifaceGoneMs = ~0ull;
  bool free = false;
  unsigned waited = 0;
  for (; waited <= 10000; waited += 100) {
    const bool ifnetThere = ifnetNamed("en0"), nameThere = registryNamed("en0");
    IONetworkInterface *iface = nullInterface(null);
    const bool ifaceThere = iface != nullptr;
    OSSafeReleaseNULL(iface);
    if (!ifnetThere && ifnetGoneMs == ~0ull) ifnetGoneMs = waited;
    if (!nameThere && nameGoneMs == ~0ull) nameGoneMs = waited;
    if (!ifaceThere && ifaceGoneMs == ~0ull) ifaceGoneMs = waited;
    if (!ifnetThere && !nameThere && !ifaceThere) {
      free = true;
      break;
    }
    IOSleep(100);
  }
  if (rc == kIOReturnSuccess) null->terminate();   // the controller follows its interface (nothing of it is in use any more)
  null->release();
  auto at = [](uint64_t ms) { return ms == ~0ull ? -1 : int(ms); };
  if (free)
    note("en0: free after %u ms (ifnet en0 gone at %d ms, the registry's name at %d ms, the null's interface at %d ms): this "
         "interface is registered as the primary built-in one",
         waited, at(ifnetGoneMs), at(nameGoneMs), at(ifaceGoneMs));
  else
    note("en0: still taken after 10 s (ifnet en0 %s, the registry's name %s, the null's interface %s): this interface is "
         "registered as a secondary one",
         ifnetGoneMs == ~0ull ? "still there" : "gone", nameGoneMs == ~0ull ? "still there" : "gone",
         ifaceGoneMs == ~0ull ? "still there" : "gone");
  return free;
}

bool PianoWifiEthernet::start(IOService *provider)
{
  if (!IOEthernetController::start(provider)) return false;
  uint32_t takeEn0 = 0;
  if (!PE_parse_boot_argn("piano-wifi-en0", &takeEn0, sizeof takeEn0)) takeEn0 = 0;
  fTakeEn0 = takeEn0 != 0;
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
  // PianoNullEthernet keeps the primary built-in en0 (TimeSync), unless this interface takes it over (piano-wifi-en0=1).
  const bool en0Free = fTakeEn0 && takeOverEn0();
  fInterface->setProperty(kIOPrimaryInterface, en0Free);
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
  // The BSD name comes later, from configd (the counters' lines carry it): getUnitNumber() is 0 until then, which is why boot
  // 10a's "en0 published" was taken for the BSD name it never had.
  note("the interface is published (MAC %02x:%02x:%02x:..:%02x, built in, %s, link down until the station's port is "
       "authorized, its own work loop %s; macOS names it); PianoWifi's data path %s; the scan list is the property "
       "piano-wifi-scan",
       fAddress.bytes[0], fAddress.bytes[1], fAddress.bytes[2], fAddress.bytes[5], en0Free ? "primary (en0 was freed)" : "not primary",
       fWorkLoop ? "running" : "MISSING",
       fAttached ? "attached" : rc == ENODEV ? "not there (below level 6)" : "refused the attach");
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
