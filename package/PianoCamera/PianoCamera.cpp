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
//   piano-camera=3  RAW10 frames from one sensor (CameraStream.h): level 2's power-up with TITAN_TOP for either bus, MM1 and CAMCC's
//                   domains through the shared votes (PianoMmVotes.h), CAMCC PLL0 and the stream's clocks, the SMMU route of stream
//                   0x1c00 (bypass as the boot chain left it, else our window), a contiguous buffer, CSID-lite0, TFE-lite0's write
//                   master 0, the CSIPHY with data/camera's lane table, the sensor's tables, a bounded stream, one frame's
//                   statistics, everything down in reverse. piano-camera-sensor=2 the front, else the rear; piano-camera-frames=N
//                   (1..8, 2), piano-camera-pattern=0|1 (the rear's colour bars, 1), piano-camera-timeout-ms=MS (500..10000, 3000),
//                   piano-camera-gain=CODE (32..2048, 64), piano-camera-mclk24 (1 here), piano-camera-stop=3xx.
// Without piano-camera the driver declines in probe() and maps nothing. Levels 4-5 (the software ISP, the macOS camera device) are
// designed in camera.md, not built. piano-camera-votes=0 (levels 2-3: no shared votes; level 3 always votes), piano-camera-mmcx-floor,
// piano-camera-ae=0 (level 3: no exposure loop), piano-camera-ae-target=N (16..800, 160 above black), piano-camera-ae-steps=N (1..8).
// Other arguments: piano-camera-sensor=1 rear | 2 front | 3 both (default 3, rear first), piano-camera-rear-bus=1 (software I2C on
// gpio115/116 instead of CCI0), piano-camera-mclk24=1 (the rear's MCLK at 24 MHz from the MCLK PLL), piano-camera-cci-khz=100|400,
// piano-camera-i2c-half-us=N (2..50, default 5), piano-camera-pull=1, piano-camera-blind=1, piano-camera-hold=MS (0..10000),
// piano-camera-stop=STEP, piano-camera-cycles=N (1..20), piano-camera-delay=S; the survey's parts: piano-camera-cc=0,
// piano-camera-mclkcc=0, piano-camera-blocks=0, piano-camera-qup=0, piano-camera-spmi=0, piano-camera-smmu=0.
// piano-camera-plan=NAME,NAME,... (CameraPlan.h): several test images' sequences on one boot after the survey, in that order, each
// with its image's arguments, the next only after a complete power-down; a "plan result" line per item at the end.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoPowerContract.h>
#include "CameraPower.h"
#include "CameraStream.h"
#include "CameraRpmh.h"
#include "CameraSurvey.h"
#include "CameraPlan.h"
#include "PianoCameraABI.h"
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
// The shared multimedia votes (package/include/PianoMmVotes.h: MXC, MMCX, MM1 with PianoVideo), PianoA830's words too.
extern volatile uint32_t piano_mm_votes_owner;
extern volatile uint32_t piano_mm_votes[32];
// XNU's (osfmk/arm64): the bytes left on the current thread's kernel stack (or the interrupt stack it is on).
vm_offset_t ml_stack_remaining(void);
}
#include <PianoMmVotes.h>

namespace {

constexpr uint32_t kTag = 0x43414d31u;   // "CAM1" in the lock words

struct Args {
  uint32_t level, sensor, rearBus, mclk24, cciKhz, halfUs, pull, blind, hold, stop, cycles, delay;
  uint32_t cc, mclkcc, blocks, qup, spmi, smmu;
  uint32_t votes, mmcxFloor;
  uint32_t frames, pattern, timeoutMs, gain;
  uint32_t ae, aeTarget, aeSteps;
};
Args gArgs;
char gPlanText[160];   // piano-camera-plan as given
Plan gPlan;

// Level 5 (the user client, first version: one capture per call; PianoCameraABI.h): the request words under gServeLock, the last
// capture's description, and memory type 0 (the last capture's RAW10 frame, kRawCap bytes: the rear's 16 MiB frame buffer's worth).
constexpr uint64_t kRawCap = 16ull << 20;
IOLock *gServeLock;
bool gRequestPending, gRequestDone, gServing;
PianoCameraCaptureArgs gRequest;
PianoCameraFrameInfo gInfo;
IOBufferMemoryDescriptor *gRawMem;
uint8_t *gRawBytes;
bool gKeepFrame;      // the next level-3 frameReady copies the frame into gRawMem
uint64_t gKept;       // the bytes it copied
uint32_t gCaptures;

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
  gArgs.mclk24 = bootArg("piano-camera-mclk24", gArgs.level >= 3 ? 1 : 0) ? 1 : 0;   // level 3: Linux's 24 MHz by default
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
  gArgs.votes = bootArg("piano-camera-votes", 1) ? 1 : 0;
  gArgs.mmcxFloor = clampArg(bootArg("piano-camera-mmcx-floor", kCamMmcxFloor), 0, 0x1000);
  gArgs.frames = clampArg(bootArg("piano-camera-frames", 2), 1, 8);
  gArgs.pattern = bootArg("piano-camera-pattern", 1) ? 1 : 0;
  gArgs.timeoutMs = clampArg(bootArg("piano-camera-timeout-ms", 3000), 500, 10000);
  gArgs.gain = clampArg(bootArg("piano-camera-gain", 64), 32, 2048);
  gArgs.ae = bootArg("piano-camera-ae", 1) ? 1 : 0;   // level 3: the exposure loop for the scene (CameraStream.h)
  gArgs.aeTarget = clampArg(bootArg("piano-camera-ae-target", 160), 16, 800);
  gArgs.aeSteps = clampArg(bootArg("piano-camera-ae-steps", 8), 1, 8);
  // piano-camera-plan (CameraPlan.h): its level for probe and the power tree; a plan that does not parse leaves level 1 (the survey
  // and the plan's error line, nothing written).
  gPlanText[0] = 0;
  if (PE_parse_boot_arg_str("piano-camera-plan", gPlanText, sizeof gPlanText) && gPlanText[0]) {
    gPlan = parsePlan(gPlanText);
    gArgs.level = gPlan.error ? 1 : gPlan.level > gArgs.level ? gPlan.level : gArgs.level;
  }
}
// One plan item's arguments in place of the image's (CameraPlan.h kPlanItems).
void applyPlanItem(const PlanItem &p)
{
  gArgs.level = p.level;
  gArgs.sensor = p.sensor;
  gArgs.rearBus = p.rearBus;
  gArgs.mclk24 = p.mclk24;
  gArgs.pattern = p.pattern;
  gArgs.frames = p.frames;
  gArgs.stop = p.stop;
}

// ------------------------------------------------------------------------------------------------ the camera thread's stack
// XNU's kernel threads have 16 KiB of stack (KERNEL_STACK_SIZE; the exception vector's check is SP within kstackptr - 0x4000).
// c3rp of 2026-11-13 overflowed it (`Invalid kernel stack pointer (probable overflow)`, then `MACH Reboot`) entering DRV-2's
// 64 KiB mapping at step 201: the sequence's objects were on it. They live in static slots now (gWork below); the least free stack
// seen at a log line or before an IOKit mapping is logged at the end of every run, and a mapping with less than kStackWarn left
// says so first (the line a further overflow would leave in the mailbox).
constexpr vm_offset_t kStackBytes = 16384, kStackWarn = 6144;
thread_t gThread;                    // the camera thread (the only one measured)
vm_offset_t gStackStart, gStackLeast = kStackBytes;
int32_t gStackLeastAt;               // piano_camera_stage when the least was seen
bool gStackWarned;
vm_offset_t stackMark()
{
  if (!gThread || current_thread() != gThread) return kStackBytes;
  const vm_offset_t left = ml_stack_remaining();
  if (left < gStackLeast) {
    gStackLeast = left;
    gStackLeastAt = piano_camera_stage;
  }
  return left;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Before an IOKit mapping or allocation (several KiB of XNU's VM path below the caller): the margin, and a line when it is short.
void stackCheck(const char *what)
{
  const vm_offset_t left = stackMark();
  if (left >= kStackWarn || gStackWarned) return;
  gStackWarned = true;
  note("stack: only %lu bytes of the thread's stack left before %s at step %d (an overflow would follow as a panic)",
       (unsigned long)left, what, piano_camera_stage);
}
void reportStack()
{
  note("stack: %lu of %lu bytes free at the thread's start, the least %lu at step %d (logging and IOKit mappings measured)",
       (unsigned long)gStackStart, (unsigned long)kStackBytes, (unsigned long)gStackLeast, gStackLeastAt);
}

void note(const char *fmt, ...)
{
  stackMark();
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
  stackCheck("a register page's mapping");
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
  stackCheck("an SPMI window's mapping");
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
    stackCheck("cmd-db's mapping");
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
  stackCheck("DRV-2's mapping");
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

// ------------------------------------------------------------------ the shared multimedia votes (PianoMmVotes.h): MXC, MMCX, MM1
// This kext is the helper's camera client: CAMCC's domains (levels 2 and 3), MM1 for the camera NoC (level 3). The resources from
// the cmd-db copy (the helper's reader), each transaction one TCS on apps DRV-2 under the DRV-2 lock (rpmhSend).
PianoMmArc gArc[2];   // mxc.lvl, mmcx.lvl
uint32_t gMm1Addr;
int gMmLookup = 1;    // 1 not looked up, 0 found, negative missing
const char *gMmMissing = "";
int mmLookup()
{
  if (gMmLookup <= 0) return gMmLookup;
  uint32_t any = 0;
  (void)cmdDbLookup(nullptr, "mxc.lvl", &any);   // the copy, made once
  if (!gCmdDb) {
    gMmMissing = "cmd-db";
    return gMmLookup = -12;
  }
  int rc = piano_mm_cmd_db_arc(gCmdDb, kCmdDbBytes, "mxc.lvl", &gArc[0]);
  if (rc) gMmMissing = "mxc.lvl";
  if (!rc && (rc = piano_mm_cmd_db_arc(gCmdDb, kCmdDbBytes, "mmcx.lvl", &gArc[1]))) gMmMissing = "mmcx.lvl";
  if (!rc && (rc = piano_mm_cmd_db(gCmdDb, kCmdDbBytes, "MM1", PIANO_MM_SLAVE_BCM, &gMm1Addr, nullptr, nullptr))) gMmMissing = "MM1";
  if (!rc && (gArc[0].addr == gArc[1].addr || gMm1Addr == gArc[0].addr || gMm1Addr == gArc[1].addr)) {
    rc = -3;
    gMmMissing = "distinct addresses";
  }
  gMmLookup = rc ? rc : 0;
  if (!rc)
    note("votes: mxc.lvl %#x (%u levels, top vlvl %#x), mmcx.lvl %#x (%u levels), MM1 %#x", gArc[0].addr, gArc[0].count,
         gArc[0].vlvl[gArc[0].count - 1], gArc[1].addr, gArc[1].count, gMm1Addr);
  return gMmLookup;
}
uint32_t mmAddress(void *, unsigned r) { return r == PIANO_MM_MXC ? gArc[0].addr : r == PIANO_MM_MMCX ? gArc[1].addr : r == PIANO_MM_MM1 ? gMm1Addr : 0; }
int mmLevel(void *, unsigned r, uint32_t vlvl, uint32_t *hlvl) { return r < 2 ? piano_mm_arc_level(&gArc[r], vlvl, hlvl) : -3; }
int mmSend(void *, const PianoMmCmd *cmds, unsigned n)
{
  if (!mapRsc()) return -19;
  uint32_t waits = 0;
  const uint32_t held = __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE);
  if (held) note("rsc: the DRV-2 lock is held by %#x; waiting (at most 2 s)", held);
  if (!lockAcquire(&sm8750_rpmh_drv2_owner, &waits, 200000)) {
    note("rsc: the DRV-2 lock stayed held by %#x for 2 s: the shared votes not sent", __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE));
    return -16;
  }
  RpmhCmd c[3];
  for (unsigned i = 0; i < n && i < 3; ++i) c[i] = {cmds[i].addr, cmds[i].data};
  VrmResult res;
  const int rc = rpmhSend(gRsc, gRscIo, c, n, res);
  lockRelease(&sm8750_rpmh_drv2_owner);
  note("rsc: shared votes: %u command(s) (%#x=%#x%s) through TCS %d %s after %u ms", n, c[0].addr, c[0].data, n > 1 ? ", ..." : "", res.tcs,
       res.done ? "completed" : rc == -16 ? "NOT sent (no idle TCS)" : "NOT completed (left in flight, not used again)", res.waited);
  return rc;
}
void mmSleep(void *, uint32_t ms) { IOSleep(ms); }
void mmReport(void *, uint32_t event, uint32_t a, uint32_t b)
{
  if (event == PianoMmLockWaiting) note("votes: the shared vote lock is held by %#x; waiting", a);
  else if (event == PianoMmLockGot) note("votes: the shared vote lock taken after %u ms (from %#x)", b, a);
  else if (event == PianoMmLockBusy) note("votes: the shared vote lock stayed held by %#x for %u ms: nothing voted", a, b);
  else if (event == PianoMmLockLost) note("votes: the shared vote lock was found held by %#x when given back", a);
}
const PianoMmVotesOps gMmOps = {nullptr, mmAddress, mmLevel, mmSend, mmSleep, mmReport};
PianoMmVotes gMm;
int mmUpdate(uint32_t mask, const uint32_t need[PIANO_MM_RESOURCES], const char *what)
{
  if (mmLookup()) {
    note("votes: %s: cmd-db lacks %s: nothing voted", what, gMmMissing);
    return -19;
  }
  gMm.words = piano_mm_votes;
  gMm.lock = &piano_mm_votes_owner;
  gMm.tag = PianoMmTagCamera;
  gMm.client = PIANO_MM_CAMERA;
  gMm.floor[PIANO_MM_MMCX] = gArgs.mmcxFloor;
  gMm.ops = &gMmOps;
  const int rc = piano_mm_votes_update(&gMm, mask, need);
  if (mask & (1u << PIANO_MM_MXC))
    note("votes: %s: %s; mxc hlvl %u (vlvl %#x, others %#x), mmcx hlvl %u (vlvl %#x, floor %#x, others %#x); %u command(s) sent", what,
         rc ? "FAILED" : "ok", gMm.level[PIANO_MM_MXC], gMm.vote[PIANO_MM_MXC], gMm.others[PIANO_MM_MXC], gMm.level[PIANO_MM_MMCX],
         gMm.vote[PIANO_MM_MMCX], gArgs.mmcxFloor, gMm.others[PIANO_MM_MMCX], gMm.count);
  else
    note("votes: %s: %s; MM1 %08x (other clients %s it); %u command(s) sent", what, rc ? "FAILED" : "ok", gMm.level[PIANO_MM_MM1],
         gMm.others[PIANO_MM_MM1] ? "need" : "do not need", gMm.count);
  return rc;
}

// --------------------------------------------------------------------------------------------------------------- the report
const char *onOff(bool on) { return on ? "on" : "off"; }
const char *branchWord(uint32_t v)
{
  if (branchRunning(v)) return branchEnabled(v) ? "on" : "on (not ours: voted/hw)";
  if (branchEnabled(v)) return branchHwGated(v) ? "on (hardware-gated, idle)" : "ENABLED but halted";
  return branchHwGated(v) ? "off (hardware-gated)" : "off";
}

// Items joined into lines of at most ~200 characters, each line under its title (a continuation says "(cont.)").
class Lines {
 public:
  explicit Lines(const char *title) : title_(title) {}
  void add(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char item[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(item, sizeof item, fmt, ap);
    va_end(ap);
    const size_t len = strlen(item);
    if (n_ && n_ + 2 + len > kWidth) flush();
    n_ += snprintf(line_ + n_, sizeof line_ - n_, "%s%s", n_ ? ", " : "", item);
  }
  void flush()
  {
    if (!n_) return;
    note("%s%s: %s", title_, parts_ ? " (cont.)" : "", line_);
    ++parts_;
    n_ = 0;
    line_[0] = 0;
  }
  // The same buffer under the next title (one Lines at a time on the camera thread's stack).
  void restart(const char *title)
  {
    flush();
    title_ = title;
    parts_ = 0;
  }

 private:
  static constexpr size_t kWidth = 190;
  const char *title_;
  char line_[kWidth + 130] = {};
  size_t n_ = 0;
  unsigned parts_ = 0;
};

// GCC's camera branches, the two clock controllers, the CAMSS blocks and CCI0 (level 1's survey; again after level 2's step 201).
void reportControllers(const Survey &s)
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
    // Lines of at most ~200 characters: the console cuts a line at 255 with its prefix and loses its newline (c3r, 2026-11-12: the
    // branches, RCGs and PLLs ran into one cut line).
    // One Lines for the four (each is 0.3 KiB of the camera thread's stack).
    Lines l("camcc gdsc");
    for (unsigned i = 0; i < kCcGdscCount; ++i)
      l.add("%s %08x/%08x (%s)", kCcGdscs[i].name, c.gdsc[i].gdscr, c.gdsc[i].cfg, gdscOn(c.gdsc[i].gdscr) ? "ON" : "off");
    l.restart("camcc branches");
    for (unsigned i = 0; i < kCcBranchCount; ++i) l.add("%s %08x", kCcBranches[i].name, c.branch[i]);
    l.restart("camcc rcgs");
    for (unsigned i = 0; i < kCcRcgCount; ++i)
      l.add("%s %08x/%08x src %u%s", kCcRcgs[i].name, c.rcg[i].cmd, c.rcg[i].cfg, rcgSource(c.rcg[i].cfg), c.rcg[i].rootOff ? " root-off" : "");
    l.restart("camcc plls");
    for (unsigned p = 0; p < kCcPllCount; ++p)
      l.add("pll%u mode %08x op %x L %#x (%s)", p, c.pllMode[p], c.pllOpmode[p], c.pllL[p],
            pllRunning(c.pllMode[p], c.pllOpmode[p]) ? "RUNNING" : pllLocked(c.pllMode[p]) ? "locked" : "off");
    l.flush();
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
}

void reportSurvey(const Survey &s)
{
  reportControllers(s);
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
           onOff(gccClockOn(s.gcc.cameraAhb)), onOff(gccClockOn(s.gcc.mclkAhb)), s.cc.read ? "read" : "not read",
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
  void surveyed(const Survey &s)
  {
    note("step 202: the controllers with GCC's branches enabled (the survey again):");
    reportControllers(s);
  }
  int mmRails(bool on)
  {
    const uint32_t need[PIANO_MM_RESOURCES] = {on ? kCamMxcNeed : 0u, on ? kCamMmcxNeed : 0u, 0u};
    return mmUpdate(1u << PIANO_MM_MXC | 1u << PIANO_MM_MMCX, need, on ? "CAMCC's domains up" : "CAMCC's domains withdrawn");
  }
  int mmBus(bool on)
  {
    const uint32_t need[PIANO_MM_RESOURCES] = {0u, 0u, on ? PIANO_MM_BCM_ON : 0u};
    return mmUpdate(1u << PIANO_MM_MM1, need, on ? "the camera NoC (MM1) on" : "the camera NoC (MM1) withdrawn");
  }
  void step(int n, const char *what)
  {
    piano_camera_stage = n;
    note("step %d: %s", n, what);
  }
  void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    stackMark();
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    IOLog("PianoCamera: %s\n", line);
  }
  // Level 3: the frame buffer (one physically contiguous range, 2 MiB aligned, below 1 TiB: the write master's reach), a cacheable
  // kernel view; the window's two table pages (one 16 KiB XNU page, contiguous).
  bool dmaAlloc(uint64_t bytes, bool tables, Dma &d)
  {
    renew(d);
    stackCheck("the frame buffer's allocation");
    IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), bytes, 0x000000ffffe00000ull);
    if (!buf || buf->prepare() != kIOReturnSuccess) {
      if (buf) buf->release();
      ::note("dma: no physically contiguous %llu bytes (2 MiB aligned) for the frame buffer", (unsigned long long)bytes);
      return false;
    }
    IOByteCount length = 0;
    d.pa = buf->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    d.bytes = bytes;
    d.cpu = static_cast<uint8_t *>(buf->getBytesNoCopy());
    d.cookie = buf;
    if (!d.pa || length < bytes || !d.cpu) {
      ::note("dma: the frame buffer is not one segment (%#llx, %llu of %llu bytes)", (unsigned long long)d.pa, (unsigned long long)length,
             (unsigned long long)bytes);
      dmaFree(d);
      return false;
    }
    if (tables) {
      tableMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
          kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), 0x4000, 0x000000fffffff000ull);
      if (!tableMem || tableMem->prepare() != kIOReturnSuccess) {
        if (tableMem) tableMem->release();
        tableMem = nullptr;
        dmaFree(d);
        return false;
      }
      d.tablePa = tableMem->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
      d.tables = static_cast<uint64_t *>(tableMem->getBytesNoCopy());
    }
    ::note("dma: frame buffer %llu bytes at %#llx (cpu %p)%s", (unsigned long long)bytes, (unsigned long long)d.pa, (void *)d.cpu,
           tables ? "; the window's tables follow" : "");
    if (tables) ::note("dma: the window's tables at %#llx", (unsigned long long)d.tablePa);
    return true;
  }
  void dmaFree(Dma &d)
  {
    if (IOBufferMemoryDescriptor *buf = static_cast<IOBufferMemoryDescriptor *>(d.cookie)) {
      buf->complete();
      buf->release();
    }
    if (tableMem) {
      tableMem->complete();
      tableMem->release();
      tableMem = nullptr;
    }
    d = Dma();
  }
  static void cacheLines(const void *p, uint64_t n)
  {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = uintptr_t(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = uintptr_t(p) & ~(line - 1); a < uintptr_t(p) + n; a += line) __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    dsb();
  }
  void cacheClean(const void *p, uint64_t n) { cacheLines(p, n); }        // clean + invalidate to the PoC before the device writes
  void cacheInvalidate(const void *p, uint64_t n) { cacheLines(p, n); }   // and after: the CPU wrote nothing in between
  uint64_t nowUs()
  {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
  }
  // Level 5: the stream's last frame (the write master stopped, the buffer invalidated by the statistics) into memory type 0.
  void frameReady(const uint8_t *buf, const StreamResult &r)
  {
    const uint64_t n = uint64_t(r.stats.stride) * r.height;
    gKept = 0;
    if (!gKeepFrame || !gRawBytes || !n || n > kRawCap) return;
    memcpy(gRawBytes, buf, n);
    gKept = n;
  }
  IOBufferMemoryDescriptor *tableMem = nullptr;
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
         x.voted ? (x.ours ? "" : " (vote failed)") : x.found.found ? " (not voted: found on)" : " (not reached)");
  }
  note("%s: %u writes logged, %u software I2C line changes, %u refused, %u off-side warnings; power down %s", f.name, r.writes,
       r.quietWrites, r.refusedStores, r.offWarnings, r.cleanOff ? "complete (everything as found)" : "INCOMPLETE");
  char *out = gResult[r.which == Sensor::rear ? 0 : 1];
  if (r.failure == Failure::refused) snprintf(out, sizeof gResult[0], "refused: %s", r.why ? r.why : "?");
  else if (r.idOk) snprintf(out, sizeof gResult[0], "chip id %#06x ok over %s, mclk %u Hz", r.id, r.bus, r.mclkHz);
  else snprintf(out, sizeof gResult[0], "failed at step %d: %s", -r.failedStep, failureName(r.failure));
}

// ---------------------------------------------------------------------------------------------------------------- level 3
char gL3[240];
// After the power-down: the verdict and the power-down's line (everything the stream found was logged before the power-down by
// Stream::run, CameraStream.h reportStream).
void reportStream(const StreamResult &r)
{
  const SensorFacts &f = kSensors[r.which == Sensor::rear ? 0 : 1];
  const FrameStats &s = r.stats;
  if (r.failedStep)
    note("level 3: %s %s %ux%u RAW10: FAILED at step %d: %s%s%s", f.name, f.part, r.width, r.height, -r.failedStep,
         streamFailureName(r.failure), r.why ? ": " : "", r.why ? r.why : "");
  else
    note("level 3: %s %s %ux%u RAW10: %s (%u frames, %u stale BUF_DONE; guard %s; window fault %s)", f.name, f.part, r.width, r.height,
         r.ok ? "OK" : "streamed, but see the checks", r.bufDone, r.staleBufDone, s.done ? (s.guardIntact ? "intact" : "DAMAGED") : "not read",
         (r.fault.fsr & iova::kFsrFaults) ? "YES" : "none");
  note("level 3: %u writes logged, %u quiet (the lane table, the stream's status clears), %u refused, %u off-side warnings; power down %s; "
       "buffer %s",
       r.writes, r.quietWrites, r.refusedStores, r.offWarnings, r.cleanOff ? "complete (everything as found)" : "INCOMPLETE",
       r.bufferFreed ? "freed" : r.pa ? "KEPT (not confirmed idle)" : "never allocated");
  if (r.failedStep)
    snprintf(gL3, sizeof gL3, "%s: failed at step %d: %s", f.name, -r.failedStep, streamFailureName(r.failure));
  else
    snprintf(gL3, sizeof gL3, "%s: %u frames, %s, guard %s, means %u/%u/%u/%u, ae %s (exposure %u gain %u), power down %s", f.name,
             r.bufDone, routeName(r.route), s.guardIntact ? "intact" : "damaged", s.ch[0].mean(), s.ch[1].mean(), s.ch[2].mean(),
             s.ch[3].mean(), aeVerdictShort(r.aeVerdict), r.exposure, r.gain, r.cleanOff ? "complete" : "incomplete");
}

// Level 5: the last capture's description from its stream's result.
void fillInfo(const StreamResult &r)
{
  PianoCameraFrameInfo &i = gInfo;
  const bool rear = r.which == Sensor::rear;
  const ExposureFacts &f = rear ? kRearExposure : kFrontExposure;
  memset(&i, 0, sizeof i);
  i.version = kPianoCameraABIVersion;
  i.status = gKept ? 0u : r.failedStep ? uint32_t(-r.failedStep) : 2u;
  i.sensor = rear ? kPianoCameraRear : kPianoCameraFront;
  i.width = r.width;
  i.height = r.height;
  i.stride = r.stats.stride;
  i.bayer = rear ? kPianoCameraGRBG : kPianoCameraBGGR;
  i.black = kRawBlack;
  i.bytes = gKept;
  i.exposure = r.exposure;
  i.gain = r.gain;
  i.gainUnit = f.gUnit;
  i.aeVerdict = uint32_t(r.aeVerdict);
  i.aeSamples = r.aeCount;
  for (unsigned p = 0; p < 4; ++p) i.means[p] = r.stats.ch[p].mean();
  i.sequence = ++gCaptures;
  i.level = 5;
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  i.timeNs = ns;
  snprintf(i.verdict, sizeof i.verdict, "%s", gL3);
}

// ------------------------------------------------------------------------------------------- the sequence's objects, off the stack
// One object in static storage, built in place for each use and destroyed after it (level 2's Session per cycle, level 3's Session
// and Stream): a Session is 2.6 KiB with its survey copy, a Stream 1.9 KiB with its result, on a 16 KiB stack (see stackMark).
template <class T>
class Slot {
 public:
  template <class... A>
  T &make(A &&...a)
  {
    drop();
    p_ = ::new (static_cast<void *>(buf_)) T(static_cast<A &&>(a)...);
    return *p_;
  }
  void drop()
  {
    if (p_) p_->~T();
    p_ = nullptr;
  }

 private:
  alignas(T) unsigned char buf_[sizeof(T)];
  T *p_ = nullptr;
};
// The one camera thread's (start() makes one per boot): level 1's survey, the options, the ops, the sequence's objects.
struct Work {
  Survey survey;
  SurveyOptions so;
  PowerOptions po;
  StreamOptions o;
  Ops ops;
  Slot<Session<Ops>> session;
  Slot<Stream<Ops>> stream;
};
Work gWork;

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoCamera : public IOService {
  OSDeclareDefaultStructors(PianoCamera)
  void run();
  bool sequence(char *verdict, size_t size);
  void serve();
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
       "delay=%u s; survey cc=%u mclkcc=%u blocks=%u qup=%u spmi=%u smmu=%u; votes=%u mmcx-floor=%#x ae=%u target=%u",
       gArgs.level, gArgs.sensor, gArgs.rearBus ? "gpio" : "cci", gArgs.mclk24, gArgs.cciKhz, gArgs.halfUs, gArgs.pull, gArgs.blind,
       gArgs.hold, gArgs.stop, gArgs.cycles, gArgs.delay, gArgs.cc, gArgs.mclkcc, gArgs.blocks, gArgs.qup, gArgs.spmi, gArgs.smmu,
       gArgs.votes, gArgs.mmcxFloor, gArgs.ae, gArgs.aeTarget);
  if (gArgs.level > 3) {   // 4 (the software ISP) lives in user space: both are level 5's kernel side
    if (gArgs.level != 5) note("piano-camera=%u: level 4 is the user-space ISP (docs/drivers/camera.md 4.4); running 5", gArgs.level);
    gArgs.level = 5;
    note("level 5: the user client PianoCameraUserClient (a capture per call: the sensor up, the exposure loop, the last RAW10 frame "
         "kept in memory 0, everything down; an administrator only)");
    setProperty("IOUserClientClass", "PianoCameraUserClient");
  }
  if (gPlanText[0] && gPlan.error)
    note("plan: '%s' not run (%s): the survey only", gPlanText,
         gPlan.error == -1 ? "an unknown name (one of c1b c2f c2r c2rg c2r24 c3rp c3r c3rs c3f)"
         : gPlan.error == -2 ? "more than 8 names" : "empty");
  else if (gPlan.count)
    note("plan: %s (%u sequences after the survey, each with its image's arguments, the next only after a complete power-down)",
         gPlanText, gPlan.count);
  else if (gArgs.level == 3)
    note("level 3: %s sensor, frames=%u pattern=%u timeout=%u ms gain=%u mclk24=%u", gArgs.sensor == 2 ? "the front" : "the rear", gArgs.frames,
         gArgs.pattern, gArgs.timeoutMs, gArgs.gain, gArgs.mclk24);
  if (gArgs.level == 1) note("level 1: loads only, nothing written (the SPMI observer's read command is the only store)");
  setProperty("piano-camera", gArgs.level, 32);
  gSequenceLock = IOLockAlloc();
  gServeLock = IOLockAlloc();
  thread_t t = nullptr;
  if (!gSequenceLock || !gServeLock || kernel_thread_start(&PianoCamera::thread, this, &t) != KERN_SUCCESS) {
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
  gThread = current_thread();
  gStackStart = ml_stack_remaining();
  gStackLeast = gStackStart;
  static_cast<PianoCamera *>(self)->run();
  reportStack();
}

// Everything of size in gWork (the camera thread's stack is 16 KiB; c3rp of 2026-11-13 overflowed it).
void PianoCamera::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  Work &k = gWork;
  SurveyOptions &so = k.so;
  so.cc = gArgs.cc != 0;
  so.mclkcc = gArgs.mclkcc != 0;
  so.blocks = gArgs.blocks != 0;
  so.qup = gArgs.qup != 0;
  so.spmi = gArgs.spmi != 0;
  so.smmu = gArgs.smmu != 0;
  stage(1, "the survey (loads only; the SPMI observer for the PMIC)");
  Survey &s = k.survey;
  survey(gLoader, gPmic, so, s);
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
    if (why && !s.mclk.read && gArgs.mclkcc && !gccClockOn(s.gcc.mclkAhb))
      note("level 2 verdict, %s: pending: the clock controllers were not readable (GCC's AHB off); level 2's step 201 turns them on "
           "(for the rear on CCI0 after voting CAMCC's domains) and checks again at step 203",
           kSensors[w == Sensor::rear ? 0 : 1].name);
    else
      note("level 2 verdict, %s: %s%s", kSensors[w == Sensor::rear ? 0 : 1].name, why ? "would refuse: " : "ready",
           why ? why : " (its rails, pins and MCLK are as level 2 expects)");
  }
  note("survey: %llu MMIO loads, %u observer reads (%u failed), %u map failures", (unsigned long long)gLoads, s.pmicReads, s.pmicFailures,
       gMapFailures);
  {   // the SPMI lock every kext shares (PianoSpmi.h): this kext's transactions and waits, and every kext's
    char lockLine[360];
    pianospmi::format(lockLine, sizeof lockLine);
    note("%s", lockLine);
  }
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
  char verdict[240];
  if (gArgs.level >= 5) {
    serve();   // never returns
    return;
  }
  if (!gPlan.count) {
    sequence(verdict, sizeof verdict);
    cmdDbFree();
    return;
  }
  // The plan (CameraPlan.h): each item with its image's arguments, the next only after a complete power-down.
  static char results[kPlanMax][240];
  unsigned ran = 0;
  bool stopped = false;
  for (unsigned i = 0; i < gPlan.count; ++i) {
    const PlanItem &p = *gPlan.item[i];
    if (i) IOSleep(500);   // the rails off a while between two sequences (each powers its sensor up from reset anyway)
    applyPlanItem(p);
    note("plan %u/%u: %s (level %u, the %s; rear-bus %s mclk24 %u pattern %u frames %u stop %u)", i + 1, gPlan.count, p.name, p.level,
         p.sensor == 2 ? "front" : "rear", p.rearBus ? "gpio" : "cci", p.mclk24, p.pattern, p.frames, p.stop);
    const bool goesOn = sequence(verdict, sizeof verdict);
    snprintf(results[i], sizeof results[i], "%s", verdict);
    ++ran;
    if (!goesOn && i + 1 < gPlan.count) {
      note("plan: stopped after %s (its power-down was not complete, or its buffer was kept): %u of %u not run", p.name,
           gPlan.count - i - 1, gPlan.count);
      stopped = true;
      break;
    }
  }
  for (unsigned i = 0; i < ran; ++i) note("plan result %u/%u %s: %s", i + 1, gPlan.count, gPlan.item[i]->name, results[i]);
  note("plan: %u of %u run%s", ran, gPlan.count, stopped ? " (stopped)" : "");
  setProperty("piano-camera-plan", gPlanText);
  cmdDbFree();
}

// One image's sequence after the survey, from gArgs (level 2: each sensor of piano-camera-sensor, cycles; level 3: one sensor). The
// verdict line is the image's property; true when the plan may go on (CameraPlan.h planGoesOn).
bool PianoCamera::sequence(char *verdict, size_t size)
{
  Work &k = gWork;
  const SurveyOptions &so = k.so;
  Survey &s = k.survey;
  const Sensor both[2] = {Sensor::rear, Sensor::front};
  if (gArgs.level == 2) stage(3, "level 2: power each sensor, read its chip ID, power it down");
  PowerOptions &po = k.po;
  renew(po);
  po.rearCci = !gArgs.rearBus;
  po.mclk24 = gArgs.mclk24 != 0;
  po.cciKhz = gArgs.cciKhz;
  po.bitHalfUs = gArgs.halfUs;
  po.busPullUp = gArgs.pull != 0;
  po.blind = gArgs.blind != 0;
  po.holdMs = gArgs.hold;
  po.stopAt = int(gArgs.stop);
  po.votes = gArgs.votes != 0;
  po.camss = true;   // TITAN_TOP and CAMCC's domains for either sensor (c2fx, 2026-11-13: the front's MCLK4 needs them)
  Ops &ops = k.ops;
  ops.survey = &s;
  if (gArgs.level == 3) {
    // One sensor (piano-camera-sensor=2: the front; else the rear), TITAN_TOP and CAMCC's domains for either bus, the rear's MCLK at
    // Linux's 24 MHz (piano-camera-mclk24 defaults to 1 here); piano-camera-stop below 300 stops level 2's part.
    const Sensor w = gArgs.sensor == 2 ? Sensor::front : Sensor::rear;
    po.camss = true;
    po.votes = true;
    po.stopAt = gArgs.stop < 300 ? int(gArgs.stop) : 0;
    StreamOptions &o = k.o;
    renew(o);
    o.frames = gArgs.frames;
    o.pattern = gArgs.pattern != 0;
    o.timeoutMs = gArgs.timeoutMs;
    o.gain = gArgs.gain;
    o.ae = gArgs.ae != 0;
    o.aeTarget = gArgs.aeTarget;
    o.aeSteps = gArgs.aeSteps;
    o.stopAt = gArgs.stop >= 300 ? int(gArgs.stop) : 0;
    stage(5, "level 3: RAW10 frames through CSIPHY, CSID-lite0 and TFE-lite0's RDI0 into memory");
    IOLockLock(gSequenceLock);
    Session<Ops> &session = k.session.make(ops, po);
    Stream<Ops> &stream = k.stream.make(ops, session, o);
    const StreamResult &r = stream.run(w, gLoader, gPmic, so, s);
    IOLockUnlock(gSequenceLock);
    reportResult(r.sensor);
    reportStream(r);
    if (gKeepFrame) fillInfo(r);   // after reportStream: the description carries its result line
    setProperty("piano-camera-level3", gL3);
    stage(r.failedStep ? -6 : 6, r.ok ? "done (level 3)" : "done (level 3, see the lines above)");
    note("done: level 3 %s; %llu MMIO loads, %llu stores logged, %llu quiet stores, %u map failures", r.ok ? "ok" : "not ok",
         (unsigned long long)gLoads, (unsigned long long)gStores, (unsigned long long)gQuietStores, gMapFailures);
    snprintf(verdict, size, "%s", gL3);
    const bool goesOn = planGoesOn(r.cleanOff, r.pa && !r.bufferFreed);
    k.stream.drop();
    k.session.drop();
    return goesOn;
  }
  unsigned passed[2] = {0, 0}, attempted[2] = {0, 0};
  bool refused[2] = {false, false};   // a sensor the checks refused is not tried again in later cycles
  bool clean = true;
  for (unsigned cycle = 0; cycle < gArgs.cycles; ++cycle) {
    for (Sensor w : both) {
      const unsigned i = w == Sensor::rear ? 0 : 1;
      if (!(gArgs.sensor & (1u << i)) || refused[i]) continue;
      note("cycle %u: %s %s", cycle + 1, kSensors[i].name, kSensors[i].part);
      IOLockLock(gSequenceLock);
      Session<Ops> &session = k.session.make(ops, po);   // a fresh session for each cycle, as before
      const SensorResult &r = session.run(w, gLoader, gPmic, so, s);
      IOLockUnlock(gSequenceLock);
      reportResult(r);
      ++attempted[i];
      passed[i] += r.idOk && r.cleanOff;
      refused[i] = r.failure == Failure::refused;
      clean = clean && r.cleanOff;
      k.session.drop();
    }
  }
  if (gArgs.sensor & 1) setProperty("piano-camera-rear", gResult[0]);
  if (gArgs.sensor & 2) setProperty("piano-camera-front", gResult[1]);
  stage(4, "done (level 2)");
  note("done: rear %u/%u, front %u/%u (chip ID and a clean power-down); %llu MMIO loads, %llu stores logged, %llu software I2C line "
       "changes, %u map failures",
       passed[0], attempted[0], passed[1], attempted[1], (unsigned long long)gLoads, (unsigned long long)gStores,
       (unsigned long long)gQuietStores, gMapFailures);
  if (gArgs.sensor == 3) snprintf(verdict, size, "rear %s; front %s", gResult[0], gResult[1]);
  else snprintf(verdict, size, "%s %s", kSensors[gArgs.sensor == 2 ? 1 : 0].name, gResult[gArgs.sensor == 2 ? 1 : 0]);
  return planGoesOn(clean, false);
}

// Level 5: the camera thread waits for a user client's capture (gRequest), runs it as a level-3 sequence with the request's sensor,
// pattern and exposure loop (one frame after the loop), keeps the frame in memory type 0 and its description in gInfo, and is idle
// (everything powered down) between captures.
void PianoCamera::serve()
{
  gRawMem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryKernelUserShared),
                                                     kRawCap, 16384);
  gRawBytes = gRawMem ? static_cast<uint8_t *>(gRawMem->getBytesNoCopy()) : nullptr;
  memset(&gInfo, 0, sizeof gInfo);
  gInfo.version = kPianoCameraABIVersion;
  gInfo.status = 1;
  gInfo.level = 5;
  if (!gRawBytes) {
    stage(-7, "level 5: no memory for the frame (16 MiB); no user client");
    return;
  }
  IOLockLock(gServeLock);
  gServing = true;
  IOLockUnlock(gServeLock);
  stage(7, "level 5: ready for captures (PianoCameraUserClient)");
  for (;;) {
    IOLockLock(gServeLock);
    while (!gRequestPending) IOLockSleep(gServeLock, &gRequestPending, THREAD_UNINT);
    const PianoCameraCaptureArgs q = gRequest;
    IOLockUnlock(gServeLock);
    note("level 5: capture %u: the %s, %s, exposure loop %s (target %u)", gCaptures + 1, q.sensor == kPianoCameraFront ? "front" : "rear",
         q.pattern ? "colour bars" : "the scene", q.ae ? "on" : "off", q.aeTarget ? q.aeTarget : gArgs.aeTarget);
    const Args saved = gArgs;
    gArgs.level = 3;
    gArgs.sensor = q.sensor == kPianoCameraFront ? 2 : 1;
    gArgs.pattern = q.pattern ? 1 : 0;
    gArgs.ae = q.ae ? 1 : 0;
    if (q.aeTarget) gArgs.aeTarget = clampArg(q.aeTarget, 16, 800);
    gArgs.frames = 1;
    gArgs.stop = 0;
    gArgs.mclk24 = 1;
    gKeepFrame = true;
    gKept = 0;
    char verdict[240];
    sequence(verdict, sizeof verdict);
    gKeepFrame = false;
    gArgs = saved;
    reportStack();
    note("level 5: capture %u: %s; %llu bytes kept (status %u)", gInfo.sequence, gInfo.verdict, (unsigned long long)gInfo.bytes,
         gInfo.status);
    IOLockLock(gServeLock);
    gRequestPending = false;
    gRequestDone = true;
    IOLockWakeup(gServeLock, &gRequestDone, false);
    IOLockUnlock(gServeLock);
  }
}

// ------------------------------------------------------------------------------------------------------- the user client
class PianoCameraUserClient : public IOUserClient {
  OSDeclareDefaultStructors(PianoCameraUserClient)

 public:
  bool initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) override;
  IOReturn clientClose() override;
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *a, IOExternalMethodDispatch *dispatch, OSObject *target,
                          void *reference) override;
  IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
};

OSDefineMetaClassAndStructors(PianoCameraUserClient, IOUserClient)

// An administrator only, in this version (the camera extension, sandboxed and not root, needs an entitlement check: camera.md 4.5).
bool PianoCameraUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties)
{
  if (clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return false;
  return IOUserClient::initWithTask(owningTask, securityToken, type, properties);
}

IOReturn PianoCameraUserClient::clientClose()
{
  if (!isInactive()) terminate();
  return kIOReturnSuccess;
}

// One capture: posted to the camera thread, waited for (at most the request's timeout); one at a time.
static IOReturn capture(const PianoCameraCaptureArgs &args, PianoCameraFrameInfo &out)
{
  IOLockLock(gServeLock);
  if (!gServing) {
    IOLockUnlock(gServeLock);
    return kIOReturnNotReady;
  }
  if (gRequestPending) {
    IOLockUnlock(gServeLock);
    return kIOReturnBusy;
  }
  gRequest = args;
  gRequestDone = false;
  gRequestPending = true;
  IOLockWakeup(gServeLock, &gRequestPending, false);
  uint64_t deadline = 0;
  clock_interval_to_deadline(args.timeoutMs ? args.timeoutMs : 20000, kMillisecondScale, &deadline);
  while (!gRequestDone) {
    if (IOLockSleepDeadline(gServeLock, &gRequestDone, deadline, THREAD_UNINT) == THREAD_TIMED_OUT && !gRequestDone) {
      IOLockUnlock(gServeLock);
      return kIOReturnTimeout;   // the capture goes on; a later info call reads its result
    }
  }
  out = gInfo;
  IOLockUnlock(gServeLock);
  return kIOReturnSuccess;
}

IOReturn PianoCameraUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a, IOExternalMethodDispatch *, OSObject *,
                                               void *)
{
  if (!a || !a->structureOutput || a->structureOutputSize < sizeof(PianoCameraFrameInfo)) return kIOReturnBadArgument;
  PianoCameraFrameInfo info;
  IOReturn rc = kIOReturnSuccess;
  switch (selector) {
  case kPianoCameraInfo:
    IOLockLock(gServeLock);
    info = gInfo;
    IOLockUnlock(gServeLock);
    break;
  case kPianoCameraCapture: {
    PianoCameraCaptureArgs args;
    if (!a->structureInput || a->structureInputSize != sizeof args) return kIOReturnBadArgument;
    memcpy(&args, a->structureInput, sizeof args);
    if (args.version != kPianoCameraABIVersion || (args.sensor != kPianoCameraRear && args.sensor != kPianoCameraFront))
      return kIOReturnBadArgument;
    rc = capture(args, info);
    if (rc) return rc;
    break;
  }
  default:
    return kIOReturnUnsupported;
  }
  memcpy(a->structureOutput, &info, sizeof info);
  a->structureOutputSize = sizeof info;
  return rc;
}

IOReturn PianoCameraUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
  if (type != kPianoCameraMemoryRaw || !gRawMem) return kIOReturnBadArgument;
  gRawMem->retain();
  *memory = gRawMem;
  *options = kIOMapReadOnly;
  return kIOReturnSuccess;
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
