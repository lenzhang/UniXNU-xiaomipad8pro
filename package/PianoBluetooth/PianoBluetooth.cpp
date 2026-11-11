// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoBluetooth: the WCN7860's Bluetooth side on piano (SM8750: QUP2 SE6 "UART14" at 0x898000 as a 4-wire UART to the chip, BT_EN
// on GPIO 3 of the pm8550ve "f" PMIC at SPMI SID 5, the PMU's six rails shared with PianoWifi) for macOS 27, native at EL1. Design,
// the macOS-side verdict, the ladder and the test plan: docs/drivers/bluetooth.md. Facts and their sources: BtHw.h.
// Level 1 only:
//   piano-bt=1  a read-only survey, logged as "PianoBluetooth: ..." lines: the QUP2 vote word, the five branch clocks of the wrapper
//               and SE6, the SE6 clock source (RCG and its DFS levels), the UART's four pins and the PMU's WLAN_EN / SW_CTRL, the
//               wrapper's version and SE6's registers (only when all five branches run; never its FIFO windows), the SMMU entries
//               of the wrapper's stream 0x423, BT_EN's PMIC registers and its SPMI owner over the SPMI observer channel; then a
//               summary and a verdict for level 2. A codec self check (no hardware) shows the protocol code is in the kext.
// Nothing is written: every MMIO access is a load (GeniUart.h has no store); the only store is the SPMI observer channel's read
// command (package/PianoPower/SpmiObserver.h). The later levels' code (GeniUart.h plans, HciH4.h, QcaCodec.h's set-up sequence) is
// host-tested (tests/bt_host.cpp) and not run by this kext. Without piano-bt the driver declines in probe() and maps nothing.
// Other arguments: piano-bt-delay=S (wait first, default 0), piano-bt-se=0 (skip SE6 even when clocked), piano-bt-smmu=0 (skip the
// SMMU), piano-bt-hwcg=1 (count an AHB branch under hardware gating whose vote bit is set as running, PianoWifi's rule),
// piano-bt-spmi=0|1 (default 1; 0 when piano-wifi-spmi=1 or piano-wifi>=2 is set: PianoWifi reads the same BT_EN peripheral then,
// and one observer channel serves one reader at a time).
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "GeniUart.h"
#include "HciH4.h"
#include "QcaCodec.h"
#include "SpmiObserver.h"

using namespace pianobt;
using pianopower::Window;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_bt_stage = 0;
}

namespace {

struct Args {
  uint32_t level, delay, se, smmu, hwcg, spmi;
  bool spmiDefaulted;
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
  gArgs.level = bootArg("piano-bt", 0);
  gArgs.delay = bootArg("piano-bt-delay", 0);
  if (gArgs.delay > 600) gArgs.delay = 600;
  gArgs.se = bootArg("piano-bt-se", 1);
  gArgs.smmu = bootArg("piano-bt-smmu", 1);
  gArgs.hwcg = bootArg("piano-bt-hwcg", 0);
  const bool wifiReadsBtEn = bootArg("piano-wifi-spmi", 0) != 0 || bootArg("piano-wifi", 0) >= 2;
  uint32_t v = 0;
  gArgs.spmiDefaulted = !PE_parse_boot_argn("piano-bt-spmi", &v, sizeof v);
  gArgs.spmi = gArgs.spmiDefaulted ? (wifiReadsBtEn ? 0 : 1) : v;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoBluetooth: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_bt_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

// Explicit ldr, as the other piano kexts; the str is the SPMI observer command only (ioWr below).
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}

// ------------------------------------------------------------------------------------------------ MMIO pages, mapped on demand
// GeniUart.h decides which addresses are loaded (host-tested); a page is mapped the first time one of its words is loaded.
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

// ------------------------------------------------------------------------------------------------------------ SPMI (read only)
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
// The observer channel's command word: the read request itself (SpmiObserver.h). No PMIC register is written.
void ioWr(void *, Window w, uint32_t offset, uint32_t value)
{
  ++gSpmiReads;
  __asm__ volatile("str %w0, [%1]" ::"r"(value), "r"(windowBase(w) + offset) : "memory");
}
void ioDelay(void *, unsigned us) { IODelay(us); }
const pianopower::Io gIo = {nullptr, ioRd, ioWr, ioDelay};

struct BtEn {
  bool mapped = false, v7 = false, found = false, read = false;
  int apid = -1;
  uint32_t owner = 0, version = 0, count = 0;
  uint8_t type = 0, subtype = 0, status1 = 0, rtSts = 0;
  uint8_t regs40[7] = {};   // MODE_CTL, DIG_VIN, DIG_PULL, DIG_IN, DIG_OUT_SOURCE, DIG_OUT, EN_CTL
  const char *why = "";
};

BtEn readBtEn()
{
  using namespace pianopower;
  BtEn b;
  if (!mapRegion(gCore, kSpmiCore, kSpmiCoreBytes) || !mapRegion(gCnfg, kSpmiCnfg, kSpmiCnfgBytes) ||
      !mapRegion(gObsv, kSpmiObsv, kSpmiObsvBytes)) {
    b.why = "the arbiter windows do not map";
    return b;
  }
  b.mapped = true;
  const Arbiter a = spmiArbiter(gIo);
  b.version = a.version;
  b.count = a.count;
  b.v7 = a.v7;
  if (!a.v7) {
    b.why = "not the v7 arbiter layout";
    return b;
  }
  const Apid id = spmiFindApid(gIo, a, kBtEnPpid);
  b.apid = id.apid;
  b.owner = id.owner;
  if (id.apid < 0) {
    b.why = "PPID 0x58a is not in the arbiter's map";
    return b;
  }
  b.found = true;
  uint8_t two[2] = {};
  if (spmiRead(gIo, id.apid, kPgType, two, 2) != ReadResult::ok || spmiRead(gIo, id.apid, kPgStatus1, &b.status1, 1) != ReadResult::ok ||
      spmiRead(gIo, id.apid, kPgRtSts, &b.rtSts, 1) != ReadResult::ok || spmiRead(gIo, id.apid, kPgMode, b.regs40, 7) != ReadResult::ok) {
    b.why = "an observer read failed";
    return b;
  }
  b.type = two[0];
  b.subtype = two[1];
  b.read = true;
  return b;
}

// ------------------------------------------------------------------------------------------------------------- the report
const char *onOff(bool on) { return on ? "on" : "off"; }

void reportClocks(const Clocks &c)
{
  note("GCC QUP2 vote 0x52010 %08x: core %u s-ahb %u m-ahb %u core2x %u s6 %u", c.vote, (c.vote & kVoteCore) != 0,
       (c.vote & kVoteSAhb) != 0, (c.vote & kVoteMAhb) != 0, (c.vote & kVoteCore2x) != 0, (c.vote & kVoteS6) != 0);
  note("GCC branches: m-ahb %08x (%s) s-ahb %08x (%s) core %08x (%s) core2x %08x (%s) s6 %08x (%s): SE6 %s", c.cbcr[kBrMAhb],
       onOff(c.on[kBrMAhb]), c.cbcr[kBrSAhb], onOff(c.on[kBrSAhb]), c.cbcr[kBrCore], onOff(c.on[kBrCore]), c.cbcr[kBrCore2x],
       onOff(c.on[kBrCore2x]), c.cbcr[kBrS6], onOff(c.on[kBrS6]),
       c.seOn ? "clocked (its registers are read)" : "NOT clocked (its registers are not read)");
  note("GCC s6 rcg cmd %08x (%s) cfg %08x m %08x n %08x d %08x: source %u, %u Hz", c.rcgCmd, (c.rcgCmd >> 31) ? "root off" : "root on",
       c.rcgCfg, c.rcgM, c.rcgN, c.rcgD, (c.rcgCfg >> 8) & 7u, c.rcgHz);
  if (!c.dfs) {
    note("GCC s6 DFS off (cmd_dfsr %08x): the clock select chooses from the RCG table (7.3728 .. 128 MHz)", c.dfsCmd);
    return;
  }
  note("GCC s6 DFS on, level %u: %u %u %u %u %u %u %u %u Hz", c.dfsLevel, c.dfsHz[0], c.dfsHz[1], c.dfsHz[2], c.dfsHz[3], c.dfsHz[4],
       c.dfsHz[5], c.dfsHz[6], c.dfsHz[7]);
}

void reportPins(const Pins &p)
{
  for (unsigned i = 0; i < kPinCount; ++i) {
    const PinDecode d = decodePin(p.pin[i].cfg, p.pin[i].io);
    note("pin gpio%u %s: cfg %08x io %08x: function %u pull %s %u mA output %s, in %u out %u%s", kPins[i].gpio, kPins[i].name,
         p.pin[i].cfg, p.pin[i].io, d.function, pullName(d.pull), d.driveMa, d.outputEnable ? "enabled" : "disabled", d.in, d.out,
         pinAsDt(kPins[i], d) ? " (as the DT's state)" : " (not the DT's state)");
  }
}

void reportSe(const Se &s, const Clocks &c)
{
  if (!s.read) {
    note("SE6: not read (%s)", !gArgs.se ? "piano-bt-se=0" : c.seOn ? "?" : "its clocks are off: level 2 votes them first");
    return;
  }
  const FwRevision fw = decodeFwRevision(s.fwRevision);
  const Fifo tx = decodeFifo(s.hwParam0, s.qupVersion), rx = decodeFifo(s.hwParam1, s.qupVersion);
  note("QUP2 version %08x (%u.%u); SE6 firmware revision %08x: protocol %u (%s) version %u; FIFO interface %s; DMA mode %s",
       s.qupVersion, qupMajor(s.qupVersion), qupMinor(s.qupVersion), s.fwRevision, fw.protocol, protocolName(fw.protocol), fw.version,
       (s.ifDisable & kFifoIfDisable) ? "DISABLED (GSI only)" : "on", (s.dmaMode & kDmaModeOn) ? "SE DMA" : "FIFO");
  note("SE6 FIFOs: tx %u x %u bits, rx %u x %u bits; tx status %08x rx status %08x (%u bytes waiting); watermarks tx %u rx %u rfr %u",
       tx.depth, tx.widthBits, rx.depth, rx.widthBits, s.txFifoStatus, s.rxFifoStatus, rxBytes(decodeRxStatus(s.rxFifoStatus)), s.txWm,
       s.rxWm, s.rxRfrWm);
  const SerClk k = decodeSerClk(s.mClk);
  uint32_t levels[16];
  const unsigned n = clockLevels(c, levels, 16);
  const uint32_t src = (s.clkSel & 7u) < n ? levels[s.clkSel & 7u] : 0;
  note("SE6 clock: m %08x s %08x (%s, divider %u), select %u (%u Hz): %u baud at %u samples a bit", s.mClk, s.sClk,
       k.enabled ? "enabled" : "disabled", k.divider, s.clkSel & 7u, src, seBaud(s, src), sampling(s.qupVersion));
  note("SE6 UART: tx trans %08x (parity %s, CTS %s) rx trans %08x word %u/%u stop %u parity %08x/%08x stale %u loopback %08x "
       "io macro %08x manual rfr %08x (%s)",
       s.txTrans, (s.txTrans & kUartTxParityEn) ? "on" : "off", (s.txTrans & kUartCtsMask) ? "ignored" : "honoured", s.rxTrans,
       s.txWordLen, s.rxWordLen, s.txStopBits, s.txParity, s.rxParity, s.rxStale, s.loopback, s.ioMacro, s.manualRfr,
       (s.manualRfr & kRfrManual) ? ((s.manualRfr & kRfrNotReady) ? "RFR held not ready" : "RFR held ready") : "RFR by the engine");
  note("SE6 state: status %08x (main %s, secondary %s) m cmd %08x irq %08x/%08x, s cmd %08x irq %08x/%08x, ios %08x (rx line %u, "
       "cts %s), se irq en %08x, gsi %08x, dma tx %08x rx %08x (rx len %u), cgc %08x output %08x force %08x",
       s.status, (s.status & kMCmdActive) ? "busy" : "idle", (s.status & kSCmdActive) ? "busy" : "idle", s.mCmd0, s.mIrqStatus,
       s.mIrqEn, s.sCmd0, s.sIrqStatus, s.sIrqEn, s.ios, (s.ios & kIosRx) != 0, (s.ios & kIosCts) ? "high (deasserted)" : "low (asserted)",
       s.seIrqEn, s.gsiEventEn, s.dmaTxIrq, s.dmaRxIrq, s.dmaRxLenIn, s.cgc, s.outputCtrl, s.forceDefault);
}

void reportSmmu(const SmmuStream &m)
{
  if (m.error) {
    note("SMMU: %s (idr0 %08x idr1 %08x)", m.error, m.idr0, m.idr1);
    return;
  }
  note("SMMU idr0 %08x idr1 %08x: %u stream-match groups, %u banks, %u pages", m.idr0, m.idr1, m.groups, m.banks, m.pages);
  if (!m.count) {
    note("SMMU stream 0x%03x: no stream-match entry (SE DMA needs one; FIFO mode does not)", kStreamQup2);
    return;
  }
  for (unsigned i = 0; i < m.count; ++i) {
    const SmmuMatch &x = m.found[i];
    const uint32_t type = (x.s2cr >> 16) & 3u;
    note("SMMU stream 0x%03x: group %u smr %08x s2cr %08x: %s", kStreamQup2, x.group, x.smr, x.s2cr,
         type == 1 ? "bypass" : type == 2 ? "fault" : (x.sctlr & 1u) ? "translate (bank with translation on)" : "translate (bank with translation off)");
  }
}

void reportBtEn(const BtEn &b)
{
  if (!b.read) {
    note("BT_EN (pm8550ve-f gpio3, PPID 0x%03x): not read (%s; arbiter %08x, %u APIDs, APID %d)", kBtEnPpid, b.why, b.version, b.count,
         b.apid);
    return;
  }
  const uint8_t mode = b.regs40[0], out = b.regs40[4], en = b.regs40[6];
  const bool lvmv = pgLvMv(b.subtype);
  note("BT_EN (pm8550ve-f gpio3, PPID 0x%03x): APID %d owner EE %u (%s); type %02x subtype %02x (%s) status1 %02x rt_sts %02x",
       kBtEnPpid, b.apid, b.owner, b.owner == 0 ? "the apps processor: level 3 can drive it" : "NOT the apps processor", b.type, b.subtype,
       b.type != kPgTypeGpio ? "not a GPIO" : lvmv ? "LV/MV" : "older GPIO kind", b.status1, b.rtSts);
  note("BT_EN mode %02x (%s) vin %02x pull %02x (%s) dig_in %02x out_src %02x (level %u, function %u) dig_out %02x en %02x (%s)", mode,
       (mode & 3u) == 0 ? "input" : (mode & 3u) == 1 ? "output" : (mode & 3u) == 2 ? "input+output" : "analog", b.regs40[1], b.regs40[2],
       (b.regs40[2] & 7u) == kPgPullNone ? "none" : (b.regs40[2] & 7u) == 4 ? "down" : "up", b.regs40[3], out, (out & kPgOutLevel) != 0,
       out & 0xfu, b.regs40[5], en, (en & kPgEnabled) ? "enabled" : "disabled");
}

// BT_EN's driven level: an enabled LV/MV output's DIG_OUT_SOURCE_CTL bit 7 (-1: not known).
int btEnLevel(const BtEn &b)
{
  if (!b.read || b.type != kPgTypeGpio || !pgLvMv(b.subtype)) return -1;
  if (!(b.regs40[6] & kPgEnabled)) return 0;
  if ((b.regs40[0] & 3u) == 0) return -1;
  return (b.regs40[4] & kPgOutLevel) ? 1 : 0;
}

// The protocol code, linked into this kext, on canned data (no hardware): the request's bytes, a version reply, the file names.
H4Decoder gDecoder;   // a 1 KiB buffer: a global, constant-initialised, not on the kernel stack
bool codecSelfCheck(char *line, size_t size)
{
  uint8_t f[8];
  const size_t n = cmdEdl(f, sizeof f, kEdlVersion);
  const uint8_t reply[] = {kH4Event, 0x0e, 18, 0x01, 0x00, 0xfc, 0x00, 0x19, 0x00, 0x13, 0x00, 0x00, 0x00, 0x01, 0x00,
                           0x00, 0x02, 0x00, 0x02, 0x21, 0x40};
  H4Decoder &dec = gDecoder;
  dec.reset();
  CommandComplete cc;
  QcaVersion v;
  bool ok = false;
  dec.feed(reply, sizeof reply, [&](uint8_t type, const uint8_t *p, size_t len) {
    if (p && type == kH4Event) ok = parseCommandComplete(p, len, cc) && parseVersion(cc, v);
  });
  char fw[24], nvm[24];
  ok = ok && n == 5 && v.romCode == kStockRom && rampatchName(fw, sizeof fw, v.romCode) && nvmName(nvm, sizeof nvm, v.romCode, 0x2e);
  snprintf(line, size, "version request %02x %02x %02x %02x %02x, a canned reply decoded (rom 0x%02x, %s, %s): %s", f[0], f[1], f[2],
           f[3], f[4], v.romCode, ok ? fw : "-", ok ? nvm : "-", ok ? "ok" : "FAILED");
  return ok;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoBluetooth : public IOService {
  OSDeclareDefaultStructors(PianoBluetooth)
  void run();
  static void thread(void *self, wait_result_t);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoBluetooth, IOService)

IOService *PianoBluetooth::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoBluetooth::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-bt=%u delay=%u se=%u smmu=%u hwcg=%u spmi=%u%s (WCN7860 Bluetooth on QUP2 SE6 %#llx \"UART14\", SPI %u, BT_EN PPID "
       "0x%03x)",
       gArgs.level, gArgs.delay, gArgs.se, gArgs.smmu, gArgs.hwcg, gArgs.spmi,
       gArgs.spmiDefaulted && !gArgs.spmi ? " (PianoWifi reads BT_EN in this boot)" : "", (unsigned long long)kSe6, kSpiUart14, kBtEnPpid);
  if (gArgs.level > 1) note("piano-bt=%u: only level 1 exists (docs/drivers/bluetooth.md section 8); running 1", gArgs.level);
  note("level 1: loads only, nothing written (the SPMI observer's read command is the only store)");
  setProperty("piano-bt", gArgs.level, 32);
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoBluetooth::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;
  }
  thread_deallocate(t);
  return true;
}

void PianoBluetooth::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-bt");
  static_cast<PianoBluetooth *>(self)->run();
}

void PianoBluetooth::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  char line[300];
  const bool codecOk = codecSelfCheck(line, sizeof line);
  note("codec self check: %s", line);

  stage(1, "GCC: the QUP2 wrapper's and SE6's clocks, the SE6 clock source (read only)");
  const Clocks c = surveyClocks(load, gArgs.hwcg != 0);
  reportClocks(c);

  stage(2, "TLMM: the UART's pins, the PMU's WLAN_EN and SW_CTRL (read only)");
  const Pins p = surveyPins(load);
  reportPins(p);

  stage(3, "QUP2 wrapper and SE6 (read only, only when clocked; never the FIFO windows)");
  const Se s = surveySe(c, gArgs.se != 0, load);
  reportSe(s, c);

  SmmuStream m;
  if (gArgs.smmu) {
    stage(4, "SMMU: the entries of the wrapper's stream 0x423 (read only)");
    m = surveySmmu(kStreamQup2, [](uint32_t off) { return load(kSmmu + off); });
    reportSmmu(m);
  } else {
    note("stage 4: SMMU skipped (piano-bt-smmu=0)");
  }

  BtEn b;
  if (gArgs.spmi) {
    stage(5, "BT_EN over the SPMI observer channel (observer reads)");
    b = readBtEn();
    reportBtEn(b);
  } else {
    note("stage 5: BT_EN not read (%s)", gArgs.spmiDefaulted ? "PianoWifi reads it in this boot; piano-bt-spmi=1 overrides" : "piano-bt-spmi=0");
  }

  // The summary (one line, also the piano-bt-survey property) and what it means for level 2.
  bool pinsAsDt = true;
  for (unsigned i = kPinCts; i <= kPinRx; ++i) pinsAsDt = pinsAsDt && pinAsDt(kPins[i], decodePin(p.pin[i].cfg, p.pin[i].io));
  const int level = btEnLevel(b);
  const FwRevision fw = decodeFwRevision(s.fwRevision);
  const PinDecode wlan = decodePin(p.pin[kPinWlanEn].cfg, p.pin[kPinWlanEn].io), sw = decodePin(p.pin[kPinSwCtrl].cfg, p.pin[kPinSwCtrl].io);
  char summary[240];
  snprintf(summary, sizeof summary, "bt_en %s owner %s | wlan_en %u sw_ctrl %u | se6 %s%s | uart pins %s | smmu 0x423 %s | codec %s",
           level < 0 ? "?" : level ? "high" : "low", b.read ? (b.owner == 0 ? "apps" : "other") : "?", wlan.out, sw.in,
           c.seOn ? "clocked" : "unclocked", s.read ? (fw.protocol == kProtoUart ? " uart" : " NOT-uart") : "", pinsAsDt ? "as dt" : "not as dt",
           !gArgs.smmu ? "skipped" : m.error ? "?" : !m.count ? "none" : ((m.found[0].s2cr >> 16) & 3u) == 1 ? "bypass" : "translate",
           codecOk ? "ok" : "FAILED");
  setProperty("piano-bt-survey", summary);
  note("summary: %s", summary);
  if (s.read && fw.protocol != kProtoUart)
    note("verdict: SE6 does not carry the UART protocol (protocol %u): stop; the boot chain configures SE6 differently than "
         "for Ubuntu", fw.protocol);
  else if (b.read && b.owner != 0)
    note("verdict: BT_EN's SPMI peripheral is owned by EE %u, not the apps processor: level 3 cannot drive it from here", b.owner);
  else
    note("verdict: level 2 %s; level 3 %s", c.seOn ? "may read the SE as it is (already clocked)" : "must vote SE6's clocks first",
         level == 1 ? "finds BT_EN already high (something powered the Bluetooth side)" : "raises BT_EN after the shared rails");
  stage(6, "done");
  note("done: %llu MMIO loads, 0 MMIO stores; %u SPMI observer reads; %u pages mapped, %u map failures", (unsigned long long)gLoads,
       gSpmiReads, gPageCount, gMapFailures);
  unmapAll();
  unmapRegion(gCore);
  unmapRegion(gCnfg);
  unmapRegion(gObsv);
}
