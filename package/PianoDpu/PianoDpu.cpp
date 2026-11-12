// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu: piano's display processor (SM8750 MDSS / DPU 12.0, two bonded DSI 6G controllers, the NT36532 panel) for macOS 27, native
// at EL1. Design, the staged plan (zero-copy flips, refresh switching, Night Light, panel off/on) and the test plan:
// docs/drivers/dpu.md. Facts and their sources: DpuHw.h. This is level 1 only:
//   piano-dpu=1  a read-only survey of what the boot stages left driving the panel, logged as "PianoDpu: ..." lines: the display
//                clocks (the gate: nothing of MDSS is loaded while its AHB or core clock is off), every pipe's source address,
//                size, position, stride and format, the CTL paths, the layer mixers' blend stages, the DSPPs' PCC/GC enables and
//                matrix, MERGE_3D, the DSC encoders, INTF1/INTF2's timing and counters, both DSI controllers' mode and timing,
//                the panel ID pin, the apps SMMU's entries for the DPU's streams 0x800-0x803 and their context bank; then the
//                refresh, measured from INTF1/INTF2's frame counters read twice; then the path and what a zero-copy flip would
//                write. Nothing is written: every MMIO access of this level is a load (DpuSurvey.h has no store at all).
// Other arguments: piano-dpu-smmu=N (0 skip the SMMU, 1 its global pages only, 2 (default) also the bank's registers, 3 also walk
// the bank's own translation of the splash framebuffer in the boot loader's page tables: loads from memory XNU does not own),
// piano-dpu-measure=MS (the counter interval, default 1000, 20..10000), piano-dpu-delay=S (wait S s first, default 0),
// piano-dpu-repeat=S with piano-dpu-samples=N (re-measure every S s, N times, default 3: one line each, to see a change after
// WindowServer starts or the display sleeps). Without piano-dpu the driver declines in probe() and maps nothing. NativeGopDisplay is
// not touched: this driver never registers a display.
//   piano-dpu-ahb-vote=N (default 0 at level 1: it stores nothing at all; 1 at level 2, since 2026-11-12) the GCC display AHB branch
//                before the survey (DpuAhbVote.h): 1 its
//                CLK_ENABLE set if clear (under PianoA830's GCC vote lock) and the gate's GCC check by Linux's rule (enabled and
//                hardware-gated counts as on), 2 also its HW_CTL cleared (forced on, CLK_OFF must read clear); the only stores of
//                level 1, all to GCC_DISP_AHB_CBCR, undone after the survey unless a later stage reads MDSS.
//   piano-dpu=2  level 1, then B1b, the zero-copy flips (DpuFlip.cpp: piano-dpu-flip=N, the ladder of docs/drivers/dpu.md section 8);
//                the service is registered so that NativeGopDisplay (native-gop-display-flip=1) finds its "PianoDpuFlip" function.
// The writing slices B1c (piano-dpu-refresh), B1d (piano-dpu-pcc) and B1e (piano-dpu-panel) are DpuSlices.cpp's: their own thread,
// page maps and store guard, started here after the survey's thread and waiting for it (at any level >= 1); their platform functions
// come through callPlatformFunction below. Without their arguments nothing of them runs.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "DpuSurvey.h"
#include "DpuAhbVote.h"
#include "DpuFlipKernel.h"

using namespace pianodpu;

// The writing slices (DpuSlices.cpp).
bool dpuSlicesWanted();
void dpuSlicesStart(IOService *dpu);
bool dpuSlicesPlatformFunction(const OSSymbol *name, void *p1, void *p2, void *p3, IOReturn *result);

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_dpu_stage = 0;
// PianoA830's lock word of the GCC clock registers (0 free, else the holder's tag); PianoA830 links first and is a library of
// PianoDpu.json. Taken only by piano-dpu-ahb-vote.
extern volatile uint32_t sm8750_gcc_vote_owner;
}

namespace {

struct Args {
  uint32_t level, smmu, measure, delay, repeat, samples, ahbVote;
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
  gArgs.level = bootArg("piano-dpu", 0);
  gArgs.smmu = bootArg("piano-dpu-smmu", 2);
  if (gArgs.smmu > 3) gArgs.smmu = 3;
  gArgs.measure = bootArg("piano-dpu-measure", 1000);
  if (gArgs.measure < 20) gArgs.measure = 20;
  if (gArgs.measure > 10000) gArgs.measure = 10000;
  gArgs.delay = bootArg("piano-dpu-delay", 0);
  if (gArgs.delay > 600) gArgs.delay = 600;
  gArgs.repeat = bootArg("piano-dpu-repeat", 0);
  if (gArgs.repeat > 3600) gArgs.repeat = 3600;
  gArgs.samples = bootArg("piano-dpu-samples", 3);
  if (gArgs.samples > 100) gArgs.samples = 100;
  // B1b's levels (piano-dpu=2) need the survey's path, which needs the gate open: the AHB vote's rule 1 by default there (2026-11-12:
  // dpu-ahb1 passed with it; it writes nothing while CLK_ENABLE reads set, as on the tablet). Level 1 keeps 0, loads only.
  gArgs.ahbVote = bootArg("piano-dpu-ahb-vote", gArgs.level >= 2 ? 1 : 0);
  if (gArgs.ahbVote > 2) gArgs.ahbVote = 0;   // an unknown value writes nothing
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[480];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoDpu: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_dpu_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

// Explicit ldr, as PianoAudio, PianoPower and PianoWifi do. The one store helper below (gccStore) serves piano-dpu-ahb-vote only.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}

// ------------------------------------------------------------------------------------------------ MMIO pages, mapped on demand
// PianoAudio's scheme: a page is mapped (device memory, uncached) the first time one of its words is loaded. The survey touches
// about 60 pages: GCC, DISPCC, MDSS, the DPU blocks, both DSI controllers, one TLMM pin, the SMMU's global pages and one bank.
struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
constexpr unsigned kMaxPages = 96;
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

uint64_t load64(uint64_t pa) { return uint64_t(load(pa)) | uint64_t(load(pa + 4)) << 32; }

// piano-dpu-ahb-vote's store (DpuAhbVote.h): GCC_DISP_AHB_CBCR only, into the page its load mapped, never a new mapping.
uint64_t gStores;
void gccStore(uint64_t pa, uint32_t v)
{
  if (pa != kGcc + kGccDispAhb) return;
  const uint64_t base = pa & ~0xfffull;
  for (unsigned i = 0; i < gPageCount; ++i)
    if (gPages[i].pa == base) {
      __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(gPages[i].va + uintptr_t(pa & 0xfff)) : "memory");
      __builtin_arm_dsb(0xf);
      ++gStores;
      return;
    }
}

// PianoA830's lock word, PianoTouch's protocol: tried every 10 us, at most 50 ms; not taken: nothing written.
struct GccLock {
  bool take(unsigned *tries)
  {
    for (unsigned n = 1; n <= kGccLockTries; ++n) {
      *tries = n;
      uint32_t expected = 0;
      if (__atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, kDpuGccVoteTag, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return true;
      IODelay(10);
    }
    return false;
  }
  void give()
  {
    uint32_t expected = kDpuGccVoteTag;
    __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, 0u, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  }
};

void unmapAll()
{
  for (unsigned i = 0; i < gPageCount; ++i) gPages[i].map->release();
  gPageCount = 0;
}

uint64_t nowNs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns;
}

// ------------------------------------------------------------------------------------------------------------- the report
const char *intfName(unsigned i)
{
  static const char *const names[kIntfCount] = {"INTF0 (DP)", "INTF1 (DSI0)", "INTF2 (DSI1)", "INTF3 (DP)"};
  return i < kIntfCount ? names[i] : "?";
}

// "VIG0+VIG1" from a kPipes bit set ("none" when empty).
void pipeList(unsigned bits, char *out, size_t size)
{
  size_t n = 0;
  out[0] = 0;
  for (unsigned i = 0; i < kPipeCount && n < size; ++i)
    if ((bits >> i) & 1u) n += size_t(snprintf(out + n, size - n, "%s%s", n ? "+" : "", kPipes[i].name));
  if (!n) snprintf(out, size, "none");
}
void indexList(unsigned bits, const char *prefix, unsigned count, char *out, size_t size)
{
  size_t n = 0;
  out[0] = 0;
  for (unsigned i = 0; i < count && n < size; ++i)
    if ((bits >> i) & 1u) n += size_t(snprintf(out + n, size - n, "%s%s%u", n ? "+" : "", prefix, i));
  if (!n) snprintf(out, size, "none");
}

// Three lines (the console cuts a line at 255 characters: before 2026-11-12 one line lost the MDP RCG's source and divider).
void reportClocks(const ClockGate &g)
{
  const unsigned src = (g.rcgCfg >> 8) & 7u, div = g.rcgCfg & 0x1fu;
  note("clocks: gcc disp ahb %08x (enable %u, hw gating %u, clk-off %u: %s) hf-axi %08x (clk-off %u)", g.gccDispAhb,
       g.gccDispAhb & kCbcrEnable, (g.gccDispAhb & kCbcrHwCtl) >> 1, g.gccDispAhb >> 31,
       branchOn(g.gccDispAhb) ? "running" : branchHwGated(g.gccDispAhb) ? "hardware-gated, idle" : "off", g.gccDispAxi, g.gccDispAxi >> 31);
  note("clocks: dispcc mdss gdsc %08x (%s) ahb %08x non-gdsc-ahb %08x mdp %08x vsync %08x pclk %08x/%08x byte %08x/%08x esc0 %08x",
       g.gdsc, gdscOn(g.gdsc) ? "on" : "off", g.ahb, g.nonGdscAhb, g.mdp, g.vsync, g.pclk[0], g.pclk[1], g.byte[0], g.byte[1], g.esc0);
  note("clocks: mdp rcg cmd %08x cfg %08x (source %u, divider %u.%u)", g.rcgCmd, g.rcgCfg, src, div ? (div + 1) / 2 : 1,
       div && !(div & 1u) ? 5 : 0);
  note("gate: %s", g.why);
}

void reportVote(const AhbVote &v)
{
  if (v.lockFailed)
    note("ahb vote %u: GCC_DISP_AHB_CBCR %08x: the GCC vote lock stayed taken (%u tries, holder %08x): nothing written", v.mode, v.found,
         v.lockTries, __atomic_load_n(&sm8750_gcc_vote_owner, __ATOMIC_ACQUIRE));
  else if (v.wrote)
    note("ahb vote %u: GCC_DISP_AHB_CBCR %08x -> wrote %08x (%s%s) under the GCC vote lock (%u tries); now %08x after %u us: %s",
         v.mode, v.found, v.written, v.setEnable ? "enable set" : "", v.clearedHwCtl ? (v.setEnable ? ", hw gating off" : "hw gating off") : "",
         v.lockTries, v.after, v.waitedUs, v.on ? "passes the gate's rule" : "does NOT pass the gate's rule");
  else
    note("ahb vote %u: GCC_DISP_AHB_CBCR %08x: %s; nothing written; %s", v.mode, v.found,
         v.found == kUnreadable ? "not readable" : v.mode >= 2 ? "already enabled, hw gating off" : "already enabled (the boot's vote)",
         v.on ? (v.mode == 1 && !branchOn(v.after) ? "enabled and hardware-gated: on by Linux's rule" : "running") : "does NOT pass the gate's rule");
}

void reportTop(const Survey &s)
{
  const TopRegs &t = s.top;
  note("mdss hw version %08x (%u.%u.%u) intr status %08x; dpu top intf-sel %08x intr en %08x status %08x intr2 en %08x status %08x "
       "split %08x/%08x/%08x vsync-sel %08x dce-sel %08x dp-phy-intf-sel %08x danger %08x safe %08x",
       s.mdssVersion, s.mdssVersion >> 28, (s.mdssVersion >> 16) & 0xfffu, s.mdssVersion & 0xffffu, s.mdssIntr, t.intfSel, t.intrEn,
       t.intrStatus, t.intr2En, t.intr2Status, t.splitEn, t.splitUpper, t.splitLower, t.vsyncSel, t.dceSel, t.dpPhyIntfSel,
       t.danger, t.safe);
  char en[64];
  size_t n = 0;
  en[0] = 0;
  for (unsigned i = 0; i < kIntfCount && n < sizeof en; ++i)
    if ((t.intrEn >> kIntrIntfVsync[i]) & 1u) n += size_t(snprintf(en + n, sizeof en - n, "%sINTF%u", n ? "+" : "", i));
  note("vsync interrupts enabled at the DPU: %s (INTF1 bit 27, INTF2 bit 29; status now %s/%s)", n ? en : "none",
       (t.intrStatus >> 27) & 1u ? "set" : "clear", (t.intrStatus >> 29) & 1u ? "set" : "clear");
}

void reportPipes(const Survey &s)
{
  unsigned idle = 0;
  for (unsigned i = 0; i < kPipeCount; ++i) {
    const PipeRegs &p = s.pipes[i];
    const PipeView v = viewPipe(p);
    if (!v.configured) {
      idle |= 1u << i;
      continue;
    }
    for (unsigned r = 0; r < (v.rect1 ? 2u : 1u); ++r) {
      char where[48] = "";
      if (v.fromSplash[r] && v.origin[r].valid)
        snprintf(where, sizeof where, " [splash at %u,%u]", v.origin[r].x, v.origin[r].y);
      else if (!v.fromSplash[r])
        snprintf(where, sizeof where, " [not the splash]");
      const Format &f = v.fmt[r];
      note("pipe %s rect %u: %ux%u @%u,%u -> %ux%u @%u,%u addr %08x stride %u format %08x unpack %08x '%s' %u%u%u%u %u B %s%s%s%s",
           kPipes[i].name, r, v.src[r].w, v.src[r].h, v.srcPos[r].w, v.srcPos[r].h, v.out[r].w, v.out[r].h, v.outPos[r].w,
           v.outPos[r].h, v.addr[r], v.stride[r], r ? p.format1 : p.format, r ? p.unpack1 : p.unpack, f.order, f.bits[2], f.bits[0],
           f.bits[1], f.bits[3], f.bpp, frameFormatName(f.frameFormat), f.alpha ? " alpha" : " opaque",
           flippableBgra(f) ? " flippable" : "", where);
    }
    note("pipe %s: multirect %08x (%s) op %08x/%08x src1 %08x src2 %08x src3 %08x ystride1 %08x const %08x fetch %08x ubwc %08x "
         "ubwc-err %08x qos %08x sw-status %08x",
         kPipes[i].name, p.multirect, v.rect1 ? (v.timeMux ? "2 rects, time-multiplexed" : "2 rects, parallel") : "1 rect",
         p.opMode, p.opMode1, p.addr[1], p.addr[2], p.addr[3], p.ystride[1], p.constColor, p.fetchConfig, p.ubwcStatic, p.ubwcError,
         p.qos, p.swStatus);
  }
  char list[96];
  pipeList(idle, list, sizeof list);
  note("pipes idle (no size or address): %s", list);
}

void reportCtls(const Survey &s)
{
  char unused[32];
  size_t un = 0;
  unused[0] = 0;
  for (unsigned c = 0; c < kCtlCount; ++c) {
    const CtlRegs &r = s.ctls[c];
    const CtlView v = viewCtl(r);
    if (!v.used && !r.pipeActive && !r.layerActive) {
      un += size_t(snprintf(unused + un, sizeof unused - un, "%s%u", un ? " " : "", c));
      continue;
    }
    char intfs[48], lms[48], pipes[96], fetch[96];
    indexList(v.intfs, "INTF", kIntfCount, intfs, sizeof intfs);
    indexList(v.lms, "LM", kLmCount, lms, sizeof lms);
    pipeList(v.pipes, pipes, sizeof pipes);
    pipeList(v.fetch, fetch, sizeof fetch);
    note("ctl%u: %s mode, group %u, intf %s (master %08x), lm %s, pipes %s (fetch %s), dsc %02x, merge3d %02x, wb %02x cwb %02x "
         "cdm %02x; top %08x flush %08x start %08x prepare %08x intf-flush %08x dsc-flush %08x merge3d-flush %08x periph-flush %08x "
         "dspp-flush %08x %08x %08x %08x",
         c, v.video ? "video" : "command", v.group, intfs, r.intfMaster, lms, pipes, fetch, v.dsc, v.merge3d, r.wbActive, r.cwbActive,
         r.cdmActive, r.top, r.flush, r.start, r.prepare, r.intfFlush, r.dscFlush, r.merge3dFlush, r.periphFlush, r.dsppFlush[0],
         r.dsppFlush[1], r.dsppFlush[2], r.dsppFlush[3]);
  }
  note("ctls unused: %s", un ? unused : "none");
}

void reportLms(const Survey &s)
{
  for (unsigned l = 0; l < kLmCount; ++l) {
    const LmRegs &r = s.lms[l];
    const LmView v = viewLm(r);
    if (!v.stagedCount && !r.outSize) continue;
    char stages[200];
    size_t n = 0;
    stages[0] = 0;
    for (unsigned st = 0; st < kLmStages && n < sizeof stages; ++st)
      for (unsigned k = 0; k < 2 && n < sizeof stages; ++k) {
        unsigned rect = 0;
        const int p = stagedPipe(r.src[st], k, &rect);
        if (p < 0) continue;
        n += size_t(snprintf(stages + n, sizeof stages - n, "%sstage %u: %s rect %u (op %08x alpha %08x)", n ? ", " : "", st,
                             kPipes[p].name, rect, r.op[st], r.alpha[st]));
      }
    note("lm%u: out %ux%u%s op %08x bg-src %08x border %08x/%08x; %s", l, v.out.w, v.out.h, v.right ? " (right of a split pair)" : "",
         r.opMode, r.bgSrcSel, r.border0, r.border1, n ? stages : "nothing staged");
  }
}

void reportDspps(const Survey &s)
{
  for (unsigned i = 0; i < kDsppCount; ++i) {
    const DsppRegs &d = s.dspps[i];
    const PccView p = viewPcc(d);
    note("dspp%u: pcc %s%s [%d %d %d | %d %d %d | %d %d %d] (thousandths; raw en %08x c %08x %08x %08x); gc %s (en %08x swap %08x)",
         i, p.enabled ? "on" : "off", p.identity ? " identity" : "", p.m[0][0], p.m[0][1], p.m[0][2], p.m[1][0], p.m[1][1], p.m[1][2],
         p.m[2][0], p.m[2][1], p.m[2][2], d.pcc[0], d.pcc[1], d.pcc[2], d.pcc[3], (d.gcEnable & 1u) ? "on" : "off", d.gcEnable,
         d.gcSwap);
  }
}

void reportDsc(const Survey &s)
{
  note("merge3d: %08x/%08x %08x/%08x %08x/%08x %08x/%08x (mux/mode)", s.merge3d[0][0], s.merge3d[0][1], s.merge3d[1][0],
       s.merge3d[1][1], s.merge3d[2][0], s.merge3d[2][1], s.merge3d[3][0], s.merge3d[3][1]);
  for (unsigned i = 0; i < kDscCount; ++i) {
    const DscRegs &d = s.dscs[i];
    if (!d.mainCnf && !d.mainConf[0] && !d.mainConf[1] && (d.ctl[0] & 0xfu) == 0xfu && (d.ctl[1] & 0xfu) == 0xfu) continue;
    for (unsigned e = 0; e < 2; ++e) {
      const Size pic = size(d.picSize[e]), slice = size(d.sliceSize[e]);
      const unsigned bpp16 = (d.mainConf[e] >> 10) & 0x3ffu;
      note("dsc%u enc%u: main-cnf %08x df %08x (%s) conf %08x (bpc %u, bpp %u.%u, block-pred %s, v1.%u) picture %ux%u slice %ux%u, "
           "input %s (ctl %08x cfg %08x)",
           i, e, d.mainCnf, d.dfCtrl[e], (d.dfCtrl[e] >> 9) & 1u ? "video" : "command", d.mainConf[e], d.mainConf[e] & 0xfu,
           bpp16 >> 4, (bpp16 & 0xfu) * 10 / 16, (d.mainConf[e] >> 20) & 1u ? "on" : "off", d.mainConf[e] >> 28, pic.w, pic.h,
           slice.w, slice.h, (d.ctl[e] & 0xfu) == 0xfu ? "none" : "pingpong", d.ctl[e], d.cfg[e]);
    }
  }
}

void reportIntfs(const Survey &s)
{
  note("intf0 and intf3 (DP): not read (nothing drives DP)");
  for (unsigned i = 0; i < kIntfCount; ++i) {
    if (!intfSurveyed(i)) continue;
    const IntfRegs &f = s.intfs[i];
    const IntfTiming t = viewIntf(f);
    if (!t.enabled && !f.hsyncCtl) {
      note("intf%u %s: timing engine off, no timing", i, intfName(i));
      continue;
    }
    note("intf%u %s: engine %s; htotal %u (hsync %u hbp %u active %u hfp %u) vtotal %u%s (vsync %u vbp %u active %u vfp %u); "
         "data width %u, wide bus %u, dsc %u; counters frame %s line %s: frame %u line %u",
         i, intfName(i), t.enabled ? "on" : "off", t.htotal, t.hsync, t.hbp, t.hactive, t.hfp, t.vtotal, t.exact ? "" : " (inexact)",
         t.vsync, t.vbp, t.vactive, t.vfp, t.dataWidth, t.wideBus, t.compressed, t.frameCounter ? "on" : "off",
         t.lineCounter ? "on" : "off", f.frameCount, f.lineCount);
    note("intf%u raw: config %08x hsync %08x period %08x vwidth %08x dv %08x-%08x av %08x-%08x dhctl %08x ahctl %08x skew %08x "
         "polarity %08x config2 %08x data-hctl %08x panel-format %08x prog-fetch %08x mux %08x status %08x avr %08x intr en %08x "
         "status %08x",
         i, f.config, f.hsyncCtl, f.vsyncPeriod, f.vsyncWidth, f.dispVStart, f.dispVEnd, f.actVStart, f.actVEnd, f.dispHctl, f.actHctl,
         f.skew, f.polarity, f.config2, f.dataHctl, f.panelFormat, f.progFetch, f.mux, f.status, f.avr, f.intrEn, f.intrStatus);
  }
}

void reportDsi(const Survey &s)
{
  const bool bonded = viewDsi(s.dsi[0]).enabled && viewDsi(s.dsi[1]).enabled;
  for (unsigned i = 0; i < 2; ++i) {
    if (!s.dsiRead[i]) {
      note("dsi%u: not read (its byte clock %08x or pixel clock %08x is off)", i, s.clocks.byte[i], s.clocks.pclk[i]);
      continue;
    }
    const DsiRegs &d = s.dsi[i];
    const DsiTiming t = viewDsi(d);
    const int m = matchMode(t, bonded);
    char mode[96];
    if (m >= 0)
      snprintf(mode, sizeof mode, "Ubuntu's %u Hz mode (full-panel HFP %u, vtotal %u)", kModes[m].hz, kModes[m].hfp,
               modeVtotal(kModes[m]));
    else
      snprintf(mode, sizeof mode, "no Ubuntu mode (full-panel HFP %u)", bonded ? 2 * t.hfp : t.hfp);
    note("dsi%u: ctrl %08x (%s, %s%s, %u lanes%s) hw %08x version %08x; per-link htotal %u (hsync %u hbp %u active %u hfp %u) vtotal %u "
         "(vsync %u vbp %u active %u vfp %u): %s; compression %s wc %u",
         i, d.ctrl, t.enabled ? "on" : "off", t.video ? "video" : "", t.command ? " command" : "", t.lanes,
         t.clockLane ? ", clock lane" : "", d.hwVersion, d.version, t.htotal, t.hsync, t.hbp, t.hactive, t.hfp, t.vtotal, t.vsync, t.vbp,
         t.vactive, t.vfp, mode, t.compression ? "on" : "off", t.compressWc);
    note("dsi%u raw: status0 %08x fifo %08x vid-cfg0 %08x vid-cfg1 %08x active-h %08x active-v %08x total %08x hsync %08x vsync %08x/%08x "
         "lane status %08x lane ctrl %08x err-mask %08x intr %08x clk ctrl %08x status %08x timing words %08x %08x %08x",
         i, d.status0, d.fifoStatus, d.vidCfg0, d.vidCfg1, d.activeH, d.activeV, d.total, d.activeHsync, d.vsyncHpos, d.vsyncVpos,
         d.laneStatus, d.laneCtrl, d.errMask, d.intrCtrl, d.clkCtrl, d.clkStatus, d.timing[0], d.timing[1], d.timing[2]);
  }
}

void reportSmmu(const SmmuSurvey &m)
{
  note("SMMU scr0 %08x idr0 %08x idr1 %08x: %u stream-match groups, %u banks, %u pages%s%s", m.scr0, m.idr0, m.idr1, m.groups, m.banks,
       m.pages, m.error ? ": " : "", m.error ? m.error : "");
  if (m.error) return;
  if (!m.count) note("SMMU streams 0x800-0x803: no stream-match entry");
  for (unsigned i = 0; i < m.count; ++i) {
    const SmmuEntry &e = m.found[i];
    note("SMMU group %u smr %08x (id %#x mask %#x) s2cr %08x (%s, bank %u) cbar %08x (%s) cba2r %08x (%s)", e.group, e.smr,
         e.smr & 0x7fffu, (e.smr >> 16) & 0x7fffu, e.s2cr, s2crType(e.s2cr), e.s2cr & 0xffu, e.bank.cbar, cbarType(e.bank.cbar),
         e.bank.cba2r, (e.bank.cba2r & 1u) ? "AArch64 tables" : "AArch32 tables");
    if (!e.bank.read) continue;
    const SmmuBank &b = e.bank;
    note("SMMU bank %u: sctlr %08x (translation %s, fault report %s/%s) tcr %08x (T0SZ %u: %u-bit IOVA, granule %u KiB, EPD1 %u) "
         "tcr2 %08x (PASize %u, SEP %u) ttbr0 %08x%08x (table %#llx, ASID %u) mair0 %08x fsr %08x",
         e.s2cr & 0xffu, b.sctlr, (b.sctlr & 1u) ? "on" : "off", (b.sctlr >> 5) & 1u ? "on" : "off", (b.sctlr >> 6) & 1u ? "irq" : "no irq",
         b.tcr, tcrT0sz(b.tcr), 64 - tcrT0sz(b.tcr), tcrGranuleKiB(b.tcr), (b.tcr >> 23) & 1u, b.tcr2, b.tcr2 & 7u, (b.tcr2 >> 15) & 7u,
         b.ttbr0hi, b.ttbr0lo, (unsigned long long)ttbr0Base(b), b.ttbr0hi >> 16, b.mair0, b.fsr);
  }
  if (m.more) note("SMMU: %u more matching groups not listed", m.more);
}

// smmu level 3: the bank's own translation of the splash (first and last page) and of 0, 1, 2 and 3 GiB.
void reportWalk(const SmmuSurvey &m)
{
  const SmmuEntry *e = nullptr;
  for (unsigned i = 0; i < m.count && !e; ++i)
    if (smrCovers(m.found[i].smr, kStreamDpu) && m.found[i].bank.read) e = &m.found[i];
  if (!e || !(e->bank.sctlr & 1u)) {
    note("walk: skipped (%s)", !e ? "no translating bank read for 0x800" : "the bank's translation is off: IOVA = PA");
    return;
  }
  const uint64_t table = ttbr0Base(e->bank);
  if (table >= (1ull << 40)) {
    note("walk: skipped (table %#llx out of range)", (unsigned long long)table);
    return;
  }
  const uint64_t iovas[] = {kSplash, kSplash + kSplashBytes - 0x1000, 0x0, 0x40000000, 0x80000000, 0xc0000000};
  for (uint64_t iova : iovas) {
    const Walk w = walk(e->bank, iova, load64);
    note("walk: iova %#llx -> %s %#llx (level %u, %u loads: %016llx %016llx %016llx %016llx)%s%s", (unsigned long long)iova,
         w.ok ? "pa" : "no translation at", (unsigned long long)(w.ok ? w.pa : 0), w.level, w.loads,
         (unsigned long long)w.desc[0], (unsigned long long)w.desc[1], (unsigned long long)w.desc[2], (unsigned long long)w.desc[3],
         w.error ? ": " : (w.ok && w.pa == iova ? " (identity)" : ""), w.error ? w.error : "");
  }
}

struct Refresh {
  uint64_t milliHz[2] = {0, 0}, dtNs = 0;
  Counters a, b;
};
Refresh measure(uint32_t ms)
{
  Refresh r;
  const uint64_t t0 = nowNs();
  r.a = readCounters(load);
  IOSleep(ms);
  r.b = readCounters(load);
  r.dtNs = nowNs() - t0;
  for (unsigned i = 0; i < 2; ++i) r.milliHz[i] = refreshMilliHz(r.a.frame[i], r.b.frame[i], r.dtNs);
  return r;
}

void reportRefresh(const Survey &s, const Refresh &r)
{
  for (unsigned i = 0; i < 2; ++i) {
    const uint32_t period = s.intfs[i + 1].vsyncPeriod;
    if (r.milliHz[i])
      note("refresh: intf%u frames %u -> %u in %llu us: %llu.%03llu Hz (INTF pixel clock %llu kHz); line %u -> %u", i + 1,
           r.a.frame[i], r.b.frame[i], (unsigned long long)(r.dtNs / 1000), (unsigned long long)(r.milliHz[i] / 1000),
           (unsigned long long)(r.milliHz[i] % 1000), (unsigned long long)intfPixelKHz(r.milliHz[i], period), r.a.line[i], r.b.line[i]);
    else
      note("refresh: intf%u frame counter did not move (%u -> %u, counter %s); line %u -> %u: not measurable read-only", i + 1,
           r.a.frame[i], r.b.frame[i], viewIntf(s.intfs[i + 1]).frameCounter ? "enabled" : "disabled", r.a.line[i], r.b.line[i]);
  }
}

// The path and what a zero-copy flip would write; one summary line for ioreg.
void reportPath(const Survey &s, const SmmuSurvey &m, const Refresh &r, char *summary, size_t size)
{
  const Path p = viewPath(s);
  char pipes[96], intfs[48], lms[48], dsc[16];
  pipeList(p.splashPipes, pipes, sizeof pipes);
  indexList(p.view.intfs, "INTF", kIntfCount, intfs, sizeof intfs);
  indexList(p.view.lms, "LM", kLmCount, lms, sizeof lms);
  snprintf(dsc, sizeof dsc, "%02x", p.view.dsc);
  const uint64_t hz = r.milliHz[0] ? r.milliHz[0] : r.milliHz[1];
  if (p.ctl < 0)
    note("path: no CTL drives INTF1/INTF2 (pipes from the splash: %s)", pipes);
  else
    note("path: %s (%u rects) from the splash, stride %u -> ctl%d (%s, %u CTLs on the DSI INTFs) -> %s (staged: %s) -> dsc %s -> %s%s; "
         "%llu.%03llu Hz measured",
         pipes, p.splashRects, p.stride, p.ctl, p.view.video ? "video" : "command", p.ctlCount, lms, p.stagedOk ? "yes" : "no", dsc, intfs,
         p.dualDsi ? " (dual DSI)" : "", (unsigned long long)(hz / 1000), (unsigned long long)(hz % 1000));
  const Route rt = route(m);
  note("zero-copy: flip = the SRC address of %s (both rects when multirect) + ctl%d FLUSH %08x (no START in video mode); format %s%s",
       pipes, p.ctl, flipFlushMask(p.splashPipes), p.flippable ? "takes a 'BGRA' IOSurface as is" : "needs more than an address change",
       p.alpha ? " (per-pixel alpha on: the flip clears SRCC3_EN or WindowServer's alpha shows)" : "");
  note("zero-copy route: %s", routeVerdict(rt));
  const char *routeWord = rt == Route::bankOff ? "bank-off" : rt == Route::bankOn ? "bank-on" : rt == Route::bypass ? "bypass"
                          : rt == Route::none ? "none" : rt == Route::unread ? "unread" : "other";
  unsigned bank = 0xff;
  for (unsigned i = 0; i < m.count; ++i)
    if (smrCovers(m.found[i].smr, kStreamDpu)) {
      bank = m.found[i].s2cr & 0xffu;
      break;
    }
  snprintf(summary, size, "pipes %s stride %u ctl%d lm %s intf %s dsc %s %s %llu.%03llu Hz smmu %s bank %u panel %s",
           pipes, p.stride, p.ctl, lms, intfs, dsc, p.view.video ? "video" : "command", (unsigned long long)(hz / 1000),
           (unsigned long long)(hz % 1000), routeWord, bank, panelModule(s.panelIdIo));
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoDpu : public IOService {
  OSDeclareDefaultStructors(PianoDpu)
  void run();
  static void thread(void *self, wait_result_t);

 public:
  using IOService::callPlatformFunction;
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) override;
};

OSDefineMetaClassAndStructors(PianoDpu, IOService)

IOService *PianoDpu::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoDpu::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-dpu=%u smmu=%u measure=%u ms delay=%u repeat=%u samples=%u (MDSS %#llx, DPU %#llx, DSI %#llx/%#llx)", gArgs.level,
       gArgs.smmu, gArgs.measure, gArgs.delay, gArgs.repeat, gArgs.samples, (unsigned long long)kMdss, (unsigned long long)kMdp,
       (unsigned long long)kDsi[0], (unsigned long long)kDsi[1]);
  if (gArgs.level > 2) note("piano-dpu=%u: levels 1 and 2 exist (docs/drivers/dpu.md section 8); running level 2", gArgs.level);
  if (gArgs.ahbVote)
    note("level 1: loads only, but piano-dpu-ahb-vote=%u: GCC_DISP_AHB_CBCR (GCC %#x) %s, under the GCC vote lock, before the survey%s",
         gArgs.ahbVote, kGccDispAhb, gArgs.ahbVote >= 2 ? "enable set and hw gating off if not so" : "enable set if clear",
         gArgs.level >= 2 ? "; then B1b (piano-dpu=2): the flips of piano-dpu-flip" : "");
  else note("level 1: loads only, nothing written%s", gArgs.level >= 2 ? "; then B1b (piano-dpu=2): the flips of piano-dpu-flip" : "");
  setProperty("piano-dpu", gArgs.level, 32);
  if (gArgs.level >= 2) registerService();   // NativeGopDisplay finds the flip function by this class
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoDpu::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;
  }
  thread_deallocate(t);
  dpuSlicesStart(this);
  return true;
}

IOReturn PianoDpu::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4)
{
  IOReturn result = kIOReturnUnsupported;
  if (flipFunction(name, p1, &result)) return result;
  if (dpuSlicesPlatformFunction(name, p1, p2, p3, &result)) return result;
  return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
}

void PianoDpu::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-dpu");
  static_cast<PianoDpu *>(self)->run();
}

void PianoDpu::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  static Survey s;   // ~3 KiB: not on the thread's stack
  GccLock lock;
  AhbVote vote;
  if (gArgs.ahbVote) {
    stage(1, "clock gate: piano-dpu-ahb-vote first (GCC_DISP_AHB_CBCR, under the GCC vote lock), then GCC display AHB, DISPCC MDSS "
             "GDSC / AHB / MDP (read only)");
    vote = ahbVote(gArgs.ahbVote, load, gccStore, [](unsigned us) { IODelay(us); }, lock);
    reportVote(vote);
  } else {
    stage(1, "clock gate: GCC display AHB, DISPCC MDSS GDSC / AHB / MDP (read only)");
  }
  // piano-dpu-ahb-vote=1: GCC's display AHB branch enabled and hardware-gated counts as on (Linux's rule); 2 keeps the strict rule.
  s.clocks = surveyClocks(load, &s.loads, gArgs.ahbVote == 1);
  reportClocks(s.clocks);
  if (s.clocks.readable) {
    stage(2, "MDSS, DPU and DSI registers (read only)");
    surveyDpu(s, load);
    reportTop(s);
    reportPipes(s);
    reportCtls(s);
    reportLms(s);
    reportDspps(s);
    reportDsc(s);
    reportIntfs(s);
    reportDsi(s);
  } else {
    stage(2, "MDSS, DPU and DSI skipped: the gate is closed (docs/drivers/dpu.md section 8: the next step)");
  }
  surveyPanelId(s, load);
  note("panel id gpio%u: cfg %08x io %08x -> %s (rate-group registers %s)", kPanelIdGpio, s.panelIdCfg, s.panelIdIo,
       panelModule(s.panelIdIo), rateOrder(s.panelIdIo));
  SmmuSurvey m;
  if (gArgs.smmu) {
    stage(3, gArgs.smmu >= 3 ? "SMMU streams 0x800-0x803, their bank and its translation of the splash (read only)"
                             : "SMMU streams 0x800-0x803 and their bank (read only)");
    m = surveySmmu(gArgs.smmu, [](uint32_t off) { return load(kSmmu + off); });
    reportSmmu(m);
    if (gArgs.smmu >= 3 && !m.error) reportWalk(m);
  }
  Refresh r;
  char summary[240] = "gate closed";
  if (s.clocks.readable) {
    stage(4, "refresh: INTF1/INTF2 frame counters read twice");
    r = measure(gArgs.measure);
    reportRefresh(s, r);
    reportPath(s, m, r, summary, sizeof summary);
    if (r.milliHz[0]) setProperty("piano-dpu-refresh-mhz", r.milliHz[0], 64);
  }
  setProperty("piano-dpu-survey", summary);
  note("summary: %s", summary);
  for (uint32_t i = 1; gArgs.repeat && s.clocks.readable && i <= gArgs.samples; ++i) {
    IOSleep(gArgs.repeat * 1000);
    const Refresh again = measure(gArgs.measure);
    const Path p = viewPath(s);
    uint32_t addr[2] = {0, 0};
    unsigned k = 0;
    for (unsigned q = 0; q < kPipeCount && k < 2; ++q)
      if ((p.splashPipes >> q) & 1u) addr[k++] = load(kMdp + kPipes[q].base + kSrc0Addr);
    note("sample %u (+%u s): %llu.%03llu Hz, intf1 status %08x, splash pipes' SRC0 now %08x %08x, ctl%d flush %08x", i, i * gArgs.repeat,
         (unsigned long long)(again.milliHz[0] / 1000), (unsigned long long)(again.milliHz[0] % 1000),
         load(kMdp + kIntfBase + kIntfStride + kIntfStatus), addr[0], addr[1], p.ctl,
         p.ctl >= 0 ? load(kMdp + kCtlBase + kCtlStride * unsigned(p.ctl) + kCtlFlush) : 0);
  }
  if (vote.setEnable || vote.clearedHwCtl) {
    // B1b (piano-dpu=2) and the writing slices read MDSS after this: the vote stays for the boot; else the found bits come back.
    if (gArgs.level >= 2 || dpuSlicesWanted()) {
      note("ahb vote %u: kept for the boot (%s reads MDSS after the survey): GCC_DISP_AHB_CBCR %08x", vote.mode,
           gArgs.level >= 2 ? "B1b" : "a writing slice", load(kGcc + kGccDispAhb));
    } else {
      ahbUndo(vote, load, gccStore, lock);
      if (vote.undone) note("ahb vote %u: undone after the survey: GCC_DISP_AHB_CBCR %08x (found %08x)", vote.mode, vote.undoneTo, vote.found);
      else note("ahb vote %u: NOT undone: the GCC vote lock stayed taken; GCC_DISP_AHB_CBCR %08x stays as voted", vote.mode,
                load(kGcc + kGccDispAhb));
    }
  }
  stage(5, "done");
  note("done: %llu MMIO loads, %llu stores%s; %u pages mapped, %u map failures", (unsigned long long)gLoads,
       (unsigned long long)gStores, gStores ? " (piano-dpu-ahb-vote's, GCC_DISP_AHB_CBCR only)" : "", gPageCount, gMapFailures);
  unmapAll();
  if (gArgs.level >= 2) flipRun(this, s, m, gArgs.smmu);   // B1b: DpuFlip.cpp, its own mappings and stores
}
