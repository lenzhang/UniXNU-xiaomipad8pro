// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoUsbHost: USB host mode and the Type-C port of piano (SM8750: the DWC3 at usb@a600000 as an xHCI, the QMP USB3/DP combo PHY,
// the DIO4485 SBU/orientation switch, the PMIH010x Type-C block, the DSP's UCSI and port notifications) for macOS 27, native at EL1.
// Design, the macOS-side verdict, the ladder and the test plan: docs/drivers/usb-host.md. Facts and their sources: UsbHostHw.h.
// This is level 1 only, separate from PianoDwc3 (the device-mode debug console, which keeps the controller):
//   piano-usbhost=1  a read-only survey, logged as "PianoUsbHost: ..." lines: the Type-C state (PMIC Type-C block and the eUSB2
//                    repeater over the SPMI observer channel, the orientation line gpio61, the DIO4485's bus pins), the USB power
//                    domains and clocks, the DWC3 role (GCTL.PRTCAPDIR), PHY and glue words, the xHCI capability words (only with
//                    the core powered and clocked), the xHCI operational/port words and extended capabilities (only in host/OTG
//                    mode), the QMP PHY's Type-C mux and PCS words (only with its power domain and clocks on), and the role the
//                    policy of docs/drivers/usb-host.md section 6 would choose
//   piano-usbhost=2  the same, then a watch: the Type-C state, gpio61, the core's role and the PHY's mux re-read every
//                    piano-usbhost-period ms for piano-usbhost-watch s; a line for every change (plug, unplug, orientation)
// Nothing is written: every MMIO access is a load (UsbHostSurvey.h has no store); the only store is the SPMI observer channel's
// read command (package/PianoPower/SpmiObserver.h), as PianoPower's level 1. Not read: the DIO4485 (an I2C transaction needs the
// pins and the QUP engine set up), the DSP (UCSI and port notifications need PianoPower's level 2), the M31 PHY (PianoDwc3's probe
// reads it). Without piano-usbhost the driver declines in probe() and maps nothing.
// Other arguments: piano-usbhost-delay=S (wait first, default 0), piano-usbhost-xhci=0|1|2 (default 1; 2 walks the xHCI registers
// in device mode too), piano-usbhost-qmp=0 (skip the PHY), piano-usbhost-spmi=0|1 (default 1, 0 when piano-power is set: PianoPower
// reads the same Type-C block on its own schedule and one observer channel serves one reader at a time), piano-usbhost-watch=S
// (default 300), piano-usbhost-period=MS (default 1000, at least 100).
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "UsbHostSurvey.h"
#include "ChargerPmic.h"

using namespace pianousbhost;
using pianopower::Window;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_usbhost_stage = 0;
}

namespace {

struct Args {
  uint32_t level, delay, xhci, qmp, spmi, watch, period;
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
  gArgs.level = bootArg("piano-usbhost", 0);
  gArgs.delay = bootArg("piano-usbhost-delay", 0);
  if (gArgs.delay > 600) gArgs.delay = 600;
  gArgs.xhci = bootArg("piano-usbhost-xhci", 1);
  if (gArgs.xhci > 2) gArgs.xhci = 2;
  gArgs.qmp = bootArg("piano-usbhost-qmp", 1);
  gArgs.spmi = bootArg("piano-usbhost-spmi", bootArg("piano-power", 0) ? 0 : 1);
  gArgs.watch = bootArg("piano-usbhost-watch", 300);
  if (gArgs.watch > 3600) gArgs.watch = 3600;
  gArgs.period = bootArg("piano-usbhost-period", 1000);
  if (gArgs.period < 100) gArgs.period = 100;
  if (gArgs.period > 60000) gArgs.period = 60000;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoUsbHost: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_usbhost_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// Explicit ldr, as the other piano kexts; the str is the SPMI observer command only (ioWr below).
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}

// ------------------------------------------------------------------------------------------------ MMIO pages, mapped on demand
// The survey decides which addresses it loads (UsbHostSurvey.h, host-tested); a page is mapped the first time one of its words is
// loaded (PianoAudio's scheme).
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
  __asm__ volatile("str %w0, [%1]" ::"r"(value), "r"(windowBase(w) + offset) : "memory");
}
void ioDelay(void *, unsigned us) { IODelay(us); }
const pianopower::Io gIo = {nullptr, ioRd, ioWr, ioDelay};

struct Spmi {
  bool up = false;
  pianopower::Arbiter arbiter;
  pianopower::ChargerLayout charger;
  int repeaterApid = -1;
};
Spmi gSpmi;

bool spmiByte(int apid, uint8_t reg, uint8_t &value)
{
  return pianopower::spmiRead(gIo, apid, reg, &value, 1) == pianopower::ReadResult::ok;
}

bool spmiUp()
{
  using namespace pianopower;
  if (!mapRegion(gCore, kSpmiCore, kSpmiCoreBytes) || !mapRegion(gCnfg, kSpmiCnfg, kSpmiCnfgBytes) ||
      !mapRegion(gObsv, kSpmiObsv, kSpmiObsvBytes)) {
    note("spmi: the arbiter windows do not map");
    return false;
  }
  gSpmi.arbiter = spmiArbiter(gIo);
  if (!gSpmi.arbiter.v7) {
    note("spmi: arbiter version %#x, %u APIDs: not the v7 layout, not read", gSpmi.arbiter.version, gSpmi.arbiter.count);
    return false;
  }
  gSpmi.charger = discoverCharger(gIo, gSpmi.arbiter);
  if (!gSpmi.charger.found) {
    note("spmi: charger PMIC: %s", gSpmi.charger.why);
    return false;
  }
  gSpmi.repeaterApid = spmiFindApid(gIo, gSpmi.arbiter, uint16_t(pianohw::kRepeaterPpid)).apid;
  note("spmi: arbiter %#x, %u APIDs; PMIH010x model %#x, charger %#x (subtype %#x), Type-C block %#x (APID %d, owner EE %u); "
       "eUSB2 repeater APID %d",
       gSpmi.arbiter.version, gSpmi.arbiter.count, gSpmi.charger.model, gSpmi.charger.charger.address, gSpmi.charger.subtype,
       gSpmi.charger.typec.address, gSpmi.charger.typec.apid, gSpmi.charger.typec.owner, gSpmi.repeaterApid);
  return true;
}

Typec readTypec()
{
  Typec t;
  if (!gSpmi.up) return t;
  const int a = gSpmi.charger.typec.apid;
  uint8_t six[6] = {};
  if (pianopower::spmiRead(gIo, a, kTcSnkStatus, six, 6) != pianopower::ReadResult::ok) return t;
  t.snk = six[0];
  t.src = six[2];
  t.smState = six[3];
  t.smStatus = six[4];
  t.misc = six[5];
  t.ok = spmiByte(a, kTcLegacy, t.legacy) && spmiByte(a, kTcUusb, t.uusb) && spmiByte(a, kTcRtStatus, t.rt) &&
         spmiByte(a, kTcModeCfg, t.mode) && spmiByte(a, kTcVconn, t.vconn) && spmiByte(a, kTcCcout, t.ccout);
  return t;
}

// ------------------------------------------------------------------------------------------------------------- the report
void reportPins(const Pins &p)
{
  const PinWords *words[3] = {&p.orientation, &p.sda, &p.scl};
  const char *names[3] = {"orientation (pmic-glink orientation-gpios)", "DIO4485 SDA (i2c@a8c000)", "DIO4485 SCL (i2c@a8c000)"};
  const unsigned gpios[3] = {kGpioOrientation, kGpioMuxSda, kGpioMuxScl};
  for (unsigned i = 0; i < 3; ++i) {
    const PinDecode d = decodePin(words[i]->cfg, words[i]->io);
    note("pin gpio%u %s: cfg %08x io %08x: function %u pull %s %u mA output %s, in %u", gpios[i], names[i], words[i]->cfg,
         words[i]->io, d.function, pullName(d.pull), d.driveMa, d.outputEnable ? "enabled" : "disabled", d.in);
  }
}

void reportClocks(const Clocks &c)
{
  note("GCC usb30 gdsc %08x master %08x sleep %08x mock_utmi %08x cfg_noc_axi %08x aggre_axi %08x: core %s", c.usb30Gdsc, c.master,
       c.sleep, c.mockUtmi, c.cfgNoc, c.aggre, c.coreOn ? "powered and clocked" : "NOT powered/clocked (its registers are not loaded)");
  note("GCC usb3 phy gdsc %08x aux %08x com_aux %08x pipe %08x; TCSR clkref usb2 %08x usb3 %08x: QMP PHY %s", c.phyGdsc, c.aux,
       c.comAux, c.pipe, c.clkrefUsb2, c.clkrefUsb3, c.phyOn ? "powered and clocked" : "NOT powered/clocked (not loaded)");
}

void reportCore(const Core &k)
{
  if (!k.read) {
    note("DWC3: not read (the core is not powered/clocked)");
    return;
  }
  note("DWC3 %s GSNPSID %08x GVERSION %08x: GCTL %08x PRTCAPDIR %u = %s%s, GSTS %08x (current mode %s)", ipName(k.gsnpsid), k.gsnpsid,
       k.gversion, k.gctl, prtcap(k.gctl), prtcapName(prtcap(k.gctl)), (k.gctl & kGctlCoreSoftReset) ? ", core soft reset held" : "",
       k.gsts, (k.gsts & kGstsCurmodMask) == 1 ? "host" : (k.gsts & kGstsCurmodMask) == 0 ? "device" : "other");
  note("DWC3 GUSB2PHYCFG %08x (susphy %u enblslpm %u) GUSB3PIPECTL %08x (susphy %u ux_exit_px %u dis_rxdet_inp3 %u) GUCTL %08x "
       "GSBUSCFG0 %08x; device DCTL %08x DSTS %08x",
       k.gusb2phycfg, (k.gusb2phycfg & kGusb2PhySusphy) != 0, (k.gusb2phycfg & kGusb2PhyEnblslpm) != 0, k.gusb3pipectl,
       (k.gusb3pipectl & kGusb3PipeSusphy) != 0, (k.gusb3pipectl & kGusb3PipeUxExitPx) != 0,
       (k.gusb3pipectl & kGusb3PipeDisRxDetInP3) != 0, k.guctl, k.gsbuscfg0, k.dctl, k.dsts);
  note("QSCRATCH general %08x (UTMI clock as PIPE clock: %s) hs_phy %08x ss_phy %08x: VBUS/session override %u of 3 bits (%s)",
       k.qsGeneral, utmiAsPipe(k) ? "yes" : "no", k.qsHsPhy, k.qsSsPhy, vbusOverride(k),
       vbusOverride(k) == 3 ? "device role" : vbusOverride(k) == 0 ? "host role or untouched" : "mixed");
}

void reportXhci(const Xhci &x)
{
  if (!x.caps) {
    note("xHCI: not read (%s)", x.why);
    return;
  }
  note("xHCI caps %08x %08x %08x %08x %08x %08x %08x %08x: %s", x.cap[0], x.cap[1], x.cap[2], x.cap[3], x.cap[4], x.cap[5],
       x.cap[6], x.cap[7], x.sane ? "an xHCI" : "NOT an xHCI's (nothing followed)");
  if (!x.sane) return;
  note("xHCI version %x.%02x, caplength %#x, %u slots, %u interrupters, %u ports, AC64 %u CSZ %u PPC %u, xECP %#x, doorbells %#x, "
       "runtime %#x",
       x.hciVersion >> 8, x.hciVersion & 0xff, x.capLength, x.maxSlots, x.maxIntrs, x.maxPorts, x.ac64, x.csz, x.ppc, x.xecp * 4,
       x.dbOff, x.rtsOff);
  if (!x.walked) {
    note("xHCI: operational and port registers not read (%s)", x.why);
    return;
  }
  note("xHCI USBCMD %08x USBSTS %08x (%s%s%s) PAGESIZE %08x CONFIG %08x", x.usbCmd, x.usbSts, (x.usbSts & kUsbStsHalted) ? "halted" : "running",
       (x.usbSts & kUsbStsCnr) ? ", not ready" : "", (x.usbSts & kUsbStsHce) ? ", host controller error" : "", x.pageSize, x.config);
  char ids[160];
  size_t n = 0;
  ids[0] = 0;
  for (unsigned i = 0; i < x.xcapCount && n < sizeof ids; ++i)
    n += size_t(snprintf(ids + n, sizeof ids - n, "%s%u %s", i ? ", " : "", x.xcapIds[i], xcapName(x.xcapIds[i])));
  note("xHCI extended capabilities: %s%s%s", ids, *x.why ? "; " : "", x.why);
  for (unsigned i = 0; i < x.protocolCount; ++i) {
    const Protocol &p = x.protocols[i];
    note("xHCI protocol USB %x.%02x (name %08x): ports %u..%u", p.major, p.minor, p.name, p.portOffset,
         p.portCount ? p.portOffset + p.portCount - 1 : p.portOffset);
  }
  for (unsigned i = 0; i < x.portCount; ++i) {
    const uint32_t v = x.portsc[i];
    note("xHCI port %u PORTSC %08x: connected %u enabled %u power %u reset %u over-current %u, link state %u, speed %s", i + 1, v,
         (v & kPortCcs) != 0, (v & kPortPed) != 0, (v & kPortPp) != 0, (v & kPortPr) != 0, (v & kPortOca) != 0, portLinkState(v),
         speedName(portSpeed(v)));
  }
}

void reportQmp(const Qmp &q, const Clocks &c)
{
  if (!q.read) {
    note("QMP combo PHY: not read (%s)", gArgs.qmp ? (c.phyOn ? "?" : "its power domain or clocks are off") : "piano-usbhost-qmp=0");
    return;
  }
  note("QMP DP_COM mode %08x (%s) sw_reset %08x power_down %08x swi %08x typec %08x (%s, %s) typec_pwrdn %08x reset_ovrd %08x",
       q.phyMode, qmpModeName(q.phyMode), q.swReset, q.powerDown, q.swi, q.typec,
       (q.typec & kTypecPortSelectMux) ? "software lane select" : "lane select from the PHY's own input",
       orientationName(qmpOrientation(q.typec)), q.typecPwrdn, q.resetOvrd);
  note("QMP USB3 PCS sw_reset %08x status1 %08x (%s) power_down %08x start %08x; DP PHY pd_ctl %08x mode %08x", q.pcsSwReset,
       q.pcsStatus1, (q.pcsStatus1 & kPcsPhyStatus) ? "PHY not ready" : "PHY ready", q.pcsPowerDown, q.pcsStart, q.dpPdCtl, q.dpMode);
}

void reportTypec(const Typec &t, const TypecView &v)
{
  if (!t.ok) {
    note("Type-C: not read (%s)", gArgs.spmi ? "the SPMI read failed" : "piano-usbhost-spmi=0");
    return;
  }
  note("Type-C snk %02x src %02x sm_state %02x sm_status %02x misc %02x legacy %02x u_usb %02x rt %02x mode %02x (%s) vconn %02x ccout %02x",
       t.snk, t.src, t.smState, t.smStatus, t.misc, t.legacy, t.uusb, t.rt, t.mode, portModeName(t.mode), t.vconn, t.ccout);
  if (!v.attached) {
    note("Type-C: nothing attached%s%s", v.vbus ? " (VBUS detected)" : "", v.water ? ", water detected" : "");
    return;
  }
  note("Type-C: attached on CC%u, %s: %s%s; VBUS %s%s%s", v.cc, v.sourceMode ? "we are the source" : "we are the sink",
       partnerName(v.partner), v.rpMa ? (v.rpMa == 3000 ? ", Rp 3.0 A" : v.rpMa == 1500 ? ", Rp 1.5 A" : ", Rp default USB") : "",
       v.vbus ? "detected" : "absent", (t.smStatus & kSmVsafe5v) ? " (vSafe5V)" : "", v.vbusError ? ", VBUS error" : "");
}

void reportRepeater()
{
  if (!gSpmi.up || gSpmi.repeaterApid < 0) {
    note("eUSB2 repeater: not read");
    return;
  }
  uint8_t st = 0, en = 0, fe = 0, fv = 0, tune[4] = {};
  const int a = gSpmi.repeaterApid;
  bool ok = spmiByte(a, kRepStatus, st) && spmiByte(a, kRepEnable, en) && spmiByte(a, kRepForceEn5, fe) && spmiByte(a, kRepForceVal5, fv);
  for (unsigned i = 0; i < 4 && ok; ++i) ok = spmiByte(a, kRepTune[i], tune[i]);
  if (!ok) {
    note("eUSB2 repeater: a read failed");
    return;
  }
  note("eUSB2 repeater status %02x enable %02x force_en_5 %02x force_val_5 %02x (%s), tune [51] %02x [54] %02x [56] %02x [57] %02x",
       st, en, fe, fv, ((fe & kRepForce19p2) && (fv & kRepForce19p2)) ? "host: 19.2 MHz forced" : "device setting", tune[0], tune[1],
       tune[2], tune[3]);
}

// One compact line: the summary property and the watch's comparison.
void summary(const TypecView &v, bool typecOk, const Pins &p, const Clocks &c, const Core &k, const Qmp &q, const RoleDecision &r,
             char *line, size_t size)
{
  const Orientation o = orientation(v, p, q);
  char tc[96];
  if (!typecOk) snprintf(tc, sizeof tc, "typec unread");
  else if (!v.attached) snprintf(tc, sizeof tc, "typec detached vbus %u", v.vbus);
  else
    snprintf(tc, sizeof tc, "typec cc%u %s %s vbus %u", v.cc, v.sourceMode ? "source" : "sink",
             v.partner == Partner::source ? "rp" : v.partner == Partner::usbDevice ? "rd" : v.partner == Partner::audio ? "audio" : "other",
             v.vbus);
  snprintf(line, size, "%s | gpio61 %u | core %s%s%s | phy %s%s | orientation %s%s | role %s", tc,
           decodePin(p.orientation.cfg, p.orientation.io).in, c.coreOn ? "on " : "off", c.coreOn && k.read ? prtcapName(prtcap(k.gctl)) : "",
           c.coreOn && k.read ? (vbusOverride(k) == 3 ? " override" : "") : "", c.phyOn ? "on " : "off",
           q.read ? qmpModeName(q.phyMode) : "", o.known ? orientationName(o.pmic ? o.pmic : o.gpio ? o.gpio : o.qmp) : "-",
           o.known && !o.agree ? " (witnesses DISAGREE)" : "", roleName(r.role));
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoUsbHost : public IOService {
  OSDeclareDefaultStructors(PianoUsbHost)
  void run();
  static void thread(void *self, wait_result_t);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoUsbHost, IOService)

IOService *PianoUsbHost::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoUsbHost::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-usbhost=%u delay=%u xhci=%u qmp=%u spmi=%u watch=%u period=%u (DWC3 %#llx, QMP %#llx, orientation gpio%u)",
       gArgs.level, gArgs.delay, gArgs.xhci, gArgs.qmp, gArgs.spmi, gArgs.watch, gArgs.period, (unsigned long long)kXhci,
       (unsigned long long)kQmp, kGpioOrientation);
  if (gArgs.level > 2) note("piano-usbhost=%u: only levels 1 and 2 exist (docs/drivers/usb-host.md section 8); running 2", gArgs.level);
  note("level %u: loads only, nothing written (the SPMI observer's read command is the only store)", gArgs.level > 1 ? 2u : 1u);
  setProperty("piano-usbhost", gArgs.level, 32);
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoUsbHost::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;
  }
  thread_deallocate(t);
  return true;
}

void PianoUsbHost::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-usbhost");
  static_cast<PianoUsbHost *>(self)->run();
}

void PianoUsbHost::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  stage(1, "Type-C: the PMIC's Type-C block and the eUSB2 repeater over SPMI (read only)");
  if (gArgs.spmi) gSpmi.up = spmiUp();
  else note("spmi: skipped (piano-usbhost-spmi=0%s)", bootArg("piano-power", 0) ? ", the default with piano-power set" : "");
  Typec t = readTypec();
  TypecView v = decodeTypec(t);
  reportTypec(t, v);
  reportRepeater();

  stage(2, "TLMM: the orientation line and the DIO4485's bus pins (read only)");
  Pins p = surveyPins(load);
  reportPins(p);

  stage(3, "GCC: the USB power domains and clocks (read only)");
  Clocks c = surveyClocks(load);
  reportClocks(c);

  stage(4, "DWC3: role, PHY and glue words; xHCI capability words (only if powered and clocked)");
  Core k = surveyCore(c, load);
  reportCore(k);
  const Xhci x = surveyXhci(c, k, gArgs.xhci, load);
  reportXhci(x);

  stage(5, "QMP combo PHY: the Type-C mux and PCS words (only if powered and clocked)");
  Qmp q = surveyQmp(c, gArgs.qmp != 0, load);
  reportQmp(q, c);

  const Orientation o = orientation(v, p, q);
  note("orientation: PMIC %s, gpio61 %s, PHY %s: %s", orientationName(o.pmic), orientationName(o.gpio), orientationName(o.qmp),
       !o.known ? "no cable (nothing to compare)" : o.agree ? "the witnesses agree" : "the witnesses DISAGREE");
  RoleInput in;
  in.typecValid = t.ok;
  in.typec = v;
  const RoleDecision r = decideRole(in);
  note("role: the policy would choose %s: %s (level 1 changes nothing)", roleName(r.role), r.why);
  char line[240];
  summary(v, t.ok, p, c, k, q, r, line, sizeof line);
  setProperty("piano-usbhost-survey", line);
  note("summary: %s", line);

  if (gArgs.level >= 2 && gArgs.watch) {
    stage(6, "watch: the port, the orientation line, the core's role and the PHY's mux, re-read (read only)");
    const uint64_t start = nowMs(), end = start + uint64_t(gArgs.watch) * 1000;
    unsigned changes = 0, samples = 0;
    char last[240];
    strlcpy(last, line, sizeof last);
    while (nowMs() < end) {
      IOSleep(gArgs.period);
      t = readTypec();
      v = decodeTypec(t);
      p = surveyPins(load);
      c = surveyClocks(load);
      k = surveyCore(c, load);
      q = surveyQmp(c, gArgs.qmp != 0, load);
      in.typecValid = t.ok;
      in.typec = v;
      summary(v, t.ok, p, c, k, q, decideRole(in), line, sizeof line);
      ++samples;
      if (strcmp(line, last) != 0) {
        ++changes;
        const uint64_t at = nowMs() - start;
        note("change +%llu.%01llu s: %s", (unsigned long long)(at / 1000), (unsigned long long)(at % 1000 / 100), line);
        if (t.ok) note("change: Type-C snk %02x src %02x misc %02x sm_status %02x mode %02x", t.snk, t.src, t.misc, t.smStatus, t.mode);
        strlcpy(last, line, sizeof last);
      }
    }
    note("watch done: %u samples, %u changes in %u s", samples, changes, gArgs.watch);
  }
  stage(7, "done");
  note("done: %llu MMIO loads, 0 MMIO stores; %u pages mapped, %u map failures", (unsigned long long)gLoads, gPageCount, gMapFailures);
  unmapAll();
  unmapRegion(gCore);
  unmapRegion(gCnfg);
  unmapRegion(gObsv);
}
