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
//   piano-wifi=2   power: the six PMU rails by RPMh (mode NPM, the voltage piano's DT asks for, enable) with the PMIC read back
//                  before and after, PERST# low, CLKREQ# to its PCIe function, WLAN_EN low 150 ms then high, the PMU's 50 ms,
//                  SW_CTRL read. The PHY's two LDOs (one of them the UFS PHY's PLL) are read, never voted.
//   piano-wifi=3   the link: the GDSCs, the RCG sources, the branch votes (under PianoA830's GCC vote lock), the TCSR reference
//                  gate, the block resets, the PARF set-up, the QMP v7 PHY sequence, the BDF-to-SID entry of the endpoint, the root
//                  port, PERST# released, the LTSSM, L0, the configuration window, `17cb:110e`, the memory window, BAR0, and the
//                  SMMU stream 0x1401 through a bypass context bank.
//   piano-wifi=4   MHI: the DMA window, the MHI registers in BAR0, `image/peach/amss20.bin` read at run time from the tablet's own
//                  modem_a, BHI (the boot loader), the MHI contexts, BHIe (the rest of the image), mission mode.
//   piano-wifi=5   QMI: the chip's two IPC router channels over MHI, the router itself, and the WLAN firmware's QMI service — the
//                  radio's and the chip's capability, the indication registration, the host capability, the firmware's memory
//                  regions answered out of the same window, the regulatory database and the China-region board file in 6 KiB
//                  segments, the PHY and auxiliary microcodes by address, the trace configuration, and the firmware's ready
//                  indication. Every file is read at run time from the tablet's own modem_a.
//   piano-wifi=6   the station's first half: the copy engine's rings over BAR0 (the pipes level 5 described to the firmware),
//                  the host transport's handshake (its ready message, the firmware's message interface connected, set-up
//                  complete), that interface's service-ready and ready events, the host's initialisation with this chip's
//                  resource configuration, a station virtual device, and then a **passive scan**: the networks it finds are
//                  logged (the address masked unless piano-wifi-bssid=1) and published as the property piano-wifi-scan, which
//                  PianoWifiEthernet's en1 and tooling/native/piano-wifi read. The scan is stopped and the virtual device
//                  deleted before the level ends. No association, no keys and no data path: those are levels 7 and 8.
//                  piano-wifi=6 implies the mode message of level 5 (the firmware's radio must be running), so it switches
//                  piano-wifi-mode on by itself.
// Levels are cumulative and each one stops the ladder where it fails, leaving WLAN_EN low and PERST# asserted. Other arguments:
// piano-wifi-delay=S (wait S seconds first, default 0), piano-wifi-repeat=S with piano-wifi-samples=N (resample the MMIO part every
// S s, N times, default 3, one summary line each), piano-wifi-hwcg=1 (count a hardware-gated, voted branch under HW_CTL that reads
// CLK_OFF as running), piano-wifi-smmu=0 / piano-wifi-cmddb=0 (skip those reads), piano-wifi-gen=N (the target link speed, default
// 2), piano-wifi-physeq=0 (only the PHY sequence's frame writes), piano-wifi-rcg=0 (leave the aux/rchng sources alone),
// piano-wifi-blind=1 (vote a rail whose PMIC state could not be read), piano-wifi-release=1 (on a failure withdraw the enable of
// the rails this driver enabled), piano-wifi-stop=N (return after step N), piano-wifi-dma=M (the DMA window in MiB, default 24),
// piano-wifi-fwtimeout=MS (the deadline of each MHI phase, default 20000), piano-wifi-qmitimeout=MS (the deadline of each level 5
// step, default 10000), piano-wifi-iova=1 (give the endpoint's stream a translating context bank whose 32-bit window maps the DMA
// window, instead of the bypass bank of level 3: the only way to hand this chip a 32-bit address, since piano's XNU memory is all
// above 4 GiB), piano-wifi-mode=1 (level 5 also sends the firmware's options, the host's copy-engine configuration and the mode,
// which is level 6's doorway), piano-wifi-shadow=1 (the shadow register list in that configuration),
// piano-wifi-wmitimeout=MS (the deadline of each level 6 step, default 10000), piano-wifi-scanwait=MS (how long the scan itself
// may take, default 12000), piano-wifi-band=N (1 both bands, 0 only 2.4 GHz), piano-wifi-bssid=1 (print a network's whole
// address in the log instead of its first three bytes). Without piano-wifi the driver declines in probe() and maps nothing.
//
// Power (package/include/PianoPowerContract.h): the radio is off unless piano-wifi>=2, since nothing below that level votes a
// rail or releases PERST#. From level 2 on the driver joins the power tree, and its only power state change is to stop the
// radio on a suspend: the scan is stopped, the virtual device deleted, the copy engine's rings disabled, WLAN_EN taken low and
// PERST# asserted. The rails stay voted (Bluetooth shares them) and nothing is brought back on the wake: the Wi-Fi path is off
// until the next boot, which is what an attended bring-up level should do to the battery. The stop waits for the ladder's own
// thread instead of racing it, so setPowerState answers with a deadline and acknowledges when it is done.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoPowerContract.h>
#include "SmmuStream.h"
#include "WifiBringup.h"
#include "WifiChannels.h"
#include "WifiFirmware.h"
#include "WifiLink.h"
#include "WifiMhi.h"
#include "WifiSmmuWindow.h"
#include "WifiSurvey.h"
#include "WifiWlfw.h"

using namespace pianowifi;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_wifi_stage = 0;
}

namespace {

struct Args {
  uint32_t level, spmi, smmu, cmddb, hwcg, delay, repeat, samples;
  uint32_t gen, physeq, rcg, blind, release, stop, dmaMib, fwTimeoutMs;
  uint32_t iova, mode, shadow, qmiTimeoutMs;
  uint32_t wmiTimeoutMs, scanWaitMs, band, bssid;
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
  gArgs.gen = bootArg("piano-wifi-gen", kDefaultGen);
  if (!gArgs.gen || gArgs.gen > kMaxGen) gArgs.gen = kDefaultGen;
  gArgs.physeq = bootArg("piano-wifi-physeq", 1);
  gArgs.rcg = bootArg("piano-wifi-rcg", 1);
  gArgs.blind = bootArg("piano-wifi-blind", 0);
  gArgs.release = bootArg("piano-wifi-release", 0);
  gArgs.stop = bootArg("piano-wifi-stop", 0);
  // Level 5 needs the firmware's regions, the two microcodes and the router's buffers in the same window as level 4's image.
  const uint32_t dmaDefault = (gArgs.level >= 5 ? kDmaWindowBytesL5 : kDmaWindowBytes) >> 20;
  gArgs.dmaMib = bootArg("piano-wifi-dma", dmaDefault);
  if (gArgs.dmaMib < 12 || gArgs.dmaMib > 96) gArgs.dmaMib = dmaDefault;
  gArgs.dmaMib &= ~1u;   // whole 2 MiB blocks: the window's own mapping (piano-wifi-iova) maps it in blocks of that size
  gArgs.fwTimeoutMs = bootArg("piano-wifi-fwtimeout", 20000);
  if (gArgs.fwTimeoutMs < 1000 || gArgs.fwTimeoutMs > 60000) gArgs.fwTimeoutMs = 20000;
  gArgs.iova = bootArg("piano-wifi-iova", 0);
  // Level 6 needs the firmware's radio running, which is what level 5's last three messages do.
  gArgs.mode = bootArg("piano-wifi-mode", gArgs.level >= 6 ? 1 : 0);
  gArgs.shadow = bootArg("piano-wifi-shadow", 0);
  gArgs.qmiTimeoutMs = bootArg("piano-wifi-qmitimeout", kWlfwTimeoutMs);
  if (gArgs.qmiTimeoutMs < 1000 || gArgs.qmiTimeoutMs > 60000) gArgs.qmiTimeoutMs = kWlfwTimeoutMs;
  gArgs.wmiTimeoutMs = bootArg("piano-wifi-wmitimeout", kWmiTimeoutMs);
  if (gArgs.wmiTimeoutMs < 1000 || gArgs.wmiTimeoutMs > 60000) gArgs.wmiTimeoutMs = kWmiTimeoutMs;
  gArgs.scanWaitMs = bootArg("piano-wifi-scanwait", kScanWaitMsDefault);
  if (gArgs.scanWaitMs < 1000 || gArgs.scanWaitMs > 60000) gArgs.scanWaitMs = kScanWaitMsDefault;
  gArgs.band = bootArg("piano-wifi-band", 1);
  gArgs.bssid = bootArg("piano-wifi-bssid", 0);
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
// The arbiter as the survey read it, kept for levels 2 and up (one regulator read at a time through the same channel).
Arbiter gArbiter;
bool gArbiterReady;

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

// What levels 2 and up need from level 1: each rail's RPMh address (cmd-db) and the PPID its regulator answered on (the SPMI
// survey, which also settles the LDO page rule). 0 means "not found", and a rail without both is never voted.
uint32_t gRailAddress[kRailCount];
uint16_t gRailPpid[kRailCount];

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
    if (i < kRailCount) gRailAddress[i] = rc ? 0 : address;
    n += size_t(rc ? snprintf(line + n, sizeof line - n, "%s%s none (%d)", i ? ", " : "", name, rc)
                   : snprintf(line + n, sizeof line - n, "%s%s %#x", i ? ", " : "", name, address));
  }
  IOFreeData(copy, kCmdDbBytes);
  note("cmd-db: %s", line);
}

// Also keeps the arbiter for levels 2 and up (hwPmic reads one regulator through the same observer channel).
void reportPmic()
{
  PmicSurvey s;
  surveyPmic(gSpmi, s);
  gArbiter = s.arbiter;
  gArbiterReady = s.arbiter.v7;
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
    gRailPpid[i] = p->ppid;
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

// ================================================================================= levels 2 and up: the windows and the writes
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }
inline void dsb() { __asm__ volatile("dsb sy" ::: "memory"); }

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// Levels above 1 write, so each block is mapped as one window (level 1's page-at-a-time loads stay for the survey). hwRead and
// hwWrite refuse an offset outside the window: an offset that came from the hardware (all ones while the endpoint does not answer)
// must never reach Device memory unaligned or out of range, which is a panic.
struct WindowMap {
  uint64_t pa;
  uint32_t bytes;
  const char *name;
  IOMemoryMap *map;
  uintptr_t va;
};
WindowMap gWin[kWinCount] = {
  {kGcc, 0x80000, "gcc", nullptr, 0},
  {kTlmm, 0x70000, "tlmm", nullptr, 0},
  {kTcsrPcie0ClkRef & ~0xfffull, 0x1000, "tcsr", nullptr, 0},
  {kParf, uint32_t(kParfBytes), "parf", nullptr, 0},
  {kPhy, uint32_t(kPhyBytes), "phy", nullptr, 0},
  {kDbi, uint32_t(kDbiBytes), "dbi", nullptr, 0},
  {kAtu, uint32_t(kAtuBytes), "atu", nullptr, 0},
  {kConfig, 0x1000, "cfg", nullptr, 0},
};

bool mapWindow(WindowMap &w)
{
  if (w.va) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(w.pa, w.bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return false;
  w.map = map;
  w.va = uintptr_t(map->getVirtualAddress());
  return true;
}

bool mapWindows()
{
  for (unsigned i = 0; i < kWinCount; ++i)
    if (!mapWindow(gWin[i])) {
      note("cannot map the %s window (%#llx + %#x)", gWin[i].name, (unsigned long long)gWin[i].pa, gWin[i].bytes);
      return false;
    }
  return true;
}

void unmapWindows()
{
  for (unsigned i = 0; i < kWinCount; ++i)
    if (gWin[i].map) {
      gWin[i].map->release();
      gWin[i].map = nullptr;
      gWin[i].va = 0;
    }
}

// Every write is logged once with what it is and why; a repeat of the same kind (the 126 PHY sequence writes, the 256 cleared
// BDF-to-SID words) is counted instead of logged again.
unsigned gWrites, gBadAccess;
const char *gSeen[64];
unsigned gSeenCount, gSeenRepeats[64];

uint32_t hwRead(void *, unsigned win, uint32_t off)
{
  if (win >= kWinCount || !gWin[win].va || (off & 3) || off + 4 > gWin[win].bytes) {
    ++gBadAccess;
    return 0xffffffffu;
  }
  return rd32(gWin[win].va + off);
}

void hwWrite(void *, unsigned win, uint32_t off, uint32_t value, const char *what)
{
  if (win >= kWinCount || !gWin[win].va || (off & 3) || off + 4 > gWin[win].bytes) {
    ++gBadAccess;
    note("refused a write outside the %s window: +%#x = %08x (%s)", win < kWinCount ? gWin[win].name : "?", off, value, what);
    return;
  }
  wr32(gWin[win].va + off, value);
  dsb();
  ++gWrites;
  for (unsigned i = 0; i < gSeenCount; ++i)
    if (gSeen[i] == what) {
      ++gSeenRepeats[i];
      return;
    }
  if (gSeenCount < 64) {
    gSeen[gSeenCount] = what;
    gSeenRepeats[gSeenCount++] = 0;
  }
  note("write %s+%#x = %08x: %s", gWin[win].name, off, value, what);
}

// The writes of the level just finished (the counts are deltas, so each level's line stands on its own).
unsigned gWritesBefore, gKindsBefore;
void writeSummary(const char *level)
{
  char line[360];
  size_t n = 0;
  for (unsigned i = gKindsBefore; i < gSeenCount && n + 40 < sizeof line; ++i)
    if (gSeenRepeats[i]) n += size_t(snprintf(line + n, sizeof line - n, "%s%s x%u", n ? ", " : "", gSeen[i], gSeenRepeats[i] + 1));
  note("%s: %u writes (%u kinds, %u refused accesses in all)%s%s", level, gWrites - gWritesBefore, gSeenCount - gKindsBefore,
       gBadAccess, n ? "; repeated: " : "", n ? line : "");
  gWritesBefore = gWrites;
  gKindsBefore = gSeenCount;
}

void hwUdelay(void *, unsigned us) { IODelay(us); }
void hwMsleep(void *, unsigned ms) { IOSleep(ms); }
uint64_t hwNow(void *) { return nowMs(); }
void hwStep(void *, int step, const char *what)
{
  piano_wifi_stage = step;
  note("step %d: %s", step, what);
}

// ---- PianoA830's lock words (package/PianoA830/adreno830/platform/sm8750/Sm8750Rpmh.h and Sm8750GpuPower.c): one 32-bit word
// each, 0 = free, else the holder's tag. PianoA830 is listed before PianoWifi in configs/k27-native-kexts.txt and is named as a
// library in PianoWifi.json, so the KC linker (no weak imports) resolves both to A830's single copy, the way PianoPower,
// PianoKeyboard (DRV-2) and PianoTouch (the GCC vote words) do.
extern "C" {
extern volatile uint32_t sm8750_gcc_vote_owner;
extern volatile uint32_t sm8750_rpmh_drv2_owner;
}
constexpr uint32_t kTag = 0x57494649;   // "WIFI"
constexpr unsigned kLockSteps = 200000, kLockStepUs = 10;

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
    IODelay(kLockStepUs);
  }
}
void lockRelease(volatile uint32_t *word)
{
  uint32_t expected = kTag;
  __atomic_compare_exchange_n(word, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

// The GCC vote words and the collapse word are read-modify-written under A830's lock word (PianoTouch's protocol: 10 us steps, at
// most 50 ms; a holder that keeps it longer than that is reported and the update goes ahead, as the bits are only ever set).
int hwVoteLock(void *, int take)
{
  if (!take) {
    lockRelease(&sm8750_gcc_vote_owner);
    return 0;
  }
  uint32_t waits = 0;
  if (lockAcquire(&sm8750_gcc_vote_owner, &waits, 5000)) {
    if (waits) note("GCC vote lock taken after %u waits of %u us", waits, kLockStepUs);
    return 0;
  }
  note("GCC vote lock held by %#x for 50 ms: the votes are set anyway (bits are only ever set)",
       __atomic_load_n(&sm8750_gcc_vote_owner, __ATOMIC_ACQUIRE));
  return 0;
}

// ---- one RPMh VRM transaction on apps DRV-2, under the DRV-2 lock (the protocol of PianoA830's Sm8750Rpmh.h, as PianoPower and
// PianoKeyboard run it): an idle ACTIVE TCS, its commands, the trigger, completion polled for at most 100 ms, the TCS reclaimed.
// A TCS that did not complete is left alone and never used again.
struct RscLayout {
  uint32_t tcsStride, cmdStride, irqStatus, irqClear, waitForCmpl, control, status, cmdEnable, cmdMsgid, cmdAddr, cmdData;
};
constexpr RscLayout rscLayoutFor(uint32_t drvId)
{
  return ((drvId >> 16) & 0xff) >= 3 ? RscLayout{672, 24, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c}
                                     : RscLayout{672, 20, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38};
}
constexpr uint32_t kAmcEnable = 1u << 16, kAmcTrigger = 1u << 24, kMsgidWriteResp = 8u | 1u << 8 | 1u << 16,
                   kCmdCompleted = 1u << 16;
IOMemoryMap *gRscMap;
uintptr_t gRscVa;
RscLayout gRsc;
uint32_t gRscId;
bool gTcsStuck[3];

uint32_t rscRd(uint32_t at) { return rd32(gRscVa + kRscTcsBlock + at); }
void rscWr(uint32_t at, uint32_t v)
{
  wr32(gRscVa + kRscTcsBlock + at, v);
  dsb();
}

bool mapRsc()
{
  if (gRscVa) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(kRscDrv2, kRscDrv2Bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) {
    note("rsc: cannot map apps DRV-2 (%#llx)", (unsigned long long)kRscDrv2);
    return false;
  }
  gRscMap = map;
  gRscVa = uintptr_t(map->getVirtualAddress());
  gRscId = rd32(gRscVa);
  gRsc = rscLayoutFor(gRscId);
  note("rsc: apps DRV-2 id %#x (version %u.%u: %s layout), TCS block +%#x", gRscId, (gRscId >> 16) & 0xff, (gRscId >> 8) & 0xff,
       ((gRscId >> 16) & 0xff) >= 3 ? "v3" : "v2", kRscTcsBlock);
  return true;
}

int vrmLocked(const char *resource, uint32_t base, const VrmCmd *cmds, unsigned count)
{
  const RscLayout &l = gRsc;
  static const unsigned kOrder[3] = {2, 1, 0};
  int tcs = -1;
  for (unsigned k = 0; k < 3 && tcs < 0; ++k)
    if (!gTcsStuck[kOrder[k]] && rscRd(l.tcsStride * kOrder[k] + l.status)) tcs = int(kOrder[k]);
  if (tcs < 0) {
    note("rsc: %s: no idle ACTIVE TCS, nothing sent", resource);
    return -16;
  }
  const uint32_t at = l.tcsStride * unsigned(tcs), bit = 1u << tcs, irqEnable = rscRd(0), mask = (1u << count) - 1;
  if (rscRd(l.irqStatus) & bit) rscWr(l.irqClear, bit);
  for (unsigned c = 0; c < count; ++c) {
    const uint32_t cmd = at + l.cmdStride * c;
    rscWr(cmd + l.cmdMsgid, kMsgidWriteResp);
    rscWr(cmd + l.cmdAddr, base + cmds[c].reg);
    rscWr(cmd + l.cmdData, cmds[c].value);
  }
  rscWr(at + l.waitForCmpl, mask);
  rscWr(at + l.cmdEnable, mask);
  rscWr(0, irqEnable | bit);
  const uint32_t control = rscRd(at + l.control);
  rscWr(at + l.control, control & ~kAmcTrigger);
  rscWr(at + l.control, control & ~(kAmcTrigger | kAmcEnable));
  rscWr(at + l.control, kAmcEnable);
  rscWr(at + l.control, kAmcEnable | kAmcTrigger);
  bool done = false;
  unsigned waited = 0;
  uint32_t status[3] = {0, 0, 0};
  for (;; ++waited) {
    uint32_t all = kCmdCompleted;
    for (unsigned c = 0; c < count; ++c) {
      status[c] = rscRd(at + l.cmdStride * c + l.cmdData + 4);
      all &= status[c];
    }
    done = (rscRd(l.irqStatus) & bit) || all == kCmdCompleted;
    if (done || waited >= 100) break;
    IOSleep(1);
  }
  if (done) {
    rscWr(at + l.cmdEnable, 0);
    rscWr(l.irqClear, bit);
    rscWr(at + l.control, rscRd(at + l.control) & ~kAmcTrigger);
    rscWr(at + l.control, 0);
  } else {
    gTcsStuck[tcs] = true;
  }
  rscWr(0, irqEnable);
  note("rsc: %s (%#x): %u commands through TCS %d %s after %u ms (status %#x %#x %#x)", resource, base, count, tcs,
       done ? "completed" : "NOT completed (left in flight, not used again)", waited, status[0], status[1], status[2]);
  return done ? 0 : -60;
}

int hwVrm(void *, const char *resource, const VrmCmd *cmds, unsigned count, uint32_t *address)
{
  if (!gRscVa || !cmds || !count || count > 3) return -22;
  uint32_t base = 0;
  for (unsigned i = 0; i < kRailCount; ++i) {
    const char *a = kRails[i].cmdDb, *b = resource;
    unsigned k = 0;
    while (k < 8 && a[k] && a[k] == b[k]) ++k;
    if (k == 8 || a[k] == b[k]) base = gRailAddress[i];   // equal, or equal in the eight characters RPMh names have
  }
  if (address) *address = base;
  if (!base) {
    note("rsc: %s: no RPMh resource of that name in cmd-db, nothing sent", resource);
    return -19;
  }
  uint32_t waits = 0;
  const uint32_t held = __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE);
  if (held) note("rsc: the DRV-2 lock is held by %#x; waiting (at most 2 s)", held);
  if (!lockAcquire(&sm8750_rpmh_drv2_owner, &waits, kLockSteps)) {
    note("rsc: the DRV-2 lock stayed held by %#x for 2 s: %s not voted",
         __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE), resource);
    return -16;
  }
  const int rc = vrmLocked(resource, base, cmds, count);
  lockRelease(&sm8750_rpmh_drv2_owner);
  return rc;
}

// ---- one regulator over the SPMI observer channel (reads only, the same channel level 1 uses)
int hwPmic(void *, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts)
{
  if (!gArbiterReady || !ppid) return -19;
  const Apid found = spmiFindApid(gSpmi, gArbiter, ppid);
  if (found.apid < 0) return -19;
  uint8_t regs[7] = {};
  if (spmiRead(gSpmi, found.apid, 0x08, status, 1) != ReadResult::ok) return -5;
  if (spmiRead(gSpmi, found.apid, 0x40, regs, 7) != ReadResult::ok) return -5;
  if (millivolts) *millivolts = uint16_t(uint16_t(regs[0]) | uint16_t(regs[1]) << 8);
  if (mode) *mode = regs[5];
  if (enable) *enable = regs[6];
  return 0;
}

const Hw gHw = {nullptr, hwRead, hwWrite, hwUdelay, hwMsleep, hwNow, hwVoteLock, hwVrm, hwPmic, hwStep};

Options options()
{
  Options o;
  o.level = gArgs.level;
  o.gen = gArgs.gen;
  o.physeq = gArgs.physeq;
  o.rcg = gArgs.rcg;
  o.blind = gArgs.blind;
  o.release = gArgs.release;
  o.stopAfter = int(gArgs.stop);
  return o;
}

// ------------------------------------------------------------------------------------------------------ the levels' reports
void reportPower(const PowerState &p)
{
  for (unsigned i = 0; i < kRailCount; ++i) {
    const Rail &rail = kRails[i];
    const RailVote &r = p.rails[i];
    if (!rail.order) {
      note("rail %s %s (%s): %u mV en %02x -> %u mV en %02x; never voted (%s)", rail.supply, rail.regulator, rail.cmdDb,
           r.millivolts, r.enable, r.millivoltsAfter, r.enableAfter, rail.shared);
      continue;
    }
    note("rail %s %s (%s) %#x: before %s %u mV mode %02x en %02x -> sent mode %u%s%s -> after %s %u mV mode %02x en %02x (rc %d)",
         rail.supply, rail.regulator, rail.cmdDb, r.address, r.read ? "read" : "unread", r.millivolts, r.mode, r.enable,
         kVrmModeNpm, r.voted ? ", voltage" : " (voltage kept)", r.sentEnable ? ", enable" : "",
         r.readAfter ? "read" : "unread", r.millivoltsAfter, r.modeAfter, r.enableAfter, r.rc);
  }
  note("pins: WLAN_EN cfg %08x io %08x, PERST# cfg %08x io %08x, CLKREQ# cfg %08x io %08x, SW_CTRL io %08x (in %u)", p.wlanEnCfg,
       p.wlanEnIo, p.perstCfg, p.perstIo, p.clkreqCfg, p.clkreqIo, p.swCtrlIo, p.swCtrlIo & 1u);
  note("level 2: %u rails with a voltage, %u left at theirs; step %d (%s)", p.voted, p.kept, p.step,
       p.error ? errorName(p.error) : "done");
}

void reportLink(const LinkState &l)
{
  note("GCC: collapse %08x -> %08x, pcie0 gdsc %08x cfg %08x, phy gdsc %08x cfg %08x after %u polls", l.collapseBefore,
       l.collapseAfter, l.gdsc, l.gdscCfg, l.phyGdsc, l.phyGdscCfg, l.gdscPolls);
  note("GCC: votes 0x52008 %08x 0x52000 %08x, aux rcg %08x/%08x rchng rcg %08x/%08x pipe mux %08x, clkref %08x, branches-off %03x",
       l.vote8, l.vote0, l.auxRcg, l.auxCfg, l.rchngRcg, l.rchngCfg, l.pipeMux, l.clkref, l.branchOff);
  note("GCC: resets pcie0 %08x pcie0_phy %08x; pipe cbcr %08x", l.bcr[0], l.bcr[1], l.pipeCbcr);
  note("PARF: device type %08x phy_ctrl %08x dbi base %08x atu base %08x slave space hi %08x sys %08x ltssm %08x", l.parfDeviceType,
       l.parfPhyCtrl, l.parfDbiBase, l.parfAtuBase, l.parfSlvSpaceHi, l.parfSysCtrl, l.parfLtssm);
  note("PHY: %u writes, PCS status1 %08x (%s) after %u polls of 100 us", l.phyWrites, l.pcsStatus,
       (l.pcsStatus & kPcsPhyStatus) ? "not ready" : "ready", l.phyPolls);
  note("PARF: BDF-to-SID hash of %#x is %u, entry %08x, cfg %08x (%s)", kEndpointRid, l.sidHash, l.sidEntry, l.sidCfg,
       (l.sidCfg & kParfBdfToSidBypass) ? "bypass" : "table");
  note("root port: id %08x buses %08x command %08x pcie cap %#x link ctl2 %08x; PERST# low %u ms", l.rootId, l.busNumbers,
       l.rootCommand, l.expCap, l.linkCtl2, l.perstLowMs);
  note("link: debug0 %08x (LTSSM %#x %s) debug1 %08x (%s) status %08x (Gen%u x%u) after %u ms", l.debug0, l.debug0 & kLtssmMask,
       ltssmName(l.debug0), l.debug1, (l.debug1 & kPortDebug1LinkUp) ? "up" : "down", l.linkStatus, (l.linkStatus >> 16) & 0xfu,
       (l.linkStatus >> 20) & 0x3fu, l.linkMs);
  note("endpoint: id %08x (%s) class %08x subsystem %08x pcie cap %#x msi cap %#x after %u polls", l.endpointId,
       idIsEndpoint(l.endpointId) ? "17cb:110e, the WCN7860" : "not the WCN7860", l.endpointClass, l.endpointSubsys,
       l.endpointCap, l.endpointMsiCap, l.endpointPolls);
  note("BAR0: mask %08x -> %llu KiB at %#llx (value %08x), command %08x; iATU ctrl2 %08x/%08x, memory base/limit %08x", l.barMask,
       (unsigned long long)(l.barBytes / 1024), (unsigned long long)l.barAddress, l.barValue, l.command, l.atuCtrl2[0],
       l.atuCtrl2[1], l.memBaseLimit);
  note("level 3: step %d (%s)", l.step, l.error ? errorName(l.error) : "done");
}

// =========================================================================================== level 3's tail: the SMMU stream
// The endpoint's stream (0x1401) through a bypass context bank, exactly as PianoDwc3 gives stream 0x40 and NativeUfs the UFS one
// (SmmuStream.h): a free group and a free bank, nothing of an existing UFS / GPU / USB entry touched, and an entry the firmware
// already left that bypasses is kept as it is. piano-wifi-smmu=2 does the root port's stream (0x1400) as well; 0 skips both.
IOMemoryMap *gSmmuMap;
uintptr_t gSmmuVa;

uint32_t smmuRead(uint32_t off) { return rd32(gSmmuVa + off); }

// Mapped on demand, by level 3's tail and by level 5's window (which needs it even with piano-wifi-smmu=0).
bool mapSmmu()
{
  if (gSmmuVa) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(kSmmu, kSmmuBytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) {
    note("SMMU: cannot map %#llx", (unsigned long long)kSmmu);
    return false;
  }
  gSmmuMap = map;
  gSmmuVa = uintptr_t(map->getVirtualAddress());
  return true;
}

bool smmuStream(uint16_t stream)
{
  if (!mapSmmu()) return false;
  const SmmuPlan p = planSmmuStream(stream, kSmmuBytes, smmuRead);
  note("SMMU stream %#x: groups %u banks %u pages %u; %s slot %u (smr %08x s2cr %08x sctlr %08x cbar %08x cba2r %08x) -> %s bank %u%s%s",
       stream, p.groups, p.banks, p.pages, p.reuse ? "existing entry" : "free", p.slot, p.smr, p.s2cr, p.sctlr, p.cbar, p.cba2r,
       p.keep ? "keep" : "bypass", p.bank, p.error ? ": " : "", p.error ? p.error : "");
  if (p.error) return false;
  const uint64_t deadline = nowMs() + 100;
  const char *failure = applySmmuStream(p, stream, smmuRead, [](uint32_t off, uint32_t v) { wr32(gSmmuVa + off, v); dsb(); },
                                        [deadline] { return nowMs() >= deadline; });
  note("SMMU stream %#x: %s (slot %u: smr %08x s2cr %08x)", stream, failure ? failure : p.keep ? "left as it was" : "bypass set",
       p.slot, smmuRead(0x800 + 4 * p.slot), smmuRead(0xc00 + 4 * p.slot));
  return failure == nullptr;
}

// ================================================================================================== level 4: DMA, BAR0, MHI
// The DMA window: one physically contiguous range below 2^36 (what ath12k and the 13T's cnss2 both allow this chip), the cacheable
// alias cleaned out of the caches once and then only ever reached through a write-combined mapping, as PianoDwc3's gadget memory
// is: the endpoint's stream bypasses the SMMU, so a bus address is a physical address and nothing keeps the two views coherent.
IOBufferMemoryDescriptor *gDmaBuffer;
IOMemoryMap *gDmaMap;
Arena gArena;
uint64_t gDmaPa;

void cleanToPoc(const void *start, size_t length)
{
  uint64_t ctr = 0;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t line = uintptr_t(4) << ((ctr >> 16) & 15);
  for (uintptr_t a = uintptr_t(start) & ~(line - 1); a < uintptr_t(start) + length; a += line)
    __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
  dsb();
}

bool dmaWindow()
{
  if (gArena.va) return true;
  const uint64_t bytes = uint64_t(gArgs.dmaMib) << 20;
  IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), bytes,
      0x0000000fffe00000ull);   // 36 bits, 2 MiB aligned
  if (!buffer) {
    note("dma: no %llu MiB contiguous below 2^36", (unsigned long long)gArgs.dmaMib);
    return false;
  }
  if (buffer->prepare() != kIOReturnSuccess) {
    buffer->release();
    note("dma: the window could not be prepared");
    return false;
  }
  IOByteCount contiguous = 0;
  const uint64_t pa = buffer->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
  uint8_t *cached = static_cast<uint8_t *>(buffer->getBytesNoCopy());
  if (!pa || !cached || contiguous < bytes || pa + bytes > (1ull << 36)) {
    buffer->complete();
    buffer->release();
    note("dma: the window is not usable (pa %#llx contiguous %llu)", (unsigned long long)pa, (unsigned long long)contiguous);
    return false;
  }
  bzero(cached, size_t(bytes));
  cleanToPoc(cached, size_t(bytes));
  gDmaMap = buffer->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
  uint8_t *va = gDmaMap ? reinterpret_cast<uint8_t *>(gDmaMap->getVirtualAddress()) : nullptr;
  if (!va) {
    buffer->complete();
    buffer->release();
    note("dma: no write-combined mapping of the window");
    return false;
  }
  gDmaBuffer = buffer;
  gDmaPa = pa;
  // piano-wifi-iova=1: the chip's addresses are the 32-bit window's, which the SMMU translates to this range (iovaWindow()
  // below, called once stream 0x1401 has its bank); otherwise they are physical, which only works while the stream bypasses.
  arenaInit(gArena, va, gArgs.iova ? uint64_t(kIovaWindowBase) : pa, uint32_t(bytes));
  note("dma: %llu MiB at %#llx (write-combined mapping %p), the endpoint's addresses are %s",
       (unsigned long long)gArgs.dmaMib, (unsigned long long)pa, va,
       gArgs.iova ? "this window's 32-bit addresses (stream 0x1401 translates)" : "physical (stream 0x1401 bypasses)");
  return true;
}

// ---- the 32-bit window for the endpoint's stream (WifiSmmuWindow.h), with piano-wifi-iova=1 ----
// The page table is a buffer of its own, not part of the window: the chip can reach every byte of the window, and its own
// translation tables must not be among them. The table is cacheable and cleaned to the point of coherency once, as the SMMU's
// walker is not coherent with the CPU's caches (the bank walks non-cacheable).
IOBufferMemoryDescriptor *gIovaTable;
iova::Window gIovaWindow;
iova::Plan gIovaPlan;

bool iovaWindow()
{
  if (!mapSmmu()) return false;
  IOBufferMemoryDescriptor *table = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), iova::kTableBytes,
      0x0000000ffffff000ull);   // 36 bits, 4 KiB aligned
  if (!table || table->prepare() != kIOReturnSuccess) {
    if (table) table->release();
    note("iova: no page table of %llu bytes", (unsigned long long)iova::kTableBytes);
    return false;
  }
  IOByteCount contiguous = 0;
  const uint64_t tablePa = table->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
  uint64_t *words = static_cast<uint64_t *>(table->getBytesNoCopy());
  if (!tablePa || !words || contiguous < iova::kTableBytes) {
    table->complete();
    table->release();
    note("iova: the page table is not usable (pa %#llx)", (unsigned long long)tablePa);
    return false;
  }
  if (!iova::buildWindow(gIovaWindow, words, words + 512, tablePa, gDmaPa, uint32_t(uint64_t(gArgs.dmaMib) << 20),
                         kIovaWindowBase)) {
    table->complete();
    table->release();
    note("iova: the window (%#x + %u MiB over %#llx) does not fit the rule", kIovaWindowBase, gArgs.dmaMib,
         (unsigned long long)gDmaPa);
    return false;
  }
  cleanToPoc(words, size_t(iova::kTableBytes));
  gIovaTable = table;
  gIovaPlan = iova::plan(kStreamEndpoint, kSmmuBytes, [](uint32_t off) { return smmuRead(off); });
  note("iova: window %#x + %u MiB -> %#llx in %u blocks of 2 MiB, table at %#llx; SMMU groups %u banks %u pages %u: %s slot %u "
       "bank %u (output %u bits)",
       gIovaWindow.base, gIovaWindow.bytes >> 20, (unsigned long long)gDmaPa, gIovaWindow.blocks,
       (unsigned long long)tablePa, gIovaPlan.groups, gIovaPlan.banks, gIovaPlan.pages,
       gIovaPlan.reuse ? "the stream's own" : "free", gIovaPlan.slot, gIovaPlan.bank, iova::kPaBits[gIovaPlan.pasize]);
  if (gIovaPlan.error) {
    note("iova: no context bank for stream %#x: %s", kStreamEndpoint, gIovaPlan.error);
    return false;
  }
  // The self-check before the chip is given any of these addresses: every block translates to where the window really is.
  for (uint32_t block = 0; block < gIovaWindow.blocks; ++block) {
    const uint64_t at = uint64_t(gIovaWindow.base) + uint64_t(block) * kIovaBlockBytes;
    if (iova::translate(gIovaWindow, at) == gDmaPa + uint64_t(block) * kIovaBlockBytes) continue;
    note("iova: the table does not translate %#llx: the window is not usable", (unsigned long long)at);
    return false;
  }
  const uint64_t deadline = nowMs() + 100;
  const char *failure = iova::apply(gIovaPlan, kStreamEndpoint, tablePa, [](uint32_t off) { return smmuRead(off); },
                                    [](uint32_t off, uint32_t v) { wr32(gSmmuVa + off, v); dsb(); },
                                    [deadline] { return nowMs() >= deadline; });
  if (failure) {
    note("iova: stream %#x did not take the window: %s", kStreamEndpoint, failure);
    return false;
  }
  note("iova: stream %#x translates through bank %u (smr %08x s2cr %08x sctlr %08x)", kStreamEndpoint, gIovaPlan.bank,
       smmuRead(0x800 + 4 * gIovaPlan.slot), smmuRead(0xc00 + 4 * gIovaPlan.slot),
       smmuRead(gIovaPlan.cb + iova::kCbSctlr));
  return true;
}

IOMemoryMap *gBarMap;
uintptr_t gBarVa;
uint32_t gBarBytes;

bool mapBar(uint64_t pa, uint32_t bytes)
{
  if (gBarVa) return true;
  if (!pa || !bytes || bytes > 0x400000) return false;
  IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return false;
  gBarMap = map;
  gBarVa = uintptr_t(map->getVirtualAddress());
  gBarBytes = bytes;
  return true;
}

uint32_t barRd(void *, uint32_t off)
{
  if ((off & 3) || off + 4 > gBarBytes) {
    ++gBadAccess;
    return 0xffffffffu;
  }
  return rd32(gBarVa + off);
}
void barWr(void *, uint32_t off, uint32_t value)
{
  if ((off & 3) || off + 4 > gBarBytes) {
    ++gBadAccess;
    return;
  }
  wr32(gBarVa + off, value);
  dsb();
}
const MhiIo gMhiIo = {nullptr, barRd, barWr};

// The chip's own registers above 0x80000 through its remap window; level 4 reads only the SoC version, for the log, and level 6
// reaches the copy engine's rings the same way. One thread owns the window (this driver polls and never takes an interrupt), so
// the two accesses of a windowed read or write are never interleaved with another's.
uint32_t chipRead(uint32_t address)
{
  if (address < kChipWindowStart) return barRd(nullptr, address);
  barWr(nullptr, kChipWindowReg, kChipWindowEnable | ((address >> kChipWindowShift) & kChipWindowMask));
  (void)barRd(nullptr, kChipWindowReg);
  return barRd(nullptr, kChipWindowStart + (address & kChipWindowRange));
}
void chipWrite(uint32_t address, uint32_t value)
{
  if (address < kChipWindowStart) {
    barWr(nullptr, address, value);
    return;
  }
  barWr(nullptr, kChipWindowReg, kChipWindowEnable | ((address >> kChipWindowShift) & kChipWindowMask));
  (void)barRd(nullptr, kChipWindowReg);
  barWr(nullptr, kChipWindowStart + (address & kChipWindowRange), value);
}
uint32_t chipIoRead(void *, uint32_t address) { return chipRead(address); }
void chipIoWrite(void *, uint32_t address, uint32_t value) { chipWrite(address, value); }
const ChipIo gChipIo = {nullptr, chipIoRead, chipIoWrite};

MhiContexts gContexts;
uint32_t gMhiRegLen;

struct FirmwarePlan {
  FwLayout layout;
  uint32_t bytes = 0, segments = 0, tableEntries = 0;
  uint64_t sblBus = 0, tableBus = 0, contextBus = 0, segmentBus[kMhiMaxSegments] = {};
  const char *partition = nullptr;
  const char *directory = nullptr;
  int rc = 0;
};

// The image straight from the tablet's own modem_a into the DMA window: the boot loader into its buffer, each 512 KiB segment into
// its own, then the BHIe vector table. The file is never held whole in the heap and nothing of it is logged.
bool loadFirmware(FirmwarePlan &plan)
{
  plan.rc = piano_wifi_firmware_open(&plan.partition);
  if (plan.rc) {
    note("firmware: no partition (%d: %s)", plan.rc,
         plan.rc == PianoWifiFwNotReady ? "NativeUfs has not published it yet" : "not mounted");
    return false;
  }
  plan.rc = piano_wifi_firmware_stat(kAmssName, &plan.bytes, &plan.directory);
  if (plan.rc) {
    note("firmware: %s not found on %s (%d)", kAmssName, plan.partition, plan.rc);
    return false;
  }
  uint8_t magic[kElfMagicBytes] = {};
  const bool haveMagic = plan.bytes > kMhiSblBytes &&
                         !piano_wifi_firmware_read(kAmssName, kMhiSblBytes, magic, sizeof magic);
  if (!firmwareLayoutFromMagic(haveMagic ? magic : nullptr, plan.bytes, kMhiSblBytes, plan.layout)) {
    note("firmware: %s (%u bytes) has no usable layout", kAmssName, plan.bytes);
    return false;
  }
  note("firmware: %s/%s on %s, %u bytes: boot loader %u bytes, %s image %u bytes at %u", plan.directory, kAmssName,
       plan.partition, plan.bytes, plan.layout.sblBytes, plan.layout.split ? "a second ELF" : "the whole file",
       plan.layout.imageBytes, plan.layout.imageOffset);
  plan.segments = (plan.layout.imageBytes + kMhiSegBytes - 1) / kMhiSegBytes;
  if (plan.segments > kMhiMaxSegments) {
    note("firmware: %u segments of 512 KiB is more than the %u this driver plans for", plan.segments, kMhiMaxSegments);
    return false;
  }
  uint8_t *sbl = arenaAlloc(gArena, kMhiSblBytes, &plan.sblBus);
  if (!sbl) return note("firmware: the DMA window has no room for the boot loader"), false;
  if (piano_wifi_firmware_read(kAmssName, plan.layout.sblOffset, sbl, plan.layout.sblBytes))
    return note("firmware: the boot loader could not be read"), false;
  uint64_t addresses[kMhiMaxSegments] = {};
  for (uint32_t i = 0; i < plan.segments; ++i) {
    uint8_t *segment = arenaAlloc(gArena, kMhiSegBytes, &plan.segmentBus[i]);
    if (!segment) return note("firmware: the DMA window has no room for segment %u", i), false;
    const uint32_t done = i * kMhiSegBytes, left = plan.layout.imageBytes - done;
    if (piano_wifi_firmware_read(kAmssName, plan.layout.imageOffset + done, segment, left < kMhiSegBytes ? left : kMhiSegBytes))
      return note("firmware: segment %u could not be read", i), false;
    addresses[i] = plan.segmentBus[i];
  }
  BhieEntry *table = reinterpret_cast<BhieEntry *>(arenaAlloc(gArena, 4096, &plan.tableBus));
  if (!table) return note("firmware: the DMA window has no room for the vector table"), false;
  plan.tableEntries = bhieTable(table, 4096 / sizeof(BhieEntry), addresses, plan.layout.imageBytes, kMhiSegBytes);
  if (!plan.tableEntries) return note("firmware: the vector table does not fit"), false;
  dsb();
  note("firmware: in the window: boot loader at %#llx, %u segments, vector table at %#llx (%u entries)",
       (unsigned long long)plan.sblBus, plan.segments, (unsigned long long)plan.tableBus, plan.tableEntries);
  return true;
}

int level4(const LinkState &link, PowerState &power, const Options &opt)
{
  if (!mapBar(link.barAddress, link.barBytes > kBar0BytesExpected ? kBar0BytesExpected : link.barBytes)) {
    note("level 4: BAR0 (%#llx + %u) could not be mapped", (unsigned long long)link.barAddress, link.barBytes);
    return -14;
  }
  note("level 4: BAR0 mapped (%#llx + %#x); MHI version %08x, BHIOFF %08x, BHIEOFF %08x, MHISTATUS %08x",
       (unsigned long long)link.barAddress, gBarBytes, barRd(nullptr, kMhiRegLen), barRd(nullptr, kMhiBhiOff),
       barRd(nullptr, kMhiBhieOff), barRd(nullptr, kMhiStatus));
  MhiBoot boot;
  if (!mhiOpen(boot, gMhiIo, gBarBytes)) {
    note("level 4: the MHI register layout is not usable (reglen %08x bhi %08x bhie %08x): %s", boot.reglen, boot.bhi, boot.bhie,
         mhiFailName(boot.error));
    safeOff(gHw, power, opt);
    return -5;
  }
  gMhiRegLen = boot.reglen;
  const uint32_t version = chipRead(kChipTcsrSocHwVersion);
  note("level 4: BHI at %#x, BHIe at %#x, EE %s, state %#x; chip TCSR version %08x (major %u)", boot.bhi, boot.bhie,
       mhiEeName(mhiEeRead(boot)), mhiStateRead(boot), version, (version >> 8) & 0xfu);
  FirmwarePlan plan;
  if (!loadFirmware(plan)) {
    piano_wifi_firmware_close();
    safeOff(gHw, power, opt);
    return -2;
  }
  if (gArgs.level < 5) piano_wifi_firmware_close();   // level 5 reads its own files from the same partition
  MhiContexts &contexts = gContexts;
  uint64_t contextBus = 0;
  uint8_t *contextVa = arenaAlloc(gArena, kMhiContextBytes, &contextBus);
  if (!contextVa || !mhiContextsPlan(contexts, contextVa, contextBus, kMhiContextBytes)) {
    note("level 4: no room for the MHI contexts");
    safeOff(gHw, power, opt);
    return -12;
  }
  dsb();
  mhiContextsProgram(contexts, gMhiIo, boot.reglen);
  note("level 4: contexts at %#llx (channels %u, event rings %u: control at %#llx, data at %#llx, command ring at %#llx)",
       (unsigned long long)contextBus, kMhiChannels, kMhiEventRings, (unsigned long long)contexts.eventBus[0],
       (unsigned long long)contexts.eventBus[1], (unsigned long long)contexts.commandBus);
  uint32_t phase = mhiBegin(boot, 0, plan.sblBus, plan.layout.sblBytes, plan.tableBus,
                            plan.tableEntries * uint32_t(sizeof(BhieEntry)), uint32_t(nowMs()) | 1u);
  note("level 4: boot begun in phase %s (EE %s)", mhiPhaseName(phase), mhiEeName(boot.ee));
  const uint64_t started = nowMs();
  uint64_t since = started;
  uint32_t last = phase, steps = 0;
  while (phase != kMhiDone && phase != kMhiFailed) {
    if (phase == kMhiNeedContexts) {
      dsb();
      mhiContextsProgram(contexts, gMhiIo, boot.reglen);
      phase = mhiContextsDone(boot);
    } else {
      phase = mhiStep(boot);
      if (phase != kMhiNeedContexts && phase != kMhiDone && phase != kMhiFailed) mhiEventAck(contexts, gMhiIo, boot.reglen);
    }
    ++steps;
    const uint64_t now = nowMs();
    if (phase != last) {
      note("level 4: %s after %llu ms (EE %s, state %#x)", mhiPhaseName(phase), (unsigned long long)(now - started),
           mhiEeName(boot.ee), boot.state);
      last = phase;
      since = now;
      continue;
    }
    if (now - since > gArgs.fwTimeoutMs) {
      phase = mhiTimedOut(boot);
      break;
    }
    IOSleep(5);
  }
  note("level 4: %s after %llu ms, %u steps, %u context writes, %u event acknowledgements; EE %s, state %#x, BHI status %08x, "
       "error code %08x (%s)",
       phase == kMhiDone ? "mission mode" : "stopped", (unsigned long long)(nowMs() - started), steps, contexts.writes,
       contexts.acks, mhiEeName(mhiEeRead(boot)), boot.state, boot.lastStatus, boot.errcode, mhiFailName(boot.error));
  if (phase != kMhiDone) {
    safeOff(gHw, power, opt);
    return -1;
  }
  return 0;
}

// =============================================== level 5: the IPC router's channels, QRTR, and the firmware's QMI service
// The transport (WifiChannels.h) over the contexts level 4 gave the chip, the copied router (WifiQrtr.c) on top of it, and the
// WLFW client (WifiWlfw.h) on top of that. Everything is polled from this thread: no MSI, no interrupt, no allocation after the
// window, and every wait is bounded by piano-wifi-qmitimeout.
Ipcr gIpcr;
QcomQrtr gRouter;
WlfwClient gClient;
unsigned gQrtrUnrouted;

// The link's send: a packet that already sits in one of our outbound buffers is submitted as it is; a control packet the router
// built on its stack (hello, resume) is copied into a free buffer first.
int qrtrSend(void *, const uint8_t *packet, uint32_t length)
{
  for (unsigned i = 0; i < kIpcrOutBuffers; ++i)
    if (gIpcr.tx[i].va == packet) return ipcrSubmitTx(gIpcr, gMhiIo, i, length) ? 0 : -1;
  const int index = ipcrClaimTx(gIpcr);
  if (index < 0 || length > gIpcr.tx[index].bytes) return -1;
  memcpy(gIpcr.tx[index].va, packet, length);
  return ipcrSubmitTx(gIpcr, gMhiIo, unsigned(index), length) ? 0 : -1;
}

// A data packet for our port: the client's next message.
void qrtrReceive(void *, QcomQrtr *, uint32_t, uint32_t, uint32_t, const uint8_t *data, uint32_t length)
{
  wlfwReceive(gClient, data, length);
}

// What the client needs: a buffer with room for the router's header in front of the message, the send, the firmware files of the
// stock partition, memory of the window, and one line per step.
uint8_t *wlfwClaim(void *, uint32_t *capacity)
{
  const int index = ipcrClaimTx(gIpcr);
  if (index < 0) return nullptr;
  *capacity = gIpcr.tx[index].bytes - QCOM_QRTR_HEADER;
  return gIpcr.tx[index].va + QCOM_QRTR_HEADER;
}
int wlfwSendMessage(void *, uint8_t *buffer, uint32_t length)
{
  uint8_t *base = buffer - QCOM_QRTR_HEADER;
  uint32_t node = 0, port = 0;
  if (qcom_qrtr_find_service(&gRouter, kWlfwService, kWlfwInstanceWord, &node, &port)) return -1;
  for (unsigned i = 0; i < kIpcrOutBuffers; ++i)
    if (gIpcr.tx[i].va == base)
      return qcom_qrtr_send_buffer(&gRouter, base, gIpcr.tx[i].bytes, kQrtrClientPort, node, port, length);
  return -1;
}
uint32_t wlfwFileSize(void *, const char *name)
{
  uint32_t bytes = 0;
  return piano_wifi_firmware_stat(name, &bytes, nullptr) ? 0 : bytes;
}
int wlfwFileRead(void *, const char *name, uint64_t offset, void *to, uint32_t bytes)
{
  return piano_wifi_firmware_read(name, offset, to, bytes);
}
uint64_t wlfwMemory(void *, uint32_t bytes, uint8_t **va)
{
  uint64_t bus = 0;
  uint8_t *p = arenaAlloc(gArena, bytes, &bus);
  if (!p) return 0;
  if (va) *va = p;
  return bus;
}
void wlfwStepNote(void *, int step, const char *what)
{
  piano_wifi_stage = step;
  note("step %d: %s", step, what);
}

// One turn: the event rings walked, every received packet given to the router, the buffers given back to the chip. Returns how
// many events were seen; `stop` is set when the chip reported an environment this driver cannot go on from.
unsigned pump(bool &stop)
{
  bool fatal = false;
  const unsigned seen = ipcrPoll(gIpcr, gContexts, gMhiIo, [&](uint32_t kind, uint32_t, uint32_t code, uint32_t length,
                                                               uint8_t *va) {
    switch (kind) {
    case kIpcrPacket:
      if (qcom_qrtr_receive(&gRouter, va, length)) ++gQrtrUnrouted;
      break;
    case kIpcrState:
      note("level 5: the chip reports state %#x", code);
      break;
    case kIpcrEe:
      note("level 5: the chip reports environment %s", mhiEeName(code));
      if (code == kEeRddm) fatal = true;
      break;
    default:
      break;
    }
  });
  if (seen) ipcrPostRx(gIpcr, gMhiIo);
  stop = fatal;
  return seen;
}

// A channel started: one command on the primary ring, then its completion event.
bool startChannel(uint32_t channel)
{
  if (!ipcrCommand(gIpcr, gMhiIo, channel, kTreCmdStart)) {
    note("level 5: the start command for channel %u could not be queued (%s)", channel,
         gIpcr.error ? gIpcr.error : "a command is in flight");
    return false;
  }
  const uint64_t deadline = nowMs() + kMhiCommandTimeoutMs;
  while (nowMs() < deadline) {
    bool stop = false;
    pump(stop);
    if (stop) return false;
    if ((channel == kChanIpcrOut && gIpcr.started[0]) || (channel == kChanIpcrIn && gIpcr.started[1])) return true;
    if (gIpcr.commandErrors) {
      note("level 5: the chip refused to start channel %u (completion code %#x)", channel, gIpcr.lastCode);
      return false;
    }
    IOSleep(2);
  }
  note("level 5: no completion for the start of channel %u within %u ms", channel, kMhiCommandTimeoutMs);
  return false;
}

int level5(PowerState &power, const Options &opt)
{
  stage(500, "level 5: the IPC router's channels, QRTR, and the WLAN firmware's QMI service");
  if (!ipcrPlan(gIpcr, gArena, gContexts)) {
    stage(-500, "level 5: the DMA window has no room for the channels' rings and buffers");
    safeOff(gHw, power, opt);
    return -1;
  }
  if (!ipcrOpen(gIpcr, gMhiIo, gMhiRegLen)) {
    stage(-500, "level 5: the chip's doorbell arrays are not where the register space says");
    safeOff(gHw, power, opt);
    return -2;
  }
  note("level 5: channels %u and %u planned (%u elements each, %u KiB of buffers), channel doorbells at %#x, event doorbells "
       "at %#x",
       kChanIpcrOut, kChanIpcrIn, kIpcrElements, (kIpcrRxBuffers + kIpcrOutBuffers) * kIpcrBufferBytes / 1024, gIpcr.chdb,
       gIpcr.erdb);
  if (!startChannel(kChanIpcrOut) || !startChannel(kChanIpcrIn)) {
    stage(-501, "level 5: a channel did not start");
    safeOff(gHw, power, opt);
    return -3;
  }
  const unsigned posted = ipcrPostRx(gIpcr, gMhiIo);
  note("level 5: both channels run (states %#x / %#x), %u receive buffers posted", ipcrChanState(gIpcr.out),
       ipcrChanState(gIpcr.in), posted);
  // The router: our hello, then the firmware's hello and its service announcement.
  qcom_qrtr_init(&gRouter, kQrtrLocalNode, qrtrSend, nullptr);
  if (qcom_qrtr_add_client(&gRouter, kQrtrClientPort, qrtrReceive, nullptr)) {
    stage(-502, "level 5: the router's client port could not be added");
    safeOff(gHw, power, opt);
    return -4;
  }
  qcom_qrtr_start(&gRouter);
  uint32_t node = 0, port = 0;
  uint64_t deadline = nowMs() + gArgs.qmiTimeoutMs;
  while (nowMs() < deadline) {
    bool stop = false;
    pump(stop);
    if (stop) break;
    if (!qcom_qrtr_find_service(&gRouter, kWlfwService, kWlfwInstanceWord, &node, &port)) break;
    IOSleep(5);
  }
  if (!gRouter.hello_received || qcom_qrtr_find_service(&gRouter, kWlfwService, kWlfwInstanceWord, &node, &port)) {
    note("level 5: hello %s, %u services announced, %u packets in (%u unrouted)", gRouter.hello_received ? "answered" : "unanswered",
         gRouter.remote_count, gRouter.rx_packets, gRouter.rx_unrouted);
    stage(-502, "level 5: the firmware's QMI service was not announced on the router");
    safeOff(gHw, power, opt);
    return -5;
  }
  note("level 5: the router is up (our node %u port %#x, the chip's node %u; the firmware's service %u instance %#x on port %u)",
       kQrtrLocalNode, kQrtrClientPort, gRouter.remote_node, kWlfwService, kWlfwInstanceWord, port);
  // The service's exchange.
  WlfwIo io;
  io.ctx = nullptr;
  io.claim = wlfwClaim;
  io.send = wlfwSendMessage;
  io.file_size = wlfwFileSize;
  io.file_read = wlfwFileRead;
  io.memory = wlfwMemory;
  io.step = wlfwStepNote;
  const uint64_t started = nowMs();
  if (!wlfwStart(gClient, io, gArgs.mode, gArgs.shadow)) {
    note("level 5: the first request was not sent: %s", wlfwErrorName(gClient.error));
    stage(-503, "level 5: the QMI exchange did not start");
    safeOff(gHw, power, opt);
    return -6;
  }
  int last = gClient.step;
  deadline = nowMs() + gArgs.qmiTimeoutMs;
  while (gClient.step != kWlfwStepDone && gClient.step != kWlfwStepStopped) {
    bool stop = false;
    pump(stop);
    if (stop) {
      wlfwTimedOut(gClient);
      note("level 5: the chip went to its crash dump environment during %s", wlfwStepName(last));
      break;
    }
    if (gClient.step != last) {   // a step moved: its own deadline
      last = gClient.step;
      deadline = nowMs() + gArgs.qmiTimeoutMs;
      continue;
    }
    if (nowMs() > deadline) {
      wlfwTimedOut(gClient);
      break;
    }
    IOSleep(2);
  }
  // What the firmware said, and what was sent to it.
  note("level 5: the chip: chip %#x family %#x system-on-chip %#x, board %#x, %u radio(s)%s", gClient.chipId,
       gClient.chipFamily, gClient.socId, gClient.boardId, gClient.radios, gClient.singleChipMlo ? ", single-chip multi-link" : "");
  note("level 5: the firmware: version %#x built %s, capabilities %#llx, status %#llx, %u MAC(s), %u mV", gClient.fwVersion,
       gClient.buildTimestamp[0] ? gClient.buildTimestamp : "?", (unsigned long long)gClient.fwCaps,
       (unsigned long long)gClient.fwStatus, gClient.macs, gClient.voltageMv);
  note("level 5: memory: %u request(s), %u region(s) of %u KiB asked for, %u KiB given%s", gClient.memRequests,
       gClient.regionCount, gClient.regionBytes / 1024, gClient.regionGiven / 1024,
       gClient.memDelayed ? " (the last answer asked the firmware to come back with smaller pieces)" : "");
  for (uint32_t i = 0; i < gClient.regionCount && i < 8; ++i)
    note("level 5: region %u: %u KiB of %s at %#llx", i, gClient.regions[i].size / 1024,
         wlfwMemTypeName(gClient.regions[i].type), (unsigned long long)gClient.regions[i].address);
  note("level 5: %u requests, %u responses, %u indications, %u download segments, %u shadow registers, %u unexpected messages",
       gClient.requests, gClient.responses, gClient.indications, gClient.segments, gClient.shadowCount, gClient.unexpected);
  note("level 5: the transport: %u packets out, %u in, %u completions, %u commands, %u events dropped, %u refused claims; "
       "the router: %u packets in, %u data, %u unrouted, %u refused sends",
       gIpcr.sent, gIpcr.received, gIpcr.txDone, gIpcr.commands, gIpcr.dropped, gIpcr.refused, gRouter.rx_packets,
       gRouter.rx_data, gRouter.rx_unrouted, gRouter.tx_refused);
  const bool done = gClient.step == kWlfwStepDone;
  note("level 5: %s after %llu ms (step %d: %s%s%s)", done ? "the firmware is ready" : "stopped",
       (unsigned long long)(nowMs() - started), done ? gClient.step : gClient.failedStep,
       wlfwStepName(done ? gClient.step : gClient.failedStep), done ? "" : ", ", done ? "" : wlfwErrorName(gClient.error));
  if (!done) {
    safeOff(gHw, power, opt);
    return -7;
  }
  return 0;
}

// ============================================== level 6: the copy engine, the host transport and the firmware's messages
// The rings come out of the same window as levels 4 and 5 (the firmware's regions are already in it), the pipes are the ones
// level 5 described to the firmware, and everything is polled from this one thread: no MSI, no interrupt, no allocation after
// the window. Every wait is bounded by piano-wifi-wmitimeout (and piano-wifi-scanwait for the scan).
Ce gCe;
Link gLink;
volatile bool gLinkRunning;         // the ladder's thread is inside level 6: the power hook asks it to stop instead of racing
volatile uint32_t gStopRequested;   // set by the power hook, read by the ladder each turn

void linkNote(void *, int step, const char *what)
{
  piano_wifi_stage = step;
  note("step %d: %s", step, what);
}

// The scan's result in the registry: one entry per network, and a line for the log. The addresses are published whole (the
// macOS side needs them to join later) and masked in the log unless piano-wifi-bssid=1 (docs/drivers/wifi.md section 11).
void publishScan(IOService *owner, const ScanList &list, bool full)
{
  OSArray *array = OSArray::withCapacity(list.count ? list.count : 1);
  if (!array) return;
  for (uint32_t i = 0; i < list.count; ++i) {
    const ScanEntry &entry = list.entries[i];
    OSDictionary *one = OSDictionary::withCapacity(8);
    if (!one) break;
    char bssid[24] = {};
    scanFormatBssid(entry, true, bssid, sizeof bssid);
    OSString *ssid = OSString::withCString(entry.ssidLength ? entry.ssid : "");
    OSString *address = OSString::withCString(bssid);
    OSNumber *channel = OSNumber::withNumber(uint64_t(entry.channel), 32);
    OSNumber *freq = OSNumber::withNumber(uint64_t(entry.freq), 32);
    OSNumber *rssi = OSNumber::withNumber(uint64_t(uint32_t(entry.rssi)), 32);
    OSNumber *beacons = OSNumber::withNumber(uint64_t(entry.beacons), 32);
    OSNumber *probes = OSNumber::withNumber(uint64_t(entry.probes), 32);
    if (ssid) one->setObject("ssid", ssid);
    if (address) one->setObject("bssid", address);
    if (channel) one->setObject("channel", channel);
    if (freq) one->setObject("freq", freq);
    if (rssi) one->setObject("rssi", rssi);
    if (beacons) one->setObject("beacons", beacons);
    if (probes) one->setObject("probes", probes);
    one->setObject("hidden", entry.hidden ? kOSBooleanTrue : kOSBooleanFalse);
    OSSafeReleaseNULL(ssid);
    OSSafeReleaseNULL(address);
    OSSafeReleaseNULL(channel);
    OSSafeReleaseNULL(freq);
    OSSafeReleaseNULL(rssi);
    OSSafeReleaseNULL(beacons);
    OSSafeReleaseNULL(probes);
    array->setObject(one);
    one->release();
  }
  owner->setProperty("piano-wifi-scan", array);
  array->release();
  char line[200];
  snprintf(line, sizeof line, "%u network(s), %u frame(s) (%u beacon, %u probe), %u ignored, %u dropped", list.count,
           list.frames, list.beacons, list.probes, list.ignored, list.dropped);
  owner->setProperty("piano-wifi-scan-summary", line);
  for (uint32_t i = 0; i < list.count; ++i) {
    const ScanEntry &entry = list.entries[i];
    char bssid[24] = {};
    scanFormatBssid(entry, full, bssid, sizeof bssid);
    note("level 6: network %u: ssid \"%s\"%s bssid %s channel %u (%u MHz) signal %d dBm, %u beacon(s) %u probe(s)", i,
         entry.ssidLength ? entry.ssid : "", entry.hidden ? " (it does not say its name)" : "", bssid, entry.channel,
         entry.freq, int(entry.rssi), entry.beacons, entry.probes);
  }
}

// The teardown: the scan stopped and the virtual device deleted, waited for (bounded), then the rings disabled.
void linkTeardown()
{
  const uint64_t deadline = nowMs() + 1000;
  bool idle = false;
  while (!idle && nowMs() < deadline) {
    idle = linkStopRequest(gLink, nowMs());
    if (!idle) IOSleep(5);
  }
  linkDisable(gLink);
  note("level 6: the teardown: the scan %s, the virtual device %s, the rings disabled%s",
       !gLink.scanStarted ? "was not running" : gLink.scanTimedOut ? "was stopped by this driver" : "ended by itself",
       gLink.deleted ? "deleted" : "was not created",
       idle ? "" : " (the chip had not taken everything: the rings were disabled anyway)");
}

int level6(IOService *owner, PowerState &power, const Options &opt)
{
  stage(600, "level 6: the copy engine, the host transport and the firmware's messages");
  if (!cePlan(gCe, gArena)) {
    stage(-600, "level 6: the DMA window has no room for the copy engine's rings and buffers");
    safeOff(gHw, power, opt);
    return -1;
  }
  ceProgram(gCe, gChipIo);
  unsigned posted = 0;
  for (unsigned i = 0; i < kCeHostPipeCount; ++i) posted += cePostRx(gCe, gChipIo, i);
  note("level 6: %u pipes: %u, %u, %u and %u; %u receive buffers posted, %u register writes", kCeHostPipeCount,
       kCeHostPipes[0].pipe, kCeHostPipes[1].pipe, kCeHostPipes[2].pipe, kCeHostPipes[3].pipe, posted, gCe.writes);
  LinkOptions options;
  options.timeoutMs = gArgs.wmiTimeoutMs;
  options.scanWaitMs = gArgs.scanWaitMs;
  options.with5GHz = gArgs.band != 0;
  options.passive = true;
  gLink.note = linkNote;
  gLink.noteCtx = nullptr;
  const uint64_t started = nowMs();
  linkBegin(gLink, gCe, gChipIo, options, started);
  gLinkRunning = true;
  const uint64_t overall = started + uint64_t(gArgs.wmiTimeoutMs) * 6 + gArgs.scanWaitMs;
  int step = gLink.step;
  while (step != kLinkDone && step != kLinkStopped) {
    if (__atomic_load_n(&gStopRequested, __ATOMIC_ACQUIRE)) {
      note("level 6: a suspend asked the radio to stop during %s", linkStepName(step));
      break;
    }
    step = linkRun(gLink, nowMs());
    if (nowMs() > overall) {
      note("level 6: the ladder as a whole ran out of time during %s", linkStepName(step));
      break;
    }
    if (step != kLinkDone && step != kLinkStopped) IOSleep(2);
  }
  // What the firmware said, and what the scan found.
  note("level 6: the transport: %u credits of %u bytes, %u endpoints, version %#x; our endpoint %d (pipes %u out, %u in), "
       "%u credit reports (%u credits), %u control messages",
       gLink.htc.credits, gLink.htc.creditSize, gLink.htc.endpoints, gLink.htc.version, gLink.endpoint,
       gLink.endpoint > 0 ? gLink.htc.ep[gLink.endpoint].ulPipe : 0u,
       gLink.endpoint > 0 ? gLink.htc.ep[gLink.endpoint].dlPipe : 0u, gLink.htc.creditReports, gLink.htc.creditsGiven,
       gLink.controlMessages);
  note("level 6: the chip: %u radio chain(s), board %#x, high-throughput %#x very-high-throughput %#x, system %#x, "
       "%u scan channel(s), %u radio(s), chain mask %#x, %u memory request(s)",
       gLink.caps.rfChains, gLink.caps.boardId, gLink.caps.htCaps, gLink.caps.vhtCaps, gLink.caps.sysCaps,
       gLink.caps.scanChannels, gLink.caps.supportedMacs, gLink.caps.chainMask, gLink.caps.memRequests);
  if (gLink.ready.seen) {
    char mac[24] = {};
    ScanEntry asAddress;
    bool zero = true;
    for (unsigned i = 0; i < 6; ++i) {
      asAddress.bssid[i] = gLink.ready.mac[i];
      zero = zero && gLink.ready.mac[i] == 0;
    }
    scanFormatBssid(asAddress, gArgs.bssid != 0, mac, sizeof mac);
    note("level 6: the firmware is ready: status %#x, %u peer(s), %u extra address(es); the chip's own address %s%s",
         gLink.ready.status, gLink.ready.totalPeers, gLink.ready.extraMacs, zero ? "is all zeros" : mac,
         zero ? " (this chip's firmware reports none: the unit's own is in the persist partition, section 13.1)" : "");
    // An address the firmware does report is what the macOS side's en1 takes (PianoWifiEthernet); it is never logged whole
    // unless piano-wifi-bssid=1, and never committed (section 11).
    if (!zero) {
      OSData *bytes = OSData::withBytes(gLink.ready.mac, sizeof gLink.ready.mac);
      if (bytes) {
        owner->setProperty("piano-wifi-mac", bytes);
        bytes->release();
      }
    }
  }
  note("level 6: the scan: %u channel(s) asked for, started %u, %u scan event(s), %u channel visit(s), %s%s",
       gLink.request.channelCount, gLink.scanStarted ? 1u : 0u, gLink.scanEventsSeen, gLink.scanChannels,
       gLink.scanCompleted ? wmiScanReasonName(gLink.scanReason) : "it did not report an end",
       gLink.scanTimedOut ? " (after this driver stopped waiting for it)" : "");
  publishScan(owner, gLink.scan, gArgs.bssid != 0);
  note("level 6: %u commands, %u events (%u extended capability), %u management frames (%u refused by the firmware's own "
       "checks, %u unreadable), %u unexpected messages, %u messages dropped, %u credit waits; the copy engine: %u out "
       "(%u bytes), %u in (%u bytes), %u completions, %u refused claims, %u dropped",
       gLink.commands, gLink.events, gLink.extEvents, gLink.mgmtFrames, gLink.scan.badFrames, gLink.scan.ignored,
       gLink.unexpected, gLink.badMessages, gLink.creditWaits, gCe.sent, gCe.bytesOut, gCe.received, gCe.bytesIn,
       gCe.completions, gCe.refused, gCe.dropped);
  const bool done = gLink.step == kLinkDone;
  note("level 6: %s after %llu ms (step %d: %s%s%s)", done ? "the scan is done" : "stopped",
       (unsigned long long)(nowMs() - started), done ? gLink.step : gLink.failedStep,
       linkStepName(done ? gLink.step : gLink.failedStep), done ? "" : ", ", done ? "" : linkErrorName(gLink.error));
  linkTeardown();
  gLinkRunning = false;
  char state[200];
  snprintf(state, sizeof state, "step %d %s%s%s; %u network(s), endpoint %d, %u credits", done ? gLink.step : gLink.failedStep,
           linkStepName(done ? gLink.step : gLink.failedStep), done ? "" : ": ", done ? "" : linkErrorName(gLink.error),
           gLink.scan.count, gLink.endpoint, gLink.htc.credits);
  owner->setProperty("piano-wifi-level6", state);
  if (!done) {
    safeOff(gHw, power, opt);
    return -2;
  }
  return 0;
}

// The power contract (package/include/PianoPowerContract.h): on a suspend the radio stops. The ladder's own thread owns the
// hardware, so the hook asks it to stop and waits for it (bounded); then the scan and the virtual device are gone, the rings
// are disabled, and WLAN_EN goes low with PERST# asserted. Nothing is brought back on the wake.
PowerState gPowerStorage;       // the rails and pins as level 2 left them: what safeOff needs, from any thread
PowerState *gPowerState;
Options gPowerOptions;
bool gRadioStopped;

void stopRadio(const char *why)
{
  if (gRadioStopped || gArgs.level < 2) return;
  gRadioStopped = true;
  __atomic_store_n(&gStopRequested, 1u, __ATOMIC_RELEASE);
  const uint64_t deadline = nowMs() + 2000;
  while (gLinkRunning && nowMs() < deadline) IOSleep(10);
  note("%s: stopping the radio (the ladder's thread %s)", why, gLinkRunning ? "did not finish in 2 s" : "is done");
  if (!gLinkRunning && gLink.ce) linkTeardown();
  if (gPowerState && gWin[kWinTlmm].va) {
    safeOff(gHw, *gPowerState, gPowerOptions);
    note("%s: WLAN_EN is low and PERST# asserted; the rails stay voted (Bluetooth shares them) and the Wi-Fi path stays off "
         "until the next boot",
         why);
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoWifi : public IOService {
  OSDeclareDefaultStructors(PianoWifi)
  void run();
  static void thread(void *self, wait_result_t);

  static void stopThread(void *self, wait_result_t);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  void systemWillShutdown(IOOptionBits specifier) override;
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
  if (gArgs.level >= 2)
    note("levels: 1 survey, 2 power, 3 link%s%s%s (gen %u, physeq %u, rcg %u, blind %u, release %u, stop %u, dma %u MiB, "
         "phase timeout %u ms, qmi timeout %u ms, iova %u, mode %u, shadow %u, wmi timeout %u ms, scan wait %u ms, band %u, "
         "bssid %u)",
         gArgs.level >= 4 ? ", 4 MHI firmware" : "", gArgs.level >= 5 ? ", 5 QMI" : "",
         gArgs.level >= 6 ? ", 6 the copy engine, the transport and a scan" : "", gArgs.gen, gArgs.physeq, gArgs.rcg,
         gArgs.blind, gArgs.release, gArgs.stop, gArgs.dmaMib, gArgs.fwTimeoutMs, gArgs.qmiTimeoutMs, gArgs.iova, gArgs.mode,
         gArgs.shadow, gArgs.wmiTimeoutMs, gArgs.scanWaitMs, gArgs.band, gArgs.bssid);
  if (gArgs.level > 6) note("piano-wifi=%u: levels above 6 are not implemented; running up to level 6", gArgs.level);
  note("level 1: loads only, nothing written%s", gArgs.spmi || gArgs.level >= 2 ? "; SPMI observer read commands for the PMIC part"
                                                                               : "");
  setProperty("piano-wifi", gArgs.level, 32);
  // From level 2 on something is powered, so the driver joins the power tree: its kOff stops the radio (the power contract,
  // package/include/PianoPowerContract.h). Below level 2 nothing of the chip is on and there is nothing to do.
  if (gArgs.level >= 2) {
    const IOReturn rc = pianopower::join(this);
    if (rc != kIOReturnSuccess) note("the power tree refused this driver (%#x): a suspend will not stop the radio", rc);
  }
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

// The power contract: kOff stops the radio (and nothing comes back on the wake, §10.14). Stopping it waits for the ladder's own
// thread (at most 2 s) and then for the chip to take the two commands (at most 1 s), which is longer than the one second a
// synchronous answer may take — so the answer is a deadline in microseconds and an acknowledgement when it is done
// (docs/power.md §4), and the waiting happens on a thread of its own.
IOReturn PianoWifi::setPowerState(unsigned long state, IOService *)
{
  if (state != pianopower::kOff || gArgs.level < 2 || gRadioStopped) return pianopower::kAckNow;
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoWifi::stopThread, this, &t) != KERN_SUCCESS) {
    stopRadio("suspend");   // no thread to be had: do it here, late acknowledgement and all
    return pianopower::kAckNow;
  }
  thread_deallocate(t);
  return 4000000;
}

void PianoWifi::stopThread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-wifi-stop");
  PianoWifi *driver = static_cast<PianoWifi *>(self);
  stopRadio("suspend");
  driver->acknowledgeSetPowerState();
}

void PianoWifi::systemWillShutdown(IOOptionBits specifier)
{
  if (specifier == kIOMessageSystemWillPowerOff || specifier == kIOMessageSystemWillRestart) stopRadio("shutdown");
  IOService::systemWillShutdown(specifier);
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
  // Levels 2 and up need both: the rails' RPMh addresses and the PPIDs their regulators answer on.
  if (gArgs.cmddb || gArgs.level >= 2) {
    stage(5, "AOP command database: the supplies' RPMh resources (read only)");
    reportCmdDb();
  }
  if (gArgs.spmi || gArgs.level >= 2) {
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
  stage(7, "the survey is done");
  note("level 1: %llu MMIO loads, %u SPMI observer commands, 0 other stores; %u pages mapped, %u map failures",
       (unsigned long long)gLoads, gSpmiStores, gPageCount, gMapFailures);
  if (gArgs.level < 2) {
    unmapAll();
    return;
  }

  // ---- level 2 and up: the first writes of this driver -----------------------------------------------------------------
  const Options opt = options();
  PowerState &power = gPowerStorage;   // a file-scope state, so that the power hook's safeOff has it on its own thread
  for (unsigned i = 0; i < kRailCount; ++i) {
    power.rails[i].ppid = gRailPpid[i];
    power.rails[i].address = gRailAddress[i];
  }
  gPowerOptions = opt;
  gPowerState = &power;
  if (!mapWindows() || !mapRsc()) {
    stage(-8, "level 2: the windows or apps DRV-2 could not be mapped; nothing was written");
    unmapWindows();
    unmapAll();
    return;
  }
  note("level 2: the six PMU rails, PERST# low, WLAN_EN; the PHY's two LDOs are read only (one is the UFS PHY's PLL)");
  int rc = powerUp(power, gHw, opt);
  reportPower(power);
  writeSummary("level 2");
  if (rc) {
    stage(power.step, "level 2 stopped here");
    note("level 2: FAILED at step %d (%s): WLAN_EN is low and PERST# asserted; nothing above this level ran", power.step,
         errorName(power.error));
    unmapWindows();
    unmapAll();
    return;
  }
  note("level 2: done (rails voted, WLAN_EN high, PERST# low, SW_CTRL in %u)", power.swCtrlIo & 1u);
  if (gArgs.level < 3) {
    unmapAll();
    return;
  }

  // ---- level 3 ---------------------------------------------------------------------------------------------------------
  LinkState link;
  rc = linkUp(link, power, gHw, opt);
  reportLink(link);
  writeSummary("level 3");
  char state[200];
  snprintf(state, sizeof state, "level %u step %d%s%s link %s gen %u bar %llu KiB at %#llx", gArgs.level, link.step,
           link.error ? " error " : "", link.error ? errorName(link.error) : "",
           (link.debug1 & kPortDebug1LinkUp) ? "up" : "down", (link.linkStatus >> 16) & 0xfu,
           (unsigned long long)(link.barBytes / 1024), (unsigned long long)link.barAddress);
  setProperty("piano-wifi-link", state);
  if (rc) {
    stage(link.step, "level 3 stopped here");
    note("level 3: FAILED at step %d (%s): WLAN_EN is low and PERST# asserted", link.step, errorName(link.error));
    unmapWindows();
    unmapAll();
    return;
  }
  note("level 3: done (L0 at Gen%u x%u, the endpoint answers 17cb:110e, BAR0 %llu KiB at %#llx)", (link.linkStatus >> 16) & 0xfu,
       (link.linkStatus >> 20) & 0x3fu, (unsigned long long)(link.barBytes / 1024), (unsigned long long)link.barAddress);
  if (gArgs.smmu) {
    stage(kStepBar + 1, "the SMMU: the endpoint's stream through a bypass context bank");
    if (!smmuStream(kStreamEndpoint)) {
      stage(-(kStepBar + 1), "level 3: the endpoint's SMMU stream was not set up; no DMA is started");
      unmapWindows();
      unmapAll();
      return;
    }
    if (gArgs.smmu >= 2) (void)smmuStream(kStreamRoot);
  } else {
    note("level 3: piano-wifi-smmu=0: the SMMU is untouched (level 4 then needs stream 0x1401 to bypass already)");
  }
  if (gArgs.level < 4) {
    unmapAll();
    return;
  }

  // ---- level 4 ---------------------------------------------------------------------------------------------------------
  stage(400, "level 4: the DMA window, BAR0's MHI registers, the firmware over BHI/BHIe");
  if (!dmaWindow()) {
    stage(-400, "level 4: no DMA window; nothing was given to the endpoint");
    safeOff(gHw, power, opt);
    unmapWindows();
    unmapAll();
    return;
  }
  if (gArgs.iova && !iovaWindow()) {
    stage(-400, "level 4: the window's 32-bit addresses were not set up; nothing was given to the endpoint");
    safeOff(gHw, power, opt);
    unmapWindows();
    unmapAll();
    return;
  }
  rc = level4(link, power, opt);
  writeSummary("level 4");
  stage(rc ? -401 : 401, rc ? "level 4 stopped before mission mode" : "level 4: the firmware runs (mission mode)");
  note("level 4: %s%s", rc ? "the chip did not reach mission mode" : "the chip is in mission mode",
       rc ? "" : gArgs.level < 5 ? "; the ladder ends here (QMI is level 5, WMI and the station levels 6 and up)" : "");
  if (rc || gArgs.level < 5) {
    unmapAll();
    return;
  }

  // ---- level 5 ---------------------------------------------------------------------------------------------------------
  rc = level5(power, opt);
  piano_wifi_firmware_close();
  writeSummary("level 5");
  stage(rc ? -504 : 504, rc ? "level 5 stopped before the firmware was ready" : "level 5: the firmware is ready");
  note("level 5: %s; %s", rc ? "the firmware did not become ready" : "the firmware is ready",
       rc ? "the ladder ends here"
          : gArgs.level < 6 ? "the copy engine, the transport and the scan are level 6 (docs/drivers/wifi.md section 13)"
                            : "level 6 follows: the copy engine, the transport and a passive scan");
  if (rc || gArgs.level < 6) {
    unmapAll();
    return;
  }
  if (!gArgs.mode) {
    note("level 6: not run: the firmware's radio was not started (piano-wifi-mode=0); piano-wifi=6 needs it");
    unmapAll();
    return;
  }

  // ---- level 6 ---------------------------------------------------------------------------------------------------------
  rc = level6(this, power, opt);
  writeSummary("level 6");
  stage(rc ? -613 : 613, rc ? "level 6 stopped before the scan was done" : "level 6: the scan is done");
  note("level 6: %s; %s", rc ? "the scan did not finish" : "the scan is done",
       rc ? "the ladder ends here" : "association, keys and the data path are levels 7 and 8 (docs/drivers/wifi.md section 13)");
  if (!rc) {
    // The gate PianoWifiEthernet waits for: en1 appears (link down, with the scan list) only when level 6 came up.
    publishResource("PianoWifiLink");
    note("level 6: the resource PianoWifiLink is published: PianoWifiEthernet's en1 may come up (link down, the scan list in "
         "the property piano-wifi-scan; tooling/native/piano-wifi prints it)");
  }
  unmapAll();
}
