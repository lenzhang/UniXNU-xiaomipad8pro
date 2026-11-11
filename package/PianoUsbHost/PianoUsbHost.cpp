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
// Levels 1 and 2 write nothing: every MMIO access is a load (UsbHostSurvey.h has no store); the only store is the SPMI observer
// channel's read command (package/PianoPower/SpmiObserver.h), as PianoPower's level 1. Not read: the DIO4485 (an I2C transaction
// needs the pins and the QUP engine set up), the DSP (UCSI and port notifications need PianoPower's level 2), the M31 PHY
// (PianoDwc3's probe reads it). Without piano-usbhost the driver declines in probe() and maps nothing.
//   piano-usbhost=5  (H5) the survey, then host mode with our own minimal xHCI probe (XhciHost.h, host-tested): the SMMU stream
//                    0x40 through a bypass bank (PianoDwc3's SmmuStream.h; in start(), on the matching thread), the DWC3 switched
//                    to host (the device controller stopped, the VBUS/session override cleared and the UTMI clock selected as the
//                    PIPE clock only with the glue clocks re-read on, DCTL.CSFTRST, PRTCAPDIR host), halt + HCRST, a command ring and
//                    an event ring in a non-cacheable DMA region, a No-Op command (completion code 1, its TRB's address), the root
//                    ports' power and connection, optionally a USB 2 port reset; a port watch; then halt + HCRST, and with
//                    piano-usbhost-back=S back to device mode after S s. Polled: no interrupt is enabled.
//   piano-usbhost=6  (H6 preparation, path A1) the same switch (and by default the No-Op probe first), then Apple's own AppleUSBXHCI
//                    on our PianoUsbHostNub (QcomDwc3's nub: usb-host0, the core's first 32 KiB, bus-number 0, interrupt source 0
//                    over SPI 133 or polled) through the codeless personality of tooling/k27/kext/PianoUsbHostStack.json, with the
//                    stream 0x40 identity-mapped write-back (SmmuIdentity.h) for coherency; the host stack's resource gate
//                    PianoUsbHostStack published first (the stack is linked only with PIANO_USBHOST_STACK=1, and its
//                    IOResources personalities wait for that gate). No subclass, no vtable fixture (A2 is the owner's decision).
// Levels 5 and 6 refuse while PianoDwc3 owns the controller (piano-usb >= 2), except level 5 with piano-usbhost-back=S and a
// piano-usb-delay that starts PianoDwc3's takeover at least 30 s after the host session ends (a hand-off by time, no PianoDwc3 code).
// Other arguments: piano-usbhost-delay=S (wait first, default 0), piano-usbhost-xhci=0|1|2 (default 1; 2 walks the xHCI registers
// in device mode too), piano-usbhost-qmp=0 (skip the PHY), piano-usbhost-spmi=0|1 (default 1, 0 when piano-power is set: PianoPower
// reads the same Type-C block on its own schedule and one observer channel serves one reader at a time), piano-usbhost-watch=S
// (default 300; at levels 5/6 the host session's port watch), piano-usbhost-period=MS (default 1000, at least 100); levels 5/6:
// piano-usbhost-back=S (0: stay in host mode), -csr=0|1|2 (GCTL soft reset: never, always, Linux's rule; default 2), -utmi=0|1
// (default 1), -reset=0|1 (a connected USB 2 port reset, default 0), -smmu=0|1|2 (none, bypass, identity; default 1 at level 5,
// 2 at level 6), -slots=N (default 8); level 6: -attach=0|1|2 (nub only, the personality, a direct allocation; default 1),
// -irq=0|1 (polled, SPI 133; default 1), -probe=0|1 (our No-Op probe before Apple's driver, default 1).
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "UsbHostSurvey.h"
#include "XhciHost.h"
#include "SmmuIdentity.h"
#include "SmmuStream.h"
#include "ChargerPmic.h"
#include "../common/PianoIrq.h"

using namespace pianousbhost;
using pianopower::Window;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_usbhost_stage = 0;
}

namespace {

struct Args {
  uint32_t level, delay, xhci, qmp, spmi, watch, period;
  uint32_t back, csr, utmi, reset, smmu, slots, attach, irq, probe;   // levels 5 and 6
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
  gArgs.back = bootArg("piano-usbhost-back", 0);
  if (gArgs.back > 3600) gArgs.back = 3600;
  gArgs.csr = bootArg("piano-usbhost-csr", 2);
  if (gArgs.csr > 2) gArgs.csr = 2;
  gArgs.utmi = bootArg("piano-usbhost-utmi", 1) != 0;
  gArgs.reset = bootArg("piano-usbhost-reset", 0) != 0;
  gArgs.smmu = bootArg("piano-usbhost-smmu", gArgs.level == 6 ? 2 : 1);
  if (gArgs.smmu > 2) gArgs.smmu = 2;
  gArgs.slots = bootArg("piano-usbhost-slots", 8);
  if (gArgs.slots < 1) gArgs.slots = 1;
  if (gArgs.slots > 255) gArgs.slots = 255;
  gArgs.attach = bootArg("piano-usbhost-attach", 1);
  if (gArgs.attach > 2) gArgs.attach = 1;
  gArgs.irq = bootArg("piano-usbhost-irq", 1) != 0;
  gArgs.probe = bootArg("piano-usbhost-probe", 1) != 0;
}
// The level that runs: 1 and 2 (read only), 5 (our xHCI probe), 6 (Apple's xHCI on our nub); 3 and 4 (the DSP's VBUS and UCSI,
// PianoPower's) and anything else run as 2.
uint32_t runLevel(uint32_t level) { return level == 1 || level == 5 || level == 6 ? level : 2; }

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

// Explicit ldr/str, as the other piano kexts (a store's value always in a register, never the zero register: Qualcomm's trapped
// MMIO). At levels 1 and 2 the only str is the SPMI observer command (ioWr below); levels 5 and 6 store through store().
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }
inline void dsb() { __asm__ volatile("dsb sy" ::: "memory"); }

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
uint64_t gLoads, gStores;

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
// Levels 5 and 6 only (XhciHost.h decides what and when), each store followed by a dsb.
void store(uint64_t pa, uint32_t v)
{
  const uintptr_t va = pageVa(pa);
  if (!va) {
    ++gMapFailures;
    return;
  }
  ++gStores;
  wr32(va + uintptr_t(pa & 0xfff), v);
  dsb();
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

// ---------------------------------------------------------------------------------------------- levels 5 and 6: the hardware
// The DMA region of our own xHCI probe: physically contiguous, 64 KiB aligned, below 4 GiB when possible (then every high half is
// 0 and the 64-bit write order cannot matter), else below 2^36. The core reaches it without cache coherence (a bypass bank): the
// CPU uses a second, write-combined (Normal non-cacheable) mapping only, after the cacheable alias is cleaned out of the caches
// (PianoDwc3's and PianoWifi's pattern). Kept for the life of the kernel once the controller has seen it.
struct Dma {
  IOBufferMemoryDescriptor *buffer;
  IOMemoryMap *map;
  uint8_t *va;
  uint64_t bus;
  uint32_t bytes;
  bool below4g;
};
Dma gDma;

void cleanToPoc(const void *start, size_t length)
{
  uint64_t ctr = 0;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t line = uintptr_t(4) << ((ctr >> 16) & 15);
  for (uintptr_t a = uintptr_t(start) & ~(line - 1); a < uintptr_t(start) + length; a += line) __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
  dsb();
}

bool dmaAlloc(uint32_t bytes)
{
  if (gDma.va) return gDma.bytes >= bytes;
  const uint64_t masks[2] = {0x00000000ffff0000ull, 0x0000000fffff0000ull};
  for (unsigned i = 0; i < 2 && !gDma.va; ++i) {
    IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), bytes, masks[i]);
    if (!b) continue;
    if (b->prepare() != kIOReturnSuccess) {
      b->release();
      continue;
    }
    IOByteCount contiguous = 0;
    const uint64_t pa = b->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
    uint8_t *cached = static_cast<uint8_t *>(b->getBytesNoCopy());
    if (!pa || (pa & (kRegionAlign - 1)) || contiguous < bytes || !cached) {
      b->complete();
      b->release();
      continue;
    }
    bzero(cached, bytes);
    cleanToPoc(cached, bytes);
    IOMemoryMap *m = b->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
    if (!m) {
      b->complete();
      b->release();
      continue;
    }
    gDma = {b, m, reinterpret_cast<uint8_t *>(m->getVirtualAddress()), pa, bytes, i == 0};
  }
  return gDma.va != nullptr;
}

// XhciHost.h's Hal: MMIO through the on-demand pages (load/store), the DMA region through its write-combined mapping.
struct KernelHal {
  uint32_t rd(uint64_t pa) { return load(pa); }
  void wr(uint64_t pa, uint32_t v) { store(pa, v); }
  void delayUs(uint32_t us) { IODelay(us); }
  void sleepMs(uint32_t ms) { IOSleep(ms); }
  // The USB30 GDSC and the four branch clocks of the core and its glue, re-read now (the QSCRATCH store gate, K:7dffd78b342d).
  bool glueClocked()
  {
    using namespace pianohw;
    return gdscOn(load(kGcc + kGccUsb30Gdsc)) && branchOn(load(kGcc + kGccUsb30Master)) && branchOn(load(kGcc + kGccUsb30Sleep)) &&
           branchOn(load(kGcc + kGccCfgNocUsb3Axi)) && branchOn(load(kGcc + kGccAggreUsb3Axi));
  }
  uint32_t memRd(uint32_t off) { return off + 4 <= gDma.bytes ? rd32(uintptr_t(gDma.va) + off) : 0; }
  void memWr(uint32_t off, uint32_t v)
  {
    if (off + 4 <= gDma.bytes) wr32(uintptr_t(gDma.va) + off, v);
  }
  void memBarrier() { dsb(); }
  uint64_t dmaBus() const { return gDma.bus; }
};
KernelHal gHal;

// The SMMU stream 0x40: 1 a bypass bank (PianoDwc3's SmmuStream.h: fine for our non-cacheable rings), 2 an identity-mapped stage 1
// bank, write-back (SmmuIdentity.h: coherent, for Apple's xHCI). In start(), on the matching thread, as PianoDwc3 does (the
// stream-match registers are not touched later from another thread).
Region gSmmu;
IOBufferMemoryDescriptor *gSmmuTable;
uint32_t smmuRd(uint32_t off) { return rd32(gSmmu.va + off); }
void smmuWr(uint32_t off, uint32_t v)
{
  wr32(gSmmu.va + off, v);
  dsb();
}
bool smmuSetup(uint32_t mode)
{
  using namespace pianohw;
  if (!mapRegion(gSmmu, kSmmu, kSmmuBytes)) {
    note("smmu: the apps SMMU does not map: stream %#x left alone", kUsbStream);
    return false;
  }
  const uint64_t deadline = nowMs() + 100;
  const auto expired = [deadline] { return nowMs() >= deadline; };
  if (mode == 1) {
    const pianousb::SmmuPlan p = pianousb::planSmmuStream(kUsbStream, kSmmuBytes, smmuRd);
    const char *failure = pianousb::applySmmuStream(p, kUsbStream, smmuRd, smmuWr, expired);
    note("smmu: stream %#x: %s (groups %u banks %u; slot %u bank %u%s; smr %08x s2cr %08x)", kUsbStream,
         failure ? failure : p.keep ? "kept: it already bypasses" : "through a bypass bank (non-coherent: our rings are non-cacheable)",
         p.groups, p.banks, p.slot, p.bank, p.reuse ? ", the existing entry" : "", p.slot < 128 ? smmuRd(0x800 + 4 * p.slot) : 0,
         p.slot < 128 ? smmuRd(0xc00 + 4 * p.slot) : 0);
    return failure == nullptr;
  }
  gSmmuTable = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), 4096, 0x000000fffffff000ull);
  if (!gSmmuTable || gSmmuTable->prepare() != kIOReturnSuccess) {
    note("smmu: no page for the identity table: stream %#x left alone", kUsbStream);
    return false;
  }
  uint64_t *table = static_cast<uint64_t *>(gSmmuTable->getBytesNoCopy());
  const uint64_t tablePa = gSmmuTable->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
  bzero(table, 4096);
  identityTable([table](unsigned i, uint64_t d) { table[i] = d; });
  cleanToPoc(table, 4096);
  const IdentityPlan p = planSmmuIdentity(kUsbStream, kSmmuBytes, smmuRd);
  const char *failure = applySmmuIdentity(p, kUsbStream, tablePa, smmuRd, smmuWr, expired);
  const uint32_t cb = (p.pages + p.bank) * 4096;
  note("smmu: stream %#x: %s (groups %u banks %u; slot %u bank %u%s; table %#llx; sctlr %08x fsr %08x)", kUsbStream,
       failure ? failure : "identity map of DRAM from 2 GiB, write-back inner shareable (coherent)", p.groups, p.banks, p.slot, p.bank,
       p.reuse ? ", the existing entry redirected" : "", (unsigned long long)tablePa, p.bank < 128 ? smmuRd(cb + kCbSctlr) : 0,
       p.bank < 128 ? smmuRd(cb + kCbFsr) : 0);
  return failure == nullptr;
}

// ------------------------------------------------------------------------------------------------- levels 5 and 6: the steps
HostPolicy policy()
{
  HostPolicy p;
  p.coreSoftReset = gArgs.csr;
  p.utmiAsPipe = gArgs.utmi != 0;
  return p;
}

bool hostSwitchLogged(SwitchResult &sw)
{
  stage(10, "host switch: the device controller stopped, the glue (override off, UTMI as PIPE), soft reset, PRTCAPDIR host");
  sw = hostSwitch(gHal, policy());
  const CoreWords &f = sw.found, &n = sw.now;
  note("host switch: %s GSNPSID %08x GVERSION %08x; found GCTL %08x (PRTCAPDIR %u) DCTL %08x (run/stop %u, halted %u in %u us) GEVNTSIZ "
       "%08x, override %u of 3, UTMI as PIPE %u; DCTL.CSFTRST %s in %u us, GCTL soft reset %s",
       ipName(sw.gsnpsid), sw.gsnpsid, sw.gversion, f.gctl, prtcap(f.gctl), f.dctl, sw.wasRunning, sw.devHalted, sw.haltUs, f.gevntsiz,
       unsigned((f.qsHs & pianohw::kQsUtmiOtgVbusValid) != 0) + unsigned((f.qsHs & pianohw::kQsSwSessvldSel) != 0) +
           unsigned((f.qsSs & pianohw::kQsLane0PwrPresent) != 0),
       unsigned(utmiAsPipeBits(f.qsGeneral)), sw.csftrst ? "cleared" : "-", sw.csftrstUs, sw.gctlReset ? "pulsed 100 ms" : "not needed");
  note("host switch: now GCTL %08x (PRTCAPDIR %u) GSTS %08x (CURMOD %u after %u us) GUSB2PHYCFG %08x GUSB3PIPECTL %08x GUCTL %08x; "
       "QSCRATCH general %08x hs %08x ss %08x; %u stores (%u QSCRATCH, each after the clocks read on): %s",
       n.gctl, prtcap(n.gctl), n.gsts, n.gsts & 3u, sw.curmodUs, n.gusb2, n.gusb3, n.guctl, n.qsGeneral, n.qsHs, n.qsSs, sw.stores,
       sw.glueStores, sw.ok ? "host mode" : sw.why);
  if (!sw.ok) stage(-10, sw.why);
  return sw.ok;
}

struct Probe {
  Xhci x;
  Rings rings;
  bool running = false, noopOk = false;
};

// Our own xHCI probe up to a running controller and the No-Op command (stages 11..17). False: stopped at a stage (logged).
bool xhciProbe(Probe &pr)
{
  stage(11, "xHCI capabilities");
  pr.x = readCaps(gHal);
  const Xhci live = surveyXhci(surveyClocks(load), surveyCore(surveyClocks(load), load), 1, load);   // the walk: protocols per port
  pr.x.protocolCount = live.protocolCount;
  for (unsigned i = 0; i < live.protocolCount; ++i) pr.x.protocols[i] = live.protocols[i];
  RingLayout l = ringLayout(pr.x, gArgs.slots);
  note("xHCI version %x.%02x, caplength %#x, %u slots (%u used), %u ports, %u scratchpad buffers, ERST max %u, AC64 %u CSZ %u PPC %u; "
       "doorbells %#x runtime %#x; protocols %u; region %#x bytes%s%s",
       pr.x.hciVersion >> 8, pr.x.hciVersion & 0xff, pr.x.capLength, pr.x.maxSlots, l.slots, pr.x.maxPorts, hcsScratchpads(pr.x.cap[2]),
       hcsErstMax(pr.x.cap[2]), pr.x.ac64, pr.x.csz, pr.x.ppc, pr.x.dbOff, pr.x.rtsOff, pr.x.protocolCount, l.bytes, l.error ? ": " : "",
       l.error ? l.error : "");
  if (l.error) {
    stage(-11, l.error);
    return false;
  }
  stage(12, "DMA region");
  if (!dmaAlloc(l.bytes)) {
    stage(-12, "no 64 KiB aligned contiguous DMA region");
    return false;
  }
  note("dma: %#x bytes at %#llx (%s), write-combined CPU mapping %p", gDma.bytes, (unsigned long long)gDma.bus,
       gDma.below4g ? "below 4 GiB: the 64-bit halves' order cannot matter" : "above 4 GiB: high halves first", gDma.va);
  pr.rings.l = l;
  pr.rings.bus = gDma.bus;
  stage(13, "xHCI halt and HCRST");
  const ResetResult rs = xhciReset(gHal, pr.x);
  note("xHCI reset: USBCMD %08x USBSTS %08x (was running %u; CNR %u us, halt %u us, HCRST + CNR %u us): %s", rs.usbcmd, rs.usbsts,
       rs.wasRunning, rs.cnrUs, rs.haltUs, rs.resetUs, rs.ok ? "ok" : rs.why);
  if (!rs.ok) {
    stage(-13, rs.why);
    return false;
  }
  stage(14, "xHCI rings: DCBAA, command ring, event ring, ERST, scratchpads");
  const InitResult in = xhciInit(gHal, pr.x, pr.rings);
  note("xHCI rings: DCBAAP %#llx CRCR %#llx ERSTBA %#llx ERDP %#llx CONFIG %08x PAGESIZE %08x USBSTS %08x: %s",
       (unsigned long long)in.dcbaap, (unsigned long long)in.crcr, (unsigned long long)in.erstba, (unsigned long long)in.erdp, in.config,
       in.pageSize, in.usbsts, in.ok ? "read back" : in.why);
  if (!in.ok) {
    stage(-14, in.why);
    return false;
  }
  stage(15, "root port power");
  const PortsResult ports = portsUp(gHal, pr.x);
  note("ports: %u, %u switched on (PPC %u)", ports.count, ports.powered, pr.x.ppc);
  stage(16, "run (RS 1, polled: no interrupt enabled)");
  const RunResult run = xhciRun(gHal, pr.x);
  note("xHCI run: USBCMD %08x USBSTS %08x after %u us: %s", run.usbcmd, run.usbsts, run.waitedUs, run.ok ? "running" : run.why);
  if (!run.ok) {
    stage(-16, run.why);
    return false;
  }
  pr.running = true;
  stage(17, "No-Op command");
  const NoopResult n = noop(gHal, pr.x, pr.rings);
  note("No-Op: event %08x %08x %08x %08x: type %u code %u for TRB %#llx (expected %#llx) after %u us, %u other events (ports %#x), "
       "USBSTS %08x: %s",
       n.completion.d[0], n.completion.d[1], n.completion.d[2], n.completion.d[3], n.completion.type(), n.completion.code(),
       (unsigned long long)n.completion.pointer(), (unsigned long long)n.trb, n.waitedUs, n.other, n.portEvents, n.usbsts,
       n.ok ? "PASS" : n.why);
  if (!n.ok) stage(-17, n.why);
  pr.noopOk = n.ok;
  return n.ok;
}

void portLine(const Xhci &x, unsigned port, uint32_t v, const char *what)
{
  note("port %u (USB %u)%s%s PORTSC %08x: connected %u enabled %u power %u reset %u over-current %u, link state %u, speed %s, changes %03x",
       port, portMajor(x, port), *what ? " " : "", what, v, (v & kPortCcs) != 0, (v & kPortPed) != 0, (v & kPortPp) != 0, (v & kPortPr) != 0,
       (v & kPortOca) != 0, portLinkState(v), speedName(portSpeed(v)), (v & kPortChangeBits) >> 17);
}

// The root ports: each one's state; a connected USB 2 port reset (piano-usbhost-reset=1); the change bits acknowledged.
void portsReport(Probe &pr, char *summaryOut, size_t size)
{
  stage(18, "root ports: connection (and, with piano-usbhost-reset=1, a USB 2 port reset)");
  size_t used = 0;
  summaryOut[0] = 0;
  const unsigned count = pr.x.maxPorts < 16 ? pr.x.maxPorts : 16;
  for (unsigned i = 1; i <= count; ++i) {
    const uint32_t v = gHal.rd(portscAt(pr.x, i));
    portLine(pr.x, i, v, "");
    const char *state = (v & kPortCcs) ? "connected" : "-";
    if ((v & kPortCcs) && gArgs.reset && portMajor(pr.x, i) == 2) {
      const PortResetResult r = portReset(gHal, pr.x, i);
      note("port %u: reset: PORTSC %08x -> %08x in %u ms: %s%s%s", i, r.before, r.after, r.waitedMs, r.ok ? "enabled, " : r.why,
           r.ok ? speedName(portSpeed(r.after)) : "", r.ok ? " speed" : "");
      state = r.ok ? speedName(portSpeed(r.after)) : "reset failed";
    } else {
      portAck(gHal, pr.x, i, v);
    }
    if (used < size) used += size_t(snprintf(summaryOut + used, size - used, "%s%u:%s", i > 1 ? " " : "", i, state));
  }
  uint32_t ports = 0;
  const unsigned events = drainEvents(gHal, pr.x, pr.rings, &ports);
  if (events) note("events: %u more (port status changes for ports %#x), acknowledged", events, ports);
}

// The host session's watch: PORTSC re-read every period, a line per change; events drained.
void portWatch(Probe &pr, uint32_t seconds)
{
  if (!seconds) return;
  stage(19, "port watch (plug and unplug now)");
  const unsigned count = pr.x.maxPorts < 16 ? pr.x.maxPorts : 16;
  uint32_t last[16] = {};
  for (unsigned i = 1; i <= count; ++i) last[i - 1] = gHal.rd(portscAt(pr.x, i)) & ~kPortChangeBits;
  const uint64_t start = nowMs(), end = start + uint64_t(seconds) * 1000;
  unsigned changes = 0;
  while (nowMs() < end) {
    IOSleep(gArgs.period);
    uint32_t ports = 0;
    drainEvents(gHal, pr.x, pr.rings, &ports);
    for (unsigned i = 1; i <= count; ++i) {
      const uint32_t v = gHal.rd(portscAt(pr.x, i));
      if ((v & ~kPortChangeBits) == last[i - 1] && !(v & kPortChangeBits)) continue;
      const uint64_t at = nowMs() - start;
      char what[32];
      snprintf(what, sizeof what, "+%llu.%01llu s:", (unsigned long long)(at / 1000), (unsigned long long)(at % 1000 / 100));
      if ((v & ~kPortChangeBits) != last[i - 1]) {
        ++changes;
        portLine(pr.x, i, v, what);
      }
      last[i - 1] = v & ~kPortChangeBits;
      if ((v & kPortCcs) && !(v & kPortPed) && (v & kPortCsc) && gArgs.reset && portMajor(pr.x, i) == 2) {
        const PortResetResult r = portReset(gHal, pr.x, i);
        note("port %u: reset: PORTSC %08x -> %08x in %u ms: %s%s", i, r.before, r.after, r.waitedMs, r.ok ? "enabled, " : r.why,
             r.ok ? speedName(portSpeed(r.after)) : "");
        last[i - 1] = gHal.rd(portscAt(pr.x, i)) & ~kPortChangeBits;
      } else {
        portAck(gHal, pr.x, i, v);
      }
    }
  }
  note("port watch done: %u changes in %u s; USBSTS %08x", changes, seconds, gHal.rd(xhciAt(pr.x).op + kOpUsbSts));
}

// The end of a host session: the controller halted and reset (it forgets the rings); with piano-usbhost-back, device mode.
void hostEnd(Probe *pr, const SwitchResult &sw, bool back)
{
  if (pr && pr->running) {
    const StopResult st = xhciStop(gHal, pr->x);
    note("xHCI stop: halted and reset: USBSTS %08x: %s", st.usbsts, st.ok ? "ok" : st.why);
    pr->running = false;
  }
  if (!back) {
    note("host mode kept (piano-usbhost-back=0): the core stays in the host role until the next boot");
    return;
  }
  stage(20, "back to device mode (piano-usbhost-back)");
  const BackResult b = backToDevice(gHal, sw, policy());
  note("back: GCTL %08x (PRTCAPDIR %u) GUSB2PHYCFG %08x GUSB3PIPECTL %08x GUCTL %08x QSCRATCH hs %08x ss %08x; GCTL soft reset %u, "
       "override restored %u; %u stores: %s",
       b.now.gctl, prtcap(b.now.gctl), b.now.gusb2, b.now.gusb3, b.now.guctl, b.now.qsHs, b.now.qsSs, b.gctlReset, b.overrideRestored,
       b.stores, b.ok ? "device role (PianoDwc3's takeover programs the rest)" : b.why);
  if (!b.ok) stage(-20, b.why);
}

}  // namespace

// --------------------------------------------------------------------------------------------- level 6: PianoUsbHostNub (A1)
// The provider Apple's AppleUSBXHCI gets instead of a PCI device, after QcomDwc3's QcomDwc3Nub (BSD-2-Clause-Patent; facts and
// shape, written anew): the core's first 32 KiB as device memory 0, the name usb-host0, a 4-byte bus-number 0, and interrupt
// source 0 in the IOService terms an IOInterruptEventSource uses (type, register, enable, disable). The line is SPI 133 through
// ArmCpu's armcpu_spi_attach (PianoIrq.h's SpiLine: one handler per INTID, no detach): enable/disable are the distributor's
// set-enable / clear-enable bits (write-one, no read-modify-write); a storm (20000 entries in 100 ms) disables it for good. With
// piano-usbhost-irq=0, or when the line is taken (PianoDwc3's piano-usb-irq=1), a thread calls the handler every millisecond while
// it is enabled, with interrupts off as a primary handler would run.
class PianoUsbHostNub : public IOService {
  OSDeclareDefaultStructors(PianoUsbHostNub)

 public:
  IOReturn getInterruptType(int source, int *interruptType) override;
  IOReturn registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refCon) override;
  IOReturn unregisterInterrupt(int source) override;
  IOReturn enableInterrupt(int source) override;
  IOReturn disableInterrupt(int source) override;
};
OSDefineMetaClassAndStructors(PianoUsbHostNub, IOService)

namespace {

constexpr const char *kStackGate = "PianoUsbHostStack";   // the IOResources gate of the stack's resource personalities
struct NubIrq {
  IOInterruptAction action;
  OSObject *target;
  void *refCon;
  PianoUsbHostNub *nub;
  volatile bool enabled, polled, pollThread;
  pianoirq::SpiLine line;
  pianoirq::StormGuard storm;
  volatile uint64_t entries, strays, polls, registers, enables, disables;
};
NubIrq gNubIrq;

void lineMask()
{
  NubIrq &q = gNubIrq;
  if (q.line.up && q.line.gicd) {
    wr32(q.line.gicd + pianoirq::gicdIcenabler(q.line.id), pianoirq::gicdBit(q.line.id));
    dsb();
  }
}
void lineUnmask()
{
  NubIrq &q = gNubIrq;
  if (q.line.up && q.line.gicd && !q.line.tripped) {
    wr32(q.line.gicd + 0x100 + 4 * (q.line.id / 32), pianoirq::gicdBit(q.line.id));   // GICD_ISENABLER
    dsb();
  }
}
// Primary interrupt context (ArmCpu's handler, before the EOI): Apple's filter runs here and disables the source until its work
// loop has run (IOFilterInterruptEventSource's disableInterruptOccurred calls disableInterrupt below).
void nubPrimary(void *)
{
  NubIrq &q = gNubIrq;
  pianoirq::bump(q.entries);
  if (q.storm.stray(mach_absolute_time())) {
    if (!q.line.tripped) q.line.disable();
    return;
  }
  if (!q.enabled || !q.action) {
    pianoirq::bump(q.strays);
    lineMask();
    return;
  }
  q.action(q.target, q.refCon, q.nub, 0);
}
void nubPoll(void *, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-usbhost-poll");
  NubIrq &q = gNubIrq;
  for (;;) {
    IOSleep(1);
    if (!q.enabled || !q.action) continue;
    const boolean_t was = ml_set_interrupts_enabled(FALSE);
    if (q.enabled && q.action) q.action(q.target, q.refCon, q.nub, 0);
    ml_set_interrupts_enabled(was);
    pianoirq::bump(q.polls);
  }
}
void startPolling()
{
  NubIrq &q = gNubIrq;
  q.polled = true;
  if (q.pollThread) return;
  thread_t t = nullptr;
  if (kernel_thread_start(nubPoll, nullptr, &t) == KERN_SUCCESS) {
    thread_deallocate(t);
    q.pollThread = true;
  }
}

}  // namespace

IOReturn PianoUsbHostNub::getInterruptType(int source, int *interruptType)
{
  if (source != 0 || !interruptType) return kIOReturnNoInterrupt;
  *interruptType = kIOInterruptTypeLevel;
  return kIOReturnSuccess;
}

IOReturn PianoUsbHostNub::registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refCon)
{
  NubIrq &q = gNubIrq;
  if (source != 0 || !handler) return kIOReturnNoInterrupt;
  if (q.action) return kIOReturnNoResources;
  q.target = target;
  q.refCon = refCon;
  q.nub = this;
  q.enabled = false;
  pianoirq::bump(q.registers);
  if (!q.polled && !q.line.up) {
    uint64_t window = 0;
    nanoseconds_to_absolutetime(100 * 1000 * 1000ull, &window);
    q.storm = pianoirq::stormGuard(window, 20000);
    const IOReturn r = q.line.attach(pianoirq::intid(pianohw::kDwc3Spi), false, nubPrimary, nullptr);
    if (r == kIOReturnSuccess) {
      lineMask();   // until enableInterrupt
      note("nub: interrupt 0 registered by %s: SPI %u (INTID %u, level) attached, readback %#x router %#llx; masked until enabled",
           target ? target->getMetaClass()->getClassName() : "?", pianohw::kDwc3Spi, q.line.id, q.line.readback,
           (unsigned long long)q.line.router);
    } else {
      note("nub: interrupt 0 registered by %s: SPI %u attach failed (%#x: taken by PianoDwc3's piano-usb-irq=1?): polled every 1 ms",
           target ? target->getMetaClass()->getClassName() : "?", pianohw::kDwc3Spi, r);
      startPolling();
    }
  } else if (q.polled) {
    note("nub: interrupt 0 registered by %s: polled every 1 ms (piano-usbhost-irq=0)", target ? target->getMetaClass()->getClassName() : "?");
    startPolling();
  }
  __atomic_thread_fence(__ATOMIC_RELEASE);
  q.action = handler;
  return kIOReturnSuccess;
}

IOReturn PianoUsbHostNub::unregisterInterrupt(int source)
{
  NubIrq &q = gNubIrq;
  if (source != 0) return kIOReturnNoInterrupt;
  q.enabled = false;
  lineMask();
  q.action = nullptr;
  q.target = nullptr;
  q.refCon = nullptr;
  return kIOReturnSuccess;
}

IOReturn PianoUsbHostNub::enableInterrupt(int source)
{
  NubIrq &q = gNubIrq;
  if (source != 0) return kIOReturnNoInterrupt;
  pianoirq::bump(q.enables);
  q.enabled = true;
  lineUnmask();
  return kIOReturnSuccess;
}

IOReturn PianoUsbHostNub::disableInterrupt(int source)
{
  NubIrq &q = gNubIrq;
  if (source != 0) return kIOReturnNoInterrupt;
  pianoirq::bump(q.disables);
  q.enabled = false;
  lineMask();
  return kIOReturnSuccess;
}

namespace {

PianoUsbHostNub *gNub;
IOService *gAppleXhci;   // retained once seen

// The nub, attached to the platform expert as QcomDwc3 does (the provider chain Apple's family sees above a PCI controller ends
// at the platform too). Not registered yet.
PianoUsbHostNub *makeNub()
{
  PianoUsbHostNub *nub = OSTypeAlloc(PianoUsbHostNub);
  IODeviceMemory *range = IODeviceMemory::withRange(kXhci, kXhciBytes);
  const OSObject *object = range;
  OSArray *ranges = range ? OSArray::withObjects(&object, 1) : nullptr;
  OSData *zero = OSData::withBytes("\0\0\0\0", 4);
  bool ok = nub && nub->init() && ranges && zero;
  if (ok) {
    nub->setDeviceMemory(ranges);
    nub->setName("usb-host0");
    nub->setProperty("bus-number", zero);
    nub->setProperty("piano-usbhost-nub", "DWC3 usb@a600000 xHCI window, SPI 133 (docs/drivers/usb-host.md H6)");
    ok = nub->attach(IOService::getPlatform());
  }
  OSSafeReleaseNULL(ranges);
  OSSafeReleaseNULL(range);
  OSSafeReleaseNULL(zero);
  if (!ok) OSSafeReleaseNULL(nub);
  return nub;
}

IOService *copyClient(IOService *nub)
{
  IOService *found = nullptr;
  if (OSIterator *it = nub->getClientIterator()) {
    while (OSObject *o = it->getNextObject())
      if ((found = OSDynamicCast(IOService, o))) {
        found->retain();
        break;
      }
    it->release();
  }
  return found;
}

// What Apple's stack enumerated: how many IOUSBHostDevice services, the last one's vendor and product (QcomDwc3's hostStats).
unsigned usbDevices(uint32_t *vidPid)
{
  unsigned count = 0;
  OSDictionary *match = IOService::serviceMatching("IOUSBHostDevice");
  OSIterator *devices = match ? IOService::getMatchingServices(match) : nullptr;
  while (OSObject *o = devices ? devices->getNextObject() : nullptr) {
    ++count;
    IORegistryEntry *e = OSDynamicCast(IORegistryEntry, o);
    OSNumber *vendor = e ? OSDynamicCast(OSNumber, e->getProperty("idVendor")) : nullptr;
    OSNumber *product = e ? OSDynamicCast(OSNumber, e->getProperty("idProduct")) : nullptr;
    if (vendor && product) *vidPid = vendor->unsigned32BitValue() << 16 | (product->unsigned32BitValue() & 0xffffu);
  }
  OSSafeReleaseNULL(devices);
  OSSafeReleaseNULL(match);
  return count;
}

// AppleUSBXHCI's kext and class: loaded on demand (its libraries: IOUSBHostFamily, IOAccessoryManager, IOPortFamily, ...).
bool appleXhciPresent()
{
  const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.driver.usb.AppleUSBXHCI");
  const OSSymbol *name = OSSymbol::withCString("AppleUSBXHCI");
  const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
  if (name) name->release();
  note("AppleUSBXHCI: kext load %#x, class %s (instance size %u)%s", loaded, meta ? "registered" : "NOT registered",
       meta ? meta->getClassSize() : 0, meta ? "" : ": the host stack is not in this kernel collection (PIANO_USBHOST_STACK=1)");
  return meta != nullptr;
}

// piano-usbhost-attach=2: the class by name, as QcomDwc3 starts its subclass (init, attach, start), without one.
IOService *attachDirect(PianoUsbHostNub *nub)
{
  OSObject *object = OSMetaClass::allocClassWithName("AppleUSBXHCI");
  IOService *driver = OSDynamicCast(IOService, object);
  if (!driver) {
    note("attach: allocClassWithName(AppleUSBXHCI) gave %s: an abstract class? (A1 cannot work then; A2 is the owner's decision)",
         object ? "no IOService" : "nothing");
    OSSafeReleaseNULL(object);
    return nullptr;
  }
  OSDictionary *table = OSDictionary::withCapacity(4);
  if (table) {
    const char *const pairs[3][2] = {{"IOClass", "AppleUSBXHCI"}, {"IOProviderClass", "PianoUsbHostNub"},
                                     {"CFBundleIdentifier", "com.apple.driver.usb.AppleUSBXHCI"}};
    for (const auto &pair : pairs) {
      const OSSymbol *value = OSSymbol::withCStringNoCopy(pair[1]);
      if (value) {
        table->setObject(pair[0], value);
        value->release();
      }
    }
  }
  const bool initialised = driver->init(table);
  OSSafeReleaseNULL(table);
  bool started = false;
  if (initialised && driver->attach(nub)) {
    started = driver->start(nub);
    if (!started) driver->detach(nub);
  }
  note("attach: AppleUSBXHCI allocated (%u bytes), init %u, start %s", driver->getMetaClass()->getClassSize(), initialised,
       started ? "true" : "FALSE");
  if (!started) {
    driver->release();
    return nullptr;
  }
  driver->registerService();
  return driver;   // retained (allocation reference)
}

struct AppleState {
  uint32_t usbcmd, usbsts, portsc[4], power, devices, vidPid;
  uint64_t dcbaap, erstba, erdp;
};
AppleState appleState(const Xhci &x)
{
  AppleState s = {};
  const XhciAt at = xhciAt(x);
  s.usbcmd = load(at.op + kOpUsbCmd);
  s.usbsts = load(at.op + kOpUsbSts);
  for (unsigned i = 0; i < 4 && i < x.maxPorts; ++i) s.portsc[i] = load(portscAt(x, i + 1));
  s.dcbaap = rd64(gHal, at.op + kOpDcbaap);
  s.erstba = rd64(gHal, at.rt + kRtIr0 + kIrErstba);
  s.erdp = rd64(gHal, at.rt + kRtIr0 + kIrErdp) & ~0xfull;
  s.power = gAppleXhci ? uint32_t(gAppleXhci->getPowerState()) : ~0u;
  s.devices = usbDevices(&s.vidPid);
  return s;
}
void appleLine(const char *when, const AppleState &s)
{
  const NubIrq &q = gNubIrq;
  note("Apple xHCI %s: USBCMD %08x USBSTS %08x PORTSC %08x %08x DCBAAP %#llx ERSTBA %#llx ERDP %#llx (%s); power state %d; "
       "USB devices %u (last %04x:%04x); interrupt %s: %llu entries %llu strays %llu polls, %llu enables %llu disables",
       when, s.usbcmd, s.usbsts, s.portsc[0], s.portsc[1], (unsigned long long)s.dcbaap, (unsigned long long)s.erstba,
       (unsigned long long)s.erdp, ((s.dcbaap | s.erstba | s.erdp) >> 32) ? "64-bit addresses: the write order matters" : "below 4 GiB",
       int(s.power), s.devices, s.vidPid >> 16, s.vidPid & 0xffff, q.polled ? "polled" : q.line.up ? "SPI 133" : "not registered",
       (unsigned long long)q.entries, (unsigned long long)q.strays, (unsigned long long)q.polls, (unsigned long long)q.enables,
       (unsigned long long)q.disables);
}

// Level 6 after the switch: the gate, the nub, Apple's xHCI on it, then a watch of what it does (stages 30..33).
void appleStack(const Xhci &x, uint32_t seconds)
{
  stage(30, "the host stack's gate resource PianoUsbHostStack (IOUSBHostFamily's and IOUSBMassStorageDriver's resource personalities)");
  IOService::publishResource(kStackGate);
  // AppleUSBHostResources and AppleUSBHostResourcesTypeC share one match category on IOResources: one of them starts.
  IOService *resources = nullptr;
  const char *const names[2] = {"AppleUSBHostResources", "AppleUSBHostResourcesTypeC"};
  for (unsigned i = 0; i < 2 && !resources; ++i) {
    OSDictionary *match = IOService::serviceMatching(names[i]);
    resources = match ? IOService::waitForMatchingService(match, (i ? 1ull : 10ull) * kSecondScale) : nullptr;
    OSSafeReleaseNULL(match);
  }
  note("stack: %s %s", resources ? resources->getMetaClass()->getClassName() : "AppleUSBHostResources",
       resources ? "up (IOUSBHostFamily loaded through the gate)" :
                   "not up within 11 s (no IOUSBHostFamily in this kernel collection: PIANO_USBHOST_STACK=1, or its personality's gate)");
  OSSafeReleaseNULL(resources);
  const OSObject *cable = IOService::getResourceService()->getProperty("AppleUSBCableType");
  const OSString *cableName = OSDynamicCast(OSString, cable);   // an OSSymbol too
  note("stack: resource AppleUSBCableType %s", cableName ? cableName->getCStringNoCopy() : cable ? "(not a string)" : "(none)");
  const bool present = appleXhciPresent();
  stage(31, "PianoUsbHostNub: usb-host0, the core's first 32 KiB, bus-number 0, interrupt source 0");
  gNubIrq.polled = !gArgs.irq;
  gNub = makeNub();
  if (!gNub) {
    stage(-31, "the nub could not be made or attached");
    return;
  }
  note("nub: %s attached to the platform expert (memory %#llx + %#x, interrupt 0 = %s)", gNub->getName(), (unsigned long long)kXhci,
       kXhciBytes, gArgs.irq ? "SPI 133 through ArmCpu" : "polled (piano-usbhost-irq=0)");
  stage(32, gArgs.attach == 1 ? "AppleUSBXHCI through the codeless personality (registerService)" :
            gArgs.attach == 2 ? "AppleUSBXHCI allocated by name, init/attach/start (no personality)" : "nub only (piano-usbhost-attach=0)");
  if (gArgs.attach == 0) {
    note("attach: none (piano-usbhost-attach=0): the nub is not registered");
  } else if (!present) {
    note("attach: skipped: no AppleUSBXHCI class");
  } else if (gArgs.attach == 2) {
    gAppleXhci = attachDirect(gNub);
  } else {
    gNub->registerService();
    for (unsigned i = 0; i < 100 && !gAppleXhci; ++i) {
      IOSleep(100);
      gAppleXhci = copyClient(gNub);
    }
    note("attach: %s", gAppleXhci ? "a client on the nub" : "no client within 10 s (the personality did not match, or its start failed: "
                                                            "piano-usbhost-attach=2 logs the allocation and start)");
  }
  if (gAppleXhci)
    note("attach: client %s, power state %u", gAppleXhci->getMetaClass()->getClassName(), unsigned(gAppleXhci->getPowerState()));
  stage(33, "watch of Apple's xHCI (registers from our side, interrupt counts, IOUSBHostDevice services)");
  AppleState last = appleState(x);
  appleLine("+0 s", last);
  const uint64_t start = nowMs(), end = start + uint64_t(seconds) * 1000;
  uint64_t lastLine = start;
  while (nowMs() < end) {
    IOSleep(gArgs.period);
    const AppleState s = appleState(x);
    const bool changed = s.usbcmd != last.usbcmd || (s.usbsts & ~kUsbStsEint) != (last.usbsts & ~kUsbStsEint) || s.portsc[0] != last.portsc[0] ||
                         s.portsc[1] != last.portsc[1] || s.dcbaap != last.dcbaap || s.erstba != last.erstba || s.power != last.power ||
                         s.devices != last.devices || s.vidPid != last.vidPid;
    if (changed || nowMs() - lastLine >= 30000) {
      char when[32];
      const uint64_t at = nowMs() - start;
      snprintf(when, sizeof when, "+%llu.%01llu s", (unsigned long long)(at / 1000), (unsigned long long)(at % 1000 / 100));
      appleLine(when, s);
      lastLine = nowMs();
    }
    last = s;
  }
}

// Levels 5 and 6 after the survey: one host session; *line gets its one-line summary (the piano-usbhost-host property).
void hostSession(char *line, size_t size)
{
  const uint32_t session = gArgs.back ? gArgs.back : gArgs.watch;
  SwitchResult sw;
  if (!hostSwitchLogged(sw)) {
    snprintf(line, size, "switch FAILED (%s)", sw.why);
    if (gArgs.back && sw.stores) hostEnd(nullptr, sw, true);
    return;
  }
  if (gArgs.level == 5) {
    Probe pr;
    const bool ok = xhciProbe(pr);
    char ports[96] = "-";
    if (pr.running) {
      portsReport(pr, ports, sizeof ports);
      portWatch(pr, session);
    }
    hostEnd(&pr, sw, gArgs.back != 0);
    snprintf(line, size, "switch ok | No-Op %s | ports %s | smmu %s | dma %s | %s", ok ? "PASS" : pr.running ? "FAIL" : "not reached",
             ports, gArgs.smmu == 2 ? "identity" : gArgs.smmu ? "bypass" : "untouched", !gDma.va ? "none" : gDma.below4g ? "<4G" : ">4G",
             gArgs.back ? "back to device" : "host kept");
    return;
  }
  const Xhci x = readCaps(gHal);
  if (!x.sane) {
    stage(-11, "the capability words are not an xHCI's");
    snprintf(line, size, "switch ok | xHCI capability words not sane");
    if (gArgs.back) hostEnd(nullptr, sw, true);
    return;
  }
  const char *probe = "skipped";
  if (gArgs.probe) {
    Probe pr;
    probe = xhciProbe(pr) ? "PASS" : pr.running ? "FAIL" : "not reached";
    if (pr.running) {
      const StopResult st = xhciStop(gHal, pr.x);
      note("xHCI stop before Apple's driver: halted and reset: USBSTS %08x: %s", st.usbsts, st.ok ? "ok" : st.why);
    }
  }
  appleStack(x, session);
  uint32_t vidPid = 0;
  const unsigned devices = usbDevices(&vidPid);
  snprintf(line, size, "switch ok | probe %s | nub %s | AppleUSBXHCI %s | USB devices %u (last %04x:%04x) | interrupt %s | %s", probe,
           gNub ? "up" : "none", gAppleXhci ? "attached" : "not attached", devices, vidPid >> 16, vidPid & 0xffff,
           gNubIrq.polled ? "polled" : gNubIrq.line.up ? "SPI 133" : "-", gArgs.back ? "back to device" : "host kept");
  if (gArgs.back) {
    if (gNub) {
      gNub->terminate(kIOServiceSynchronous);   // a removed controller's path: Apple's family stops its devices and the controller
      note("nub terminated: Apple's xHCI stopped");
    }
    OSSafeReleaseNULL(gAppleXhci);
    const StopResult st = xhciStop(gHal, x);
    note("xHCI stop: USBSTS %08x: %s", st.usbsts, st.ok ? "ok" : st.why);
    hostEnd(nullptr, sw, true);
  }
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
  const uint32_t asked = gArgs.level;
  gArgs.level = runLevel(asked);
  if (gArgs.level != asked)
    note("piano-usbhost=%u: levels 1, 2, 5 and 6 exist (docs/drivers/usb-host.md section 8; 3 and 4 are PianoPower's); running 2", asked);
  // PianoDwc3 owns the controller with piano-usb >= 2: no host mode, except level 5's timed hand-off (its takeover starts at
  // piano-usb-delay, at least 30 s after our host session ends; piano-usbhost-back returns the core to device mode first).
  const uint32_t usb = bootArg("piano-usb", 0), usbDelay = bootArg("piano-usb-delay", 0);
  if (gArgs.level >= 5 && usb >= 2) {
    const bool handOff = gArgs.level == 5 && gArgs.back && usbDelay >= gArgs.delay + gArgs.back + 30;
    if (!handOff) {
      note("piano-usbhost=%u refused: PianoDwc3 owns the controller (piano-usb=%u); boot with piano-usb=0%s; running the level 1 survey",
           gArgs.level, usb, gArgs.level == 5 ? ", or piano-usbhost-back=S with piano-usb-delay >= piano-usbhost-delay + S + 30" : "");
      gArgs.level = 1;
    } else {
      note("hand-off by time: host mode for %u s from %u s, back to device mode, PianoDwc3's takeover at %u s (piano-usb-delay)", gArgs.back,
           gArgs.delay, usbDelay);
    }
  }
  if (gArgs.level <= 2) {
    note("level %u: loads only, nothing written (the SPMI observer's read command is the only store)", gArgs.level);
  } else {
    note("level %u: %s; back=%u csr=%u utmi=%u reset=%u smmu=%u slots=%u%s", gArgs.level,
         gArgs.level == 5 ? "host mode with our own xHCI probe (writes the core, its glue and the xHCI; polled)"
                          : "host mode with Apple's AppleUSBXHCI on PianoUsbHostNub (path A1: no subclass)",
         gArgs.back, gArgs.csr, gArgs.utmi, gArgs.reset, gArgs.smmu, gArgs.slots, gArgs.level == 6 ? "" : "");
    if (gArgs.level == 6) note("level 6: attach=%u irq=%u probe=%u", gArgs.attach, gArgs.irq, gArgs.probe);
    if (gArgs.smmu) {
      stage(9, gArgs.smmu == 2 ? "SMMU: stream 0x40 through an identity-mapped write-back context bank" : "SMMU: stream 0x40 through a bypass bank");
      if (!smmuSetup(gArgs.smmu)) {
        stage(-9, "SMMU: no route for stream 0x40: host mode not tried (piano-usbhost-smmu=0 leaves the SMMU as the firmware left it)");
        gArgs.level = 1;
      }
    } else {
      note("smmu: stream %#x left as the firmware left it (piano-usbhost-smmu=0)", pianohw::kUsbStream);
    }
  }
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
  note("role: the policy would choose %s: %s (%s)", roleName(r.role), r.why,
       gArgs.level >= 5 ? "levels 5 and 6 take the host role whatever the cable says" : "level 1 changes nothing");
  char line[240];
  summary(v, t.ok, p, c, k, q, r, line, sizeof line);
  setProperty("piano-usbhost-survey", line);
  note("summary: %s", line);

  if (gArgs.level == 5 || gArgs.level == 6) {
    char host[200];
    hostSession(host, sizeof host);
    setProperty("piano-usbhost-host", host);
    note("host summary: %s", host);
  }

  if (gArgs.level == 2 && gArgs.watch) {
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
  note("done: %llu MMIO loads, %llu MMIO stores; %u pages mapped, %u map failures", (unsigned long long)gLoads, (unsigned long long)gStores,
       gPageCount, gMapFailures);
  unmapAll();
  unmapRegion(gCore);
  unmapRegion(gCnfg);
  unmapRegion(gObsv);
}
