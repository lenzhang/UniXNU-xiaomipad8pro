// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoCamera's portable parts against a board model:
//   - the clock controllers and CAMSS with their gates (a load or store to CAMCC without GCC's camera AHB, to the MCLK controller
//     without its AHB, to CCI0 or a CSID/TFE without TITAN_TOP and their clocks, to QUP2 SE1 without its clocks: a "stall", a
//     failure); branches, RCGs, the TITAN_TOP GDSC, the MCLK PLL; TLMM with open-drain bus lines whose pull-ups need the sensor's
//     vddio; PMIC LDOs over a SPMI observer and RPMh VRM votes; cmd-db; the SMMU's groups;
//   - CCI0 executing its queues against the rear sensor, and a bit-level I2C slave per bus (16-bit register pointer) that answers only
//     when its sensor is powered, clocked and out of reset long enough;
//   - level 1: loads only, the gates respected, what is reported; level 2 per sensor and bus: the chip ID, the drivers' order and
//     waits, every rail at its exact voltage in HPM, and after power-down every register and rail exactly as found; failures at each
//     step (a missing sensor, a wrong ID, a rail at another voltage, a failed vote, a GDSC that stays off, a stuck bus, a stop step),
//     each powered down cleanly; the refusals (nothing stored); the allowlist; the CCI's packing, errors and recovery;
//   - the sensor tables' header (generated from data/camera by the build) against data/camera/MANIFEST's counts.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. -I../../PianoKeyboard -I<gen> camera_host.cpp -o /tmp/camera_host && /tmp/camera_host
#include "CameraPower.h"
#include "CameraRpmh.h"
#include "CameraSurvey.h"
#include "CameraTables.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace pianocamera;
static int failures;
#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                   \
      ++failures;                                                                \
    }                                                                            \
  } while (0)

// --------------------------------------------------------------------------------------------------------- the I2C slave
struct Slave {
  uint8_t address = 0x10;
  std::map<uint16_t, uint8_t> reg;
  uint16_t pointer = 0;
  bool sdaLow = false, sclLow = false, masterSda = true, masterScl = true, lastSda = true, lastScl = true;
  enum { idle, addr, ackAddr, wr, ackWr, rd, rdAck, ignore } st = idle;
  int bits = 0, wbytes = 0;
  uint8_t shift = 0, byte = 0;
  bool reading = false, masterAck = false, present = true;
  unsigned writes = 0, starts = 0, acks = 0;
  std::function<bool()> ready;
  bool sda() const { return masterSda && !sdaLow; }
  bool scl() const { return masterScl && !sclLow; }
  void load()
  {
    byte = reg.count(pointer) ? reg[pointer] : 0;
    ++pointer;
    sdaLow = !(byte & 0x80);
  }
  void rising()
  {
    if (st == addr || st == wr) {
      shift = uint8_t(shift << 1 | (sda() ? 1 : 0));
      ++bits;
    } else if (st == rdAck) {
      masterAck = !sda();
    }
  }
  void falling()
  {
    switch (st) {
    case addr:
      if (bits == 8) {
        if ((shift >> 1) == address && present && ready && ready()) {
          st = ackAddr;
          reading = shift & 1;
          sdaLow = true;
          ++acks;
        } else {
          st = ignore;
        }
      }
      break;
    case ackAddr:
      sdaLow = false;
      if (reading) {
        st = rd;
        bits = 0;
        load();
      } else {
        st = wr;
        bits = 0;
        shift = 0;
        wbytes = 0;
      }
      break;
    case wr:
      if (bits == 8) {
        if (wbytes == 0) pointer = uint16_t(shift << 8);
        else if (wbytes == 1) pointer = uint16_t(pointer | shift);
        else {
          reg[pointer++] = shift;
          ++writes;
        }
        ++wbytes;
        st = ackWr;
        sdaLow = true;
      }
      break;
    case ackWr:
      sdaLow = false;
      st = wr;
      bits = 0;
      shift = 0;
      break;
    case rd:
      if (++bits < 8) sdaLow = !((byte >> (7 - bits)) & 1);
      else {
        sdaLow = false;
        st = rdAck;
      }
      break;
    case rdAck:
      if (masterAck) {
        st = rd;
        bits = 0;
        load();
      } else {
        st = ignore;
        sdaLow = false;
      }
      break;
    default: break;
    }
  }
  void update()
  {
    const bool s = sda(), c = scl();
    if (c && lastScl && lastSda && !s) {
      st = addr;
      bits = 0;
      shift = 0;
      ++starts;
    } else if (c && lastScl && !lastSda && s) {
      st = idle;
    } else if (c && !lastScl) {
      rising();
    } else if (!c && lastScl) {
      falling();
    }
    lastSda = sda();
    lastScl = scl();
  }
};

// ---------------------------------------------------------------------------------------------------------- the board
struct PmicLdo {
  uint8_t sid, page, subtype = 0x31;
  uint16_t mv = 1800;
  uint8_t mode = 0x05, en = 0;
  int apid;
};

struct Board {
  std::map<uint64_t, uint32_t> r;   // every register's value
  std::vector<std::string> violations;
  uint64_t now = 0;
  unsigned loads = 0, stores = 0, quietStores = 0, cciStarts = 0;
  // injections
  bool titanStuck = false, rearAbsent = false, frontAbsent = false, sdaStuck = false, pllNoLock = false;
  uint16_t rearId = 0x38e1, frontId = 0x5632;
  int vrmFail = -1, wrongMv = -1;
  uint16_t wrongMvValue = 0;
  bool noCmdDb[kRailCount] = {};
  // PMIC
  std::vector<PmicLdo> ldo;
  // timing
  int64_t at[32];
  enum { tRearVddd, tRearVdda, tRearVddio, tRearAfvdd, tRearMclk, tRearReset, tFrontVddio, tFrontDvdd, tFrontAvdd, tFrontMclk,
         tFrontReset, tRearResetLow, tRearMclkOff, tRearVdddOff, tRearVddaOff, tFrontResetLow, tFrontMclkOff, tFrontRailOff };
  Slave rearSlave, frontSlave;
  // CCI
  std::vector<uint32_t> queue[2];
  std::vector<uint32_t> readFifo;
  unsigned railVotes = 0;
  std::vector<std::string> vrmLog;

  Board()
  {
    for (auto &x : at) x = -1;
    // GCC: the AHB/XO branches on (their reset state), the AXI ones off.
    for (uint32_t o : {kGccCameraAhb, kGccCameraXo, kGccMclkAhb}) r[kGcc + o] = 0x1;
    for (uint32_t o : {kGccCameraHfAxi, kGccCameraSfAxi, kGccQmipNrt, kGccQmipRt, kGccQmipCmd}) r[kGcc + o] = 0x80000000u;
    r[kGcc + kGccQup2Vote] = 0;
    for (uint32_t o : {kGccQup2MAhb, kGccQup2SAhb, kGccQup2Core, kGccQup2Core2x, kGccQup2S1}) r[kGcc + o] = 0x80000000u;
    r[kGcc + kGccQup2S1Rcg] = 0x80000000u;
    // CAMCC: everything off, TITAN_TOP collapsed with other wait values.
    for (const Reg &g : kCcGdscs) {
      r[kCamCc + g.off] = 0x00222001;
      r[kCamCc + g.off + 4] = kGdscCfgDownDone;
    }
    for (const Reg &b : kCcBranches) r[kCamCc + b.off] = 0x80000000u;
    for (const Reg &c : kCcRcgs) {
      r[kCamCc + c.off] = 0x80000000u;
      r[kCamCc + c.off + 4] = 0;
    }
    r[kCamCc + kCcSlowAhbRcg + 4] = 0x0000020f;   // found on a PLL source: level 2 must switch it to the XO and back
    for (unsigned n = 0; n < kMclkCount; ++n) {
      r[kMclkCc + mclkRcg(n)] = 0x80000000u;
      for (uint32_t o : {kRcgCfg, kRcgM, kRcgN, kRcgD}) r[kMclkCc + mclkRcg(n) + o] = 0;
      r[kMclkCc + mclkBranch(n)] = 0x80000000u;
    }
    for (uint32_t o = 0; o <= 0x30; o += 4) r[kMclkCc + kMclkPll + o] = 0;
    r[kCamCc + kCcCci0Rcg + kRcgM] = r[kCamCc + kCcCci0Rcg + kRcgN] = r[kCamCc + kCcCci0Rcg + kRcgD] = 0;
    r[kMclkCc + kMclkSleep] = 0x1;
    // TLMM: the DT's sleep states (reset/enable output low with pull-down, MCLK pins in their function with pull-down), the buses
    // as GPIO inputs; egpio present on the pins that have it, owned by another processor on one of them.
    auto pin = [&](unsigned gpio, uint32_t cfg, uint32_t io) {
      r[tlmmCfg(gpio)] = cfg;
      r[tlmmIo(gpio)] = io;
    };
    pin(6, 0x201 | kTlmmEgpioPresent | kTlmmEgpioEnable, 0);
    pin(7, 0x201 | kTlmmEgpioPresent, 0);
    pin(175, 0x201 | kTlmmEgpioPresent | kTlmmEgpioEnable, 0);
    pin(176, 0x201 | kTlmmEgpioPresent | kTlmmEgpioEnable, 0);
    pin(90, 1u << 2 | 1u << 6 | 1, 0);
    pin(93, 1u << 2 | 1, 0);
    pin(115, 0x1, 0);
    pin(116, 0x1, 0);
    pin(4, 1u << 6 | kTlmmEgpioPresent | kTlmmEgpioEnable, 0);
    pin(5, 1u << 6 | kTlmmEgpioPresent | kTlmmEgpioEnable, 0);
    // CCI0
    r[kCci0 + kCciHwVersion] = 0x20000000;
    // SMMU: 64 groups, 64 banks, NUMPAGENDXB 7 (256 global pages, then the banks)
    r[kSmmu + 0x20] = 64;
    r[kSmmu + 0x24] = 6u << 28 | 83;   // the tablet's (PianoVideo's survey: 128 pages, 83 banks)
    // PMIC: the camera LDOs off; the CSIPHY ones on. L1I and L3I at the SM8550 page (0x40 + n - 1), the rest at 0xc0 + n.
    int apid = 100;
    for (unsigned i = 0; i < kRailCount; ++i) {
      const Rail &t = kRails[i];
      PmicLdo l{t.sid, t.sid == 8 ? ldoPageAlt(t.ldo) : ldoPage(t.ldo), 0x31, 1800, 0x05, 0, apid++};
      l.mv = t.voted ? 1504 : t.mv;
      l.en = t.voted ? 0 : 0x80;
      ldo.push_back(l);
    }
    rearSlave.reg[0x0000] = 0x38;
    rearSlave.reg[0x0001] = 0xe1;
    frontSlave.reg[0x300a] = 0x56;
    frontSlave.reg[0x300b] = 0x32;
    rearSlave.ready = [this] { return sensorReady(true); };
    frontSlave.ready = [this] { return sensorReady(false); };
  }

  // ------------------------------------------------------------------------------------------- derived state and gates
  uint32_t v(uint64_t pa) const
  {
    auto it = r.find(pa);
    return it == r.end() ? 0 : it->second;
  }
  bool running(uint64_t cbcr) const { return !(v(cbcr) & kCbcrClkOff); }
  bool titan() const { return v(kCamCc + kGdscTitanTop) & kGdscPwrOn; }
  bool railOn(unsigned i) const { return ldo[i].en & 0x80; }
  bool pinHigh(unsigned gpio) const
  {
    const uint32_t c = v(tlmmCfg(gpio)), io = v(tlmmIo(gpio));
    return pinFunction(c) == 0 && (c & kTlmmOe) && (io & kTlmmOut);
  }
  bool mclkRunning(unsigned n) const { return running(kMclkCc + mclkBranch(n)); }
  bool sensorReady(bool rear) const
  {
    if (rear) {
      if (rearAbsent) return false;
      return railOn(kRailRearVddd) && railOn(kRailRearVddio) && pinHigh(175) && mclkRunning(1) && pinHigh(6) && at[tRearReset] >= 0 &&
             int64_t(now) >= at[tRearReset] + 10000;
    }
    if (frontAbsent) return false;
    return railOn(kRailFrontVddio) && railOn(kRailFrontDvdd) && pinHigh(176) && mclkRunning(4) && pinHigh(7) && at[tFrontReset] >= 0 &&
           int64_t(now) >= at[tFrontReset] + 20000;
  }
  bool inRange(uint64_t pa, uint64_t base, uint64_t bytes) const { return pa >= base && pa < base + bytes; }
  // Whether an access to pa would complete on the real bus.
  const char *gate(uint64_t pa) const
  {
    if (inRange(pa, kCamCc, kCamCcBytes) && !running(kGcc + kGccCameraAhb)) return "CAMCC without GCC's camera AHB";
    if (inRange(pa, kMclkCc, kMclkCcBytes) && !running(kGcc + kGccMclkAhb)) return "the MCLK controller without its AHB";
    const bool camAhb = running(kGcc + kGccCameraAhb) && titan() && running(kCamCc + kCcCamTopAhb);
    if (inRange(pa, kCci0, kCci0Bytes) && !(camAhb && running(kCamCc + kCcCci0))) return "CCI0 unpowered or unclocked";
    for (const Block &b : kBlocks)
      if (inRange(pa, b.pa, 0x1000) && !(camAhb && running(kCamCc + b.branch))) return "a CAMSS block unpowered or unclocked";
    if (inRange(pa, kQup2Se1, 0x4000)) return "QUP2 SE1 (never clocked in this model)";
    return nullptr;
  }

  // ------------------------------------------------------------------------------------------------- loads and stores
  uint32_t rd(uint64_t pa)
  {
    ++loads;
    if (const char *g = gate(pa)) {
      violations.push_back(std::string("load stalled: ") + g);
      return 0xffffffffu;
    }
    if (inRange(pa, kTlmm, 0x200000) && (pa & 0xfff) == 4) return ioValue(pa);
    if (pa == kCci0 + cciReadData(kCciMaster)) {
      if (readFifo.empty()) return 0;
      const uint32_t w = readFifo.front();
      readFifo.erase(readFifo.begin());
      r[kCci0 + cciReadLevel(kCciMaster)] = uint32_t(readFifo.size());
      return w;
    }
    return v(pa);
  }
  uint32_t ioValue(uint64_t pa)
  {
    const unsigned gpio = unsigned((pa - kTlmm) / kTlmmStride);
    uint32_t io = v(pa) & kTlmmOut;
    bool level = false;
    if (gpio == 4 || gpio == 5 || gpio == 115 || gpio == 116) {
      level = line(gpio);
    } else {
      const uint32_t c = v(tlmmCfg(gpio));
      level = (c & kTlmmOe) && (v(pa) & kTlmmOut);
    }
    return io | (level ? kTlmmIn : 0);
  }
  bool ourLow(unsigned gpio) const
  {
    const uint32_t c = v(tlmmCfg(gpio));
    return pinFunction(c) == 0 && (c & kTlmmOe) && !(v(tlmmIo(gpio)) & kTlmmOut);
  }
  bool pulled(unsigned gpio) const
  {
    const bool front = gpio == 4 || gpio == 5;
    const bool supply = front ? railOn(kRailFrontVddio) : railOn(kRailRearVddio);
    return supply || (v(tlmmCfg(gpio)) & kTlmmPullMask) == 3;
  }
  bool line(unsigned gpio)
  {
    Slave &s = (gpio == 4 || gpio == 5) ? frontSlave : rearSlave;
    const bool sda = gpio == 4 || gpio == 115;
    if (sda && sdaStuck) return false;
    return pulled(gpio) && !ourLow(gpio) && !(sda ? s.sdaLow : s.sclLow);
  }
  void syncSlave(bool front)
  {
    Slave &s = front ? frontSlave : rearSlave;
    const unsigned sda = front ? 4 : 115, scl = front ? 5 : 116;
    if (pinFunction(v(tlmmCfg(sda))) != 0) return;   // CCI owns the rear pins
    s.masterSda = pulled(sda) && !ourLow(sda) && !(sdaStuck && front);
    s.masterScl = pulled(scl) && !ourLow(scl);
    s.update();
  }
  void wr(uint64_t pa, uint32_t value)
  {
    ++stores;
    if (const char *g = gate(pa)) {
      violations.push_back(std::string("store stalled: ") + g);
      return;
    }
    if (inRange(pa, kTlmm, 0x200000)) {
      const unsigned gpio = unsigned((pa - kTlmm) / kTlmmStride);
      if ((pa & 0xfff) == 4) value &= kTlmmOut;
      const bool wasHigh[2] = {pinHigh(6), pinHigh(7)};
      const bool mclkWas[2] = {mclkRunning(1), mclkRunning(4)};
      (void)mclkWas;
      r[pa] = value;
      if ((gpio == 4 || gpio == 5 || gpio == 115 || gpio == 116) && pinFunction(v(tlmmCfg(gpio))) == 0 && (v(tlmmCfg(gpio)) & kTlmmOe) &&
          (v(tlmmIo(gpio)) & kTlmmOut))
        violations.push_back("an I2C line driven high");
      if (gpio == 175 && pinHigh(175) && at[tRearVdda] < 0) at[tRearVdda] = int64_t(now);
      if (gpio == 175 && !pinHigh(175) && at[tRearVdda] >= 0 && at[tRearVddaOff] < 0) at[tRearVddaOff] = int64_t(now);
      if (gpio == 176 && pinHigh(176) && at[tFrontAvdd] < 0) at[tFrontAvdd] = int64_t(now);
      if (gpio == 6 && pinHigh(6) && !wasHigh[0]) at[tRearReset] = int64_t(now);
      if (gpio == 6 && !pinHigh(6) && wasHigh[0]) at[tRearResetLow] = int64_t(now);
      if (gpio == 7 && pinHigh(7) && !wasHigh[1]) at[tFrontReset] = int64_t(now);
      if (gpio == 7 && !pinHigh(7) && wasHigh[1]) at[tFrontResetLow] = int64_t(now);
      if (gpio == 4 || gpio == 5) syncSlave(true);
      if (gpio == 115 || gpio == 116) syncSlave(false);
      return;
    }
    if (inRange(pa, kCci0, kCci0Bytes)) return cciWrite(uint32_t(pa - kCci0), value);
    // CBCRs, GDSCs, RCGs, the PLL: by address.
    const uint64_t g = kCamCc + kGdscTitanTop;
    if (pa == g) {
      r[pa] = value;
      const bool on = !(value & kGdscCollapse) && running(kCamCc + kCcGdscClk) && !titanStuck;
      if (on) {
        r[pa] |= kGdscPwrOn;
        r[pa + 4] = kGdscCfgUpDone;
      } else {
        r[pa] &= ~kGdscPwrOn;
        r[pa + 4] = (value & kGdscCollapse) ? kGdscCfgDownDone : 0;
        if (!(value & kGdscCollapse)) {
          // stays off: the branches under it stop
        }
      }
      return;
    }
    if (isCbcr(pa)) {
      const bool mclk1 = pa == kMclkCc + mclkBranch(1), mclk4 = pa == kMclkCc + mclkBranch(4);
      const bool was = running(pa);
      r[pa] = (value & ~kCbcrClkOff) | (branchCanRun(pa, value) ? 0 : kCbcrClkOff);
      if (mclk1 && running(pa) && !was) at[tRearMclk] = int64_t(now);
      if (mclk1 && !running(pa) && was) at[tRearMclkOff] = int64_t(now);
      if (mclk4 && running(pa) && !was) at[tFrontMclk] = int64_t(now);
      if (mclk4 && !running(pa) && was) at[tFrontMclkOff] = int64_t(now);
      return;
    }
    if (isRcgCmd(pa)) {
      uint32_t x = value & ~kRcgUpdate;   // the update completes at once
      if (value & kRcgRootEn) x &= ~kRcgRootOff;
      else x |= kRcgRootOff;
      r[pa] = x;
      return;
    }
    if (pa == kMclkCc + kMclkPll || pa == kMclkCc + kMclkPll + 4) {
      r[pa] = value;
      const uint32_t mode = v(kMclkCc + kMclkPll), op = v(kMclkCc + kMclkPll + 4);
      const bool lock = (mode & 4u) && (op & 1u) && v(kMclkCc + kMclkPll + kPllRivianL) == 0x32 &&
                        v(kMclkCc + kMclkPll + 0x1c) == 0x12000000 && !pllNoLock;
      r[kMclkCc + kMclkPll] = (mode & ~kPllLockDet) | (lock ? kPllLockDet : 0);
      return;
    }
    r[pa] = value;
  }
  bool isCbcr(uint64_t pa) const
  {
    for (uint32_t o : {kGccCameraAhb, kGccCameraXo, kGccMclkAhb})
      if (pa == kGcc + o) return true;
    for (const Reg &b : kCcBranches)
      if (pa == kCamCc + b.off) return true;
    for (unsigned n = 0; n < kMclkCount; ++n)
      if (pa == kMclkCc + mclkBranch(n)) return true;
    return false;
  }
  bool isRcgCmd(uint64_t pa) const
  {
    for (const Reg &c : kCcRcgs)
      if (pa == kCamCc + c.off) return true;
    for (unsigned n = 0; n < kMclkCount; ++n)
      if (pa == kMclkCc + mclkRcg(n)) return true;
    return false;
  }
  bool branchCanRun(uint64_t pa, uint32_t value) const
  {
    if (!(value & kCbcrEnable)) return false;
    // A branch under TITAN_TOP runs only with it powered; the MCLK branch at 24 MHz only with the PLL locked.
    if (pa == kCamCc + kCcCamTopAhb || pa == kCamCc + kCcCci0) return titan();
    if (pa == kMclkCc + mclkBranch(1) && rcgSource(v(kMclkCc + mclkRcg(1) + 4)) == 3)
      return v(kMclkCc + kMclkPll) & kPllLockDet;
    return true;
  }

  // --------------------------------------------------------------------------------------------------------------- CCI
  void cciWrite(uint32_t off, uint32_t value)
  {
    const unsigned m = kCciMaster;
    if (off == kCciResetCmd) {
      queue[0].clear();
      queue[1].clear();
      r[kCci0 + kCciIrqStatus] |= kCciRstDone;
      r[kCci0 + cciCurCount(m, 0)] = r[kCci0 + cciCurCount(m, 1)] = 0;
      return;
    }
    if (off == kCciHaltReq) {
      r[kCci0 + kCciIrqStatus] |= cciHaltAck(m);
      return;
    }
    if (off == kCciIrqClear) {
      r[kCci0 + kCciIrqStatus] &= ~value;
      return;
    }
    for (unsigned q = 0; q < 2; ++q)
      if (off == cciLoad(m, q)) {
        queue[q].push_back(value);
        r[kCci0 + cciCurCount(m, q)] = uint32_t(queue[q].size());
        return;
      }
    if (off == kCciQueueStart) {
      ++cciStarts;
      for (unsigned q = 0; q < 2; ++q)
        if (value & (1u << (2 * m + q))) runQueue(q);
      return;
    }
    r[kCci0 + off] = value;
  }
  void runQueue(unsigned q)
  {
    const unsigned m = kCciMaster;
    if (v(kCci0 + cciExecCount(m, q)) != queue[q].size()) violations.push_back("CCI queue started without EXEC_WORD_CNT");
    uint8_t address = 0;
    uint32_t status = 0;
    bool error = false;
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < queue[q].size() && !error; ++i) {
      const uint32_t w = queue[q][i], cmd = w & 0xf;
      if (cmd == kCciSetParam) {
        address = uint8_t((w >> 4) & 0x7f);
      } else if (cmd == kCciWrite) {
        const unsigned count = (w >> 4) & 0xf;
        bytes.clear();
        for (unsigned b = 1; b <= count; ++b) {
          const size_t word = i + b / 4;
          bytes.push_back(uint8_t(queue[q][word] >> (8 * (b % 4))));
        }
        i += (count + 1 + 3) / 4 - 1;
        if (address != rearSlave.address || !sensorReady(true)) {
          error = true;
          status |= cciNack(m, q) | (1u << 13);
          break;
        }
        if (count >= 2) rearSlave.pointer = uint16_t(bytes[0] << 8 | bytes[1]);
        for (unsigned b = 2; b < count; ++b) rearSlave.reg[rearSlave.pointer++] = bytes[b];
      } else if (cmd == kCciRead) {
        const unsigned count = (w >> 4) & 0xf;
        if (address != rearSlave.address || !sensorReady(true)) {
          error = true;
          status |= cciNack(m, q) | (1u << 13);
          break;
        }
        std::vector<uint8_t> data = {uint8_t(address << 1 | 1)};
        for (unsigned b = 0; b < count; ++b) {
          const uint16_t p = rearSlave.pointer++;
          uint8_t val = rearSlave.reg.count(p) ? rearSlave.reg[p] : 0;
          if (p == 0x0000) val = uint8_t(rearId >> 8);
          if (p == 0x0001) val = uint8_t(rearId);
          data.push_back(val);
        }
        readFifo.clear();
        for (size_t b = 0; b < data.size(); b += 4) {
          uint32_t word = 0;
          for (size_t k = 0; k < 4 && b + k < data.size(); ++k) word |= uint32_t(data[b + k]) << (8 * k);
          readFifo.push_back(word);
        }
        if (count % 4 == 3) readFifo.push_back(0);   // the controller's count / 4 + 1 words
        r[kCci0 + cciReadLevel(m)] = uint32_t(readFifo.size());
        status |= cciRdDone(m);
      } else if (cmd == kCciReport) {
        status |= cciQReport(m, q);
      }
    }
    queue[q].clear();
    r[kCci0 + cciCurCount(m, q)] = 0;
    r[kCci0 + kCciIrqStatus] |= status;
  }

  // ------------------------------------------------------------------------------------------------------------- PMIC
  int apidOf(uint16_t ppid) const
  {
    for (const PmicLdo &l : ldo)
      if (uint16_t(l.sid << 8 | l.page) == ppid) return l.apid;
    return -1;
  }
  PmicLdo *byApid(int apid)
  {
    for (PmicLdo &l : ldo)
      if (l.apid == apid) return &l;
    return nullptr;
  }
  int pmicRead(int apid, uint8_t reg, uint8_t *value, unsigned n)
  {
    PmicLdo *l = byApid(apid);
    if (!l) return -5;
    for (unsigned i = 0; i < n; ++i) {
      const uint8_t a = uint8_t(reg + i);
      value[i] = a == kPmicType ? kPmicTypeLdo : a == kPmicSubtype ? l->subtype : a == kPmicStatus1 ? ((l->en & 0x80) ? kPmicReady : 0)
               : a == kPmicVset ? uint8_t(l->mv) : a == kPmicVset + 1 ? uint8_t(l->mv >> 8) : a == kPmicMode ? l->mode
               : a == kPmicEnCtl ? l->en : 0;
    }
    return 0;
  }
  int vrm(unsigned rail, const VrmCmd *cmds, unsigned n)
  {
    ++railVotes;
    if (int(rail) == vrmFail) return -60;
    if (!kRails[rail].voted) violations.push_back("a read-only rail voted");
    PmicLdo &l = ldo[rail];
    char line[96];
    std::string s = kRails[rail].cmdDb;
    for (unsigned i = 0; i < n; ++i) {
      std::snprintf(line, sizeof line, " %u=%u", cmds[i].reg, cmds[i].value);
      s += line;
      if (cmds[i].reg == kVrmVoltage) l.mv = (int(rail) == wrongMv) ? wrongMvValue : uint16_t(cmds[i].value);
      if (cmds[i].reg == kVrmMode) l.mode = cmds[i].value == kVrmModeHpm ? 0x07 : 0x05;
      if (cmds[i].reg == kVrmEnable) {
        const bool was = l.en & 0x80;
        l.en = cmds[i].value ? 0x80 : 0;
        if (!was && cmds[i].value) {
          if (rail == kRailRearVddd) at[tRearVddd] = int64_t(now);
          if (rail == kRailRearVddio) at[tRearVddio] = int64_t(now);
          if (rail == kRailRearAfvdd) at[tRearAfvdd] = int64_t(now);
          if (rail == kRailFrontVddio) at[tFrontVddio] = int64_t(now);
          if (rail == kRailFrontDvdd) at[tFrontDvdd] = int64_t(now);
        }
        if (was && !cmds[i].value) {
          if (rail == kRailRearVddd) at[tRearVdddOff] = int64_t(now);
          if (rail == kRailFrontVddio || rail == kRailFrontDvdd) at[tFrontRailOff] = int64_t(now);
        }
      }
    }
    vrmLog.push_back(s);
    return 0;
  }
  // The snapshot level 2 must leave unchanged: GCC/CAMCC/MCLKCC/TLMM registers and the rails' enables and modes. CCI0's own
  // registers are left as the reset made them (not compared); the PLL's lock bit is derived.
  std::map<uint64_t, uint32_t> snapshot() const
  {
    std::map<uint64_t, uint32_t> s;
    for (auto &[pa, val] : r)
      if (!inRange(pa, kCci0, kCci0Bytes)) s[pa] = val;
    // The rails' enables (a withdrawn vote leaves the mode and set point votes, as PianoWcnRails.h does: compared apart).
    for (unsigned i = 0; i < ldo.size(); ++i) s[0xf000000000ull + i] = uint32_t(ldo[i].en) << 8;
    return s;
  }
};

static bool same(const std::map<uint64_t, uint32_t> &a, const std::map<uint64_t, uint32_t> &b)
{
  bool ok = true;
  for (auto &[pa, v] : b) {
    auto it = a.find(pa);
    if (it == a.end() || it->second != v) {
      std::printf("  changed: %#llx: %08x -> %08x\n", (unsigned long long)pa, it == a.end() ? 0xdeadbeefu : it->second, v);
      ok = false;
    }
  }
  return ok;
}

// ---------------------------------------------------------------------------------------------------------- the ops
struct TestOps {
  explicit TestOps(Board &board) : b(board) {}
  Board &b;
  std::vector<std::string> notes;
  std::vector<int> steps;
  unsigned gccLocks = 0, gccUnlocks = 0;
  uint32_t rd(uint64_t pa) { return b.rd(pa); }
  void wr(uint64_t pa, uint32_t v, const char *) { b.wr(pa, v); }
  void wrQuiet(uint64_t pa, uint32_t v)
  {
    ++b.quietStores;
    b.wr(pa, v);
  }
  void udelay(unsigned us)
  {
    b.now += us;
    b.syncSlave(true);
    b.syncSlave(false);
  }
  void msleep(unsigned ms) { udelay(ms * 1000); }
  int vrm(unsigned rail, const VrmCmd *cmds, unsigned n) { return b.vrm(rail, cmds, n); }
  int pmic(unsigned rail, RailState &r)
  {
    r = RailState();
    r.found = true;
    r.apid = b.ldo[rail].apid;
    uint8_t st = 0, vs[2], me[2];
    b.pmicRead(r.apid, kPmicStatus1, &st, 1);
    b.pmicRead(r.apid, kPmicVset, vs, 2);
    b.pmicRead(r.apid, kPmicMode, me, 2);
    r.status = st;
    r.mv = uint16_t(vs[0] | vs[1] << 8);
    r.mode = me[0];
    r.en = me[1];
    r.read = true;
    return 0;
  }
  bool gccLock(bool take)
  {
    (take ? gccLocks : gccUnlocks)++;
    return true;
  }
  void step(int n, const char *) { steps.push_back(n); }
  void note(const char *fmt, ...)
  {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    notes.push_back(line);
  }
};

static uint32_t bRd(void *c, uint64_t pa) { return static_cast<Board *>(c)->rd(pa); }
static int bApid(void *c, uint16_t ppid, int *apid, uint32_t *owner)
{
  *apid = static_cast<Board *>(c)->apidOf(ppid);
  *owner = 0;
  return *apid < 0 ? -19 : 0;
}
static int bPmicRead(void *c, int apid, uint8_t reg, uint8_t *v, unsigned n) { return static_cast<Board *>(c)->pmicRead(apid, reg, v, n); }
static int bCmdDb(void *c, const char *name, uint32_t *address)
{
  Board *b = static_cast<Board *>(c);
  for (unsigned i = 0; i < kRailCount; ++i)
    if (!std::strcmp(kRails[i].cmdDb, name)) {
      if (b->noCmdDb[i]) return -2;
      *address = 0x30000u + 0x100u * i;
      return 0;
    }
  return -2;
}

struct Rig {
  Board b;
  TestOps ops{b};
  Loader loader{&b, bRd};
  Pmic pmic{&b, bApid, bPmicRead, bCmdDb};
  SurveyOptions so;
  Survey survey() { return pianocamera::survey(loader, pmic, so); }
  SensorResult run(Sensor w, const PowerOptions &po)
  {
    Survey s = survey();
    Session<TestOps> session(ops, po);
    return session.run(w, loader, pmic, so, s);
  }
};

// --------------------------------------------------------------------------------------------------------------- tests
static void testSurvey()
{
  Rig r;
  const Survey s = r.survey();
  CHECK(r.b.stores == 0);
  CHECK(r.b.violations.empty());
  CHECK(s.cc.read && !s.cc.titanOn());
  CHECK(s.mclk.read);
  CHECK(!s.cci.read && !std::strcmp(s.cci.why, "TITAN_TOP GDSC off"));
  for (const BlockState &b : s.blocks) CHECK(!b.read);
  CHECK(!s.se1.read);
  for (unsigned i = 0; i < kRailCount; ++i) {
    CHECK(s.rails[i].found && s.rails[i].read && s.rails[i].address);
    CHECK(s.rails[i].alt == (kRails[i].sid == 8));
    CHECK(s.rails[i].enabled() == !kRails[i].voted);
  }
  CHECK(s.cc.rcg[0].cfg == 0x20f && rcgSource(s.cc.rcg[0].cfg) == 2);
  CHECK(s.smmu.read && s.smmu.count == 0 && s.smmu.groups == 64);
  CHECK(!s.pins[kPinRearReset].cfg || !pinDrivenHigh(s.pins[kPinRearReset].cfg, s.pins[kPinRearReset].io));
  BusChoice bus;
  CHECK(sensorRefusal(s, Sensor::rear, bus) == nullptr);
  CHECK(sensorRefusal(s, Sensor::front, bus) == nullptr);

  // Without GCC's camera AHB, CAMCC is never touched (and so neither is anything under it).
  Rig r2;
  r2.b.r[kGcc + kGccCameraAhb] = 0x80000000u;
  r2.b.r[kGcc + kGccMclkAhb] = 0x80000000u;
  const Survey s2 = r2.survey();
  CHECK(r2.b.violations.empty());
  CHECK(!s2.cc.read && !s2.mclk.read && !s2.cci.read);
  CHECK(sensorRefusal(s2, Sensor::front, bus) != nullptr);

  // With TITAN_TOP and the clocks on (another owner), CCI0 and the CSIDs are read; a group covering the stream is listed.
  Rig r3;
  r3.b.r[kCamCc + kCcGdscClk] = 1;
  r3.b.wr(kCamCc + kGdscTitanTop, 0);
  r3.b.r[kCamCc + kCcCamTopAhb] = 1;
  r3.b.r[kCamCc + kCcCci0] = 1;
  r3.b.r[kCamCc + kCcCsid] = 1;
  r3.b.r[kSmmu + 0x800 + 4 * 5] = 0x80000000u | 0x1c00;
  r3.b.r[kSmmu + 0xc00 + 4 * 5] = 1u << 16;
  r3.b.stores = 0;
  const Survey s3 = r3.survey();
  CHECK(r3.b.violations.empty());
  CHECK(s3.cci.read && s3.cci.version == 0x20000000);
  CHECK(s3.blocks[0].read && s3.blocks[1].read && !s3.blocks[5].read);
  CHECK(s3.smmu.count == 1 && s3.smmu.found[0].index == 5 && ((s3.smmu.found[0].s2cr >> 16) & 3) == 1);
  CHECK(r3.b.stores == 0);

  // Refusals: a reset line driven high, an MCLK already on, a rail missing from cmd-db or found at another voltage.
  Rig r4;
  r4.b.r[tlmmIo(6)] = kTlmmOut;
  CHECK(sensorRefusal(r4.survey(), Sensor::rear, bus) != nullptr);
  Rig r5;
  r5.b.r[kMclkCc + mclkBranch(4)] = 1;
  CHECK(sensorRefusal(r5.survey(), Sensor::front, bus) != nullptr);
  Rig r6;
  r6.b.noCmdDb[kRailFrontDvdd] = true;
  CHECK(sensorRefusal(r6.survey(), Sensor::front, bus) != nullptr);
  CHECK(sensorRefusal(r6.survey(), Sensor::rear, bus) == nullptr);
  Rig r7;
  r7.b.ldo[kRailRearVddio].en = 0x80;
  r7.b.ldo[kRailRearVddio].mv = 1200;
  CHECK(sensorRefusal(r7.survey(), Sensor::rear, bus) != nullptr);
}

// The drivers' order and waits, from the board's event times.
static void checkRearTiming(const Board &b)
{
  using B = Board;
  CHECK(b.at[B::tRearVddd] >= 0 && b.at[B::tRearVdda] >= b.at[B::tRearVddd] + 1000);
  CHECK(b.at[B::tRearVddio] >= b.at[B::tRearVdda] && b.at[B::tRearAfvdd] >= b.at[B::tRearVddio]);
  CHECK(b.at[B::tRearMclk] >= b.at[B::tRearAfvdd] && b.at[B::tRearReset] >= b.at[B::tRearMclk]);
  CHECK(b.at[B::tRearResetLow] >= b.at[B::tRearReset] && b.at[B::tRearMclkOff] >= b.at[B::tRearResetLow]);
  CHECK(b.at[B::tRearVdddOff] >= b.at[B::tRearVddaOff] + 1000 && b.at[B::tRearVddaOff] >= b.at[B::tRearMclkOff]);
}
static void checkFrontTiming(const Board &b)
{
  using B = Board;
  const int64_t supplies = std::max(std::max(b.at[B::tFrontVddio], b.at[B::tFrontDvdd]), b.at[B::tFrontAvdd]);
  CHECK(b.at[B::tFrontVddio] >= 0 && b.at[B::tFrontDvdd] >= 0 && b.at[B::tFrontAvdd] >= 0);
  CHECK(b.at[B::tFrontMclk] >= supplies + 5000);
  CHECK(b.at[B::tFrontReset] >= b.at[B::tFrontMclk] + 10000);
  CHECK(b.at[B::tFrontResetLow] >= b.at[B::tFrontReset] && b.at[B::tFrontMclkOff] >= b.at[B::tFrontResetLow]);
  CHECK(b.at[B::tFrontRailOff] >= b.at[B::tFrontMclkOff]);
}

static void testRearCci()
{
  Rig r;
  const auto before = r.b.snapshot();
  PowerOptions po;
  const SensorResult res = r.run(Sensor::rear, po);
  CHECK(res.idOk && res.id == 0x38e1 && res.tries == 1);
  CHECK(res.cleanOff && res.refusedStores == 0);
  CHECK(res.cciVersion == 0x20000000);
  CHECK(r.b.violations.empty());
  for (auto &v : r.b.violations) std::printf("  violation: %s\n", v.c_str());
  CHECK(same(before, r.b.snapshot()));
  checkRearTiming(r.b);
  // Each voted rail: HPM, its exact voltage, enabled in one transaction; then the enable withdrawn.
  CHECK(r.b.vrmLog.size() == 6);
  if (r.b.vrmLog.size() == 6) {
    CHECK(r.b.vrmLog[0] == "ldoi1 8=7 0=1096 4=1");
    CHECK(r.b.vrmLog[1] == "ldob6 8=7 0=1800 4=1");
    CHECK(r.b.vrmLog[2] == "ldob7 8=7 0=2800 4=1");
    CHECK(r.b.vrmLog[3] == "ldob7 4=0" && r.b.vrmLog[4] == "ldob6 4=0" && r.b.vrmLog[5] == "ldoi1 4=0");
  }
  CHECK(r.ops.gccLocks == r.ops.gccUnlocks);
  CHECK(r.b.quietStores == 0);
  // The steps in order, ending with the power-down.
  CHECK(!r.ops.steps.empty() && r.ops.steps.back() == kStepDown);
  for (size_t i = 1; i < r.ops.steps.size(); ++i) CHECK(r.ops.steps[i] > r.ops.steps[i - 1]);
}

static void testRearGpio()
{
  Rig r;
  const auto before = r.b.snapshot();
  PowerOptions po;
  po.rearCci = false;
  const SensorResult res = r.run(Sensor::rear, po);
  CHECK(res.idOk && res.id == 0x38e1);
  CHECK(res.cleanOff && r.b.violations.empty());
  CHECK(same(before, r.b.snapshot()));
  CHECK(r.b.quietStores > 50);
  CHECK(r.b.cciStarts == 0);
  checkRearTiming(r.b);
  // No CAMCC store at all on this path.
  CHECK(r.b.v(kCamCc + kGdscTitanTop) == 0x00222001);
}

static void testRearMclk24()
{
  Rig r;
  const auto before = r.b.snapshot();
  PowerOptions po;
  po.mclk24 = true;
  const SensorResult res = r.run(Sensor::rear, po);
  CHECK(res.idOk && res.mclkHz == 24000000);
  CHECK(res.cleanOff && r.b.violations.empty());
  // Everything as found but the PLL's configuration words, which stay programmed (Linux writes them once, at its probe), and its
  // MODE's RESET_N | BYPASSNL (the configured standby state).
  auto after = r.b.snapshot(), was = before;
  for (uint32_t o : {0x10u, 0x1cu, 0x20u, 0x24u}) {
    after.erase(kMclkCc + kMclkPll + o);
    was.erase(kMclkCc + kMclkPll + o);
  }
  CHECK(after[kMclkCc + kMclkPll] == 6u);
  after[kMclkCc + kMclkPll] = was[kMclkCc + kMclkPll];
  CHECK(same(was, after));
  // The PLL is in standby, its outputs off, the RCG back on the XO.
  CHECK(!(r.b.v(kMclkCc + kMclkPll) & kPllOutctrl) && r.b.v(kMclkCc + kMclkPll + 4) == 0);
  CHECK(r.b.v(kMclkCc + mclkRcg(1) + 4) == 0);
  CHECK(r.b.v(kMclkCc + kMclkPll + kPllRivianL) == 0x32 && r.b.v(kMclkCc + kMclkPll + 0x1c) == 0x12000000);
  // 24 MHz from the settings level 2 programs.
  CHECK(rcgRate(960000000ull, 19u | 3u << 8 | 2u << 12, 1, 0xfc, 8) == 24000000ull);
  CHECK(rcgRate(kXoHz, 0, 0, 0, 8) == kXoHz);
  // A PLL that never locks: the step fails, everything is taken down.
  Rig r2;
  r2.b.pllNoLock = true;
  const auto before2 = r2.b.snapshot();
  const SensorResult f = r2.run(Sensor::rear, po);
  CHECK(!f.idOk && f.failure == Failure::pll && f.failedStep == -kStepMclk && f.cleanOff);
  for (unsigned i = 0; i < kRailCount; ++i) CHECK(r2.b.ldo[i].en == before2.at(0xf000000000ull + i) >> 8);
  CHECK(r2.b.v(kMclkCc + mclkRcg(1) + 4) == 0 && !(r2.b.v(kMclkCc + mclkBranch(1)) & kCbcrEnable));
}

static void testFront()
{
  Rig r;
  const auto before = r.b.snapshot();
  PowerOptions po;
  const SensorResult res = r.run(Sensor::front, po);
  CHECK(res.idOk && res.id == 0x5632 && res.tries == 1);
  CHECK(res.cleanOff && r.b.violations.empty());
  for (auto &v : r.b.violations) std::printf("  violation: %s\n", v.c_str());
  CHECK(same(before, r.b.snapshot()));
  checkFrontTiming(r.b);
  CHECK(r.b.vrmLog.size() == 4);
  if (r.b.vrmLog.size() == 4) {
    CHECK(r.b.vrmLog[0] == "ldob10 8=7 0=1800 4=1" && r.b.vrmLog[1] == "ldog2 8=7 0=1200 4=1");
    CHECK(r.b.vrmLog[2] == "ldog2 4=0" && r.b.vrmLog[3] == "ldob10 4=0");
  }
  // The front path never touches CAMCC, GCC's camera AHB/XO or CCI0.
  CHECK(r.b.v(kCamCc + kGdscTitanTop) == 0x00222001 && r.b.cciStarts == 0);
  // gpio7's egpio bit was not ours: set while in use (as Linux claims the pin), back as found after.
  CHECK(!(r.b.v(tlmmCfg(7)) & kTlmmEgpioEnable));
}

static void testFailures()
{
  PowerOptions po;
  // A missing rear sensor: NACK, three tries, a clean power-down.
  {
    Rig r;
    r.b.rearAbsent = true;
    const auto before = r.b.snapshot();
    const SensorResult res = r.run(Sensor::rear, po);
    CHECK(!res.idOk && res.failure == Failure::idRead && res.cci == CciResult::nack && res.tries == 3);
    CHECK(res.cleanOff && same(before, r.b.snapshot()) && r.b.violations.empty());
  }
  // The same without the front sensor (software I2C: no ACK for the address), five tries.
  {
    Rig r;
    r.b.frontAbsent = true;
    const auto before = r.b.snapshot();
    const SensorResult res = r.run(Sensor::front, po);
    CHECK(!res.idOk && res.failure == Failure::idRead && res.i2c == I2c::nackAddress && res.tries == 5);
    CHECK(res.cleanOff && same(before, r.b.snapshot()) && r.b.violations.empty());
  }
  // Another chip at the address.
  {
    Rig r;
    r.b.rearId = 0x30d5;
    const SensorResult res = r.run(Sensor::rear, po);
    CHECK(!res.idOk && res.failure == Failure::idMismatch && res.id == 0x30d5 && res.cleanOff);
  }
  // A rail that reads back at another voltage: the step fails at once, the rails voted so far are withdrawn.
  {
    Rig r;
    r.b.wrongMv = kRailRearVddio;
    r.b.wrongMvValue = 1200;
    const auto before = r.b.snapshot();
    const SensorResult res = r.run(Sensor::rear, po);
    CHECK(!res.idOk && res.failure == Failure::railVoltage && res.failedStep == -kStepRails && res.cleanOff);
    CHECK(same(before, r.b.snapshot()));
    CHECK(r.b.at[Board::tRearMclk] < 0 && r.b.at[Board::tRearReset] < 0);   // never clocked, never out of reset
  }
  // A vote that does not complete.
  {
    Rig r;
    r.b.vrmFail = kRailFrontDvdd;
    const auto before = r.b.snapshot();
    const SensorResult res = r.run(Sensor::front, po);
    CHECK(!res.idOk && res.failure == Failure::rail && res.cleanOff && same(before, r.b.snapshot()));
  }
  // TITAN_TOP stays off: the CCI clocks never come on, nothing under TITAN_TOP is touched.
  {
    Rig r;
    r.b.titanStuck = true;
    const auto before = r.b.snapshot();
    const SensorResult res = r.run(Sensor::rear, po);
    CHECK(!res.idOk && res.failure == Failure::gdsc && res.failedStep == -kStepBus);
    CHECK(r.b.violations.empty() && res.cleanOff);
    for (auto &v : r.b.violations) std::printf("  violation: %s\n", v.c_str());
    CHECK(same(before, r.b.snapshot()));
    CHECK(r.b.railVotes == 0);   // the bus comes first: no rail was voted
  }
  // A bus held low: the software I2C gives up, the sensor is powered down.
  {
    Rig r;
    r.b.sdaStuck = true;
    const auto before = r.b.snapshot();
    const SensorResult res = r.run(Sensor::front, po);
    CHECK(!res.idOk && res.failure == Failure::idRead && res.i2c == I2c::busBusy && res.cleanOff && same(before, r.b.snapshot()));
  }
  // piano-camera-stop: after the rails, powered down.
  {
    Rig r;
    PowerOptions stop = po;
    stop.stopAt = kStepRails;
    const auto before = r.b.snapshot();
    const SensorResult res = r.run(Sensor::rear, stop);
    CHECK(!res.idOk && res.failure == Failure::stopped && res.cleanOff && same(before, r.b.snapshot()));
    CHECK(r.b.vrmLog.size() == 6);
  }
  // A refusal: nothing is stored at all (GCC's branches were on already).
  {
    Rig r;
    r.b.r[tlmmIo(7)] = kTlmmOut;
    r.b.r[tlmmCfg(7)] |= kTlmmOe;
    r.b.stores = 0;
    const SensorResult res = r.run(Sensor::front, po);
    CHECK(res.failure == Failure::refused && res.why && r.b.stores == 0 && r.b.railVotes == 0);
  }
  // A rail found on at its voltage: used, never voted, never withdrawn.
  {
    Rig r;
    r.b.ldo[kRailFrontVddio].en = 0x80;
    r.b.ldo[kRailFrontVddio].mv = 1800;
    const SensorResult res = r.run(Sensor::front, po);
    CHECK(res.idOk && res.cleanOff && !res.rails[0].voted && r.b.ldo[kRailFrontVddio].en == 0x80);
    CHECK(r.b.vrmLog.size() == 2);
  }
  // GCC's MCLK controller AHB found off: turned on for the run, off again after (and the survey then reads the controller).
  {
    Rig r;
    r.b.r[kGcc + kGccMclkAhb] = 0x80000000u;
    const auto before = r.b.snapshot();
    const SensorResult res = r.run(Sensor::front, po);
    CHECK(res.idOk && res.cleanOff && same(before, r.b.snapshot()) && r.b.violations.empty());
    CHECK(r.ops.gccLocks == 2 && r.ops.gccUnlocks == 2);
  }
}

static void testAllowlist()
{
  Board b;
  TestOps ops{b};
  Session<TestOps> s(ops, PowerOptions());
  CHECK(s.writable(kGcc + kGccCameraAhb) && s.writable(tlmmCfg(176)) && s.writable(tlmmIo(4)) && s.writable(kCci0 + kCciResetCmd));
  CHECK(!s.writable(kGcc + kGccQup2Vote) && !s.writable(kGcc + kGccCameraHfAxi) && !s.writable(tlmmCfg(16)) && !s.writable(tlmmCfg(8)));
  CHECK(!s.writable(kCamCc + kGdscTfe0) && !s.writable(kCamCc + kCcCsid) && !s.writable(kMclkCc + mclkBranch(0)));
  CHECK(!s.writable(kBlocks[0].pa) && !s.writable(kSmmu) && !s.writable(kCci0 + 0x1000) && !s.writable(kCci0 + 2));
  CHECK(!s.writable(kCmdDb) && !s.writable(kRscDrv2));
}

// The CCI master alone: packing, a 4-byte read, NACK recovery, a queue that never finishes.
struct CciModel {
  Board b;
  CciModel()
  {
    b.r[kCamCc + kCcGdscClk] = 1;
    b.wr(kCamCc + kGdscTitanTop, 0);
    b.r[kCamCc + kCcCamTopAhb] = 1;
    b.r[kCamCc + kCcCci0] = 1;
  }
};
static uint32_t mRd(void *c, uint32_t off) { return static_cast<Board *>(c)->rd(kCci0 + off); }
static std::vector<std::pair<uint32_t, uint32_t>> gCciWrites;
static void mWr(void *c, uint32_t off, uint32_t v, const char *)
{
  gCciWrites.push_back({off, v});
  static_cast<Board *>(c)->wr(kCci0 + off, v);
}
static void mDelay(void *c, unsigned us) { static_cast<Board *>(c)->now += us; }
static void testCci()
{
  CciModel m;
  // the rear sensor "ready": powered and out of reset long ago
  m.b.ldo[kRailRearVddd].en = m.b.ldo[kRailRearVddio].en = 0x80;
  m.b.r[tlmmCfg(175)] = kTlmmOe;
  m.b.r[tlmmIo(175)] = kTlmmOut;
  m.b.r[tlmmCfg(6)] = kTlmmOe;
  m.b.r[tlmmIo(6)] = kTlmmOut;
  m.b.r[kMclkCc + mclkBranch(1)] = 1;
  m.b.at[Board::tRearReset] = 0;
  m.b.now = 100000;
  CciMaster c(CciIo{&m.b, mRd, mWr, mDelay});
  CHECK(c.init(kCciXo400k) == CciResult::ok && c.version == 0x20000000);
  CHECK(m.b.v(kCci0 + cciSclCtl(1)) == (20u << 16 | 28u) && m.b.v(kCci0 + cciMiscCtl(1)) == (6u << 4 | 3u));
  gCciWrites.clear();
  const uint8_t w[4] = {0x01, 0x00, 0x00, 0x01};
  CHECK(c.write(0x10, w, 4) == CciResult::ok);
  // SET_PARAM 0x101, then WRITE|4<<4 with 3 bytes in the first word and 1 in the second, REPORT|IRQ.
  std::vector<uint32_t> loads;
  for (auto &[off, v] : gCciWrites)
    if (off == cciLoad(1, 0)) loads.push_back(v);
  CHECK(loads.size() == 4 && loads[0] == 0x101 && loads[1] == (0x49u | 0x01u << 8 | 0x00u << 16 | 0x00u << 24) && loads[2] == 0x01 &&
        loads[3] == 0x108);
  CHECK(m.b.rearSlave.reg[0x0100] == 0x00 && m.b.rearSlave.reg[0x0101] == 0x01);
  m.b.rearSlave.reg[0x0002] = 0xa0;
  m.b.rearSlave.reg[0x0003] = 0x05;
  uint8_t out[4] = {};
  CHECK(c.readReg(0x10, 0x0000, 2, out, 4) == CciResult::ok);
  CHECK(out[0] == 0x38 && out[1] == 0xe1 && out[2] == 0xa0 && out[3] == 0x05);
  // NACK: halt, reset of master 1, the failure reported; the next transfer works again.
  const unsigned resets = c.resets;
  CHECK(c.readReg(0x11, 0x0000, 2, out, 2) == CciResult::nack && c.resets == resets + 1);
  CHECK(c.readReg(0x10, 0x0000, 2, out, 2) == CciResult::ok && out[0] == 0x38);
  CHECK(c.write(0x10, w, 12) == CciResult::tooLong);
}

static void testTables()
{
  // The generated header against data/camera/MANIFEST's counts (the build passes MANIFEST's path).
  const char *man = std::getenv("PIANO_CAMERA_MANIFEST");
  CHECK(man != nullptr);
  if (!man) return;
  FILE *f = std::fopen(man, "r");
  CHECK(f != nullptr);
  if (!f) return;
  char line[512];
  unsigned checked = 0;
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    char file[32], sha[80];
    int at = 0;
    if (std::sscanf(line, "%31s %79s %n", file, sha, &at) != 2) continue;
    std::string sensor(file);
    sensor = sensor.substr(0, sensor.find('.'));
    const char *p = line + at;
    char item[80];
    int used = 0;
    while (std::sscanf(p, "%79s%n", item, &used) == 1) {
      p += used;
      std::string it(item);
      const size_t colon = it.find(':'), eq = it.find('=');
      const std::string kind = it.substr(0, colon), name = it.substr(colon + 1, eq - colon - 1);
      const unsigned count = unsigned(std::atoi(it.c_str() + eq + 1));
      bool found = false;
      for (const CameraTableSection &s : kCameraTableSections)
        if (sensor == s.sensor && kind == s.kind && name == s.name) {
          found = true;
          unsigned writes = 0;
          for (uint32_t k = s.first; k < s.first + s.words; k += 2)
            if ((kCameraTableWords[k] >> 16) != 0xffff) ++writes;
          CHECK(writes == count);
          ++checked;
        }
      CHECK(found);
    }
  }
  std::fclose(f);
  CHECK(checked == sizeof kCameraTableSections / sizeof kCameraTableSections[0]);
  // Spot checks: the rear's init starts with the page register; each mode's parameters; delays decode.
  const CameraTableSection *init = nullptr;
  for (const CameraTableSection &s : kCameraTableSections)
    if (!std::strcmp(s.sensor, "s5kjn1") && !std::strcmp(s.kind, "init")) init = &s;
  CHECK(init && kCameraTableWords[init->first] == (2u << 30 | 0x6028) && kCameraTableWords[init->first + 1] == 0x2400);
  bool rearMode = false, frontMode = false;
  for (const CameraTableMode &m : kCameraTableModes) {
    if (!std::strcmp(m.sensor, "s5kjn1") && m.width == 4080) rearMode = m.height == 3072 && m.a == 4352 && m.b == 4288;
    if (!std::strcmp(m.sensor, "ov32d40") && m.width == 3264 && m.height == 2448) frontMode = m.a == 4728 && m.b == 2820;
  }
  CHECK(rearMode && frontMode);
  for (uint32_t k = 0; k < sizeof kCameraTableWords / sizeof kCameraTableWords[0]; k += 2) {
    const uint32_t a = kCameraTableWords[k];
    if ((a >> 16) == 0xffff) CHECK((a & 0xffff) * 100 <= kCameraTableWords[k + 1]);
    else CHECK((a >> 30) >= 1 && ((a >> 16) & 0x3fff) == 0);
  }
}

int main()
{
  testSurvey();
  testRearCci();
  testRearGpio();
  testRearMclk24();
  testFront();
  testFailures();
  testAllowlist();
  testCci();
  testTables();
  if (failures) {
    std::printf("camera_host: %d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("camera_host: all checks passed\n");
  return 0;
}
