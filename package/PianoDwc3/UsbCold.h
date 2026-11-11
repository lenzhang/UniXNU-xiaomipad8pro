// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDwc3's cold start (piano-usb-cold, docs/drivers/usb-device-debug.md section 17): the USB controller, its eUSB2 PHY, the eUSB2
// repeater and their rails brought up from this driver's own tables, whatever the boot path left, instead of relying on the state
// ABL fastboot leaves behind.
//
// Why: every USB console so far followed ABL fastboot, which leaves the controller, the M31 PHY and the repeater running as a device
// for the Mac. On the normal power-on path (ABL -> BootShim -> UEFI -> UniXNU -> XNU) the installed daily ESP ran for hours with
// PianoDwc3 at "no-host ..., link state 3, configuration 0": the device controller ran and the Mac never saw it
// (docs/forensics-2026-11-11.md section 3, docs/results-2026-11-11.md).
//
// What runs (each step one or more "PianoDwc3: cold ..." console lines; the breadcrumb piano_dwc3_cold holds the step reached,
// negative where a hard step failed):
//   inspect   read only: GCC (the USB30 GDSC, the five branch clocks, the master and mock_utmi RCGs, GPLL0), the TCSR USB2 reference
//             clock, the core (its role, whether the firmware ran it as a device), the M31 PHY, the repeater and its rails over SPMI,
//             the Type-C orientation (TLMM GPIO61; the PMIC's Type-C block only where no other kext reads it). reasons() names what
//             is not a running device controller; piano-usb-cold=auto runs the cold start when there is any.
//   c1 power  the GDSC on, GPLL0's enable vote (GCC 0x52020, under PianoA830's GCC vote lock, bits only ever set), the branches,
//             the RCGs at 200 MHz / 19.2 MHz (the DT's assigned rates), the TCSR USB2 reference clock (the M31's ref).
//   c2 stop   whatever runs stopped (the device controller halted, or the xHCI's run bit cleared), the event interrupt masked; with
//             the BCR step (opt-in) the controller's block reset (USB30_PRIM_BCR), as Linux's glue does at its probe.
//   c3 rails  vreg_l2d_0p88 (the M31's vdd), vreg_l15b_1p8 and vreg_l5b_3p1 (the repeater's vdd18 and vdd3) voted on through
//             RPMh (apps DRV-2 under PianoA830's DRV-2 lock word; mode, set point, enable), read back over SPMI. vreg_l3g_1p2 (the
//             M31's vdda12) is never voted: it is also the UFS PHY's PLL rail (on whenever UFS runs).
//   c4 repeater  enabled, the board's tuning (piano's DT) when it was found off, device mode (no forced 19.2 MHz host clock),
//             status OK awaited (only when its APID belongs to EE 0, else nothing is written).
//   c5 PHY    the M31 eUSB2 PHY reset (GCC QUSB2PHY_PRIM_BCR) and initialised from PianoUsbHw.h's kM31Init (the stock firmware's
//             sequence; its end state is what the fastboot path reads), read back.
// The takeover then goes on as before: halt, DMA, the UTMI clock as PIPE clock, core soft reset, device mode (PRTCAPDIR, high
// speed), the QSCRATCH VBUS/session override, RUN_STOP.
//
// Never touched: the ADSP (it hosts charging, audio and the sensor hub), the QMP PHY and the DIO4485 mux (high speed needs neither),
// the CX/MX levels and the interconnect votes (shared RPMh resources: one vote per DRV, so a second writer on DRV-2 would replace
// another kext's). Every poll is bounded; a hard step that fails returns at once (the USB thread retries: at most kColdTries cold
// starts per boot). No core, QSCRATCH or M31 access happens with the glue unclocked, none to the M31 with its vdd known off.
//
// The test hook piano-usb-cold-sim=MASK (simulate()) first leaves the hardware the way the normal path may leave it, so the cold
// start can be proven from a RAM boot (ABL fastboot) without writing the ESP.
//
// Portable and header-only: PianoDwc3.cpp instantiates it with its kernel Hal, tests/usb_cold_host.cpp with a model of the hardware.
// The Hal:
//   uint32_t gcc(uint32_t off); void gccWrite(uint32_t off, uint32_t v);        GCC words (always readable)
//   uint32_t tcsr(); void tcsrWrite(uint32_t v);                                the TCSR USB2 reference clock word
//   uint32_t core(uint32_t off); void coreWrite(uint32_t off, uint32_t v);      DWC3 (xHCI and global/device registers)
//   uint32_t m31(uint32_t off); void m31Write(uint32_t off, uint32_t v);        the M31 eUSB2 PHY
//   uint32_t qs(uint32_t off); bool qsWrite(uint32_t off, uint32_t v);          QSCRATCH (the write through PianoDwc3's sleep guard)
//   int spmiRead(uint16_t ppid, uint8_t reg, uint8_t *v);                       one byte over the observer channel: 0, or negative
//   int spmiWrite(uint16_t ppid, uint8_t reg, uint8_t v);                       one byte (the APID owned by EE 0): 0, or negative
//   int spmiOwner(uint16_t ppid);                                               the APID's owner EE; negative: not mapped / SPMI off
//   int railVote(unsigned rail, uint16_t mv, bool enable);                      one VRM transaction under the DRV-2 lock: 0, or <0
//   bool voteLock(bool take);                                                   the GCC vote lock word: true when taken
//   bool gpio61(uint32_t *cfg, uint32_t *io);  TypeC typec();                   the orientation input; the PMIC's Type-C block
//   void udelay(unsigned us); void cold(int step); void note(const char *fmt, ...);
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "Dwc3Regs.h"
#include "PianoUsbHw.h"

namespace pianousb {
namespace cold {

// --------------------------------------------------------------------------------------------------------- the boot arguments
// piano-usb-cold: 0 the inherited state only (the takeover of before, no cold start, no link watch), 1 always the cold start, auto
// (the default) the cold start when inspect() finds the inherited state is not a running device controller, plus the link watch.
enum class Mode : uint8_t { inherited = 0, always = 1, automatic = 2 };
inline const char *modeName(Mode m) { return m == Mode::inherited ? "0" : m == Mode::always ? "1" : "auto"; }
// The argument's text ("0", "1", "auto"); absent, empty or anything else: auto, and *known false for anything else.
inline Mode parseMode(const char *text, bool given, bool *known = nullptr)
{
  if (known) *known = true;
  if (!given) return Mode::automatic;
  if (text && text[0] == '0' && !text[1]) return Mode::inherited;
  if (text && text[0] == '1' && !text[1]) return Mode::always;
  if (text && text[0] == 'a' && text[1] == 'u' && text[2] == 't' && text[3] == 'o' && !text[4]) return Mode::automatic;
  if (known) *known = false;
  return Mode::automatic;
}

// piano-usb-cold-steps: which optional parts the cold start runs (c1 power and c2 stop always run).
enum : uint32_t {
  kStepRails = 1u << 0,        // c3: vote the three rails
  kStepRepeater = 1u << 1,     // c4: the repeater (enable, device mode; the DT tuning when it was found off)
  kStepPhy = 1u << 2,          // c5: the M31 PHY reset and init
  kStepBcr = 1u << 3,          // c2: the controller's block reset (opt-in: it also drops what the firmware set in the globals)
  kStepRcg = 1u << 4,          // c1: program the RCGs that are not at their rates
  kStepTuneAlways = 1u << 5,   // c4: the DT tuning even when the repeater was found on (opt-in)
  kStepsDefault = kStepRails | kStepRepeater | kStepPhy | kStepRcg,
  kStepsAll = 0x3fu,
};
// piano-usb-cold-sim: what simulate() leaves off before the decision (a test from ABL fastboot); several bits combine.
enum : uint32_t {
  kSimRepeater = 1u << 0,   // the repeater disabled and its host-mode clock forced
  kSimPhy = 1u << 1,        // the M31 PHY reset and left uninitialised, its reference clock off
  kSimHost = 1u << 2,       // the core left in the host role, the VBUS/session override cleared
  kSimClocks = 1u << 3,     // the RCGs parked on TCXO, the branch clocks off, the USB30 GDSC collapsed
  kSimRails = 1u << 4,      // the three rails' DRV-2 votes withdrawn (enable 0)
  kSimAll = 0x1fu,
};
constexpr unsigned kColdTries = 3;           // cold starts per boot at most (the first, retries and the link watch's together)
constexpr uint32_t kWatchDefaultS = 15;      // piano-usb-cold-wait: no bus reset this long after RUN_STOP: a cold start (0: off)
constexpr uint32_t kWatchMaxMs = 300000;     // the wait doubles after each cold start, up to this
constexpr uint32_t kRetryMs = 2000;          // a cold start that failed: the next one this much later

// ------------------------------------------------------------------------------------------------------------------- facts
// The rails (piano's boot DT: phy@88e3000 vdd-supply / vdda12-supply, pmic@7 phy@fd00 vdd18-supply / vdd3-supply; the regulators'
// names, ranges and PMICs; cmd-db names as PianoKeyboard's ldob11/ldob14 and PianoWifi's ldof*/ldog3, hardware-verified there).
// PMIC b = SID 1 (pm8550), d = SID 3 (pm8550ve). The regulator's peripheral: page 0x40 + n - 1 or 0xc0 + n (the SM8550 page rule
// PianoWifi's survey uses; PianoWcnRails.h), the one whose TYPE reads LDO.
struct Rail {
  const char *name, *cmdDb, *what;
  uint8_t sid, page, altPage;
  uint16_t minMv, maxMv;
};
constexpr unsigned kRails = 3, kRailL2d = 0, kRailL15b = 1, kRailL5b = 2;
constexpr Rail kRailTable[kRails] = {
  {"vreg_l2d_0p88", "ldod2", "M31 vdd, QMP vdda-phy", 3, 0xc2, 0x41, 880, 912},
  {"vreg_l15b_1p8", "ldob15", "repeater vdd18", 1, 0x4e, 0xcf, 1800, 1800},
  {"vreg_l5b_3p1", "ldob5", "repeater vdd3", 1, 0x44, 0xc5, 3100, 3148},
};
// A PMIC5 LDO's registers (as PianoWifi/PianoBluetooth read them): TYPE 0x04 (LDO 0x04), STATUS1 0x08 (b7 on), the set point
// 0x40/0x41 (mV, little-endian), MODE 0x45, EN_CTL 0x46 (b7 enabled).
constexpr uint8_t kRegType = 0x04, kTypeLdo = 0x04, kRegStatus = 0x08, kRegVsetLb = 0x40, kRegVsetUb = 0x41, kRegMode = 0x45,
                  kRegEnCtl = 0x46, kRegOn = 0x80;
struct RailRead {
  uint16_t ppid = 0;
  bool found = false, read = false;   // found: an LDO-type peripheral at one of the two pages; read: its registers answered
  uint8_t status = 0, mode = 0, enable = 0;
  uint16_t mv = 0;
  bool on() const { return read && ((enable & kRegOn) || (status & kRegOn)); }
  bool off() const { return read && !(enable & kRegOn) && !(status & kRegOn); }   // known off (both bits clear)
};
// The set point voted: the PMIC's own when it is within the DT's range, else the DT minimum (never above the maximum).
inline uint16_t voteMv(const Rail &r, const RailRead &s) { return s.read && s.mv >= r.minMv && s.mv <= r.maxMv ? s.mv : r.minMv; }

// The repeater's board tuning: piano's DT phy@fd00 (qcom,tune-usb2-amplitude 0x0e, qcom,tune-res-fsdif 1, qcom,tune-usb2-disc-thres
// 7, qcom,tune-usb2-preem 6, qcom,squelch-detector-bp -3000 = code 3 of the -6000..+1000 bp scale in steps of 1000), into the
// registers Linux's repeater driver names for those properties. Board facts, the same on every unit (no calibration).
struct RegByte { uint8_t reg, value; };
constexpr RegByte kRptTune[] = {{0x51, 0x0e}, {0x52, 0x01}, {0x53, 0x07}, {0x54, 0x03}, {0x57, 0x06}};
constexpr unsigned kRptTuneCount = sizeof kRptTune / sizeof kRptTune[0];

// The M31 PHY's state words (byte-wide registers, the low byte of a 32-bit read; bit names after Linux's phy-qcom-m31-eusb2.c):
// UTMI_CTRL0 0x3c (b0 SLEEPM), UTMI_CTRL5 0x50 (b1 POR), HS_PHY_CTRL_COMMON0 0x54 (b0 PHY_ENABLE, b1 SIDDQ_SEL, b2 SIDDQ, 6:4 FSEL,
// 6 = 38.4 MHz), HS_PHY_CTRL2 0x64 (b2 USB2_SUSPEND_N, b3 its select), CFG0 0x94 (b1 the common-control override). Ready = enabled,
// not powered down, 38.4 MHz, out of POR, not suspended, override released: kM31Init's end state (01 00 6b 07 00 on the fastboot path).
struct M31Read { uint8_t utmi0 = 0, por = 0, common0 = 0, hsCtrl2 = 0, override = 0; };
inline bool m31Ready(const M31Read &m)
{
  return (m.common0 & 0x07) == 0x03 && ((m.common0 >> 4) & 7) == 6 && !(m.por & 0x02) && (m.hsCtrl2 & 0x04) && !(m.override & 0x02);
}

// The PMIC's Type-C block (package/PianoPower/ChargerPmic.h): misc +0x0b (b0 attached, b1 CC2, b5 VBUS, b6 we source), sink status
// +0x06 (a source's Rp), source status +0x08, mode +0x44.
struct TypeC { bool read = false; uint8_t misc = 0, snk = 0, src = 0, mode = 0; };

inline bool isDwc3(uint32_t snpsId)
{
  const uint32_t ip = snpsId >> 16;
  return ip == 0x5533 || ip == 0x3331 || ip == 0x3332;
}
constexpr uint32_t kBranches[] = {pianohw::kGccCfgNocUsb3Axi, pianohw::kGccAggreUsb3Axi, pianohw::kGccUsb30Master,
                                  pianohw::kGccUsb30Sleep, pianohw::kGccUsb30MockUtmi};

// ------------------------------------------------------------------------------------------------------- the inherited state
struct Inherited {
  uint32_t gdsc = 0, cfgNoc = 0, aggre = 0, master = 0, sleep = 0, mockUtmi = 0, masterCfg = 0, mockCfg = 0, gpll0 = 0,
           gpllVote = 0, clkref = 0;
  bool clocked = false;                          // the GDSC and the five branches on: the core, QSCRATCH and the M31 answer
  bool coreRead = false;                         // the core answered as a DWC_usb3x (only read when clocked)
  uint32_t snpsId = 0, gctl = 0, gevntLo = 0, gevntHi = 0, devten = 0, dctl = 0, dsts = 0, dcfg = 0;
  bool phyRead = false;
  M31Read phy;
  bool rptRead = false;
  int rptOwner = -1;
  uint8_t rptStatus = 0, rptEnable = 0, rptForceEn = 0, rptForceVal = 0;
  RailRead rails[kRails];
  bool gpioRead = false;
  uint32_t gpioCfg = 0, gpioIo = 0;
  TypeC typec;
  uint32_t role() const { return (gctl >> 12) & 3u; }   // GCTL.PRTCAPDIR: 1 host, 2 device, 3 OTG
};

// What is not a running device controller. Unknowns (a register that could not be read) are never a reason.
enum : uint32_t {
  kWhyGdsc = 1u << 0, kWhyClocks = 1u << 1, kWhyRcg = 1u << 2, kWhyClkRef = 1u << 3, kWhyCore = 1u << 4, kWhyRole = 1u << 5,
  kWhyNeverRan = 1u << 6, kWhyPhy = 1u << 7, kWhyRepeater = 1u << 8, kWhyRepeaterHost = 1u << 9, kWhyRails = 1u << 10,
};
constexpr unsigned kWhyCount = 11;
inline const char *whyName(unsigned bit)
{
  static const char *const names[kWhyCount] = {
    "the USB30 GDSC is collapsed", "a branch clock is off", "an RCG is not at its rate (master 200 MHz, mock_utmi 19.2 MHz)",
    "the USB2 reference clock is off", "the core does not answer", "the core is not in the device role",
    "the firmware never ran it as a device (no event buffer, no device events)", "the M31 PHY is not initialised",
    "the repeater is not enabled/ready", "the repeater has the host-mode clock forced", "a rail is off",
  };
  return bit < kWhyCount ? names[bit] : "?";
}
inline uint32_t reasons(const Inherited &s)
{
  using namespace pianohw;
  uint32_t r = 0;
  if (!(s.gdsc & kGdscPwrOn)) r |= kWhyGdsc;
  else if (!s.clocked) r |= kWhyClocks;
  if ((s.masterCfg & kRcgCfgMask) != kRcgMasterCfg || (s.mockCfg & kRcgCfgMask) != kRcgMockUtmiCfg) r |= kWhyRcg;
  if (!(s.clkref & 1u)) r |= kWhyClkRef;
  if (s.clocked && !s.coreRead) r |= kWhyCore;
  if (s.coreRead && s.role() != 2) r |= kWhyRole;
  if (s.coreRead && !s.gevntLo && !s.gevntHi && !s.devten) r |= kWhyNeverRan;
  if (s.phyRead && !m31Ready(s.phy)) r |= kWhyPhy;
  if (s.rptRead && !((s.rptStatus & kRptOk) && (s.rptEnable & kRptEnable))) r |= kWhyRepeater;
  if (s.rptRead && (s.rptForceEn & kRptClk19p2)) r |= kWhyRepeaterHost;
  for (const RailRead &rail : s.rails)
    if (rail.off()) r |= kWhyRails;
  return r;
}
// "a, b, c" for the reasons in r (or "none"), into out (always terminated).
inline void whyText(uint32_t r, char *out, size_t n)
{
  if (!n) return;
  size_t at = 0;
  auto put = [&](const char *s) {
    while (*s && at + 1 < n) out[at++] = *s++;
  };
  if (!r) put("none");
  for (unsigned bit = 0, first = 1; bit < kWhyCount; ++bit)
    if (r & (1u << bit)) {
      if (!first) put("; ");
      put(whyName(bit));
      first = 0;
    }
  out[at] = 0;
}

// ---------------------------------------------------------------------------------------------------------------- helpers
template <class H> bool waitGcc(H &h, uint32_t off, uint32_t mask, uint32_t want, unsigned us)
{
  for (unsigned waited = 0; waited < us; waited += 10) {
    if ((h.gcc(off) & mask) == want) return true;
    h.udelay(10);
  }
  return (h.gcc(off) & mask) == want;
}
template <class H> bool waitCore(H &h, uint32_t off, uint32_t mask, uint32_t want, unsigned us)
{
  for (unsigned waited = 0; waited < us; waited += 100) {
    if ((h.core(off) & mask) == want) return true;
    h.udelay(100);
  }
  return (h.core(off) & mask) == want;
}
template <class H> bool glueClocked(H &h)
{
  if (!(h.gcc(pianohw::kGccUsb30Gdsc) & pianohw::kGdscPwrOn)) return false;
  for (uint32_t b : kBranches)
    if (h.gcc(b) & pianohw::kCbcrClkOff) return false;
  return true;
}
template <class H> void readRail(H &h, unsigned i, RailRead &s)
{
  const Rail &r = kRailTable[i];
  s = RailRead();
  const uint8_t pages[2] = {r.page, r.altPage};
  for (unsigned k = 0; k < 2 && !s.found; ++k) {
    const uint16_t ppid = uint16_t(r.sid << 8 | pages[k]);
    uint8_t type = 0;
    if (h.spmiRead(ppid, kRegType, &type) == 0 && type == kTypeLdo) {
      s.ppid = ppid;
      s.found = true;
    }
  }
  if (!s.found) return;
  uint8_t lb = 0, ub = 0;
  s.read = h.spmiRead(s.ppid, kRegStatus, &s.status) == 0 && h.spmiRead(s.ppid, kRegVsetLb, &lb) == 0 &&
           h.spmiRead(s.ppid, kRegVsetUb, &ub) == 0 && h.spmiRead(s.ppid, kRegMode, &s.mode) == 0 &&
           h.spmiRead(s.ppid, kRegEnCtl, &s.enable) == 0;
  s.mv = uint16_t(lb | ub << 8);
}
template <class H> M31Read readM31(H &h)
{
  using namespace pianohw;
  M31Read m;
  m.utmi0 = uint8_t(h.m31(kM31UtmiCtrl0));
  m.por = uint8_t(h.m31(kM31Por));
  m.common0 = uint8_t(h.m31(kM31Common0));
  m.hsCtrl2 = uint8_t(h.m31(kM31HsCtrl2));
  m.override = uint8_t(h.m31(kM31Override));
  return m;
}
inline const char *railState(const RailRead &s) { return !s.found ? "no LDO found" : !s.read ? "not readable" : s.on() ? "on" : "OFF"; }

// ------------------------------------------------------------------------------------------------------------------ inspect
// Read only. spmi: the rails and the repeater over SPMI (piano-usb-spmi). The M31 is read only with the glue clocked, its reference
// clock on and its vdd not known off; when a test withdrew the rails' votes (railsWithdrawn) only with its vdd read back on.
template <class H> Inherited inspect(H &h, bool spmi, bool railsWithdrawn = false)
{
  using namespace pianohw;
  using namespace dwc3;
  Inherited s;
  s.gdsc = h.gcc(kGccUsb30Gdsc);
  s.cfgNoc = h.gcc(kGccCfgNocUsb3Axi);
  s.aggre = h.gcc(kGccAggreUsb3Axi);
  s.master = h.gcc(kGccUsb30Master);
  s.sleep = h.gcc(kGccUsb30Sleep);
  s.mockUtmi = h.gcc(kGccUsb30MockUtmi);
  s.masterCfg = h.gcc(kGccUsb30MasterRcg + 4);
  s.mockCfg = h.gcc(kGccUsb30MockUtmiRcg + 4);
  s.gpll0 = h.gcc(kGccGpll0Mode);
  s.gpllVote = h.gcc(kGccGpllVote);
  s.clkref = h.tcsr();
  s.clocked = glueClocked(h);
  if (spmi) {
    for (unsigned i = 0; i < kRails; ++i) readRail(h, i, s.rails[i]);
    const uint16_t p = kRepeaterPpid;
    s.rptOwner = h.spmiOwner(p);
    s.rptRead = s.rptOwner >= 0 && h.spmiRead(p, kRepeaterStatus, &s.rptStatus) == 0 && h.spmiRead(p, kRepeaterEnable, &s.rptEnable) == 0 &&
                h.spmiRead(p, kRptForceEn5, &s.rptForceEn) == 0 && h.spmiRead(p, kRptForceVal5, &s.rptForceVal) == 0;
  }
  if (s.clocked) {
    s.snpsId = h.core(GSNPSID);
    s.coreRead = isDwc3(s.snpsId);
    if (s.coreRead) {
      s.gctl = h.core(GCTL);
      s.gevntLo = h.core(GEVNTADRLO);
      s.gevntHi = h.core(GEVNTADRHI);
      s.devten = h.core(DEVTEN);
      s.dctl = h.core(DCTL);
      s.dsts = h.core(DSTS);
      s.dcfg = h.core(DCFG);
    }
    const RailRead &vdd = s.rails[kRailL2d];
    if ((s.clkref & 1u) && !vdd.off() && (!railsWithdrawn || vdd.on())) {
      s.phy = readM31(h);
      s.phyRead = true;
    }
  }
  s.gpioRead = h.gpio61(&s.gpioCfg, &s.gpioIo);
  s.typec = h.typec();
  return s;
}

// The inspect lines and the verdict. Returns the reasons.
template <class H> uint32_t report(H &h, const Inherited &s, Mode mode, bool spmi)
{
  using namespace pianohw;
  const uint32_t r = reasons(s);
  h.note("cold: inherited GCC: gdsc %08x cfg_noc %08x aggre %08x master %08x sleep %08x mock_utmi %08x; RCG cfg master %08x mock_utmi "
         "%08x; GPLL0 mode %08x vote %08x; TCSR usb2 clkref %08x",
         s.gdsc, s.cfgNoc, s.aggre, s.master, s.sleep, s.mockUtmi, s.masterCfg, s.mockCfg, s.gpll0, s.gpllVote, s.clkref);
  if (s.clocked)
    h.note("cold: inherited core %08x GCTL %08x (role %u: %s) GEVNTADR %08x%08x DEVTEN %08x DCTL %08x DSTS %08x DCFG %08x; M31 %s "
           "[3c] %02x [50] %02x [54] %02x [64] %02x [94] %02x",
           s.snpsId, s.gctl, s.role(), s.role() == 2 ? "device" : s.role() == 1 ? "host" : s.role() == 3 ? "OTG" : "none", s.gevntHi,
           s.gevntLo, s.devten, s.dctl, s.dsts, s.dcfg, !s.phyRead ? "not read (its reference clock off or its vdd off)" :
           m31Ready(s.phy) ? "ready" : "NOT initialised", s.phy.utmi0, s.phy.por, s.phy.common0, s.phy.hsCtrl2, s.phy.override);
  else
    h.note("cold: inherited: the glue is NOT clocked: the core, QSCRATCH and the M31 are not read");
  if (spmi) {
    h.note("cold: inherited repeater (SID 7 0xfd00): %s status %02x enable %02x force_en_5 %02x force_val_5 %02x (APID owner EE %d)",
           !s.rptRead ? "NOT read:" : (s.rptStatus & kRptOk) && (s.rptEnable & kRptEnable) ? "ready and enabled," : "NOT ready/enabled,",
           s.rptStatus, s.rptEnable, s.rptForceEn, s.rptForceVal, s.rptOwner);
    for (unsigned i = 0; i < kRails; ++i) {
      const RailRead &v = s.rails[i];
      h.note("cold: inherited rail %s (%s, %s): PPID %#x status %02x enable %02x mode %02x %u mV: %s", kRailTable[i].name,
             kRailTable[i].cmdDb, kRailTable[i].what, v.ppid, v.status, v.enable, v.mode, v.mv, railState(v));
    }
  } else {
    h.note("cold: inherited: the repeater and the rails are not read (piano-usb-spmi=0)");
  }
  if (s.gpioRead)
    h.note("cold: Type-C orientation %s (GPIO61 level %u, cfg %08x): high speed needs no lane mux; %s", (s.gpioIo & 1u) ? "reverse (CC2)" :
           "normal (CC1)", s.gpioIo & 1u, s.gpioCfg, s.typec.read ? "PMIC Type-C below" : "the PMIC's Type-C block is PianoPower's to read");
  if (s.typec.read)
    h.note("cold: PMIC Type-C misc %02x (attached %u, CC%u, VBUS %u, we source %u) sink %02x source %02x mode %02x%s", s.typec.misc,
           s.typec.misc & 1u, (s.typec.misc >> 1 & 1u) + 1, s.typec.misc >> 5 & 1u, s.typec.misc >> 6 & 1u, s.typec.snk, s.typec.src,
           s.typec.mode, (s.typec.misc & 0x40) ? ": a device (not a host) is attached: device mode cannot enumerate" : "");
  char why[480];
  whyText(r, why, sizeof why);
  const bool cold = mode == Mode::always || (mode == Mode::automatic && r);
  h.note("cold: verdict (piano-usb-cold=%s): the inherited state is %s (%s): %s", modeName(mode),
         r ? "NOT a running device controller" : "a running device controller", why,
         cold ? "the cold start runs" : r ? "taken as it is anyway (piano-usb-cold=0)" : "taken as it is (the takeover of before)");
  return r;
}

// ---------------------------------------------------------------------------------------------------------------- the steps
struct Result {
  int failed = 0;         // 0, or the step (1, 2) whose hard part failed
  uint32_t soft = 0;      // 1 << step for each step that failed softly (logged; the start went on)
  unsigned votes = 0;     // rails voted
  bool voted[kRails] = {false, false, false};
  bool reset = false, rptInit = false, tuned = false, phyInit = false;
  uint32_t snpsId = 0;
  const char *why = nullptr;
  bool ok() const { return failed == 0; }
};

template <class H> bool fail(H &h, Result &res, int step, const char *why)
{
  res.failed = step;
  res.why = why;
  h.cold(-step);
  h.note("cold c%d FAILED: %s", step, why);
  return false;
}

template <class H> void stepRcg(H &h, uint32_t cmd, uint32_t want, const char *name, Result &res)
{
  using namespace pianohw;
  const uint32_t cfg = h.gcc(cmd + 4);
  if ((cfg & kRcgCfgMask) == want) return;
  h.gccWrite(cmd, h.gcc(cmd) | kRcgRootEn);   // the root forced on while the source changes
  const bool root = waitGcc(h, cmd, kRcgRootOff, 0, 500);
  h.gccWrite(cmd + 4, (cfg & ~kRcgCfgMask) | want);
  h.gccWrite(cmd, h.gcc(cmd) | kRcgUpdate);
  const bool done = waitGcc(h, cmd, kRcgUpdate, 0, 1000);
  h.gccWrite(cmd, h.gcc(cmd) & ~(kRcgRootEn | kRcgUpdate));
  h.note("cold c1: RCG %s CFG %08x -> %08x (%s%s)", name, cfg, h.gcc(cmd + 4), done ? "updated" : "the update did NOT complete",
         root ? "" : "; its root did not report on");
  if (!done) res.soft |= 1u << 1;
}

// c1: power and clocks. Hard: the GDSC and every branch must come on (else nothing of the core may be touched).
template <class H> bool stepPower(H &h, uint32_t steps, Result &res)
{
  using namespace pianohw;
  h.cold(1);
  const uint32_t g = h.gcc(kGccUsb30Gdsc);
  if (!(g & kGdscPwrOn)) {
    h.note("cold c1: the USB30 GDSC is collapsed (%08x): SW_COLLAPSE off, RETAIN_FF on, the waits 2/2/15", g);
    h.gccWrite(kGccUsb30Gdsc, (g & ~(kGdscSwCollapse | kGdscWaitMask)) | kGdscWaits | kGdscRetainFf);
    if (!waitGcc(h, kGccUsb30Gdsc, kGdscPwrOn, kGdscPwrOn, 10000)) return fail(h, res, 1, "the USB30 GDSC does not power on in 10 ms");
  }
  const uint32_t vote = h.gcc(kGccGpllVote);
  const char *gpll = "voted already";
  if (!(vote & kGpll0Vote)) {
    const bool locked = h.voteLock(true);
    const uint32_t v = h.gcc(kGccGpllVote);
    if (!(v & kGpll0Vote)) h.gccWrite(kGccGpllVote, v | kGpll0Vote);
    if (locked) h.voteLock(false);
    gpll = waitGcc(h, kGccGpll0Mode, kPllLockDet, kPllLockDet, 1000) ? (locked ? "voted (lock taken), locked" : "voted (the lock was held: "
           "voted anyway, a bit only ever set), locked") : "voted, NOT locked within 1 ms";
  }
  unsigned switched = 0;
  for (uint32_t b : kBranches) {
    const uint32_t v = h.gcc(b);
    if (!(v & kCbcrClkOff)) continue;
    h.gccWrite(b, v | kCbcrClkEnable);
    ++switched;
    if (!waitGcc(h, b, kCbcrClkOff, 0, 10000)) {
      h.note("cold c1: GCC branch %#x stays off (%08x)", b, h.gcc(b));
      return fail(h, res, 1, "a USB branch clock does not come on");
    }
  }
  if (steps & kStepRcg) {
    stepRcg(h, kGccUsb30MasterRcg, kRcgMasterCfg, "usb30_master (200 MHz)", res);
    stepRcg(h, kGccUsb30MockUtmiRcg, kRcgMockUtmiCfg, "mock_utmi (19.2 MHz)", res);
  }
  const uint32_t c = h.tcsr();
  if (!(c & 1u)) h.tcsrWrite(c | 1u);
  h.note("cold c1 power: GDSC %08x; GPLL0 mode %08x vote %08x (%s); branches cfg_noc %08x aggre %08x master %08x sleep %08x mock_utmi %08x "
         "(%u switched on); RCG cfg master %08x mock_utmi %08x; usb2 clkref %08x", h.gcc(kGccUsb30Gdsc), h.gcc(kGccGpll0Mode),
         h.gcc(kGccGpllVote), gpll, h.gcc(kGccCfgNocUsb3Axi), h.gcc(kGccAggreUsb3Axi), h.gcc(kGccUsb30Master), h.gcc(kGccUsb30Sleep),
         h.gcc(kGccUsb30MockUtmi), switched, h.gcc(kGccUsb30MasterRcg + 4), h.gcc(kGccUsb30MockUtmiRcg + 4), h.tcsr());
  return true;
}

// c2: stop what runs; the block reset with kStepBcr. Hard: the core must answer (before and after the reset).
template <class H> bool stepStop(H &h, uint32_t steps, Result &res)
{
  using namespace pianohw;
  using namespace dwc3;
  h.cold(2);
  const uint32_t snps = h.core(GSNPSID);
  if (!isDwc3(snps)) {
    h.note("cold c2: GSNPSID %08x", snps);
    return fail(h, res, 2, "the core does not answer as a DWC_usb3x (GSNPSID)");
  }
  h.coreWrite(GEVNTSIZ, h.core(GEVNTSIZ) | GEVNTSIZ_INTMASK);
  const uint32_t gctl = h.core(GCTL), role = (gctl >> 12) & 3u;
  const char *stopped = "nothing ran";
  if (role == 1) {   // host: the xHCI's operational registers start at CAPLENGTH
    const uint32_t cap = h.core(0) & 0xffu;
    if (cap >= 0x20 && cap < 0x100 && (h.core(cap) & 1u)) {
      h.coreWrite(cap, h.core(cap) & ~1u);
      stopped = waitCore(h, cap + 4, 1u, 1u, 100000) ? "the xHCI the firmware ran is halted" : "the xHCI did NOT halt within 100 ms";
    }
  } else if (h.core(DCTL) & DCTL_RUN_STOP) {
    h.coreWrite(DCTL, h.core(DCTL) & ~DCTL_RUN_STOP);
    stopped = waitCore(h, DSTS, DSTS_DEVCTRLHLT, DSTS_DEVCTRLHLT, 100000) ? "the device controller the firmware ran is halted"
                                                                          : "the device controller did NOT halt within 100 ms";
  }
  if (steps & kStepBcr) {
    h.gccWrite(kGccUsb30Bcr, h.gcc(kGccUsb30Bcr) | 1u);
    h.udelay(20);
    h.gccWrite(kGccUsb30Bcr, h.gcc(kGccUsb30Bcr) & ~1u);
    h.udelay(1000);
    res.reset = true;
    if (!isDwc3(h.core(GSNPSID))) return fail(h, res, 2, "the core does not answer after its block reset");
  }
  res.snpsId = h.core(GSNPSID);
  h.note("cold c2 stop: core %08x, was role %u (GCTL %08x): %s; %s; now GCTL %08x GUSB2PHYCFG %08x GUSB3PIPECTL %08x GSBUSCFG0 %08x "
         "DCTL %08x DSTS %08x", res.snpsId, role, gctl, stopped, res.reset ? "block reset (USB30_PRIM_BCR) done" : "no block reset "
         "(piano-usb-cold-steps bit 3 adds it)", h.core(GCTL), h.core(GUSB2PHYCFG), h.core(GUSB3PIPECTL), h.core(GSBUSCFG0),
         h.core(DCTL), h.core(DSTS));
  return true;
}

// c3: the rails. Soft: a vote that fails or a rail that does not read back on is logged; the start goes on.
template <class H> void stepRails(H &h, uint32_t steps, Result &res, RailRead after[kRails])
{
  h.cold(3);
  RailRead before[kRails];
  int rc[kRails] = {1, 1, 1};   // 1: not sent
  uint16_t mv[kRails] = {};
  for (unsigned i = 0; i < kRails; ++i) {
    readRail(h, i, before[i]);
    if (!(steps & kStepRails)) continue;
    mv[i] = voteMv(kRailTable[i], before[i]);
    rc[i] = h.railVote(i, mv[i], true);
    res.voted[i] = rc[i] == 0;
    if (rc[i] == 0) ++res.votes;
  }
  if (steps & kStepRails) h.udelay(5000);
  for (unsigned i = 0; i < kRails; ++i) {
    readRail(h, i, after[i]);
    const RailRead &b = before[i], &a = after[i];
    if (!(steps & kStepRails)) {
      h.note("cold c3 rail %s (%s): not voted (piano-usb-cold-steps): status %02x enable %02x %u mV: %s", kRailTable[i].name,
             kRailTable[i].what, a.status, a.enable, a.mv, railState(a));
      if (a.off()) res.soft |= 1u << 3;
      continue;
    }
    h.note("cold c3 rail %s (%s, %s): PPID %#x before status %02x enable %02x %u mV (%s); voted %u mV, mode 7, enable: %s; after status "
           "%02x enable %02x mode %02x %u mV: %s", kRailTable[i].name, kRailTable[i].cmdDb, kRailTable[i].what, a.ppid, b.status, b.enable,
           b.mv, railState(b), mv[i], rc[i] == 0 ? "done" : "NOT done", a.status, a.enable, a.mode, a.mv, railState(a));
    if (rc[i] != 0 || a.off()) res.soft |= 1u << 3;
  }
}

// c4: the repeater. Soft: an APID not ours, a failed write or a status that never reads OK.
template <class H> void stepRepeater(H &h, uint32_t steps, Result &res)
{
  using namespace pianohw;
  h.cold(4);
  if (!(steps & kStepRepeater)) {
    h.note("cold c4 repeater: left as it is (piano-usb-cold-steps)");
    return;
  }
  const uint16_t p = kRepeaterPpid;
  const int owner = h.spmiOwner(p);
  if (owner != 0) {
    h.note("cold c4 repeater: %s: nothing written", owner == -2 ? "SPMI is off (piano-usb-spmi=0)" : owner < 0 ? "PPID 0x7fd is not in the "
           "SPMI map" : "its APID belongs to another execution environment");
    res.soft |= 1u << 4;
    return;
  }
  uint8_t st = 0, en = 0, fe = 0, fv = 0, tune[kRptTuneLast - kRptTuneFirst + 1] = {};
  bool read = h.spmiRead(p, kRepeaterStatus, &st) == 0 && h.spmiRead(p, kRepeaterEnable, &en) == 0 && h.spmiRead(p, kRptForceEn5, &fe) == 0 &&
              h.spmiRead(p, kRptForceVal5, &fv) == 0;
  for (unsigned i = 0; i < sizeof tune && read; ++i) read = h.spmiRead(p, uint8_t(kRptTuneFirst + i), &tune[i]) == 0;
  const bool wasOn = read && (st & kRptOk) && (en & kRptEnable);
  bool ok = h.spmiWrite(p, kRepeaterEnable, kRptEnable) == 0;
  const bool tuneNow = !wasOn || (steps & kStepTuneAlways);
  for (unsigned i = 0; i < kRptTuneCount && tuneNow && ok; ++i) ok = h.spmiWrite(p, kRptTune[i].reg, kRptTune[i].value) == 0;
  ok = ok && h.spmiWrite(p, kRptForceEn5, 0) == 0 && h.spmiWrite(p, kRptForceVal5, 0) == 0;   // device mode
  bool ready = false;
  uint8_t v = 0;
  for (unsigned i = 0; i < 100 && ok && !ready; ++i) {
    ready = h.spmiRead(p, kRepeaterStatus, &v) == 0 && (v & kRptOk);
    if (!ready) h.udelay(100);
  }
  uint8_t st2 = 0, en2 = 0, fe2 = 0, fv2 = 0, t[kRptTuneCount] = {};
  (void)(h.spmiRead(p, kRepeaterStatus, &st2) || h.spmiRead(p, kRepeaterEnable, &en2) || h.spmiRead(p, kRptForceEn5, &fe2) ||
         h.spmiRead(p, kRptForceVal5, &fv2));
  for (unsigned i = 0; i < kRptTuneCount; ++i) (void)h.spmiRead(p, kRptTune[i].reg, &t[i]);
  res.rptInit = ok && ready;
  res.tuned = tuneNow && ok;
  h.note("cold c4 repeater: before%s status %02x enable %02x force %02x/%02x tune [51] %02x [52] %02x [53] %02x [54] %02x [57] %02x; "
         "enable, %s, device mode: %s; after status %02x enable %02x force %02x/%02x tune %02x %02x %02x %02x %02x: %s",
         read ? "" : " (NOT all read)", st, en, fe, fv, tune[1], tune[2], tune[3], tune[4], tune[7],
         tuneNow ? "the board's tuning (DT)" : "the tuning kept (it was on)", ok ? "written" : "a write FAILED", st2, en2, fe2, fv2, t[0],
         t[1], t[2], t[3], t[4], res.rptInit ? "ready" : "NOT ready within 10 ms");
  if (!res.rptInit) res.soft |= 1u << 4;
}

// c5: the M31 PHY. Soft: skipped while its vdd reads off (its registers would not answer), or not ready after the init.
template <class H> void stepPhy(H &h, uint32_t steps, const RailRead &vdd, bool voted, Result &res)
{
  using namespace pianohw;
  h.cold(5);
  if (!(steps & kStepPhy)) {
    h.note("cold c5 PHY: left as it is (piano-usb-cold-steps)");
    return;
  }
  if (vdd.off() || (!vdd.read && !voted)) {
    h.note("cold c5 PHY: vreg_l2d_0p88 (its vdd) %s: the M31 is not touched", vdd.off() ? "reads off" : "is neither read nor voted");
    res.soft |= 1u << 5;
    return;
  }
  if (!(h.tcsr() & 1u)) h.tcsrWrite(h.tcsr() | 1u);
  const M31Read before = readM31(h);
  h.gccWrite(kGccQusb2PhyBcr, h.gcc(kGccQusb2PhyBcr) | 1u);
  h.udelay(10);
  h.gccWrite(kGccQusb2PhyBcr, h.gcc(kGccQusb2PhyBcr) & ~1u);
  h.udelay(10);
  for (const RegWrite &w : kM31Init) {
    if (w.offset == kDelay) {
      h.udelay(w.value);
      continue;
    }
    h.m31Write(w.offset, w.value);
  }
  const M31Read after = readM31(h);
  res.phyInit = m31Ready(after);
  h.note("cold c5 PHY (M31: reset, the stock init): before [3c] %02x [50] %02x [54] %02x [64] %02x [94] %02x, after %02x %02x %02x %02x "
         "%02x: %s", before.utmi0, before.por, before.common0, before.hsCtrl2, before.override, after.utmi0, after.por, after.common0,
         after.hsCtrl2, after.override, res.phyInit ? "ready" : "NOT ready");
  if (!res.phyInit) res.soft |= 1u << 5;
}

// The whole cold start (c1..c5). The caller goes on with the takeover only when ok().
template <class H> Result coldStart(H &h, uint32_t steps)
{
  Result res;
  h.note("cold: start (steps %#x: rails %u, repeater %u, M31 PHY %u, block reset %u, RCGs %u, tuning always %u)", steps, steps & kStepRails ? 1 : 0,
         steps & kStepRepeater ? 1 : 0, steps & kStepPhy ? 1 : 0, steps & kStepBcr ? 1 : 0, steps & kStepRcg ? 1 : 0,
         steps & kStepTuneAlways ? 1 : 0);
  if (!stepPower(h, steps, res) || !stepStop(h, steps, res)) return res;
  RailRead after[kRails];
  stepRails(h, steps, res, after);
  stepRepeater(h, steps, res);
  stepPhy(h, steps, after[kRailL2d], res.voted[kRailL2d], res);
  h.cold(6);
  h.note("cold: done: %u rail votes, repeater %s, M31 %s%s; soft failures %#x (bit n: step cn)", res.votes, res.rptInit ? "ready" : "NOT ready",
         res.phyInit ? "ready" : "NOT ready", res.reset ? ", block reset" : "", res.soft);
  return res;
}

// ------------------------------------------------------------------------------------------------------- the test hook
// piano-usb-cold-sim: the hardware left as the normal boot path may leave it, before the decision. Each part only where it is safe:
// the core only while clocked (halted first), the repeater only on an APID of EE 0.
template <class H> void simulate(H &h, uint32_t mask)
{
  using namespace pianohw;
  using namespace dwc3;
  const bool clocked = glueClocked(h);
  h.note("cold sim %#x (piano-usb-cold-sim, a test of the cold start from ABL fastboot):%s%s%s%s%s", mask,
         mask & kSimRepeater ? " [1] the repeater disabled, its host clock forced" : "", mask & kSimPhy ? " [2] the M31 reset and left "
         "uninitialised, its reference clock off" : "", mask & kSimHost ? " [4] the core in the host role, no VBUS/session override" : "",
         mask & kSimClocks ? " [8] the RCGs on TCXO, the branches off, the GDSC collapsed" : "", mask & kSimRails ? " [16] the rails' "
         "DRV-2 votes withdrawn" : "");
  if (clocked && isDwc3(h.core(GSNPSID))) {
    h.coreWrite(GEVNTSIZ, h.core(GEVNTSIZ) | GEVNTSIZ_INTMASK);
    if (h.core(DCTL) & DCTL_RUN_STOP) {
      h.coreWrite(DCTL, h.core(DCTL) & ~DCTL_RUN_STOP);
      waitCore(h, DSTS, DSTS_DEVCTRLHLT, DSTS_DEVCTRLHLT, 100000);
    }
    if (mask & kSimHost) {
      h.coreWrite(GCTL, (h.core(GCTL) & ~GCTL_PRTCAPDIR_MASK) | 1u << 12);
      h.qsWrite(kQsHsPhyCtrl, h.qs(kQsHsPhyCtrl) & ~(kQsUtmiOtgVbusValid | kQsSwSessvldSel));
      h.qsWrite(kQsSsPhyCtrl, h.qs(kQsSsPhyCtrl) & ~kQsLane0PwrPresent);
    }
  } else if (mask & kSimHost) {
    h.note("cold sim: the glue is not clocked: the host role is not set");
  }
  if (mask & kSimPhy) {
    h.gccWrite(kGccQusb2PhyBcr, h.gcc(kGccQusb2PhyBcr) | 1u);
    h.udelay(10);
    h.gccWrite(kGccQusb2PhyBcr, h.gcc(kGccQusb2PhyBcr) & ~1u);
    h.tcsrWrite(h.tcsr() & ~1u);
  }
  if (mask & kSimRepeater) {
    const uint16_t p = kRepeaterPpid;
    if (h.spmiOwner(p) == 0) {
      (void)(h.spmiWrite(p, kRepeaterEnable, 0) || h.spmiWrite(p, kRptForceEn5, kRptClk19p2) || h.spmiWrite(p, kRptForceVal5, kRptClk19p2));
    } else {
      h.note("cold sim: the repeater's APID is not EE 0's (or SPMI is off): left as it is");
    }
  }
  if (mask & kSimClocks) {
    const uint32_t rcgs[2] = {kGccUsb30MasterRcg, kGccUsb30MockUtmiRcg};
    for (uint32_t cmd : rcgs) {   // parked on TCXO / 1, as a disabled shared RCG is
      h.gccWrite(cmd, h.gcc(cmd) | kRcgRootEn);
      h.gccWrite(cmd + 4, (h.gcc(cmd + 4) & ~kRcgCfgMask) | 0x001u);
      h.gccWrite(cmd, h.gcc(cmd) | kRcgUpdate);
      waitGcc(h, cmd, kRcgUpdate, 0, 1000);
      h.gccWrite(cmd, h.gcc(cmd) & ~(kRcgRootEn | kRcgUpdate));
    }
    for (unsigned i = sizeof kBranches / sizeof kBranches[0]; i-- > 0;) h.gccWrite(kBranches[i], h.gcc(kBranches[i]) & ~kCbcrClkEnable);
    h.gccWrite(kGccUsb30Gdsc, h.gcc(kGccUsb30Gdsc) | kGdscSwCollapse);
    waitGcc(h, kGccUsb30Gdsc, kGdscPwrOn, 0, 10000);
  }
  if (mask & kSimRails)
    for (unsigned i = kRails; i-- > 0;) (void)h.railVote(i, 0, false);
  h.note("cold sim: done: GDSC %08x master %08x RCG cfg master %08x; usb2 clkref %08x", h.gcc(kGccUsb30Gdsc), h.gcc(kGccUsb30Master),
         h.gcc(kGccUsb30MasterRcg + 4), h.tcsr());
}

// ------------------------------------------------------------------------------------------------------------- the link watch
// After RUN_STOP: no bus reset from a host for waitMs: a cold start (the inherited state looked right but was not, or the last cold
// start left something off). The wait doubles after each one; at most kColdTries cold starts per boot (counted by the caller in
// colds). A host seen once ends the watch for the boot.
enum class WatchDo : uint8_t { nothing, coldStart, giveUp };
struct Watch {
  uint32_t waitMs = 0;   // 0: no watch
  unsigned colds = 0;
  bool armed = false, hostSeen = false;
  uint64_t sinceMs = 0;
  void arm(uint32_t ms, uint64_t nowMs)
  {
    waitMs = ms;
    armed = ms != 0 && !hostSeen;
    sinceMs = nowMs;
  }
  void restarted(uint64_t nowMs) { sinceMs = nowMs; }
  void disarm() { armed = false; }
  WatchDo step(uint64_t nowMs, bool host)
  {
    if (host) {
      hostSeen = true;
      armed = false;
      return WatchDo::nothing;
    }
    if (!armed || nowMs - sinceMs < waitMs) return WatchDo::nothing;
    if (colds >= kColdTries) {
      armed = false;
      return WatchDo::giveUp;
    }
    waitMs = waitMs * 2 > kWatchMaxMs ? kWatchMaxMs : waitMs * 2;
    sinceMs = nowMs;
    return WatchDo::coldStart;
  }
};

}  // namespace cold
}  // namespace pianousb
