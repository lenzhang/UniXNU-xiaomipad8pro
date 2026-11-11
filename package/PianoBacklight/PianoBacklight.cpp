// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoBacklight: the panel backlight of the Xiaomi Pad 8 Pro (piano, SM8750) for native macOS 27 at EL1. Two Kinetic KTZ8866 ICs at
// I2C address 0x11 drive the panel's LED strings and get the same brightness (Ktz8866.h: the register facts, their sources and the
// state machine). The primary sits on the I2C hub's SE3 (DT i2c@98c000, pins gpio70 SDA / gpio71 SCL, function i2chub0_se3), the
// secondary on QUP2 SE0 (DT i2c@880000, gpio0 SDA / gpio1 SCL, function qup2_se0). Both buses are driven in software (BitI2c.h) on
// their TLMM pins, as PianoKeyboard drives the cover's: for each burst of accesses the two pins are switched from their QUP function
// to GPIO (open drain: released = input with the pull-up, low = output enable with the output value 0) and given back exactly as
// found afterwards (CTL and IO registers rewritten with the values the survey read, then read back). No QUP engine, GCC clock or RPMh
// vote is touched, so no lock of PianoA830's is needed. Never touched: the ICs' HWEN (TLMM gpio2, read only), the panel's bias
// enables (gpio117/118), its reset (gpio98), DSI, DPU, and every KTZ8866 register but 0x04, 0x05 and 0x08 bit 6.
//
// One kernel thread does everything:
//   1 survey   read only: the five pins' TLMM registers (gpio70, 71, 0, 1 and HWEN gpio2), then both ICs' registers (ID, the
//              backlight and bias configuration, the code, BL_EN); nothing is written at level 1
//   2 control  the found state adopted, the boot level applied, then events: NativeGopDisplay's display power (off -> BL_EN cleared on
//              both ICs; on -> the code, then BL_EN), brightness steps (PianoBacklightClient.h), the cover's hall sensor (PianoKeyboard's
//              "piano-keyboard-hall" property, polled; acted on only with piano-backlight-lid=1), "piano-backlight-percent" /
//              "piano-backlight-step" set as properties by an administrator; a failed access retried every second
// Every write is logged ("PianoBacklight: write ..."), at most 24 such lines per 10 s (the rest counted).
// System sleep (docs/power.md 4, PianoPowerContract.h): at level 2 the service is in the power tree. setPowerState(off) has the
// thread clear BL_EN on both ICs (whatever the display path said; retried twice) and waits for it (at most 800 ms); on has the
// thread restore what the display and the lid want. HWEN (gpio2) stays untouched: it is read once, never written.
//
// Boot arguments (default: inert, the service does not even probe):
//   piano-backlight=1           the survey (pins, both ICs read back, the found brightness); writes nothing
//   piano-backlight=2           control (and NativeGopDisplay calls out its display power: it reads this argument too)
//   piano-backlight-level=P     boot brightness, P % of the 11-bit range, set whatever was found (default at level 2: 45, applied
//                               only when the boot stages left more; 0: leave as found)
//   piano-backlight-min=CODE    the floor of steps and levels (default 128 of 2047; 32..1024); off is BL_EN, never code 0
//   piano-backlight-lid=1       cover closed (PianoKeyboard's hall sensor, piano-keyboard>=1) -> backlight off, open -> on again
//   piano-backlight-keys=1      read by the producers (PianoDwc3's HID input from the Mac): brightness usages 0x6F/0x70 and Right
//                               Ctrl + 1/2 step the backlight
//   piano-backlight-flags=1     the survey also reads FLAG (0x0f), which clears its latched fault bits
//   piano-backlight-halfbit=US  I2C half-bit time (default 5 us: ~100 kHz; 2..50)
//   piano-backlight-swap=B      SDA/SCL swapped on bus 0 (bit 0, primary) / bus 1 (bit 1, secondary): only if the survey's reads
//                               fail with "no ack (address)" on idle-high pins
//   piano-backlight-auto=1      automatic brightness (level 2): targets of the ambient light policy, permille of the range, from
//                               the platform function "PianoBacklightAmbient" or PianoSensors' property piano-sensors-light-target
//                               (polled every 250 ms with its piano-sensors-seq; nothing happens until PianoSensors publishes it);
//                               the brightness keys then shift the curve by the steps (kept for the boot); piano-backlight-percent
//                               switches to manual, piano-backlight-auto=0/1 as a property leaves / enters again. Without it nothing
//                               of this runs (docs/drivers/backlight.md, sensors.md 5a.4)
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "BitI2c.h"
#include "Ktz8866.h"
#include "PianoBacklightClient.h"
#include <PianoPowerContract.h>

using namespace pianobacklight;

extern "C" {
// Post-mortem breadcrumb: 1 survey, 2 control running, 3 level 1 done; negative: failed there.
volatile int32_t piano_backlight_stage = 0;
}

namespace {

struct Args {
  uint32_t level, percent, percentGiven, floor, lid, keys, flags, halfUs, swap, autoAllowed;
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
  bool given = false;
  gArgs.level = bootArg("piano-backlight", 0);
  gArgs.percent = clampArg(bootArg("piano-backlight-level", 45, &given), 0, 100);
  gArgs.percentGiven = given;
  gArgs.floor = clampArg(bootArg("piano-backlight-min", 128), 32, 1024);
  gArgs.lid = bootArg("piano-backlight-lid", 0);
  gArgs.keys = bootArg("piano-backlight-keys", 0);
  gArgs.flags = bootArg("piano-backlight-flags", 0);
  gArgs.halfUs = clampArg(bootArg("piano-backlight-halfbit", 5), 2, 50);
  gArgs.swap = bootArg("piano-backlight-swap", 0);
  gArgs.autoAllowed = bootArg("piano-backlight-auto", 0) == 1;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoBacklight: %s\n", line);
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// Explicit ldr/str with a barrier after each store, as PianoKeyboard does: a bus line has moved before the half-bit wait starts.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]\n dsb sy" ::"r"(v), "r"(a) : "memory"); }

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

// ------------------------------------------------------------------------------------------------------------- the pins (TLMM)
// TLMM (DT pinctrl@f100000, qcom,sm8750-tlmm), pin n at n * 0x1000 (PianoKeyboard.cpp's facts, run on piano): CTL (pull 1:0, 3 up;
// function 5:2; drive 8:6; output enable 9; eGPIO present 11 / enable 12), IO (bit 0 the pad, bit 1 the output value). None of the
// five pins is in the DT's gpio-reserved-ranges (36-39, 48-51, 74). Pin order in the DT's pinctrl states: SDA first, then SCL.
constexpr uint64_t kTlmm = 0x0f100000;
constexpr uint32_t kCtl = 0x0, kIo = 0x4;
constexpr uint32_t kPullUp = 3, kFunction = 0xfu << 2, kOe = 1u << 9, kEgpioPresent = 1u << 11, kEgpioEnable = 1u << 12;

struct Pin {
  unsigned gpio;
  const char *what;
  Region page;
  uint32_t ctl, io;   // as found
};
enum : unsigned { kPrimarySda, kPrimaryScl, kSecondarySda, kSecondaryScl, kHwen, kPinCount };
Pin gPin[kPinCount] = {
  {70, "primary SDA (i2c@98c000, I2C hub SE3)", {}, 0, 0}, {71, "primary SCL", {}, 0, 0},
  {0, "secondary SDA (i2c@880000, QUP2 SE0)", {}, 0, 0}, {1, "secondary SCL", {}, 0, 0},
  {2, "KTZ8866 HWEN (read only, never written)", {}, 0, 0},
};

struct Bus {
  unsigned sda, scl;   // gPin indexes
  bool usable, muxed;
  BitI2c i2c;
  uint32_t reads, writes, errors, restores, restoreMismatches;
};
Bus gBus[kIcs] = {{kPrimarySda, kPrimaryScl, false, false, {}, 0, 0, 0, 0, 0}, {kSecondarySda, kSecondaryScl, false, false, {}, 0, 0, 0, 0, 0}};

// An eGPIO-capable pin whose eGPIO enable is clear belongs to the other (LPI) TLMM: not driven from here.
bool foreign(const Pin &p) { return (p.ctl & kEgpioPresent) && !(p.ctl & kEgpioEnable); }
uint32_t gpioCtl(const Pin &p, bool low) { return (p.ctl & ~(kFunction | kOe | 3u)) | kPullUp | (low ? kOe : 0); }
void lineSda(void *ctx, bool release) { const Pin &p = gPin[static_cast<Bus *>(ctx)->sda]; wr32(p.page.va + kCtl, gpioCtl(p, !release)); }
void lineScl(void *ctx, bool release) { const Pin &p = gPin[static_cast<Bus *>(ctx)->scl]; wr32(p.page.va + kCtl, gpioCtl(p, !release)); }
bool readSda(void *ctx) { return rd32(gPin[static_cast<Bus *>(ctx)->sda].page.va + kIo) & 1; }
bool readScl(void *ctx) { return rd32(gPin[static_cast<Bus *>(ctx)->scl].page.va + kIo) & 1; }
void delayUs(void *, unsigned us) { IODelay(us); }

const char *icName(unsigned i) { return kIcName[i]; }

// The bus's pins to GPIO, both released (output value 0 first, then CTL with the function 0 and the output disabled).
bool muxToGpio(Bus &b)
{
  const unsigned pins[2] = {b.sda, b.scl};
  for (unsigned idx : pins) {
    const Pin &p = gPin[idx];
    wr32(p.page.va + kIo, 0);
    wr32(p.page.va + kCtl, gpioCtl(p, false));
  }
  b.muxed = true;
  IODelay(2 * gArgs.halfUs);
  return true;
}
// Back exactly as found: IO's output value, then CTL (the QUP function, pull, drive); both read back.
void restorePins(Bus &b)
{
  bool same = true;
  const unsigned pins[2] = {b.sda, b.scl};
  for (unsigned idx : pins) {
    const Pin &p = gPin[idx];
    wr32(p.page.va + kIo, p.io & 2);
    wr32(p.page.va + kCtl, p.ctl);
    same = same && rd32(p.page.va + kCtl) == p.ctl;
  }
  b.muxed = false;
  ++b.restores;
  if (!same && b.restoreMismatches++ < 4)
    note("pins of the %s bus: CTL read back differs after the restore (gpio%u %08x, gpio%u %08x; found %08x, %08x)",
         icName(unsigned(&b - gBus)), gPin[b.sda].gpio, rd32(gPin[b.sda].page.va + kCtl), gPin[b.scl].gpio,
         rd32(gPin[b.scl].page.va + kCtl), gPin[b.sda].ctl, gPin[b.scl].ctl);
}

// The two ICs on the two software buses. write() refuses everything Ktz8866.h does not need, whatever the caller asks.
class KextIcs final : public Ics {
 public:
  bool read(unsigned ic, uint8_t reg, uint8_t *value) override
  {
    if (ic >= kIcs || !gBus[ic].usable) return false;
    Bus &b = gBus[ic];
    if (!b.muxed) muxToGpio(b);
    const I2c r = b.i2c.writeRead(kAddress, &reg, 1, value, 1);
    ++b.reads;
    if (r != I2c::ok) failed(ic, "read", reg, r);
    return r == I2c::ok;
  }
  bool write(unsigned ic, uint8_t reg, uint8_t value) override
  {
    const char *refusal = gArgs.level < 2 ? "level 1 writes nothing"
                        : !writable(reg) ? "not one of 0x04, 0x05, 0x08"
                        : reg == kRegEn && (value & kEnSoftReset) ? "BL_EN with the software reset bit"
                        : reg == kRegBrtMsb && value < (gArgs.floor >> 3) ? "a code under the floor"
                        : nullptr;
    if (refusal || ic >= kIcs) {
      note("REFUSED write %s 0x%02x = 0x%02x: %s", ic < kIcs ? icName(ic) : "?", reg, value, refusal ? refusal : "no such IC");
      return false;
    }
    Bus &b = gBus[ic];
    if (!b.usable) return false;
    if (!b.muxed) muxToGpio(b);
    const uint8_t bytes[2] = {reg, value};
    const I2c r = b.i2c.write(kAddress, bytes, 2);
    ++b.writes;
    if (r != I2c::ok) failed(ic, "write", reg, r);
    return r == I2c::ok;
  }
  void done() override
  {
    for (Bus &b : gBus)
      if (b.muxed) restorePins(b);
  }
  void log(const char *line) override { IOLog("PianoBacklight: %s\n", line); }
  uint64_t nowMs() override { return ::nowMs(); }

 private:
  void failed(unsigned ic, const char *what, uint8_t reg, I2c r)
  {
    ++gBus[ic].errors;
    uint32_t dropped = 0;
    if (limit_.allow(::nowMs(), &dropped))
      note("%s %s 0x%02x: %s (errors on this bus %u)%s", what, icName(ic), reg, i2cName(r), gBus[ic].errors, dropped ? " (some not logged)" : "");
  }
  LogLimit limit_{12, 10000};
};

// Requests from callPlatformFunction and setProperties, taken by the thread.
struct Pending {
  bool any, display, on;
  uint32_t system, api;
  int32_t steps;
  int32_t percent;   // -1: none
  const char *source;
  uint32_t displayCalls, stepCalls;
  bool ambient;              // piano-backlight-auto=1 only: the newest target (permille, -1 leaves automatic mode)
  int32_t target;
  uint32_t lux;
  const char *ambientSource;
  int32_t automatic;         // the property piano-backlight-auto: -1 none, 0 off, 1 on
  uint32_t ambientCalls;
  int32_t sleep;             // setPowerState: -1 none, 1 off (a system sleep), 0 on (the wake)
};
IOLock *gLock;
Pending gPending = {false, false, false, 0, 0, 0, -1, "?", 0, 0, false, -1, 0, "?", -1, 0, -1};
// The thread's answer to a sleep request: the requests taken, and whether the ICs were dark after the last one.
uint32_t gSleepAsked, gSleepTaken;
bool gServing, gDarkForSleep;

}  // namespace

// --------------------------------------------------------------------------------------------------------------------- the service
class PianoBacklight : public IOService {
  OSDeclareDefaultStructors(PianoBacklight)

  KextIcs fIcs;
  Controller fCtl{fIcs};
  IOService *fDisplay = nullptr, *fKeyboard = nullptr, *fSensors = nullptr;   // fSensors: piano-backlight-auto=1 only
  uint64_t fSensorsSeq = ~0ull;   // PianoSensors' piano-sensors-seq as last taken
  int64_t fSensorsTarget = -2;    // its piano-sensors-light-target as last taken (-2: none yet)
  int fAutoPublished = -1, fOffsetPublished = 0x7fffffff;
  uint64_t fNextLookupMs = 0;
  int fDisplayProperty = -1;   // NativeGopDisplay's piano-display-power as last read (-1: none yet)
  bool fDisplayKnown = false, fDisplayOn = true;
  uint32_t fSystem = 0, fApi = 0;

  static void threadMain(void *self, wait_result_t);
  void run();
  bool surveyPins();
  void serve();
  void poll(uint64_t now, bool lidOnly);
  IOService *sibling(const char *className);
  void publishState();

 public:
  using IOService::callPlatformFunction;
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) override;
  IOReturn setProperties(OSObject *properties) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
};

OSDefineMetaClassAndStructors(PianoBacklight, IOService)

IOService *PianoBacklight::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoBacklight::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-backlight=%u (%s); boot level %u %%%s, floor %u, lid %u, keys %u, flags %u, half-bit %u us, swap %u%s",
       gArgs.level, gArgs.level >= 2 ? "control" : "read-only survey", gArgs.percent,
       gArgs.percentGiven ? " (given: set whatever was found)" : " (default: only lowers a higher found level)", gArgs.floor, gArgs.lid,
       gArgs.keys, gArgs.flags, gArgs.halfUs, gArgs.swap,
       gArgs.autoAllowed ? "; automatic brightness allowed (piano-backlight-auto=1: PianoBacklightAmbient, PianoSensors' "
                           "piano-sensors-light-target)" : "");
  setProperty("piano-backlight", gArgs.level, 32);
  Config c;
  c.floor = uint16_t(gArgs.floor);
  c.bootPercent = gArgs.percent;
  c.bootForce = gArgs.percentGiven && gArgs.percent;
  c.lidWatch = gArgs.lid == 1;
  c.readFlags = gArgs.flags == 1;
  c.autoAllowed = gArgs.level >= 2 && gArgs.autoAllowed;
  fCtl.configure(c);
  gLock = IOLockAlloc();
  thread_t thread = nullptr;
  if (!gLock || kernel_thread_start(threadMain, this, &thread) != KERN_SUCCESS) {
    piano_backlight_stage = -1;
    note("FAILED: no lock or no thread; nothing done");
    return true;
  }
  thread_deallocate(thread);
  registerService();   // NativeGopDisplay (piano-backlight>=2) and the key producers find the service by its class
  if (gArgs.level >= 2) note("system sleep: in the power tree (%#x): off clears BL_EN on both ICs, on restores it", pianopower::join(this));
  return true;
}

void PianoBacklight::threadMain(void *self, wait_result_t) { static_cast<PianoBacklight *>(self)->run(); }

bool PianoBacklight::surveyPins()
{
  for (Pin &p : gPin) {
    if (!mapRegion(p.page, kTlmm + uint64_t(p.gpio) * 0x1000, 0x10)) {
      note("pins: gpio%u (%s) does not map", p.gpio, p.what);
      continue;
    }
    p.ctl = rd32(p.page.va + kCtl);
    p.io = rd32(p.page.va + kIo);
    note("pins: gpio%-2u %-40s CTL %08x (function %u, pull %u, drive %u mA, %s%s) IO %08x (pad %u, out %u)", p.gpio, p.what, p.ctl,
         (p.ctl >> 2) & 15, p.ctl & 3, 2 * (((p.ctl >> 6) & 7) + 1), p.ctl & kOe ? "output" : "input",
         p.ctl & kEgpioPresent ? (p.ctl & kEgpioEnable ? ", eGPIO here" : ", eGPIO ELSEWHERE") : "", p.io, p.io & 1, (p.io >> 1) & 1);
  }
  bool any = false;
  for (unsigned i = 0; i < kIcs; ++i) {
    Bus &b = gBus[i];
    if (gArgs.swap & (1u << i)) {
      const unsigned t = b.sda;
      b.sda = b.scl;
      b.scl = t;
    }
    const Pin &sda = gPin[b.sda], &scl = gPin[b.scl];
    const char *why = !sda.page.va || !scl.page.va ? "a pin does not map"
                    : foreign(sda) || foreign(scl) ? "a pin is routed to the other TLMM (eGPIO)"
                    : !(sda.io & scl.io & 1) ? "a line is low as found (bus busy or no pull-up)"
                    : nullptr;
    b.usable = !why;
    b.i2c.configure({&b, lineSda, lineScl, readSda, readScl, delayUs}, gArgs.halfUs, 2000);
    note("bus %s: SDA gpio%u, SCL gpio%u: %s%s", icName(i), sda.gpio, scl.gpio, why ? "NOT used: " : "used (software I2C, pins given back after each burst)",
         why ? why : "");
    any = any || b.usable;
  }
  return any;
}

void PianoBacklight::run()
{
  piano_backlight_stage = 1;
  note("step 1: survey (read only): pins, then both ICs");
  if (!surveyPins()) {
    piano_backlight_stage = -1;
    note("FAILED at step 1: no usable bus; nothing written");
    return;
  }
  const bool both = fCtl.survey();
  const IcState &p = fCtl.ic(0), &q = fCtl.ic(1);
  note("found: primary %s code %u (%u %%) %s, secondary %s code %u (%u %%) %s; %s", p.answered ? "answered," : "SILENT,", p.foundCode,
       percentOf(p.foundCode), p.foundLit ? "lit" : "DARK", q.answered ? "answered," : "SILENT,", q.foundCode, percentOf(q.foundCode),
       q.foundLit ? "lit" : "DARK", both ? "both KTZ8866" : "NOT both KTZ8866");
  setProperty("piano-backlight-found-code", p.foundCode, 32);
  setProperty("piano-backlight-found", both, 32);
  if (gArgs.level < 2) {
    piano_backlight_stage = 3;
    note("level 1: survey done, nothing written (bus accesses: %u + %u reads, %u + %u errors, pins restored %u + %u times)",
         gBus[0].reads, gBus[1].reads, gBus[0].errors, gBus[1].errors, gBus[0].restores, gBus[1].restores);
    return;
  }
  piano_backlight_stage = 2;
  note("step 2: control");
  if (!fCtl.start()) {
    piano_backlight_stage = -2;
    return;
  }
  publishState();
  serve();
}

// The display power (NativeGopDisplay's property, a fallback for a call missed while this service was not yet published) and the
// cover's hall sensor (PianoKeyboard's property): looked up every 2 s until found, read on each poll.
void PianoBacklight::poll(uint64_t now, bool lidOnly)
{
  if ((!fDisplay || !fKeyboard || (gArgs.autoAllowed && !fSensors)) && now >= fNextLookupMs) {
    fNextLookupMs = now + 2000;
    const struct {
      IOService **slot;
      const char *name;
    } wanted[3] = {{&fDisplay, "NativeGopDisplay"}, {&fKeyboard, "PianoKeyboard"}, {&fSensors, "PianoSensors"}};
    for (const auto &w : wanted) {
      IOService **slot = w.slot;
      const char *name = w.name;
      if (*slot || (slot == &fSensors && !gArgs.autoAllowed)) continue;
      OSDictionary *matching = IOService::serviceMatching(name);
      *slot = matching ? IOService::copyMatchingService(matching) : nullptr;
      OSSafeReleaseNULL(matching);
      if (!*slot && slot == &fSensors) *slot = sibling(name);   // PianoSensors (levels 1-3) does not register itself
      if (*slot)
        note("found %s (%s)", name, slot == &fKeyboard ? "its hall sensor property is watched"
             : slot == &fSensors ? "its piano-sensors-light-target is taken when it publishes one" : "its display power property is watched");
    }
  }
  if (fKeyboard) {
    OSNumber *hall = OSDynamicCast(OSNumber, fKeyboard->getProperty("piano-keyboard-hall"));
    if (hall) fCtl.lid(hall->unsigned32BitValue() == 0 ? 0 : hall->unsigned32BitValue() == 1 ? 1 : -1);
  }
  // PianoSensors' light target (its level 4; the property is absent before): a new one when its sequence or the value moved.
  if (fSensors) {
    OSNumber *target = OSDynamicCast(OSNumber, fSensors->getProperty(kSensorsTargetProperty));
    OSNumber *seq = OSDynamicCast(OSNumber, fSensors->getProperty(kSensorsSeqProperty));
    OSNumber *lux = OSDynamicCast(OSNumber, fSensors->getProperty(kSensorsLuxProperty));
    const uint64_t t = target ? target->unsigned64BitValue() : ~0ull, q = seq ? seq->unsigned64BitValue() : 0;
    if (target && t <= 1000 && (int64_t(t) != fSensorsTarget || (seq && q != fSensorsSeq))) {
      fSensorsTarget = int64_t(t);
      fSensorsSeq = q;
      fCtl.ambient(int32_t(t), lux ? lux->unsigned32BitValue() : 0, "PianoSensors");
    }
  }
  if (lidOnly || !fDisplay) return;
  OSBoolean *power = OSDynamicCast(OSBoolean, fDisplay->getProperty(kDisplayPowerProperty));
  const int value = power ? power->isTrue() : -1;
  if (value >= 0 && value != fDisplayProperty && (!fDisplayKnown || bool(value) != fDisplayOn)) {
    note("display power from NativeGopDisplay's property: %s", value ? "on" : "off");
    fDisplayKnown = true;
    fDisplayOn = value;
    fCtl.display(value, fSystem, fApi);
  }
  fDisplayProperty = value;
}

// A service beside this one under IOPlatformExpertDevice, by class, registered or not (copyMatchingService finds registered ones
// only). Retained.
IOService *PianoBacklight::sibling(const char *className)
{
  IOService *provider = getProvider();
  OSIterator *it = provider ? provider->getChildIterator(gIOServicePlane) : nullptr;
  IOService *found = nullptr;
  if (it) {
    while (OSObject *o = it->getNextObject())
      if (o->metaCast(className) && (found = OSDynamicCast(IOService, o))) {
        found->retain();
        break;
      }
    it->release();
  }
  return found;
}

void PianoBacklight::serve()
{
  IOLockLock(gLock);
  gServing = true;
  IOLockUnlock(gLock);
  uint64_t nextPoll = 0, nextSlow = 0, nextAlive = nowMs() + 60000;
  bool lastLit = fCtl.lit(0);
  uint16_t lastCode = fCtl.code();
  for (;;) {
    Pending p;
    IOLockLock(gLock);
    if (!gPending.any) {
      uint64_t deadline = 0;
      clock_interval_to_deadline(250, kMillisecondScale, &deadline);
      IOLockSleepDeadline(gLock, &gPending, deadline, THREAD_UNINT);
    }
    p = gPending;
    gPending.any = gPending.display = gPending.ambient = false;
    gPending.steps = 0;
    gPending.percent = gPending.automatic = gPending.sleep = -1;
    const uint32_t asked = gSleepAsked;
    IOLockUnlock(gLock);

    if (p.sleep >= 0) {
      fCtl.sleep(p.sleep == 1);
      for (unsigned retry = 0; p.sleep == 1 && fCtl.pending() && retry < 2; ++retry) {   // the off must take before the freeze
        IOSleep(20);
        fCtl.retry();
      }
      IOLockLock(gLock);
      gDarkForSleep = !fCtl.lit(0) && !fCtl.lit(1);
      gSleepTaken = asked;
      IOLockWakeup(gLock, &gSleepTaken, false);
      IOLockUnlock(gLock);
    }

    if (p.display) {
      fDisplayKnown = true;
      fDisplayOn = p.on;
      fSystem = p.system;
      fApi = p.api;
      fCtl.display(p.on, p.system, p.api);
    }
    if (p.steps) fCtl.step(p.steps, p.source);
    if (p.percent >= 0) fCtl.percent(uint32_t(p.percent), "setProperties");
    if (p.automatic >= 0) fCtl.automatic(p.automatic == 1, "setProperties");
    if (p.ambient) fCtl.ambient(p.target, p.lux, p.ambientSource);
    const uint64_t now = nowMs();
    if (now >= nextPoll) {
      nextPoll = now + 250;
      const bool slow = now >= nextSlow;
      if (slow) nextSlow = now + 1000;
      poll(now, !slow);
      if (slow) fCtl.retry();
    }
    const pianoauto::Ambient &a = fCtl.ambientState();
    if (fCtl.lit(0) != lastLit || fCtl.code() != lastCode ||
        (gArgs.autoAllowed && (int(a.automatic()) != fAutoPublished || a.offset() != fOffsetPublished))) {
      lastLit = fCtl.lit(0);
      lastCode = fCtl.code();
      publishState();
    }
    if (now >= nextAlive) {
      nextAlive = now + 600000;
      note("alive: %s, code %u (%u %%), %s; %u writes, %u failed accesses; calls: display %u, steps %u", fCtl.controlling() ? "controlling" : "NOT controlling",
           fCtl.code(), percentOf(fCtl.code()), fCtl.lit(0) && fCtl.lit(1) ? "lit" : fCtl.lit(0) || fCtl.lit(1) ? "HALF lit" : "dark",
           fCtl.writes(), fCtl.errors(), p.displayCalls, p.stepCalls);
    }
  }
}

void PianoBacklight::publishState()
{
  setProperty("piano-backlight-code", fCtl.code(), 32);
  setProperty("piano-backlight-percent", percentOf(fCtl.code()), 32);
  setProperty("piano-backlight-lit", fCtl.lit(0) && fCtl.lit(1));
  if (!gArgs.autoAllowed) return;
  const pianoauto::Ambient &a = fCtl.ambientState();
  fAutoPublished = a.automatic();
  fOffsetPublished = a.offset();
  setProperty(pianoauto::kPropertyAuto, a.automatic());
  setProperty(pianoauto::kPropertyOffset, uint64_t(uint32_t(int32_t(a.offset()))), 32);   // signed 32-bit
  if (a.lastTarget() >= 0) {
    setProperty(pianoauto::kPropertyTarget, uint64_t(a.lastTarget()), 32);
    setProperty(pianoauto::kPropertyLux, uint64_t(a.lastLux()), 32);
  }
}

IOReturn PianoBacklight::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4)
{
  const bool display = name && name->isEqualTo(kFunctionDisplay), step = name && name->isEqualTo(kFunctionStep);
  const bool ambient = name && name->isEqualTo(kFunctionAmbient);
  if (!display && !step && !ambient) return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
  if (ambient && (gArgs.level < 2 || !gArgs.autoAllowed)) return kIOReturnUnsupported;
  if (!gLock) return kIOReturnNotReady;
  IOLockLock(gLock);
  if (ambient) {
    const intptr_t t = intptr_t(p1);
    gPending.ambient = true;
    gPending.target = t < 0 ? -1 : t > 1000 ? 1000 : int32_t(t);
    gPending.lux = uint32_t(uintptr_t(p3) > 0xffffffffu ? 0xffffffffu : uintptr_t(p3));
    gPending.ambientSource = p2 ? static_cast<const char *>(p2) : "?";
    ++gPending.ambientCalls;
  } else if (display) {
    gPending.display = true;
    gPending.on = p1 != nullptr;
    gPending.system = uint32_t(uintptr_t(p2));
    gPending.api = uint32_t(uintptr_t(p3));
    ++gPending.displayCalls;
  } else {
    const int32_t delta = int32_t(intptr_t(p1));
    gPending.steps = int32_t(clampArg(uint32_t(gPending.steps + delta + 20), 0, 40)) - 20;
    gPending.source = p2 ? static_cast<const char *>(p2) : "?";
    ++gPending.stepCalls;
  }
  gPending.any = true;
  IOLockWakeup(gLock, &gPending, false);
  IOLockUnlock(gLock);
  return kIOReturnSuccess;
}

// docs/power.md 4: off = the backlight dark before the system freezes (the thread clears BL_EN; this waits for it, at most 800 ms, so
// the answer is synchronous), on = the thread lights it again if the display and the lid want it (asynchronous). Not controlling
// (level 1, control off): nothing.
IOReturn PianoBacklight::setPowerState(unsigned long state, IOService *)
{
  if (!gLock) return pianopower::kAckNow;
  IOLockLock(gLock);
  if (!gServing) {
    IOLockUnlock(gLock);
    return pianopower::kAckNow;
  }
  gPending.sleep = state == pianopower::kOff ? 1 : 0;
  gPending.any = true;
  const uint32_t ask = ++gSleepAsked;
  IOLockWakeup(gLock, &gPending, false);
  bool taken = true, dark = true;
  if (state == pianopower::kOff) {
    uint64_t deadline = 0;
    clock_interval_to_deadline(800, kMillisecondScale, &deadline);
    while (gSleepTaken != ask && IOLockSleepDeadline(gLock, &gSleepTaken, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
    }
    taken = gSleepTaken == ask;
    dark = gDarkForSleep;
  }
  IOLockUnlock(gLock);
  if (state == pianopower::kOff && (!taken || !dark))
    note("system sleep: %s", !taken ? "the thread did not answer within 800 ms (the backlight may stay lit while suspended)"
                                    : "an IC did not take BL_EN off (the backlight may stay lit while suspended)");
  return pianopower::kAckNow;
}

// An administrator's request (IORegistryEntrySetCFProperties): "piano-backlight-percent" = 0..100 of the range, "piano-backlight-step"
// = a signed number of steps. Level 2 only.
IOReturn PianoBacklight::setProperties(OSObject *properties)
{
  OSDictionary *d = OSDynamicCast(OSDictionary, properties);
  if (!d || gArgs.level < 2 || !gLock) return kIOReturnUnsupported;
  if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return kIOReturnNotPrivileged;
  OSNumber *percent = OSDynamicCast(OSNumber, d->getObject("piano-backlight-percent"));
  OSNumber *steps = OSDynamicCast(OSNumber, d->getObject("piano-backlight-step"));
  OSObject *automatic = gArgs.autoAllowed ? d->getObject(pianoauto::kArgAuto) : nullptr;   // OSNumber 0/1 or OSBoolean
  OSNumber *autoNumber = OSDynamicCast(OSNumber, automatic);
  OSBoolean *autoBoolean = OSDynamicCast(OSBoolean, automatic);
  if (!percent && !steps && !autoNumber && !autoBoolean) return kIOReturnBadArgument;
  IOLockLock(gLock);
  if (percent) gPending.percent = int32_t(clampArg(percent->unsigned32BitValue(), 0, 100));
  if (steps) {
    gPending.steps = int32_t(clampArg(uint32_t(gPending.steps + int32_t(steps->unsigned32BitValue()) + 20), 0, 40)) - 20;
    gPending.source = "setProperties";
  }
  if (autoNumber || autoBoolean) gPending.automatic = autoBoolean ? autoBoolean->isTrue() : autoNumber->unsigned32BitValue() ? 1 : 0;
  gPending.any = true;
  IOLockWakeup(gLock, &gPending, false);
  IOLockUnlock(gLock);
  return kIOReturnSuccess;
}
