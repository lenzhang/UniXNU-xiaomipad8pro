// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoPower: battery and charging for macOS 27 on piano (SM8750), native at EL1. Design and test plan: docs/drivers/charging.md.
//
// The charger (the buck stage of the PMIH010x PMIC) belongs to the audio DSP's firmware; this processor may only read it over SPMI.
// The DSP is not running when our UEFI hands over, and without it the PMIC charges on whatever configuration the boot stages left.
// So there are two levels:
//   piano-power=1  read only. Every 30 s one report on the console: the battery from its two fuel gauges (voltage, current, state of
//                  charge, temperature), the USB-C port from the charger PMIC's Type-C block (attached, a source's Rp), the DSP's
//                  state from SMEM. No PMIC register is written and the DSP is not started; the only writes are the SPMI observer
//                  channel's command and the four gauge pins' TLMM configuration (taken as GPIOs for the read, then given back).
//   piano-power=2  the same, and the charger: the DSP is started (its images from modem_a, PAS) with GLINK and the IPC router's service
//                  locator, then the charger properties are read every 5 s and the conservative policy of PianoMca.c is applied: input
//                  current limit by charger type (at most 1.5 A at 5 V), charge current 1.0 A between 15 and 38 degC (0.5 A outside or
//                  without a temperature), the charger's enable bit set if it is clear, back to the firmware's own 100 mA / 500 mA on
//                  detach. USB-C port notifications are acknowledged. Nothing else is written.
// Further boot arguments: piano-power-period=S (report interval, default 30), piano-power-gauge=0 (no gauge reads),
// piano-power-write=0 (level 2 without the charger writes: the DSP runs on its own defaults), piano-power-hv=1 (level 2 also asks a PD
// source with a fixed 9 V PDO for 9 V and then allows 1.6 A), piano-power-fw=DIR (the DSP's images as DIR/adsp.mbn and
// DIR/adsp_dtb.mbn instead of modem_a). Every step logs "PianoPower: ...".
// At both levels the PMIC RTC is read with every report from the first one after IOBSD is published (RtcReadout.h,
// docs/drivers/rtc.md): its count, enable bit and the date with the offset of piano-rtc-offset=N or else the SDAM cell. Read only;
// the kernel's calendar from it is k27 0060 (piano-rtc=1).
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "SpmiObserver.h"
#include "ChargerPmic.h"
#include "Gauge.h"
#include "PianoBattery.h"
#include "Servreg.h"
#include "RtcReadout.h"
extern "C" {
#include "PianoAdsp.h"
#include "AdspHealth.h"
#include "PianoMca.h"
}

using namespace pianopower;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last start stage reached, negative when it failed there.
volatile int32_t piano_power_stage = 0;
}

namespace {

struct Args {
  uint32_t level, period, gauge, write, hv, unsuspend, ps, psFloor, board, ucsi;
  bool hasRtcOffset;
  uint32_t rtcOffset;
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
  gArgs.level = bootArg("piano-power", 0);
  gArgs.period = bootArg("piano-power-period", 30);
  if (gArgs.period < 5) gArgs.period = 5;
  if (gArgs.period > 3600) gArgs.period = 3600;
  gArgs.gauge = bootArg("piano-power-gauge", 1);
  gArgs.write = bootArg("piano-power-write", 1);
  gArgs.hv = bootArg("piano-power-hv", 0);
  gArgs.unsuspend = bootArg("piano-power-unsuspend", 0);
  gArgs.board = bootArg("piano-power-board", 1);   // the stock board configuration at level 2 with writes (PianoMca.h)
  gArgs.ucsi = bootArg("piano-power-ucsi", 1);     // one read of the UCSI owner's buffer per charger generation (PianoMca.h)
  gArgs.ps = bootArg("piano-power-ps", 0);            // the battery for macOS (PianoBattery.cpp)
  gArgs.psFloor = bootArg("piano-power-ps-floor", 0); // the lowest percentage shown on battery (BatteryMap.h)
  if (gArgs.psFloor > 50) gArgs.psFloor = 50;
  gArgs.rtcOffset = 0;
  gArgs.hasRtcOffset = PE_parse_boot_argn("piano-rtc-offset", &gArgs.rtcOffset, sizeof gArgs.rtcOffset);
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoPower: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_power_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// Explicit ldr/str, as PianoDwc3 does: every store from a general register.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }

struct Region {
  IOMemoryMap *map;
  uintptr_t va;
};

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

// ------------------------------------------------------------------------------------------------------------ SPMI (read only)
Region gCore, gCnfg, gObsv;
uintptr_t windowBase(Window w) { return w == Window::core ? gCore.va : w == Window::cnfg ? gCnfg.va : gObsv.va; }
uint32_t ioRd(void *, Window w, uint32_t offset) { return rd32(windowBase(w) + offset); }
void ioWr(void *, Window w, uint32_t offset, uint32_t value) { wr32(windowBase(w) + offset, value); }
void ioDelay(void *, unsigned us) { IODelay(us); }
const Io gIo = {nullptr, ioRd, ioWr, ioDelay};

// ------------------------------------------------------------------------------------------------- the gauges' pins (TLMM)
// TLMM (DT pinctrl@f100000): pin n's configuration at n * 0x1000 (pull 1:0, function 5:2, drive 8:6, output enable 9), its input
// (bit 0) and output (bit 1) at +4 (UniXNU-sheng TouchBoard.h). For the read a pin is a GPIO with the pull-up on, its output value
// 0: a line is released (output disabled) or driven low (output enabled). The bits above 9 stay as they were.
constexpr uint64_t kTlmm = 0x0f100000;
constexpr uint32_t kTlmmStride = 0x1000, kTlmmPullUp = 3, kTlmmOe = 1u << 9;

struct Pin {
  unsigned gpio;
  Region page;
  uint32_t saved;
};
struct GaugeBus {
  const char *name;   // as the log calls it
  const char *where;  // the DT node
  Pin sda, scl;
};
GaugeBus gBuses[2] = {
    {"A", "i2c@a94000 qup1_se5", {52, {}, 0}, {53, {}, 0}},
    {"B", "i2c@88c000 qup2_se3", {12, {}, 0}, {13, {}, 0}},
};

uint32_t gpioCfg(const Pin &p, bool drive) { return (p.saved & ~0x3ffu) | kTlmmPullUp | (drive ? kTlmmOe : 0); }
bool mapPin(Pin &p) { return mapRegion(p.page, kTlmm + uint64_t(p.gpio) * kTlmmStride, 8); }
void takePin(Pin &p)
{
  p.saved = rd32(p.page.va);
  wr32(p.page.va + 4, 0);                  // output value 0, used only while the output is enabled
  wr32(p.page.va, gpioCfg(p, false));      // a GPIO, released
}
void givePin(Pin &p) { wr32(p.page.va, p.saved); }

void busSda(void *ctx, bool release) { Pin &p = static_cast<GaugeBus *>(ctx)->sda; wr32(p.page.va, gpioCfg(p, !release)); }
void busScl(void *ctx, bool release) { Pin &p = static_cast<GaugeBus *>(ctx)->scl; wr32(p.page.va, gpioCfg(p, !release)); }
bool busReadSda(void *ctx) { return rd32(static_cast<GaugeBus *>(ctx)->sda.page.va + 4) & 1; }
bool busReadScl(void *ctx) { return rd32(static_cast<GaugeBus *>(ctx)->scl.page.va + 4) & 1; }
void busDelay(void *, unsigned us) { IODelay(us); }

GaugeReading readBus(GaugeBus &b)
{
  takePin(b.scl);
  takePin(b.sda);
  IODelay(20);
  const I2cIo io = {&b, busSda, busScl, busReadSda, busReadScl, busDelay};
  I2cBus bus(io);
  GaugeReading g = readGauge(bus);
  givePin(b.sda);
  givePin(b.scl);
  return g;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoPower : public IOService {
  OSDeclareDefaultStructors(PianoPower)

  // SPMI
  Arbiter fArbiter;
  ChargerLayout fCharger;
  bool fSpmi = false;
  RtcReading fRtc;
  bool fRtcProbed = false;
  // gauges
  bool fGauges = false;
  GaugeReading fPack[2];
  PackReading fBattery;
  // the DSP and its charger properties (level 2)
  PianoMca fMca;
  PianoAdspChannel *fChannel = nullptr;
  IOSimpleLock *fRingLock = nullptr;
  static constexpr unsigned kRing = 8;
  struct {uint32_t length; uint8_t data[PIANO_ADSP_MAX_MESSAGE];} fRing[kRing];
  unsigned fRingHead = 0, fRingCount = 0;
  uint32_t fDropped = 0;
  volatile uint32_t fLinkUp = 0, fLinkChanged = 0;
  bool fWasUp = false;
  // the charger_pd gate (serviceMca): the channel's state, since when, whether the 60 s fallback was taken, the generation linked
  bool fChannelUp = false, fGateTimedOut = false;
  uint64_t fChannelSinceMs = 0;
  uint32_t fPdUpsLinked = 0;
  uint64_t fRequestDue = 0, fPanDue = 0, fUcsiDue = 0, fCycleDue = 0, fReadyWarnDue = 0;
  uint32_t fUcsiNotesLogged = 0;
  uint64_t fStartMs = 0;

  bool probeSpmi();
  void readRtcLine(char *line, size_t size);
  void surveyAdsp();
  void probeGauges();
  void sampleGauges();
  void report();
  void startCharger();
  void serviceMca(uint64_t now);
  void stepQuiet(uint64_t now);
  // The quiet flag for the DSP's other clients (PianoAdspContract.h): raised per charger generation until it is configured.
  PowerQuiet fQuiet = {};
  static constexpr uint32_t kQuietCapMs = 60000;
  void pumpMca(uint64_t now);
  void logMcaEvents();
  void mcaLine(char *line, size_t size);
  void diagLine(char *line, size_t size);
  void diagPortLine(char *line, size_t size, bool board);
  void publishBattery(uint64_t t, const PortState *port);
  void run();
  static void thread(void *self, wait_result_t);
  static void channelState(void *context, int up);
  static void channelReceive(void *context, const uint8_t *data, uint32_t length);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoPower, IOService)

IOService *PianoPower::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoPower::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-power=%u period %u s, gauges %s; %s", gArgs.level, gArgs.period, gArgs.gauge ? "on" : "off",
       gArgs.level >= 2 ? "level 2: the DSP's charger firmware is started and its charger properties are managed"
                        : "level 1: read only (no PMIC register written, the DSP is not started)");
  if (gArgs.level >= 2)
    note("level 2 policy: %s%s", gArgs.write ? "input limit by charger type (<= 1.5 A at 5 V), charge current 1.0 A in 15-38 C"
                                             : "no charger writes (piano-power-write=0): the DSP keeps its own 100 mA input limit",
         gArgs.write && gArgs.hv ? ", 9 V from a PD source's fixed PDO (piano-power-hv=1, then <= 1.6 A)" : "");
  if (gArgs.level >= 2 && gArgs.write && gArgs.unsuspend)
    note("piano-power-unsuspend=1: the input-suspend flag (0x2019) is written to 0 when it reads non-zero (never to 1)");
  if (gArgs.ps)
    note("piano-power-ps=1: the battery is published to macOS as AppleSmartBattery (IOPMPowerSource) from the gauges and the "
         "Type-C state, refreshed every %u s%s%s", gArgs.period, gArgs.psFloor ? "; the percentage shown on battery stays at least " : "",
         gArgs.psFloor ? "the floor (piano-power-ps-floor)" : "");
  setProperty("piano-power", gArgs.level, 32);
  fRingLock = IOSimpleLockAlloc();
  thread_t t = nullptr;
  if (!fRingLock || kernel_thread_start(&PianoPower::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;   // stays attached and does nothing more
  }
  thread_deallocate(t);
  return true;
}

void PianoPower::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-power");
  static_cast<PianoPower *>(self)->run();
}

bool PianoPower::probeSpmi()
{
  if (!mapRegion(gCore, kSpmiCore, kSpmiCoreBytes) || !mapRegion(gCnfg, kSpmiCnfg, kSpmiCnfgBytes) ||
      !mapRegion(gObsv, kSpmiObsv, kSpmiObsvBytes)) {
    stage(-1, "SPMI arbiter windows do not map");
    return false;
  }
  fArbiter = spmiArbiter(gIo);
  note("spmi: arbiter version %#x, %u APIDs%s", fArbiter.version, fArbiter.count, fArbiter.v7 ? "" : " (not a v7 arbiter: stop)");
  if (!fArbiter.v7) {
    stage(-1, "SPMI arbiter");
    return false;
  }
  fCharger = discoverCharger(gIo, fArbiter);
  note("charger PMIC (SID 7): %u peripherals, owned by EE0 %u, EE1 %u, EE2 %u, EE3 %u, others %u", fCharger.peripherals,
       fCharger.ownedBy[0], fCharger.ownedBy[1], fCharger.ownedBy[2], fCharger.ownedBy[3],
       fCharger.ownedBy[4] + fCharger.ownedBy[5] + fCharger.ownedBy[6] + fCharger.ownedBy[7]);
  if (!fCharger.found) {
    note("charger: not found (%s; REVID type %#x model %#x): no port state in the report", fCharger.why, fCharger.revType,
         fCharger.model);
    stage(1, "SPMI (no charger layout)");
    return true;
  }
  note("charger: PMIH010x model %#x, charger block %#x (subtype %#x, APID %d, owner EE %u), DCDC %#x (APID %d, EE %u), "
       "USBIN %#x (APID %d, EE %u), Type-C %#x (APID %d, EE %u)%s",
       fCharger.model, fCharger.charger.address, fCharger.subtype, fCharger.charger.apid, fCharger.charger.owner,
       fCharger.dcdc.address, fCharger.dcdc.apid, fCharger.dcdc.owner, fCharger.usbin.address, fCharger.usbin.apid,
       fCharger.usbin.owner, fCharger.typec.address, fCharger.typec.apid, fCharger.typec.owner,
       fCharger.charger.owner ? ": writes belong to that EE (the DSP's firmware), this side reads" : "");
  fSpmi = true;
  stage(1, "SPMI");
  return true;
}

// "rtc: COUNT (DATE UTC, offset N from SOURCE), enabled E": tooling/native/rtc-offset.py pairs these lines with the Mac's clock.
void PianoPower::readRtcLine(char *line, size_t size)
{
  fRtc = readRtc(gIo, fArbiter, gArgs.hasRtcOffset, gArgs.rtcOffset, &fRtc);
  const RtcReading &r = fRtc;
  if (!r.ok) {
    snprintf(line, size, "rtc: not read (%s)", r.why);
    return;
  }
  struct piano_rtc_date d;
  piano_rtc_civil(r.utc, &d);
  if (r.source == RtcOffsetSource::none)
    snprintf(line, size, "rtc: %u (no offset: the SDAM cell does not read and there is no piano-rtc-offset), enabled %u", r.count,
             r.enabled ? 1u : 0u);
  else if (!r.dated)
    snprintf(line, size, "rtc: %u (+ offset %u from %s = %u, year %u: not a date, measure piano-rtc-offset), enabled %u", r.count,
             r.offset, rtcSourceName(r.source), r.utc, d.year, r.enabled ? 1u : 0u);
  else
    snprintf(line, size, "rtc: %u (%04u-%02u-%02u %02u:%02u:%02u UTC, offset %u from %s), enabled %u", r.count, d.year, d.month,
             d.day, d.hour, d.minute, d.second, r.offset, rtcSourceName(r.source), r.enabled ? 1u : 0u);
}

void PianoPower::surveyAdsp()
{
  PianoAdspSurvey s;
  if (piano_adsp_survey(&s)) {
    note("adsp: survey: window %d does not map", s.mapped);
    return;
  }
  const char *state = s.slave < 0 ? "no SMP2P state from the DSP: not running"
                      : s.slave & 1 ? "fatal"
                      : s.slave & 2 ? "running (ready)"
                      : "SMP2P item there, not ready";
  note("adsp: survey: SMEM APPS/ADSP partition %s, items %#x, slave-kernel %d (%s), AOSS %#x, RSC id %#x%s%s",
       s.partition ? "missing" : "found", s.items, s.slave, state, s.aoss, s.rsc_id, s.crash[0] ? ", crash reason: " : "", s.crash);
  stage(2, "DSP survey");
}

void PianoPower::probeGauges()
{
  if (!gArgs.gauge) {
    note("gauges: off (piano-power-gauge=0)");
    return;
  }
  for (GaugeBus &b : gBuses) {
    if (!mapPin(b.sda) || !mapPin(b.scl)) {
      note("gauge %s: TLMM pins %u/%u do not map", b.name, b.sda.gpio, b.scl.gpio);
      continue;
    }
    note("gauge %s (%s, SDA gpio%u, SCL gpio%u): pins cfg %#x/%#x, lines %u/%u before the read", b.name, b.where, b.sda.gpio,
         b.scl.gpio, rd32(b.sda.page.va), rd32(b.scl.page.va), rd32(b.sda.page.va + 4) & 1, rd32(b.scl.page.va + 4) & 1);
    fGauges = true;
  }
  sampleGauges();
  for (unsigned i = 0; i < 2; ++i) {
    const GaugeReading &g = fPack[i];
    if (g.ok)
      note("gauge %s: bq27z561 answers: %d mV, %d mA, %d %%, %d/%d mAh, %d.%d C, status %#x, %d cycles", gBuses[i].name, g.voltageMv,
           g.currentMa, g.socPct, g.remainingMah, g.fullMah, g.tempDeciC / 10, (g.tempDeciC < 0 ? -g.tempDeciC : g.tempDeciC) % 10,
           g.status, g.cycles);
    else
      note("gauge %s: no reading (%s at command %#x)", gBuses[i].name, i2cName(g.error), g.failedCommand);
  }
  stage(3, "gauges");
}

void PianoPower::sampleGauges()
{
  if (!fGauges) return;
  for (unsigned i = 0; i < 2; ++i) fPack[i] = gBuses[i].sda.page.va && gBuses[i].scl.page.va ? readBus(gBuses[i]) : GaugeReading();
  fBattery = combinePacks(fPack, 2);
  piano_mca_battery_temperature(&fMca, fBattery.ok, fBattery.tempDeciC / 10);
}

void PianoPower::mcaLine(char *line, size_t size)
{
  const PianoMca &m = fMca;
#define HAS(f) piano_mca_has(&m, f)
  snprintf(line, size,
           "mca: bus %d mV %d mA, type %s, input limit %d mA (set %u), charge current set %u mA, tbat %d C, charge enable %d, "
           "status %#x, pack %d/%d, %u cycles, %u writes ok, %u failed, %u timeouts, %u port acks%s",
           HAS(McaBusVoltage) ? m.value[McaBusVoltage] / 1000 : -1, HAS(McaBusCurrent) ? m.value[McaBusCurrent] / 1000 : -1,
           HAS(McaType) ? piano_mca_type_name(m.value[McaType]) : "?", HAS(McaIcl) ? m.value[McaIcl] : -1, m.icl_set, m.fcc_set,
           HAS(McaTbat) ? m.value[McaTbat] : -99, HAS(McaChargeEnable) ? m.value[McaChargeEnable] : -1,
           HAS(McaStatus) ? m.value[McaStatus] : -1, HAS(McaPackVoltage) ? m.value[McaPackVoltage] : -1,
           HAS(McaPackCurrent) ? m.value[McaPackCurrent] : -1, m.cycles, m.writes_ok, m.writes_failed, m.timeouts, m.pan_acks,
           m.hv == McaHvOn ? ", 9 V" : m.hv == McaHvBlocked ? ", 9 V blocked" : "");
#undef HAS
}

// The input path as the firmware reports it, logged after every cycle of the first minute (and then with each report): the three
// properties that can hold the input off whatever the limit says, the decoded charger status, and whether the DSP has sent any
// USB-C port notification at all. Read only: nothing here writes.
// The port as the firmware sees it, the board configuration read back, the charger domain and the gauges (docs/ubuntu-history/
// device.md §3.1): a DSP that believes it sources, or is stuck in an adapter authentication, explains a live bus with no input.
void PianoPower::diagPortLine(char *line, size_t size, bool board)
{
  const PianoMca &m = fMca;
#define HAS(f) piano_mca_has(&m, f)
#define VAL(f) (HAS(f) ? m.value[f] : -1)
  uint32_t ups = 0;
  const uint32_t pd = piano_qrtr_charger_pd(&ups);
  const int termRaw = VAL(McaTermVolt);
  static const char *const kPan[] = {"not answered yet", "answered", "NO ANSWER"};
  static const char *const kUcsi[] = {"off", "due", "asked", "answered", "NO ANSWER"};
  if (!board) {
    snprintf(line, size,
             "diag port: verify (0x21002) %d, PD state (0x2100d) %d, data role (0x2100c) %d, OTG boost (0x200d) %d, gate (0x200e) %d, "
             "plug-in (0x21020) %d, Type-C mode (0x21015) %d, CC toggle (0x21021) %d, sink/source (0x21022) %d, online (0x20001) %d",
             VAL(McaVerify), VAL(McaPdState), VAL(McaDataRole), VAL(McaOtgBoost), VAL(McaOtgGate), VAL(McaOtgPlugin),
             VAL(McaTypecMode), VAL(McaCcToggle), VAL(McaSnkSrc), VAL(McaUsbType));
  } else {
    snprintf(line, size,
             "diag board: CP (0x201c) %d, CID (0x201b) %#x, OTG (0x201a) %d, sent %#x%s; float (0x2007) %d = %d mV; port enable %s; "
             "UCSI %s; charger_pd %s (generation %u); battery (gauges) %d mA",
             VAL(McaCpState), (uint32_t)VAL(McaCidCfg), VAL(McaOtgCfg), m.board_done, m.board_allowed ? "" : " (off)", termRaw,
             HAS(McaTermVolt) ? (termRaw + 3600) / 2 : -1, kPan[m.pan_enabled < 3 ? m.pan_enabled : 0], kUcsi[m.ucsi < 5 ? m.ucsi : 0],
             servreg_state_name(pd), ups, fBattery.ok ? fBattery.currentMa : 0);
  }
#undef VAL
#undef HAS
}

void PianoPower::diagLine(char *line, size_t size)
{
  const PianoMca &m = fMca;
#define HAS(f) piano_mca_has(&m, f)
  snprintf(line, size,
           "diag: input suspend (0x2019) %s, AICL enable (0x2012) %s, AICL status (0x2029) %s%#x, charger status (0x2001) %#x (%s), "
           "bus current (0x20004) %d mA, input limit (0x2008) %d mA; port notifications %u received, %u acked, enable %s; "
           "%u reads, %u errors, %u timeouts, %u ignored, %u notifications",
           HAS(McaInputSuspend) ? (m.value[McaInputSuspend] ? "1 (SUSPENDED)" : "0 (not suspended)") : "not readable",
           HAS(McaAiclEnable) ? (m.value[McaAiclEnable] ? "1" : "0") : "not readable",
           HAS(McaAiclStatus) ? "" : "not readable ", HAS(McaAiclStatus) ? (uint32_t)m.value[McaAiclStatus] : 0u,
           HAS(McaStatus) ? (uint32_t)m.value[McaStatus] : 0u,
           HAS(McaStatus) ? piano_mca_status_name(m.value[McaStatus]) : "not readable",
           HAS(McaBusCurrent) ? m.value[McaBusCurrent] / 1000 : -1, HAS(McaIcl) ? m.value[McaIcl] : -1,
           m.pan_notifies, m.pan_acks, m.pan_enable ? "due" : "sent", m.answers, m.errors, m.timeouts, m.ignored, m.notifications);
#undef HAS
}

// piano-power-ps=1: the battery for macOS (PianoBattery.cpp), from this cycle's gauge and port readings; one line per cycle.
void PianoPower::publishBattery(uint64_t t, const PortState *port)
{
  const BatteryValues v = mapBattery(fPack, 2, fBattery, port, gArgs.psFloor);
  if (!v.known) {
    note("[%llus] ps: no gauge reading: the power source is %s", t, pianoBatteryUpdate(this, v) ? "left as it was" : "not published yet");
    return;
  }
  pianoBatteryUpdate(this, v);
  note("[%llus] ps: %u %%%s, %s, %u mV, %d mA (instant %d), %u/%u mAh (design %u), %u cycles, time remaining %u min%s", t,
       v.percent, v.clamped ? " (floor)" : "",
       v.external ? (v.charging ? "on power, charging" : v.full ? "on power, full" : "on power, not charging") : "on battery",
       v.voltageMv, v.amperageMa, v.instantMa, v.rawMah, v.fullMah, v.designMah, v.cycles, v.timeRemaining,
       v.timeRemaining == kTimeUnknown ? " (unknown)" : "");
}

void PianoPower::report()
{
  const uint64_t t = (nowMs() - fStartMs) / 1000;
  sampleGauges();
  if (fGauges) {
    const PackReading &b = fBattery;
    if (b.ok)
      note("[%llus] battery: %d mV, %d mA (%s), %d %% (%d/%d mAh), %d.%d C, %u of 2 packs | A %d mV %d mA %d %% st %#x | B %d mV %d mA "
           "%d %% st %#x",
           t, b.voltageMv, b.currentMa, b.charging ? "charging" : b.discharging ? "discharging" : "idle", b.socPct, b.remainingMah,
           b.fullMah, b.tempDeciC / 10, (b.tempDeciC < 0 ? -b.tempDeciC : b.tempDeciC) % 10, b.packs, fPack[0].voltageMv,
           fPack[0].currentMa, fPack[0].socPct, fPack[0].status, fPack[1].voltageMv, fPack[1].currentMa, fPack[1].socPct,
           fPack[1].status);
    else
      note("[%llus] battery: no gauge answers (A: %s, B: %s)", t, i2cName(fPack[0].error), i2cName(fPack[1].error));
  }
  // The RTC from the first report after IOBSD is published: the kernel's calendar (k27 0060, IOKitInitializeTime) reads the same
  // observer channels earlier in bsd_init, and IOKitBSDInit publishes IOBSD after it, so the two never share a channel.
  const IOService *resources = IOService::getResourceService();
  if (fArbiter.v7 && (fRtcProbed || (resources && resources->getProperty(kIOBSDKey)))) {
    char line[200];
    readRtcLine(line, sizeof line);
    note("[%llus] %s", t, line);
    if (!fRtcProbed)
      note("rtc: RTC_RW APID %d (owner EE %u) type %#x subtype %#x EN_CTL %#x; SDAM APID %d (owner EE %u), offset cell %s %u (%#x)",
           fRtc.rtc.apid, fRtc.rtc.owner, fRtc.type, fRtc.subtype, fRtc.enCtl, fRtc.sdam.apid, fRtc.sdam.owner,
           fRtc.sdamOk ? "reads" : "does not read:", fRtc.sdamOffset, fRtc.sdamOffset);
    fRtcProbed = true;
  }
  PortState p;
  const bool havePort = fSpmi && fCharger.found;
  if (havePort) {
    p = readPort(gIo, fCharger);
    if (p.ok)
      note("[%llus] usb: %s%s, Rp bits %#x (%u mA), mode %#x | typec %02x %02x %02x %02x %02x %02x rt %#x | usbin %02x %02x "
           "%02x %02x %02x %02x %02x %02x rt %#x | dcdc %02x %02x %02x %02x %02x %02x %02x %02x rt %#x | chgr %02x %02x %02x %02x %02x "
           "%02x %02x %02x",
           t, !p.attached ? "nothing attached" : p.sourceSeen ? "a source is attached (charger present)" : p.sinkSeen ? "a device is attached (we source)" : "attached",
           p.attached ? (p.cc == 2 ? " on CC2" : " on CC1") : "", p.rpBits, rpMilliamps(p.rpBits), p.mode, p.typec[0], p.typec[1],
           p.typec[2], p.typec[3], p.typec[4], p.typec[5], p.typecRt, p.usbin[0], p.usbin[1], p.usbin[2], p.usbin[3], p.usbin[4],
           p.usbin[5], p.usbin[6], p.usbin[7], p.usbinRt, p.dcdc[0], p.dcdc[1], p.dcdc[2], p.dcdc[3], p.dcdc[4], p.dcdc[5], p.dcdc[6],
           p.dcdc[7], p.dcdcRt, p.charger[0], p.charger[1], p.charger[2], p.charger[3], p.charger[4], p.charger[5], p.charger[6],
           p.charger[7]);
    else
      note("[%llus] usb: Type-C status not readable", t);
  }
  if (gArgs.ps) publishBattery(t, havePort && p.ok ? &p : nullptr);
  if (gArgs.level >= 2) {
    PianoAdspStatus s;
    piano_adsp_status(&s);
    uint32_t open = 0, delivered = 0, refused = 0;
    piano_qrtr_port_counters(&open, &delivered, &refused);
    PianoAdspHealth h;
    piano_adsp_health(&h);
    note("[%llus] adsp: stage %d (%s)%s, slave-kernel %d, %u fatal, %u doorbells, GLINK %u in %u out, channels %#x%s, qrtr %u (%u "
         "asked, %u answered, %u services), link generation %u, quiet %#x, client ports %u (%u delivered, %u refused)%s%s",
         t, s.stage, piano_adsp_stage_name(s.stage), s.stage < 0 ? " FAILED" : "", s.slave, s.fatal, s.doorbells, s.glink_in, s.glink_out,
         s.channels_up, s.broken ? " BROKEN" : "", s.qrtr_stage, s.qrtr_requests, s.qrtr_answers, s.qrtr_services, h.link_generation,
         h.quiet, open, delivered, refused, s.text[0] ? ", " : "", s.text);
    char line[520];
    mcaLine(line, sizeof line);
    note("[%llus] %s", t, line);
    diagLine(line, sizeof line);
    note("[%llus] %s", t, line);
    diagPortLine(line, sizeof line, false);
    note("[%llus] %s", t, line);
    diagPortLine(line, sizeof line, true);
    note("[%llus] %s", t, line);
  } else {
    PianoAdspSurvey s;
    if (!piano_adsp_survey(&s)) note("[%llus] adsp: slave-kernel %d (not started at level 1)", t, s.slave);
  }
}

// ---------------------------------------------------------------------------------------- level 2: the charger properties
void PianoPower::channelState(void *context, int up)
{
  PianoPower *self = static_cast<PianoPower *>(context);
  __atomic_store_n(&self->fLinkUp, up ? 1u : 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&self->fLinkChanged, 1u, __ATOMIC_RELEASE);
  thread_wakeup((event_t)self);
}

void PianoPower::channelReceive(void *context, const uint8_t *data, uint32_t length)
{
  PianoPower *self = static_cast<PianoPower *>(context);
  IOSimpleLockLock(self->fRingLock);
  if (self->fRingCount < kRing && length <= PIANO_ADSP_MAX_MESSAGE) {
    auto &slot = self->fRing[(self->fRingHead + self->fRingCount) % kRing];
    memcpy(slot.data, data, length);
    slot.length = length;
    self->fRingCount++;
  } else {
    self->fDropped++;
  }
  IOSimpleLockUnlock(self->fRingLock);
  thread_wakeup((event_t)self);
}

void PianoPower::startCharger()
{
  piano_mca_init(&fMca, gArgs.write ? 1 : 0, gArgs.hv != 0);
  piano_mca_allow_unsuspend(&fMca, gArgs.unsuspend != 0);
  piano_mca_allow_board(&fMca, gArgs.board != 0);
  piano_mca_allow_ucsi(&fMca, gArgs.ucsi != 0);
  if (fBattery.ok) piano_mca_battery_temperature(&fMca, 1, fBattery.tempDeciC / 10);
  static const PianoAdspClient client = {&PianoPower::channelState, &PianoPower::channelReceive};
  fChannel = piano_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 4, 0, &client, this);
  if (!fChannel) {
    stage(-4, "no PMIC_RTR_ADSP_APPS channel slot");
    return;
  }
  note("level 2: starting the DSP after piano-power-delay (default 30 s); once it runs, its firmware limits the input to 100 mA until "
       "the input limit write takes (watch for 'mca: write input limit ... ok')");
  piano_adsp_start();
  fReadyWarnDue = nowMs() + 180000 + uint64_t(bootArg("piano-power-delay", 30)) * 1000;   // 3 minutes after the bring-up began
  stage(4, "DSP start requested, PMIC_RTR_ADSP_APPS registered");
}

void PianoPower::logMcaEvents()
{
  const uint32_t e = piano_mca_take_events(&fMca);
  if (!e) return;
  const PianoMca &m = fMca;
  if (e & McaEvAttach)
    note("mca: a source is attached: %d mV, type %s", m.value[McaBusVoltage] / 1000,
         piano_mca_has(&m, McaType) ? piano_mca_type_name(m.value[McaType]) : "?");
  if (e & McaEvDetach) note("mca: the source is gone: input limit and charge current back to the firmware's own values");
  if (e & McaEvHvAsked) note("mca: asking the PD source for its fixed 9 V PDO");
  if (e & McaEvHvOn) note("mca: the bus is at %d mV: 9 V on", m.value[McaBusVoltage] / 1000);
  if (e & McaEvHvBlocked) note("mca: 9 V not reached or lost: 5 V until the source is detached");
  if (e & McaEvRefused) note("mca: a write outside the allowed set was refused (%u so far)", m.refused);
  if (e & McaEvPanEnabled) note("mca: USB-C port notifications enabled: owner 0x800c answered the enable (opcode 0x15, cmd 0x10)");
  if (e & McaEvPanSilent) note("mca: no answer from owner 0x800c to the port enable in 5 s: the port owner is not served now");
  if (e & McaEvUcsiSilent) note("ucsi: no answer from owner 0x800b in 5 s");
  if (e & McaEvBoardDone) note("mca: board configuration taken (CP state, OTG configuration, CID): the policy follows");
  if (e & McaEvBoardGaveUp) note("mca: board configuration not taken after 5 cycles (sent %#x): the policy goes on without it", m.board_done);
  if (e & McaEvWrite) {
    const char *what = m.last_property == PIANO_MCA_ICL ? "input limit" : m.last_property == PIANO_MCA_FCC ? "charge current"
                       : m.last_property == PIANO_MCA_CHARGE_ENABLE ? "charge enable" : m.last_property == PIANO_MCA_PD_FIXED ? "PD fixed request"
                       : m.last_property == PIANO_MCA_INPUT_SUSPEND ? "input suspend" : m.last_property == PIANO_MCA_CP_STATE ? "CP state"
                       : m.last_property == PIANO_MCA_OTG_CFG ? "OTG configuration" : m.last_property == PIANO_MCA_CID_CFG ? "CID configuration"
                       : "property";
    note("mca: write %s (%#x) = %u: %s (%d)", what, m.last_property, m.last_value,
         m.last_result == 0 ? "ok" : m.last_result == -60 ? "no answer" : "refused by the firmware", m.last_result);
  }
}

void PianoPower::pumpMca(uint64_t now)
{
  uint8_t out[PIANO_MCA_REQUEST_BYTES];
  if (!fRequestDue) {
    const uint32_t n = piano_mca_next(&fMca, out);
    if (n) {
      if (piano_adsp_send(fChannel, out, n)) note("mca: send queue full");
      fRequestDue = now + 1000;
    }
  }
  // The port owner's writes get 5 s each, as Linux gives them (kernel.md §2.5): no answer to the enable means the owner is not served.
  if (!fPanDue) {
    const uint32_t n = piano_mca_pan_next(&fMca, out);
    if (n) {
      piano_adsp_send(fChannel, out, n);
      fPanDue = now + 5000;
    }
  }
  if (!fUcsiDue) {
    const uint32_t n = piano_mca_ucsi_next(&fMca, out);
    if (n) {
      note("ucsi: reading the buffer of owner 0x800b (read only)");
      piano_adsp_send(fChannel, out, n);
      fUcsiDue = now + 5000;
    }
  }
}

void PianoPower::serviceMca(uint64_t now)
{
  if (!fChannel) return;
  // The channel, then the gate: like every Linux pmic_glink client, nothing is sent until msm/adsp/charger_pd reports up on the DSP's
  // service registry (PianoQrtr.c); without any answer for 60 s after the channel came up, the codec starts anyway (logged). Each
  // new charger generation (charger_pd up again) links the codec afresh, so the board configuration, the port enable and the
  // policy are all sent again (the firmware is back on its defaults; Ubuntu #18 needed the same re-send to recover).
  if (__atomic_exchange_n(&fLinkChanged, 0u, __ATOMIC_ACQ_REL)) {
    const bool up = __atomic_load_n(&fLinkUp, __ATOMIC_ACQUIRE) != 0;
    if (up != fChannelUp) {
      note("mca: PMIC_RTR_ADSP_APPS %s", up ? "up: waiting for msm/adsp/charger_pd to report up" : "down");
      fChannelUp = up;
      fChannelSinceMs = now;
      fGateTimedOut = false;
    }
  }
  uint32_t ups = 0;
  const uint32_t pd = piano_qrtr_charger_pd(&ups);
  const bool pdUp = pd == SERVREG_STATE_UP;
  if (fChannelUp && !pdUp && !fGateTimedOut && now - fChannelSinceMs >= 60000) {
    fGateTimedOut = true;
    note("mca: no 'up' from msm/adsp/charger_pd 60 s after the channel (state %s): starting without it", servreg_state_name(pd));
  }
  const bool ready = fChannelUp && (pdUp || fGateTimedOut);
  if (ready && fWasUp && pdUp && ups != fPdUpsLinked) {
    note("mca: msm/adsp/charger_pd up again (generation %u): sending everything again", ups);
    piano_mca_link(&fMca, 0);
    fWasUp = false;
  }
  if (ready != fWasUp) {
    note("mca: %s", ready ? (pdUp ? "msm/adsp/charger_pd is up: reading the charger properties" : "reading the charger properties")
                          : "the charger is not reachable now: requests stop");
    piano_mca_link(&fMca, ready);
    fRequestDue = fPanDue = fUcsiDue = 0;
    fCycleDue = now + 5000;
    fWasUp = ready;
    fPdUpsLinked = ups;
  }
  for (;;) {
    uint8_t data[PIANO_ADSP_MAX_MESSAGE];
    uint32_t length = 0;
    IOSimpleLockLock(fRingLock);
    if (fRingCount) {
      length = fRing[fRingHead].length;
      memcpy(data, fRing[fRingHead].data, length);
      fRingHead = (fRingHead + 1) % kRing;
      fRingCount--;
    }
    IOSimpleLockUnlock(fRingLock);
    if (!length) break;
    const int r = piano_mca_receive(&fMca, data, length);
    if (r == McaAnswer || r == McaCycleDone || r == McaWritten) fRequestDue = 0;
    if (r == McaPanDone) fPanDue = 0;
    if (r == McaUcsiDone) {
      fUcsiDue = 0;
      note("ucsi: owner 0x800b answers: version %x.%x, CCI %#x, %u-byte buffer (UCSI %s), return code %u", fMca.ucsi_version >> 8,
           fMca.ucsi_version & 0xff, fMca.ucsi_cci, fMca.ucsi_bytes, fMca.ucsi_bytes > 48 ? "2.x" : "1.x", fMca.ucsi_ret);
    }
    if (r == McaUcsiNotified && fUcsiNotesLogged < 8) {
      fUcsiNotesLogged++;
      note("ucsi: notification from owner 0x800b (%u so far; nothing is sent back: we run no UCSI client)", fMca.ucsi_notifies);
    }
    if (r == McaPanNotified) note("mca: USB-C port notification (port %u): acknowledged", data[12]);
    if (r == McaCycleDone) {
      char line[520];
      if (fMca.cycles == 1) {
        mcaLine(line, sizeof line);
        note("first charger reading: %s", line);
      }
      // The input-path diagnostic after every cycle of the first minute (12 cycles of 5 s), then with each report.
      if (fMca.cycles <= 12) {
        diagLine(line, sizeof line);
        note("[%llus] %s", (nowMs() - fStartMs) / 1000, line);
        diagPortLine(line, sizeof line, false);
        note("[%llus] %s", (nowMs() - fStartMs) / 1000, line);
        diagPortLine(line, sizeof line, true);
        note("[%llus] %s", (nowMs() - fStartMs) / 1000, line);
      }
    }
  }
  if (!fWasUp) {stepQuiet(now); return;}
  if (fRequestDue && now >= fRequestDue) {piano_mca_timeout(&fMca); fRequestDue = 0;}
  if (fPanDue && now >= fPanDue) {piano_mca_pan_timeout(&fMca); fPanDue = 0;}
  if (fUcsiDue && now >= fUcsiDue) {piano_mca_ucsi_timeout(&fMca); fUcsiDue = 0;}
  if (now >= fCycleDue) {piano_mca_refresh(&fMca); fCycleDue = now + 5000;}
  pumpMca(now);
  logMcaEvents();
  stepQuiet(now);
}

// Clients (audio, sensors) hold their traffic while a charger generation is configured: from the codec's link until a whole cycle ends
// with nothing left to write and the port enable answered (or timed out), at most 60 s.
void PianoPower::stepQuiet(uint64_t now)
{
  const int e = power_quiet_step(&fQuiet, fWasUp, fMca.links, piano_mca_configured(&fMca), now, kQuietCapMs);
  if (e == PowerQuietNone) return;
  piano_adsp_quiet_set(PIANO_ADSP_QUIET_CHARGER, fQuiet.raised);
  note("quiet: %s", e == PowerQuietRaised ? "raised for the DSP's other clients: configuring the charger"
                    : e == PowerQuietReleased ? "released: the charger is configured"
                    : e == PowerQuietTimedOut ? "released after 60 s with the charger not configured"
                                              : "dropped: the charger is not reachable now");
}

void PianoPower::run()
{
  fStartMs = nowMs();
  if (!probeSpmi()) note("continuing without the SPMI view");
  surveyAdsp();
  probeGauges();
  if (gArgs.level >= 2) startCharger();
  report();
  uint64_t reportDue = nowMs() + uint64_t(gArgs.period) * 1000;
  stage(5, "reporting");
  for (;;) {
    uint64_t now = nowMs();
    serviceMca(now);
    if (now >= reportDue) {
      report();
      reportDue = now + uint64_t(gArgs.period) * 1000;
    }
    if (fReadyWarnDue && now >= fReadyWarnDue) {
      fReadyWarnDue = 0;
      if (fMca.icl_set == 0 && gArgs.write)
        note("WARNING: 3 minutes into the DSP bring-up no input limit write has taken: the charger may be held at the firmware's "
             "100 mA; reboot (or boot Ubuntu/Android) if the battery drains");
    }
    // Sleep until the next deadline or a message.
    now = nowMs();
    uint64_t next = reportDue;
    if (fChannel && fWasUp) {
      if (fCycleDue && fCycleDue < next) next = fCycleDue;
      if (fRequestDue && fRequestDue < next) next = fRequestDue;
      if (fPanDue && fPanDue < next) next = fPanDue;
    }
    if (fReadyWarnDue && fReadyWarnDue < next) next = fReadyWarnDue;
    const uint32_t wait = next > now ? uint32_t(next - now < 60000 ? next - now : 60000) : 1;
    assert_wait_timeout((event_t)this, THREAD_UNINT, wait, kMillisecondScale);
    bool pending = __atomic_load_n(&fLinkChanged, __ATOMIC_ACQUIRE) != 0;
    IOSimpleLockLock(fRingLock);
    pending |= fRingCount != 0;
    IOSimpleLockUnlock(fRingLock);
    if (pending) clear_wait(current_thread(), THREAD_AWAKENED);
    else thread_block(THREAD_CONTINUE_NULL);
  }
}
