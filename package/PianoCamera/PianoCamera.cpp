// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera: piano's two cameras (rear Samsung S5KJN1 on CCI0, front OmniVision OV32D40 on QUP2 SE1's pins; SM8750 CAMSS) for
// macOS 27, native at EL1. Design, the level ladder, facts, test plan: docs/drivers/camera.md. Facts and sources: CameraHw.h.
//   piano-camera=1  a read-only survey, logged as "PianoCamera: ..." lines (CameraSurvey.h): the GCC camera branches, CAMCC's
//                   GDSCs/branches/clock sources/PLLs, the MCLK controller, CCI0 and the CSID/TFE versions (only for a powered and
//                   clocked block), QUP2 SE1's GENI words (only while clocked), the ten camera pins, the eight LDOs over the SPMI
//                   observer and the five voted ones' cmd-db addresses, the SMMU groups of stream 0x1c00; a summary and level 2's
//                   verdict per sensor. Every MMIO access is a load; the only store is the SPMI observer's read command.
//   piano-camera=2  the survey, then each sensor in turn (CameraPower.h): its bus, its rails (RPMh under PianoA830's DRV-2 lock,
//                   read back over SPMI), its supply enable and reset pins, its MCLK from the XO, the chip ID read (0x38e1 / 0x5632),
//                   and everything taken back down in reverse order, exactly what was changed. No sensor register is written.
// Without piano-camera the driver declines in probe() and maps nothing. Levels 3-5 (raw frames, the software ISP, the macOS camera
// device) are designed in camera.md, not built.
// Other arguments: piano-camera-sensor=1 rear | 2 front | 3 both (default 3, rear first), piano-camera-rear-bus=1 (software I2C on
// gpio115/116 instead of CCI0), piano-camera-mclk24=1 (the rear's MCLK at 24 MHz from the MCLK PLL), piano-camera-cci-khz=100|400,
// piano-camera-i2c-half-us=N (2..50, default 5), piano-camera-pull=1, piano-camera-blind=1, piano-camera-hold=MS (0..10000),
// piano-camera-stop=STEP, piano-camera-cycles=N (1..20), piano-camera-delay=S; the survey's parts: piano-camera-cc=0,
// piano-camera-mclkcc=0, piano-camera-blocks=0, piano-camera-qup=0, piano-camera-spmi=0, piano-camera-smmu=0.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoPowerContract.h>
#include "CameraPower.h"
#include "CameraRpmh.h"
#include "CameraSurvey.h"
#include "SpmiObserver.h"

using namespace pianocamera;
using pianopower::Window;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage or step reached, negative when it failed there.
volatile int32_t piano_camera_stage = 0;
// PianoA830's lock words (Sm8750Rpmh.h, Sm8750GpuPower.c): PianoA830 is listed before this kext and named as a library in
// PianoCamera.json, so the KC link resolves both to its one copy (as for PianoWifi, PianoTouch, PianoBluetooth).
extern volatile uint32_t sm8750_gcc_vote_owner;
extern volatile uint32_t sm8750_rpmh_drv2_owner;
}

namespace {

constexpr uint32_t kTag = 0x43414d31u;   // "CAM1" in the lock words

struct Args {
  uint32_t level, sensor, rearBus, mclk24, cciKhz, halfUs, pull, blind, hold, stop, cycles, delay;
  uint32_t cc, mclkcc, blocks, qup, spmi, smmu;
};
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}
uint32_t clampArg(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void readArgs()
{
  gArgs.level = bootArg("piano-camera", 0);
  gArgs.sensor = clampArg(bootArg("piano-camera-sensor", 3), 1, 3);
  gArgs.rearBus = bootArg("piano-camera-rear-bus", 0) ? 1 : 0;
  gArgs.mclk24 = bootArg("piano-camera-mclk24", 0) ? 1 : 0;
  gArgs.cciKhz = bootArg("piano-camera-cci-khz", 400) == 100 ? 100 : 400;
  gArgs.halfUs = clampArg(bootArg("piano-camera-i2c-half-us", 5), 2, 50);
  gArgs.pull = bootArg("piano-camera-pull", 0) ? 1 : 0;
  gArgs.blind = bootArg("piano-camera-blind", 0) ? 1 : 0;
  gArgs.hold = clampArg(bootArg("piano-camera-hold", 0), 0, 10000);
  gArgs.stop = bootArg("piano-camera-stop", 0);
  gArgs.cycles = clampArg(bootArg("piano-camera-cycles", 1), 1, 20);
  gArgs.delay = clampArg(bootArg("piano-camera-delay", 0), 0, 600);
  gArgs.cc = bootArg("piano-camera-cc", 1);
  gArgs.mclkcc = bootArg("piano-camera-mclkcc", 1);
  gArgs.blocks = bootArg("piano-camera-blocks", 1);
  gArgs.qup = bootArg("piano-camera-qup", 1);
  gArgs.spmi = bootArg("piano-camera-spmi", 1);
  gArgs.smmu = bootArg("piano-camera-smmu", 1);
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoCamera: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_camera_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }
inline void dsb() { __asm__ volatile("dsb sy" ::: "memory"); }

// ------------------------------------------------------------------------------------------------ MMIO pages, mapped on demand
struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
constexpr unsigned kMaxPages = 64;
Page gPages[kMaxPages];
unsigned gPageCount, gMapFailures;
uint64_t gLoads, gStores, gQuietStores;

uintptr_t pageVa(uint64_t pa)
{
  const uint64_t base = pa & ~0xfffull;
  for (unsigned i = 0; i < gPageCount; ++i)
    if (gPages[i].pa == base) return gPages[i].va;
  if (gPageCount == kMaxPages) return 0;
  IODeviceMemory *m = IODeviceMemory::withRange(base, 0x1000);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return 0;
  gPages[gPageCount] = {base, map, uintptr_t(map->getVirtualAddress())};
  return gPages[gPageCount++].va;
}
uint32_t load(uint64_t pa)
{
  const uintptr_t va = pageVa(pa);
  if (!va) {
    ++gMapFailures;
    return 0xffffffffu;
  }
  ++gLoads;
  return rd32(va + uintptr_t(pa & 0xfff));
}
void store(uint64_t pa, uint32_t v)
{
  const uintptr_t va = pageVa(pa);
  if (!va) {
    ++gMapFailures;
    return;
  }
  wr32(va + uintptr_t(pa & 0xfff), v);
  dsb();
}
void unmapAll()
{
  for (unsigned i = 0; i < gPageCount; ++i) gPages[i].map->release();
  gPageCount = 0;
}
uint32_t loaderRd(void *, uint64_t pa) { return load(pa); }
const Loader gLoader = {nullptr, loaderRd};

// ------------------------------------------------------------------------------------------------------------------- SPMI
struct Region {
  IOMemoryMap *map;
  uintptr_t va;
};
Region gCore, gCnfg, gObsv;
unsigned gSpmiReads;

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
void unmapRegion(Region &r)
{
  if (r.map) r.map->release();
  r = {};
}
uintptr_t windowBase(Window w) { return w == Window::core ? gCore.va : w == Window::cnfg ? gCnfg.va : gObsv.va; }
uint32_t ioRd(void *, Window w, uint32_t offset) { return rd32(windowBase(w) + offset); }
// The observer channel's command word: the read request itself (SpmiObserver.h). No PMIC register is written this way.
void ioWr(void *, Window w, uint32_t offset, uint32_t value)
{
  ++gSpmiReads;
  wr32(windowBase(w) + offset, value);
}
void ioDelay(void *, unsigned us) { IODelay(us); }
const pianopower::Io gIo = {nullptr, ioRd, ioWr, ioDelay};
pianopower::Arbiter gArbiter;
bool gSpmiTried, gSpmiOk;

bool mapSpmi()
{
  using namespace pianopower;
  if (gSpmiTried) return gSpmiOk;
  gSpmiTried = true;
  if (!mapRegion(gCore, kSpmiCore, kSpmiCoreBytes) || !mapRegion(gCnfg, kSpmiCnfg, kSpmiCnfgBytes) ||
      !mapRegion(gObsv, kSpmiObsv, kSpmiObsvBytes)) {
    note("spmi: the arbiter windows do not map: no PMIC is read");
    return false;
  }
  gArbiter = spmiArbiter(gIo);
  note("spmi: arbiter %#x, %u APIDs%s", gArbiter.version, gArbiter.count, gArbiter.v7 ? "" : ": NOT the v7 layout, no PMIC is read");
  gSpmiOk = gArbiter.v7;
  return gSpmiOk;
}
int pmicApid(void *, uint16_t ppid, int *apid, uint32_t *owner)
{
  if (!mapSpmi()) return -19;
  const pianopower::Apid a = pianopower::spmiFindApid(gIo, gArbiter, ppid);
  *apid = a.apid;
  *owner = a.owner;
  return a.apid < 0 ? -19 : 0;
}
int pmicRead(void *, int apid, uint8_t reg, uint8_t *value, unsigned n)
{
  if (!mapSpmi()) return -19;
  return pianopower::spmiRead(gIo, apid, reg, value, n) == pianopower::ReadResult::ok ? 0 : -5;
}

// ---------------------------------------------------------------------------------------------------------------- cmd-db
uint8_t *gCmdDb;   // a copy, read once (IOMallocData), freed at the end of the run
int cmdDbLookup(void *, const char *name, uint32_t *address)
{
  if (!gCmdDb) {
    uint8_t *copy = static_cast<uint8_t *>(IOMallocData(kCmdDbBytes));
    IODeviceMemory *m = copy ? IODeviceMemory::withRange(kCmdDb, kCmdDbBytes) : nullptr;
    IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (m) m->release();
    if (!map) {
      if (copy) IOFreeData(copy, kCmdDbBytes);
      return -12;
    }
    const uintptr_t va = uintptr_t(map->getVirtualAddress());
    for (uint32_t i = 0; i < kCmdDbBytes; i += 4) {
      const uint32_t w = rd32(va + i);
      memcpy(copy + i, &w, 4);
    }
    map->release();
    gCmdDb = copy;
  }
  return pianokeyboard::cmdDbFind(gCmdDb, kCmdDbBytes, name, address);
}
void cmdDbFree()
{
  if (gCmdDb) IOFreeData(gCmdDb, kCmdDbBytes);
  gCmdDb = nullptr;
}
const Pmic gPmic = {nullptr, pmicApid, pmicRead, cmdDbLookup};

// ---------------------------------------------------------------------------------------------------------- the lock words
bool lockAcquire(volatile uint32_t *word, uint32_t *waits, unsigned steps)
{
  for (uint32_t n = 0;; ++n) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(word, &expected, kTag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      *waits = n;
      return true;
    }
    if (n >= steps) {
      *waits = n;
      return false;
    }
    IODelay(10);
  }
}
void lockRelease(volatile uint32_t *word)
{
  uint32_t expected = kTag;
  __atomic_compare_exchange_n(word, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

// --------------------------------------------------------------------------------------------------------------- RPMh DRV-2
IOMemoryMap *gRscMap;
uintptr_t gRscVa;
Rsc gRsc;
uint32_t rscRd(void *, uint32_t off) { return rd32(gRscVa + kRscTcsBlock + off); }
void rscWr(void *, uint32_t off, uint32_t v)
{
  wr32(gRscVa + kRscTcsBlock + off, v);
  dsb();
}
void rscSleep(void *, unsigned ms) { IOSleep(ms); }
const RscIo gRscIo = {nullptr, rscRd, rscWr, rscSleep};
bool mapRsc()
{
  if (gRscVa) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(kRscDrv2, kRscDrv2Bytes);
  gRscMap = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!gRscMap) {
    note("rsc: cannot map apps DRV-2 (%#llx)", (unsigned long long)kRscDrv2);
    return false;
  }
  gRscVa = uintptr_t(gRscMap->getVirtualAddress());
  const uint32_t id = rd32(gRscVa);
  gRsc.layout = pianokeyboard::rscLayout(id);
  note("rsc: apps DRV-2 id %#x (version %u.%u), TCS block +%#x", id, (id >> 16) & 0xff, (id >> 8) & 0xff, kRscTcsBlock);
  return true;
}

// --------------------------------------------------------------------------------------------------------------- the report
const char *onOff(bool on) { return on ? "on" : "off"; }
const char *branchWord(uint32_t v) { return branchRunning(v) ? (branchEnabled(v) ? "on" : "on (not ours: voted/hw)") : branchEnabled(v) ? "ENABLED but halted" : "off"; }

void reportSurvey(const Survey &s)
{
  const GccState &g = s.gcc;
  note("gcc: camera ahb %08x (%s) xo %08x (%s) mclk-cc ahb %08x (%s) hf-axi %08x (%s) sf-axi %08x (%s); qmip %08x %08x %08x",
       g.cameraAhb, branchWord(g.cameraAhb), g.cameraXo, branchWord(g.cameraXo), g.mclkAhb, branchWord(g.mclkAhb), g.hfAxi,
       branchWord(g.hfAxi), g.sfAxi, branchWord(g.sfAxi), g.qmip[0], g.qmip[1], g.qmip[2]);
  note("gcc: QUP2 vote %08x (s1 %u); m-ahb %08x s-ahb %08x core %08x core2x %08x s1 %08x; s1 rcg cmd %08x cfg %08x (source %u): SE1 %s",
       g.qupVote, (g.qupVote & kVoteS1) != 0, g.qup[0], g.qup[1], g.qup[2], g.qup[3], g.qup[4], g.s1RcgCmd, g.s1RcgCfg,
       rcgSource(g.s1RcgCfg), g.qupSe1Clocked ? "clocked" : "not clocked");
  const CcState &c = s.cc;
  if (!c.read) {
    note("camcc: not read (%s)", c.why);
  } else {
    char line[360];
    int n = 0;
    for (unsigned i = 0; i < kCcGdscCount && n < int(sizeof line) - 40; ++i)
      n += snprintf(line + n, sizeof line - n, "%s%s %08x/%08x (%s)", i ? ", " : "", kCcGdscs[i].name, c.gdsc[i].gdscr, c.gdsc[i].cfg,
                    gdscOn(c.gdsc[i].gdscr) ? "ON" : "off");
    note("camcc gdsc: %s", line);
    n = 0;
    for (unsigned i = 0; i < kCcBranchCount && n < int(sizeof line) - 32; ++i)
      n += snprintf(line + n, sizeof line - n, "%s%s %08x", i ? " " : "", kCcBranches[i].name, c.branch[i]);
    note("camcc branches: %s", line);
    n = 0;
    for (unsigned i = 0; i < kCcRcgCount && n < int(sizeof line) - 48; ++i)
      n += snprintf(line + n, sizeof line - n, "%s%s %08x/%08x src %u%s", i ? ", " : "", kCcRcgs[i].name, c.rcg[i].cmd, c.rcg[i].cfg,
                    rcgSource(c.rcg[i].cfg), c.rcg[i].rootOff ? " root-off" : "");
    note("camcc rcgs: %s", line);
    n = 0;
    for (unsigned p = 0; p < kCcPllCount && n < int(sizeof line) - 48; ++p)
      n += snprintf(line + n, sizeof line - n, "%spll%u mode %08x op %x L %#x (%s)", p ? ", " : "", p, c.pllMode[p], c.pllOpmode[p],
                    c.pllL[p], pllRunning(c.pllMode[p], c.pllOpmode[p]) ? "RUNNING" : pllLocked(c.pllMode[p]) ? "locked" : "off");
    note("camcc plls: %s", line);
  }
  const MclkState &m = s.mclk;
  if (!m.read) {
    note("mclk-cc: not read (%s)", m.why);
  } else {
    note("mclk-cc: pll0 mode %08x op %x L %#x user %08x (%s); sleep branch %08x", m.pllMode, m.pllOpmode, m.pllL, m.pllUser,
         pllRunning(m.pllMode, m.pllOpmode) ? "RUNNING" : pllLocked(m.pllMode) ? "locked" : "off", m.sleep);
    for (unsigned i = 0; i < kMclkCount; ++i)
      note("mclk%u: rcg %08x/%08x m %02x n %02x d %02x (source %u%s, %llu Hz from the XO) branch %08x (%s)%s", i, m.rcg[i].cmd,
           m.rcg[i].cfg, m.rcg[i].m & 0xff, m.rcg[i].n & 0xff, m.rcg[i].d & 0xff, rcgSource(m.rcg[i].cfg),
           m.rcg[i].rootOff ? ", root off" : "", (unsigned long long)m.rcg[i].hz, m.branch[i], branchWord(m.branch[i]),
           i == 1 ? " <- rear (24 MHz in Linux)" : i == 4 ? " <- front (19.2 MHz)" : "");
  }
  for (unsigned i = 0; i < kBlockCount; ++i) {
    const BlockState &b = s.blocks[i];
    if (b.read)
      note("camss %s: HW_VERSION %08x (%u.%u.%u)", kBlocks[i].name, b.version, b.version >> 28, (b.version >> 16) & 0xfff, b.version & 0xffff);
    else
      note("camss %s: not read (%s)", kBlocks[i].name, b.why);
  }
  if (s.cci.read)
    note("cci0: HW_VERSION %08x; irq mask %08x status %08x; m1 scl %08x sda0 %08x misc %08x; queues %u/%u words; read level %u",
         s.cci.version, s.cci.irqMask, s.cci.irqStatus, s.cci.sclCtl, s.cci.sdaCtl0, s.cci.miscCtl, s.cci.curCount[0],
         s.cci.curCount[1], s.cci.readLevel);
  else
    note("cci0: not read (%s)", s.cci.why);
  if (s.se1.read)
    note("qup2 se1: GENI fw revision %08x (protocol %u%s), status %08x", s.se1.fwRevision, s.se1.protocol(),
         s.se1.protocol() == 3 ? ": I2C" : "", s.se1.status);
  else
    note("qup2 se1: not read (%s)", s.se1.why);
  for (unsigned i = 0; i < kPinCount; ++i) {
    const PinState &p = s.pins[i];
    note("pin %s gpio%u: cfg %08x (function %u, pull %s, %u mA, %s%s) io %08x (in %u out %u)%s", kPins[i].name, kPins[i].gpio, p.cfg,
         pinFunction(p.cfg), pullName(p.cfg), drvMa(p.cfg), (p.cfg & kTlmmOe) ? "output" : "input",
         (p.cfg & kTlmmEgpioPresent) ? ((p.cfg & kTlmmEgpioEnable) ? ", egpio: ours" : ", egpio: NOT ours") : "", p.io, p.io & 1u,
         (p.io >> 1) & 1u, pinDrivenHigh(p.cfg, p.io) ? " DRIVEN HIGH" : "");
  }
  for (unsigned i = 0; i < kRailCount; ++i) {
    const RailState &r = s.rails[i];
    const Rail &t = kRails[i];
    if (!r.found)
      note("rail %s %s (%s, SID %u LDO %u): no LDO found at page %#x or %#x%s; cmd-db %s %#x", t.supply, t.regulator, t.cmdDb, t.sid,
           t.ldo, ldoPage(t.ldo), ldoPageAlt(t.ldo), gArgs.spmi ? "" : " (piano-camera-spmi=0)", r.cmdDbRc ? "MISSING" : "at", r.address);
    else
      note("rail %s %s (%s): PPID %#x%s APID %d owner EE %u: type %02x subtype %02x status %02x (%s) %u mV mode %02x en %02x (%s)%s; "
           "cmd-db %s %#x%s",
           t.supply, t.regulator, t.cmdDb, r.ppid, r.alt ? " (SM8550 page rule)" : "", r.apid, r.owner, r.type, r.subtype, r.status,
           r.ready() ? "ready" : "not ready", r.mv, r.mode, r.en, r.enabled() ? "ENABLED" : "disabled", r.read ? "" : " (NOT READ)",
           r.cmdDbRc ? "MISSING" : "at", r.address, t.voted ? "" : "; read only, never voted");
  }
  const SmmuState &mm = s.smmu;
  if (!mm.read) {
    note("smmu: not read (%s)", mm.why);
  } else {
    note("smmu: scr0 %08x idr0 %08x idr1 %08x: %u groups (%u free), %u banks; %u group(s) cover streams %#x-%#x", mm.scr0, mm.idr0,
         mm.idr1, mm.groups, mm.freeGroups, mm.banks, mm.count, kCamStream, kCamStreamScanLast);
    for (unsigned i = 0; i < mm.count; ++i) {
      const SmmuGroup &g = mm.found[i];
      const unsigned type = (g.s2cr >> 16) & 3u;
      note("smmu group %u: smr %08x (id %#x mask %#x) s2cr %08x (%s%s) cbar %08x sctlr %08x", g.index, g.smr, g.smr & 0x7fffu,
           (g.smr >> 16) & 0x7fffu, g.s2cr, type == 0 ? "translate via bank " : type == 1 ? "bypass" : "fault",
           type == 0 ? (g.bankRead ? "" : "?") : "", g.cbar, g.sctlr);
    }
  }
}

void summary(const Survey &s, char *out, unsigned size)
{
  unsigned on = 0, read = 0;
  for (unsigned i = 0; i < kRailCount; ++i) {
    read += s.rails[i].read;
    on += s.rails[i].enabled();
  }
  snprintf(out, size, "gcc cam-ahb %s mclk-ahb %s; camcc %s titan %s; mclk-cc %s mclk1 %s mclk4 %s; cci0 %s; rails %u/%u read, %u on; smmu %u group(s)",
           onOff(branchRunning(s.gcc.cameraAhb)), onOff(branchRunning(s.gcc.mclkAhb)), s.cc.read ? "read" : "not read",
           s.cc.read ? onOff(s.cc.titanOn()) : "?", s.mclk.read ? "read" : "not read",
           s.mclk.read ? onOff(branchRunning(s.mclk.branch[1])) : "?", s.mclk.read ? onOff(branchRunning(s.mclk.branch[4])) : "?",
           s.cci.read ? "read" : "gated", read, kRailCount, on, s.smmu.count);
}

// --------------------------------------------------------------------------------------------------------------- level 2 ops
class Ops {
 public:
  uint32_t rd(uint64_t pa) { return load(pa); }
  void wr(uint64_t pa, uint32_t v, const char *what)
  {
    ++gStores;
    note("write %#llx = %08x (%s)", (unsigned long long)pa, v, what);
    store(pa, v);
  }
  void wrQuiet(uint64_t pa, uint32_t v)
  {
    ++gQuietStores;
    store(pa, v);
  }
  void udelay(unsigned us)
  {
    if (us >= 2000) IOSleep((us + 999) / 1000);
    else IODelay(us);
  }
  void msleep(unsigned ms) { IOSleep(ms); }
  int vrm(unsigned rail, const VrmCmd *cmds, unsigned n)
  {
    const uint32_t base = survey ? survey->rails[rail].address : 0;
    if (!base || !mapRsc()) {
      note("rsc: %s: no RPMh address (or no DRV-2 mapping), nothing sent", kRails[rail].cmdDb);
      return -19;
    }
    uint32_t waits = 0;
    const uint32_t held = __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE);
    if (held) note("rsc: the DRV-2 lock is held by %#x; waiting (at most 2 s)", held);
    if (!lockAcquire(&sm8750_rpmh_drv2_owner, &waits, 200000)) {
      note("rsc: the DRV-2 lock stayed held by %#x for 2 s: %s not voted", __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE),
           kRails[rail].cmdDb);
      return -16;
    }
    VrmResult res;
    const int rc = vrmSend(gRsc, gRscIo, base, cmds, n, res);
    lockRelease(&sm8750_rpmh_drv2_owner);
    char line[120];
    int at = 0;
    for (unsigned i = 0; i < n && at < int(sizeof line) - 24; ++i)
      at += snprintf(line + at, sizeof line - at, "%s%s %u", i ? ", " : "",
                     cmds[i].reg == kVrmVoltage ? "mV" : cmds[i].reg == kVrmEnable ? "enable" : "mode", cmds[i].value);
    note("rsc: %s (%#x): %s through TCS %d %s after %u ms", kRails[rail].cmdDb, base, line, res.tcs,
         res.done ? "completed" : rc == -16 ? "NOT sent (no idle TCS)" : "NOT completed (left in flight, not used again)", res.waited);
    return rc;
  }
  int pmic(unsigned rail, RailState &r)
  {
    r = RailState();
    if (!survey || !survey->rails[rail].found) return -19;
    r = survey->rails[rail];
    r.read = false;
    uint8_t vset[2] = {}, me[2] = {};
    if (pmicRead(nullptr, r.apid, kPmicStatus1, &r.status, 1) || pmicRead(nullptr, r.apid, kPmicVset, vset, 2) ||
        pmicRead(nullptr, r.apid, kPmicMode, me, 2))
      return -5;
    r.mv = uint16_t(vset[0] | vset[1] << 8);
    r.mode = me[0];
    r.en = me[1];
    r.read = true;
    note("rail %s (%s): status %02x %u mV mode %02x en %02x", kRails[rail].supply, kRails[rail].cmdDb, r.status, r.mv, r.mode, r.en);
    return 0;
  }
  bool gccLock(bool take)
  {
    if (!take) {
      if (gccHeld) lockRelease(&sm8750_gcc_vote_owner);
      gccHeld = false;
      return true;
    }
    uint32_t waits = 0;
    gccHeld = lockAcquire(&sm8750_gcc_vote_owner, &waits, 5000);
    if (!gccHeld)
      note("GCC vote lock held by %#x for 50 ms: the camera branch is written anyway (no other driver writes it)",
           __atomic_load_n(&sm8750_gcc_vote_owner, __ATOMIC_ACQUIRE));
    return gccHeld;
  }
  void step(int n, const char *what)
  {
    piano_camera_stage = n;
    note("step %d: %s", n, what);
  }
  void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    IOLog("PianoCamera: %s\n", line);
  }
  const Survey *survey = nullptr;
  bool gccHeld = false;
};

IOLock *gSequenceLock;   // held while a level 2 sequence runs: a power transition waits for its power-down
char gResult[2][200];

void reportResult(const SensorResult &r)
{
  const SensorFacts &f = kSensors[r.which == Sensor::rear ? 0 : 1];
  if (r.failure == Failure::refused) {
    note("%s %s: REFUSED: %s; nothing written for it", f.name, f.part, r.why ? r.why : "?");
  } else if (r.idOk) {
    note("%s %s: chip ID %#06x (expected %#06x): OK over %s after %u tr%s; MCLK %u Hz", f.name, f.part, r.id, f.idValue, r.bus,
         r.tries, r.tries == 1 ? "y" : "ies", r.mclkHz);
  } else {
    note("%s %s: FAILED at step %d: %s%s%s (chip ID read %#06x, %u tr%s, bus %s, cci %s, i2c %s, last value %08x)", f.name, f.part,
         -r.failedStep, failureName(r.failure), r.why ? ": " : "", r.why ? r.why : "", r.id, r.tries, r.tries == 1 ? "y" : "ies", r.bus,
         cciName(r.cci), i2cName(r.i2c), r.lastValue);
  }
  if (r.cciVersion) note("%s: CCI0 HW_VERSION %08x", f.name, r.cciVersion);
  const unsigned first = r.which == Sensor::rear ? kRailRearVddd : kRailFrontVddio, count = r.which == Sensor::rear ? 3 : 2;
  for (unsigned i = 0; i < count; ++i) {
    const RailRecord &x = r.rails[i];
    note("%s rail %s (%s): found %u mV en %02x; on %u mV en %02x status %02x; off en %02x%s", f.name, kRails[first + i].supply,
         kRails[first + i].cmdDb, x.found.mv, x.found.en, x.on.mv, x.on.en, x.on.status, x.off.en,
         x.voted ? (x.ours ? "" : " (vote failed)") : " (not voted: found on)");
  }
  note("%s: %u writes logged, %u software I2C line changes, %u refused, %u off-side warnings; power down %s", f.name, r.writes,
       r.quietWrites, r.refusedStores, r.offWarnings, r.cleanOff ? "complete (everything as found)" : "INCOMPLETE");
  char *out = gResult[r.which == Sensor::rear ? 0 : 1];
  if (r.failure == Failure::refused) snprintf(out, sizeof gResult[0], "refused: %s", r.why ? r.why : "?");
  else if (r.idOk) snprintf(out, sizeof gResult[0], "chip id %#06x ok over %s, mclk %u Hz", r.id, r.bus, r.mclkHz);
  else snprintf(out, sizeof gResult[0], "failed at step %d: %s", -r.failedStep, failureName(r.failure));
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoCamera : public IOService {
  OSDeclareDefaultStructors(PianoCamera)
  void run();
  static void thread(void *self, wait_result_t);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
};

OSDefineMetaClassAndStructors(PianoCamera, IOService)

IOService *PianoCamera::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoCamera::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-camera=%u sensor=%u rear-bus=%s mclk24=%u cci=%u kHz i2c-half=%u us pull=%u blind=%u hold=%u ms stop=%u cycles=%u "
       "delay=%u s; survey cc=%u mclkcc=%u blocks=%u qup=%u spmi=%u smmu=%u",
       gArgs.level, gArgs.sensor, gArgs.rearBus ? "gpio" : "cci", gArgs.mclk24, gArgs.cciKhz, gArgs.halfUs, gArgs.pull, gArgs.blind,
       gArgs.hold, gArgs.stop, gArgs.cycles, gArgs.delay, gArgs.cc, gArgs.mclkcc, gArgs.blocks, gArgs.qup, gArgs.spmi, gArgs.smmu);
  if (gArgs.level > 2) {
    note("piano-camera=%u: levels 3-5 are designed, not built (docs/drivers/camera.md section 4); running 2", gArgs.level);
    gArgs.level = 2;
  }
  if (gArgs.level == 1) note("level 1: loads only, nothing written (the SPMI observer's read command is the only store)");
  setProperty("piano-camera", gArgs.level, 32);
  gSequenceLock = IOLockAlloc();
  thread_t t = nullptr;
  if (!gSequenceLock || kernel_thread_start(&PianoCamera::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no lock or no thread; nothing done");
    return true;
  }
  thread_deallocate(t);
  if (gArgs.level >= 2) pianopower::join(this);   // setPowerState(kOff) waits for a running sequence's power-down
  registerService();
  return true;
}

void PianoCamera::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-camera");
  static_cast<PianoCamera *>(self)->run();
}

void PianoCamera::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  SurveyOptions so;
  so.cc = gArgs.cc != 0;
  so.mclkcc = gArgs.mclkcc != 0;
  so.blocks = gArgs.blocks != 0;
  so.qup = gArgs.qup != 0;
  so.spmi = gArgs.spmi != 0;
  so.smmu = gArgs.smmu != 0;
  stage(1, "the survey (loads only; the SPMI observer for the PMIC)");
  Survey s = survey(gLoader, gPmic, so);
  reportSurvey(s);
  char line[220];
  summary(s, line, sizeof line);
  setProperty("piano-camera-survey", line);
  note("summary: %s", line);
  BusChoice bus;
  bus.rearCci = !gArgs.rearBus;
  bus.blind = gArgs.blind != 0;
  const Sensor both[2] = {Sensor::rear, Sensor::front};
  for (Sensor w : both) {
    const char *why = sensorRefusal(s, w, bus);
    note("level 2 verdict, %s: %s%s", kSensors[w == Sensor::rear ? 0 : 1].name, why ? "would refuse: " : "ready",
         why ? why : " (its rails, pins and MCLK are as level 2 expects)");
  }
  note("survey: %llu MMIO loads, %u observer reads (%u failed), %u map failures", (unsigned long long)gLoads, s.pmicReads, s.pmicFailures,
       gMapFailures);
  if (gArgs.level < 2) {
    stage(2, "done (level 1)");
    note("done: %llu MMIO loads, 0 stores", (unsigned long long)gLoads);
    cmdDbFree();
    unmapAll();
    unmapRegion(gCore);
    unmapRegion(gCnfg);
    unmapRegion(gObsv);
    return;
  }

  stage(3, "level 2: power each sensor, read its chip ID, power it down");
  PowerOptions po;
  po.rearCci = !gArgs.rearBus;
  po.mclk24 = gArgs.mclk24 != 0;
  po.cciKhz = gArgs.cciKhz;
  po.bitHalfUs = gArgs.halfUs;
  po.busPullUp = gArgs.pull != 0;
  po.blind = gArgs.blind != 0;
  po.holdMs = gArgs.hold;
  po.stopAt = int(gArgs.stop);
  Ops ops;
  ops.survey = &s;
  unsigned passed[2] = {0, 0}, attempted[2] = {0, 0};
  bool refused[2] = {false, false};   // a sensor the checks refused is not tried again in later cycles
  for (unsigned cycle = 0; cycle < gArgs.cycles; ++cycle) {
    for (Sensor w : both) {
      const unsigned k = w == Sensor::rear ? 0 : 1;
      if (!(gArgs.sensor & (1u << k)) || refused[k]) continue;
      note("cycle %u: %s %s", cycle + 1, kSensors[k].name, kSensors[k].part);
      IOLockLock(gSequenceLock);
      Session<Ops> session(ops, po);
      const SensorResult r = session.run(w, gLoader, gPmic, so, s);
      IOLockUnlock(gSequenceLock);
      reportResult(r);
      ++attempted[k];
      passed[k] += r.idOk && r.cleanOff;
      refused[k] = r.failure == Failure::refused;
    }
  }
  if (gArgs.sensor & 1) setProperty("piano-camera-rear", gResult[0]);
  if (gArgs.sensor & 2) setProperty("piano-camera-front", gResult[1]);
  stage(4, "done (level 2)");
  note("done: rear %u/%u, front %u/%u (chip ID and a clean power-down); %llu MMIO loads, %llu stores logged, %llu software I2C line "
       "changes, %u map failures",
       passed[0], attempted[0], passed[1], attempted[1], (unsigned long long)gLoads, (unsigned long long)gStores,
       (unsigned long long)gQuietStores, gMapFailures);
  cmdDbFree();
}

// A sequence is short (well under a second) and always ends powered down: a power transition only waits for it.
IOReturn PianoCamera::setPowerState(unsigned long state, IOService *)
{
  if (state == pianopower::kOff && gSequenceLock) {
    IOLockLock(gSequenceLock);
    IOLockUnlock(gSequenceLock);
  }
  return pianopower::kAckNow;
}
