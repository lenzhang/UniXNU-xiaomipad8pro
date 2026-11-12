// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoBluetooth: the WCN7860's Bluetooth side on piano (SM8750: QUP2 SE6 "UART14" at 0x898000 as a 4-wire UART to the chip, BT_EN
// on GPIO 3 of the pm8550ve "f" PMIC at SPMI SID 5, the PMU's six rails shared with PianoWifi) for macOS 27, native at EL1. Design,
// the macOS-side verdict, the ladder and the test plan: docs/drivers/bluetooth.md. Facts and their sources: BtHw.h.
//   piano-bt=1  a read-only survey, logged as "PianoBluetooth: ..." lines: the QUP2 vote word, the five branch clocks of the wrapper
//               and SE6, the SE6 clock source (RCG and its DFS levels), the UART's four pins and the PMU's WLAN_EN / SW_CTRL, the
//               wrapper's version and SE6's registers (only when all five branches run; never its FIFO windows), the SMMU entries
//               of the wrapper's stream 0x423, BT_EN's PMIC registers and its SPMI owner over the SPMI observer channel; then a
//               summary and a verdict. A codec self check (no hardware) shows the protocol code is in the kext. Nothing is written:
//               every MMIO access is a load (GeniUart.h has no store); the only store is the observer channel's read command.
//   piano-bt=2  power (BtBringup.h powerOn): the PMU lock, the six PMU rails through the helper shared with PianoWifi
//               (package/include/PianoWcnRails.h: voted over RPMh by whichever radio comes first, reference-counted), BT_EN to the
//               DT's state and high over the SPMI owner channel (only when its APID's owner is EE 0, the apps processor; else a
//               clear line and nothing written), the PMU's 50 ms, SW_CTRL.
//   piano-bt=3  the UART (uartUp): the QUP2 votes under PianoA830's GCC vote lock, SE6's protocol and FIFOs, its clock source (a DFS
//               level, or the RCG at 0x1e77c programmed for 115200 x 16), the engine at 115200 8N1 with RTS/CTS, polled FIFO, the
//               internal loopback self test, then gpio24..27 to qup2_se6.
//   piano-bt=4  the controller (controllerUp, QcaCodec.h): HCI_Reset, the QCA version, the WCN7860 frame and 3.2 Mbit/s, the rampatch
//               gngbtfw20.mbn and the NVM gngbtnv20.bXX by board id read at run time from the tablet's own bluetooth_a (never
//               from this repository), SoC logging off, HCI_Reset, the build label, the unit's address from the locally built ADT
//               (private, never logged), HCI_Reset again, Read Local Version. The controller then idles; no host stack yet.
// Levels are cumulative; the first failure leaves everything safe (BtBringup.h safeOff: the engine stopped, the pins inputs with
// pull-downs, BT_EN low, this driver's clock votes withdrawn, the rails put). Below piano-bt=2 the radio is never powered. From level
// 2 the driver joins the power tree (package/include/PianoPowerContract.h): system sleep takes the radio down the same way and the
// wake brings it back to the same level.
// Other arguments: piano-bt-delay=S (wait first; 30 by default at level 2 and up when piano-wifi runs in the same boot, whose SPMI
// reads must not overlap ours), piano-bt-se=0, piano-bt-smmu=0, piano-bt-hwcg=1, piano-bt-spmi=0|1 (the survey's; levels 2 and up
// read BT_EN regardless), piano-bt-baud=B (the operating speed, default 3200000; 115200 stays at the initial one), piano-bt-peri=0
// (no WCN7860 peripheral baud frame), piano-bt-ibs=1 (keep the NVM's in-band sleep bit), piano-bt-reset0=0 (no HCI_Reset before
// the version), piano-bt-loop=0 (no loopback self test), piano-bt-release=0 (never withdraw a rail enable), piano-bt-blind=1 (vote a
// rail whose PMIC state could not be read), piano-bt-addr=0 (keep the NVM's default address), piano-bt-stop=N (return after step N).
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoPowerContract.h>
#define PIANO_WCN_RAILS_KERNEL
#include <PianoWcnRails.h>
#include "BtBringup.h"
#include "BtFirmware.h"
#include "BtRpmh.h"
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
  uint32_t baud, peri, ibs, reset0, loop, release, blind, addr, stop;
  bool spmiDefaulted, delayDefaulted;
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
  uint32_t v = 0;
  gArgs.delayDefaulted = !PE_parse_boot_argn("piano-bt-delay", &v, sizeof v);
  // PianoWifi's level 1 PMIC survey and level 2 read the same peripherals over the observer channel and take no PMU lock yet
  // (docs/drivers/bluetooth.md section 7.2): at level 2 and up, by default, start after it.
  gArgs.delay = gArgs.delayDefaulted ? (gArgs.level >= 2 && bootArg("piano-wifi", 0) >= 1 ? 30 : 0) : v;
  if (gArgs.delay > 600) gArgs.delay = 600;
  gArgs.se = bootArg("piano-bt-se", 1);
  gArgs.smmu = bootArg("piano-bt-smmu", 1);
  gArgs.hwcg = bootArg("piano-bt-hwcg", 0);
  const bool wifiReadsBtEn = bootArg("piano-wifi-spmi", 0) != 0 || bootArg("piano-wifi", 0) >= 2;
  gArgs.spmiDefaulted = !PE_parse_boot_argn("piano-bt-spmi", &v, sizeof v);
  gArgs.spmi = gArgs.spmiDefaulted ? (wifiReadsBtEn && gArgs.level < 2 ? 0 : 1) : v;
  gArgs.baud = bootArg("piano-bt-baud", kOperBaud);
  if (baudCode(gArgs.baud) < 0 || baudCode(gArgs.baud) > kBaudCodeMax) gArgs.baud = kOperBaud;
  gArgs.peri = bootArg("piano-bt-peri", 1);
  gArgs.ibs = bootArg("piano-bt-ibs", 0);
  gArgs.reset0 = bootArg("piano-bt-reset0", 1);
  gArgs.loop = bootArg("piano-bt-loop", 1);
  gArgs.release = bootArg("piano-bt-release", 1);
  gArgs.blind = bootArg("piano-bt-blind", 0);
  gArgs.addr = bootArg("piano-bt-addr", 1);
  gArgs.stop = bootArg("piano-bt-stop", 0);
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

// Explicit ldr/str, as the other piano kexts.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }
inline void dsb() { __asm__ volatile("dsb sy" ::: "memory"); }

uint64_t nowUs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}

// ------------------------------------------------------------------------------------------------ MMIO pages, mapped on demand
// Level 1: GeniUart.h decides which addresses are loaded (host-tested); a page is mapped the first time one of its words is loaded.
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

// ------------------------------------------------------------------------------------------------------------------- SPMI
// The observer channel (reads, SpmiObserver.h) for level 1 and the rails; BT_EN's owner channel (writes) from level 2, mapped only for
// an APID owned by EE 0 and used only for BT_EN's five registers.
struct Region {
  IOMemoryMap *map;
  uintptr_t va;
};
Region gCore, gCnfg, gObsv, gChnl;
unsigned gSpmiReads, gPmicWrites, gPmicRefused;

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

bool mapSpmi()
{
  using namespace pianopower;
  if (!mapRegion(gCore, kSpmiCore, kSpmiCoreBytes) || !mapRegion(gCnfg, kSpmiCnfg, kSpmiCnfgBytes) ||
      !mapRegion(gObsv, kSpmiObsv, kSpmiObsvBytes))
    return false;
  if (!gArbiter.v7) gArbiter = spmiArbiter(gIo);
  return gArbiter.v7;
}

struct BtEn {
  bool mapped = false, v7 = false, found = false, read = false;
  int apid = -1;
  uint32_t owner = 0, version = 0, count = 0;
  uint8_t type = 0, subtype = 0, status1 = 0, rtSts = 0;
  uint8_t regs40[7] = {};   // MODE_CTL, DIG_VIN, DIG_PULL, DIG_IN, DIG_OUT_SOURCE, DIG_OUT, EN_CTL
  const char *why = "";
};
BtEn gBtEn;

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
  gArbiter = a;
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
    note("SE6: not read (%s)", !gArgs.se ? "piano-bt-se=0" : c.seOn ? "?" : "its clocks are off: level 3 votes them first");
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
       kBtEnPpid, b.apid, b.owner, b.owner == 0 ? "the apps processor: level 2 can drive it" : "NOT the apps processor", b.type, b.subtype,
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

// ================================================================================== levels 2 and up: the windows and the writes
// Each block one window (level 1's page-at-a-time loads stay for the survey). hwRead/hwWrite refuse an offset outside the window.
struct WindowMap {
  const char *name;
  IOMemoryMap *map;
  uintptr_t va;
};
WindowMap gWin[kWinCount] = {{"gcc", nullptr, 0}, {"tlmm", nullptr, 0}, {"se6", nullptr, 0}, {"qup2", nullptr, 0}};

bool mapWindows()
{
  for (unsigned i = 0; i < kWinCount; ++i) {
    if (gWin[i].va) continue;
    IODeviceMemory *m = IODeviceMemory::withRange(kWinBase[i], kWinBytes[i]);
    IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (m) m->release();
    if (!map) {
      note("cannot map the %s window (%#llx + %#llx)", gWin[i].name, (unsigned long long)kWinBase[i], (unsigned long long)kWinBytes[i]);
      return false;
    }
    gWin[i].map = map;
    gWin[i].va = uintptr_t(map->getVirtualAddress());
  }
  return true;
}

// Every write kind is logged once with what it is; a repeat of the same kind is counted. The TX FIFO's words are never logged
// (they carry level 4's traffic, the unit's address among it).
unsigned gWrites, gBadAccess;
const char *gSeen[96];
unsigned gSeenCount, gSeenRepeats[96];

uint32_t hwRead(void *, unsigned win, uint32_t off)
{
  if (win >= kWinCount || !gWin[win].va || (off & 3) || off + 4 > kWinBytes[win]) {
    ++gBadAccess;
    return 0xffffffffu;
  }
  return rd32(gWin[win].va + off);
}

void hwWrite(void *, unsigned win, uint32_t off, uint32_t value, const char *what)
{
  if (win >= kWinCount || !gWin[win].va || (off & 3) || off + 4 > kWinBytes[win]) {
    ++gBadAccess;
    note("refused a write outside the %s window: +%#x (%s)", win < kWinCount ? gWin[win].name : "?", off, what);
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
  if (gSeenCount < 96) {
    gSeen[gSeenCount] = what;
    gSeenRepeats[gSeenCount++] = 0;
  }
  if (win == kWinSe && off == kTxFifo) note("write se6+%#x: %s (values not logged)", off, what);
  else note("write %s+%#x = %08x: %s", gWin[win].name, off, value, what);
}

unsigned gWritesBefore, gKindsBefore;
void writeSummary(const char *level)
{
  char line[360];
  size_t n = 0;
  for (unsigned i = gKindsBefore; i < gSeenCount && n + 48 < sizeof line; ++i)
    if (gSeenRepeats[i]) n += size_t(snprintf(line + n, sizeof line - n, "%s%s x%u", n ? ", " : "", gSeen[i], gSeenRepeats[i] + 1));
  note("%s: %u writes (%u kinds, %u refused accesses in all)%s%s", level, gWrites - gWritesBefore, gSeenCount - gKindsBefore,
       gBadAccess, n ? "; repeated: " : "", n ? line : "");
  gWritesBefore = gWrites;
  gKindsBefore = gSeenCount;
}

void hwUdelay(void *, unsigned us) { IODelay(us); }
void hwMsleep(void *, unsigned ms) { IOSleep(ms); }
uint64_t hwNow(void *) { return nowUs(); }
void hwStep(void *, int step, const char *what)
{
  piano_bt_stage = step;
  note("step %d: %s", step, what);
}

// ---- PianoA830's lock words (Sm8750Rpmh.h, Sm8750GpuPower.c; PianoA830 is listed before this kext and named as a library in
// PianoBluetooth.json, so the KC linker resolves both to A830's single copy, as for PianoWifi, PianoTouch, PianoKeyboard).
extern "C" {
extern volatile uint32_t sm8750_gcc_vote_owner;
extern volatile uint32_t sm8750_rpmh_drv2_owner;
}
constexpr uint32_t kTag = PianoWcnTagBt;   // "BTH1" in every lock word
constexpr unsigned kLockStepUs = 10;

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

// The QUP2 vote word is read-modify-written under A830's lock word (PianoTouch's protocol: 10 us steps, at most 50 ms; a holder that
// keeps it longer is reported and the update goes ahead, as bits are only ever set by it, and withdrawn only when they were ours).
bool gVoteLocked;
int hwVoteLock(void *, int take)
{
  if (!take) {
    if (gVoteLocked) lockRelease(&sm8750_gcc_vote_owner);
    gVoteLocked = false;
    return 0;
  }
  uint32_t waits = 0;
  gVoteLocked = lockAcquire(&sm8750_gcc_vote_owner, &waits, 5000);
  if (!gVoteLocked)
    note("GCC vote lock held by %#x for 50 ms: the vote word is updated anyway", __atomic_load_n(&sm8750_gcc_vote_owner, __ATOMIC_ACQUIRE));
  else if (waits)
    note("GCC vote lock taken after %u waits of %u us", waits, kLockStepUs);
  return 0;
}

// ---- the rails: cmd-db for the six resources, one VRM transaction on apps DRV-2 under the DRV-2 lock (BtRpmh.h), the regulators over
// the observer channel. Wrapped by the shared helper (PianoWcnRails.h), whose words are PianoA830's.
uint32_t gRailAddress[PIANO_WCN_VOTED_RAILS];
bool gCmdDbRead;
IOMemoryMap *gRscMap;
uintptr_t gRscVa;
Rsc gRsc;

bool readCmdDb()
{
  if (gCmdDbRead) return true;
  uint8_t *copy = static_cast<uint8_t *>(IOMallocData(kCmdDbBytes));
  IODeviceMemory *m = copy ? IODeviceMemory::withRange(kCmdDb, kCmdDbBytes) : nullptr;
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) {
    if (copy) IOFreeData(copy, kCmdDbBytes);
    note("cmd-db: cannot map %#llx or allocate its copy", (unsigned long long)kCmdDb);
    return false;
  }
  const uintptr_t va = uintptr_t(map->getVirtualAddress());
  for (uint32_t i = 0; i < kCmdDbBytes; i += 4) {
    const uint32_t w = rd32(va + i);
    memcpy(copy + i, &w, 4);
  }
  map->release();
  char line[300];
  size_t n = 0;
  bool all = true;
  for (unsigned i = 0; i < PIANO_WCN_VOTED_RAILS && n < sizeof line; ++i) {
    const char *name = piano_wcn_rail_table[i].cmd_db;
    const int rc = pianokeyboard::cmdDbFind(copy, kCmdDbBytes, name, &gRailAddress[i]);
    if (rc) gRailAddress[i] = 0;
    all = all && !rc;
    n += size_t(rc ? snprintf(line + n, sizeof line - n, "%s%s none (%d)", i ? ", " : "", name, rc)
                   : snprintf(line + n, sizeof line - n, "%s%s %#x", i ? ", " : "", name, gRailAddress[i]));
  }
  IOFreeData(copy, kCmdDbBytes);
  note("cmd-db: %s", line);
  gCmdDbRead = true;
  return all;
}

uint32_t rscRd(void *, uint32_t off) { return rd32(gRscVa + kRscTcsBlock + off); }
void rscWr(void *, uint32_t off, uint32_t v)
{
  wr32(gRscVa + kRscTcsBlock + off, v);
  dsb();
}
void rscSleep(void *, unsigned ms) { IOSleep(ms); }
void rscDelay(void *, unsigned us) { IODelay(us); }   // the TCS poll runs under the DRV-2 lock: a busy wait, never a sleep (P17)
const RscIo gRscIo = {nullptr, rscRd, rscWr, rscSleep, rscDelay};

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
  const uint32_t id = rd32(gRscVa);
  gRsc.layout = pianokeyboard::rscLayout(id);
  note("rsc: apps DRV-2 id %#x (version %u.%u), TCS block +%#x", id, (id >> 16) & 0xff, (id >> 8) & 0xff, kRscTcsBlock);
  return true;
}

int wcnVrm(void *, const char *resource, const PianoWcnVrmCmd *cmds, unsigned count)
{
  uint32_t base = 0;
  for (unsigned i = 0; i < PIANO_WCN_VOTED_RAILS; ++i)
    if (!strncmp(piano_wcn_rail_table[i].cmd_db, resource, 8)) base = gRailAddress[i];
  if (!base || !gRscVa) {
    note("rsc: %s: no RPMh resource of that name (or no DRV-2 mapping), nothing sent", resource);
    return -19;
  }
  uint32_t waits = 0;
  const uint32_t held = __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE);
  if (held) note("rsc: the DRV-2 lock is held by %#x; waiting (at most 2 s)", held);
  if (!lockAcquire(&sm8750_rpmh_drv2_owner, &waits, 200000)) {
    note("rsc: the DRV-2 lock stayed held by %#x for 2 s: %s not voted", __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE),
         resource);
    return -16;
  }
  VrmResult res;
  const int rc = vrmSend(gRsc, gRscIo, base, cmds, count, res);
  lockRelease(&sm8750_rpmh_drv2_owner);
  note("rsc: %s (%#x): %u commands through TCS %d %s after %u us (status %#x %#x %#x)", resource, base, count, res.tcs,
       res.done ? "completed" : rc == -16 ? "NOT sent (no idle TCS)" : "NOT completed (left in flight, not used again)", res.waitedUs,
       res.status[0], res.status[1], res.status[2]);
  return rc;
}

int wcnPmic(void *, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts)
{
  using namespace pianopower;
  if (!mapSpmi()) return -19;
  const Apid found = spmiFindApid(gIo, gArbiter, ppid);
  if (found.apid < 0) return -19;
  uint8_t regs[7] = {};
  if (spmiRead(gIo, found.apid, 0x08, status, 1) != ReadResult::ok) return -5;
  if (spmiRead(gIo, found.apid, 0x40, regs, 7) != ReadResult::ok) return -5;
  *millivolts = uint16_t(uint16_t(regs[0]) | uint16_t(regs[1]) << 8);
  *mode = regs[5];
  *enable = regs[6];
  return 0;
}

int wcnPmicType(void *, uint16_t ppid, uint8_t *type)
{
  using namespace pianopower;
  if (!mapSpmi()) return -19;
  const Apid found = spmiFindApid(gIo, gArbiter, ppid);
  if (found.apid < 0) return -19;
  return spmiRead(gIo, found.apid, 0x04, type, 1) == ReadResult::ok ? 0 : -5;
}

PIANO_WCN_KERNEL_RAILS(gWcn, PianoWcnTagBt, "PianoBluetooth", wcnVrm, wcnPmic, wcnPmicType);

int hwPmuLock(void *, int take) { return piano_wcn_pmu_lock(&gWcn.rails, take); }
int hwRailsGet(void *)
{
  gWcn.rails.blind = int(gArgs.blind);
  if (!readCmdDb() || !mapRsc()) note("rails: not every resource is known; the helper refuses what it cannot vote");
  return piano_wcn_rails_get(&gWcn.rails, PIANO_WCN_BT);
}
// An unpatched PianoWifi (docs/drivers/bluetooth.md section 7.2) votes the rails without the helper and is no owner in its word: while
// piano-wifi>=2 runs in this boot, nothing is withdrawn.
unsigned hwRailsPut(void *, bool release)
{
  const bool wifiAlone = bootArg("piano-wifi", 0) >= 2 && !(piano_wcn_rails_owners(&gWcn.rails) & PIANO_WCN_WLAN);
  return piano_wcn_rails_put(&gWcn.rails, PIANO_WCN_BT | (release && !wifiAlone ? unsigned(PIANO_WCN_RELEASE) : 0u));
}

// ---- BT_EN over SPMI: the observer channel for reads, its owner channel (EE 0 only) for the five registers of BtHw.h.
int hwBtEnInfo(void *, BtEnInfo *out)
{
  if (!gBtEn.read) gBtEn = readBtEn();
  if (!gBtEn.found) return -19;
  out->apid = gBtEn.apid;
  out->owner = gBtEn.owner;
  out->type = gBtEn.type;
  out->subtype = gBtEn.subtype;
  return gBtEn.read ? 0 : -5;
}

int hwPmicRead(void *, uint8_t reg, uint8_t *value, unsigned bytes)
{
  using namespace pianopower;
  if (!gBtEn.found || !mapSpmi()) return -19;
  return spmiRead(gIo, gBtEn.apid, reg, value, bytes) == ReadResult::ok ? 0 : -5;
}

int hwPmicWrite(void *, uint8_t reg, uint8_t value)
{
  const bool allowed = reg == kPgMode || reg == kPgVin || reg == kPgPull || reg == kPgOutSrc || reg == kPgEn;
  if (!allowed || !gBtEn.found || gBtEn.owner != 0) {
    ++gPmicRefused;
    note("pmic: refused a write of BT_EN register %#04x (%s)", reg, !allowed ? "not one of its five" : "not owned by EE 0");
    return -13;
  }
  if (!mapRegion(gChnl, kSpmiChnls + uint64_t(kChnlStride) * uint32_t(gBtEn.apid), kChnlBytes)) return -14;
  // The transaction is PianoSpmi.h's, under the SPMI lock every kext shares (mapped above: nothing inside the hold maps or logs).
  const uintptr_t va = gChnl.va;
  uint32_t st = 0;
  const pianospmi::Result r = pianospmi::write([va](uint32_t off) { return rd32(va + off); },
                                               [va](uint32_t off, uint32_t v) {
                                                 wr32(va + off, v);
                                                 if (off == pianospmi::kCmd) dsb();
                                               },
                                               [](unsigned us) { IODelay(us); }, reg, &value, 1, &st);
  ++gPmicWrites;
  const bool ok = r == pianospmi::Result::ok;
  note("pmic write BT_EN %#04x = %02x: status %08x (%s)", reg, value, st, ok ? "done" : (st & 1u) ? "FAILED/denied" : "timed out");
  return ok ? 0 : -5;
}

volatile bool gAbort;
bool hwCancelled(void *) { return __atomic_load_n(&gAbort, __ATOMIC_ACQUIRE); }

const Hw gHw = {nullptr, hwRead, hwWrite, hwUdelay, hwMsleep, hwNow, hwVoteLock, hwPmuLock, hwRailsGet, hwRailsPut,
                hwBtEnInfo, hwPmicRead, hwPmicWrite, hwCancelled, hwStep};

// ---- level 4's files: the partition opened once, a file read whole into memory once (the rampatch is 227 KiB) and sent from there.
struct FwCache {
  char name[24];
  uint8_t *data;
  uint32_t bytes;
};
FwCache gFw;

void fwDrop()
{
  if (gFw.data) IOFreeData(gFw.data, gFw.bytes);
  gFw = {};
}
int fwSize(void *, const char *name, uint32_t *bytes)
{
  const int rc = piano_bt_firmware_stat(name, bytes);
  note("firmware: %s %s", name, rc ? "not on the partition" : "found");
  return rc;
}
int fwRead(void *, const char *name, uint32_t offset, uint8_t *to, uint32_t n)
{
  if (strncmp(gFw.name, name, sizeof gFw.name)) {
    fwDrop();
    uint32_t bytes = 0;
    if (piano_bt_firmware_stat(name, &bytes) || !bytes || bytes > kFirmwareMaxBytes) return PianoBtFwRange;
    uint8_t *data = static_cast<uint8_t *>(IOMallocData(bytes));
    if (!data) return PianoBtFwNoMemory;
    const uint64_t t0 = nowUs();
    if (piano_bt_firmware_read(name, 0, data, bytes)) {
      IOFreeData(data, bytes);
      return PianoBtFwReadError;
    }
    gFw.data = data;
    gFw.bytes = bytes;
    strlcpy(gFw.name, name, sizeof gFw.name);
    note("firmware: image/%s, %u bytes, read in %llu ms (held in memory while it is sent)", name, bytes,
         (unsigned long long)((nowUs() - t0) / 1000));
  }
  if (uint64_t(offset) + n > gFw.bytes) return PianoBtFwRange;
  memcpy(to, gFw.data + offset, n);
  return 0;
}
const FwIo gFwIo = {nullptr, fwSize, fwRead};

// ---- the unit's address: /arm-io/piano-bt bt-address in the locally built ADT (private; never logged).
uint8_t gAddress[6];
bool readAddress()
{
  IORegistryEntry *e = IORegistryEntry::fromPath(kAddressNode, gIODTPlane);
  if (!e) return false;
  OSData *d = OSDynamicCast(OSData, e->getProperty(kAddressProperty));
  bool ok = d && d->getLength() == 6;
  if (ok) memcpy(gAddress, d->getBytesNoCopy(), 6);
  e->release();
  if (!ok) return false;
  bool zero = true, ones = true;
  for (uint8_t b : gAddress) {
    zero = zero && b == 0;
    ones = ones && b == 0xff;
  }
  return !zero && !ones && !(gAddress[0] & 1u);   // a unicast address that is not all zeros or all ones
}

Bringup gB;   // the port's buffers (3 KiB): a global, not on the kernel stack
uint8_t gNvm[kNvmMaxBytes];

Options bringupOptions(unsigned level, bool haveAddress)
{
  Options o;
  o.level = level;
  o.loopback = gArgs.loop != 0;
  o.release = gArgs.release != 0;
  o.stopAfter = int(gArgs.stop);
  o.setup.operBaud = gArgs.baud;
  o.setup.peri = gArgs.peri != 0;
  o.setup.resetFirst = gArgs.reset0 != 0;
  o.setup.ibs = gArgs.ibs ? -1 : 0;
  o.setup.deepSleep = 0;
  o.setup.stopAfter = int(gArgs.stop);
  o.setup.address = haveAddress ? gAddress : nullptr;
  return o;
}

// ------------------------------------------------------------------------------------------------------ the levels' reports
void reportPower(const PowerState &s)
{
  const PianoWcnRails &w = gWcn.rails;
  if (w.step == PianoWcnStepVerify && !w.error) {
    note("rails: up already (state %#x): every rail read back enabled, nothing sent", piano_wcn_rails_owners(&w));
  } else {
    for (unsigned i = 0; i < PIANO_WCN_RAIL_COUNT; ++i) {
      const PianoWcnRail &t = piano_wcn_rail_table[i];
      const PianoWcnRailRecord &r = w.rail[i];
      if (!t.order) {
        note("rail %s %s (%s) ppid %03x: %u mV en %02x -> %u mV en %02x; never voted", t.supply, t.regulator, t.cmd_db, r.ppid,
             r.millivolts, r.enable, r.millivolts_after, r.enable_after);
        continue;
      }
      note("rail %s %s (%s) ppid %03x: before %s %u mV mode %02x en %02x -> sent mode 7%s%s -> after %s %u mV en %02x (rc %d)%s",
           t.supply, t.regulator, t.cmd_db, r.ppid, r.read ? "read" : "unread", r.millivolts, r.mode, r.enable,
           r.voted ? ", voltage" : " (voltage kept)", r.sent_enable ? ", enable" : "", r.read_after ? "read" : "unread",
           r.millivolts_after, r.enable_after, r.rc, r.added ? " (enabled by this vote)" : "");
    }
  }
  note("rails: step %d error %d (%s); state %#x, PMU lock %#x; %u lock waits, max %u ms", w.step, w.error,
       w.error == PianoWcnRailRead ? "a rail could not be read" : w.error == PianoWcnRailVote ? "a vote was refused"
       : w.error == PianoWcnRailDropped ? "a rail came back lower" : w.error == PianoWcnBusy ? "the PMU lock stayed held" : "ok",
       piano_wcn_load(&piano_wcn_rails_state), piano_wcn_load(&piano_wcn_pmu_owner), w.contended, w.max_wait_ms);
  if (s.bten.apid >= 0)
    note("BT_EN APID %d owner EE %u type %02x subtype %02x: before mode %02x vin %02x pull %02x out_src %02x en %02x (%s%s) -> "
         "out_src %02x en %02x; %u PMIC writes%s",
         s.bten.apid, s.bten.owner, s.bten.type, s.bten.subtype, s.regs[0], s.regs[1], s.regs[2], s.regs[4], s.regs[6],
         s.configured ? "the DT's state" : "not the DT's state", s.wasHigh ? ", found high: low 100 ms first" : "", s.after[4],
         s.after[6], s.pmicWrites, s.failedReg ? ", one did not read back" : "");
  if (s.failedReg) note("BT_EN: register %#04x written %02x, read %02x", s.failedReg, s.failedWant, s.failedRead);
  if (!s.error || s.error == kBtStopped)
    note("level 2: %s: rails up (state %#x), BT_EN %s, SW_CTRL in %u, WLAN_EN out %u (not ours)",
         s.error ? "stopped" : "done", piano_wcn_load(&piano_wcn_rails_state), s.high ? "high" : "low", s.swCtrlIo & 1u,
         (s.wlanEnIo >> 1) & 1u);
}

void reportUart(const UartState &u, const Port &p)
{
  note("level 3: GCC QUP2 votes %08x -> %08x (added %08x); branches m-ahb %08x s-ahb %08x core %08x core2x %08x s6 %08x after %u polls",
       u.voteBefore, u.voteAfter, u.added, u.cbcr[kBrMAhb], u.cbcr[kBrSAhb], u.cbcr[kBrCore], u.cbcr[kBrCore2x], u.cbcr[kBrS6],
       u.branchPolls);
  if (u.step == -kStepClocks) return;
  const FwRevision fw = decodeFwRevision(u.fwRevision);
  note("level 3: QUP2 version %08x; SE6 firmware %08x (protocol %u: %s); FIFO interface %s; FIFOs tx %u rx %u words of %u bits",
       u.qupVersion, u.fwRevision, fw.protocol, protocolName(fw.protocol), (u.ifDisable & kFifoIfDisable) ? "OFF" : "on", u.tx.depth,
       u.rx.depth, u.tx.widthBits);
  if (u.step == -kStepSeCheck) return;
  if (u.dfs)
    note("level 3: SE6 source: DFS on (cmd_dfsr %08x), level %u (%u Hz); the RCG is not written", u.dfsCmd, u.clock.level,
         u.clock.sourceHz);
  else
    note("level 3: SE6 source: RCG %u Hz -> %u Hz (cfg %08x -> %08x, update after %u polls)", u.rcgHzBefore, u.clock.sourceHz,
         u.rcgCfgBefore, u.rcgCfgAfter, u.rcgPolls);
  note("level 3: serial clock %u Hz / %u / %u = %u baud (%d ppm); %u set-up writes", u.clock.sourceHz, u.clock.divider,
       u.clock.sampling, u.clock.baud, u.clock.errorPpm, u.planWrites);
  if (u.loopBytes) note("level 3: loopback: %u of %u bytes echoed, %u wrong", u.loopEchoed, u.loopBytes, u.loopMismatch);
  if (u.pinsMuxed || u.step == kStepPins)
    note("level 3: pins gpio24 %08x gpio25 %08x gpio26 %08x gpio27 %08x; IOS %08x (rx line %u, cts %s); %u bytes drained", u.pinCfg[0],
         u.pinCfg[1], u.pinCfg[2], u.pinCfg[3], u.ios, (u.ios & kIosRx) != 0, (u.ios & kIosCts) ? "high: the controller is not ready"
                                                                                                 : "low: the controller is ready",
         u.drained);
  if (!u.error) note("level 3: done: UART at %u baud 8N1, RTS/CTS, polled FIFO (tx %u, rx %u words)", p.baud, p.txDepth, p.rxDepth);
}

void reportSetup(const SetupResult &r, const Port &p, bool haveAddress)
{
  note("level 4: version product %08x patch %04x rom %04x soc %08x (rom code 0x%02x); first reset %s", r.version.product,
       r.version.patch, r.version.rom, r.version.soc, r.version.romCode,
       !gArgs.reset0 ? "not sent" : r.reset0 == kSetupOk ? (r.reset0Status ? "answered with an error status" : "answered")
                                                         : setupErrorName(r.reset0));
  if (r.step < 0 && -r.step <= kStepVersion) return;
  note("level 4: %u bit/s%s; rampatch %s (%u bytes, %u segments, %s mode %u); board id %04x%s; NVM %s (%u bytes, %u segments, %u tags, "
       "hci %02x -> %02x, baud %02x -> %02x, sleep %02x -> %02x); %u acknowledgements, %u other events",
       r.baud, r.versionAtSpeed.soc ? " (the version read again there)" : "", r.rampatch, r.rampatchPlan.bytes, r.rampatchPlan.segments,
       r.rampatchPlan.kind == FwKind::elf ? "ELF" : "TLV", r.rampatchPlan.mode, r.boardId, r.boardIdRead ? "" : " (not read)",
       r.nvm[0] ? r.nvm : "-", r.nvmPlan.bytes, r.nvmPlan.segments, r.nvmInfo.tags, r.nvmInfo.hciBefore, r.nvmInfo.hciAfter,
       r.nvmInfo.baudBefore, r.nvmInfo.baudAfter, r.nvmInfo.sleepBefore, r.nvmInfo.sleepAfter, r.acks, r.unmatched);
  if (r.build[0]) note("level 4: build \"%s\"", r.build);
  if (r.addressRead)
    note("level 4: address: the controller started with %s; %s", r.addressIsNvmDefault ? "its NVM's default" : "another address",
         !haveAddress ? "no private address in this ADT (/arm-io/piano-bt): it stays (a host stack would see it unconfigured)"
         : r.addressSet ? (r.addressRewritten ? "the unit's own set and read back, lost by the reset, written again"
                                              : r.addressKept ? "the unit's own set and read back, kept over the reset"
                                                              : "the unit's own set and read back")
                        : "the unit's own NOT set");
  if (r.step == kStepLocalVersion && !r.error)
    note("level 4: HCI %u revision %04x, LMP %u subversion %04x, manufacturer %04x (%s)", r.local.hci, r.local.hciRevision, r.local.lmp,
         r.local.lmpSubversion, r.local.manufacturer, r.local.manufacturer == kManufacturerQualcomm ? "Qualcomm" : "NOT Qualcomm");
  note("uart: %llu bytes sent, %llu received, %u TX timeouts, %u cancels, %u aborts, %u IBS bytes, %u packets dropped", p.txBytes,
       p.rxBytes, p.txTimeouts, p.cancels, p.aborts, p.ibs, p.droppedPackets);
  if (r.error && r.error != kSetupStopped)
    note("level 4: FAILED at step %d (%s; last opcode %04x status %02x hardware error %02x)", -r.step, setupErrorName(r.error),
         r.lastOpcode, r.lastStatus, r.hardwareError);
  else if (!r.error)
    note("level 4: done: the controller is set up at %u bit/s and idle (no host stack yet: docs/drivers/bluetooth.md levels 5..7)",
         r.baud);
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoBluetooth : public IOService {
  OSDeclareDefaultStructors(PianoBluetooth)
  void run();
  void survey();
  void levels();
  static void thread(void *self, wait_result_t);
  IOLock *fLock = nullptr;
  bool fWant = false, fSuspended = false, fJoined = false;
  unsigned fRuns = 0;

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  void systemWillShutdown(IOOptionBits specifier) override;
  void quiesce(const char *why);
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
  note("start: piano-bt=%u delay=%u%s se=%u smmu=%u hwcg=%u spmi=%u%s (WCN7860 Bluetooth on QUP2 SE6 %#llx \"UART14\", SPI %u, BT_EN "
       "PPID 0x%03x)",
       gArgs.level, gArgs.delay, gArgs.delayDefaulted && gArgs.delay ? " (piano-wifi runs in this boot)" : "", gArgs.se, gArgs.smmu,
       gArgs.hwcg, gArgs.spmi, gArgs.spmiDefaulted && !gArgs.spmi ? " (PianoWifi reads BT_EN in this boot)" : "",
       (unsigned long long)kSe6, kSpiUart14, kBtEnPpid);
  if (gArgs.level >= 2)
    note("levels: 1 survey, 2 power%s%s (baud %u, peri %u, ibs %u, reset0 %u, loop %u, release %u, blind %u, addr %u, stop %u)",
         gArgs.level >= 3 ? ", 3 UART" : "", gArgs.level >= 4 ? ", 4 controller" : "", gArgs.baud, gArgs.peri, gArgs.ibs, gArgs.reset0,
         gArgs.loop, gArgs.release, gArgs.blind, gArgs.addr, gArgs.stop);
  if (gArgs.level > 4) note("piano-bt=%u: levels above 4 are not implemented; running up to level 4", gArgs.level);
  note("level 1: loads only, nothing written (the SPMI observer's read command is the only store)");
  setProperty("piano-bt", gArgs.level, 32);
  fLock = IOLockAlloc();
  if (!fLock) {
    stage(-1, "no lock");
    return true;
  }
  if (gArgs.level >= 2) fJoined = pianopower::join(this) == kIOReturnSuccess;   // kOff takes the radio down before a sleep
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

void PianoBluetooth::survey()
{
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

  if (gArgs.spmi) {
    stage(5, "BT_EN over the SPMI observer channel (observer reads)");
    // The PMU lock keeps a PianoWifi that takes it (docs/drivers/bluetooth.md section 7.2) off the same peripheral meanwhile.
    gWcn.rails.bound_ms = 5000;
    const bool held = !piano_wcn_pmu_lock(&gWcn.rails, 1);
    gBtEn = readBtEn();
    if (held) piano_wcn_pmu_lock(&gWcn.rails, 0);
    gWcn.rails.bound_ms = 0;
    reportBtEn(gBtEn);
  } else {
    note("stage 5: BT_EN not read (%s)", gArgs.spmiDefaulted ? "PianoWifi reads it in this boot; piano-bt-spmi=1 overrides" : "piano-bt-spmi=0");
  }

  // The summary (one line, also the piano-bt-survey property) and what it means for the next levels.
  bool pinsAsDt = true;
  for (unsigned i = kPinCts; i <= kPinRx; ++i) pinsAsDt = pinsAsDt && pinAsDt(kPins[i], decodePin(p.pin[i].cfg, p.pin[i].io));
  const int level = btEnLevel(gBtEn);
  const FwRevision fw = decodeFwRevision(s.fwRevision);
  const PinDecode wlan = decodePin(p.pin[kPinWlanEn].cfg, p.pin[kPinWlanEn].io), sw = decodePin(p.pin[kPinSwCtrl].cfg, p.pin[kPinSwCtrl].io);
  char summary[240];
  snprintf(summary, sizeof summary, "bt_en %s owner %s | wlan_en %u sw_ctrl %u | se6 %s%s | uart pins %s | smmu 0x423 %s | codec %s",
           level < 0 ? "?" : level ? "high" : "low", gBtEn.read ? (gBtEn.owner == 0 ? "apps" : "other") : "?", wlan.out, sw.in,
           c.seOn ? "clocked" : "unclocked", s.read ? (fw.protocol == kProtoUart ? " uart" : " NOT-uart") : "", pinsAsDt ? "as dt" : "not as dt",
           !gArgs.smmu ? "skipped" : m.error ? "?" : !m.count ? "none" : ((m.found[0].s2cr >> 16) & 3u) == 1 ? "bypass" : "translate",
           codecOk ? "ok" : "FAILED");
  setProperty("piano-bt-survey", summary);
  note("summary: %s", summary);
  if (s.read && fw.protocol != kProtoUart)
    note("verdict: SE6 does not carry the UART protocol (protocol %u): stop; the boot chain configures SE6 differently than "
         "for Ubuntu", fw.protocol);
  else if (gBtEn.read && gBtEn.owner != 0)
    note("verdict: BT_EN's SPMI peripheral is owned by EE %u, not the apps processor: level 2 cannot drive it from here", gBtEn.owner);
  else
    note("verdict: level 3 %s; level 2 %s", c.seOn ? "finds SE6 clocked already" : "votes SE6's clocks first",
         level == 1 ? "finds BT_EN already high (something powered the Bluetooth side)" : "raises BT_EN after the shared rails");
  stage(6, "the survey is done");
  note("level 1: %llu MMIO loads, 0 MMIO stores; %u SPMI observer reads; %u pages mapped, %u map failures", (unsigned long long)gLoads,
       gSpmiReads, gPageCount, gMapFailures);
  {   // the SPMI lock every kext shares (PianoSpmi.h): this kext's transactions and waits, and every kext's
    char lockLine[360];
    pianospmi::format(lockLine, sizeof lockLine);
    note("%s", lockLine);
  }
  unmapAll();
}

// One climb of the ladder (at start, and after every wake), under fLock; a pending sleep ends it at once (gAbort).
void PianoBluetooth::levels()
{
  unsigned level = gArgs.level > 4 ? 4 : gArgs.level;
  ++fRuns;
  note("levels 2..%u: climb %u%s", level, fRuns, fRuns > 1 ? " (after a wake)" : "");
  if (!mapWindows()) {
    stage(-200, "levels 2+: the windows could not be mapped; nothing was written");
    return;
  }
  if (level >= 4) {
    const char *partition = nullptr;
    int rc = PianoBtFwNotReady;
    for (unsigned i = 0; i < 30 && rc == PianoBtFwNotReady && !hwCancelled(nullptr); ++i) {
      rc = piano_bt_firmware_open(&partition);
      if (rc == PianoBtFwNotReady) IOSleep(1000);
    }
    if (rc) {
      note("level 4 not run: no firmware partition (%d: %s); stopping at level 3", rc,
           rc == PianoBtFwNotReady ? "NativeUfs has not published bluetooth_a/bluetooth_b" : "not a FAT volume, or no memory");
      level = 3;
    }
  }
  const bool haveAddress = level >= 4 && gArgs.addr && readAddress();
  if (level >= 4) note("level 4: %s", haveAddress ? "a private address is in this ADT: it will be set (never logged)"
                                                  : gArgs.addr ? "no usable private address in this ADT: the NVM's default stays"
                                                               : "piano-bt-addr=0: the NVM's default stays");
  const Options o = bringupOptions(level, haveAddress);
  stage(200, "level 2: the shared rails and BT_EN");
  int rc = powerOn(gB, gHw, o);
  reportPower(gB.power);
  writeSummary("level 2");
  if (rc) {
    stage(gB.power.step, rc == kBtStopped ? "level 2 stopped here on request" : "level 2 stopped here");
    if (rc != kBtStopped)
      note("level 2: FAILED at step %d (%s): BT_EN low, the rails put; nothing above this level ran", -gB.power.step,
           bringupErrorName(rc));
  }
  if (!rc && level >= 3) {
    stage(300, "level 3: the UART (clocks, source, engine, loopback, pins)");
    rc = uartUp(gB, gHw, o);
    reportUart(gB.uart, gB.port);
    writeSummary("level 3");
    if (rc) {
      stage(gB.uart.step, rc == kBtStopped ? "level 3 stopped here on request" : "level 3 stopped here");
      if (rc != kBtStopped)
        note("level 3: FAILED at step %d (%s): the UART stopped, BT_EN low, clocks and rails back", -gB.uart.step, bringupErrorName(rc));
    }
  }
  if (!rc && level >= 4) {
    stage(400, "level 4: the controller (HCI over the UART)");
    rc = controllerUp(gB, gHw, o, gFwIo, gNvm, sizeof gNvm);
    reportSetup(gB.setup, gB.port, haveAddress);
    writeSummary("level 4");
    fwDrop();
    piano_bt_firmware_close();
    if (rc && rc != kBtStopped) stage(-401, "level 4 stopped before the controller was set up");
  }
  memset(gAddress, 0, sizeof gAddress);
  gB.up = !rc;
  char state[160];
  snprintf(state, sizeof state, "level %u %s step %d%s%s", level, !rc ? "up" : rc == kBtStopped ? "stopped" : "FAILED",
           !rc ? (level >= 4 ? gB.setup.step : level >= 3 ? gB.uart.step : gB.power.step) : piano_bt_stage, rc ? " " : "",
           rc ? bringupErrorName(rc) : "");
  setProperty("piano-bt-state", state);
  note("state: %s; rails %#x", state, piano_wcn_load(&piano_wcn_rails_state));
  if (!rc) stage(int32_t(level * 100 + 99), "the ladder reached its level");
}

void PianoBluetooth::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  survey();
  if (gArgs.level < 2) {
    unmapRegion(gCore);
    unmapRegion(gCnfg);
    unmapRegion(gObsv);
    return;
  }
  IOLockLock(fLock);
  fWant = true;
  for (;;) {
    while (!fWant) IOLockSleep(fLock, &fWant, THREAD_UNINT);
    fWant = false;
    if (fSuspended) continue;
    levels();
  }
}

// Off: whatever runs stops at its next poll, then everything this driver holds is let go (BtBringup.h safeOff). Synchronous: the
// engine's cancels, one SPMI write, the vote word and at most six rail transactions.
void PianoBluetooth::quiesce(const char *why)
{
  __atomic_store_n(&gAbort, true, __ATOMIC_RELEASE);
  IOLockLock(fLock);
  const bool wasUp = gB.up;
  safeOff(gB, gHw, bringupOptions(gArgs.level, false));
  fSuspended = true;
  __atomic_store_n(&gAbort, false, __ATOMIC_RELEASE);
  IOLockUnlock(fLock);
  note("%s: the radio is off (%s; BT_EN low, the UART stopped, the clock votes and the rails let go; rails state %#x)", why,
       wasUp ? "it was up" : "it was not up", piano_wcn_load(&piano_wcn_rails_state));
}

IOReturn PianoBluetooth::setPowerState(unsigned long state, IOService *)
{
  if (!fLock || gArgs.level < 2) return pianopower::kAckNow;
  if (state == pianopower::kOff) {
    quiesce("system sleep");
    return pianopower::kAckNow;
  }
  IOLockLock(fLock);
  if (fSuspended) {
    fSuspended = false;
    fWant = true;
    IOLockWakeup(fLock, &fWant, true);
    note("wake: the ladder climbs again to level %u", gArgs.level > 4 ? 4 : gArgs.level);
  }
  IOLockUnlock(fLock);
  return pianopower::kAckNow;
}

void PianoBluetooth::systemWillShutdown(IOOptionBits specifier)
{
  if (fLock && gArgs.level >= 2) quiesce("shutdown or restart");
  IOService::systemWillShutdown(specifier);
}
