// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoInput: native input for macOS 27 on piano (SM8750) as HID devices. First source: the three physical keys (power, vol-,
// vol+), read from the PMICs over SPMI (Buttons.h, SpmiObserver.h) and published as a Consumer Control (HidReports.h) that
// IOHIDFamily's IOHIDEventDriver turns into Power / Volume Increment / Volume Decrement key events. The touch screen (NT36532E
// over GENI SPI) is to be the second source, a digitizer PianoHIDDevice of its own.
//
// The HID class (provenance: UPSTREAM) is the only IOHIDDevice subclass proven on this macOS
// 27.0.1 kernel collection (its 334-slot vtable checked at KC link time); the same overrides, without virtio. No interrupts:
// a kernel thread polls, read only (no PMIC register is written, no interrupt configured): every 10 ms while a key is down or
// changing and for a second after, every piano-input-idle ms otherwise (docs/power-irq.md; the PMIC arbiter's interrupt path is
// described there, not used).
//
// Boot arguments (default: inert, the service does not even probe):
//   piano-input=1        publish the buttons device and poll
//   piano-input=2        the same, and log every key edge and report
//   piano-input-mask=N   the keys reported to macOS (bit 0 power, bit 1 vol+, bit 2 vol-; default 7); masked keys are still logged
//   piano-input-idle=MS  the poll period while no key is busy (default 40; 0: every 10 ms always, as before; at most 200)
//   piano-wake-log=S     the "idle: N wakeups" line every S seconds (default 60, 0 none; shared with the other converted drivers)
// The power key with PianoPolicy (docs/policy.md 5.1, PowerKey.h): while a PianoPolicy service is there (piano-policy=N), a press is
// not reported at once: still down after 1.5 s it is a long press (the HID Power usage down then, up at the release); released
// before, PianoPolicy is asked on the release (callPlatformFunction "piano-policy-power-key", the press in ms): taken, macOS gets
// nothing; not taken, the HID Power usage down and up at once. Without PianoPolicy the key is reported as it happens, as before.
// System sleep (docs/power.md 4, PianoPowerContract.h): in the power tree; setPowerState(off) parks the poll thread between two
// samples (no SPMI read while the kernel polls the power key through the same observer channel), on reads the keys afresh (a key
// down then is reported only after it has been up once, as at start; the press that woke the tablet the kernel held until released).
// Every step logs "PianoInput: ...": stage 1-6 of start, the raw PMIC registers once, the event driver attaching, and an
// "alive" line with counters 15 s and 60 s after start, then every 5 minutes.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "SpmiObserver.h"
#include "Buttons.h"
#include "HidReports.h"
#include "PowerKey.h"
#include <PianoPowerContract.h>

using namespace pianoinput;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last start stage reached, negative when it failed there.
volatile int32_t piano_input_stage = 0;
}

namespace {

struct Args {
  uint32_t level;   // piano-input
  uint32_t mask;    // piano-input-mask
  uint32_t idle;    // piano-input-idle
};
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}

void readArgs()
{
  gArgs.level = bootArg("piano-input", 0);
  gArgs.mask = bootArg("piano-input-mask", kKeysAll) & kKeysAll;
  gArgs.idle = PollPlan::clampIdle(bootArg("piano-input-idle", kIdlePollMs));
}

// Explicit ldr/str, as PianoDwc3 does: every store from a general register.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }

struct Region {
  IOMemoryMap *map;
  uintptr_t va;
};

bool mapRegion(Region &r, uint64_t pa, uint64_t bytes)
{
  if (r.va) return true;
  const uint64_t base = pa & ~0xfffull, length = ((pa + bytes + 0xfff) & ~0xfffull) - base;
  IODeviceMemory *m = IODeviceMemory::withRange(base, length);
  r.map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  r.va = r.map ? uintptr_t(r.map->getVirtualAddress()) + uintptr_t(pa - base) : 0;
  return r.va != 0;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoInput: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_input_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// The SPMI arbiter's three windows, and SpmiObserver.h's Io over them.
Region gCore, gCnfg, gObsv;

uintptr_t windowBase(Window w) { return w == Window::core ? gCore.va : w == Window::cnfg ? gCnfg.va : gObsv.va; }
uint32_t ioRd(void *, Window w, uint32_t offset) { return rd32(windowBase(w) + offset); }
void ioWr(void *, Window w, uint32_t offset, uint32_t value) { wr32(windowBase(w) + offset, value); }
void ioDelay(void *, unsigned us) { IODelay(us); }
const Io gIo = {nullptr, ioRd, ioWr, ioDelay};

const char *resultName(ReadResult r) { return r == ReadResult::ok ? "ok" : r == ReadResult::error ? "error" : "timeout"; }

}  // namespace

// ------------------------------------------------------------------------------------------------------ the HID device
// An IOHIDDevice subclass (UPSTREAM) with what a device says about itself taken from HidReports.h's DeviceInfo.
class PianoHIDDevice : public IOHIDDevice {
  OSDeclareDefaultStructors(PianoHIDDevice)

public:
  Kind fKind = Kind::buttons;

  IOReturn newReportDescriptor(IOMemoryDescriptor **descriptor) const override;
  OSString *newTransportString() const override { return OSString::withCString("SPMI"); }
  OSString *newManufacturerString() const override { return OSString::withCString("UniXNU"); }
  OSString *newProductString() const override { return OSString::withCString(deviceInfo(fKind).product); }
  OSNumber *newVendorIDNumber() const override { return OSNumber::withNumber(kVendorId, 32); }
  OSNumber *newProductIDNumber() const override { return OSNumber::withNumber(deviceInfo(fKind).productId, 32); }
  OSNumber *newLocationIDNumber() const override { return OSNumber::withNumber(0x50490000u | unsigned(fKind), 32); }
  OSNumber *newPrimaryUsagePageNumber() const override { return OSNumber::withNumber(deviceInfo(fKind).usagePage, 32); }
  OSNumber *newPrimaryUsageNumber() const override { return OSNumber::withNumber(deviceInfo(fKind).usage, 32); }
};

OSDefineMetaClassAndStructors(PianoHIDDevice, IOHIDDevice)

IOReturn PianoHIDDevice::newReportDescriptor(IOMemoryDescriptor **descriptor) const
{
  const DeviceInfo &info = deviceInfo(fKind);
  *descriptor = IOBufferMemoryDescriptor::withBytes(info.descriptor, info.descriptorBytes, kIODirectionNone);
  return *descriptor ? kIOReturnSuccess : kIOReturnNoMemory;
}

// ------------------------------------------------------------------------------------------------------------ the service
class PianoInput : public IOService {
  OSDeclareDefaultStructors(PianoInput)

  PianoHIDDevice *fButtons = nullptr;
  IONotifier *fPublished = nullptr;
  int fPonApid = -1, fGpioApid = -1;
  Debounce fFilter;
  uint8_t fDown = 0, fSent = 0, fPon = 0, fGpio = 0;
  uint64_t fSamples = 0, fStartMs = 0;
  uint32_t fReadErrors = 0, fEdges = 0, fReports = 0, fSendErrors = 0;
  // the power key with PianoPolicy (PowerKey.h)
  PowerKey fPower;
  IOService *fPolicy = nullptr;   // PianoPolicy, looked up at each power key press until found, then kept
  uint32_t fPolicyTaken = 0, fPolicyPassed = 0;
  // system sleep: the poll thread parks between kOff and kOn
  IOLock *fLock = nullptr;
  bool fRunning = false, fSleepWanted = false, fParked = false, fWentOff = false;
  uint32_t fSleeps = 0;

  bool probeKeys();
  bool publishButtons();
  void poll();
  void send(uint8_t keys);
  IOService *policy();
  void askPolicy(uint32_t heldMs, uint8_t report);
  bool park();
  void describe(uint64_t seconds);
  static void pollThread(void *self, wait_result_t);
  static bool eventServicePublished(void *target, void *refCon, IOService *service, IONotifier *notifier);

public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
};

OSDefineMetaClassAndStructors(PianoInput, IOService)

IOService *PianoInput::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoInput::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-input=%u mask %#x: power, vol-, vol+ over SPMI, polled every %u ms (%u ms while idle%s), debounce %u ms, read only",
       gArgs.level, gArgs.mask, kPollMs, gArgs.idle, gArgs.idle > kPollMs ? "" : ": not adaptive", kDebounceMs);
  setProperty("piano-input", gArgs.level, 32);
  if (!probeKeys() || !publishButtons()) {
    setProperty("piano-input-stage", uint64_t(uint32_t(piano_input_stage)), 32);
    return true;   // stays attached and does nothing more
  }
  stage(5, "poll thread");
  fStartMs = nowMs();
  fLock = IOLockAlloc();
  thread_t thread = nullptr;
  if (!fLock || kernel_thread_start(pollThread, this, &thread) != KERN_SUCCESS) {
    stage(-5, "poll thread: kernel_thread_start");
    return true;
  }
  thread_deallocate(thread);
  fRunning = true;
  registerService();
  stage(6, "running: the keys go to macOS as consumer usages Power 0x30, Volume Increment 0xE9, Volume Decrement 0xEA");
  setProperty("piano-input-stage", uint64_t(uint32_t(piano_input_stage)), 32);
  note("power key: %s; system sleep: the poll thread parks while suspended (power tree joined: %#x)",
       gArgs.mask & kKeyPower ? "held back for PianoPolicy while its service is there (a press under 1.5 s goes to it on the release, "
                                "a longer one to macOS), else reported as it happens"
                              : "masked (piano-input-mask): neither macOS nor PianoPolicy gets it",
       pianopower::join(this));
  return true;
}

// Stages 1-3: the arbiter, the two key peripherals' APIDs, and their registers once (proof that the reads work).
bool PianoInput::probeKeys()
{
  stage(1, "map the SPMI arbiter (core 0x0c400000, cnfg 0x0c42d000, obsv 0x0c440000)");
  if (!mapRegion(gCore, kSpmiCore, kSpmiCoreBytes) || !mapRegion(gCnfg, kSpmiCnfg, kSpmiCnfgBytes) ||
      !mapRegion(gObsv, kSpmiObsv, kSpmiObsvBytes)) {
    stage(-1, "cannot map the SPMI arbiter");
    return false;
  }
  stage(2, "SPMI arbiter v7 and the APIDs of PON (PPID 0x013) and PM8550 GPIO6 (PPID 0x18d)");
  const Arbiter arbiter = spmiArbiter(gIo);
  note("SPMI arbiter version %08x, %u peripherals%s", arbiter.version, arbiter.count, arbiter.v7 ? "" : ": not the v7 layout");
  if (!arbiter.v7) {
    stage(-2, "not an SPMI arbiter v7: nothing read");
    return false;
  }
  const Apid pon = spmiFindApid(gIo, arbiter, kPonPpid), gpio = spmiFindApid(gIo, arbiter, kVolUpGpioPpid);
  note("PON (SID 0 0x1300) APID %d owner EE %u; GPIO6 (SID 1 0x8d00) APID %d owner EE %u", pon.apid, pon.owner, gpio.apid, gpio.owner);
  if (pon.apid < 0 || gpio.apid < 0) {
    stage(-2, "a key peripheral is not in the APID map");
    return false;
  }
  fPonApid = pon.apid;
  fGpioApid = gpio.apid;

  stage(3, "raw key registers, read once");
  struct Raw {
    const char *what;
    int apid;
    uint8_t reg, value;
    ReadResult result;
    uint32_t status;
  } raw[] = {
    {"PON type", fPonApid, kRegType, 0, ReadResult::timeout, 0},
    {"PON subtype", fPonApid, kRegSubtype, 0, ReadResult::timeout, 0},
    {"PON rt", fPonApid, kRegRtStatus, 0, ReadResult::timeout, 0},
    {"GPIO6 type", fGpioApid, kRegType, 0, ReadResult::timeout, 0},
    {"GPIO6 subtype", fGpioApid, kRegSubtype, 0, ReadResult::timeout, 0},
    {"GPIO6 rt", fGpioApid, kRegRtStatus, 0, ReadResult::timeout, 0},
    {"GPIO6 mode", fGpioApid, kRegGpioMode, 0, ReadResult::timeout, 0},
    {"GPIO6 enable", fGpioApid, kRegGpioEnable, 0, ReadResult::timeout, 0},
  };
  char line[300];
  size_t used = 0;
  line[0] = 0;
  for (Raw &r : raw) {
    r.result = spmiRead8(gIo, r.apid, r.reg, &r.value, &r.status);
    if (used < sizeof line) {
      if (r.result == ReadResult::ok)
        used += size_t(snprintf(line + used, sizeof line - used, "%s%s [%02x] %02x", used ? ", " : "", r.what, r.reg, r.value));
      else
        used += size_t(snprintf(line + used, sizeof line - used, "%s%s [%02x] %s (status %08x)", used ? ", " : "", r.what, r.reg,
                                resultName(r.result), r.status));
    }
  }
  note("raw: %s", line);
  // The two peripherals' interrupt configuration as the firmware left it (read only): the facts a PMIC-arbiter interrupt path
  // would start from (docs/power-irq.md). LATCHED/PENDING set without an enable would show the latch works on its own.
  const struct {
    const char *name;
    int apid;
  } peripherals[] = {{"PON", fPonApid}, {"GPIO6", fGpioApid}};
  for (const auto &peripheral : peripherals) {
    static const uint8_t kIrqRegs[] = {kRegIntSetType, kRegIntPolarityHigh, kRegIntPolarityLow, kRegIntEnable, kRegIntLatched,
                                       kRegIntPending};
    static const char *const kIrqNames[] = {"type", "pol-high", "pol-low", "enabled", "latched", "pending"};
    used = 0;
    line[0] = 0;
    for (unsigned i = 0; i < sizeof kIrqRegs; ++i) {
      uint8_t value = 0;
      uint32_t status = 0;
      const ReadResult result = spmiRead8(gIo, peripheral.apid, kIrqRegs[i], &value, &status);
      if (used < sizeof line)
        used += size_t(result == ReadResult::ok
                         ? snprintf(line + used, sizeof line - used, "%s%s [%02x] %02x", used ? ", " : "", kIrqNames[i], kIrqRegs[i], value)
                         : snprintf(line + used, sizeof line - used, "%s%s [%02x] %s", used ? ", " : "", kIrqNames[i], kIrqRegs[i],
                                    resultName(result)));
    }
    note("irq registers %s: %s", peripheral.name, line);
  }
  if (raw[2].result != ReadResult::ok || raw[5].result != ReadResult::ok) {
    stage(-3, "the key status registers (PON 0x1310, GPIO6 0x8d10) cannot be read");
    return false;
  }
  fPon = raw[2].value;
  fGpio = raw[5].value;
  const uint8_t keys = decodeKeys(fPon, fGpio);
  note("keys now: power %s, vol- %s, vol+ %s (PON [10] %02x bits 7/6, GPIO6 [10] %02x bit 0 active low)%s",
       keys & kKeyPower ? "DOWN" : "up", keys & kKeyVolumeDown ? "DOWN" : "up", keys & kKeyVolumeUp ? "DOWN" : "up", fPon, fGpio,
       keys ? "; a key down now is reported only after it has been up once" : "");
  return true;
}

// Stage 4: the buttons' IOHIDDevice under this service: OSTypeAlloc, init, attach, start (UPSTREAM).
bool PianoInput::publishButtons()
{
  stage(4, "HID device: Consumer Control (Power, Volume Increment, Volume Decrement)");
  PianoHIDDevice *hid = OSTypeAlloc(PianoHIDDevice);
  if (hid) hid->fKind = Kind::buttons;
  if (!hid || !hid->init(nullptr)) {
    OSSafeReleaseNULL(hid);
    stage(-4, "HID device: init");
    return false;
  }
  hid->setProperty(kIOHIDBuiltInKey, true);
  if (!hid->attach(this)) {
    OSSafeReleaseNULL(hid);
    stage(-4, "HID device: attach");
    return false;
  }
  if (!hid->start(this)) {
    hid->detach(this);
    OSSafeReleaseNULL(hid);
    stage(-4, "HID device: IOHIDDevice::start");
    return false;
  }
  fButtons = hid;
  note("HID device %s: vendor %#x product %#x, usage %#x/%#x, %u-byte descriptor, registry ID %#llx", deviceInfo(Kind::buttons).product,
       kVendorId, kProductButtons, deviceInfo(Kind::buttons).usagePage, deviceInfo(Kind::buttons).usage,
       deviceInfo(Kind::buttons).descriptorBytes, hid->getRegistryEntryID());
  // IOHIDEventDriver on the device's IOHIDInterface is what turns the reports into key events: say when it is there.
  if (OSDictionary *matching = IOService::serviceMatching("IOHIDEventService")) {
    fPublished = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, eventServicePublished, this, nullptr);
    matching->release();
  }
  return true;
}

bool PianoInput::eventServicePublished(void *, void *, IOService *service, IONotifier *)
{
  for (IOService *provider = service->getProvider(); provider; provider = provider->getProvider())
    if (OSDynamicCast(PianoHIDDevice, provider)) {
      IOService *interface = service->getProvider();
      note("event service %s published on %s of %s: macOS has the keys", service->getMetaClass()->getClassName(),
           interface ? interface->getMetaClass()->getClassName() : "-", deviceInfo(static_cast<PianoHIDDevice *>(provider)->fKind).product);
      break;
    }
  return true;
}

void PianoInput::pollThread(void *self, wait_result_t)
{
  PianoInput *input = static_cast<PianoInput *>(self);
  note("poll thread running (APIDs %d and %d)", input->fPonApid, input->fGpioApid);
  uint64_t nextDescribe = 15000;
  PollPlan plan(gArgs.idle);
  pianoirq::WakeStats wakes = pianoirq::wakeStats(pianoirq::wakeLogMs());
  uint32_t fastWakes = 0;
  for (;;) {
    if (input->park()) {   // woken from a system sleep: the keys read afresh, as at start
      input->fFilter = Debounce();
      input->fPower.reset();
      plan = PollPlan(gArgs.idle);
      note("wake %u: polling again; the keys are read afresh (a key down now is reported only after it has been up once)",
           input->fSleeps);
    }
    input->poll();
    const uint64_t now = nowMs(), elapsed = now - input->fStartMs;
    if (elapsed >= nextDescribe) {
      input->describe(elapsed / 1000);
      nextDescribe = nextDescribe < 60000 ? 60000 : nextDescribe + 300000;
    }
    const uint32_t period = plan.next(input->fFilter, now);
    pianoirq::WakeStats::Window w;
    if (wakes.due(now, &w)) {
      note("idle: %u wakeups in %u s, irq 0 (polled: %u at %u ms while a key was busy, %u at %u ms idle)", w.wakeups, w.seconds, fastWakes,
           kPollMs, w.wakeups - fastWakes, gArgs.idle);
      fastWakes = 0;
    }
    if (period == kPollMs) ++fastWakes;
    IOSleep(period);
    wakes.count(pianoirq::Woke::timeout);
  }
}

void PianoInput::poll()
{
  uint8_t pon = 0, gpio = 0;
  uint32_t status = 0;
  ReadResult result = spmiRead8(gIo, fPonApid, kRegRtStatus, &pon, &status);
  const char *which = "PON 0x1310";
  if (result == ReadResult::ok) {
    result = spmiRead8(gIo, fGpioApid, kRegRtStatus, &gpio, &status);
    which = "GPIO6 0x8d10";
  }
  const bool valid = result == ReadResult::ok;
  ++fSamples;
  if (valid) {
    fPon = pon;
    fGpio = gpio;
  } else if (++fReadErrors <= 5 || fReadErrors % 1000 == 0) {
    note("read error %u: %s %s (status %08x); counted as all keys up", fReadErrors, which, resultName(result), status);
  }
  const uint8_t down = fFilter.update(valid, valid ? decodeKeys(pon, gpio) : 0, nowMs());
  if (const uint8_t changed = down ^ fDown) {
    fDown = down;
    for (uint8_t bit = 1; bit & kKeysAll; bit = uint8_t(bit << 1)) {
      if (!(changed & bit)) continue;
      ++fEdges;
      if (gArgs.level >= 2)
        note("key %s %s (PON [10] %02x, GPIO6 [10] %02x)%s", keyName(bit), down & bit ? "down" : "up", fPon, fGpio,
             gArgs.mask & bit ? "" : ", masked: not reported");
    }
  }
  // The power key (PowerKey.h): with PianoPolicy there at the press it is held back; a short press is PianoPolicy's to take.
  const bool power = down & kKeyPower;
  const bool hold = fPower.starts(power) && (gArgs.mask & kKeyPower) && policy();
  uint32_t heldMs = 0;
  const uint64_t at = nowMs();
  const uint32_t longs = fPower.longPresses();
  const PowerStep step = fPower.update(power, hold, at, &heldMs);
  const uint8_t report = fPower.report(down, gArgs.mask);
  if (step == PowerStep::ask) askPolicy(heldMs, report);
  else if (fPower.longPresses() != longs)
    note("power key: held %u ms: a long press, macOS's (HID Power usage down now, up at the release)", kLongPressMs);
  if (report != fSent) send(report);   // also retries a report that failed
}

// PianoPolicy's service by class (no link dependency): looked up at a power key press until found, then kept (a kext of the
// collection never unloads). Absent (piano-policy=0: it does not probe), the power key is reported as it happens.
IOService *PianoInput::policy()
{
  if (fPolicy) return fPolicy;
  OSDictionary *match = IOService::serviceMatching("PianoPolicy");
  fPolicy = match ? IOService::copyMatchingService(match) : nullptr;
  OSSafeReleaseNULL(match);
  if (fPolicy) note("power key: PianoPolicy found: a press under %u ms is asked of it on the release (docs/policy.md 5.1)", kLongPressMs);
  return fPolicy;
}

// A held-back short press ended: PianoPolicy takes it (sleep now, or a display wake), else macOS gets the Power usage down and up now.
void PianoInput::askPolicy(uint32_t heldMs, uint8_t report)
{
  const IOReturn r = fPolicy ? fPolicy->callPlatformFunction("piano-policy-power-key", false, reinterpret_cast<void *>(uintptr_t(heldMs)),
                                                             nullptr, nullptr, nullptr)
                             : kIOReturnUnsupported;
  if (r == kIOReturnSuccess) {
    ++fPolicyTaken;
    note("power key: short press %u ms: PianoPolicy took it", heldMs);
    return;
  }
  ++fPolicyPassed;
  note("power key: short press %u ms: macOS's (HID Power usage)", heldMs);
  send(uint8_t(report | kKeyPower));
  send(report);
}

// setPowerState(kOff) asks the poll thread to stop; it parks here, between two samples, until kOn. True after a park (a wake).
bool PianoInput::park()
{
  if (!__atomic_load_n(&fSleepWanted, __ATOMIC_ACQUIRE)) return false;
  IOLockLock(fLock);
  const bool parking = fSleepWanted;
  if (parking) {
    fParked = true;
    IOLockWakeup(fLock, &fParked, false);
    while (fSleepWanted) IOLockSleep(fLock, &fSleepWanted, THREAD_UNINT);
    fParked = false;
  }
  IOLockUnlock(fLock);
  return parking;
}

// docs/power.md 4: off = the poll thread parked (nothing of this driver is powered: the PMIC keys are the kernel's while suspended);
// on = the thread polls again. Synchronous both ways: the park is at most one poll period away (piano-input-idle, <= 200 ms).
IOReturn PianoInput::setPowerState(unsigned long state, IOService *)
{
  if (!fRunning) return pianopower::kAckNow;
  if (state == pianopower::kOff) {
    IOLockLock(fLock);
    __atomic_store_n(&fSleepWanted, true, __ATOMIC_RELEASE);
    uint64_t deadline = 0;
    clock_interval_to_deadline(500, kMillisecondScale, &deadline);
    while (!fParked && IOLockSleepDeadline(fLock, &fParked, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
    }
    const bool parked = fParked;
    IOLockUnlock(fLock);
    fWentOff = true;
    note("sleep %u: %s; power key presses %u short (PianoPolicy took %u, macOS %u), %u long", ++fSleeps,
         parked ? "the poll thread is parked (the kernel reads the power key while suspended)"
                : "the poll thread did not park within 500 ms (it parks at its next sample)",
         fPower.shortPresses(), fPolicyTaken, fPolicyPassed, fPower.longPresses());
    return pianopower::kAckNow;
  }
  if (!fWentOff) return pianopower::kAckNow;   // the power tree's first kOn (pianopower::join)
  fWentOff = false;
  IOLockLock(fLock);
  __atomic_store_n(&fSleepWanted, false, __ATOMIC_RELEASE);
  IOLockWakeup(fLock, &fSleepWanted, false);
  IOLockUnlock(fLock);
  return pianopower::kAckNow;
}

void PianoInput::send(uint8_t keys)
{
  IOBufferMemoryDescriptor *report = IOBufferMemoryDescriptor::withBytes(&keys, kButtonsReportBytes, kIODirectionNone);
  const IOReturn rc = report ? fButtons->handleReport(report) : kIOReturnNoMemory;
  OSSafeReleaseNULL(report);
  ++fReports;
  if (rc == kIOReturnSuccess) fSent = keys;   // else the next sample tries again
  if (rc != kIOReturnSuccess && ++fSendErrors > 5 && fSendErrors % 1000) return;   // a failing report: the first 5, then every 1000th
  if (gArgs.level >= 2 || rc != kIOReturnSuccess)
    note("report %u: %02x (power %u, vol+ %u, vol- %u) handleReport %#x", fReports, keys, keys & kKeyPower ? 1 : 0,
         keys & kKeyVolumeUp ? 1 : 0, keys & kKeyVolumeDown ? 1 : 0, rc);
}

// Bring-up: is the thread alive, and who is attached to the buttons' IOHIDInterface (IOHIDEventDriver, and its user clients).
void PianoInput::describe(uint64_t seconds)
{
  note("alive at %llu s: %llu samples, %u read errors, %u key edges, %u reports, keys %02x, PON [10] %02x, GPIO6 [10] %02x", seconds,
       fSamples, fReadErrors, fEdges, fReports, fDown, fPon, fGpio);
  OSIterator *interfaces = fButtons->getChildIterator(gIOServicePlane);
  while (OSObject *object = interfaces ? interfaces->getNextObject() : nullptr) {
    IOService *interface = OSDynamicCast(IOService, object);
    if (!interface) continue;
    OSIterator *drivers = interface->getChildIterator(gIOServicePlane);
    unsigned n = 0;
    while (OSObject *child = drivers ? drivers->getNextObject() : nullptr) {
      IOService *driver = OSDynamicCast(IOService, child);
      unsigned users = 0;
      OSIterator *clients = driver ? driver->getClientIterator() : nullptr;
      while (OSObject *client = clients ? clients->getNextObject() : nullptr) users += OSDynamicCast(IOUserClient, client) != nullptr;
      OSSafeReleaseNULL(clients);
      note("buttons: %s -> %s, %u user client(s)", interface->getMetaClass()->getClassName(), child->getMetaClass()->getClassName(), users);
      ++n;
    }
    if (!n) note("buttons: %s has no client (no event driver: the keys go nowhere)", interface->getMetaClass()->getClassName());
    OSSafeReleaseNULL(drivers);
  }
  OSSafeReleaseNULL(interfaces);
}
