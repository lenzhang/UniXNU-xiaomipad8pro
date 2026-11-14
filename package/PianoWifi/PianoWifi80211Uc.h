// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi80211Uc: the IOUserClient side of the apple80211 front end (docs/drivers/wifi-menu.md section 7, milestone M1b),
// included by PianoWifi80211.h. Off unless asked for (`piano-wifi-apple80211-uc=N`, `sysctl machdep.piano_wifi_a80211_uc`), so
// wm2's behaviour stays the default.
//
// Why: on wm1/wm2 every apple80211 ioctl was answered and airportd's legacy half drove the interface (discovery, power, scans),
// but its corewifi/WiFiManager half never built its per-interface object (`wdutil`: Interface Name (null); CoreWLAN's scan empty),
// and over a whole boot the only client of our service was the family's own `IOEthernetInterface`. The user-space IO80211 library
// prefers `IOServiceOpen` on the service it finds by `IOInterfaceName` and falls back to the ioctls for *that* bind; the
// per-interface object may not fall back (inferred, private/host-checks/20261114-flash/wm2-diag.md section 5 Rank 1). So this
// offers a client to open and answers the two selectors that bind needs, and logs **everything** that opens us: that one boot
// tells whether adoption is bind-gated.
//
// The selectors come from the host analysis of 27.3's IO80211 (private/host-checks/20261114-io80211/REPORT.md section 2.1 and
// docs/drivers/wifi-menu.md section 5.7): 0 an apple80211 request in a 0x3c30-byte structure, 1 `useIOUCWhenPossible`,
// 7 DriverKit support, 5 "use the event pipe", 6 the same as 0 with a 0x3c030-byte structure. No Apple code is used: the request
// numbers and the two field offsets are data, and the answers are our own (WifiApple80211.h).
#pragma once

// The structure selector 0 carries: the ioctl's number, the length and the user pointer at its head, then the 40-byte
// apple80211 request's name, type and value near its end. Only these five fields are read or written; the rest is the caller's.
namespace pianowifi {
namespace a80211 {

constexpr uint32_t kUcSmallBytes = 0x3c30;    // selector 0's structure
constexpr uint32_t kUcLargeBytes = 0x3c030;   // selector 6's structure (the same fields, 0x3c000 further on)
constexpr uint32_t kUcCmdAt = 0x0;            // the ioctl number (0x69c9 get / 0x69c8 set, the low 16 bits of SIOC[SG]A80211)
constexpr uint32_t kUcLenAt = 0x4;            // the request's length
constexpr uint32_t kUcDataAt = 0x8;           // the user pointer of the request's buffer
constexpr uint32_t kUcNameAt = 0x3c08;        // the interface's name (16 bytes)
constexpr uint32_t kUcTypeAt = 0x3c18;        // the request number
constexpr uint32_t kUcValAt = 0x3c1c;         // req_val
constexpr uint32_t kUcGetCmd = 0x69c9, kUcSetCmd = 0x69c8;

// The selectors of 27.3's user-space IO80211, as the host analysis names them.
enum : uint32_t {
  kUcSelRequest = 0,      // an apple80211 request in kUcSmallBytes
  kUcSelUseIouc = 1,      // one byte out: whether user space should prefer this client
  kUcSelPipeStart = 2,    // the event pipe (not offered: our events stay on the kernel-event socket)
  kUcSelPipeSetup = 3,
  kUcSelPipeHalt = 4,
  kUcSelUsePipe = 5,      // one byte out: whether to use the event pipe (0: keep the kernel-event socket)
  kUcSelRequestBig = 6,   // an apple80211 request in kUcLargeBytes
  kUcSelDkSupport = 7,    // one byte out: DriverKit support
  kUcSelMapBuffer = 8,
  kUcSelMax = 16,
};

// The user-client switches (`piano-wifi-apple80211-uc`, `machdep.piano_wifi_a80211_uc`), each tried on its own boot or turned on
// at run time; 0 is wm2's behaviour (no client offered at all).
enum : uint32_t {
  kUcOffer = 0x1,         // newUserClient answers: a client is made (and every open is logged, even without this)
  kUcControllerName = 0x2,   // the controller also carries IOInterfaceName / IO80211InterfaceRole, so user space's lookup by
                             // IOInterfaceName (any class) can find the controller and open it
  kUcEventPayloads = 0x4,    // the events carry 27.3-shaped payloads instead of none (wm2-diag Rank 3)
  kUcOverride = 0x8,         // IOClassNameOverride = "IO80211Controller" on the controller (ADOPTION.md P0, wm3's stage 1)
  kUcUsePipe = 0x10,         // selector 5 answers 1 (the event pipe) instead of 0
  kUcOverrideIf = 0x20,      // the same override on the interface object too
};

}  // namespace a80211
}  // namespace pianowifi

// A client of the controller or of the interface: it holds the driver, answers the selectors above and logs each one.
class PianoWifi80211Uc : public IOUserClient {
  OSDeclareDefaultStructors(PianoWifi80211Uc)

  PianoWifi80211 *fOwner = nullptr;   // retained
  task_t fTask = nullptr;
  uint32_t fType = 0;
  uint64_t fCalls = 0;
  uint32_t fSeen = 0;   // bit per selector below kUcSelMax, for one line each

 public:
  bool initWithOwner(PianoWifi80211 *owner, task_t task, uint32_t type);
  IOReturn clientClose() override;
  void free() override;
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
                          void *reference) override;
  IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) override;
};

OSDefineMetaClassAndStructors(PianoWifi80211Uc, IOUserClient)

bool PianoWifi80211Uc::initWithOwner(PianoWifi80211 *owner, task_t task, uint32_t type)
{
  if (!owner || !IOUserClient::init()) return false;
  owner->retain();
  fOwner = owner;
  fTask = task;
  fType = type;
  return true;
}

IOReturn PianoWifi80211Uc::clientClose()
{
  if (fOwner) fOwner->noteUserClient("closed", fTask, fType, fCalls);
  terminate();
  return kIOReturnSuccess;
}

void PianoWifi80211Uc::free()
{
  OSSafeReleaseNULL(fOwner);
  IOUserClient::free();
}

// A notification port means user space wants the family's event pipe. We keep events on the kernel-event socket (27.3's
// EventMonitoringInit falls back to it), so the port is only logged.
IOReturn PianoWifi80211Uc::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
  if (fOwner) fOwner->noteUserClient("asked for a notification port", fTask, uint32_t(type), fCalls);
  return IOUserClient::registerNotificationPort(port, type, refCon);
}

IOReturn PianoWifi80211Uc::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
                                          OSObject *, void *)
{
  using namespace pianowifi::a80211;
  if (!fOwner || !args) return kIOReturnNotReady;
  ++fCalls;
  const uint32_t inBytes = args->structureInputSize, outBytes = args->structureOutputSize;
  IOReturn result = kIOReturnUnsupported;
  uint32_t answered = 0;   // bytes of the structure output written
  switch (selector) {
  case kUcSelRequest:
  case kUcSelRequestBig:
    result = fOwner->ucRequest(args, selector == kUcSelRequestBig, answered);
    break;
  case kUcSelUseIouc:
  case kUcSelDkSupport:
  case kUcSelUsePipe: {
    // One byte out: prefer this client (1), no DriverKit (0), the event pipe only when asked for (kUcUsePipe).
    const uint8_t value = selector == kUcSelUseIouc ? 1 : selector == kUcSelUsePipe ? ((fOwner->ucMode() & kUcUsePipe) ? 1 : 0) : 0;
    if (args->structureOutput && outBytes >= 1) {
      *static_cast<uint8_t *>(args->structureOutput) = value;
      answered = 1;
      result = kIOReturnSuccess;
    } else if (args->scalarOutput && args->scalarOutputCount >= 1) {
      args->scalarOutput[0] = value;
      result = kIOReturnSuccess;
    } else {
      result = kIOReturnBadArgument;
    }
    break;
  }
  default:
    break;
  }
  const bool first = selector < kUcSelMax && !(fSeen & (1u << selector));
  if (selector < kUcSelMax) fSeen |= 1u << selector;
  if (first || result != kIOReturnSuccess)
    fOwner->noteSelector(selector, inBytes, outBytes, args->scalarInputCount, args->scalarOutputCount, answered, result, fCalls);
  return result;
}
