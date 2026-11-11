// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal B6 in the kernel: the PMIC ADC thread, its console lines and IORegistry properties, and the board temperatures for
// charging safety (package/include/PianoBoardTemps.h). Off unless piano-thermal-adc is set. docs/drivers/thermal.md section 12.
//   piano-thermal-adc=1  first, reads only: the SPMI arbiter, the APID and owner of the ADC's SDAM 0x9000, that SDAM's state, the
//                        five thermistor pins (high-impedance?). Then every piano-thermal-adc-s (default piano-thermal-log-s, 30) a
//                        round: one conversion of each of the eleven board thermistors (AdcChannels.h, PmicAdc.h), the console
//                        line "[Ns] board: batt 31.2 C, charger 33.0 C, ...", the properties BoardTemperaturesDeciC, BoardAdc and
//                        BoardAdcState, and the snapshot piano_thermal_board_temps() answers from. The first round logs raw codes.
//                        The charger's reading counts only while its pin, the firmware's PMIH0108 GPIO 12, reads high-impedance
//                        (read before each of its conversions; a line at each change of it: AdcChannels.h kPinGates).
//   piano-thermal-adc=2  the same, the raw codes every round.
// Writes: PmicAdc.h's allowlist (SDAM 0's conversion bytes) through the read/write channel page of that SDAM's APID, mapped only
// after the owner table gave that APID to EE 0. No other PMIC register and no arbiter core/cnfg register is ever stored to.
// Its own thread ("piano-thermal-adc"): a slow conversion (up to 600 ms) never delays the TSENS polling (10 ms near a trip).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "BoardTemps.h"
#include "PianoThermalAdc.h"
#include "PmicAdc.h"
#include "Tsens.h"

using namespace pianothermal;
using namespace pianothermal::adc;

namespace {

uint64_t nowNs()
{
  uint64_t ns;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns;
}
uint64_t gStartNs;
uint32_t upS() { return uint32_t((nowNs() - gStartNs) / 1000000000ull); }

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[480];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoThermal: %s\n", line);
}

int catf(char *line, size_t size, int at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
int catf(char *line, size_t size, int at, const char *fmt, ...)
{
  if (at < 0 || size_t(at) + 1 >= size) return at;
  va_list ap;
  va_start(ap, fmt);
  const int wanted = vsnprintf(line + at, size - size_t(at), fmt, ap);
  va_end(ap);
  if (wanted < 0) return at;
  return size_t(at) + size_t(wanted) >= size ? int(size - 1) : at + wanted;
}

// milli-C as "31.2" (rounded to the tenth)
struct Tenths {
  char s[16];
  explicit Tenths(int32_t milli) { deciText(milli >= 0 ? (milli + 50) / 100 : -((-milli + 50) / 100), s, sizeof s); }
};

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}

// Explicit ldr/str, as the other piano kexts do.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(a) : "memory"); }

// ------------------------------------------------------------------------------------------------------------- the windows
struct Window {
  IOMemoryMap *map;
  uintptr_t va;
  uint32_t bytes;
};
Window gWin[4];   // by Win: core, cnfg, obsv (EE 0's observer channels), chnl (the ADC APID's read/write channel page)

bool mapWindow(Window &w, uint64_t pa, uint32_t bytes)
{
  IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return false;
  w = {map, uintptr_t(map->getVirtualAddress()), bytes};
  return true;
}

uint32_t ioRd(void *, Win w, uint32_t off)
{
  const Window &x = gWin[unsigned(w)];
  if (!x.va || (off & 3) || off + 4 > x.bytes) return 0;   // unmapped reads as "not done" / "not v7": every wait ends
  return rd32(x.va + off);
}
void ioWr(void *, Win w, uint32_t off, uint32_t v)
{
  if (w != Win::obsv && w != Win::chnl) return;   // the arbiter's core and owner table are never written
  const Window &x = gWin[unsigned(w)];
  if (!x.va || (off & 3) || off + 4 > x.bytes) return;
  wr32(x.va + off, v);
  __builtin_arm_dsb(0xf);
}
void ioDelay(void *, uint32_t us)
{
  if (us >= 1000) IOSleep(us / 1000);
  else IODelay(us);
}
uint64_t ioNow(void *) { return nowNs() / 1000; }
const Io kIo = {nullptr, ioRd, ioWr, ioDelay, ioNow};

// ------------------------------------------------------------------------------------------------------------- state
struct Hook {
  IOService *svc;
  uint32_t level, periodS, delayS;
};
Hook gHook;
Board gBoard;
const char *gOffWhy;   // why the ADC never started (before find(): the windows), else null

// The snapshot piano_thermal_board_temps() reads: a sequence count, odd while the ADC thread rewrites it.
Snapshot gSnap;
uint32_t gSeq;

template <typename T> inline void st(T &dst, T v) { __atomic_store_n(&dst, v, __ATOMIC_RELAXED); }
template <typename T> inline T ld(const T &src) { return __atomic_load_n(&src, __ATOMIC_RELAXED); }

void publishSnapshot(const Snapshot &s)
{
  __atomic_fetch_add(&gSeq, 1u, __ATOMIC_ACQ_REL);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  st(gSnap.state, s.state);
  st(gSnap.rounds, s.rounds);
  st(gSnap.periodMs, s.periodMs);
  st(gSnap.validMask, s.validMask);
  st(gSnap.disabledMask, s.disabledMask);
  for (unsigned i = 0; i < kChannelCount; ++i) {
    st(gSnap.milliC[i], s.milliC[i]);
    st(gSnap.atUs[i], s.atUs[i]);
  }
  __atomic_fetch_add(&gSeq, 1u, __ATOMIC_RELEASE);
}

bool readSnapshot(Snapshot &s)
{
  for (unsigned tries = 0; tries < 1000; ++tries) {
    const uint32_t a = __atomic_load_n(&gSeq, __ATOMIC_ACQUIRE);
    if (a & 1) continue;
    s.state = ld(gSnap.state);
    s.rounds = ld(gSnap.rounds);
    s.periodMs = ld(gSnap.periodMs);
    s.validMask = ld(gSnap.validMask);
    s.disabledMask = ld(gSnap.disabledMask);
    for (unsigned i = 0; i < kChannelCount; ++i) {
      s.milliC[i] = ld(gSnap.milliC[i]);
      s.atUs[i] = ld(gSnap.atUs[i]);
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&gSeq, __ATOMIC_RELAXED) == a) return true;
  }
  return false;
}

const char *stateName(uint32_t s)
{
  return s == PIANO_BOARD_TEMPS_RUNNING ? "running" : s == PIANO_BOARD_TEMPS_STARTING ? "starting"
       : s == PIANO_BOARD_TEMPS_STOPPED ? "stopped" : "off";
}

// ------------------------------------------------------------------------------------------------------------- lines
void boardLine()
{
  char line[480];
  int at = snprintf(line, sizeof line, "[%us] board:", upS());
  for (unsigned i = 0; i < kChannelCount; ++i) {
    const ChannelState &c = gBoard.ch[i];
    if (c.valid) at = catf(line, sizeof line, at, "%s %s %s C", i ? "," : "", kChannels[i].name, Tenths(c.last.milliC).s);
    else if (c.enabled && c.pinHeld) {   // a reading of the pin's own load, not the board's: not shown as a temperature
      const int g = gatePin(i);
      at = catf(line, sizeof line, at, "%s %s - (gpio%u)", i ? "," : "", kChannels[i].name, g >= 0 ? kThermPins[g].gpio : 0u);
    } else at = catf(line, sizeof line, at, "%s %s off", i ? "," : "", kChannels[i].name);
  }
  note("%s", line);
}

void rawLine()
{
  char line[480];
  int at = snprintf(line, sizeof line, "[%us] adc raw (code, kOhm):", upS());
  for (unsigned i = 0; i < kChannelCount; ++i) {
    const ChannelState &c = gBoard.ch[i];
    if (c.valid || (c.enabled && c.pinHeld))
      at = catf(line, sizeof line, at, " %s %#x %llu.%llu%s", kChannels[i].name, c.last.code, (unsigned long long)(c.last.ohms / 1000),
                (unsigned long long)(c.last.ohms % 1000 / 100), c.valid ? "" : "(pin)");
    else at = catf(line, sizeof line, at, " %s %#x(off)", kChannels[i].name, c.last.code);
  }
  note("%s", line);
}

// A pin's state as "high-Z (type 0x10, mode 0, pull 0x5, en 0)" (or its read error).
int pinText(char *line, size_t size, int at, const PinState &p)
{
  if (p.err != Err::none) return catf(line, size, at, "not readable (%s)", errName(p.err));
  return catf(line, size, at, "%s (type %#x, mode %#x, pull %#x, en %#x)", p.highZ() ? "high-Z" : "ENABLED", p.type, p.mode, p.pull, p.en);
}

// The firmware's pin under a channel, when the round found it changed (the first round always): whether that channel counts now.
void pinLines()
{
  for (unsigned i = 0; i < kChannelCount; ++i) {
    const ChannelState &c = gBoard.ch[i];
    const int g = gatePin(i);
    if (g < 0 || !c.pinRead || !c.pinChanged) continue;
    char line[480];
    int at = snprintf(line, sizeof line, "[%us] adc: %s: %s gpio%u (the firmware's) ", upS(), kChannels[i].name, kThermPins[g].pmic,
                      kThermPins[g].gpio);
    at = pinText(line, sizeof line, at, c.pin);
    catf(line, sizeof line, at, "%s", c.pin.clear() ? ": its thermistor counts"
         : c.pin.err != Err::none ? ": its thermistor is not valid while the pin does not read"
         : ": its thermistor reads the pin's load, not the board: not valid (the DSP's charger firmware takes the pin)");
    note("%s", line);
  }
}

void eventLines()
{
  const Adc &a = gBoard.adc;
  for (unsigned k = 0; k < gBoard.eventCount; ++k) {
    const Board::Event &e = gBoard.events[k];
    const ChannelDesc &d = kChannels[e.channel];
    if (isGlobal(e.err))
      note("[%us] adc: STOPPED for this boot at %s: %s (HS %#x, CONV_REQ %#x, last SPMI status %#x); no more conversions, the board "
           "temperatures are unknown from now",
           upS(), d.name, errName(e.err), a.lastHs, a.lastConvReq, a.bus.lastStatus);
    else
      note("[%us] adc: %s (%#x, %s) DISABLED for this boot: %s (code %#x, STATUS1 %#x, EOC_STS %#x)", upS(), d.name, d.reg, d.label,
           errName(e.err), gBoard.ch[e.channel].last.code, a.lastStatus1, a.lastEoc);
  }
}

void statsLine()
{
  const Adc &a = gBoard.adc;
  note("[%us] adc: round %u: %u conversions, %u failed, %llu us each on average, %u us at most; %u of %u channels on; %u stale "
       "status clears; %u SPMI reads, %u writes",
       upS(), gBoard.rounds, a.conversions, a.failures, (unsigned long long)(a.conversions ? a.totalUs / a.conversions : 0), a.maxUs,
       gBoard.enabledCount(), kChannelCount, a.staleClears, a.bus.reads, a.bus.writeCount);
}

void publishProperties(uint32_t state)
{
  IOService *svc = gHook.svc;
  OSDictionary *temps = OSDictionary::withCapacity(kChannelCount), *status = OSDictionary::withCapacity(kChannelCount);
  if (temps && status) {
    for (unsigned i = 0; i < kChannelCount; ++i) {
      const ChannelState &c = gBoard.ch[i];
      if (c.valid) {
        const int32_t m = c.last.milliC, deci = m >= 0 ? (m + 50) / 100 : -((-m + 50) / 100);
        if (OSNumber *n = OSNumber::withNumber(uint64_t(int64_t(deci)), 32)) {
          temps->setObject(kChannels[i].name, n);
          n->release();
        }
      }
      char why[96];
      if (c.enabled && c.pinHeld) snprintf(why, sizeof why, "held: its pin (the firmware's) is not high-impedance");
      else if (c.enabled) snprintf(why, sizeof why, "%s", c.valid ? "ok" : "no reading yet");
      else snprintf(why, sizeof why, "off: %s", errName(c.err));
      if (OSString *s = OSString::withCString(why)) {
        status->setObject(kChannels[i].name, s);
        s->release();
      }
    }
    svc->setProperty("BoardTemperaturesDeciC", temps);
    svc->setProperty("BoardAdc", status);
  }
  if (temps) temps->release();
  if (status) status->release();
  char s[96];
  if (state == PIANO_BOARD_TEMPS_STOPPED) snprintf(s, sizeof s, "stopped: %s", gOffWhy ? gOffWhy : errName(gBoard.adc.stopped));
  else snprintf(s, sizeof s, "%s", stateName(state));
  svc->setProperty("BoardAdcState", s);
}

// ------------------------------------------------------------------------------------------------------------- start
// Reads only, up to enableWrites(): the windows, the arbiter, the APID and its owner; then the read/write channel page of that one
// APID; then the SDAM's state, the pins and the channel list.
bool attach()
{
  if (!mapWindow(gWin[unsigned(Win::core)], kSpmiCore, kCoreBytes) || !mapWindow(gWin[unsigned(Win::cnfg)], kSpmiCnfg, kCnfgBytes) ||
      !mapWindow(gWin[unsigned(Win::obsv)], kSpmiObsv, kObsvBytes)) {
    gOffWhy = "the SPMI arbiter windows do not map";
    gBoard.adc.stopped = Err::notArbiterV7;
    note("adc: OFF: %s; nothing written", gOffWhy);
    return false;
  }
  Adc &a = gBoard.adc;
  const Err e = a.find(kIo);
  note("adc: spmi arbiter version %#x, %u APIDs%s; SDAM 0x9000 (PPID %#x): APID %d (%u map entries), owner EE %u%s", a.arbiter.version,
       a.arbiter.count, a.arbiter.v7 ? "" : " (NOT v7)", kAdcPpid, a.where.apid, a.where.entries, a.where.owner,
       a.where.apid >= 0 && a.where.owner == 0 ? " (ours)" : "");
  if (e != Err::none) {
    note("adc: OFF: %s; nothing written", errName(e));
    return false;
  }
  if (!mapWindow(gWin[unsigned(Win::chnl)], kSpmiChnls + uint64_t(kChnlStride) * uint32_t(a.where.apid), kChnlBytes)) {
    gOffWhy = "the ADC APID's read/write channel does not map";
    a.stopped = Err::refused;
    note("adc: OFF: %s; nothing written", gOffWhy);
    return false;
  }
  a.enableWrites();
  Survey s;
  const Err se = a.survey(s);
  if (se != Err::none) {
    note("adc: OFF: SDAM 0x9000 does not read: %s (SPMI status %#x); nothing written", errName(se), a.bus.lastStatus);
    return false;
  }
  note("adc: SDAM 0x9000: type %#x subtype %#x, size register %#x; HS %#x, STATUS1 %#x, EOC_STS %#x, CONV_REQ %#x; command bytes "
       "%02x %02x %02x %02x %02x %02x %02x%s",
       s.type, s.subtype, s.size, s.hs, s.status1, s.eoc, s.convReq, s.command[0], s.command[1], s.command[2], s.command[3],
       s.command[4], s.command[5], s.command[6],
       s.hs == kHsReady && !s.convReq ? " (idle)" : " (BUSY or not an ADC SDAM: the first conversion waits up to 20 ms)");
  // The DT's pins on one line, the firmware's on its own (the console cuts a line at 255 characters: before 2026-11-12 GPIO 12's
  // state never showed). The firmware's pin is read again before each conversion on it (pinLines).
  char line[480];
  int at = snprintf(line, sizeof line, "adc: thermistor pins (DT):");
  unsigned driven = 0;
  for (unsigned i = 0; i < kThermPinCount; ++i) {
    if (!kThermPins[i].inDt) continue;
    const PinState p = a.readPin(kThermPins[i]);
    if (p.err != Err::none) at = catf(line, sizeof line, at, " %s gpio%u %s;", kThermPins[i].pmic, kThermPins[i].gpio, errName(p.err));
    else {
      driven += !p.highZ();
      at = catf(line, sizeof line, at, " %s gpio%u %s (mode %#x, en %#x);", kThermPins[i].pmic, kThermPins[i].gpio,
                p.highZ() ? "high-Z" : "ENABLED", p.mode, p.en);
    }
  }
  if (driven) catf(line, sizeof line, at, " %u not high-Z (not changed)", driven);
  note("%s", line);
  for (unsigned i = 0; i < kThermPinCount; ++i) {
    if (kThermPins[i].inDt) continue;
    at = snprintf(line, sizeof line, "adc: thermistor pin (the firmware's): %s gpio%u ", kThermPins[i].pmic, kThermPins[i].gpio);
    at = pinText(line, sizeof line, at, a.readPin(kThermPins[i]));
    catf(line, sizeof line, at, "; a reading on it counts only while it reads high-Z");
    note("%s", line);
  }
  at = snprintf(line, sizeof line, "adc: channels (DT reg, ADC):");
  for (unsigned i = 0; i < kChannelCount; ++i)
    at = catf(line, sizeof line, at, "%s %s %#x %s", i ? "," : "", kChannels[i].name, kChannels[i].reg,
              kChannels[i].gen == Gen::gen4 ? "g4" : "g3");
  catf(line, sizeof line, at, "; Gen3 decimation 1360, settle 200 us; Gen4 340, 15 us; ratiometric, 100 kOhm pull-up");
  note("%s", line);
  return true;
}

void adcThread(void *, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-thermal-adc");
  if (gHook.delayS) IOSleep(gHook.delayS * 1000);
  const uint32_t periodMs = gHook.periodS * 1000;
  const bool ok = attach();
  Snapshot first = snapshotOf(gBoard, periodMs);
  publishSnapshot(first);
  publishProperties(first.state);
  if (ok) note("adc: running: a round every %u s", gHook.periodS);
  for (;;) {
    if (!ok || gBoard.adc.stopped != Err::none || !gBoard.enabledCount()) {
      IOSleep(3600 * 1000);   // nothing left to do
      continue;
    }
    const uint64_t t0 = nowNs();
    gBoard.round();
    pinLines();
    boardLine();
    if (gBoard.rounds == 1 || gHook.level >= 2) rawLine();
    eventLines();
    if (gBoard.rounds == 1 || gBoard.rounds % 10 == 0 || gBoard.eventCount) statsLine();
    const Snapshot s = snapshotOf(gBoard, periodMs);
    publishSnapshot(s);
    publishProperties(s.state);
    const uint64_t tookMs = (nowNs() - t0) / 1000000ull;
    IOSleep(tookMs < periodMs ? uint32_t(periodMs - tookMs) : 1);
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------- the hook
namespace pianothermal::adc {

uint32_t hookLevel()
{
  const uint32_t v = bootArg("piano-thermal-adc", 0);
  return v > 2 ? 2 : v;
}

void hookStart(IOService *svc, uint32_t level, uint32_t periodS, uint32_t delayS)
{
  gStartNs = nowNs();
  svc->setProperty("piano-thermal-adc", level, 32);
  if (!level) return;
  uint32_t p = bootArg("piano-thermal-adc-s", periodS);
  p = p < 5 ? 5 : p > 3600 ? 3600 : p;
  svc->retain();   // held for the boot by the thread
  gHook = {svc, level, p, delayS};
  Snapshot starting;
  starting.state = PIANO_BOARD_TEMPS_STARTING;
  starting.periodMs = p * 1000;
  publishSnapshot(starting);
  note("adc: piano-thermal-adc=%u: the PMIC ADC5 gen3 (PMK8550 SDAM 0x9000, PBS) for the %u board thermistors, a round every %u s%s; "
       "writes: the SDAM's conversion bytes only, on its own APID once that is EE 0's",
       level, kChannelCount, p, level >= 2 ? ", raw codes every round" : "");
  thread_t t = nullptr;
  if (kernel_thread_start(&adcThread, nullptr, &t) != KERN_SUCCESS) {
    note("adc: FAILED: no thread; nothing converted");
    return;
  }
  thread_deallocate(t);
}

bool hookBoardTemps(void *param1) { return piano_thermal_board_temps(static_cast<piano_board_temps *>(param1)) >= 0; }

}  // namespace pianothermal::adc

// The board temperatures for charging safety (package/include/PianoBoardTemps.h): the last round's, lock-free.
extern "C" int piano_thermal_board_temps(struct piano_board_temps *t)
{
  if (!t || t->version != PIANO_BOARD_TEMPS_VERSION) return -1;
  Snapshot s;
  if (!readSnapshot(s)) {   // the thread rewrote it a thousand times in a row: answer "nothing valid" rather than wait
    s = Snapshot();
    s.state = PIANO_BOARD_TEMPS_STARTING;
  }
  return fillBoardTemps(s, nowNs() / 1000, t);
}
