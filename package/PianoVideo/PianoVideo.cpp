// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo: piano's video core (SM8750 Iris, VPU 3.5: H.264/HEVC/VP9 decode, H.264/HEVC encode) for macOS 27, native at EL1.
// Design, the ladder (power, firmware via PAS, HFI, one H.264 frame, VideoToolbox through the VM-era paravirtual decoder, HEVC/VP9,
// encode) and the test plan: docs/drivers/video.md. Facts and their sources: VideoHw.h. This is level 1 only:
//   piano-video=1  a read-only survey, logged as "PianoVideo: ..." lines: GCC's four video branches; VIDEO_CC (only with GCC's
//                  video AHB on): PLL0, the core clock's RCG and dividers (the rate and the OPP it matches), both GDSCs, every
//                  branch; the core's controller registers (version, CTRL_INIT/CTRL_STATUS, the queue table and UC region words,
//                  the interrupt, LPI and power-status words) only with the controller GDSC powered and its clocks on, the codec's
//                  idle words only with the codec domain on too; the apps SMMU's entries for streams 0x1940..0x194f, the free
//                  stream-match groups and the banks in use. Nothing is written: every MMIO access is a load (VideoSurvey.h has no
//                  store), and the two registers whose access acts (A2HSOFTINTCLR, IC_SOFTINT) are never loaded.
// Other arguments: piano-video-cc=0 (skip VIDEO_CC and the core: the fallback if a load there stalls; default 1),
// piano-video-smmu=N (0 skip the SMMU, 1 its global pages only, 2 (default) also the matching banks' registers),
// piano-video-carveout=1 (also load the first 32 bytes of the firmware carveout 0x9c300000, memory XNU does not own; default 0),
// piano-video-delay=S (wait S s first, default 0), piano-video-repeat=S with piano-video-samples=N (read the clock words again every
// S s, N times, default 3: one line each, to see whether anything powers the core later). Without piano-video the driver declines in
// probe() and maps nothing.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "VideoSurvey.h"

using namespace pianovideo;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_video_stage = 0;
}

namespace {

struct Args {
  uint32_t level, cc, smmu, carveout, delay, repeat, samples;
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
  gArgs.level = bootArg("piano-video", 0);
  gArgs.cc = bootArg("piano-video-cc", 1) ? 1 : 0;
  gArgs.smmu = bootArg("piano-video-smmu", 2);
  if (gArgs.smmu > 2) gArgs.smmu = 2;
  gArgs.carveout = bootArg("piano-video-carveout", 0) ? 1 : 0;
  gArgs.delay = bootArg("piano-video-delay", 0);
  if (gArgs.delay > 600) gArgs.delay = 600;
  gArgs.repeat = bootArg("piano-video-repeat", 0);
  if (gArgs.repeat > 3600) gArgs.repeat = 3600;
  gArgs.samples = bootArg("piano-video-samples", 3);
  if (gArgs.samples > 100) gArgs.samples = 100;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[480];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoVideo: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_video_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

// Explicit ldr, as PianoDpu, PianoAudio and PianoWifi do. This file has no store helper.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}

// ------------------------------------------------------------------------------------------------ MMIO pages, mapped on demand
// PianoDpu's scheme: a page is mapped (device memory, uncached) the first time one of its words is loaded. The survey touches at
// most 20 pages: GCC (one page), VIDEO_CC (three), the core (six), the SMMU's global pages and up to six banks, the carveout's first.
struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
constexpr unsigned kMaxPages = 32;
Page gPages[kMaxPages];
unsigned gPageCount, gMapFailures;
uint64_t gLoads;

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

void unmapAll()
{
  for (unsigned i = 0; i < gPageCount; ++i) gPages[i].map->release();
  gPageCount = 0;
}

const char *onOff(uint32_t cbcr) { return branchOn(cbcr) ? "on" : "off"; }

// ------------------------------------------------------------------------------------------------------------- the report
void reportGcc(const Clocks &c)
{
  note("gcc: video ahb %08x (%s) xo %08x (%s) axi0 %08x (%s) axi1 %08x (%s)", c.gccAhb, onOff(c.gccAhb), c.gccXo, onOff(c.gccXo),
       c.gccAxi0, onOff(c.gccAxi0), c.gccAxi1, onOff(c.gccAxi1));
}

void reportVideoCc(const Clocks &c)
{
  const uint64_t pll = pllHz(c.pllL, c.pllAlpha), src = mvs0SourceHz(c), core = mvs0Hz(c);
  const Rcg m = rcg(c.mvs0RcgCmd, c.mvs0RcgCfg), a = rcg(c.ahbRcgCmd, c.ahbRcgCfg);
  const int opp = oppIndex(core);
  char oppText[32];
  if (opp >= 0) snprintf(oppText, sizeof oppText, "OPP %u MHz", kOppMHz[opp]);
  else snprintf(oppText, sizeof oppText, "no OPP");
  note("videocc: pll0 mode %08x (%s, %s) opmode %08x state %08x L %u alpha %04x -> %llu MHz%s; mvs0 rcg cmd %08x cfg %08x (root %s, "
       "source %s, divider %u.%u) div %08x -> source %llu MHz, mvs0 %llu MHz (%s); mvs0c div2 %08x; ahb rcg cmd %08x cfg %08x "
       "(root %s, source %u)",
       c.pllMode, pllLocked(c.pllMode) ? "locked" : "not locked", pllRunning(c) ? "running" : "not running", c.pllOpmode, c.pllState,
       c.pllL & 0xffffu, c.pllAlpha & 0xffffu, (unsigned long long)(pll / 1000000),
       (c.pllL & 0xffffu) == kPllConfigL && (c.pllAlpha & 0xffffu) == kPllConfigAlpha ? " (Linux's configuration)" : "",
       c.mvs0RcgCmd, c.mvs0RcgCfg, m.rootOff ? "off" : "on", m.source == 0 ? "xo" : m.source == 1 ? "pll0" : "other",
       m.halfDivider / 2, m.halfDivider & 1u ? 5 : 0, c.mvs0Div, (unsigned long long)(src / 1000000),
       (unsigned long long)(core / 1000000), oppText, c.mvs0cDiv2, c.ahbRcgCmd, c.ahbRcgCfg, a.rootOff ? "off" : "on", a.source);
  note("videocc: gdsc mvs0c %08x (%s) cfg %08x, mvs0 %08x (%s) cfg %08x; branches mvs0c %08x (%s) mvs0c-freerun %08x (%s) mvs0 %08x "
       "(%s) mvs0-freerun %08x (%s, mem %08x) ahb %08x (%s) xo %08x (%s) sleep %08x (%s) shift %08x/%08x",
       c.mvs0cGdsc, gdscState(c.mvs0cGdsc), c.mvs0cGdscCfg, c.mvs0Gdsc, gdscState(c.mvs0Gdsc), c.mvs0GdscCfg, c.mvs0c, onOff(c.mvs0c),
       c.mvs0cFreerun, onOff(c.mvs0cFreerun), c.mvs0, onOff(c.mvs0), c.mvs0Freerun, onOff(c.mvs0Freerun), c.mvs0FreerunMem, c.ahb,
       onOff(c.ahb), c.xo, onOff(c.xo), c.sleep, onOff(c.sleep), c.mvs0cShift, c.mvs0Shift);
}

void reportCore(const Clocks &c, const Core &r)
{
  if (!r.controllerRead) {
    note("core: not read (%s)", c.why);
    return;
  }
  const uint32_t v = r.hwVersion;
  note("core: wrapper version %08x (%u.%u.%u; venus v6 layout, inferred); ctrl init %08x status %08x (init %s, error %u, pc-ready %u, "
       "idle %u); queue table info %08x addr %08x; sfr %08x; uc region %08x + %08x; scia arg3 %08x h2x-int-en %08x x2rpmh %08x "
       "bridge-reset %08x",
       v, (v >> 28) & 7u, (v >> 16) & 0xfffu, v & 0xffffu, r.ctrlInit, r.ctrlStatus, (r.ctrlStatus & 1u) ? "done" : "not done",
       ctrlError(r.ctrlStatus), (r.ctrlStatus >> 8) & 1u, (r.ctrlStatus >> 30) & 1u, r.qtblInfo, r.qtblAddr, r.sfrAddr, r.ucAddr,
       r.ucSize, r.sciaArg3, r.h2xIntEn, r.x2rpmh, r.bridgeReset);
  note("core: wrapper intr status %08x mask %08x; lpi debug %08x/%08x cpu-noc %08x/%08x; spare0 %08x; power status %08x (codec %s) "
       "clock config %08x; tz cpu status %08x (%s) axi-clock %08x fifo-reset %08x; aon lpi %08x/%08x sw-reset %08x clk %08x spare "
       "%08x video-ctl lpi %08x/%08x",
       r.intrStatus, r.intrMask, r.dbgLpiCtl, r.dbgLpiStatus, r.cpuNocLpiCtl, r.cpuNocLpiStatus, r.spare0, r.powerStatus,
       (r.powerStatus >> 1) & 1u ? "powered" : "collapsed", r.clockConfig, r.tzCpuStatus, (r.tzCpuStatus & 1u) ? "WFI" : "running",
       r.tzAxiClock, r.tzFifoReset, r.aonLpiCtl, r.aonLpiStatus, r.aonSwReset, r.aonClkCtl, r.aonSpare, r.aonVideoLpiCtl,
       r.aonVideoLpiStatus);
  if (r.codecRead)
    note("codec: idle status %08x %08x %08x %08x (bit 22: idle)", r.idle[0], r.idle[1], r.idle[2], r.idle[3]);
  else
    note("codec: not read (the codec domain MVS0 is off or unclocked)");
}

void reportSmmu(const SmmuSurvey &m)
{
  note("SMMU scr0 %08x idr0 %08x idr1 %08x: %u stream-match groups (%u free), %u banks (%u in use by valid groups), %u pages, "
       "coherent table walks %s%s%s",
       m.scr0, m.idr0, m.idr1, m.groups, m.freeGroups, m.banks, m.banksReferenced, m.pages, coherentWalks(m) ? "yes" : "no",
       m.error ? ": " : "", m.error ? m.error : "");
  if (m.error) return;
  if (!m.count) note("SMMU streams 0x1940-0x194f: no stream-match entry");
  for (unsigned i = 0; i < m.count; ++i) {
    const SmmuEntry &e = m.found[i];
    note("SMMU group %u smr %08x (id %#x mask %#x) s2cr %08x (%s, bank %u) cbar %08x (%s) cba2r %08x", e.group, e.smr, e.smr & 0x7fffu,
         (e.smr >> 16) & 0x7fffu, e.s2cr, s2crType(e.s2cr), e.s2cr & 0xffu, e.bank.cbar, cbarType(e.bank.cbar), e.bank.cba2r);
    if (!e.bank.read) continue;
    const SmmuBank &b = e.bank;
    note("SMMU bank %u: sctlr %08x (translation %s) tcr %08x tcr2 %08x ttbr0 %08x%08x mair0 %08x fsr %08x", e.s2cr & 0xffu, b.sctlr,
         (b.sctlr & 1u) ? "on" : "off", b.tcr, b.tcr2, b.ttbr0hi, b.ttbr0lo, b.mair0, b.fsr);
  }
  if (m.more) note("SMMU: %u more matching groups not listed", m.more);
  note("SMMU route: %s", routeVerdict(m));
}

void summarize(const Clocks &c, const Core &r, const SmmuSurvey &m, char *out, size_t size)
{
  const CoreState s = coreState(c, r);
  const uint64_t core = c.ccRead ? mvs0Hz(c) : 0;
  snprintf(out, size, "gcc ahb %s axi0 %s axi1 %s; %s; mvs0c %s mvs0 %s; pll0 %s %llu MHz; core %s; smmu 0x1940 %s 0x1947 %s free %u",
           onOff(c.gccAhb), onOff(c.gccAxi0), onOff(c.gccAxi1), c.ccRead ? "videocc read" : "videocc not read",
           c.ccRead ? (gdscOn(c.mvs0cGdsc) ? "on" : "off") : "?", c.ccRead ? (gdscOn(c.mvs0Gdsc) ? "on" : "off") : "?",
           c.ccRead ? (pllRunning(c) ? "running" : "off") : "?", (unsigned long long)(core / 1000000), coreStateName(s),
           streamWord(m, kStreamFirst), streamWord(m, kStreamSecond), m.freeGroups);
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoVideo : public IOService {
  OSDeclareDefaultStructors(PianoVideo)
  void run();
  static void thread(void *self, wait_result_t);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoVideo, IOService)

IOService *PianoVideo::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoVideo::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-video=%u cc=%u smmu=%u carveout=%u delay=%u repeat=%u samples=%u (Iris %#llx, VIDEO_CC %#llx, SPI %u, streams "
       "%#x/%#x, carveout %#llx)",
       gArgs.level, gArgs.cc, gArgs.smmu, gArgs.carveout, gArgs.delay, gArgs.repeat, gArgs.samples, (unsigned long long)kIris,
       (unsigned long long)kVideoCc, kIrisSpi, kStreamFirst, kStreamSecond, (unsigned long long)kCarveout);
  if (gArgs.level > 1) note("piano-video=%u: only level 1 exists (docs/drivers/video.md section 8); running level 1", gArgs.level);
  note("level 1: loads only, nothing written");
  setProperty("piano-video", gArgs.level, 32);
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoVideo::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;
  }
  thread_deallocate(t);
  return true;
}

void PianoVideo::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-video");
  static_cast<PianoVideo *>(self)->run();
}

void PianoVideo::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  unsigned loads = 0;
  stage(1, "GCC video branches, then VIDEO_CC behind GCC's video AHB (read only)");
  const Clocks c = surveyClocks(load, gArgs.cc, &loads);
  reportGcc(c);
  if (c.ccRead) reportVideoCc(c);
  note("gate: %s", c.why);
  Core r;
  if (c.controller) {
    stage(2, "the core's controller registers (and the codec's idle words when its domain is on), read only");
    r = surveyCore(c, load, &loads);
  } else {
    stage(2, "the core skipped: its gate is closed");
  }
  reportCore(c, r);
  const CoreState s = coreState(c, r);
  note("core: %s: %s", coreStateName(s), coreVerdict(s));
  SmmuSurvey m;
  if (gArgs.smmu) {
    stage(3, gArgs.smmu >= 2 ? "SMMU streams 0x1940-0x194f and their banks (read only)" : "SMMU streams 0x1940-0x194f (global pages, read only)");
    m = surveySmmu(gArgs.smmu, [](uint32_t off) { return load(kSmmu + off); });
    reportSmmu(m);
  }
  Carveout fw;
  if (gArgs.carveout) {
    stage(4, "the firmware carveout's first words (read only; memory XNU does not own)");
    fw = surveyCarveout(load, &loads);
    note("carveout %#llx: %08x %08x %08x %08x %08x %08x %08x %08x: %s", (unsigned long long)kCarveout, fw.words[0], fw.words[1],
         fw.words[2], fw.words[3], fw.words[4], fw.words[5], fw.words[6], fw.words[7], carveoutVerdict(fw));
  }
  char summary[240];
  summarize(c, r, m, summary, sizeof summary);
  setProperty("piano-video-survey", summary);
  note("summary: %s", summary);
  for (uint32_t i = 1; gArgs.repeat && i <= gArgs.samples; ++i) {
    IOSleep(gArgs.repeat * 1000);
    unsigned again = 0;
    const Clocks k = surveyClocks(load, gArgs.cc, &again);
    note("sample %u (+%u s): gcc ahb %08x axi0 %08x axi1 %08x; mvs0c gdsc %08x mvs0 gdsc %08x pll0 mode %08x mvs0 %08x", i, i * gArgs.repeat,
         k.gccAhb, k.gccAxi0, k.gccAxi1, k.mvs0cGdsc, k.mvs0Gdsc, k.pllMode, k.mvs0);
  }
  stage(5, "done");
  note("done: %llu MMIO loads, 0 stores; %u pages mapped, %u map failures", (unsigned long long)gLoads, gPageCount, gMapFailures);
  unmapAll();
}
