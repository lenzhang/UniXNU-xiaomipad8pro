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
//   c2 stop   whatever runs stopped, the event interrupt masked, and the core left in the device role (toDevice()): a running
//             device controller halted; in the host role the xHCI halted (USBCMD.RS 0, HCHalted awaited, bounded), then the
//             programming model's switch (PRTCAPDIR device, then the device side's soft reset in the device role), taken as done only
//             when shown; otherwise the controller's block reset (USB30_PRIM_BCR), as Linux's glue does at its probe. The block
//             reset is also taken when c1 found the GDSC collapsed (the core then holds only its retention flops) and with the BCR
//             step (opt-in). An xHCI that does not halt fails c2: no block reset under a running DMA master.
//   c3 rails  vreg_l2d_0p88 (the M31's vdd), vreg_l15b_1p8 and vreg_l5b_3p1 (the repeater's vdd18 and vdd3) voted on through
//             RPMh (apps DRV-2 under PianoA830's DRV-2 lock word; mode, set point, enable), read back over SPMI. vreg_l3g_1p2 (the
//             M31's vdda12) is never voted: it is also the UFS PHY's PLL rail (on whenever UFS runs).
//   c4 repeater  enabled, the board's tuning (piano's DT) when it was found off, device mode (no forced 19.2 MHz host clock),
//             status OK awaited (only when its APID belongs to EE 0, else nothing is written).
//   c5 PHY    the M31 eUSB2 PHY reset (GCC QUSB2PHY_PRIM_BCR) and initialised from PianoUsbHw.h's kM31Init (the stock firmware's
//             sequence; its end state is what the fastboot path reads), read back.
// The takeover then goes on as before: halt, DMA, the UTMI clock as PIPE clock, core soft reset, device mode (high speed; the role
// is already the device one), the QSCRATCH VBUS/session override, RUN_STOP. Its gate, ensureDevice(), runs before its first device
// register in every mode (piano-usb-cold=0 and the hand-off's reclaim too): a core that does not read the device role gets
// toDevice() there, and the takeover stops when it still does not.
//
// The role rule (C6, docs/forensics-2026-11-11.md section 7.6): no device register (DCFG..the endpoint commands, dwc3::isDeviceReg)
// is read or written while GCTL reads another role, and no xHCI register outside the host role. The Gadget (Dwc3Gadget.h) refuses
// the same, and the kernel's register access refuses it too (PianoDwc3.cpp's coreRd/coreWr).
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
#include "TxFifoPlan.h"

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
  kStepBcr = 1u << 3,          // c2: the controller's block reset always (opt-in: it also drops what the firmware set in the globals;
                               // without it c2 still takes it for a host role it cannot prove switched, or a collapsed GDSC)
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
inline const char *roleName(uint32_t role) { return role == 2 ? "device" : role == 1 ? "host" : role == 3 ? "OTG" : "none"; }
constexpr uint32_t kBranches[] = {pianohw::kGccCfgNocUsb3Axi, pianohw::kGccAggreUsb3Axi, pianohw::kGccUsb30Master,
                                  pianohw::kGccUsb30Sleep, pianohw::kGccUsb30MockUtmi};

// ------------------------------------------------------------------------------------------------------- the inherited state
struct Inherited {
  uint32_t gdsc = 0, cfgNoc = 0, aggre = 0, master = 0, sleep = 0, mockUtmi = 0, masterCfg = 0, mockCfg = 0, gpll0 = 0,
           gpllVote = 0, clkref = 0;
  bool clocked = false;                          // the GDSC and the five branches on: the core, QSCRATCH and the M31 answer
  bool coreRead = false;                         // the core answered as a DWC_usb3x (only read when clocked)
  uint32_t snpsId = 0, gctl = 0, gsts = 0, gevntLo = 0, gevntHi = 0, devten = 0, dctl = 0, dsts = 0, dcfg = 0;
  bool devRead = false;                          // the device registers read (only in the device role)
  bool xhciRead = false;                         // the xHCI's USBCMD/USBSTS read (only in the host role, CAPLENGTH sane)
  uint32_t usbcmd = 0, usbsts = 0;
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
  if (s.devRead && !s.gevntLo && !s.gevntHi && !s.devten) r |= kWhyNeverRan;
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
      s.gsts = h.core(GSTS);
      s.gevntLo = h.core(GEVNTADRLO);
      s.gevntHi = h.core(GEVNTADRHI);
      if (roleOf(s.gctl) == ROLE_DEVICE) {   // the role rule: the device registers only in the device role
        s.devRead = true;
        s.devten = h.core(DEVTEN);
        s.dctl = h.core(DCTL);
        s.dsts = h.core(DSTS);
        s.dcfg = h.core(DCFG);
      } else if (roleOf(s.gctl) == ROLE_HOST) {   // the xHCI's only in the host role
        const uint32_t cap = h.core(0) & 0xffu;
        if (cap >= 0x20 && !(cap & 3u)) {
          s.xhciRead = true;
          s.usbcmd = h.core(cap);
          s.usbsts = h.core(cap + 4);
        }
      }
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
  const char *m31 = !s.phyRead ? "not read (its reference clock off or its vdd off)" : m31Ready(s.phy) ? "ready" : "NOT initialised";
  if (s.clocked && s.devRead)
    h.note("cold: inherited core %08x GCTL %08x (role %u: %s) GEVNTADR %08x%08x DEVTEN %08x DCTL %08x DSTS %08x DCFG %08x; M31 %s "
           "[3c] %02x [50] %02x [54] %02x [64] %02x [94] %02x",
           s.snpsId, s.gctl, s.role(), roleName(s.role()), s.gevntHi, s.gevntLo, s.devten, s.dctl, s.dsts, s.dcfg, m31, s.phy.utmi0,
           s.phy.por, s.phy.common0, s.phy.hsCtrl2, s.phy.override);
  else if (s.clocked)
    h.note("cold: inherited core %08x GCTL %08x (role %u: %s) GSTS %08x GEVNTADR %08x%08x; the device registers are not read (not the "
           "device role); xHCI %s USBCMD %08x USBSTS %08x; M31 %s [3c] %02x [50] %02x [54] %02x [64] %02x [94] %02x",
           s.snpsId, s.gctl, s.role(), roleName(s.role()), s.gsts, s.gevntHi, s.gevntLo, !s.xhciRead ? "not read:" :
           (s.usbcmd & 1u) ? "RUNNING:" : !(s.usbsts & 1u) ? "NOT halted (RS 0, a stop not yet done):" : "halted:", s.usbcmd, s.usbsts,
           m31, s.phy.utmi0, s.phy.por,
           s.phy.common0, s.phy.hsCtrl2, s.phy.override);
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

// ------------------------------------------------------------------------------------------------------ the role: host -> device
// The DWC3 programming model's way from the host role to the device role, as Linux's role switch describes the databook's sequence
// (drivers/usb/dwc3/core.c, GPL-2.0, read as the specification only; the same order as PianoUsbHost's XhciHost.h): the xHCI halted
// (USBCMD.RS 0, USBSTS.HCHalted), GCTL.CORESOFTRESET where the core needs it (DWC_usb3, DWC_usb31 before 1.90a; piano's DWC_usb31
// 2.00a does not), PRTCAPDIR = device, then the device side's soft reset (DCTL.CSFTRST) in the device role, then the device init.
//
// C6 (2026-11-12, sim 31) did it the other way round: the takeover's soft reset with GCTL still in the host role, PRTCAPDIR written
// afterwards by the gadget's init. Every step reported done; the TX FIFOs kept the host role's sizes (FIFOs 6 and 7 without depth,
// TXFRAMNUM set) and the whole system stopped within the bounded stage 19 (STARTTRANSFER, RUN_STOP).
//
// Here nothing of the device side is touched before GCTL reads the device role, and the soft switch counts as done only when it is
// shown (proven): PRTCAPDIR and GSTS.CURMOD read device, DCTL.CSFTRST cleared, DSTS.DEVCTRLHLT set, and TX FIFOs 0-6 a device
// layout (deviceFifos()). Otherwise, and for an xHCI whose registers do not answer or a role that is neither (0, OTG), the
// controller's block reset (USB30_PRIM_BCR; C7: it leaves GCTL 00102004, the device role, and the device FIFO layout). An xHCI that
// does not halt is never reset under: nothing more is written and the step fails (no block reset of a running DMA master).
constexpr uint32_t kUsbCmdRun = 1u << 0, kUsbCmdInte = 1u << 2, kUsbStsHalted = 1u << 0;   // xHCI USBCMD / USBSTS (operational +0, +4)
constexpr uint32_t kXhciHaltUs = 100000;                       // RS 0 -> HCHalted (the specification allows 16 ms; PianoUsbHost's bound)
constexpr uint32_t kIpDwc3 = 0x5533, kIpDwc31 = 0x3331, kDwc31Version190a = 0x3139302a;   // GSNPSID[31:16]; GVERSION "190*"
inline bool needsGctlSoftReset(uint32_t snpsId, uint32_t gversion)
{
  const uint32_t ip = snpsId >> 16;
  return ip == kIpDwc3 || (ip == kIpDwc31 && gversion < kDwc31Version190a);
}
// The TX FIFOs EP0 and the three ports' six IN endpoints use (IN endpoint n sends from FIFO n). A device layout: each with a depth,
// one after the other from word 0, inside the TX FIFO RAM, TXFRAMNUM (bit 15 on DWC_usb31/32) clear; the RX FIFO with a depth. The
// device role's reset values on piano pass (C7: 00000043 0043030d 03500083 03d3030d 06e00083 07630105 08680083, RX 00000313), the
// host role's fail (C6: 00000025 00258082 ... 07840000, RX 00000028). Returns the first FIFO that fails, kFifoRx for the RX FIFO,
// or kFifosOk.
constexpr unsigned kDeviceFifos = 7, kFifosOk = 0xff, kFifoRx = 0x10;
inline unsigned deviceFifos(const uint32_t tx[kDeviceFifos], uint32_t rx, uint32_t ramWords, bool dwc31)
{
  const uint32_t depthMask = dwc31 ? 0x7fffu : 0xffffu;
  uint32_t at = 0;
  for (unsigned n = 0; n < kDeviceFifos; ++n) {
    const uint32_t start = tx[n] >> 16, depth = tx[n] & depthMask;
    if (!depth || start != at || start + depth > ramWords || (dwc31 && (tx[n] & 0x8000u))) return n;
    at = start + depth;
  }
  return (rx & 0xffffu) ? kFifosOk : kFifoRx;
}

// What the role switch found and did (c2's, and the takeover's gate's).
struct RoleSwitch {
  uint32_t found = dwc3::ROLE_DEVICE, gctl = 0;   // the role found, GCTL then
  int xhci = 0;                 // the host role's xHCI: 0 not looked at, 1 halted already, 2 running: halted now, -1 its registers
                                // do not answer (CAPLENGTH), -2 it does not report halted within kXhciHaltUs. HCHalted is 1 by
                                // default and 0 only while the xHCI still executes (RS 1, or a stop not yet done): -2 even when RS
                                // already reads 0 (a retry after c2's own clear), and nothing is reset under it
  uint32_t usbcmd = 0, usbsts = 0, haltUs = 0;
  bool soft = false;            // the programming model's switch tried
  bool gctlReset = false, curmod = false, csftrst = false, halted = false;
  uint32_t csftrstUs = 0;
  struct Fifos {                // the FIFO sizes read back
    bool read = false;
    unsigned fail = kFifosOk;   // deviceFifos() of them
    uint32_t tx[kDeviceFifos] = {}, rx = 0;
  } afterSoft, afterBcr;
  bool proven = false;          // the soft switch shown done
  const char *unproven = nullptr;
  bool blockReset = false;
  const char *bcrWhy = nullptr;
  uint32_t now = 0;             // GCTL at the end
  bool ok = false;              // GCTL reads the device role at the end
  const char *why = nullptr;    // !ok: what stopped it
};

template <class H> void readFifos(H &h, RoleSwitch::Fifos &f)
{
  using namespace dwc3;
  for (unsigned n = 0; n < kDeviceFifos; ++n) f.tx[n] = h.core(GTXFIFOSIZ0 + 4 * n);
  f.rx = h.core(GRXFIFOSIZ0);
  f.fail = deviceFifos(f.tx, f.rx, txFifoRamWords(h.core(GHWPARAMS1), h.core(GHWPARAMS6), h.core(GHWPARAMS7)),
                       (h.core(GSNPSID) >> 16) != kIpDwc3);
  f.read = true;
}

// The controller's block reset (GCC USB30_PRIM_BCR: 20 us, then 1 ms settling). True when the core answers after it.
template <class H> bool blockReset(H &h)
{
  using namespace pianohw;
  h.gccWrite(kGccUsb30Bcr, h.gcc(kGccUsb30Bcr) | 1u);
  h.udelay(20);
  h.gccWrite(kGccUsb30Bcr, h.gcc(kGccUsb30Bcr) & ~1u);
  h.udelay(1000);
  return isDwc3(h.core(dwc3::GSNPSID));
}

// The host role only (the xHCI's registers answer there): a running xHCI (RS set, or HCHalted clear) stopped, its interrupt enable
// off, HCHalted awaited (bounded).
template <class H> void xhciStop(H &h, RoleSwitch &rs)
{
  const uint32_t cap = h.core(0) & 0xffu;   // CAPLENGTH: where the operational registers start
  if (cap < 0x20 || (cap & 3u)) {
    rs.xhci = -1;
    rs.usbcmd = cap;
    return;
  }
  rs.usbcmd = h.core(cap);
  rs.usbsts = h.core(cap + 4);
  if (!(rs.usbcmd & kUsbCmdRun) && (rs.usbsts & kUsbStsHalted)) {
    rs.xhci = 1;
    return;
  }
  h.coreWrite(cap, rs.usbcmd & ~(kUsbCmdRun | kUsbCmdInte));
  for (rs.haltUs = 0; rs.haltUs < kXhciHaltUs && !(h.core(cap + 4) & kUsbStsHalted); rs.haltUs += 100) h.udelay(100);
  rs.usbsts = h.core(cap + 4);
  rs.xhci = (rs.usbsts & kUsbStsHalted) ? 2 : -2;
}

// The programming model's switch, the xHCI halted: the PHYs awake, GCTL.CORESOFTRESET where needed, PRTCAPDIR device, CURMOD
// awaited, and only then the device side's soft reset; the proof read back.
template <class H> void softSwitch(H &h, RoleSwitch &rs)
{
  using namespace dwc3;
  rs.soft = true;
  // Both PHY interfaces awake: a soft reset needs their clocks, and Linux notes a GCTL soft reset with GUSB3PIPECTL.SUSPHY set can
  // leave a Qualcomm core in a bad state.
  h.coreWrite(GUSB2PHYCFG, h.core(GUSB2PHYCFG) & ~(GUSB2PHYCFG_SUSPHY | GUSB2PHYCFG_ENBLSLPM));
  h.coreWrite(GUSB3PIPECTL, h.core(GUSB3PIPECTL) & ~GUSB3PIPECTL_SUSPHY);
  if (needsGctlSoftReset(h.core(GSNPSID), h.core(GVERSION))) {
    h.coreWrite(GCTL, h.core(GCTL) | GCTL_CORESOFTRESET);
    h.udelay(100000);   // the internal clocks' synchronisation (Linux: 100 ms)
    h.coreWrite(GCTL, h.core(GCTL) & ~GCTL_CORESOFTRESET);
    rs.gctlReset = true;
  }
  h.coreWrite(GCTL, (h.core(GCTL) & ~GCTL_PRTCAPDIR_MASK) | GCTL_PRTCAP_DEVICE);
  rs.curmod = waitCore(h, GSTS, GSTS_CURMOD_MASK, GSTS_CURMOD_DEVICE, 10000);
  if (roleOf(h.core(GCTL)) != ROLE_DEVICE) {
    rs.unproven = "GCTL.PRTCAPDIR does not read the device role";
    return;
  }
  if (!rs.curmod) {
    rs.unproven = "GSTS.CURMOD does not read device within 10 ms";
    return;
  }
  h.coreWrite(DCTL, (h.core(DCTL) & ~(DCTL_RUN_STOP | DCTL_ULSTCHNGREQ_MASK)) | DCTL_CSFTRST);
  for (rs.csftrstUs = 0; rs.csftrstUs < 250000 && (h.core(DCTL) & DCTL_CSFTRST); rs.csftrstUs += 1000) h.udelay(1000);
  rs.csftrst = !(h.core(DCTL) & DCTL_CSFTRST);
  if (!rs.csftrst) {
    rs.unproven = "DCTL.CSFTRST does not clear within 250 ms";
    return;
  }
  h.udelay(50000);   // the PHY domain's synchronisation after a DWC_usb31/32 soft reset (as the takeover's)
  rs.halted = (h.core(DSTS) & DSTS_DEVCTRLHLT) != 0;
  readFifos(h, rs.afterSoft);
  if (!rs.halted) rs.unproven = "DSTS.DEVCTRLHLT is not set after the soft reset";
  else if (rs.afterSoft.fail != kFifosOk) rs.unproven = "the FIFO sizes are not a device layout (the host role's stayed)";
  rs.proven = rs.unproven == nullptr;
}

// The core into the device role. forceBcr: the block reset whatever the role (its reason), else only when the role switch needs it.
// True when GCTL reads the device role at the end. A core found in the device role without forceBcr: nothing is done.
template <class H> bool toDevice(H &h, RoleSwitch &rs, const char *forceBcr)
{
  using namespace dwc3;
  rs.gctl = h.core(GCTL);
  rs.found = roleOf(rs.gctl);
  if (rs.found == ROLE_HOST) {
    xhciStop(h, rs);
    if (rs.xhci == -2) {
      rs.now = h.core(GCTL);
      rs.why = "the xHCI does not halt within 100 ms: nothing more is written (no block reset under a running DMA master)";
      return false;
    }
  }
  const char *bcr = forceBcr;
  if (!bcr && rs.found == ROLE_HOST && rs.xhci == -1) bcr = "the xHCI's registers do not answer in the host role";
  if (!bcr && rs.found != ROLE_HOST && rs.found != ROLE_DEVICE) bcr = "the core is in neither the host nor the device role";
  if (!bcr && rs.found == ROLE_HOST) {
    softSwitch(h, rs);
    if (!rs.proven) bcr = "the soft switch is not shown to have cleared the host state";
  }
  if (bcr) {
    rs.bcrWhy = bcr;
    rs.blockReset = true;
    if (!blockReset(h)) {
      rs.why = "the core does not answer after its block reset";
      return false;
    }
    if (roleOf(h.core(GCTL)) != ROLE_DEVICE) {   // piano's reset value is the device role (C7: GCTL 00102004)
      h.coreWrite(GCTL, (h.core(GCTL) & ~GCTL_PRTCAPDIR_MASK) | GCTL_PRTCAP_DEVICE);
      rs.curmod = waitCore(h, GSTS, GSTS_CURMOD_MASK, GSTS_CURMOD_DEVICE, 10000);
    }
    readFifos(h, rs.afterBcr);   // reported only: the reset state is the hardware's own (the takeover's soft reset follows)
  }
  rs.now = h.core(GCTL);
  rs.ok = roleOf(rs.now) == ROLE_DEVICE;
  if (!rs.ok) rs.why = "GCTL.PRTCAPDIR does not read the device role";
  return rs.ok;
}

// The role switch's lines (nothing when the core was in the device role and nothing was done).
template <class H> void noteRole(H &h, const char *who, const RoleSwitch &rs)
{
  if (rs.found == dwc3::ROLE_DEVICE && !rs.blockReset) return;
  const char *xhci = rs.xhci == 0 ? "not looked at (not the host role)" : rs.xhci == 1 ? "halted already" : rs.xhci == 2 ?
                     "RUNNING: stopped, HCHalted" : rs.xhci == -1 ? "its registers do NOT answer (CAPLENGTH in USBCMD's place)" :
                     "did NOT report halted within 100 ms";
  h.note("%s role: found role %u (%s, GCTL %08x); xHCI %s (USBCMD %08x USBSTS %08x after %u us); soft switch %s%s (GCTL.CORESOFTRESET "
         "%u, CURMOD device %u, CSFTRST cleared %u after %u us, halted %u); block reset %s; now GCTL %08x (role %u: %s)", who, rs.found,
         roleName(rs.found), rs.gctl, xhci, rs.usbcmd, rs.usbsts, rs.haltUs, !rs.soft ? "not tried" : rs.proven ? "PROVEN" : "NOT proven: ",
         rs.soft && !rs.proven && rs.unproven ? rs.unproven : "", rs.gctlReset ? 1 : 0, rs.curmod ? 1 : 0, rs.csftrst ? 1 : 0,
         rs.csftrstUs, rs.halted ? 1 : 0, rs.blockReset ? rs.bcrWhy : "not taken", rs.now, dwc3::roleOf(rs.now),
         roleName(dwc3::roleOf(rs.now)));
  const RoleSwitch::Fifos *const reads[2] = {&rs.afterSoft, &rs.afterBcr};
  for (const RoleSwitch::Fifos *f : reads) {
    if (!f->read) continue;
    const char *what = f == &rs.afterSoft ? "after the soft switch" : "after the block reset";
    if (f->fail == kFifosOk || f->fail == kFifoRx)
      h.note("%s role: TX FIFOs 0-6 %s %08x %08x %08x %08x %08x %08x %08x RX %08x: %s", who, what, f->tx[0], f->tx[1], f->tx[2],
             f->tx[3], f->tx[4], f->tx[5], f->tx[6], f->rx, f->fail == kFifosOk ? "a device layout" : "NOT a device layout (the RX FIFO "
             "has no depth)");
    else
      h.note("%s role: TX FIFOs 0-6 %s %08x %08x %08x %08x %08x %08x %08x RX %08x: NOT a device layout (FIFO %u fails)", who, what,
             f->tx[0], f->tx[1], f->tx[2], f->tx[3], f->tx[4], f->tx[5], f->tx[6], f->rx, f->fail);
  }
}

// The takeover's gate (PianoDwc3.cpp, before its first device register, in every mode and after a hand-off's reclaim): true when
// GCTL reads the device role, after toDevice() when it did not. False: the takeover stops; no device register was touched.
template <class H> bool ensureDevice(H &h, RoleSwitch &rs)
{
  rs = RoleSwitch();
  rs.gctl = rs.now = h.core(dwc3::GCTL);
  if (dwc3::roleOf(rs.gctl) == dwc3::ROLE_DEVICE) return rs.ok = true;
  const bool ok = toDevice(h, rs, nullptr);
  noteRole(h, "takeover:", rs);
  return ok;
}

// ---------------------------------------------------------------------------------------------------------------- the steps
struct Result {
  int failed = 0;         // 0, or the step (1, 2) whose hard part failed
  uint32_t soft = 0;      // 1 << step for each step that failed softly (logged; the start went on)
  unsigned votes = 0;     // rails voted
  bool voted[kRails] = {false, false, false};
  bool reset = false, rptInit = false, tuned = false, phyInit = false;
  bool collapsed = false; // c1 found the USB30 GDSC collapsed (c2 then takes the block reset)
  RoleSwitch role;        // c2's role switch
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
  res.collapsed = !(g & kGdscPwrOn);
  if (res.collapsed) {
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

// c2: stop what runs and leave the core in the device role (toDevice()). Hard: the core must answer (before and after a block
// reset), an xHCI that runs must halt, and GCTL must read the device role at the end. The block reset: with kStepBcr, when c1 found
// the GDSC collapsed (the core powered up with only its retention flops: C6 read GCTL, GUCTL1, DCFG and DEVTEN kept, GEVNTADR and
// the FIFO sizes not), or when the role switch needs it.
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
  const uint32_t gctl = h.core(GCTL), role = roleOf(gctl);
  const char *stopped = "nothing ran";
  if (role == ROLE_DEVICE && (h.core(DCTL) & DCTL_RUN_STOP)) {   // the device registers only in the device role
    h.coreWrite(DCTL, h.core(DCTL) & ~DCTL_RUN_STOP);
    stopped = waitCore(h, DSTS, DSTS_DEVCTRLHLT, DSTS_DEVCTRLHLT, 100000) ? "the device controller the firmware ran is halted"
                                                                          : "the device controller did NOT halt within 100 ms";
  }
  const char *force = (steps & kStepBcr) ? "asked for (piano-usb-cold-steps bit 3)" : res.collapsed ?
                      "c1 found the GDSC collapsed (the core kept only its retention flops)" : nullptr;
  RoleSwitch &rs = res.role;
  const bool ok = toDevice(h, rs, force);
  if (role == ROLE_HOST)
    stopped = rs.xhci == 2 ? "the xHCI the firmware ran is halted" : rs.xhci == -2 ? "the xHCI did NOT halt within 100 ms" :
              rs.xhci == -1 ? "the xHCI's registers do not answer" : "nothing ran (the xHCI was halted)";
  res.reset = rs.blockReset;
  noteRole(h, "cold c2", rs);
  if (!ok) return fail(h, res, 2, rs.why);
  res.snpsId = h.core(GSNPSID);
  const uint32_t now = h.core(GCTL), dctl = h.core(DCTL), dsts = h.core(DSTS);   // the device role now (toDevice)
  h.note("cold c2 stop: core %08x, was role %u (GCTL %08x): %s; %s; now GCTL %08x (role %u) GUSB2PHYCFG %08x GUSB3PIPECTL %08x "
         "GSBUSCFG0 %08x DCTL %08x DSTS %08x", res.snpsId, role, gctl, stopped, res.reset ? "block reset (USB30_PRIM_BCR) done" :
         "no block reset", now, roleOf(now), h.core(GUSB2PHYCFG), h.core(GUSB3PIPECTL), h.core(GSBUSCFG0), dctl, dsts);
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
    if (roleOf(h.core(GCTL)) == ROLE_DEVICE && (h.core(DCTL) & DCTL_RUN_STOP)) {   // the role rule
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
