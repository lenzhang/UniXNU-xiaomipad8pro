// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi: the Qualcomm WCN7860 Wi-Fi of piano (SM8750, PCIe0) for macOS 27, native at EL1. Design, ladder and test plan:
// docs/drivers/wifi.md. Facts and their sources: WifiHw.h. This is level 1 only:
//   piano-wifi=1   a read-only survey of what the boot stages left of the Wi-Fi path, logged as "PianoWifi: ..." lines: the PCIe0 and
//                  PHY power domains, block resets, branch clocks and their votes (GCC), the PHY's reference clock gate (TCSR), the
//                  WLAN_EN / SW_CTRL / PERST# / CLKREQ# / WAKE# pins (TLMM), the controller (PARF, DBI, iATU) and the PHY (PCS) only
//                  when they are powered and clocked, the SMMU stream-match entries of streams 0x1400/0x1401, the RPMh resources
//                  of the PMU's and the PHY's supplies (the AOP command database, memory). Nothing is written: every MMIO access of
//                  this level is a load (WifiSurvey.h has no store at all); the endpoint's configuration space is never read.
//   piano-wifi-spmi=1  adds the supplies' PMIC regulators and the BT_EN / sleep clock pins over the SPMI arbiter's observer channel:
//                  the only stores are the observer channel's read commands (as PianoPower's and PianoInput's read-only levels),
//                  never a PMIC register. Off by default, so piano-wifi=1 alone stores nothing at all.
// Other arguments: piano-wifi-delay=S (wait S seconds first, default 0), piano-wifi-repeat=S with piano-wifi-samples=N (resample the
// MMIO part every S s, N times, default 3, one summary line each), piano-wifi-hwcg=1 (count a hardware-gated, voted branch under
// HW_CTL that reads CLK_OFF as running), piano-wifi-smmu=0 / piano-wifi-cmddb=0 (skip those reads). Without piano-wifi the driver
// declines in probe() and maps nothing. Levels above 1 are not implemented; they run level 1.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "WifiSurvey.h"

using namespace pianowifi;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_wifi_stage = 0;
}

namespace {

struct Args {
  uint32_t level, spmi, smmu, cmddb, hwcg, delay, repeat, samples;
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
  gArgs.level = bootArg("piano-wifi", 0);
  gArgs.spmi = bootArg("piano-wifi-spmi", 0);
  gArgs.smmu = bootArg("piano-wifi-smmu", 1);
  gArgs.cmddb = bootArg("piano-wifi-cmddb", 1);
  gArgs.hwcg = bootArg("piano-wifi-hwcg", 0);
  gArgs.delay = bootArg("piano-wifi-delay", 0);
  if (gArgs.delay > 600) gArgs.delay = 600;
  gArgs.repeat = bootArg("piano-wifi-repeat", 0);
  if (gArgs.repeat > 3600) gArgs.repeat = 3600;
  gArgs.samples = bootArg("piano-wifi-samples", 3);
  if (gArgs.samples > 100) gArgs.samples = 100;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoWifi: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_wifi_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

// Explicit ldr, as PianoDwc3 and PianoPower do. The only store helper of this file is spmiWr (the observer channel's command).
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}

// ------------------------------------------------------------------------------------------------ MMIO pages, mapped on demand
// The survey decides which addresses it loads (WifiSurvey.h, host-tested); a page is mapped the first time one of its words is
// loaded, so a block the gates keep the survey off is not even mapped.
struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
constexpr unsigned kMaxPages = 64;
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

// ------------------------------------------------------------------------------------------------------------- SPMI (observer)
// The arbiter's windows (SpmiObserver.h): reads through load(); the one kind of store, the observer channel's read command, is
// refused for any other window or offset.
unsigned gSpmiStores, gSpmiRefused;
uint32_t spmiRd(void *, Window w, uint32_t off)
{
  const uint64_t base = w == Window::core ? kSpmiCore : w == Window::cnfg ? kSpmiCnfg : kSpmiObsv;
  return load(base + off);
}
void spmiWr(void *, Window w, uint32_t off, uint32_t value)
{
  if (w != Window::obsv || off % kObsvStride != kObsvCmd || off >= kSpmiObsvBytes || (value >> 27) != kOpExtReadL) {
    ++gSpmiRefused;
    return;
  }
  const uintptr_t va = pageVa(kSpmiObsv + off);
  if (!va) {
    ++gMapFailures;
    return;
  }
  ++gSpmiStores;
  __asm__ volatile("str %w0, [%1]" ::"r"(value), "r"(va + uintptr_t((kSpmiObsv + off) & 0xfff)) : "memory");
}
void spmiDelay(void *, unsigned us) { IODelay(us); }
const Io gSpmi = {nullptr, spmiRd, spmiWr, spmiDelay};

// ------------------------------------------------------------------------------------------------------------- the report
const char *onOff(bool on) { return on ? "on" : "off"; }

void reportClocks(const ClockState &c)
{
  note("GCC pcie0 gdsc %08x cfg %08x (%s) phy gdsc %08x cfg %08x (%s) collapse vote %08x (pcie0 %s, phy %s)", c.gdsc, c.gdscCfg,
       onOff(gdscOn(c.gdsc)), c.phyGdsc, c.phyGdscCfg, onOff(gdscOn(c.phyGdsc)), c.collapse,
       (c.collapse & kCollapsePcie0) ? "collapsed" : "released", (c.collapse & kCollapsePcie0Phy) ? "collapsed" : "released");
  char line[360];
  size_t n = 0;
  for (unsigned i = 0; i < kResetCount && n < sizeof line; ++i)
    n += size_t(snprintf(line + n, sizeof line - n, "%s%s %08x%s", i ? ", " : "", kResets[i].name, c.bcr[i],
                         resetAsserted(c.bcr[i]) ? " (asserted)" : ""));
  note("GCC resets: %s", line);
  note("GCC votes 0x52008 %08x 0x52000 %08x", c.vote8, c.vote0);
  n = 0;
  for (unsigned i = 0; i < kBranchCount && n < sizeof line; ++i)
    n += size_t(snprintf(line + n, sizeof line - n, "%s%s %08x %s%s%s", i ? ", " : "", kBranches[i].name, c.cbcr[i],
                         branchOff(c.cbcr[i]) ? "off" : "on", (c.voteMask >> i) & 1 ? " voted" : "",
                         (c.idleMask >> i) & 1 ? " hw-idle" : ""));
  note("GCC branches: %s", line);
  note("GCC aux rcg %08x/%08x rchng rcg %08x/%08x pipe mux %08x (%s); TCSR pcie0 clkref %08x (%s)", c.auxRcg, c.auxCfg, c.rchngRcg,
       c.rchngCfg, c.pipeMux, (c.pipeMux & 3) == 0 ? "PHY" : (c.pipeMux & 3) == 2 ? "TCXO" : "?", c.clkref,
       (c.clkref & 1) ? "enabled" : "disabled");
}

void reportPins(const Survey &s)
{
  for (unsigned i = 0; i < kPinCount; ++i) {
    const PinDecode d = decodePin(s.pins[i].cfg, s.pins[i].io);
    note("pin %s gpio%u: cfg %08x io %08x: function %u pull %s %u mA output %s, out %u in %u", kPins[i].name, kPins[i].gpio,
         s.pins[i].cfg, s.pins[i].io, d.function, pullName(d.pull), d.driveMa, d.outputEnable ? "enabled" : "disabled", d.out, d.in);
  }
}

void reportController(const ControllerState &r)
{
  if (!r.parfRead) {
    note("controller not read (%s): PARF, DBI and iATU not touched", r.skipped ? r.skipped : "?");
    return;
  }
  note("PARF sys %08x pm %08x pm_stts %08x phy_ctrl %08x ltssm %08x (enable %u, state %#x) int_all %08x status %08x", r.sysCtrl,
       r.pmCtrl, r.pmStts, r.phyCtrl, r.ltssm, (r.ltssm & kParfLtssmEnable) ? 1u : 0u, r.ltssm & kLtssmMask, r.intAll, r.status);
  note("PARF dbi base %08x%08x slave space %08x%08x atu base %08x%08x device type %08x bdf-to-sid %08x (%s)", r.dbiBaseHi,
       r.dbiBase, r.slvSpaceHi, r.slvSpace, r.atuBaseHi, r.atuBase, r.deviceType, r.bdfToSid, (r.bdfToSid & 1) ? "bypass" : "table");
  if (!r.dbiRead) {
    note("DBI not read (%s)", r.skipped ? r.skipped : "?");
    return;
  }
  note("DBI id %08x command %08x class %08x buses %08x; port debug0 %08x (LTSSM %#x %s) debug1 %08x (link %s)", r.id, r.command,
       r.classRev, r.busNumbers, r.debug0, r.debug0 & kLtssmMask, ltssmName(r.debug0), r.debug1,
       (r.debug1 & kPortDebug1LinkUp) ? "up" : "down");
  note("DBI pcie cap at %#x: link cap %08x control/status %08x (Gen%u x%u)", r.expCap, r.linkCap, r.linkCtlSta, linkSpeed(r),
       linkWidth(r));
  for (unsigned i = 0; i < 2; ++i)
    note("iATU outbound %u: ctrl1 %08x ctrl2 %08x (%s) base %08x limit %08x target %08x", i, r.atuCtrl1[i], r.atuCtrl2[i],
         (r.atuCtrl2[i] & kAtuCtrl2Enable) ? "enabled" : "disabled", r.atuBase0[i], r.atuLimit[i], r.atuTarget[i]);
}

void reportPhy(const PhyState &p)
{
  if (!p.read) {
    note("PHY not read (%s)", p.skipped ? p.skipped : "?");
    return;
  }
  note("PHY pcs sw_reset %08x status1 %08x (%s) power_down %08x start %08x", p.swReset, p.status1,
       (p.status1 & kPcsPhyStatus) ? "not ready" : "ready", p.powerDown, p.start);
}

void reportSmmu()
{
  const uint16_t streams[2] = {kStreamRoot, kStreamEndpoint};
  const SmmuStreams s = surveySmmu(streams, kSmmuBytes, [](uint32_t off) { return load(kSmmu + off); });
  note("SMMU idr0 %08x idr1 %08x: %u stream-match groups, %u banks, %u pages%s%s", s.idr0, s.idr1, s.groups, s.banks, s.pages,
       s.error ? ": " : "", s.error ? s.error : "");
  for (unsigned k = 0; k < 2 && !s.error; ++k) {
    if (!s.count[k]) note("SMMU stream %#x: no stream-match entry (the firmware left none)", streams[k]);
    for (unsigned i = 0; i < s.count[k]; ++i) {
      const SmmuMatch &m = s.found[k][i];
      note("SMMU stream %#x: group %u smr %08x s2cr %08x (%s, bank %u, sctlr %08x)", streams[k], m.group, m.smr, m.s2cr,
           s2crType(m.s2cr), m.s2cr & 0xffu, m.sctlr);
    }
  }
}

void reportCmdDb()
{
  uint8_t *copy = static_cast<uint8_t *>(IOMallocData(kCmdDbBytes));
  if (!copy) {
    note("cmd-db: no memory for its copy");
    return;
  }
  // memory, not MMIO: one mapping of the whole region (PianoKeyboard's way), copied out word by word and released
  IODeviceMemory *m = IODeviceMemory::withRange(kCmdDb, kCmdDbBytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) {
    IOFreeData(copy, kCmdDbBytes);
    note("cmd-db: cannot map %#llx", (unsigned long long)kCmdDb);
    return;
  }
  const uintptr_t va = uintptr_t(map->getVirtualAddress());
  for (uint32_t i = 0; i < kCmdDbBytes; i += 4) {
    const uint32_t w = rd32(va + i);
    memcpy(copy + i, &w, 4);
  }
  map->release();
  char line[360];
  size_t n = 0;
  static const char *const kExtra[] = {"rfclka1", "SN4"};   // the RF reference clock of mainline's MTP, the PCIe NoC BCM (PAGANI)
  for (unsigned i = 0; i < kRailCount + 2 && n < sizeof line; ++i) {
    const char *name = i < kRailCount ? kRails[i].cmdDb : kExtra[i - kRailCount];
    uint32_t address = 0;
    const int rc = cmdDbFind(copy, kCmdDbBytes, name, &address);
    n += size_t(rc ? snprintf(line + n, sizeof line - n, "%s%s none (%d)", i ? ", " : "", name, rc)
                   : snprintf(line + n, sizeof line - n, "%s%s %#x", i ? ", " : "", name, address));
  }
  IOFreeData(copy, kCmdDbBytes);
  note("cmd-db: %s", line);
}

void reportPmic()
{
  PmicSurvey s;
  surveyPmic(gSpmi, s);
  if (!s.arbiter.v7) {
    note("SPMI: arbiter version %08x, %u peripherals: not v7, not read", s.arbiter.version, s.arbiter.count);
    return;
  }
  for (unsigned i = 0; i < kRailCount; ++i) {
    const Rail &r = kRails[i];
    const RailState &st = s.rails[i];
    const PmicPeripheral *p = st.reg();
    if (!p) {
      note("rail %s %s (%s): no regulator found (ppid %03x %s type %02x%s)", r.supply, r.regulator, r.cmdDb, st.primary.ppid,
           st.primary.present ? "present" : "absent", st.primary.type,
           r.ldo ? (st.alternate.present ? ", alternate present" : ", alternate absent") : "");
      continue;
    }
    note("rail %s %s (%s): ppid %03x apid %d owner %u type %02x/%02x status %02x (%s) %u mV mode %02x en %02x (%s), DT %u mV%s",
         r.supply, r.regulator, r.cmdDb, p->ppid, p->apid, p->owner, p->type, p->subtype, p->status, p->ready() ? "ready" : "not ready",
         p->millivolts(), p->regs40[5], p->regs40[6], p->enabled() ? "enabled" : "disabled", r.millivolts,
         st.chosen == 1 ? " (SM8550 page rule)" : "");
  }
  for (unsigned i = 0; i < kPmicPinCount; ++i) {
    const PmicPeripheral &p = s.pins[i];
    if (!p.read) {
      note("pmic pin %s: ppid %03x %s", kPmicPins[i].name, p.ppid, p.present ? "not readable" : "absent");
      continue;
    }
    note("pmic pin %s: ppid %03x type %02x/%02x status %02x mode %02x vin %02x pull %02x src %02x out %02x en %02x", kPmicPins[i].name,
         p.ppid, p.type, p.subtype, p.status, p.regs40[0], p.regs40[1], p.regs40[2], p.regs40[4], p.regs40[5], p.regs40[6]);
  }
  note("SPMI: %u observer read commands, %u refused stores", s.commands, gSpmiRefused);
}

// One line for ioreg and for the repeated samples.
void summary(const Survey &s, char *line, size_t size)
{
  const ClockState &c = s.clocks;
  const PinDecode en = decodePin(s.pins[kPinWlanEn].cfg, s.pins[kPinWlanEn].io),
                  perst = decodePin(s.pins[kPinPerst].cfg, s.pins[kPinPerst].io),
                  sw = decodePin(s.pins[kPinSwCtrl].cfg, s.pins[kPinSwCtrl].io);
  snprintf(line, size, "pcie0 %s phy %s branches-off %03x wlan_en %u/%u perst %u/%u sw_ctrl %u link %s ltssm %#x phystatus %s",
           onOff(gdscOn(c.gdsc)), onOff(gdscOn(c.phyGdsc)), c.offMask, en.out, en.in, perst.out, perst.in, sw.in,
           s.controller.dbiRead ? (linkUp(s.controller) ? "up" : "down") : "unread",
           s.controller.parfRead ? s.controller.ltssm & kLtssmMask : 0xffu,
           s.phy.read ? ((s.phy.status1 & kPcsPhyStatus) ? "not-ready" : "ready") : "unread");
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoWifi : public IOService {
  OSDeclareDefaultStructors(PianoWifi)
  void run();
  static void thread(void *self, wait_result_t);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoWifi, IOService)

IOService *PianoWifi::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoWifi::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-wifi=%u spmi=%u smmu=%u cmddb=%u hwcg=%u delay=%u repeat=%u samples=%u (WCN7860 %04x:%04x on PCIe0 %#llx)",
       gArgs.level, gArgs.spmi, gArgs.smmu, gArgs.cmddb, gArgs.hwcg, gArgs.delay, gArgs.repeat, gArgs.samples, kEndpointVendor,
       kEndpointDevice, (unsigned long long)kParf);
  if (gArgs.level > 1) note("piano-wifi=%u: only level 1 exists (docs/drivers/wifi.md section 4); running level 1", gArgs.level);
  note("level 1: loads only, nothing written%s", gArgs.spmi ? "; SPMI observer read commands for the PMIC part" : "");
  setProperty("piano-wifi", gArgs.level, 32);
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoWifi::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;
  }
  thread_deallocate(t);
  return true;
}

void PianoWifi::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-wifi");
  static_cast<PianoWifi *>(self)->run();
}

void PianoWifi::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  stage(1, "GCC, TCSR and TLMM (read only)");
  Survey s;
  surveyClocks(s, load, gArgs.hwcg != 0);
  surveyPins(s, load);
  reportClocks(s.clocks);
  reportPins(s);
  stage(2, "PCIe0 controller: PARF, DBI, iATU (read only, only when powered and clocked)");
  surveyController(s, load);
  reportController(s.controller);
  stage(3, "PCIe0 PHY: PCS status (read only, only when powered and clocked)");
  surveyPhy(s, load);
  reportPhy(s.phy);
  if (gArgs.smmu) {
    stage(4, "SMMU streams 0x1400/0x1401 (read only)");
    reportSmmu();
  }
  if (gArgs.cmddb) {
    stage(5, "AOP command database: the supplies' RPMh resources (read only)");
    reportCmdDb();
  }
  if (gArgs.spmi) {
    stage(6, "the supplies' PMIC regulators and the BT_EN / sleep clock pins over SPMI (observer reads)");
    reportPmic();
  }
  char line[200];
  summary(s, line, sizeof line);
  setProperty("piano-wifi-survey", line);
  const PinDecode en = decodePin(s.pins[kPinWlanEn].cfg, s.pins[kPinWlanEn].io);
  note("summary: %s", line);
  note("verdict: WLAN_EN %s, PCIe0 %s, PHY %s, link %s: %s", en.out ? "driven high" : "low", onOff(gdscOn(s.clocks.gdsc)),
       onOff(gdscOn(s.clocks.phyGdsc)), linkUp(s.controller) ? "up" : "not up",
       linkUp(s.controller) ? "the boot stages left the link trained (level 2 may skip the power-up; check the endpoint first)"
                            : "level 2 must do the whole power-up (rails, WLAN_EN, domains, clocks, PHY, PERST#, LTSSM)");
  for (uint32_t i = 1; gArgs.repeat && i <= gArgs.samples; ++i) {
    IOSleep(gArgs.repeat * 1000);
    Survey again;
    survey(again, load, gArgs.hwcg != 0);
    summary(again, line, sizeof line);
    note("sample %u (+%u s): %s", i, i * gArgs.repeat, line);
  }
  stage(7, "done");
  note("done: %llu MMIO loads, %u SPMI observer commands, 0 other stores; %u pages mapped, %u map failures",
       (unsigned long long)gLoads, gSpmiStores, gPageCount, gMapFailures);
  unmapAll();
}
