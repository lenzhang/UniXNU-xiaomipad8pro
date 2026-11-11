// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDp: USB-C DisplayPort Alt Mode for macOS 27 on piano (SM8750), native at EL1. Design, ladder and test plan:
// docs/drivers/displayport.md (B8). Facts and their sources: DpHw.h. This is level 1 only, read only:
//   piano-dp=1  D1, the survey (DpSurvey.h): PianoDpu's display gate, DISPCC's DP clocks, the DP controller's blocks each only with its
//               own clocks on (version, HPD and AUX state, link, stream), the QMP PHY's DP half only with the PHY's power domain and
//               clocks on (DP_COM mode and orientation, the DP PHY's revision, power-down, mode, status), the DPU's DP side (every CTL
//               and LM, MERGE_3D, INTF0/INTF3, the TOP words) and what of it is free for a DP path, the orientation and DIO4485 pins.
//               Every MMIO access is a load. Then D2, the port events: registered with PianoPower's client contract
//               (PianoAdspContract.h, piano_port_events), every USB-C port notification the DSP sends is decoded (DpPort.h) and its
//               changes logged per port: attach, detach, DisplayPort entry and exit, pin assignment, orientation, HPD up and down,
//               IRQ_HPD, with what a plug would program (logged, never written). Event-driven: the thread sleeps until PianoPower hands
//               it a notification (PianoPower must run at level 2 for any to come). The first HPD-high re-reads the survey's gates.
// Further arguments: piano-dp-survey=0 (no survey: no MMIO at all), piano-dp-raw=N (the bytes of the first N notifications, default
// 16), piano-dp-delay=S (wait S s before the survey, default 0). D6's arguments (piano-dp-fake*, DpFake.h) are NativeGopDisplay's; this
// kext only logs the configuration they make. A level above 1 is not built yet and acts as 1. Without piano-dp the driver declines in
// probe() and maps nothing.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoAdspContract.h>
#include "DpSurvey.h"
#include "DpPort.h"
#include "DpFake.h"

using namespace pianodp;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_dp_stage = 0;
}

namespace {

struct Args {
  uint32_t level, survey, raw, delay;
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
  gArgs.level = bootArg("piano-dp", 0);
  gArgs.survey = bootArg("piano-dp-survey", 1);
  gArgs.raw = bootArg("piano-dp-raw", 16);
  if (gArgs.raw > 1000) gArgs.raw = 1000;
  gArgs.delay = bootArg("piano-dp-delay", 0);
  if (gArgs.delay > 600) gArgs.delay = 600;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[480];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoDp: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_dp_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// Explicit ldr, as PianoDpu does. This file has no store helper.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}

// ------------------------------------------------------------------------------------------------ MMIO pages, mapped on demand
// PianoDpu's scheme: a page is mapped (device memory, uncached) the first time one of its words is loaded, and every page is unmapped
// after the pass. The survey touches about 40 pages: GCC, TCSR, DISPCC, the DP controller, the QMP PHY, the DPU's TOP/CTL/LM/MERGE_3D/
// INTF pages, three TLMM pins.
struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
constexpr unsigned kMaxPages = 48;
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

const char *onOff(uint32_t cbcr) { return pianodpu::branchOn(cbcr) ? "on" : "off"; }

// ------------------------------------------------------------------------------------------------------------ the report
void reportGate(const Survey &s)
{
  const pianodpu::ClockGate &g = s.mdss;
  note("gate: %s (gcc disp ahb %08x, dispcc mdss gdsc %08x ahb %08x mdp %08x)", g.why, g.gccDispAhb, g.gdsc, g.ahb, g.mdp);
  if (!s.clocks.read) {
    note("dispcc dp clocks: not read (GCC's display AHB clock is off: DISPCC itself is not readable)");
    return;
  }
  const DpClocks &c = s.clocks;
  note("dispcc dp branches: aux %08x (%s) link %08x (%s) link_intf %08x (%s) pixel0 %08x (%s) pixel1 %08x (%s) crypto %08x (%s) "
       "usb_router_link_intf %08x (%s)", c.aux, onOff(c.aux), c.link, onOff(c.link), c.linkIntf, onOff(c.linkIntf), c.pixel0,
       onOff(c.pixel0), c.pixel1, onOff(c.pixel1), c.crypto, onOff(c.crypto), c.routerLinkIntf, onOff(c.routerLinkIntf));
  const Rcg aux = rcg(c.auxRcg[0], c.auxRcg[1]), link = rcg(c.linkRcg[0], c.linkRcg[1]), px0 = rcg(c.pixel0Rcg[0], c.pixel0Rcg[1]),
            px1 = rcg(c.pixel1Rcg[0], c.pixel1Rcg[1]);
  note("dispcc dp rcgs: aux %08x/%08x (%s, source %u, half-divider %u) link %08x/%08x (%s, source %u) pixel0 %08x/%08x (%s, source "
       "%u, mode %u, M %08x N %08x D %08x) pixel1 %08x/%08x (%s) link divider %08x", c.auxRcg[0], c.auxRcg[1],
       aux.on ? "root on" : "root off", aux.source, aux.halfDivider, c.linkRcg[0], c.linkRcg[1], link.on ? "root on" : "root off",
       link.source, c.pixel0Rcg[0], c.pixel0Rcg[1], px0.on ? "root on" : "root off", px0.source, px0.mode, c.pixel0Rcg[2],
       c.pixel0Rcg[3], c.pixel0Rcg[4], c.pixel1Rcg[0], c.pixel1Rcg[1], px1.on ? "root on" : "root off", c.linkDiv);
}

void reportCtrl(const Survey &s)
{
  const DpCtrl &d = s.ctrl;
  if (!d.core) {
    note("dp controller: %s", d.why);
    return;
  }
  const Version v = hwVersion(d.hwVersion);
  note("dp ahb: hw version %08x (%u.%u.%u), sw reset %08x, phy ctrl %08x, clk ctrl %08x active %08x, intr %08x %08x %08x %08x, mask4 %08x",
       d.hwVersion, v.major, v.minor, v.step, d.swReset, d.phyCtrl, d.clkCtrl, d.clkActive, d.intr[0], d.intr[1], d.intr[2], d.intr[3],
       d.intrMask4);
  note("dp aux: hpd ctrl %08x (detection %s), hpd int status %08x (state %s), mask %08x, reftimer %08x, event times %08x %08x; aux "
       "ctrl %08x (%s), trans ctrl %08x, timeout count %08x, limits %08x, status %08x", d.hpdCtrl, (d.hpdCtrl & 1u) ? "on" : "off",
       d.hpdIntStatus, hpdStateName(hpdState(d.hpdIntStatus)), d.hpdIntMask, d.hpdRefTimer, d.hpdEventTime[0], d.hpdEventTime[1],
       d.auxCtrl, (d.auxCtrl & 1u) ? "enabled" : "disabled", d.auxTransCtrl, d.auxTimeoutCount, d.auxLimits, d.auxStatus);
  if (d.link)
    note("dp link: mainlink ctrl %08x (%s), state ctrl %08x, configuration %08x (%u lanes), total %08x active %08x misc %08x, lane map "
         "%08x, mainlink ready %08x, levels %08x, tu %08x", d.mainlinkCtrl, (d.mainlinkCtrl & 1u) ? "enabled" : "off", d.stateCtrl,
         d.configCtrl, configuredLanes(d.configCtrl), d.totalHorVer, d.activeHorVer, d.misc, d.laneMap, d.mainlinkReady, d.levels, d.tu);
  if (d.stream)
    note("dp p0: bist %08x, timing engine %08x (%s), intf config %08x, hsync %08x, vsync period %08x, display hctl %08x, tpg %08x",
         d.p0Bist, d.p0TimingEn, (d.p0TimingEn & 1u) ? "on" : "off", d.p0IntfConfig, d.p0HsyncCtl, d.p0VsyncPeriod, d.p0DisplayHctl,
         d.p0Tpg);
  note("dp controller: %s", d.why);
}

void reportPhy(const Survey &s)
{
  const pianousbhost::Clocks &c = s.usb;
  if (!s.phy.read) {
    note("qmp: not read (the PHY's power domain or its aux/com_aux clock is off: usb3 phy gdsc %08x, aux %08x, com_aux %08x, pipe "
         "%08x, tcsr usb3 clkref %08x)", c.phyGdsc, c.aux, c.comAux, c.pipe, c.clkrefUsb3);
    return;
  }
  const DpPhy &p = s.phy;
  const pianousbhost::Qmp &q = p.com;
  const unsigned o = pianousbhost::qmpOrientation(q.typec);
  note("qmp dp_com: mode %08x (%s), sw reset %08x, power down %08x, swi %08x, typec %08x (%s), typec pwrdn %08x, reset ovrd %08x (dp "
       "reset %s, usb3 reset %s); usb3 pcs status %08x power down %08x start %08x", q.phyMode, pianousbhost::qmpModeName(q.phyMode),
       q.swReset, q.powerDown, q.swi, q.typec, o == 0 ? "lanes by the PHY's own CC input" : o == 1 ? "normal" : "reverse", q.typecPwrdn,
       q.resetOvrd, (q.resetOvrd & kOvrdDpReset) ? "held" : "released", (q.resetOvrd & kOvrdUsbReset) ? "held" : "released",
       q.pcsStatus1, q.pcsPowerDown, q.pcsStart);
  char pd[64];
  pdCtlText(p.pdCtl, pd, sizeof pd);
  note("qmp dp phy: revision %02x %02x %02x %02x, cfg %08x cfg_1 %08x, pd_ctl %08x (%s: %s), mode %08x, vco div %08x, status %08x "
       "(tsync %s, ready %s), aux int status %08x; dp serdes c_ready %08x (%s) cmn %08x (freq %s, pll %s)", p.revision[0] & 0xffu,
       p.revision[1] & 0xffu, p.revision[2] & 0xffu, p.revision[3] & 0xffu, p.cfg, p.cfg1, p.pdCtl, pd, pdCtlVerdict(p.pdCtl), p.mode,
       p.vcoDiv, p.status, (p.status & kPhyTsyncDone) ? "done" : "no", (p.status & kPhyReady) ? "yes" : "no", p.auxIntStatus,
       p.cReadyStatus, (p.cReadyStatus & kCReady) ? "ready" : "not ready", p.cmnStatus, (p.cmnStatus & kCmnFreqDone) ? "done" : "no",
       (p.cmnStatus & kCmnPllLocked) ? "locked" : "not locked");
  note("qmp dp phy aux_cfg0..9 as found: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", p.auxCfg[0] & 0xffu, p.auxCfg[1] & 0xffu,
       p.auxCfg[2] & 0xffu, p.auxCfg[3] & 0xffu, p.auxCfg[4] & 0xffu, p.auxCfg[5] & 0xffu, p.auxCfg[6] & 0xffu, p.auxCfg[7] & 0xffu,
       p.auxCfg[8] & 0xffu, p.auxCfg[9] & 0xffu);
}

void reportPath(const Survey &s, char *summary, size_t size)
{
  using namespace pianodpu;
  const SecondPath &p = s.path;
  if (!p.read) {
    note("dpu dp side: not read (the display gate is closed)");
    snprintf(summary, size, "dpu not read");
    return;
  }
  const PathView v = viewPath(p);
  note("dpu top: intf sel %08x, dp phy intf sel %08x, intr en %08x (intf0 underrun %s vsync %s) status %08x", p.intfSel, p.dpPhyIntfSel,
       p.intrEn, ((p.intrEn >> kIntrIntfUnderrun[0]) & 1u) ? "on" : "off", ((p.intrEn >> kIntrIntfVsync[0]) & 1u) ? "on" : "off",
       p.intrStatus);
  for (unsigned i = 0; i < kCtlCount; ++i) {
    const CtlView c = viewCtl(p.ctls[i]);
    if (!((v.busyCtls >> i) & 1u) && i != 1) continue;   // the busy ones and CTL1 (the DP path's first choice)
    note("dpu ctl%u: top %08x (%s, group %u), intf %#x, lm %#x, pipes %#x fetch %#x, merge_3d %#x, dsc %#x, flush %08x%s", i,
         p.ctls[i].top, c.video ? "video" : "command", c.group, c.intfs, c.lms, c.pipes, c.fetch, c.merge3d, c.dsc, p.ctls[i].flush,
         (v.busyCtls >> i) & 1u ? "" : " (free)");
  }
  for (unsigned i = 0; i < kLmCount; ++i) {
    const LmView l = viewLm(p.lms[i]);
    if (!p.lms[i].outSize && !l.stagedPipes && !((v.busyLms >> i) & 1u)) continue;
    note("dpu lm%u: out %ux%u, op %08x%s, staged pipes %#x from stage %d%s", i, l.out.w, l.out.h, p.lms[i].opMode,
         l.right ? " (right of a pair)" : "", l.stagedPipes, l.firstStage, (v.busyLms >> i) & 1u ? "" : " (in no CTL: stale)");
  }
  note("dpu merge_3d: 0 %08x/%08x 1 %08x/%08x 2 %08x/%08x 3 %08x/%08x (mux/mode)", p.merge3d[0][0], p.merge3d[0][1], p.merge3d[1][0],
       p.merge3d[1][1], p.merge3d[2][0], p.merge3d[2][1], p.merge3d[3][0], p.merge3d[3][1]);
  for (unsigned k = 0; k < 2; ++k) {
    const IntfRegs &f = p.intf[k];
    const IntfTiming t = viewIntf(f);
    note("dpu intf%u (%s): en %08x (timing engine %s), config %08x config2 %08x, %ux%u (htotal %u vtotal %u), mux %08x, status %08x, "
         "frame count %08x, intr en %08x status %08x", k ? 3u : 0u, k ? "DP MST" : "DP", f.en, t.enabled ? "running" : "off", f.config,
         f.config2, t.hactive, t.vactive, t.htotal, t.vtotal, f.mux, f.status, f.frameCount, f.intrEn, f.intrStatus);
  }
  if (v.panelCtl >= 0)
    note("dpu: the panel's path is ctl%d (intf %#x, lm %#x, merge_3d %#x); busy: ctls %#x, lms %#x, merge_3d %#x, intfs %#x", v.panelCtl,
         v.panelIntfs, v.panelLms, v.panelMerge3d, v.busyCtls, v.busyLms, v.busyMerge3d, v.busyIntfs);
  else
    note("dpu: no CTL drives INTF1/INTF2 (the panel's path was not found); busy: ctls %#x, lms %#x", v.busyCtls, v.busyLms);
  if (v.fourK)
    note("dpu verdict: ctl%d, lm%d/lm%d, merge_3d 1 and intf0 are free: the DP path of displayport.md 4.4, up to 4K (two LMs), is "
         "available", v.dpCtl, 2 * v.dpLmPair, 2 * v.dpLmPair + 1);
  else
    note("dpu verdict: DP path ctl %d, lm pair %d (lm%d/lm%d), merge_3d 1 %s, intf0 %s: %s", v.dpCtl, v.dpLmPair,
         v.dpLmPair >= 0 ? 2 * v.dpLmPair : -1, v.dpLmPair >= 0 ? 2 * v.dpLmPair + 1 : -1, v.merge3d1Free ? "free" : "taken",
         v.intf0Running ? "running" : v.intf0Free ? "free" : "taken",
         v.dpCtl >= 0 && v.dpLmPair >= 0 && v.intf0Free ? "a 2560-wide path is available, not the 4K pair" : "no DP path is free");
  snprintf(summary, size, "panel ctl%d lm %#x; dp ctl%d lm pair %d merge_3d1 %s intf0 %s", v.panelCtl, v.panelLms, v.dpCtl, v.dpLmPair,
           v.merge3d1Free ? "free" : "taken", v.intf0Running ? "running" : v.intf0Free ? "free" : "taken");
}

void reportPins(const Survey &s)
{
  using namespace pianousbhost;
  const PinDecode o = decodePin(s.pins.orientation.cfg, s.pins.orientation.io), a = decodePin(s.pins.sda.cfg, s.pins.sda.io),
                  b = decodePin(s.pins.scl.cfg, s.pins.scl.io);
  note("pins: gpio%u (Type-C orientation) cfg %08x io %08x: in %u (%s), pull %s; gpio%u/gpio%u (DIO4485 I2C) function %u/%u, in %u/%u; "
       "the DIO4485 is not read (an I2C read takes the pins)", kGpioOrientation, s.pins.orientation.cfg, s.pins.orientation.io, o.in,
       o.in ? "reverse" : "normal or none", pullName(o.pull), kGpioMuxSda, kGpioMuxScl, a.function, b.function, a.in, b.in);
}

// One line for a re-read after a plug: the gates and the words that would change if anything lit the path.
void reportGatesAgain(const Survey &s, const char *why)
{
  note("%s: dispcc dp aux %s link %s pixel0 %s; dp controller %s; qmp %s%s%s; intf0 %s", why, s.clocks.read ? onOff(s.clocks.aux) : "?",
       s.clocks.read ? onOff(s.clocks.link) : "?", s.clocks.read ? onOff(s.clocks.pixel0) : "?", s.ctrl.core ? "clocked" : "not clocked",
       s.phy.read ? pianousbhost::qmpModeName(s.phy.com.phyMode) : "not read", s.phy.read ? ", dp pd_ctl " : "",
       s.phy.read ? pdCtlVerdict(s.phy.pdCtl) : "", s.path.read ? (pianodpu::viewIntf(s.path.intf[0]).enabled ? "running" : "off") : "?");
}

void reportFake()
{
  FakeArgs a;
  a.dp = gArgs.level;
  a.plug = bootArg("piano-dp-fake", 0);
  a.boot = bootArg("piano-dp-fake-boot", 0);
  a.unplug = bootArg("piano-dp-fake-unplug", 0);
  a.cycles = bootArg("piano-dp-fake-cycles", 0);
  a.primary = bootArg("piano-dp-fake-primary", 0);
  char mode[32] = {}, mm[32] = {};
  if (PE_parse_boot_arg_str("piano-dp-fake-mode", mode, sizeof mode)) a.mode = mode;
  if (PE_parse_boot_arg_str("piano-dp-fake-mm", mm, sizeof mm)) a.mm = mm;
  const FakeConfig c = fakeConfig(a);
  if (!c.armed) {
    if (a.plug || a.boot) note("D6 fake display: off (%s)", c.note);
    return;
  }
  char when[96];
  if (c.atBoot) snprintf(when, sizeof when, "connected from its start");
  else snprintf(when, sizeof when, "hot-plugged %u s after its start", c.plugS);
  char cycle[96] = "";
  if (c.unplugS) snprintf(cycle, sizeof cycle, ", then unplugged and plugged every %u s, %u plugs in all, ending plugged", c.unplugS, c.cycles);
  note("D6 fake display (NativeGopDisplay's second instance, RAM-backed, no hardware): %ux%u@%u, %ux%u mm, primary %u; %s%s%s%s",
       c.width, c.height, c.hz, c.widthMm, c.heightMm, c.primary, when, cycle, *c.note ? "; " : "", c.note);
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoDp : public IOService {
  OSDeclareDefaultStructors(PianoDp)

  // Port notifications from PianoPower's thread, drained by ours.
  static constexpr unsigned kRing = 16;
  IOSimpleLock *fLock = nullptr;
  PianoPortEvent fRing[kRing];
  unsigned fHead = 0, fCount = 0;
  uint32_t fDropped = 0, fDroppedLogged = 0;
  PortState fPorts[kPorts];
  uint32_t fEvents = 0, fInvalid = 0;
  bool fResurveyed = false;

  static void thread(void *self, wait_result_t);
  static void portEvent(void *context, const PianoPortEvent *event);
  void run();
  void runSurvey(bool full);
  void handle(const PianoPortEvent &e);
  void logRaw(const PianoPortEvent &e);
  void publishPort(unsigned port);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoDp, IOService)

IOService *PianoDp::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoDp::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-dp=%u%s: read only: %s the port events (D2: needs piano-power=2, whose DSP sends them); nothing is written", gArgs.level,
       gArgs.level > 1 ? " (levels above 1 are not built: level 1)" : "",
       gArgs.survey ? "the D1 survey (DP controller, QMP DP half, the DPU's DP side, each block behind its clock gate), then"
                    : "no survey (piano-dp-survey=0: no MMIO at all), only");
  reportFake();
  setProperty("piano-dp", gArgs.level, 32);
  fLock = IOSimpleLockAlloc();
  thread_t t = nullptr;
  if (!fLock || kernel_thread_start(&PianoDp::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;   // stays attached and does nothing more
  }
  thread_deallocate(t);
  return true;
}

void PianoDp::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-dp");
  static_cast<PianoDp *>(self)->run();
}

// PianoPower's thread: copy and wake ours (PianoAdspContract.h rule 2).
void PianoDp::portEvent(void *context, const PianoPortEvent *event)
{
  PianoDp *self = static_cast<PianoDp *>(context);
  IOSimpleLockLock(self->fLock);
  if (self->fCount < kRing) {
    self->fRing[(self->fHead + self->fCount) % kRing] = *event;
    self->fCount++;
  } else {
    self->fDropped++;
  }
  IOSimpleLockUnlock(self->fLock);
  thread_wakeup((event_t)self);
}

void PianoDp::runSurvey(bool full)
{
  Survey s;
  survey(s, load);
  if (full) {
    reportGate(s);
    reportCtrl(s);
    reportPhy(s);
    char summary[160];
    reportPath(s, summary, sizeof summary);
    reportPins(s);
    char line[240];
    snprintf(line, sizeof line, "dp %s; qmp %s; %s", s.ctrl.core ? "clocked" : "not clocked",
             s.phy.read ? pianousbhost::qmpModeName(s.phy.com.phyMode) : "not read", summary);
    setProperty("piano-dp-survey", line);
    note("summary: %s", line);
  } else {
    reportGatesAgain(s, "after the first HPD high, read again");
  }
  note("%s: %u MMIO loads, 0 stores; %u pages mapped, %u map failures", full ? "survey done" : "re-read done", s.loads, gPageCount,
       gMapFailures);
  unmapAll();
}

void PianoDp::logRaw(const PianoPortEvent &e)
{
  const uint8_t *d = e.data;
  char hex[3 * PIANO_PORT_EVENT_MAX + 1];
  unsigned at = 0;
  const uint32_t n = e.length < PIANO_PORT_EVENT_MAX ? e.length : PIANO_PORT_EVENT_MAX;
  for (uint32_t i = 0; i < n && at + 4 < sizeof hex; ++i) {
    static const char digits[] = "0123456789abcdef";
    if (i && (i % 4) == 0) hex[at++] = ' ';
    hex[at++] = digits[d[i] >> 4];
    hex[at++] = digits[d[i] & 15];
  }
  hex[at] = 0;
  note("event #%u: %u bytes: %s", e.sequence, e.length, hex);
}

void PianoDp::publishPort(unsigned port)
{
  const PortState &s = fPorts[port];
  char key[20], value[120];
  snprintf(key, sizeof key, "piano-dp-port%u", port);
  snprintf(value, sizeof value, "%s, mux %u, %s, pin %c, HPD %s; %u events, %u plugs, %u unplugs, %u IRQ_HPD",
           s.attached ? (s.dp ? "DisplayPort" : "attached") : "detached", s.mux, orientationName(s.orientation),
           pinInfo(s.pin).letter, s.hpd ? "high" : "low", s.events, s.plugs, s.unplugs, s.irqs);
  setProperty(key, value);
}

void PianoDp::handle(const PianoPortEvent &raw)
{
  fEvents++;
  if (fEvents <= gArgs.raw) logRaw(raw);
  const PortEvent e = decodePortEvent(raw.data, raw.length);
  if (!e.valid) {
    if (++fInvalid <= 16) note("event #%u: not decoded: %s", raw.sequence, e.why);
    return;
  }
  note("port %u: SVID %#06x %s, mux %u (%s), orientation %s, %s%c, HPD %s, IRQ_HPD %u (vid %#06x, payload svid %#06x, ext %02x)", e.port,
       e.svid, svidName(e.svid), e.mux, muxName(e.mux), orientationName(e.orientation), e.dp ? "pin " : "no pin", e.dp ? pinInfo(e.pin).letter : ' ',
       e.hpd ? "high" : "low", e.irq ? 1u : 0u, e.vid, e.svidPayload, e.ext);
  PortState &s = fPorts[e.port];
  const PortStep r = portStep(s, e);
  const uint32_t ch = r.changes;
  const PinInfo pin = pinInfo(s.pin);
  if (ch & kChAttach) note("port %u: attach (%s, %s)", e.port, muxName(e.mux), orientationName(e.orientation));
  if (ch & kChOrientation)
    note("port %u: orientation %s -> %s", e.port, orientationName(r.before.orientation), orientationName(r.after.orientation));
  if (ch & kChMux) note("port %u: mux %s -> %s", e.port, muxName(r.before.mux), muxName(r.after.mux));
  if (ch & kChDpEnter) note("port %u: DisplayPort alt mode entered: pin %c (%u lanes%s)", e.port, pin.letter, pin.lanes,
                            pin.usb3 ? " + USB 3" : ", USB 2 only");
  if (ch & kChPin) note("port %u: pin assignment %c -> %c (%u lanes%s)", e.port, pinInfo(r.before.pin).letter, pin.letter, pin.lanes,
                        pin.usb3 ? " + USB 3" : ", USB 2 only");
  if (ch & kChUnsupportedPin)
    note("port %u: pin %c is not driven (A and B are legacy: the DIO4485's driver refuses them; none: no pin assignment)", e.port, pin.letter);
  if (ch & kChPinMuxMismatch)
    note("port %u: pin %c (%u lanes) does not match mux %s", e.port, pin.letter, pin.lanes, muxName(e.mux));
  if (ch & (kChDpEnter | kChPin | kChOrientation)) {
    const PlugPlan p = plugPlan(s);
    if (p.drive) {
      char modes[96] = {};
      unsigned at = 0;
      for (unsigned i = 0; i < kStdModeCount && at + 16 < sizeof modes; ++i)
        if ((p.modes >> i) & 1u) at += unsigned(snprintf(modes + at, sizeof modes - at, "%s%s", at ? ", " : "", kStdModes[i].name));
      note("port %u: plan (nothing written at level 1): DIO4485 enable %#04x select %#04x, QMP %s, DP %u lanes; at HBR2 8 bpc: %s",
           e.port, p.dioEnable, p.dioSelect, pianousbhost::qmpModeName(p.qmpMode), p.lanes, modes[0] ? modes : "no standard mode");
    }
  }
  if (ch & kChHpdUp) {
    note("port %u: HPD high: a sink is there (plug %u); level 2 (not built) would read DPCD and EDID, level 3 train", e.port, s.plugs);
    if (!fResurveyed && gArgs.survey) {
      fResurveyed = true;
      runSurvey(false);
    }
  }
  if (ch & kChIrqHpd) note("port %u: IRQ_HPD: the sink asks for attention (levels >= 2: DPCD 0x200-0x205, a re-train on a lost lock)", e.port);
  if (ch & kChIrqLow) note("port %u: IRQ_HPD with HPD low (not a DP event: ignored)", e.port);
  if (ch & kChHpdDown) note("port %u: HPD low: unplug %u (levels >= 4: the stream parked, the PHY down)", e.port, s.unplugs);
  if (ch & kChDpExit) note("port %u: DisplayPort alt mode left (%s)", e.port, s.attached ? muxName(s.mux) : "detached");
  if (ch & kChDetach) note("port %u: detach", e.port);
  if (ch & kChRepeat) note("port %u: no change (a repeat)", e.port);
  publishPort(e.port);
}

void PianoDp::run()
{
  if (gArgs.delay) {
    note("waiting %u s first (piano-dp-delay)", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  if (gArgs.survey) {
    stage(1, "D1 survey (read only; every block behind its clock gate)");
    runSurvey(true);
  } else {
    stage(1, "no survey (piano-dp-survey=0)");
  }
  static const PianoPortClient client = {&PianoDp::portEvent};
  const int slot = piano_port_events(&client, this);
  if (slot < 0) {
    stage(-2, slot == -1 ? "port events: all of PianoPower's client slots are taken" : "port events: refused");
    return;
  }
  note("port events: registered with PianoPower (slot %d); it has received %u port notifications so far (they come only with "
       "piano-power=2, once its charger link is up and the port owner answered the enable)", slot, piano_port_seen());
  stage(2, "D2 port events: waiting for the DSP's notifications (event-driven)");
  uint64_t statusDue = nowMs() + 180000;
  for (;;) {
    for (;;) {
      PianoPortEvent e;
      bool have = false;
      uint32_t dropped = 0;
      IOSimpleLockLock(fLock);
      if (fCount) {
        e = fRing[fHead];
        fHead = (fHead + 1) % kRing;
        fCount--;
        have = true;
      }
      dropped = fDropped;
      IOSimpleLockUnlock(fLock);
      if (dropped != fDroppedLogged) {
        note("%u port notifications dropped (the ring of %u was full)", dropped - fDroppedLogged, kRing);
        fDroppedLogged = dropped;
      }
      if (!have) break;
      handle(e);
    }
    // One status line 3 minutes in when nothing came (then nothing more: no polling).
    if (statusDue && nowMs() >= statusDue) {
      statusDue = 0;
      if (!fEvents) {
        PianoAdspHealth h = {};
        piano_adsp_health(&h);
        note("no port notification 3 minutes after the start (PianoPower stage %d, DSP %s, charger_pd %#x, %u seen): plug a DP "
             "monitor or dock (D2), with piano-power=2", h.stage, h.dsp_up ? "up" : "not up", h.charger_pd, piano_port_seen());
      }
    }
    if (statusDue) {
      const uint64_t now = nowMs();
      assert_wait_timeout((event_t)this, THREAD_UNINT, uint32_t(statusDue > now ? statusDue - now : 1), kMillisecondScale);
    } else {
      assert_wait((event_t)this, THREAD_UNINT);
    }
    IOSimpleLockLock(fLock);
    const bool pending = fCount != 0;
    IOSimpleLockUnlock(fLock);
    if (pending) clear_wait(current_thread(), THREAD_AWAKENED);
    else thread_block(THREAD_CONTINUE_NULL);
  }
}
