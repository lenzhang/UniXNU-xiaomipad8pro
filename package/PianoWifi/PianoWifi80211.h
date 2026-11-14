// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi80211: PianoWifiEthernet as a Wi-Fi interface for airportd (docs/drivers/wifi-menu.md section 5, milestone M1), behind
// piano-wifi-apple80211=1 (default off: PianoWifiEthernet, the Ethernet-like path, stays the default and the control).
// Included once, at the end of PianoWifiEthernet.cpp: it derives from PianoWifiEthernet (declared there) and changes nothing of
// it — the data path, the link, the en0 hand-over and the scan list's copy are PianoWifiEthernet's own.
//
// What it adds, all of it public KPI or the families' own exported functions (no Apple class is subclassed, nothing is added to
// the collection):
//   - the identity airportd's enumeration and SystemConfiguration look for (private/host-checks/20261114-io80211/REPORT.md section
//     4): the controller's class chain carries the name IO80211Controller (a class of ours by that name, between PianoWifiEthernet
//     and this one: IO80211Family is not in our native collection, so the name is free; IOKit matching and IOObjectConformsTo
//     compare class names), the interface carries IO80211InterfaceRole = "Infrastructure", and its ifnet the Wi-Fi subfamily
//     (BSD type 6 / IFT_ETHER, family Ethernet, subfamily 3) through IONetworkInterface::setInterfaceSubType, IONetworkingFamily's
//     exported setter (not in the SDK header; 27.3 disassembly: it stores the value for attachToDataLinkLayer, which puts it in
//     ifnet_init_eparams.subfamily, only before the interface is attached; docs/drivers/wifi-menu.md section 5.1);
//   - the apple80211 requests: an interface filter (iflt_attach, the BSD interface-filter KPI) whose ioctl callback answers
//     SIOCSA80211 / SIOCGA80211 before the interface's own handler (WifiApple80211.h); every other ioctl passes on, its first
//     occurrence logged;
//   - the apple80211 kernel events (power changed, scan done, link down) with kev_post_msg, as UniXNU-sheng's WifiNet.cpp does;
//   - the controller registered (registerService) once the filter answers, so that launchd's IOKit match for airportd
//     (IOProviderClass IO80211Controller) can fire only when the requests are answered.
// One log line per distinct request (number, direction, length, result), a summary every 60 s while anything changed. Never a
// network's name or address in the log.
#pragma once
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/copyio.h>   // copyin / copyout of the requesting task's buffers
#include <net/kpi_interfacefilter.h>
#include <sys/kern_event.h>
#include <sys/sysctl.h>
#include "WifiApple80211.h"

// The KPI's private poster (Unsupported.exports): kev_msg_post refuses Apple's vendor code, which the apple80211 events carry.
extern "C" int piano_kev_post_msg(struct kev_msg *) __asm__("_kev_post_msg");
// IONetworkInterface::setInterfaceSubType(unsigned int), a non-virtual member exported by the 27.3 IONetworkingFamily and absent
// from the SDK's header: called with the interface as its object (x0) and the subfamily (w1); true when it took the value
// (the interface not yet attached to the network stack).
extern "C" bool piano_netif_set_subtype(IONetworkInterface *netif, unsigned int subfamily)
    __asm__("__ZN18IONetworkInterface19setInterfaceSubTypeEj");
#ifndef EJUSTRETURN
#define EJUSTRETURN (-2)   // sys/errno.h's kernel-private value: handled, the caller returns 0 and nothing after runs
#endif

// The class name airportd's launch match and SystemConfiguration's Wi-Fi test look for. Nothing but the name.
class IO80211Controller : public PianoWifiEthernet {
  OSDeclareDefaultStructors(IO80211Controller)
};
OSDefineMetaClassAndStructors(IO80211Controller, PianoWifiEthernet)

class PianoWifi80211 : public IO80211Controller {
  OSDeclareDefaultStructors(PianoWifi80211)

  using State = pianowifi::a80211::State;
  using Seen = pianowifi::a80211::Seen;
  static constexpr unsigned kAttachTickMs = 200, kAttachTicks = 900;   // the ifnet waited for up to 180 s
  static constexpr unsigned kScanDoneMs = 2000;   // a scan answered from the list completes after a short scan's time (sheng:
                                                  // answered at once, the daemon asks again straight away)
  static constexpr unsigned kReportMs = 60000;

  IOLock *fLock = nullptr;
  State *fState = nullptr;          // data allocations (no pointer in them): the state, the scratch buffer, the log's table
  uint8_t *fScratch = nullptr;
  Seen *fSeen = nullptr;
  IONetworkInterface *fNetif = nullptr;   // PianoWifiEthernet's interface (owned there), seen in configureInterface
  IOService *fWifi = nullptr;             // PianoWifi, whose registry property piano-wifi-scan is the scan list
  interface_filter_t fFilter = nullptr;
  volatile bool fFilterGone = false, fStopping = false;
  thread_call_t fAttachCall = nullptr, fScanDoneCall = nullptr, fReportCall = nullptr;
  char fIfName[16] = {};
  unsigned fAttachTries = 0;
  bool fSubtypeTaken = false, fRegistered = false, fSysctl = false;
  // counters (under fLock)
  uint64_t fRequests = 0, fUnknown = 0, fEvents = 0, fEventErrors = 0, fListRefreshes = 0, fOtherIoctls = 0;
  uint64_t fReported = ~0ull;
  uint32_t fEventCodesLogged = 0;
  unsigned long fOtherSeen[16] = {};
  unsigned fOtherCount = 0;

  static errno_t filterIoctl(void *cookie, ifnet_t, protocol_family_t, unsigned long cmd, void *arg);
  static void filterDetached(void *cookie, ifnet_t);
  static void attachTick(thread_call_param_t self, thread_call_param_t);
  static void scanDoneTick(thread_call_param_t self, thread_call_param_t);
  static void reportTick(thread_call_param_t self, thread_call_param_t);
  static int userIn(void *, uint64_t from, void *to, uint32_t length) { return copyin(from, to, length); }
  static int userOut(void *, const void *from, uint64_t to, uint32_t length) { return copyout(from, to, length); }

  void attachStep();
  errno_t apple80211(bool set, pianowifi::a80211::Req &rq);
  void refreshScanLocked();
  void postEvent(uint32_t code, const void *payload = nullptr, uint32_t length = 0);
  void scanStarted();
  void noteOtherIoctl(unsigned long cmd);
  void report(const char *why);

  void arm(thread_call_t call, unsigned ms)
  {
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    thread_call_enter_delayed(call, deadline);
  }

 protected:
  bool configureInterface(IONetworkInterface *netif) override;
  void free() override;

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  uint32_t acceptMode();
  static int sysctlAccept(struct sysctl_oid *, void *, int, struct sysctl_req *req);
  void setAcceptMode(uint32_t mode, const char *why);
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoWifi80211, IO80211Controller)

namespace {
const pianowifi::a80211::Identity kIdentity = {"PianoWifi 1.0 (apple80211 M1)", "Qualcomm WCN7860", "PianoWifi"};

void note80211(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note80211(const char *fmt, ...)
{
  char line[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoWifi80211: %s\n", line);
}
// machdep.piano_wifi_a80211 (root to write): the accept mode, read and set at run time. Filled in at run time rather than by
// SYSCTL_OID's static initializer: its parent and handler are signed pointers on arm64e (PianoWifi.cpp's pattern).
PianoWifi80211 *volatile gA80211;
struct sysctl_oid gA80211Oid;
}  // namespace

SYSCTL_DECL(_machdep);

int PianoWifi80211::sysctlAccept(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
  PianoWifi80211 *self = gA80211;
  int value = self ? int(self->acceptMode()) : -1;
  int err = SYSCTL_OUT(req, &value, sizeof value);
  if (err || !req->newptr) return err;
  int incoming = 0;
  err = SYSCTL_IN(req, &incoming, sizeof incoming);
  if (err) return err;
  if (!self) return ENODEV;
  self->setAcceptMode(uint32_t(incoming), "machdep.piano_wifi_a80211");
  return 0;
}

namespace {
void registerAcceptSysctl(PianoWifi80211 *self)
{
  gA80211 = self;
  bzero(&gA80211Oid, sizeof gA80211Oid);
  gA80211Oid.oid_parent = &sysctl__machdep_children;
  gA80211Oid.oid_number = OID_AUTO;
  gA80211Oid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
  gA80211Oid.oid_name = "piano_wifi_a80211";
  gA80211Oid.oid_handler = PianoWifi80211::sysctlAccept;
  gA80211Oid.oid_fmt = "I";
  gA80211Oid.oid_descr = "PianoWifi80211's accept mode for apple80211 requests without a layout (docs/drivers/wifi-menu.md 6)";
  gA80211Oid.oid_version = SYSCTL_OID_VERSION;
  sysctl_register_oid(&gA80211Oid);
}
void unregisterAcceptSysctl()
{
  sysctl_unregister_oid(&gA80211Oid);
  gA80211 = nullptr;
}
}  // namespace

// Only with piano-wifi-apple80211=1; otherwise PianoWifiEthernet (the same match category, a lower score) starts instead.
IOService *PianoWifi80211::probe(IOService *provider, SInt32 *score)
{
  uint32_t on = 0;
  if (!PE_parse_boot_argn("piano-wifi-apple80211", &on, sizeof on) || !on) return nullptr;
  return IO80211Controller::probe(provider, score);
}

// Inside PianoWifiEthernet::start's attachInterface, before the interface is registered (and so before the network stack
// attaches it): the subfamily and the role, which the network stack and airportd read once.
bool PianoWifi80211::configureInterface(IONetworkInterface *netif)
{
  if (!IO80211Controller::configureInterface(netif)) return false;
  fNetif = netif;
  fSubtypeTaken = piano_netif_set_subtype(netif, 3 /* IFNET_SUBFAMILY_WIFI */);
  netif->setProperty("IO80211InterfaceRole", "Infrastructure");
  note80211("the interface is a Wi-Fi one: subfamily 3 %s, IO80211InterfaceRole Infrastructure",
            fSubtypeTaken ? "taken" : "REFUSED (the interface was attached already)");
  return true;
}

bool PianoWifi80211::start(IOService *provider)
{
  using namespace pianowifi::a80211;
  fLock = IOLockAlloc();
  fState = static_cast<State *>(IOMallocZeroData(sizeof(State)));
  fScratch = static_cast<uint8_t *>(IOMallocZeroData(kScratchBytes));
  fSeen = static_cast<Seen *>(IOMallocZeroData(sizeof(Seen)));
  fAttachCall = thread_call_allocate(&PianoWifi80211::attachTick, this);
  fScanDoneCall = thread_call_allocate(&PianoWifi80211::scanDoneTick, this);
  fReportCall = thread_call_allocate(&PianoWifi80211::reportTick, this);
  if (!fLock || !fState || !fScratch || !fSeen || !fAttachCall || !fScanDoneCall || !fReportCall) {
    note80211("no memory for the apple80211 state: not started");
    return false;
  }
  reset(*fState);
  uint32_t accept = 0;
  if (PE_parse_boot_argn("piano-wifi-apple80211-accept", &accept, sizeof accept)) fState->accept = accept;
  if (!IO80211Controller::start(provider)) return false;   // PianoWifiEthernet: the interface, its data path, the link down
  IOEthernetAddress mac = {};
  getHardwareAddress(&mac);
  for (unsigned i = 0; i < 6; ++i) fState->mac[i] = mac.bytes[i];
  if (OSDictionary *matching = IOService::serviceMatching("PianoWifi")) {
    fWifi = IOService::copyMatchingService(matching);
    matching->release();
  }
  IOLockLock(fLock);
  refreshScanLocked();
  const uint32_t networks = fState->scan.count;
  IOLockUnlock(fLock);
  setProperty("piano-wifi-apple80211", true);
  note80211("started (piano-wifi-apple80211=1): %u network(s) in PianoWifi's scan list%s; waiting for the BSD interface",
            unsigned(networks), fWifi ? "" : " (PianoWifi not found: no scan list)");
  registerAcceptSysctl(this);
  fSysctl = true;
  note80211("accept mode %#x (machdep.piano_wifi_a80211 changes it at run time: 0x1 INFRA_ENUMERATED set, 0x2 any other set, "
            "0x4 any other get zeroed, 0x8 VIRTUAL_IF_ROLE answered)", unsigned(fState->accept));
  arm(fAttachCall, kAttachTickMs);
  return true;
}

// Every 200 ms until the network stack has attached the interface (its ifnet exists once it has a BSD name): then the filter,
// the first events and the controller's registration.
void PianoWifi80211::attachStep()
{
  if (fStopping) return;
  ifnet_t ifp = fNetif ? fNetif->getIfnet() : nullptr;
  if (!ifp) {
    if (++fAttachTries < kAttachTicks) arm(fAttachCall, kAttachTickMs);
    else note80211("no BSD interface after %u s: the apple80211 requests are not answered", kAttachTicks * kAttachTickMs / 1000);
    return;
  }
  snprintf(fIfName, sizeof fIfName, "%s%u", ifnet_name(ifp), unsigned(ifnet_unit(ifp)));
  // IO80211.framework finds the interface's service by the property IOInterfaceName (any class) and reads the role there; the
  // family sets it with the name, this only makes sure of it.
  const bool named = fNetif->getProperty("IOInterfaceName") != nullptr;
  if (!named) fNetif->setProperty("IOInterfaceName", fIfName);
  iff_filter filter = {};
  filter.iff_cookie = this;
  filter.iff_name = "com.unixnu.driver.PianoWifi80211";
  filter.iff_protocol = 0;   // every protocol's ioctls
  filter.iff_ioctl = &PianoWifi80211::filterIoctl;
  filter.iff_detached = &PianoWifi80211::filterDetached;
  retain();   // the filter's reference, given back in filterDetached
  const errno_t e = iflt_attach(ifp, &filter, &fFilter);
  if (e) {
    fFilter = nullptr;
    release();
    note80211("%s: the interface filter was refused (%d): the apple80211 requests are not answered", fIfName, e);
    return;
  }
  note80211("%s: apple80211 requests answered from now on (after %u ms); the BSD interface reads type %u, family %u, subfamily %u; "
            "IOInterfaceName %s", fIfName, fAttachTries * kAttachTickMs, unsigned(ifnet_type(ifp)), unsigned(ifnet_family(ifp)),
            unsigned(ifnet_subfamily(ifp)), named ? "the family's" : "set here");
  postEvent(pianowifi::a80211::kEvPowerChanged);
  pianowifi::a80211::LinkEvent down = {};
  down.down = 1;
  postEvent(pianowifi::a80211::kEvLinkChanged, &down, sizeof down);
  setProperty("piano-wifi-apple80211-answering", fIfName);   // the BSD name; the airportd starter waits for this key
  registerService();
  fRegistered = true;
  note80211("%s: the controller is registered (class chain PianoWifi80211 < IO80211Controller < PianoWifiEthernet): airportd's "
            "IOKit match may fire", fIfName);
  arm(fReportCall, 5000);   // the first summary soon (who matched the controller), then every minute
}

void PianoWifi80211::attachTick(thread_call_param_t self, thread_call_param_t)
{
  static_cast<PianoWifi80211 *>(self)->attachStep();
}

errno_t PianoWifi80211::filterIoctl(void *cookie, ifnet_t, protocol_family_t, unsigned long cmd, void *arg)
{
  auto *self = static_cast<PianoWifi80211 *>(cookie);
  if (!self || !arg) return EOPNOTSUPP;
  if (cmd == pianowifi::a80211::kSiocSA80211 || cmd == pianowifi::a80211::kSiocGA80211)
    return self->apple80211(cmd == pianowifi::a80211::kSiocSA80211, *static_cast<pianowifi::a80211::Req *>(arg));
  self->noteOtherIoctl(cmd);
  return EOPNOTSUPP;   // not ours: the protocol and the interface handle it as before
}

void PianoWifi80211::filterDetached(void *cookie, ifnet_t)
{
  auto *self = static_cast<PianoWifi80211 *>(cookie);
  self->fFilterGone = true;
  self->release();
}

// One request, in the requesting thread (airportd's ioctl): the answer under the lock, its log line and its effects after.
errno_t PianoWifi80211::apple80211(bool set, pianowifi::a80211::Req &rq)
{
  using namespace pianowifi::a80211;
  const int32_t type = rq.type, value = rq.value;
  const uint32_t length = rq.length;
  const UserIo io = {nullptr, &PianoWifi80211::userIn, &PianoWifi80211::userOut};
  const bool scanSet = set && (type == kScanReq || type == kScanReqMultiple);
  uint32_t directed = 0;
  IOLockLock(fLock);
  if (scanSet) {
    refreshScanLocked();
    if (type == kScanReq) directed = scanReqSsidLength(rq, io);
  }
  const Answer a = handle(*fState, kIdentity, set, rq, io, fScratch);
  const bool first = fSeen->first(type, set, length, a.error);
  ++fRequests;
  if (a.error == kErrUnsupported) ++fUnknown;
  const uint32_t networks = fState->scan.count, cursor = fState->cursor;
  const bool power = fState->power;
  IOLockUnlock(fLock);
  if (first) {
    char extra[64] = "";
    if (scanSet) snprintf(extra, sizeof extra, "; %u network(s)%s", unsigned(networks), directed ? ", a directed scan" : "");
    else if (type == kScanResult && !set) snprintf(extra, sizeof extra, "; cursor %u of %u", unsigned(cursor), unsigned(networks));
    note80211("%s: %s %d %s len %u val %d -> %d (wrote %u, len %u, val %d)%s%s", fIfName, set ? "set" : "get", int(type),
              requestName(type), unsigned(length), int(value), a.error, unsigned(a.wrote), unsigned(rq.length), int(rq.value), extra,
              a.accepted ? " [accept mode]" : "");
  }
  if (a.effects & kDoPowerChanged) {
    note80211("%s: POWER set %s by airportd (M1: scans %s; the chip stays up)", fIfName, power ? "on" : "off",
              power ? "answered again" : "refused with EPWROFF");
    postEvent(kEvPowerChanged);
  }
  if (a.effects & kDoScanStarted) scanStarted();
  return a.error ? a.error : EJUSTRETURN;   // EJUSTRETURN: answered, nothing after this filter runs (ifnet_ioctl makes it 0)
}

// PianoWifi's scan list (the registry property of level 6: PianoWifi.cpp publishScan) into the compact snapshot.
void PianoWifi80211::refreshScanLocked()
{
  using namespace pianowifi::a80211;
  ScanSnapshot &snap = fState->scan;
  snap.count = 0;
  OSObject *property = fWifi ? fWifi->copyProperty("piano-wifi-scan") : nullptr;
  OSArray *list = OSDynamicCast(OSArray, property);
  for (unsigned i = 0; list && i < list->getCount() && snap.count < pianowifi::kScanEntriesMax; ++i) {
    OSDictionary *d = OSDynamicCast(OSDictionary, list->getObject(i));
    if (!d) continue;
    Bss &b = snap.bss[snap.count];
    bzero(&b, sizeof b);
    OSString *bssid = OSDynamicCast(OSString, d->getObject("bssid"));
    if (!bssid || !parseBssid(bssid->getCStringNoCopy(), b.bssid)) continue;
    if (OSData *raw = OSDynamicCast(OSData, d->getObject("ssid-raw"))) {
      b.ssidLength = uint8_t(raw->getLength() <= pianowifi::kSsidMax ? raw->getLength() : pianowifi::kSsidMax);
      memcpy(b.ssid, raw->getBytesNoCopy(), b.ssidLength);
    } else if (OSString *ssid = OSDynamicCast(OSString, d->getObject("ssid"))) {
      b.ssidLength = uint8_t(ssid->getLength() <= pianowifi::kSsidMax ? ssid->getLength() : pianowifi::kSsidMax);
      memcpy(b.ssid, ssid->getCStringNoCopy(), b.ssidLength);
    }
    auto number = [d](const char *key) -> uint32_t {
      OSNumber *n = OSDynamicCast(OSNumber, d->getObject(key));
      return n ? n->unsigned32BitValue() : 0;
    };
    b.channel = uint16_t(number("channel"));
    b.freq = number("freq");
    b.rssi = int32_t(number("rssi"));
    b.capability = uint16_t(number("capability"));
    b.beaconInterval = uint16_t(number("beacon-interval"));
    if (OSData *rates = OSDynamicCast(OSData, d->getObject("rates"))) {
      b.rateCount = uint8_t(rates->getLength() <= sizeof b.rates ? rates->getLength() : sizeof b.rates);
      memcpy(b.rates, rates->getBytesNoCopy(), b.rateCount);
    }
    if (OSData *rsn = OSDynamicCast(OSData, d->getObject("rsn"))) {
      b.rsnLength = uint8_t(rsn->getLength() <= sizeof b.rsn ? rsn->getLength() : 0);
      memcpy(b.rsn, rsn->getBytesNoCopy(), b.rsnLength);
    }
    b.hidden = d->getObject("hidden") == kOSBooleanTrue;
    ++snap.count;
  }
  OSSafeReleaseNULL(property);
  ++fListRefreshes;
}

// Vendor Apple, class 6 (IEEE 802.11), subclass 1 (apple80211), the interface's name (16 bytes) and the payload.
void PianoWifi80211::postEvent(uint32_t code, const void *payload, uint32_t length)
{
  char name[16] = {};
  memcpy(name, fIfName, sizeof name);
  struct kev_msg m;
  bzero(&m, sizeof m);
  m.vendor_code = KEV_VENDOR_APPLE;
  m.kev_class = pianowifi::a80211::kEventClass;
  m.kev_subclass = pianowifi::a80211::kEventSubclass;
  m.event_code = code;
  m.dv[0].data_length = sizeof name;
  m.dv[0].data_ptr = name;
  if (payload && length) {
    m.dv[1].data_length = length;
    m.dv[1].data_ptr = const_cast<void *>(payload);
  }
  const int e = piano_kev_post_msg(&m);
  IOLockLock(fLock);
  ++fEvents;
  if (e) ++fEventErrors;
  const bool first = code < 32 && !(fEventCodesLogged & (1u << code));
  if (code < 32) fEventCodesLogged |= 1u << code;
  IOLockUnlock(fLock);
  if (first || e) note80211("%s: event %u (%u payload bytes) posted -> %d", fIfName, unsigned(code), unsigned(length), e);
}

// One completion per accepted scan: an earlier one still waiting is posted at once (thread_call_cancel is true only for a call
// that had not started; one that runs posts its own), this one's after kScanDoneMs.
void PianoWifi80211::scanStarted()
{
  if (thread_call_cancel(fScanDoneCall)) postEvent(pianowifi::a80211::kEvScanDone);
  arm(fScanDoneCall, kScanDoneMs);
}

void PianoWifi80211::scanDoneTick(thread_call_param_t p, thread_call_param_t)
{
  auto *self = static_cast<PianoWifi80211 *>(p);
  if (self->fStopping) return;
  self->postEvent(pianowifi::a80211::kEvScanDone);
}

void PianoWifi80211::noteOtherIoctl(unsigned long cmd)
{
  IOLockLock(fLock);
  ++fOtherIoctls;
  bool first = true;
  for (unsigned i = 0; i < fOtherCount; ++i) first = first && fOtherSeen[i] != cmd;
  if (first && fOtherCount < sizeof fOtherSeen / sizeof fOtherSeen[0]) fOtherSeen[fOtherCount++] = cmd;
  else first = false;
  IOLockUnlock(fLock);
  if (first)
    note80211("%s: ioctl %#lx (group '%c', number %lu, %lu bytes) passed on", fIfName, cmd, char((cmd >> 8) & 0xff), cmd & 0xff,
              (cmd >> 16) & 0x1fff);
}

uint32_t PianoWifi80211::acceptMode()
{
  IOLockLock(fLock);
  const uint32_t mode = fState->accept;
  IOLockUnlock(fLock);
  return mode;
}

// A new accept mode: the kind table starts again, so the log shows every kind airportd asks from now on with the new answers.
void PianoWifi80211::setAcceptMode(uint32_t mode, const char *why)
{
  IOLockLock(fLock);
  const uint32_t was = fState->accept;
  fState->accept = mode & 0xf;
  fSeen->clear();
  IOLockUnlock(fLock);
  note80211("%s: accept mode %#x (was %#x, %s): every request kind is logged again from here", fIfName, unsigned(mode & 0xf),
            unsigned(was), why);
}

void PianoWifi80211::report(const char *why)
{
  IOLockLock(fLock);
  const uint64_t requests = fRequests, unknown = fUnknown, events = fEvents, eventErrors = fEventErrors, other = fOtherIoctls;
  const uint32_t kinds = fSeen->count, overflow = fSeen->overflow, scans = fState->scanRequests, results = fState->resultsServed;
  const uint32_t ends = fState->listEnds, networks = fState->scan.count, powerSets = fState->powerSets;
  const bool power = fState->power;
  const uint32_t accept = fState->accept, accepted = fState->acceptedUnknown;
  const uint64_t stamp = requests + events + other;
  const bool changed = stamp != fReported;
  fReported = stamp;
  IOLockUnlock(fLock);
  if (!changed) return;
  unsigned clients = 0;
  char names[96] = "";
  if (OSIterator *it = getClientIterator()) {
    while (OSObject *o = it->getNextObject()) {
      const size_t at = strlen(names);
      if (clients++ < 3 && at + 2 < sizeof names)
        snprintf(names + at, sizeof names - at, "%s%s", at ? " " : "", o->getMetaClass()->getClassName());
    }
    it->release();
  }
  note80211("%s (%s): %llu request(s) of %u kind(s)%s, %llu unknown; accept mode %#x (%u answered by it); %u scan(s), %u "
            "result(s) served, %u list end(s), %u network(s); power %s (%u set(s)); %llu event(s), %llu refused; %llu other "
            "ioctl(s); the controller's clients: %u (%s)",
            fIfName, why, requests, unsigned(kinds), overflow ? " (the kind table is full)" : "", unknown, unsigned(accept),
            unsigned(accepted), unsigned(scans), unsigned(results), unsigned(ends), unsigned(networks), power ? "on" : "off",
            unsigned(powerSets), events, eventErrors, other, clients, names);
}

void PianoWifi80211::reportTick(thread_call_param_t p, thread_call_param_t)
{
  auto *self = static_cast<PianoWifi80211 *>(p);
  if (self->fStopping) return;
  self->report("summary");
  self->arm(self->fReportCall, kReportMs);
}

void PianoWifi80211::stop(IOService *provider)
{
  fStopping = true;
  if (fSysctl) unregisterAcceptSysctl();
  fSysctl = false;
  if (fAttachCall) thread_call_cancel_wait(fAttachCall);
  if (fScanDoneCall) thread_call_cancel_wait(fScanDoneCall);
  if (fReportCall) thread_call_cancel_wait(fReportCall);
  if (fFilter) {
    iflt_detach(fFilter);   // the detached callback gives the filter's reference back
    for (unsigned i = 0; i < 200 && !fFilterGone; ++i) IOSleep(10);
    fFilter = nullptr;
  }
  if (fRegistered) report("at the stop");
  OSSafeReleaseNULL(fWifi);
  IO80211Controller::stop(provider);
}

void PianoWifi80211::free()
{
  if (fAttachCall) thread_call_free(fAttachCall);
  if (fScanDoneCall) thread_call_free(fScanDoneCall);
  if (fReportCall) thread_call_free(fReportCall);
  fAttachCall = fScanDoneCall = fReportCall = nullptr;
  if (fState) IOFreeData(fState, sizeof(State));
  if (fScratch) IOFreeData(fScratch, pianowifi::a80211::kScratchBytes);
  if (fSeen) IOFreeData(fSeen, sizeof(Seen));
  fState = nullptr;
  fScratch = nullptr;
  fSeen = nullptr;
  if (fLock) IOLockFree(fLock);
  fLock = nullptr;
  IO80211Controller::free();
}
