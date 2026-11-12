// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera level 2 (piano-camera=2): one sensor powered as its Linux driver powers it, its chip ID read over its bus, and the
// sensor powered down again; then the other one. Portable over an ops class O (the kernel's in PianoCamera.cpp; the host tests'
// board model in tests/camera_host.cpp), with:
//   uint32_t rd(uint64_t pa);                   void wr(uint64_t pa, uint32_t v, const char *what);   (logged)
//   void wrQuiet(uint64_t pa, uint32_t v);      (counted, not logged: the software I2C's line changes)
//   void udelay(unsigned us);                   void msleep(unsigned ms);
//   int vrm(unsigned rail, const VrmCmd *cmds, unsigned n);   one RPMh VRM transaction for kRails[rail] under the DRV-2 lock
//   int pmic(unsigned rail, RailState &r);      the rail's PMIC state again over the SPMI observer (reads only)
//   bool gccLock(bool take);                    PianoA830's GCC vote lock word around every GCC write
//   int mmRails(bool on);                       CAMCC's domains through the shared multimedia votes (PianoMmVotes.h: MXC and MMCX
//                                               at low_svs, MMCX with the display's floor; off: this client's needs withdrawn)
//   void step(int n, const char *what);         void note(const char *fmt, ...);
//
// Every store goes through put(), which refuses an address outside this level's allowlist (writable(): the camera branches of GCC
// Linux keeps on, CAMCC's TITAN_TOP GDSC with its four critical branches, the top AHB and CCI0 branches and their two clock sources,
// the two MCLKs' clock sources and branches and the MCLK PLL, the ten camera pins, CCI0's registers). A refused store fails the step.
// Each change is recorded (Undo) and powerDown() takes back exactly those, in the reverse of the drivers' order; it always runs, after
// success, a failure, or the step piano-camera-stop names. No sensor register is written: the chip ID read writes only the register
// address before the read.
//
// Rear (S5KJN1, CCI0 master 1 or software I2C on gpio115/116), steps:
//   201 on the CAMCC path (the rear on CCI0, or level 3): CAMCC's domains voted (MXC, MMCX: the shared helper), GCC's camera AHB and
//       camera XO; always the MCLK controller's AHB; each where off (a hardware-gated branch counts on once enabled: Linux's rule;
//       the c1 survey found all three off)                                     202 the survey again (the controllers readable now)
//   203 refusals (CameraSurvey.h sensorRefusal)
//   205 the bus, first (as Linux has it up from its probe): CAMCC's critical branches, TITAN_TOP, the top AHB from the XO, CCI0's
//       clock from the XO (19.2 MHz), its pins, the controller reset and timed (400 kHz, or 100 with piano-camera-cci-khz=100); or the
//       software I2C's pins (GPIO, released)
//   210 pins: reset low (asserted), the supply enable low, the MCLK pin's function
//   211 vddd (L1I 1096 mV) on and read back; 2 ms                               212 vdda (gpio175 high; 100 us)
//   213 vddio (L6B 1800 mV), afvdd (L7B 2800 mV) on and read back               214 MCLK1: XO 19.2 MHz (or 24 MHz from PLL0
//       with piano-camera-mclk24=1), its branch on                              215 reset released (gpio6 high); 15 ms
//   230 chip ID 0x0000 == 0x38e1 (three tries, 10 ms apart)                     240 power down (Undo, reverse order)
// Front (OV32D40, software I2C on gpio4/5): 201 (MCLK controller's AHB), 202, 203, 205 the software I2C's pins, 210 reset low, MCLK
//   pin; 213 vddio (L10B 1800), dvdd (L2G 1200) on and read back, avdd (gpio176 high); 6 ms; 214 MCLK4 XO 19.2 MHz, branch; 12 ms;
//   215 reset released; 25 ms; 230 chip ID 0x300a == 0x5632 (five tries, 12 ms apart); 240.
#pragma once
#include <stdint.h>
#include "BitI2c.h"
#include "CameraHw.h"
#include "CameraSurvey.h"
#include "CciMaster.h"

namespace pianocamera {

struct VrmCmd {
  uint32_t reg, value;
};

struct PowerOptions {
  bool rearCci = true;       // piano-camera-rear-bus=0 (default): CCI0; 1: software I2C on gpio115/116
  bool mclk24 = false;       // piano-camera-mclk24=1: the rear's MCLK at 24 MHz from the MCLK controller's PLL0 (960 MHz / 10 x 1/4)
  unsigned cciKhz = 400;     // 100 or 400 (CCI clock from the XO)
  unsigned bitHalfUs = 5;    // the software I2C's half bit (5: about 80-100 kHz)
  bool busPullUp = false;    // piano-camera-pull=1: the pads' pull-ups on the software I2C lines (the DT has external ones)
  bool blind = false;        // vote a rail whose PMIC state could not be read
  unsigned holdMs = 0;       // keep the sensor powered this long after the ID (piano-camera-hold)
  int stopAt = 0;            // piano-camera-stop: power down after this step
  // Before the first CAMCC access (the rear on CCI0, or `camss`): CAMCC's power domains voted through the shared multimedia-vote
  // helper (ops.mmRails: MXC and MMCX at low_svs, MMCX with the display's floor), as Linux has them up before it touches CAMCC (c1,
  // 2026-11-12: GCC's camera AHB was off, so CAMCC's first access is this level's); withdrawn after the power-down.
  // piano-camera-votes=0 leaves them out.
  bool votes = true;
  // Level 3: TITAN_TOP, the camera AHB and GCC's camera AHB/XO for either sensor and bus (CSIPHY, CSID and TFE are under it).
  bool camss = false;
};

enum : int {
  kStepGcc = 201, kStepResurvey = 202, kStepRefusals = 203, kStepBus = 205, kStepPins = 210, kStepRailFirst = 211, kStepAvdd = 212,
  kStepRails = 213, kStepMclk = 214, kStepReset = 215, kStepId = 230, kStepDown = 240,
};

enum class Failure : uint8_t {
  none, refused, allowlist, gcc, rail, railVoltage, gdsc, branch, rcg, pll, cci, bus, idMismatch, idRead, stopped, votes,
};
inline const char *failureName(Failure f)
{
  switch (f) {
  case Failure::none: return "none";
  case Failure::refused: return "refused by the survey";
  case Failure::allowlist: return "a store outside the allowlist";
  case Failure::gcc: return "a GCC branch did not run";
  case Failure::rail: return "a rail vote failed or did not read back enabled and ready";
  case Failure::railVoltage: return "a rail read back at another voltage";
  case Failure::gdsc: return "TITAN_TOP did not power up";
  case Failure::branch: return "a clock branch did not run";
  case Failure::rcg: return "a clock source did not update";
  case Failure::pll: return "the MCLK PLL did not lock";
  case Failure::cci: return "the CCI controller did not reset";
  case Failure::bus: return "the bus is held low";
  case Failure::idMismatch: return "the chip ID is not the expected one";
  case Failure::idRead: return "the chip ID read failed";
  case Failure::stopped: return "stopped (piano-camera-stop)";
  case Failure::votes: return "the shared multimedia votes (MXC/MMCX) were not sent";
  }
  return "?";
}

struct RailRecord {
  bool voted = false, ours = false;
  RailState found, on, off;
};

struct SensorResult {
  bool attempted = false, idOk = false, cleanOff = false;
  Sensor which = Sensor::rear;
  const char *bus = "";
  uint32_t mclkHz = 0;
  uint16_t id = 0;
  unsigned tries = 0;
  int failedStep = 0;
  Failure failure = Failure::none;
  const char *why = nullptr;   // a refusal's reason, or the bus's last error
  CciResult cci = CciResult::ok;
  I2c i2c = I2c::ok;
  uint32_t cciVersion = 0, lastValue = 0;
  RailRecord rails[3];
  unsigned writes = 0, quietWrites = 0, refusedStores = 0, offWarnings = 0;
  bool voted = false;          // CAMCC's domains voted (and withdrawn at the power-down)
  unsigned sensorWrites = 0, sensorWriteFailures = 0;   // level 3's register writes over the sensor's bus
};

// What was changed, so that powerDown() takes back exactly that.
struct Undo {
  bool gcc[3] = {};                   // camera AHB, camera XO, MCLK AHB enabled by us
  bool ccCritical[4] = {};
  bool titan = false;
  uint32_t titanFound = 0;
  bool topAhb = false, cci0 = false;
  bool slowAhbRcg = false, cci0Rcg = false;
  uint32_t slowAhbCfg = 0, cci0Words[4] = {};   // CFG, M, N, D as found
  bool mclkRcg = false, mclkBranch = false, pll = false, pllConfigured = false;
  uint32_t mclkWords[4] = {};
  bool pin[kPinCount] = {};
  PinState pinFound[kPinCount];
  bool rail[kRailCount] = {};
  bool cciUp = false;
  uint32_t cciIrqMask = 0;
  bool votes = false;
};

template <class O>
class Session {
 public:
  Session(O &ops, const PowerOptions &opt) : ops(ops), opt(opt) {}
  O &ops;
  PowerOptions opt;

  // Powers `which`, reads its ID, powers it down. `s` is level 1's survey (re-read here after step 201).
  SensorResult run(Sensor which, const Loader &loader, const Pmic &pmic, const SurveyOptions &so, Survey &s)
  {
    begin(which, loader, pmic, so, s);
    end();
    return r_;
  }
  // Level 3's halves of run(): begin() powers the sensor and reads its ID (true: the ID matched and it stays powered, its bus up);
  // end() powers it down (always, after begin, whatever begin returned).
  bool begin(Sensor which, const Loader &loader, const Pmic &pmic, const SurveyOptions &so, Survey &s)
  {
    r_ = SensorResult();
    u_ = Undo();
    r_.attempted = true;
    r_.which = which;
    rear_ = which == Sensor::rear;
    f_ = &kSensors[rear_ ? 0 : 1];
    r_.bus = rear_ ? (opt.rearCci ? "CCI0 master 1" : "software I2C gpio115/116") : "software I2C gpio4/5";
    found_ = s;
    if (bringUp(loader, pmic, so, s)) readId();
    return r_.idOk;
  }
  void end() { powerDown(); }
  const SensorResult &result() const { return r_; }
  bool viaCci() const { return rear_ && opt.rearCci; }
  // One register write over the sensor's bus (level 3: its tables and controls): the 16-bit register address, then `bytes` (1..3)
  // of the value, big-endian. Only while begin()'s bus is up.
  bool sensorWrite(uint16_t reg, unsigned bytes, uint32_t value)
  {
    if (!bytes || bytes > 3 || !r_.idOk) return false;
    uint8_t b[5] = {uint8_t(reg >> 8), uint8_t(reg)};
    for (unsigned i = 0; i < bytes; ++i) b[2 + i] = uint8_t(value >> (8 * (bytes - 1 - i)));
    bool ok;
    if (viaCci()) {
      r_.cci = cci_.write(f_->address, b, 2 + bytes);
      ok = r_.cci == CciResult::ok;
    } else {
      r_.i2c = bit_.write(f_->address, b, 2 + bytes);
      ok = r_.i2c == I2c::ok;
    }
    ++r_.sensorWrites;
    if (!ok) ++r_.sensorWriteFailures;
    return ok;
  }
  bool sensorRead(uint16_t reg, uint8_t *out, unsigned n)
  {
    if (!n || n > 4 || !r_.idOk) return false;
    if (viaCci()) return (r_.cci = cci_.readReg(f_->address, reg, 2, out, n)) == CciResult::ok;
    const uint8_t a[2] = {uint8_t(reg >> 8), uint8_t(reg)};
    return (r_.i2c = bit_.writeRead(f_->address, a, 2, out, n)) == I2c::ok;
  }

  // ------------------------------------------------------------------------------------------------------ the allowlist
  bool writable(uint64_t pa) const
  {
    const uint64_t gcc[3] = {kGccCameraAhb, kGccCameraXo, kGccMclkAhb};
    for (uint64_t o : gcc)
      if (pa == kGcc + o) return true;
    const uint64_t cc[] = {kGdscTitanTop, kCcDrvAhb, kCcDrvXo, kCcGdscClk, kCcSleepClk, kCcCamTopAhb, kCcCci0,
                           kCcSlowAhbRcg, kCcSlowAhbRcg + kRcgCfg, kCcCci0Rcg, kCcCci0Rcg + kRcgCfg, kCcCci0Rcg + kRcgM,
                           kCcCci0Rcg + kRcgN, kCcCci0Rcg + kRcgD};
    for (uint64_t o : cc)
      if (pa == kCamCc + o) return true;
    const unsigned mclks[2] = {1, 4};
    const uint32_t rcgRegs[5] = {0, kRcgCfg, kRcgM, kRcgN, kRcgD};
    for (unsigned n : mclks) {
      for (uint32_t o : rcgRegs)
        if (pa == kMclkCc + mclkRcg(n) + o) return true;
      if (pa == kMclkCc + mclkBranch(n)) return true;
    }
    const uint32_t pllRegs[11] = {0x00, 0x04, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30};   // Rivian ELU, MODE .. TEST_CTL_U
    for (uint32_t o : pllRegs)
      if (pa == kMclkCc + kMclkPll + o) return true;
    for (unsigned i = 0; i < kPinCount; ++i)
      if (pa == tlmmCfg(kPins[i].gpio) || pa == tlmmIo(kPins[i].gpio)) return true;
    return pa >= kCci0 && pa < kCci0 + 0xc10 && !(pa & 3);
  }

 private:
  // ---------------------------------------------------------------------------------------------------------- stores
  bool put(uint64_t pa, uint32_t v, const char *what)
  {
    if (!writable(pa)) {
      ++r_.refusedStores;
      ops.note("REFUSED store %08x to %#llx (%s): not on level 2's allowlist", v, (unsigned long long)pa, what);
      return false;
    }
    ++r_.writes;
    ops.wr(pa, v, what);
    return true;
  }
  bool putQuiet(uint64_t pa, uint32_t v)
  {
    if (!writable(pa)) {
      ++r_.refusedStores;
      return false;
    }
    ++r_.quietWrites;
    ops.wrQuiet(pa, v);
    return true;
  }
  bool poll(uint64_t pa, uint32_t mask, uint32_t want, unsigned us)
  {
    for (unsigned waited = 0;; ++waited) {
      r_.lastValue = ops.rd(pa);
      if ((r_.lastValue & mask) == want) return true;
      if (waited >= us) return false;
      ops.udelay(1);
    }
  }
  bool fail(int step, Failure f)
  {
    if (!r_.failedStep) {
      r_.failedStep = -step;
      r_.failure = f;
    }
    return false;
  }
  bool stopHere(int step)
  {
    if (opt.stopAt && step >= opt.stopAt) return !fail(step, Failure::stopped);
    return false;
  }

  // --------------------------------------------------------------------------------------------------------- branches
  bool branchOn(uint64_t cbcr, const char *what)
  {
    const uint32_t v = ops.rd(cbcr);
    if (!put(cbcr, v | kCbcrEnable, what)) return false;
    return poll(cbcr, kCbcrClkOff, 0, kBranchPollUs);
  }
  void branchOff(uint64_t cbcr, const char *what)
  {
    const uint32_t v = ops.rd(cbcr);
    if (!put(cbcr, v & ~kCbcrEnable, what)) return;
    if (!poll(cbcr, kCbcrClkOff, kCbcrClkOff, kBranchPollUs)) ++r_.offWarnings;   // a branch another vote keeps running
  }
  // GCC's camera branches: CLK_ENABLE set where clear; then, by Linux's rule, a hardware-gated branch (HW_CTL: the tablet's camera
  // and MCLK-controller AHB, c1 2026-11-12) counts as on without a CLK_OFF wait (it reads CLK_OFF set while idle); any other must
  // show CLK_OFF clear within 500 us.
  bool gccBranchOn(unsigned i, uint32_t off, const char *what)
  {
    const uint32_t v = ops.rd(kGcc + off);
    if (branchEnabled(v) && gccClockOn(v)) return true;
    ops.gccLock(true);
    const bool ok = put(kGcc + off, v | kCbcrEnable, what);
    ops.gccLock(false);
    if (!ok) return false;
    u_.gcc[i] = !(v & kCbcrEnable);
    if (branchHwGated(v)) return true;
    return poll(kGcc + off, kCbcrClkOff, 0, kBranchPollUs);
  }
  void gccBranchOff(unsigned i, uint32_t off, const char *what)
  {
    if (!u_.gcc[i]) return;
    ops.gccLock(true);
    put(kGcc + off, ops.rd(kGcc + off) & ~kCbcrEnable, what);
    ops.gccLock(false);
    u_.gcc[i] = false;
  }

  // An RCG to a configuration (the shared-RCG way: the root forced on for the switch, then released): CFG's source/divider/mode,
  // and M/N/D for an M/N counter. Returns false when the root does not come on or the update does not complete.
  bool rcgSet(uint64_t base, uint32_t cfg, bool mnd, uint32_t m, uint32_t n, uint32_t d, const char *what)
  {
    const uint32_t cmd = ops.rd(base);
    if (!put(base, cmd | kRcgRootEn, "RCG CMD: ROOT_EN (forced on for the switch)")) return false;
    bool ok = poll(base, kRcgRootOff, 0, kRcgUpdatePolls);
    if (ok) {
      if (mnd) {
        put(base + kRcgM, m, "RCG M");
        put(base + kRcgN, n, "RCG N (~(N - M))");
        put(base + kRcgD, d, "RCG D (~2D)");
      }
      const uint32_t old = ops.rd(base + kRcgCfg);
      ok = put(base + kRcgCfg, (old & ~kRcgCfgMask) | cfg, what);
      ok = ok && put(base, ops.rd(base) | kRcgUpdate, "RCG CMD: UPDATE");
      ok = ok && poll(base, kRcgUpdate, 0, kRcgUpdatePolls);
    }
    put(base, ops.rd(base) & ~kRcgRootEn, "RCG CMD: ROOT_EN released");
    return ok;
  }

  // ------------------------------------------------------------------------------------------------------------- pins
  void pinRecord(unsigned i)
  {
    if (u_.pin[i]) return;
    u_.pin[i] = true;
    u_.pinFound[i] = PinState{ops.rd(tlmmCfg(kPins[i].gpio)), ops.rd(tlmmIo(kPins[i].gpio))};
  }
  uint32_t pinBase(unsigned i, unsigned function, unsigned pull)
  {
    const uint32_t found = u_.pinFound[i].cfg;
    uint32_t cfg = (found & ~(kTlmmPullMask | kTlmmFuncMask | kTlmmDrvMask | kTlmmOe | kTlmmEgpioEnable)) |
                   uint32_t(function) << kTlmmFuncShift | drvField(kPins[i].driveMa) | pull;
    if (found & kTlmmEgpioPresent) cfg |= kTlmmEgpioEnable;   // this processor's TLMM owns the pin (as Linux claims it)
    return cfg;
  }
  bool pinOut(unsigned i, bool high, const char *what)
  {
    pinRecord(i);
    const uint64_t g = kPins[i].gpio;
    return put(tlmmIo(unsigned(g)), high ? kTlmmOut : 0, what) && put(tlmmCfg(unsigned(g)), pinBase(i, 0, 0) | kTlmmOe, what);
  }
  bool pinLevel(unsigned i, bool high, const char *what) { return put(tlmmIo(kPins[i].gpio), high ? kTlmmOut : 0, what); }
  bool pinFunc(unsigned i, const char *what)
  {
    pinRecord(i);
    return put(tlmmCfg(kPins[i].gpio), pinBase(i, kPins[i].function, 0), what);
  }
  // A software I2C line: GPIO, output value 0, released (input) until driven.
  bool pinOpenDrain(unsigned i, const char *what)
  {
    pinRecord(i);
    return put(tlmmIo(kPins[i].gpio), 0, what) && put(tlmmCfg(kPins[i].gpio), pinBase(i, 0, opt.busPullUp ? 3u : 0u), what);
  }
  void pinRestore(unsigned i)
  {
    if (!u_.pin[i]) return;
    put(tlmmIo(kPins[i].gpio), u_.pinFound[i].io & kTlmmOut, "pin: output value as found");
    put(tlmmCfg(kPins[i].gpio), u_.pinFound[i].cfg, "pin: configuration as found");
    u_.pin[i] = false;
  }

  // ------------------------------------------------------------------------------------------------------------ rails
  bool railOn(unsigned i, int step)
  {
    RailRecord &rec = r_.rails[i - railFirst()];
    rec.found = found_.rails[i];
    if (rec.found.enabled() && rec.found.ready()) {
      ops.note("rail %s (%s): already on at %u mV: used as it is, not voted", kRails[i].supply, kRails[i].cmdDb, rec.found.mv);
      rec.on = rec.found;
      return true;
    }
    const VrmCmd cmds[3] = {{kVrmMode, kVrmModeHpm}, {kVrmVoltage, kRails[i].mv}, {kVrmEnable, 1}};
    rec.voted = true;
    if (ops.vrm(i, cmds, 3)) return fail(step, Failure::rail);
    rec.ours = true;
    u_.rail[i] = true;
    ops.msleep(1);
    RailState now;
    const int rc = ops.pmic(i, now);
    rec.on = now;
    if (rc || !now.read) return opt.blind ? true : fail(step, Failure::rail);
    if (!now.enabled() || !now.ready()) return fail(step, Failure::rail);
    if (now.mv + 8 < kRails[i].mv || now.mv > kRails[i].mv + 8) return fail(step, Failure::railVoltage);
    return true;
  }
  void railOff(unsigned i)
  {
    if (!u_.rail[i]) return;
    const VrmCmd off = {kVrmEnable, 0};
    if (ops.vrm(i, &off, 1)) ++r_.offWarnings;
    u_.rail[i] = false;
    RailState now;
    if (!ops.pmic(i, now)) r_.rails[i - railFirst()].off = now;
    if (now.read && now.enabled()) ++r_.offWarnings;   // another voter keeps it on
  }
  unsigned railFirst() const { return rear_ ? kRailRearVddd : kRailFrontVddio; }

  // ------------------------------------------------------------------------------------------------------------- MCLK
  // The MCLK PLL (Rivian ELU): configured only when it is not running (CONFIG_CTL words, L 0x32, standby, RESET_N | BYPASSNL), then
  // RESET_N, RUN, LOCK_DET within 1.5 ms, the outputs, OUTCTRL. A PLL found running is used only at L 0x32 (960 MHz).
  bool pllOn()
  {
    const uint64_t p = kMclkCc + kMclkPll;
    const uint32_t mode = ops.rd(p), opmode = ops.rd(p + 4);
    if (pllRunning(mode, opmode)) return ops.rd(p + kPllRivianL) == 0x32;
    if (!(mode & kPllLockDet)) {
      put(p + 0x1c, 0x12000000, "MCLK PLL0 CONFIG_CTL");
      put(p + 0x20, 0x00890263, "MCLK PLL0 CONFIG_CTL_U");
      put(p + 0x24, 0x1af04237, "MCLK PLL0 CONFIG_CTL_U1");
      put(p + kPllRivianL, 0x32, "MCLK PLL0 L_VAL: 0x32 (960 MHz)");
      put(p + 4, 0, "MCLK PLL0 OPMODE: standby");
      put(p, (ops.rd(p) & ~7u) | 6u, "MCLK PLL0 MODE: RESET_N | BYPASSNL");
      u_.pllConfigured = true;
    }
    u_.pll = true;
    put(p, ops.rd(p) | 4u, "MCLK PLL0 MODE: RESET_N");
    put(p + 4, 1, "MCLK PLL0 OPMODE: run");
    if (!poll(p, kPllLockDet, kPllLockDet, 1500)) return false;
    put(p + kPllRivianUserCtl, ops.rd(p + kPllRivianUserCtl) | 7u, "MCLK PLL0 USER_CTL: outputs on");
    return put(p, ops.rd(p) | kPllOutctrl, "MCLK PLL0 MODE: OUTCTRL");
  }
  void pllOff()
  {
    if (!u_.pll) return;
    const uint64_t p = kMclkCc + kMclkPll;
    put(p, ops.rd(p) & ~kPllOutctrl, "MCLK PLL0 MODE: OUTCTRL off");
    put(p + kPllRivianUserCtl, ops.rd(p + kPllRivianUserCtl) & ~7u, "MCLK PLL0 USER_CTL: outputs off");
    put(p + 4, 0, "MCLK PLL0 OPMODE: standby");
    u_.pll = false;
  }
  bool mclkOn(int step)
  {
    const unsigned n = f_->mclk;
    const uint64_t rcg = kMclkCc + mclkRcg(n);
    const uint32_t cfg = ops.rd(rcg + kRcgCfg);
    u_.mclkWords[0] = cfg;
    u_.mclkWords[1] = ops.rd(rcg + kRcgM);
    u_.mclkWords[2] = ops.rd(rcg + kRcgN);
    u_.mclkWords[3] = ops.rd(rcg + kRcgD);
    if (rear_ && opt.mclk24) {
      if (!pllOn()) return fail(step, Failure::pll);
      // 24 MHz = 960 MHz / 10 x 1/4: source 3, half divider 19, dual-edge M/N with M 1, N 4 (N reg ~(N - M), D reg ~clamp(N, M, 2 (N - M))).
      const uint32_t m = 1, nn = 4, mask = 0xff, nReg = ~(nn - m) & mask, dReg = ~nn & mask;
      u_.mclkRcg = true;
      if (!rcgSet(rcg, 19u | 3u << 8 | 2u << 12, true, m, nReg, dReg, "MCLK RCG CFG: PLL0 / 10, M/N 1/4 (24 MHz)"))
        return fail(step, Failure::rcg);
      r_.mclkHz = 24000000;
    } else {
      if (!rcgIsXoUndivided(cfg)) {
        u_.mclkRcg = true;
        if (!rcgSet(rcg, kRcgCfgXo, false, 0, 0, 0, "MCLK RCG CFG: XO / 1 (19.2 MHz)")) return fail(step, Failure::rcg);
      }
      r_.mclkHz = uint32_t(kXoHz);
    }
    u_.mclkBranch = true;
    if (!branchOn(kMclkCc + mclkBranch(n), "MCLK branch on")) return fail(step, Failure::branch);
    return true;
  }
  void mclkOff()
  {
    const unsigned n = f_->mclk;
    const uint64_t rcg = kMclkCc + mclkRcg(n);
    if (u_.mclkBranch) branchOff(kMclkCc + mclkBranch(n), "MCLK branch off");
    u_.mclkBranch = false;
    if (u_.mclkRcg) {
      const bool mnd = rcgMode(u_.mclkWords[0]) != 0 || rcgMode(ops.rd(rcg + kRcgCfg)) != 0;
      if (!rcgSet(rcg, u_.mclkWords[0] & kRcgCfgMask, mnd, u_.mclkWords[1], u_.mclkWords[2], u_.mclkWords[3], "MCLK RCG CFG: as found"))
        ++r_.offWarnings;
      u_.mclkRcg = false;
    }
    pllOff();
  }

  // ------------------------------------------------------------------------------------------------------------- CAMCC
  bool titanOn(int step)
  {
    const uint32_t critical[4] = {kCcDrvAhb, kCcDrvXo, kCcGdscClk, kCcSleepClk};
    for (unsigned i = 0; i < 4; ++i) {
      const uint32_t v = ops.rd(kCamCc + critical[i]);
      if ((v & kCbcrEnable) && branchRunning(v)) continue;
      u_.ccCritical[i] = true;
      if (!branchOn(kCamCc + critical[i], "CAMCC critical branch on")) return fail(step, Failure::branch);
    }
    const uint64_t g = kCamCc + kGdscTitanTop;
    const uint32_t v = ops.rd(g);
    if (gdscOn(v) && gdscRequestedOn(v)) return true;
    u_.titan = true;
    u_.titanFound = v;
    const uint32_t init = (v & ~(kGdscHwControl | kGdscSwOverride | kGdscWaitMask)) | kGdscWaitValues;
    if (!put(g, init, "TITAN_TOP GDSCR: wait values, HW control and SW override off")) return fail(step, Failure::allowlist);
    if (!put(g, init & ~kGdscCollapse, "TITAN_TOP GDSCR: out of collapse")) return fail(step, Failure::allowlist);
    if (!poll(g + kGdscCfg, kGdscCfgUpDone, kGdscCfgUpDone, kGdscPollUs) || !gdscOn(ops.rd(g))) return fail(step, Failure::gdsc);
    ops.udelay(1);
    put(g, ops.rd(g) | kGdscRetainFf, "TITAN_TOP GDSCR: RETAIN_FF_ENABLE");
    return true;
  }
  void titanOff()
  {
    const uint64_t g = kCamCc + kGdscTitanTop;
    if (u_.titan) {
      put(g, ops.rd(g) | kGdscCollapse, "TITAN_TOP GDSCR: collapse");
      if (!poll(g + kGdscCfg, kGdscCfgDownDone, kGdscCfgDownDone, kGdscPollUs)) ++r_.offWarnings;
      // Collapsed again: the word as found (its wait values and RETAIN_FF), when it was found collapsed.
      if (u_.titanFound & kGdscCollapse) put(g, u_.titanFound, "TITAN_TOP GDSCR: as found (collapsed)");
      u_.titan = false;
    }
    const uint32_t critical[4] = {kCcDrvAhb, kCcDrvXo, kCcGdscClk, kCcSleepClk};
    for (int i = 3; i >= 0; --i)
      if (u_.ccCritical[i]) {
        branchOff(kCamCc + critical[i], "CAMCC critical branch off (as found)");
        u_.ccCritical[i] = false;
      }
  }
  // The CCI's clocks: the slow AHB source and CCI0's source on the XO (their reset state; programmed only if found otherwise), the
  // top AHB and CCI0 branches. withCci false (level 3 without CCI0): the AHB part only.
  bool cciClocksOn(int step, bool withCci = true)
  {
    const uint64_t slow = kCamCc + kCcSlowAhbRcg, cci = kCamCc + kCcCci0Rcg;
    const uint32_t slowCfg = ops.rd(slow + kRcgCfg), cciCfg = withCci ? ops.rd(cci + kRcgCfg) : 0;
    if (!rcgIsXoUndivided(slowCfg)) {
      u_.slowAhbRcg = true;
      u_.slowAhbCfg = slowCfg;
      if (!rcgSet(slow, kRcgCfgXo, false, 0, 0, 0, "slow AHB RCG CFG: XO / 1")) return fail(step, Failure::rcg);
    }
    if (withCci && !rcgIsXoUndivided(cciCfg)) {
      u_.cci0Rcg = true;
      u_.cci0Words[0] = cciCfg;
      u_.cci0Words[1] = ops.rd(cci + kRcgM);
      u_.cci0Words[2] = ops.rd(cci + kRcgN);
      u_.cci0Words[3] = ops.rd(cci + kRcgD);
      if (!rcgSet(cci, kRcgCfgXo, false, 0, 0, 0, "CCI0 RCG CFG: XO / 1 (19.2 MHz)")) return fail(step, Failure::rcg);
    }
    const uint32_t top = ops.rd(kCamCc + kCcCamTopAhb), c0 = withCci ? ops.rd(kCamCc + kCcCci0) : 0;
    if (!((top & kCbcrEnable) && branchRunning(top))) {
      u_.topAhb = true;
      if (!branchOn(kCamCc + kCcCamTopAhb, "camera top AHB branch on")) return fail(step, Failure::branch);
    }
    if (withCci && !((c0 & kCbcrEnable) && branchRunning(c0))) {
      u_.cci0 = true;
      if (!branchOn(kCamCc + kCcCci0, "CCI0 branch on")) return fail(step, Failure::branch);
    }
    return true;
  }
  void cciClocksOff()
  {
    if (u_.cci0) branchOff(kCamCc + kCcCci0, "CCI0 branch off");
    if (u_.cci0Rcg)
      rcgSet(kCamCc + kCcCci0Rcg, u_.cci0Words[0] & kRcgCfgMask, rcgMode(u_.cci0Words[0]) != 0, u_.cci0Words[1], u_.cci0Words[2],
             u_.cci0Words[3], "CCI0 RCG CFG: as found");
    if (u_.topAhb) branchOff(kCamCc + kCcCamTopAhb, "camera top AHB branch off");
    if (u_.slowAhbRcg) rcgSet(kCamCc + kCcSlowAhbRcg, u_.slowAhbCfg & kRcgCfgMask, false, 0, 0, 0, "slow AHB RCG CFG: as found");
    u_.cci0 = u_.cci0Rcg = u_.topAhb = u_.slowAhbRcg = false;
  }

  // -------------------------------------------------------------------------------------------------------------- bus
  static uint32_t cciRd(void *c, uint32_t off) { return static_cast<Session *>(c)->ops.rd(kCci0 + off); }
  static void cciWr(void *c, uint32_t off, uint32_t v, const char *what) { static_cast<Session *>(c)->put(kCci0 + off, v, what); }
  static void cciDelay(void *c, unsigned us) { static_cast<Session *>(c)->ops.udelay(us); }
  unsigned sdaPin() const { return rear_ ? kPinCciSda : kPinFrontSda; }
  unsigned sclPin() const { return rear_ ? kPinCciScl : kPinFrontScl; }
  void line(unsigned pin, bool release)
  {
    const uint64_t cfg = tlmmCfg(kPins[pin].gpio);
    const uint32_t v = ops.rd(cfg);
    putQuiet(cfg, release ? v & ~kTlmmOe : v | kTlmmOe);
  }
  static void sdaLine(void *c, bool release) { static_cast<Session *>(c)->line(static_cast<Session *>(c)->sdaPin(), release); }
  static void sclLine(void *c, bool release) { static_cast<Session *>(c)->line(static_cast<Session *>(c)->sclPin(), release); }
  static bool sdaRead(void *c)
  {
    Session *s = static_cast<Session *>(c);
    return s->ops.rd(tlmmIo(kPins[s->sdaPin()].gpio)) & kTlmmIn;
  }
  static bool sclRead(void *c)
  {
    Session *s = static_cast<Session *>(c);
    return s->ops.rd(tlmmIo(kPins[s->sclPin()].gpio)) & kTlmmIn;
  }
  static void lineDelay(void *c, unsigned us) { static_cast<Session *>(c)->ops.udelay(us); }

  bool busUp(int step)
  {
    if (rear_ && opt.rearCci) {
      if (!titanOn(step) || !cciClocksOn(step)) return false;
      if (!pinFunc(kPinCciSda, "CCI SDA: cci_i2c_sda, 4 mA, no bias") || !pinFunc(kPinCciScl, "CCI SCL: cci_i2c_scl, 4 mA, no bias"))
        return fail(step, Failure::allowlist);
      u_.cciIrqMask = ops.rd(kCci0 + kCciIrqMask);
      u_.cciUp = true;
      cci_ = CciMaster(CciIo{this, cciRd, cciWr, cciDelay});
      const CciResult rc = cci_.init(opt.cciKhz == 100 ? kCciXo100k : kCciXo400k);
      r_.cciVersion = cci_.version;
      r_.cci = rc;
      if (rc != CciResult::ok) return fail(step, Failure::cci);
      return true;
    }
    if (opt.camss && (!titanOn(step) || !cciClocksOn(step, false))) return false;   // level 3: CAMSS without CCI0
    if (!pinOpenDrain(sdaPin(), "I2C SDA: GPIO, open drain (released)") || !pinOpenDrain(sclPin(), "I2C SCL: GPIO, open drain (released)"))
      return fail(step, Failure::allowlist);
    bit_.configure(LineIo{this, sdaLine, sclLine, sdaRead, sclRead, lineDelay}, opt.bitHalfUs, 20000);
    return true;
  }
  void busDown()
  {
    if (u_.cciUp) {
      cci_.quiesce(u_.cciIrqMask);
      u_.cciUp = false;
    }
    pinRestore(sdaPin());
    pinRestore(sclPin());
    cciClocksOff();   // each part only when it was ours (the rear on CCI0, or level 3's CAMSS)
    titanOff();
  }

  // ------------------------------------------------------------------------------------------------------------ the run
  bool bringUp(const Loader &loader, const Pmic &pmic, const SurveyOptions &so, Survey &s)
  {
    const bool camcc = (rear_ && opt.rearCci) || opt.camss;   // this path touches CAMCC
    ops.step(kStepGcc, camcc ? (opt.votes ? "CAMCC's domains (MXC, MMCX: shared votes); GCC: camera AHB, camera XO, MCLK controller AHB (on where off)"
                                          : "GCC: camera AHB, camera XO, MCLK controller AHB (on where off)")
                             : "GCC: MCLK controller AHB (on where off)");
    if (camcc && opt.votes) {
      if (ops.mmRails(true)) return fail(kStepGcc, Failure::votes);
      u_.votes = true;
      r_.voted = true;
    }
    if (camcc) {
      if (!gccBranchOn(0, kGccCameraAhb, "GCC camera AHB on") || !gccBranchOn(1, kGccCameraXo, "GCC camera XO on"))
        return fail(kStepGcc, Failure::gcc);
    }
    if (!gccBranchOn(2, kGccMclkAhb, "GCC MCLK controller AHB on")) return fail(kStepGcc, Failure::gcc);
    if (stopHere(kStepGcc)) return false;
    ops.step(kStepResurvey, "the survey again, with the controllers' AHB running");
    s = survey(loader, pmic, so);
    found_ = s;
    ops.step(kStepRefusals, "level 2's checks");
    BusChoice bus;
    bus.rearCci = opt.rearCci;
    bus.blind = opt.blind;
    if (const char *why = sensorRefusal(s, r_.which, bus)) {
      r_.why = why;
      return fail(kStepRefusals, Failure::refused);
    }
    if (stopHere(kStepRefusals)) return false;
    // The bus first, as in Linux (the controller and its pins are up from its probe, before any sensor is powered).
    ops.step(kStepBus, rear_ && opt.rearCci ? "the bus: TITAN_TOP, CCI0's clocks from the XO, its pins, the controller"
                                            : "the bus: software I2C pins (GPIO, released)");
    if (!busUp(kStepBus)) return false;
    if (stopHere(kStepBus)) return false;
    const unsigned reset = rear_ ? kPinRearReset : kPinFrontReset, avdd = rear_ ? kPinRearAvdd : kPinFrontAvdd,
                   mclkPin = rear_ ? kPinRearMclk : kPinFrontMclk;
    ops.step(kStepPins, "pins: reset low (asserted), supply enable low, the MCLK pin's function");
    if (!pinOut(reset, false, "reset: GPIO output low (asserted)") || !pinOut(avdd, false, "analogue supply enable: GPIO output low") ||
        !pinFunc(mclkPin, "MCLK pin: its camera MCLK function"))
      return fail(kStepPins, Failure::allowlist);
    if (stopHere(kStepPins)) return false;
    if (rear_) {
      ops.step(kStepRailFirst, "vddd: L1I 1096 mV (RPMh), read back");
      if (!railOn(kRailRearVddd, kStepRailFirst)) return false;
      ops.udelay(kRearVdddToVddaUs);
      if (stopHere(kStepRailFirst)) return false;
      ops.step(kStepAvdd, "vdda: gpio175 high (the fixed 2.8 V supply)");
      if (!pinLevel(avdd, true, "rear vdda enable: high")) return fail(kStepAvdd, Failure::allowlist);
      ops.udelay(kFixedStartupUs);
      if (stopHere(kStepAvdd)) return false;
      ops.step(kStepRails, "vddio: L6B 1800 mV, afvdd: L7B 2800 mV (RPMh), read back");
      if (!railOn(kRailRearVddio, kStepRails) || !railOn(kRailRearAfvdd, kStepRails)) return false;
      if (stopHere(kStepRails)) return false;
    } else {
      ops.step(kStepRails, "vddio: L10B 1800 mV, dvdd: L2G 1200 mV (RPMh), read back; avdd: gpio176 high");
      if (!railOn(kRailFrontVddio, kStepRails) || !railOn(kRailFrontDvdd, kStepRails)) return false;
      if (!pinLevel(avdd, true, "front avdd enable: high")) return fail(kStepRails, Failure::allowlist);
      ops.udelay(kFixedStartupUs);
      ops.udelay(kFrontSupplyToMclkUs);
      if (stopHere(kStepRails)) return false;
    }
    ops.step(kStepMclk, rear_ && opt.mclk24 ? "MCLK1 24 MHz (PLL0 960 MHz / 10 x 1/4)" : rear_ ? "MCLK1 19.2 MHz (XO)" : "MCLK4 19.2 MHz (XO)");
    if (!mclkOn(kStepMclk)) return false;
    if (!rear_) ops.udelay(kFrontMclkToResetUs);
    if (stopHere(kStepMclk)) return false;
    ops.step(kStepReset, "reset released");
    if (!pinLevel(reset, true, "reset: high (released)")) return fail(kStepReset, Failure::allowlist);
    ops.udelay(rear_ ? kRearResetToIdUs : kFrontResetToIdUs);
    if (stopHere(kStepReset)) return false;
    return true;
  }

  void readId()
  {
    ops.step(kStepId, rear_ ? "chip ID at 0x0000 (expect 0x38e1)" : "chip ID at 0x300a (expect 0x5632)");
    const bool viaCci = rear_ && opt.rearCci;
    bool mismatch = false;
    for (unsigned t = 0; t < f_->idTries; ++t) {
      if (t) ops.msleep(f_->idRetryMs);
      ++r_.tries;
      uint8_t b[2] = {};
      bool ok;
      if (viaCci) {
        r_.cci = cci_.readReg(f_->address, f_->idReg, 2, b, 2);
        ok = r_.cci == CciResult::ok;
        if (!ok) r_.why = cciName(r_.cci);
      } else {
        const uint8_t reg[2] = {uint8_t(f_->idReg >> 8), uint8_t(f_->idReg)};
        r_.i2c = bit_.writeRead(f_->address, reg, 2, b, 2);
        ok = r_.i2c == I2c::ok;
        if (!ok) r_.why = i2cName(r_.i2c);
      }
      if (!ok) {
        mismatch = false;
        continue;
      }
      r_.id = uint16_t(b[0] << 8 | b[1]);
      if (r_.id == f_->idValue) {
        r_.idOk = true;
        r_.why = nullptr;
        break;
      }
      r_.why = "unexpected chip ID";
      mismatch = true;
    }
    if (!r_.idOk) {
      fail(kStepId, mismatch ? Failure::idMismatch : Failure::idRead);
      return;
    }
    if (opt.holdMs) ops.msleep(opt.holdMs);
  }

  // The reverse of the drivers' order; each part only for what Undo says was ours.
  void powerDown()
  {
    ops.step(kStepDown, "power down (reverse order, only what this level changed)");
    const unsigned reset = rear_ ? kPinRearReset : kPinFrontReset, avdd = rear_ ? kPinRearAvdd : kPinFrontAvdd,
                   mclkPin = rear_ ? kPinRearMclk : kPinFrontMclk;
    if (u_.pin[reset]) pinLevel(reset, false, "reset: low (asserted)");
    busDown();
    mclkOff();
    if (rear_) {
      railOff(kRailRearAfvdd);
      railOff(kRailRearVddio);
      if (u_.pin[avdd]) pinLevel(avdd, false, "rear vdda enable: low");
      ops.udelay(kRearOffVdddUs);
      railOff(kRailRearVddd);
    } else {
      if (u_.pin[avdd]) pinLevel(avdd, false, "front avdd enable: low");
      railOff(kRailFrontDvdd);
      railOff(kRailFrontVddio);
    }
    pinRestore(mclkPin);
    pinRestore(avdd);
    pinRestore(reset);
    gccBranchOff(2, kGccMclkAhb, "GCC MCLK controller AHB off (as found)");
    gccBranchOff(1, kGccCameraXo, "GCC camera XO off (as found)");
    gccBranchOff(0, kGccCameraAhb, "GCC camera AHB off (as found)");
    if (u_.votes) {   // CAMCC's domains last, as Linux's runtime suspend drops them after the clocks
      if (ops.mmRails(false)) ++r_.offWarnings;
      else u_.votes = false;
    }
    bool clean = true;
    for (bool b : u_.rail) clean = clean && !b;
    for (bool b : u_.pin) clean = clean && !b;
    r_.cleanOff = clean && !u_.titan && !u_.mclkBranch && !u_.pll && !u_.cciUp && !u_.votes;
  }

  SensorResult r_;
  Undo u_;
  Survey found_;
  bool rear_ = true;
  const SensorFacts *f_ = &kSensors[0];
  CciMaster cci_{CciIo{}};
  BitI2c bit_;
};

}  // namespace pianocamera
