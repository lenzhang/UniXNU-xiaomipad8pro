// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoUsbNet: the tablet's end of PianoDwc3's CDC-NCM function (piano-usb-net=1) as an IOEthernetController. macOS gets an
// Ethernet interface whose frames travel in NTB16s over the USB-C cable; the Mac's own NCM driver shows the other end as an
// Ethernet interface (docs/drivers/usb-device-debug.md section 14).
//
// Not built-in, and not the primary interface: PianoNullEthernet stays the built-in en0 whose MAC TimeSync takes at boot (with or
// without a cable); configd names this one (en1, or the next free unit). MAC 02:50:49:41:4e:02, fixed (UsbDescriptors.h).
//
// The controller pattern is PianoNullEthernet's (DroidVMEthernet's, see UPSTREAM): no output queue, outputPacket under a lock from
// the stack's threads, inputPacket from one thread only, the same overrides plus the link from the USB side and an own work loop.
// The data path: outputPacket copies the frame into a queue the USB thread empties into the NTB IN buffer; the USB thread puts the
// datagrams of each NTB OUT into the other queue, this file's thread turns them into mbufs. Only counts are logged.
#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOWorkLoop.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"   // the family's headers mix anonymous enums
#define __PRIVATE_SPI__   // the shipped family uses IONetworkController's reserved slots 2-5 (outputStart ... networkInterfaceNotification)
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>   // kIOReturnOutputDropped
#pragma clang diagnostic pop
#include <libkern/c++/OSString.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <sys/kpi_mbuf.h>
#include "UsbNet.h"
#include "UsbDescriptors.h"

using namespace pianousb;

namespace {
constexpr uint32_t kQueueBytes = 64 * 1024;   // each way: about 40 full frames
constexpr uint32_t kMinWireFrame = 60;        // shorter frames go out zero-padded, as a NIC sends them

IOLock *gNetLock;
ncm::FrameRing gToHost, gFromHost;
const ncm::NetStats *gGadget;
pianonet::Counters gCount;
bool gLinkUp, gLinkChanged;     // under gNetLock
uint32_t gLinkBps;
volatile bool gEnabled;         // the interface is up (enable() .. disable())
uint8_t *gFrame;                // the receive thread's frame buffer

uint64_t nowNs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns;
}
}  // namespace

class PianoUsbNet : public IOEthernetController {
  OSDeclareDefaultStructors(PianoUsbNet)

  IOEthernetInterface *fInterface = nullptr;
  IOWorkLoop *fWorkLoop = nullptr;
  IONetworkMedium *fMedium = nullptr;

protected:
  bool createWorkLoop() override
  {
    fWorkLoop = IOWorkLoop::workLoop();
    return fWorkLoop != nullptr;
  }

public:
  bool start(IOService *provider) override;
  IOWorkLoop *getWorkLoop() const override { return fWorkLoop; }
  IOReturn getHardwareAddress(IOEthernetAddress *address) override
  {
    for (unsigned i = 0; i < 6; ++i) address->bytes[i] = kNetDeviceMac[i];
    return kIOReturnSuccess;
  }
  IOReturn enable(IONetworkInterface *) override
  {
    gEnabled = true;
    return kIOReturnSuccess;
  }
  IOReturn disable(IONetworkInterface *) override
  {
    gEnabled = false;
    IOLockLock(gNetLock);
    gToHost.clear();
    IOLockUnlock(gNetLock);
    return kIOReturnSuccess;
  }
  // The thread that starts us waited for IOBSD already; the family's own wait would only cost the same check again.
  UInt32 getFeatures() const override { return kIONetworkFeatureNoBSDWait; }
  UInt32 outputPacket(mbuf_t packet, void *param) override;
  IOReturn setPromiscuousMode(bool) override { return kIOReturnSuccess; }   // a point-to-point link: the Mac sends what is ours
  IOReturn setMulticastMode(bool) override { return kIOReturnSuccess; }
  IOReturn setMulticastList(IOEthernetAddress *, UInt32) override { return kIOReturnSuccess; }
  const OSString *newVendorString() const override { return OSString::withCString("UniXNU"); }
  const OSString *newModelString() const override { return OSString::withCString("piano USB network (CDC-NCM)"); }

  void applyLink(bool up, uint32_t bps)
  {
    setLinkStatus(up ? kIONetworkLinkValid | kIONetworkLinkActive : kIONetworkLinkValid, up ? fMedium : nullptr, up ? bps : 0);
  }
  void deliver(const uint8_t *frame, uint32_t length);
  IOEthernetInterface *interface() const { return fInterface; }
};

OSDefineMetaClassAndStructors(PianoUsbNet, IOEthernetController)

bool PianoUsbNet::start(IOService *provider)
{
  if (!IOEthernetController::start(provider)) return false;
  OSDictionary *media = OSDictionary::withCapacity(1);
  fMedium = IONetworkMedium::medium(IOMediumType(kIOMediumEthernetAuto) | IOMediumType(kIOMediumOptionFullDuplex), 480 * 1000000ULL);
  if (!media || !fMedium || !IONetworkMedium::addMedium(media, fMedium) || !publishMediumDictionary(media)) {
    OSSafeReleaseNULL(media);
    IOLog("PianoDwc3: net: cannot publish the medium\n");
    return false;
  }
  media->release();
  setSelectedMedium(fMedium);
  setLinkStatus(kIONetworkLinkValid, nullptr);   // no Mac yet: the USB side reports the link
  if (!attachInterface(reinterpret_cast<IONetworkInterface **>(&fInterface), false)) {
    IOLog("PianoDwc3: net: cannot attach the interface\n");
    return false;
  }
  fInterface->setProperty(kIOBuiltin, false);
  fInterface->setProperty(kIOPrimaryInterface, false);
  fInterface->registerService();
  return true;
}

// From the stack's threads (no output queue). A frame is copied into the queue for the USB thread, or dropped when the queue is
// full or the Mac does not run the link.
UInt32 PianoUsbNet::outputPacket(mbuf_t packet, void *)
{
  const size_t length = mbuf_pkthdr_len(packet);
  bool queued = false;
  IOLockLock(gNetLock);
  if (length < ncm::kMinFrame || length > ncm::kMaxFrame) {
    ++gCount.sendRefused;
  } else if (gLinkUp && gEnabled &&
             gToHost.putWith(uint32_t(length), [packet](uint8_t *d, uint32_t off, uint32_t n) { return mbuf_copydata(packet, off, n, d) == 0; })) {
    queued = true;
    ++gCount.sent;
    gCount.sentBytes += length;
  } else {
    ++gCount.sendDrops;
  }
  IOLockUnlock(gNetLock);
  freePacket(packet);
  return queued ? kIOReturnOutputSuccess : kIOReturnOutputDropped;
}

// The receive thread only (inputPacket calls must be serialized).
void PianoUsbNet::deliver(const uint8_t *frame, uint32_t length)
{
  if (!fInterface || !gEnabled) { ++gCount.notUp; return; }
  mbuf_t m = allocatePacket(length);
  if (!m || mbuf_copyback(m, 0, length, frame, MBUF_DONTWAIT) != 0) {
    if (m) freePacket(m);
    ++gCount.receiveDrops;
    return;
  }
  fInterface->inputPacket(m, length);
  ++gCount.received;
  gCount.receivedBytes += length;
}

namespace {
PianoUsbNet *gNet;

PianoUsbNet *publish(IOService *owner)
{
  PianoUsbNet *net = OSTypeAlloc(PianoUsbNet);
  if (!net || !net->init(nullptr)) {
    OSSafeReleaseNULL(net);
    IOLog("PianoDwc3: net: the controller: init failed\n");
    return nullptr;
  }
  if (!net->attach(owner)) {
    OSSafeReleaseNULL(net);
    IOLog("PianoDwc3: net: the controller: attach failed\n");
    return nullptr;
  }
  if (!net->start(owner)) {
    net->detach(owner);
    OSSafeReleaseNULL(net);
    IOLog("PianoDwc3: net: the controller: start failed\n");
    return nullptr;
  }
  const uint8_t *m = kNetDeviceMac;
  IOLog("PianoDwc3: net: interface published (MAC %02x:%02x:%02x:%02x:%02x:%02x, not built-in; configd names it), registry ID %#llx\n",
        m[0], m[1], m[2], m[3], m[4], m[5], net->getRegistryEntryID());
  return net;
}

const char *bsdName()
{
  IOEthernetInterface *i = gNet ? gNet->interface() : nullptr;
  OSString *name = i ? OSDynamicCast(OSString, i->getProperty(kIOBSDNameKey)) : nullptr;
  return name ? name->getCStringNoCopy() : nullptr;
}

void counts(char *line, unsigned size)
{
  const ncm::NetStats z = {}, &g = gGadget ? *gGadget : z;
  snprintf(line, size, "link %s, %s %s; to the Mac %llu frames %llu bytes (%u NTBs), from the Mac %llu frames %llu bytes (%u NTBs); "
           "NTB errors %u%s%s, bad datagrams %u; drops: send %llu (+%llu bad length), receive %u (queue) %llu (no mbuf) %llu (not up)",
           g.alt == 1 ? "up" : "down", bsdName() ? bsdName() : "(no name yet)", gEnabled ? "up" : "down", gCount.sent, gCount.sentBytes,
           g.txNtbs, gCount.received, gCount.receivedBytes, g.rxNtbs, g.ntbErrors, g.lastError ? ", last: " : "",
           g.lastError ? g.lastError : "", g.badDatagrams, gCount.sendDrops, gCount.sendRefused, g.rxDrops, gCount.receiveDrops, gCount.notUp);
}

// Publishes the controller once IOBSD is up (the family's interface needs BSD), then: link changes and received frames to macOS,
// one line of counts at most every 10 s while they change.
void netThread(void *arg, wait_result_t)
{
  IOService *owner = static_cast<IOService *>(arg);
  if (OSDictionary *matching = IOService::resourceMatching(kIOBSDKey)) {
    if (IOService *bsd = IOService::waitForMatchingService(matching)) bsd->release();
    matching->release();
  }
  gNet = publish(owner);
  bool named = false, first = true;
  uint64_t reported = 0, last[3] = {};
  for (;;) {
    IOLockLock(gNetLock);
    if (!gFromHost.used() && !gLinkChanged && !first) {
      uint64_t deadline = 0;
      clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
      IOLockSleepDeadline(gNetLock, &gFromHost, deadline, THREAD_UNINT);
    }
    const bool changed = gLinkChanged || first, up = gLinkUp;
    const uint32_t bps = gLinkBps;
    gLinkChanged = first = false;
    IOLockUnlock(gNetLock);
    if (changed && gNet) {
      gNet->applyLink(up, bps);
      IOLog("PianoDwc3: net: link %s%s\n", up ? "up: the Mac runs the NCM data interface" : "down", up ? (bps > 100000000 ? " (480 Mb/s)" : " (12 Mb/s)") : "");
    }
    for (unsigned k = 0; k < 256; ++k) {   // a batch, then the link again
      IOLockLock(gNetLock);
      const uint32_t n = gFromHost.get(gFrame, ncm::kMaxFrame);
      IOLockUnlock(gNetLock);
      if (!n) break;
      if (gNet) gNet->deliver(gFrame, n);
      else ++gCount.notUp;
    }
    if (!named && bsdName()) {
      named = true;
      IOLog("PianoDwc3: net: macOS named the interface %s (en0 stays PianoNullEthernet's built-in one)\n", bsdName());
    }
    const uint64_t now = nowNs(), sum[3] = {gCount.sent + gCount.sendDrops, gCount.received + gCount.receiveDrops + gCount.notUp,
                                            gGadget ? uint64_t(gGadget->ntbErrors) + gGadget->badDatagrams + gGadget->rxDrops : 0};
    if ((sum[0] != last[0] || sum[1] != last[1] || sum[2] != last[2]) && now - reported > 10000000000ull) {
      reported = now;
      for (unsigned i = 0; i < 3; ++i) last[i] = sum[i];
      char line[400];
      counts(line, sizeof line);
      IOLog("PianoDwc3: net: %s\n", line);
    }
  }
}
}  // namespace

namespace pianonet {

bool init(const ncm::NetStats *gadget)
{
  gGadget = gadget;
  gNetLock = IOLockAlloc();
  gToHost.buf = static_cast<uint8_t *>(IOMallocData(kQueueBytes));
  gFromHost.buf = static_cast<uint8_t *>(IOMallocData(kQueueBytes));
  gFrame = static_cast<uint8_t *>(IOMallocData(ncm::kMaxFrame));
  gToHost.size = gFromHost.size = kQueueBytes;
  return gNetLock && gToHost.buf && gFromHost.buf && gFrame;
}

bool startThread(IOService *owner)
{
  thread_t thread = nullptr;
  if (kernel_thread_start(netThread, owner, &thread) != KERN_SUCCESS) return false;
  thread_deallocate(thread);
  return true;
}

unsigned txNext(uint8_t *dst, unsigned room)
{
  IOLockLock(gNetLock);
  const uint32_t length = gToHost.peek(), wire = length < kMinWireFrame ? kMinWireFrame : length;
  const uint32_t n = length && wire <= room ? gToHost.get(dst, length) : 0;
  IOLockUnlock(gNetLock);
  if (!n) return 0;
  for (uint32_t i = n; i < wire; ++i) dst[i] = 0;
  return wire;
}

unsigned rxSpace()
{
  IOLockLock(gNetLock);
  const unsigned n = gFromHost.space();
  IOLockUnlock(gNetLock);
  return n;
}

bool rxFrame(const uint8_t *src, unsigned length)
{
  if (!gEnabled) return false;   // nobody to take it: dropped now, nothing stale later
  IOLockLock(gNetLock);
  const bool ok = gFromHost.put(src, length);
  IOLockUnlock(gNetLock);
  return ok;
}

void rxDone()
{
  IOLockLock(gNetLock);
  IOLockWakeup(gNetLock, &gFromHost, false);
  IOLockUnlock(gNetLock);
}

void link(bool up, uint32_t bitsPerSecond)
{
  IOLockLock(gNetLock);
  gLinkUp = up;
  gLinkBps = bitsPerSecond;
  gLinkChanged = true;
  if (!up) gToHost.clear();   // nothing stale for the next session
  IOLockWakeup(gNetLock, &gFromHost, false);
  IOLockUnlock(gNetLock);
}

void status(char *line, unsigned size) { counts(line, size); }

}  // namespace pianonet
