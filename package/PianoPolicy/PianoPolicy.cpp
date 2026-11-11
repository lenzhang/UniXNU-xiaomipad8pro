// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoPolicy: piano's laptop-class policy for native macOS 27 (backlog B2 cover/keyboard/power key, B5 Low Power Mode), so that
// the drivers stay mechanisms. It owns no hardware: it reads what the other drivers publish, decides (Policy.h, host-tested) and
// tells IOPMrootDomain, PianoThermal and PianoA830 through their public calls. docs/policy.md is the page. Inert without
// piano-policy (the service does not even probe).
//
// Inputs (no link to the drivers it watches but PianoThermal's breadcrumb; services found by class):
//   PianoKeyboard     piano-keyboard-hall (0 closed, 1 open), -attached, -status (gpio95), each change a
//                     kIOMessageServicePropertyChange to interested clients (this one)
//   IOPMPowerSource   ExternalConnected (PianoPower's AppleSmartBattery, piano-power-ps=1); kIOPMMessageBatteryStatusHasChanged
//   NativeGopDisplay  piano-display-power (published with piano-backlight=2), read when needed and every second while a keyboard
//                     wake could come (its hold-off)
//   machdep.piano_policy_lpm   macOS Low Power Mode, written by root (the agent tooling/native/piano-policy-agent mirrors
//                     com.apple.system.lowpowermode into it; by hand: sudo sysctl machdep.piano_policy_lpm=1)
//   machdep.piano_sleep (k27 0062)   "on" = system sleep exists (piano-sleep=1), and its wake mask; read once at start
//   PianoA830         piano-gpu-ceiling-clients: >= 2, a ceiling slot per caller (docs/policy.md 5.3): ours is client 1
//   piano_thermal_state (PianoThermal, a library of the descriptor)   its protection state, only while PianoA830 has no slots
//                     (the one GPU ceiling slot shared with PianoThermal's passive trip)
// Outputs (level 2):
//   IOPMrootDomain    receivePowerNotification: kIOPMClamshellClosed / Opened (with piano-policy-lid=1), kIOPMEnableClamshell /
//                     kIOPMDisableClamshell (closing sleeps or only locks), kIOPMSetACAdaptorConnected (the power source, so its
//                     clamshell rules see it), kIOPMSleepNow (the power key); requestUserActive (display wake: cover open, power
//                     key with the display off, gpio95)
//   PianoThermal      callPlatformFunction("piano-thermal-profile", mode): 1 Power Saver with Low Power Mode, 0 its own choice
//   PianoA830         callPlatformFunction("piano-gpu-ceiling", kHz, client): Power Saver's ceiling (342 MHz), 0 clears; only
//                     when PianoA830's own maximum is above it. With PianoA830's slots client 1 (the lowest slot wins: PianoThermal's
//                     passive ceiling stays in force by itself); without them the shared slot (param2 null), never written over
//                     PianoThermal's passive ceiling (the breadcrumb read, a 5 s tick while ours is in, the put-back after a race)
// Answers (platform functions on this service, any thread):
//   "piano-policy-power-key"     param1 = the press in ms (PianoInput, on the release of a short press): kIOReturnSuccess when
//                                consumed (sleep now, or a display wake), kIOReturnUnsupported: send macOS the HID Power usage
//   "piano-policy-keyboard-wake" param1 = bool *: PianoKeyboard may arm its gpio95 latch for a system sleep (0062 bit 16);
//                                param2 = bool *: gpio95 may wake the display now
// Properties: piano-policy, -close (sleep/lock/none), -profile, -gpu-khz, -keyboard-wake, -keyboard-latch, -lpm, -changes, each
// change a kIOMessageServicePropertyChange. sysctl machdep.piano_policy: the state, decisions and counters.
//
// Boot arguments:
//   piano-policy=1          decisions logged ("would ..."), nothing sent; the platform functions answer kIOReturnUnsupported
//   piano-policy=2          act
//   piano-policy-lid=1      act on the cover (clamshell, clamshell sleep, AC adaptor). Only once the hall polarity is verified
//                           (docs/drivers/keyboard.md 10.3) and with piano-keyboard-lid=0 (else PianoKeyboard tells the clamshell)
//   piano-policy-profile=0  no PianoThermal profile, no GPU ceiling            piano-policy-gpu-mhz=M  Power Saver's GPU ceiling (342)
//   piano-policy-powerkey=0 the power key stays macOS's                       piano-policy-kbd=0  no display wake by gpio95
//   piano-policy-kbd-holdoff=MS  the display off at least MS before gpio95 counts (1000)
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include "Policy.h"

using namespace pianopolicy;

static_assert(kSleepNow == kIOPMSleepNow && kClamshellClosed == kIOPMClamshellClosed && kClamshellOpened == kIOPMClamshellOpened &&
              kDisableClamshell == kIOPMDisableClamshell && kEnableClamshell == kIOPMEnableClamshell && kSetValue == kIOPMSetValue &&
              kSetAcAdaptorConnected == kIOPMSetACAdaptorConnected, "Policy.h's IOPM message values");

extern "C" {
// PianoThermal's protection state (its Policy.h State: 0 normal, 1 warm, 2 passive, 3 critical), set at every policy update.
extern volatile int32_t piano_thermal_state;
// The kernel's sysctl by name (bsd/kern/kern_newsysctl.c).
int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
// Post-mortem breadcrumb: 1 started, 2 running.
volatile int32_t piano_policy_stage = 0;
}

namespace {

struct Args {
  uint32_t level, lid, profile, gpuMhz, powerKey, kbd, holdoffMs, keyboardLid, gpuDcvsMax, gpuMaxLevel, thermal, thermalGpuMhz;
  bool gpuMaxGiven;
};
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback, bool *given = nullptr)
{
  uint32_t v = fallback;
  const bool found = PE_parse_boot_argn(name, &v, sizeof v);
  if (given) *given = found;
  return found ? v : fallback;
}
uint32_t clampArg(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void readArgs()
{
  gArgs.level = clampArg(bootArg("piano-policy", 0), 0, 2);
  gArgs.lid = bootArg("piano-policy-lid", 0);
  gArgs.profile = bootArg("piano-policy-profile", 1);
  gArgs.gpuMhz = bootArg("piano-policy-gpu-mhz", 342);
  if (gArgs.gpuMhz) gArgs.gpuMhz = clampArg(gArgs.gpuMhz, 160, 1100);
  gArgs.powerKey = bootArg("piano-policy-powerkey", 1);
  gArgs.kbd = bootArg("piano-policy-kbd", 1);
  gArgs.holdoffMs = clampArg(bootArg("piano-policy-kbd-holdoff", 1000), 0, 10000);
  // The other drivers' own arguments, read here as they read them (no call needed): PianoKeyboard's clamshell mode, PianoA830's
  // DCVS maximum (342 MHz unless piano-gpu-dcvs-max or piano-gpu-maxlevel is given), PianoThermal's level and passive GPU ceiling.
  gArgs.keyboardLid = bootArg("piano-keyboard-lid", 0);
  bool dcvsGiven = false, levelGiven = false;
  gArgs.gpuDcvsMax = bootArg("piano-gpu-dcvs-max", 342, &dcvsGiven);
  gArgs.gpuMaxLevel = bootArg("piano-gpu-maxlevel", 0, &levelGiven);
  gArgs.gpuMaxGiven = (dcvsGiven && gArgs.gpuDcvsMax > gArgs.gpuMhz) || (levelGiven && gArgs.gpuMaxLevel);
  gArgs.thermal = clampArg(bootArg("piano-thermal", 0), 0, 2);
  gArgs.thermalGpuMhz = clampArg(bootArg("piano-thermal-gpu-mhz", 160), 100, 1100);
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoPolicy: %s\n", line);
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// A line for the first 64 events of a kind, then every 16th.
bool logged(uint32_t n) { return n <= 64 || n % 16 == 0; }

IOService *copyByClass(const char *name)
{
  OSDictionary *match = IOService::serviceMatching(name);
  if (!match) return nullptr;
  IOService *s = IOService::copyMatchingService(match);
  match->release();
  return s;
}

int numberProperty(IOService *s, const char *key)
{
  if (!s) return -1;
  OSObject *o = s->copyProperty(key);
  int r = -1;
  if (OSNumber *n = OSDynamicCast(OSNumber, o)) r = int(n->unsigned32BitValue());
  else if (OSBoolean *b = OSDynamicCast(OSBoolean, o)) r = b->isTrue() ? 1 : 0;
  OSSafeReleaseNULL(o);
  return r;
}

// ---------------------------------------------------------------------------------------------------------------- sysctls
volatile uint32_t gLpm;                 // machdep.piano_policy_lpm
IOLock *gStatusLock;
char gStatus[640] = "starting\n";
void kickFromSysctl();

int sysctlStatus(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
  char buf[sizeof gStatus];
  IOLockLock(gStatusLock);
  strlcpy(buf, gStatus, sizeof buf);
  IOLockUnlock(gStatusLock);
  return SYSCTL_OUT(req, buf, strlen(buf) + 1);
}

int sysctlLpm(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
  int v = int(__atomic_load_n(&gLpm, __ATOMIC_ACQUIRE));
  const int err = sysctl_handle_int(oidp, &v, 0, req);
  if (err || !req->newptr) return err;
  __atomic_store_n(&gLpm, v ? 1u : 0u, __ATOMIC_RELEASE);
  kickFromSysctl();
  return 0;
}

// machdep.piano_policy (read only) and machdep.piano_policy_lpm (root writes), registered at start. Filled in at run time rather
// than by SYSCTL_OID's static initializer: its parent and handler are signed pointers on arm64e.
struct sysctl_oid gOidStatus, gOidLpm;

}  // namespace

SYSCTL_DECL(_machdep);

namespace {
void fillOid(struct sysctl_oid &o, const char *name, int kind, int (*handler)(struct sysctl_oid *, void *, int, struct sysctl_req *),
             const char *fmt, const char *descr)
{
  o.oid_parent = &sysctl__machdep_children;
  o.oid_number = OID_AUTO;
  o.oid_kind = kind | CTLFLAG_OID2;
  o.oid_arg1 = nullptr;
  o.oid_arg2 = 0;
  o.oid_name = name;
  o.oid_handler = handler;
  o.oid_fmt = fmt;
  o.oid_descr = descr;
  o.oid_version = SYSCTL_OID_VERSION;
  o.oid_refcnt = 0;
}
}  // namespace

class PianoPolicy;
static PianoPolicy *gPolicy;

// ---------------------------------------------------------------------------------------------------------------- the service
class PianoPolicy : public IOService {
  OSDeclareDefaultStructors(PianoPolicy)

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                void *param4) override;
  enum : uint32_t { kKeyboard = 1, kPower = 2, kLpm = 4, kRoot = 8, kService = 16 };   // what woke the thread (for the log)
  void kick(uint32_t bits);

 private:
  enum : uintptr_t { kRefKeyboard = 1, kRefPower = 2, kRefOther = 3 };

  static void threadMain(void *self, wait_result_t);
  static bool published(void *target, void *ref, IOService *service, IONotifier *notifier);
  static IOReturn interest(void *target, void *ref, UInt32 type, IOService *provider, void *arg, vm_size_t size);
  static IOReturn rootInterest(void *target, void *ref, UInt32 type, IOService *provider, void *arg, vm_size_t size);
  void run();
  Inputs gather();
  void perform(const Actions &a, const Inputs &in, bool fromThread);
  Result call(const char *service, const char *function, uint32_t value);
  Result gpuCall(uint32_t khz, uint32_t *client);
  void describe(const Inputs &in, const View &v);
  void publishView(const View &v, const Inputs &in);

  Config fConfig;
  Engine fEngine{Config()};
  IOLock *fLock = nullptr;
  uint32_t fPending = 0;
  IOService *fKeyboard = nullptr, *fPowerSource = nullptr, *fDisplay = nullptr, *fGpu = nullptr;   // retained once found
  volatile uint32_t fStatusRises = 0, fRootClamshell = 0, fRootClamshellSeen = 0;
  uint32_t fStatusRisesSeen = 0;
  bool fSleepOn = false;
  uint32_t fWakeMask = 0;
  bool fSeenOpenForCalls = false;
  // Counters.
  uint32_t fInputChanges = 0, fRootSent = 0, fUserActive = 0, fProfileCalls = 0, fGpuCalls = 0, fKeys = 0, fKbdWakes = 0, fChanges = 0;
  Inputs fLast;
  bool fHaveLast = false;
  View fLastView;
  bool fHaveView = false;
  bool fProfileMissingLogged = false, fGpuMissingLogged = false;
  int fGpuSlotsLogged = -1;   // the piano-gpu-ceiling-clients last named on the console
};

OSDefineMetaClassAndStructors(PianoPolicy, IOService)

namespace {
void kickFromSysctl()
{
  if (gPolicy) gPolicy->kick(PianoPolicy::kLpm);
}
}

IOService *PianoPolicy::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoPolicy::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  fLock = IOLockAlloc();
  gStatusLock = IOLockAlloc();
  if (!fLock || !gStatusLock) {
    note("FAILED: no lock");
    return false;
  }
  fConfig.act = gArgs.level >= 2;
  fConfig.lid = gArgs.lid != 0;
  fConfig.lidConflict = gArgs.keyboardLid != 0;
  fConfig.profile = gArgs.profile != 0;
  fConfig.gpuKhz = gArgs.gpuMhz * 1000u;
  fConfig.gpuNeeded = gArgs.gpuMaxGiven;
  fConfig.thermalOwnsGpu = gArgs.thermal >= 2;
  fConfig.thermalGpuKhz = gArgs.thermalGpuMhz * 1000u;
  fConfig.powerKey = gArgs.powerKey != 0;
  fConfig.keyboard = gArgs.kbd != 0;
  fConfig.holdoffMs = gArgs.holdoffMs;
  fEngine = Engine(fConfig);
  // System sleep and its wake mask, as the kernel has them (k27 0062; absent before it).
  char sleep[640] = {};   // 0062's string is at most 512 bytes
  size_t len = sizeof sleep - 1;
  const int err = kernel_sysctlbyname("machdep.piano_sleep", sleep, &len, nullptr, 0);
  parseSleep(err ? nullptr : sleep, &fSleepOn, &fWakeMask);
  note("start: piano-policy=%u (%s), cover %s, profile %s, GPU ceiling %u MHz (%s), power key %s, keyboard wake %s (hold-off %u ms)",
       gArgs.level, fConfig.act ? "acting" : "decisions only: nothing sent",
       !fConfig.lid ? "not acted on (piano-policy-lid=0)" : fConfig.lidConflict ? "NOT acted on: piano-keyboard-lid is set" : "acted on",
       fConfig.profile ? "on" : "off", gArgs.gpuMhz,
       !gArgs.gpuMhz ? "none" : fConfig.gpuNeeded ? "PianoA830's own maximum is above it" : "a no-op: PianoA830's own maximum is 342 MHz",
       fConfig.powerKey ? "on" : "off", fConfig.keyboard ? "on" : "off", fConfig.holdoffMs);
  if (err) note("system sleep: machdep.piano_sleep absent (error %d: a kernel before k27 0062): no sleep, the cover only locks", err);
  else
    note("system sleep: %s, wake mask %#x (%s%s%s); machdep.piano_sleep: %.120s", fSleepOn ? "on" : "off (piano-sleep=1 turns it on)",
         fWakeMask, fWakeMask & kWakePowerKey ? "power key" : "-", fWakeMask & kWakeLid ? ", cover open" : ", NO cover-open wake",
         fWakeMask & kWakeKeyboard ? ", keyboard" : "", sleep);
  if (fConfig.thermalOwnsGpu)
    note("PianoThermal at level 2 writes a GPU ceiling at its passive trip: with PianoA830's slots ours is client 1 (the lowest wins), "
         "without them ours is never written over its passive one");
  gPolicy = this;
  fillOid(gOidStatus, "piano_policy", CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, sysctlStatus, "A",
          "PianoPolicy: inputs, decisions and counters");
  fillOid(gOidLpm, "piano_policy_lpm", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, sysctlLpm, "I",
          "PianoPolicy: macOS Low Power Mode (0/1), mirrored by the agent");
  sysctl_register_oid(&gOidStatus);
  sysctl_register_oid(&gOidLpm);
  setProperty("piano-policy", gArgs.level, 32);
  // The services watched: their appearance (now or later) is an event; PianoKeyboard and the power source are also listened to.
  static const char *const kWatched[] = {"PianoKeyboard", "IOPMPowerSource", "NativeGopDisplay", "PianoThermal", "PianoA830"};
  for (unsigned i = 0; i < sizeof kWatched / sizeof kWatched[0]; ++i) {
    OSDictionary *match = IOService::serviceMatching(kWatched[i]);
    if (!match) continue;
    const uintptr_t ref = i == 0 ? kRefKeyboard : i == 1 ? kRefPower : kRefOther;
    IOService::addMatchingNotification(gIOPublishNotification, match, &PianoPolicy::published, this, reinterpret_cast<void *>(ref));
    match->release();
  }
  if (IOPMrootDomain *root = getPMRootDomain()) root->registerInterest(gIOGeneralInterest, &PianoPolicy::rootInterest, this, nullptr);
  registerService();
  kick(kService);   // the first turn, whatever is published yet
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoPolicy::threadMain, this, &t) != KERN_SUCCESS) {
    note("FAILED: no thread");
    return true;
  }
  thread_deallocate(t);
  piano_policy_stage = 1;
  return true;
}

void PianoPolicy::kick(uint32_t bits)
{
  IOLockLock(fLock);
  fPending |= bits;
  IOLockWakeup(fLock, &fPending, false);
  IOLockUnlock(fLock);
}

bool PianoPolicy::published(void *target, void *ref, IOService *service, IONotifier *)
{
  PianoPolicy *self = static_cast<PianoPolicy *>(target);
  const uintptr_t which = reinterpret_cast<uintptr_t>(ref);
  IOService **slot = which == kRefKeyboard ? &self->fKeyboard : which == kRefPower ? &self->fPowerSource : nullptr;
  if (slot && !*slot) {
    service->retain();
    if (__atomic_exchange_n(slot, service, __ATOMIC_ACQ_REL)) service->release();
    else service->registerInterest(gIOGeneralInterest, &PianoPolicy::interest, self, ref);
    note("watching %s (%s)", service->getMetaClass()->getClassName(), which == kRefKeyboard ? "the cover" : "the power source");
  }
  self->kick(which == kRefKeyboard ? kKeyboard : which == kRefPower ? kPower : kService);
  return true;
}

IOReturn PianoPolicy::interest(void *target, void *ref, UInt32 type, IOService *provider, void *arg, vm_size_t)
{
  PianoPolicy *self = static_cast<PianoPolicy *>(target);
  if (reinterpret_cast<uintptr_t>(ref) == kRefKeyboard && type == kIOMessageServicePropertyChange) {
    // PianoKeyboard sets the property, then messages its name: a rising gpio95 is counted here, so a quick 0 -> 1 -> 0 between two
    // turns of the thread is not lost.
    const char *name = static_cast<const char *>(arg);
    if (name && !strcmp(name, "piano-keyboard-status") && numberProperty(provider, "piano-keyboard-status") == 1)
      __atomic_add_fetch(&self->fStatusRises, 1u, __ATOMIC_ACQ_REL);
    self->kick(kKeyboard);
    return kIOReturnSuccess;
  }
  if (reinterpret_cast<uintptr_t>(ref) == kRefPower && type == kIOPMMessageBatteryStatusHasChanged) {
    self->kick(kPower);
    return kIOReturnSuccess;
  }
  return kIOReturnUnsupported;
}

IOReturn PianoPolicy::rootInterest(void *target, void *, UInt32 type, IOService *, void *arg, vm_size_t)
{
  PianoPolicy *self = static_cast<PianoPolicy *>(target);
  if (type == kIOPMMessageClamshellStateChange) {
    __atomic_store_n(&self->fRootClamshell, uint32_t(uintptr_t(arg)) & (kClamshellStateBit | kClamshellSleepBit), __ATOMIC_RELEASE);
    __atomic_add_fetch(&self->fRootClamshellSeen, 1u, __ATOMIC_ACQ_REL);
    self->kick(kRoot);
    return kIOReturnSuccess;
  }
  if (type == kIOMessageSystemHasPoweredOn) {
    self->kick(kRoot);   // after a wake: everything read again
    return kIOReturnSuccess;
  }
  return kIOReturnUnsupported;
}

// Every input, fresh. Cheap: a few registry property reads.
Inputs PianoPolicy::gather()
{
  Inputs in;
  IOService *kb = __atomic_load_n(&fKeyboard, __ATOMIC_ACQUIRE);
  const int hall = numberProperty(kb, "piano-keyboard-hall");
  in.lid = hall < 0 ? Lid::unknown : hall ? Lid::open : Lid::closed;
  in.attached = numberProperty(kb, "piano-keyboard-attached") == 1;
  IOService *ps = __atomic_load_n(&fPowerSource, __ATOMIC_ACQUIRE);
  const int ext = numberProperty(ps, "ExternalConnected");
  in.power = ext < 0 ? Power::unknown : ext ? Power::external : Power::battery;
  IOService *display = __atomic_load_n(&fDisplay, __ATOMIC_ACQUIRE);
  if (!display) {
    if (IOService *d = copyByClass("NativeGopDisplay")) {
      if (__atomic_exchange_n(&fDisplay, d, __ATOMIC_ACQ_REL)) d->release();
      display = __atomic_load_n(&fDisplay, __ATOMIC_ACQUIRE);
    }
  }
  const int on = numberProperty(display, "piano-display-power");
  in.screen = on < 0 ? Screen::unknown : on ? Screen::on : Screen::off;
  in.lpm = __atomic_load_n(&gLpm, __ATOMIC_ACQUIRE) != 0;
  in.sleepOn = fSleepOn;
  in.wakeMask = fWakeMask;
  // PianoA830's slots (published before its service is: a service found has the property, or never will).
  IOService *gpu = __atomic_load_n(&fGpu, __ATOMIC_ACQUIRE);
  if (!gpu) {
    if (IOService *g = copyByClass("PianoA830")) {
      if (__atomic_exchange_n(&fGpu, g, __ATOMIC_ACQ_REL)) g->release();
      gpu = __atomic_load_n(&fGpu, __ATOMIC_ACQUIRE);
    }
  }
  const int clients = numberProperty(gpu, "piano-gpu-ceiling-clients");
  in.gpuClients = clients > 0 ? uint32_t(clients) : 0;
  // PianoThermal's breadcrumb only while its passive ceiling and ours share one slot.
  const bool shared = !fEngine.gpuOwnSlot() && !gpuOwnSlot(in.gpuClients);
  const int32_t t = fConfig.thermalOwnsGpu && shared ? piano_thermal_state : -1;
  in.thermal = t >= 0 && t <= 3 ? Thermal(t) : Thermal::unknown;
  return in;
}

// PianoThermal's / PianoA830's platform function: taken, the service absent (not published yet, or inert), or no such function.
Result PianoPolicy::call(const char *service, const char *function, uint32_t value)
{
  IOService *s = copyByClass(service);
  if (!s) return Result::absent;
  const IOReturn r = s->callPlatformFunction(function, false, reinterpret_cast<void *>(uintptr_t(value)), nullptr, nullptr, nullptr);
  s->release();
  return r == kIOReturnSuccess ? Result::taken : Result::unsupported;
}

// PianoA830's "piano-gpu-ceiling": the slot from the service called (its piano-gpu-ceiling-clients), so a PianoA830 published after
// the turn's inputs were read still gets client 1; *client is the slot written (kGpuClientShared: param2 null).
Result PianoPolicy::gpuCall(uint32_t khz, uint32_t *client)
{
  IOService *s = copyByClass("PianoA830");
  if (!s) return Result::absent;
  const int clients = numberProperty(s, "piano-gpu-ceiling-clients");
  *client = clients >= 2 ? kGpuClientPolicy : kGpuClientShared;
  const IOReturn r = s->callPlatformFunction("piano-gpu-ceiling", false, reinterpret_cast<void *>(uintptr_t(khz)),
                                             reinterpret_cast<void *>(uintptr_t(*client)), nullptr, nullptr);
  s->release();
  return r == kIOReturnSuccess ? Result::taken : Result::unsupported;
}

void PianoPolicy::perform(const Actions &a, const Inputs &in, bool fromThread)
{
  IOPMrootDomain *root = getPMRootDomain();
  const char *would = fConfig.act ? "" : "would send ";
  for (unsigned i = 0; i < a.n; ++i) {
    const pianopolicy::Action &x = a.a[i];
    switch (x.kind) {
      case Kind::root:
        ++fRootSent;
        if (fConfig.act && root) root->receivePowerNotification(x.value);
        note("root domain: %s%s (%s)", would, rootName(x.value), x.why);
        break;
      case Kind::userActive:
        ++fUserActive;
        if (fConfig.act && root) root->requestUserActive(this, x.why);
        note("root domain: %s (%s): the display wakes", fConfig.act ? "user active requested" : "would request user active", x.why);
        break;
      case Kind::thermalProfile: {
        ++fProfileCalls;
        const Result r = fConfig.act ? call("PianoThermal", "piano-thermal-profile", x.value) : Result::taken;
        if (fromThread) fEngine.profileDone(x.value, r);
        if (r == Result::unsupported && !fProfileMissingLogged) {
          fProfileMissingLogged = true;
          note("PianoThermal does not take \"piano-thermal-profile\" (no CPU frequency control: piano-cpufreq < 2, or a PianoThermal "
               "older than docs/policy.md 5.2): its CPU caps stay by the power source; Low Power Mode on external power changes the GPU "
               "ceiling only");
        } else if (r == Result::absent) {
          note("PianoThermal profile %u (%s): no PianoThermal service (inert without piano-thermal / piano-cpufreq)", x.value, x.why);
        } else if (r == Result::taken) {
          note("PianoThermal: %sprofile %u (%s)", would, x.value, x.why);
        }
        break;
      }
      case Kind::gpuCeiling: {
        ++fGpuCalls;
        uint32_t client = x.client;
        const Result r = fConfig.act ? gpuCall(x.value, &client) : Result::taken;
        // The breadcrumb right after the write: only in the shared slot (the put-back's race); our own slot needs none.
        const int32_t after = fConfig.thermalOwnsGpu && client != kGpuClientPolicy ? piano_thermal_state : -1;
        if (r == Result::taken)
          note("PianoA830: %sGPU ceiling %u MHz (%s; %s)", would, x.value / 1000, x.why,
               client == kGpuClientPolicy ? "client 1, the power profile's slot" : "the shared slot");
        else if (!fGpuMissingLogged) {
          fGpuMissingLogged = true;
          note("PianoA830: GPU ceiling %u MHz not taken (%s): asked again when PianoA830 is published",
               x.value / 1000, r == Result::absent ? "no PianoA830 service: inert without piano-gpu=2" : "no \"piano-gpu-ceiling\"");
        }
        if (!fromThread) break;
        const Actions back = fEngine.gpuDone(x.value, r, after >= 0 && after <= 3 ? Thermal(after) : Thermal::unknown, client);
        for (unsigned j = 0; j < back.n; ++j) {   // (the shared slot only: gpuDone puts nothing back in our own)
          const Result rr = fConfig.act ? call("PianoA830", "piano-gpu-ceiling", back.a[j].value) : Result::taken;
          fEngine.gpuRestored(back.a[j].value, rr);
          note("PianoA830: %sGPU ceiling %u MHz (%s)", would, back.a[j].value / 1000, back.a[j].why);
        }
        break;
      }
    }
  }
  (void)in;
}

void PianoPolicy::describe(const Inputs &in, const View &v)
{
  IOLockLock(gStatusLock);
  snprintf(gStatus, sizeof gStatus,
           "level %u (%s); cover %s, %sattached; %s; display %s; Low Power Mode %u; sleep %s, wake mask %#x; thermal %s; close: %s "
           "(%s); profile %s (%s); GPU ceiling %u MHz (written %u%s); keyboard wake %u, latch %u; sent: root %u, user-active %u, "
           "profile %u, gpu %u; power keys %u, keyboard wakes %u, input changes %u\n",
           gArgs.level, fConfig.act ? "acting" : "decisions only", lidName(in.lid), in.attached ? "" : "not ", powerName(in.power),
           screenName(in.screen), in.lpm ? 1u : 0u, in.sleepOn ? "on" : "off", in.wakeMask, thermalName(in.thermal),
           closeName(v.close), v.closeWhy, profileName(v.profile), v.profileWhy, v.gpuKhz / 1000, fEngine.gpuWritten() / 1000,
           fEngine.gpuOwnSlot() ? ", client 1" : fEngine.gpuHeld() ? ", PianoThermal holds the slot" : ", shared slot",
           v.keyboardWake ? 1u : 0u, v.keyboardLatch ? 1u : 0u, fRootSent,
           fUserActive, fProfileCalls, fGpuCalls, fKeys, fKbdWakes, fInputChanges);
  IOLockUnlock(gStatusLock);
}

void PianoPolicy::publishView(const View &v, const Inputs &in)
{
  const bool first = !fHaveView;
  struct { const char *key; uint64_t now, before; } numbers[] = {
    {"piano-policy-gpu-khz", v.gpuKhz, fLastView.gpuKhz},
    {"piano-policy-keyboard-wake", v.keyboardWake, fLastView.keyboardWake},
    {"piano-policy-keyboard-latch", v.keyboardLatch, fLastView.keyboardLatch},
    {"piano-policy-lpm", in.lpm, fLast.lpm},
  };
  bool changed = false;
  for (auto &n : numbers)
    if (first || n.now != n.before) {
      setProperty(n.key, n.now, 32);
      changed = true;
    }
  if (first || v.close != fLastView.close) {
    setProperty("piano-policy-close", closeName(v.close));
    changed = true;
  }
  if (first || v.profile != fLastView.profile) {
    setProperty("piano-policy-profile", v.profile == Profile::saver ? "saver" : "balanced");
    changed = true;
  }
  fLastView = v;
  fHaveView = true;
  if (changed) {
    setProperty("piano-policy-changes", uint64_t(++fChanges), 32);
    messageClients(kIOMessageServicePropertyChange, const_cast<char *>("piano-policy"));
  }
}

void PianoPolicy::threadMain(void *self, wait_result_t) { static_cast<PianoPolicy *>(self)->run(); }

void PianoPolicy::run()
{
  piano_policy_stage = 2;
  uint32_t tick = 0, clamshellSeen = 0;
  for (;;) {
    IOLockLock(fLock);
    while (!fPending) {
      if (!tick) {
        IOLockSleep(fLock, &fPending, THREAD_UNINT);
        continue;
      }
      uint64_t deadline = 0;
      clock_interval_to_deadline(tick, kMillisecondScale, &deadline);
      if (IOLockSleepDeadline(fLock, &fPending, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) break;
    }
    const uint32_t bits = fPending;
    fPending = 0;
    IOLockUnlock(fLock);

    const uint64_t now = nowMs();
    const Inputs in = gather();
    const bool inputsChanged = !fHaveLast || in.lid != fLast.lid || in.attached != fLast.attached || in.power != fLast.power ||
                               in.screen != fLast.screen || in.lpm != fLast.lpm || in.thermal != fLast.thermal ||
                               in.gpuClients != fLast.gpuClients;
    if (inputsChanged && logged(++fInputChanges))
      note("inputs: cover %s%s, %s, display %s, Low Power Mode %s, thermal %s (change %u)", lidName(in.lid),
           in.lid == Lid::unknown ? "" : in.attached ? " (attached)" : " (no cover attached)", powerName(in.power), screenName(in.screen),
           in.lpm ? "on" : "off", gpuOwnSlot(in.gpuClients) ? "not read (own GPU slot)" : thermalName(in.thermal), fInputChanges);
    if (int(in.gpuClients) != fGpuSlotsLogged && (in.gpuClients || __atomic_load_n(&fGpu, __ATOMIC_ACQUIRE))) {
      fGpuSlotsLogged = int(in.gpuClients);
      if (gpuOwnSlot(in.gpuClients))
        note("PianoA830: %u GPU ceiling slots (piano-gpu-ceiling-clients): Power Saver's ceiling is client 1, the lowest slot wins, so "
             "PianoThermal's passive ceiling stays in force by itself: no breadcrumb read, no GPU tick, no put-back", in.gpuClients);
      else
        note("PianoA830 publishes no piano-gpu-ceiling-clients (one shared slot): Power Saver's ceiling is never written over "
             "PianoThermal's passive one (breadcrumb read, a 5 s tick while ours is in, put back after a race)");
    }
    const Actions a = fEngine.step(in, now);
    perform(a, in, true);
    fSeenOpenForCalls = fEngine.seenOpen();
    // gpio95 rising since the last turn: a display wake if the rules allow it.
    const uint32_t rises = __atomic_load_n(&fStatusRises, __ATOMIC_ACQUIRE);
    if (rises != fStatusRisesSeen) {
      fStatusRisesSeen = rises;
      Actions out;
      const char *why = "";
      const bool wake = fEngine.keyboardRise(in, now, out, &why);
      if (wake) ++fKbdWakes;
      if (wake || logged(rises)) note("keyboard: gpio95 rising (%u): %s", rises, why);
      perform(out, in, true);
    }
    const uint32_t seen = __atomic_load_n(&fRootClamshellSeen, __ATOMIC_ACQUIRE);
    if (seen != clamshellSeen) {
      clamshellSeen = seen;
      const uint32_t s = __atomic_load_n(&fRootClamshell, __ATOMIC_ACQUIRE);
      if (logged(seen))
        note("root domain says: clamshell %s, closing %s", s & kClamshellStateBit ? "closed" : "open",
             s & kClamshellSleepBit ? "sleeps" : "does not sleep");
    }
    const View v = view(fConfig, in, fEngine.seenOpen());
    if (inputsChanged || !fHaveView || v.close != fLastView.close || v.profile != fLastView.profile)
      if (logged(fInputChanges))
        note("decision: closing the cover: %s (%s); profile %s (%s); GPU ceiling %u MHz; keyboard wake %s, latch %s", closeName(v.close),
             v.closeWhy, profileName(v.profile), v.profileWhy, v.gpuKhz / 1000, v.keyboardWake ? "on" : "off",
             v.keyboardLatch ? "on" : "off");
    publishView(v, in);
    describe(in, v);
    fLast = in;
    fHaveLast = true;
    tick = fEngine.tickMs(in);
    (void)bits;
  }
}

IOReturn PianoPolicy::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                           void *param3, void *param4)
{
  if (functionName && functionName->isEqualTo("piano-policy-power-key")) {
    const Inputs in = gather();
    Actions out;
    const char *why = "";
    const uint32_t held = uint32_t(uintptr_t(param1));
    const Key k = fEngine.powerKey(in, held, out, &why);
    const uint32_t n = __atomic_add_fetch(&fKeys, 1u, __ATOMIC_ACQ_REL);
    if (logged(n))
      note("power key: short press (%u ms), display %s: %s%s", held, screenName(in.screen), why,
           k == Key::passThrough ? " (macOS's HID Power usage)" : "");
    perform(out, in, false);
    return k == Key::passThrough ? kIOReturnUnsupported : kIOReturnSuccess;
  }
  if (functionName && functionName->isEqualTo("piano-policy-keyboard-wake")) {
    const Inputs in = gather();
    const View v = view(fConfig, in, fSeenOpenForCalls);
    if (param1) *static_cast<bool *>(param1) = fConfig.act && v.keyboardLatch;
    if (param2) *static_cast<bool *>(param2) = fConfig.act && v.keyboardWake;
    return kIOReturnSuccess;
  }
  return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}
