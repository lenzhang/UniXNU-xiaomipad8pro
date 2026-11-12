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
#include "CameraStream.h"
#include "CameraPlan.h"
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
  bool requireMxc = false, mxcVoted = false;   // CAMCC stalls without MXC (the tablet's case is unknown: the strict model)
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
  // Level 3: CAMCC PLL0, the CAMSS blocks, the SMMU, the frame buffer, the frames.
  bool pll0NoLock = false, csidResetStuck = false, sensorMute = false, framePadded = false, dmaFail = false;
  unsigned extraLines = 0;   // a frame longer than the mode by this many lines (an overflow)
  bool mm1On = false;
  // The tablet's rules (tabletC1/tabletC3): an MCLK RCG on the XO has a root and its branch runs only while GCC's camera XO branch
  // is enabled (inferred from c2r/c2f, 2026-11-12); a BUF_DONE set at once at stream-on, before any frame (a stale bit).
  bool mclkNeedsCameraXo = false, mclkNeedsTitan = false, staleBufDoneAtStart = false, staleSeen = false, bufDoneAtRdiStart = false;
  unsigned smmuStores = 0;
  // The order of the power-down's events (the teardown check): "wm off", "rdi stop", "smmu sync", "camnoc off", "hf axi off",
  // "mm1 off", "titan off", "mxc off".
  std::vector<std::string> events;
  long lastEvent(const char *what) const
  {
    for (size_t i = events.size(); i-- > 0;)
      if (events[i] == what) return long(i);
    return -1;
  }
  uint64_t memPa = 0x880000000ull, tablePa = 0x8f0000000ull;
  std::vector<uint8_t> mem;
  std::vector<uint64_t> tableMem;
  std::vector<uint32_t> aupFifo;   // the write master's latched addresses (address >> 8)
  bool rupPending = false;
  uint64_t nextFrameAt = 0;
  unsigned framesWritten = 0, framesDropped = 0, smmuFaults = 0, allocs = 0, frees = 0, cleans = 0, invalidates = 0;
  unsigned rgb[8][4];              // the colour bars: each band's value per Bayer position (10 bits)
  // The scene: 0 the flat grey 300 + position (exposure and gain ignored); else a lit scene whose value above the black level (64)
  // is scene * exposure * gain / 1x / 1000 (at most 1023), with the controls a frame late (they apply at the next frame).
  uint64_t scene[2] = {0, 0};      // rear, front
  uint64_t sceneEg[2] = {0, 0};    // exposure x gain / 1x as latched for the frame being written
  unsigned sceneEgSet[2] = {0, 0};
  bool sceneBlack = false;         // no light at all: the black level whatever the controls
  uint64_t controlsEg(bool rear) const
  {
    const Slave &s = rear ? rearSlave : frontSlave;
    auto at = [&](uint16_t a) -> uint64_t { return s.reg.count(a) ? s.reg.at(a) : 0; };
    if (rear) return (at(0x0202) << 8 | at(0x0203)) * (at(0x0204) << 8 | at(0x0205)) / 32;
    return (at(0x3500) << 16 | at(0x3501) << 8 | at(0x3502)) * (at(0x3508) << 8 | at(0x3509)) / 256;
  }

  Board()
  {
    for (unsigned b = 0; b < 8; ++b)
      for (unsigned p = 0; p < 4; ++p) rgb[b][p] = 64 + 100 * b + 20 * p;
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
    // SMMU: 64 groups (stage 1 supported), 83 banks, NUMPAGENDXB 6 (128 global pages, then the banks); 40-bit output, 4 KiB tables
    r[kSmmu + 0x20] = 64 | 1u << 30;
    r[kSmmu + 0x24] = 6u << 28 | 83;   // the tablet's (PianoVideo's survey: 128 pages, 83 banks)
    r[kSmmu + 0x28] = 2u << 4 | 1u << 12;
    r[kSmmu] = 0x002d0406;             // c1's SCR0 (USFCFG set: an unmatched stream faults)
    // Level 3's other CAMCC branches, PLL0 off
    for (uint32_t o : {kCcCamnocNrtAxi, kCcCamnocRtIfeLite, kCcQdssDebugXo}) r[kCamCc + o] = 0x80000000u;
    for (uint32_t o = 0; o <= 0x30; o += 4) r[kCamCc + kCcPll0 + o] = 0;
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
  bool camssRange(uint64_t pa) const
  {
    for (const Block &b : kBlocks)
      if (inRange(pa, b.pa, 0x1000)) return true;
    return inRange(pa, kCci0, kCci0Bytes) || inRange(pa, kTfeLite0, kTfeLiteBytes) || inRange(pa, csiphyBase(1), kCsiphyBytes) ||
           inRange(pa, csiphyBase(4), kCsiphyBytes);
  }
  // Whether an access to pa would complete on the real bus: every block's register access with its block's clocks, its power
  // domain and the votes under them held.
  const char *gate(uint64_t pa) const
  {
    if (inRange(pa, kCamCc, kCamCcBytes) && !gccClockOn(v(kGcc + kGccCameraAhb))) return "CAMCC without GCC's camera AHB";
    if (inRange(pa, kCamCc, kCamCcBytes) && requireMxc && !mxcVoted) return "CAMCC without MXC";
    if (camssRange(pa) && requireMxc && !mxcVoted) return "a CAMSS block without CAMCC's domains (MXC)";
    if (inRange(pa, kMclkCc, kMclkCcBytes) && !gccClockOn(v(kGcc + kGccMclkAhb))) return "the MCLK controller without its AHB";
    const bool camAhb = gccClockOn(v(kGcc + kGccCameraAhb)) && titan() && running(kCamCc + kCcCamTopAhb);
    if (inRange(pa, kCci0, kCci0Bytes) && !(camAhb && running(kCamCc + kCcCci0))) return "CCI0 unpowered or unclocked";
    for (const Block &b : kBlocks)
      if (inRange(pa, b.pa, 0x1000) && !(camAhb && running(kCamCc + b.branch))) return "a CAMSS block unpowered or unclocked";
    if (inRange(pa, kTfeLite0, kTfeLiteBytes) && !(camAhb && running(kCamCc + kCcIfeLite) && running(kCamCc + kCcIfeLiteAhb)))
      return "TFE-lite0 unpowered or unclocked";
    if (inRange(pa, kCsidLite0, 0x1000) && !running(kCamCc + kCcIfeLiteCphyRx)) return "CSID-lite0 without its PHY RX clock";
    for (unsigned n : {1u, 4u})
      if (inRange(pa, csiphyBase(n), kCsiphyBytes) && !(camAhb && running(kCamCc + csiphyBranch(n)) && running(kCamCc + csiTimerBranch(n))))
        return "a CSIPHY unpowered or unclocked";
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
  // Our window is live: the camera stream's group valid and translating through a bank with translation on and our table.
  bool windowLive() const
  {
    const unsigned groups = v(kSmmu + 0x20) & 0xffu, pages = 1u << (((v(kSmmu + 0x24) >> 28) & 7u) + 1);
    for (unsigned i = 0; i < groups; ++i) {
      const uint32_t smr = v(kSmmu + 0x800 + 4 * i);
      if (!(smr & 0x80000000u) || (smr & 0x7fffu) != kCamStream) continue;
      const uint32_t s2cr = v(kSmmu + 0xc00 + 4 * i);
      const uint64_t cb = kSmmu + uint64_t(pages + (s2cr & 0xffu)) * 4096;
      const uint64_t ttbr = uint64_t(v(cb + 0x20)) | uint64_t(v(cb + 0x24) & 0xffffu) << 32;
      return ((s2cr >> 16) & 3u) == 0 && (v(cb) & 1u) && ttbr == tablePa;
    }
    return false;
  }
  bool wmEnabled() const { return v(kTfeLite0 + kWm0 + kWmCfg) & kWmEnable; }
  bool cameraNocUp() const
  {
    return mm1On && (v(kGcc + kGccCameraHfAxi) & kCbcrEnable) && running(kCamCc + kCcCamnocRtAxi);
  }
  // The apps SMMU as the hypervisor presents it (c1: IDR0 0x4c017e7f, stage 1 only): a store of a configuration it does not have
  // aborts (c3rp/c3r: the free group's S2CR 0x0300006e written back, "Unhandled implementation specific error"); the TLB
  // maintenance of the camera's stream needs the camera NoC (the camera TBU: inferred, the conservative rule).
  void smmuStore(uint32_t off, uint32_t value)
  {
    ++smmuStores;
    const uint32_t idr0 = v(kSmmu + 0x20), idr1 = v(kSmmu + 0x24);
    const unsigned banks = idr1 & 0xffu, pages = 1u << (((idr1 >> 28) & 7u) + 1);
    char why[160] = {};
    if (off >= 0xc00 && off < 0xc00 + 4 * 128) {
      const uint32_t type = (value >> 16) & 3u;
      if (type == 3 || (type == 0 && (value & 0xffu) >= banks))
        std::snprintf(why, sizeof why, "SMMU abort: S2CR %u := %08x (bank %u of %u)", (off - 0xc00) / 4, value, value & 0xffu, banks);
    } else if (off >= 0x1000 && off < 0x1000 + 4 * 128) {
      const uint32_t type = (value >> 16) & 3u;
      if (type == 1 ? !(idr0 & (1u << 30)) : !(idr0 & (1u << 29)))
        std::snprintf(why, sizeof why, "SMMU abort: CBAR %u := %08x (a context type this SMMU lacks)", (off - 0x1000) / 4, value);
    } else if (off == 0x68 || off == 0x70 || (off >= pages * 4096 && ((off & 0xfff) == 0x618 || (off & 0xfff) == 0x7f0))) {
      if (!cameraNocUp()) std::snprintf(why, sizeof why, "SMMU TLB maintenance (%#x) without the camera NoC voted and clocked", off);
      if (off == 0x70) events.push_back("smmu sync");
    }
    if (why[0]) {
      violations.push_back(why);
      return;
    }
    r[kSmmu + off] = value;
  }
  void wr(uint64_t pa, uint32_t value)
  {
    ++stores;
    if (const char *g = gate(pa)) {
      violations.push_back(std::string("store stalled: ") + g);
      return;
    }
    if (inRange(pa, kSmmu, kSmmuBytes)) return smmuStore(uint32_t(pa - kSmmu), value);
    if (pa == kTfeLite0 + kWm0 + kWmCfg && !(value & kWmEnable) && wmEnabled()) events.push_back("wm off");
    if (pa == kCsidLite0 + kCsidRdiCtrl && !(value & 1u) && (v(pa) & 1u)) events.push_back("rdi stop");
    if ((pa == kGcc + kGccCameraHfAxi || pa == kCamCc + kCcCamnocRtAxi) && !(value & kCbcrEnable) && (v(pa) & kCbcrEnable)) {
      events.push_back(pa == kGcc + kGccCameraHfAxi ? "hf axi off" : "camnoc off");
      if (wmEnabled()) violations.push_back("the camera NoC's clock off while the write master is enabled");
      if (windowLive()) violations.push_back("the camera NoC's clock off while the SMMU window is live");
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
    if (inRange(pa, kCsidLite0, kCsidLiteBytes)) return csidWrite(uint32_t(pa - kCsidLite0), value);
    if (pa == kCamCc + kCcPll0 || pa == kCamCc + kCcPll0 + kPllOpmode) {
      r[pa] = value;
      const uint64_t p = kCamCc + kCcPll0;
      const uint32_t mode = v(p), op = v(p + kPllOpmode);
      const bool lock = (mode & kPllResetN) && (op & 1u) && v(p + kTaycanL) == kPll0LVal && v(p + kTaycanConfigCtl) == kPll0ConfigCtl &&
                        mxcVoted && !pll0NoLock;
      r[p] = (mode & ~kPllLockDet) | (lock ? kPllLockDet : 0);
      return;
    }
    // CBCRs, GDSCs, RCGs, the PLL: by address.
    const uint64_t g = kCamCc + kGdscTitanTop;
    if (pa == g) {
      const bool wasOn = titan();
      r[pa] = value;
      const bool on = !(value & kGdscCollapse) && running(kCamCc + kCcGdscClk) && !titanStuck;
      if (on) {
        r[pa] |= kGdscPwrOn;
        r[pa + 4] = kGdscCfgUpDone;
      } else {
        if (wasOn) {
          events.push_back("titan off");
          if (wmEnabled()) violations.push_back("TITAN_TOP collapsed with the write master enabled");
          if (windowLive()) violations.push_back("TITAN_TOP collapsed while the SMMU window is live");
          resetCamss();   // collapsed: CSID, TFE and CSIPHY registers lose their state
        }
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
      // A hardware-gated branch (HW_CTL) reads CLK_OFF set while idle, enabled or not (the tablet's GCC AHB branches).
      r[pa] = (value & ~kCbcrClkOff) | (branchCanRun(pa, value) && !(value & kCbcrHwCtl) ? 0 : kCbcrClkOff);
      if (mclk1 && running(pa) && !was) at[tRearMclk] = int64_t(now);
      if (mclk1 && !running(pa) && was) at[tRearMclkOff] = int64_t(now);
      if (mclk4 && running(pa) && !was) at[tFrontMclk] = int64_t(now);
      if (mclk4 && !running(pa) && was) at[tFrontMclkOff] = int64_t(now);
      return;
    }
    if (isRcgCmd(pa)) {
      // The update completes at once, if the configured source runs (a CAMCC PLL0 output: only with PLL0 locked, outputs on).
      const unsigned src = rcgSource(v(pa + kRcgCfg));
      bool camcc = false;   // level 3's sources only (level 2's found-on-a-PLL slow AHB keeps its old model)
      for (const L3Rcg &l : kL3Rcgs) camcc = camcc || pa == kCamCc + (l.off ? l.off : kCcCsi1TimerRcg) || pa == kCamCc + kCcCsi4TimerRcg;
      uint32_t x = (value & kRcgUpdate) && camcc && src >= 1 && src <= 3 && !pll0Running() ? value : value & ~kRcgUpdate;
      if ((value & kRcgRootEn) && !(inRange(pa, kMclkCc, kMclkCcBytes) && src == 0 && !mclkXo())) x &= ~kRcgRootOff;
      else x |= kRcgRootOff;
      r[pa] = x;
      return;
    }
    if (pa == kMclkCc + kMclkPll || pa == kMclkCc + kMclkPll + 4) {
      r[pa] = value;
      const uint32_t mode = v(kMclkCc + kMclkPll), op = v(kMclkCc + kMclkPll + 4);
      const bool lock = (mode & 4u) && (op & 1u) && v(kMclkCc + kMclkPll + kPllRivianL) == 0x32 &&
                        v(kMclkCc + kMclkPll + 0x1c) == 0x12000000 && !pllNoLock && mclkXo();
      r[kMclkCc + kMclkPll] = (mode & ~kPllLockDet) | (lock ? kPllLockDet : 0);
      return;
    }
    r[pa] = value;
  }
  bool isCbcr(uint64_t pa) const
  {
    for (uint32_t o : {kGccCameraAhb, kGccCameraXo, kGccMclkAhb, kGccCameraHfAxi})
      if (pa == kGcc + o) return true;
    for (const Reg &b : kCcBranches)
      if (pa == kCamCc + b.off) return true;
    for (uint32_t o : {kCcCamnocNrtAxi, kCcCamnocRtIfeLite, kCcQdssDebugXo})
      if (pa == kCamCc + o) return true;
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
    for (unsigned n = 0; n < kMclkCount; ++n)   // an MCLK from the XO (and the MCLK PLL's reference): GCC's camera XO (the tablet's)
      if (pa == kMclkCc + mclkBranch(n) && !mclkXo()) return false;
    // Level 3's branches: under TITAN_TOP, and only with their source running (a PLL0 output needs PLL0).
    if (inRange(pa, kCamCc, kCamCcBytes))
      if (const uint32_t rcg = rcgOfBranch(uint32_t(pa - kCamCc))) {
        if (!titan()) return false;
        const unsigned src = rcgSource(v(kCamCc + rcg + kRcgCfg));
        return src == 0 || (src <= 3 && pll0Running());
      }
    return true;
  }
  static uint32_t rcgOfBranch(uint32_t off)
  {
    switch (off) {
    case kCcCsiphy1: case kCcCsiphy4: case kCcIfeLiteCphyRx: case kCcCsidCsiphyRx: return kCcCphyRxRcg;
    case kCcCsi1Timer: return kCcCsi1TimerRcg;
    case kCcCsi4Timer: return kCcCsi4TimerRcg;
    case kCcCamnocRtAxi: case kCcCamnocNrtAxi: return kCcCamnocRtAxiRcg;
    case kCcIfeLite: case kCcCamnocRtIfeLite: return kCcIfeLiteRcg;
    case kCcIfeLiteCsid: return kCcIfeLiteCsidRcg;
    case kCcIfeLiteAhb: return kCcSlowAhbRcg;
    case kCcCamTopFastAhb: return kCcFastAhbRcg;
    case kCcQdssDebugXo: return kCcXoRcg;
    default: return 0;
    }
  }
  // An MCLK runs from the XO only with GCC's camera XO (mclkNeedsCameraXo: c2f) and TITAN_TOP up (mclkNeedsTitan: c2fx of
  // 2026-11-13, the minimal path with the camera XO on, did not run MCLK4; c3f's path with TITAN_TOP and CAMCC's domains did).
  bool mclkXo() const
  {
    return (!mclkNeedsCameraXo || (v(kGcc + kGccCameraXo) & kCbcrEnable)) && (!mclkNeedsTitan || titan());
  }
  bool pll0Running() const
  {
    const uint64_t p = kCamCc + kCcPll0;
    return (v(p) & kPllLockDet) && (v(p) & kPllOutctrl) && (v(p + kPllOpmode) & 1u) && (v(p + kTaycanUserCtl) & kPllOutputs) == kPllOutputs;
  }

  // ------------------------------------------------------------------------------------------------- level 3's CAMSS
  void resetCamss()
  {
    for (auto it = r.begin(); it != r.end();) {
      const uint64_t pa = it->first;
      const bool camss = inRange(pa, kCsidLite0, kCsidLiteBytes) || inRange(pa, kTfeLite0, kTfeLiteBytes) ||
                         inRange(pa, csiphyBase(1), kCsiphyBytes) || inRange(pa, csiphyBase(4), kCsiphyBytes);
      it = camss ? r.erase(it) : std::next(it);
    }
    aupFifo.clear();
    rupPending = false;
    nextFrameAt = 0;
  }
  void csidWrite(uint32_t off, uint32_t value)
  {
    const uint64_t c = kCsidLite0;
    switch (off) {
    case kCsidRstCmd:
      if (!csidResetStuck) r[c + kCsidTopStatus] |= kCsidRstDone;
      aupFifo.clear();
      return;
    case kCsidTopClear: r[c + kCsidTopStatus] &= ~value; return;
    case kCsidBufClear: r[c + kCsidBufStatus] &= ~value; return;
    case kCsidRxClear: r[c + kCsidRxStatus] &= ~value; return;
    case kCsidRdiClear: r[c + kCsidRdiStatus] &= ~value; return;
    case kCsidIrqCmd: return;
    case kCsidRdiCtrl:   // (an injection: a BUF_DONE left set when RDI0 starts, before any frame)
      if ((value & 1u) && bufDoneAtRdiStart) r[c + kCsidBufStatus] |= kCsidBufRdi0;
      r[c + off] = value;
      return;
    case kCsidRupAup:
      // Each write with the AUP bit latches the write master's address; the RUP bit takes effect at the next frame's start.
      if (value & kCsidAupRdi0) aupFifo.push_back(v(kTfeLite0 + kWm0 + kWmImageAddr));
      if (aupFifo.size() > 4) aupFifo.erase(aupFifo.begin());
      if (value & kCsidRupRdi0) rupPending = true;
      r[c + off] = value;
      return;
    default: r[c + off] = value; return;
    }
  }
  unsigned phyOf() const
  {
    const uint32_t cfg = v(kCsidLite0 + kCsidRxCfg0);
    return (cfg >> 20) ? (cfg >> 20) - 1 : 99;
  }
  bool phyReady(unsigned n) const
  {
    const uint64_t b = csiphyBase(n), cm = b + kCsiphyTableCommon;
    if (v(cm + 4 * 5) != 0xd5 || v(cm + 4 * 6) != 1 || v(cm + 4 * 7) != 2) return false;
    const uint8_t settle = csiphySettle(kSensors[n == 1 ? 0 : 1].linkHz, 400000000);
    for (const CsiphyLaneReg &e : kCsiphyLaneRegs) {
      if (e.kind == 7) continue;
      if (v(b + e.offset) != (e.kind == 2 ? settle : e.value)) {
        // a register written twice (0x094: 0x00, then 0x07) holds its last value: only the last entry per offset counts
        bool later = false;
        for (const CsiphyLaneReg *q = &e + 1; q < kCsiphyLaneRegs + sizeof kCsiphyLaneRegs / sizeof kCsiphyLaneRegs[0]; ++q)
          later = later || (q->offset == e.offset && q->kind != 7);
        if (!later) return false;
      }
    }
    return true;
  }
  bool streamReady() const
  {
    const unsigned n = phyOf();
    if (n != 1 && n != 4) return false;
    const bool rear = n == 1;
    const Slave &s = rear ? rearSlave : frontSlave;
    auto it = s.reg.find(0x0100);
    const bool sensorOn = it != s.reg.end() && (it->second & 1) && !sensorMute && sensorReady(rear) &&
                          (!rear || rcgSource(v(kMclkCc + mclkRcg(1) + kRcgCfg)) == 3);   // the rear streams only at 24 MHz
    const uint64_t c = kCsidLite0;
    const bool csid = v(c + kCsidRxCfg0) == csidRxCfg0(4, n) && (v(c + kCsidRdiCfg0) & kRdiCfg0Enable) && v(c + kCsidRdiCtrl) == 1 &&
                      running(kCamCc + kCcIfeLiteCsid) && running(kCamCc + kCcIfeLiteCphyRx);
    const bool wm = v(kTfeLite0 + kWm0 + kWmCfg) == (kWmEnable | kWmFrameMode) && running(kCamCc + kCcIfeLite) &&
                    running(kCamCc + kCcCamnocRtAxi) && (v(kGcc + kGccCameraHfAxi) & kCbcrEnable) && mm1On;
    return titan() && sensorOn && phyReady(n) && csid && wm;
  }
  // The SMMU for stream 0x1c00: 0 when the write faults.
  uint64_t smmuTranslate(uint64_t bus) const
  {
    const unsigned groups = v(kSmmu + 0x20) & 0xffu, pages = 1u << (((v(kSmmu + 0x24) >> 28) & 7u) + 1);
    for (unsigned i = 0; i < groups; ++i) {
      const uint32_t smr = v(kSmmu + 0x800 + 4 * i);
      if (!(smr & 0x80000000u) || ((smr ^ kCamStream) & 0x7fffu & ~((smr >> 16) & 0x7fffu))) continue;
      const uint32_t s2cr = v(kSmmu + 0xc00 + 4 * i), type = (s2cr >> 16) & 3u;
      if (type == 1) return bus;
      if (type != 0) return 0;
      const unsigned b = s2cr & 0xffu;
      const uint64_t cb = kSmmu + uint64_t(pages + b) * 4096;
      if (!(v(cb) & 1u)) return (v(kSmmu + 0x1800 + 4 * b) & 1u) ? bus : bus & 0xffffffffull;
      if (bus >> 32) return 0;
      const uint64_t ttbr = uint64_t(v(cb + 0x20)) | uint64_t(v(cb + 0x24) & 0xffffu) << 32;
      if (ttbr < tablePa || ttbr + 8192 > tablePa + tableMem.size() * 8) return 0;
      const uint64_t l1 = tableMem[(ttbr - tablePa) / 8 + ((bus >> 30) & 3)];
      if ((l1 & 3) != 3) return 0;
      const uint64_t l2pa = l1 & 0x0000fffffffff000ull;
      if (l2pa < tablePa || l2pa + 4096 > tablePa + tableMem.size() * 8) return 0;
      const uint64_t l2 = tableMem[(l2pa - tablePa) / 8 + ((bus >> 21) & 511)];
      if ((l2 & 3) != 1) return 0;
      return (l2 & 0x0000fffffffff000ull & ~0x1fffffull) | (bus & 0x1fffffull);
    }
    return 0;   // unmatched: USFCFG faults it
  }
  uint64_t cachedPage = ~0ull, cachedPa = 0;   // one 4 KiB page's translation (a frame's writes walk the pages in order)
  void dmaWrite(uint64_t bus, uint8_t byte)
  {
    if ((bus >> 12) != cachedPage) {
      cachedPage = bus >> 12;
      cachedPa = smmuTranslate(cachedPage << 12);
    }
    const uint64_t pa = cachedPa ? cachedPa + (bus & 0xfff) : 0;
    if (!pa || pa < memPa || pa >= memPa + mem.size()) {
      ++smmuFaults;
      if (pa && (pa < memPa || pa >= memPa + mem.size())) violations.push_back("a write master store outside the frame buffer");
      return;
    }
    mem[pa - memPa] = byte;
  }
  // One frame: the colour bars (the rear's test pattern) or a flat grey scene, RAW10 MIPI-packed, the lines unpadded (or padded to
  // 16 bytes with framePadded), written through the SMMU at the latched address; SOF/EOF, BUF_DONE, RUP_DONE.
  void produceFrame()
  {
    const uint64_t c = kCsidLite0;
    const bool rear = phyOf() == 1;
    const unsigned width = rear ? kRearL3Width : kFrontL3Width, height = (rear ? kRearL3Height : kFrontL3Height) + extraLines;
    r[c + kCsidRdiStatus] |= kRdiInputSof | kRdiCamifSof;
    if (rupPending) {
      r[c + kCsidRdiStatus] |= kRdiRupDone;
      rupPending = false;
    }
    cachedPage = ~0ull;   // the SMMU may have changed since the last frame
    if (aupFifo.empty()) {
      ++framesDropped;
      r[c + kCsidRdiStatus] |= 1u << 18;
      return;
    }
    const uint64_t base = uint64_t(aupFifo.front()) << 8;
    aupFifo.erase(aupFifo.begin());
    const uint32_t bpl = raw10Bytes(width), stride = framePadded ? (bpl + 15u) & ~15u : bpl;
    const bool bars = rear && rearSlave.reg.count(0x0601) && rearSlave.reg.at(0x0601) == 2;
    const unsigned si = rear ? 0 : 1;
    const uint64_t eg = sceneEgSet[si] ? sceneEg[si] : controlsEg(rear);   // the controls of the frame before
    sceneEg[si] = controlsEg(rear);
    sceneEgSet[si] = 1;
    const uint64_t lit = scene[si] * eg / 1000;
    const unsigned sceneValue = sceneBlack ? 64 : scene[si] ? unsigned(64 + (lit > 959 ? 959 : lit)) : 300;
    for (unsigned y = 0; y < height; ++y)
      for (unsigned x = 0; x < width; x += 4) {
        uint8_t g[5] = {};
        for (unsigned k = 0; k < 4; ++k) {
          const unsigned px = x + k, pos = (y & 1u) * 2 + (px & 1u);
          const unsigned val = bars ? rgb[px * 8 / width][pos] : (sceneValue + pos > 1023 ? 1023 : sceneValue + pos);
          g[k] = uint8_t(val >> 2);
          g[4] |= uint8_t((val & 3u) << (2 * k));
        }
        for (unsigned k = 0; k < 5; ++k) dmaWrite(base + uint64_t(y) * stride + x / 4 * 5 + k, g[k]);
      }
    r[c + kCsidRdiStatus] |= kRdiInputEof | kRdiCamifEof;
    r[c + kCsidBufStatus] |= kCsidBufRdi0;
    r[c + kCsidTopStatus] |= kCsidTopBufDone;
    ++framesWritten;
  }
  void tick()
  {
    if (!streamReady()) {
      nextFrameAt = 0;
      return;
    }
    if (staleBufDoneAtStart && !staleSeen) {   // a BUF_DONE at once, no frame behind it
      staleSeen = true;
      r[kCsidLite0 + kCsidBufStatus] |= kCsidBufRdi0;
    }
    if (!nextFrameAt) nextFrameAt = now + 33333;
    while (now >= nextFrameAt) {
      produceFrame();
      nextFrameAt += 33333;
    }
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
    const uint32_t was = it == a.end() ? 0u : it->second;   // a register never written reads 0 in the model
    if (was != v) {
      std::printf("  changed: %#llx: %08x -> %08x\n", (unsigned long long)pa, was, v);
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
    b.tick();
  }
  // Level 3's additions.
  std::vector<bool> mmBusCalls;
  bool mmBusFail = false;
  int mmBus(bool on)
  {
    mmBusCalls.push_back(on);
    if (mmBusFail) return -5;
    if (!on && b.mm1On) {   // the camera NoC's vote withdrawn: nothing may still use it
      b.events.push_back("mm1 off");
      if (b.wmEnabled()) b.violations.push_back("MM1 withdrawn with the write master enabled");
      if (b.windowLive()) b.violations.push_back("MM1 withdrawn while the SMMU window is live");
    }
    b.mm1On = on;
    return 0;
  }
  bool dmaAlloc(uint64_t bytes, bool tables, Dma &d)
  {
    if (b.dmaFail) return false;
    b.mem.assign(bytes, 0x5a);
    d.pa = b.memPa;
    d.bytes = bytes;
    d.cpu = b.mem.data();
    if (tables) {
      b.tableMem.assign(1024, 0x1234567812345678ull);   // two pages, not zeroed: the window builder must clear them
      d.tablePa = b.tablePa;
      d.tables = b.tableMem.data();
    }
    ++b.allocs;
    return true;
  }
  void dmaFree(Dma &d)
  {
    ++b.frees;
    d = Dma();
  }
  void cacheClean(const void *, uint64_t) { ++b.cleans; }
  void cacheInvalidate(const void *, uint64_t) { ++b.invalidates; }
  uint64_t nowUs() { return b.now; }
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
  // Step 202's survey (the kernel logs its controller lines).
  std::vector<Survey> surveys;
  void surveyed(const Survey &s) { surveys.push_back(s); }
  // The shared votes as the board sees them: CAMCC (and so every CAMSS block) is readable only with MXC voted (Board::mxc).
  std::vector<bool> mmCalls;
  bool mmFail = false;
  int mmRails(bool on)
  {
    mmCalls.push_back(on);
    if (mmFail) return -5;
    if (!on && b.mxcVoted) {   // CAMCC's domains withdrawn: TITAN_TOP and every CAMCC branch must be off by now
      b.events.push_back("mxc off");
      if (b.titan()) b.violations.push_back("CAMCC's domains withdrawn with TITAN_TOP on");
    }
    b.mxcVoted = on;
    return 0;
  }
  std::vector<std::string> log;   // the steps and notes in their order
  void step(int n, const char *)
  {
    steps.push_back(n);
    log.push_back("step " + std::to_string(n));
  }
  void note(const char *fmt, ...)
  {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    notes.push_back(line);
    log.push_back(line);
    // The console cuts a line at 255 characters with its "PianoCamera: " prefix (c3r): every line must fit.
    if (std::strlen(line) + 13 > 254) b.violations.push_back(std::string("a log line too long for the console: ") + line);
  }
  long logIndex(const char *prefix) const
  {
    for (size_t i = 0; i < log.size(); ++i)
      if (!log[i].compare(0, std::strlen(prefix), prefix)) return long(i);
    return -1;
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
  po.camss = false;   // the minimal path (piano-camera-camss=0)
  const SensorResult res = r.run(Sensor::rear, po);
  CHECK(res.idOk && res.id == 0x38e1);
  CHECK(res.cleanOff && r.b.violations.empty());
  CHECK(same(before, r.b.snapshot()));
  CHECK(r.b.quietStores > 50);
  CHECK(r.b.cciStarts == 0);
  checkRearTiming(r.b);
  // No CAMCC store at all on this path.
  CHECK(r.b.v(kCamCc + kGdscTitanTop) == 0x00222001);
  // The default (camss): TITAN_TOP up for the sensor as well (its DT power domain), all as found after.
  Rig q;
  const auto beforeQ = q.b.snapshot();
  PowerOptions pq;
  pq.rearCci = false;
  const SensorResult rq = q.run(Sensor::rear, pq);
  CHECK(rq.idOk && rq.cleanOff && q.b.violations.empty() && same(beforeQ, q.b.snapshot()) && q.b.cciStarts == 0);
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
  po.camss = false;   // the minimal path (piano-camera-camss=0); the default is testFrontCamss's
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
  // The minimal front path never touches CAMCC or CCI0.
  CHECK(r.b.v(kCamCc + kGdscTitanTop) == 0x00222001 && r.b.cciStarts == 0);
  // gpio7's egpio bit was not ours: set while in use (as Linux claims the pin), back as found after.
  CHECK(!(r.b.v(tlmmCfg(7)) & kTlmmEgpioEnable));
}

static void tabletC1(Board &b);
// c2f (2026-11-12): the front's MCLK4 never ran ("a clock branch did not run", its CBCR 0x88000000) with GCC's camera XO off and
// TITAN_TOP down; the rear's MCLK1 did (c2r) with both on. Against the tablet's model (an MCLK from the XO only with GCC's camera
// XO enabled): the minimal path now turns the camera XO on (and off again), the default path powers TITAN_TOP too (the sensors' DT
// power domain), both find 0x5632 and leave everything as found; the MCLK RCG gets Linux's shared-RCG enable even at the XO.
static void testFrontCamss()
{
  // The model's rule itself: with the camera XO off, MCLK4's RCG root and branch stay off.
  {
    Board b;
    tabletC1(b);
    b.r[kGcc + kGccMclkAhb] = 0x88000003u;
    b.wr(kMclkCc + mclkRcg(4), kRcgRootEn);
    b.wr(kMclkCc + mclkBranch(4), kCbcrEnable);
    CHECK((b.v(kMclkCc + mclkRcg(4)) & kRcgRootOff) && (b.v(kMclkCc + mclkBranch(4)) & kCbcrClkOff));
    b.r[kGcc + kGccCameraXo] = 0x08000001u;
    b.wr(kMclkCc + mclkRcg(4), kRcgRootEn);
    b.wr(kMclkCc + mclkBranch(4), kCbcrEnable);
    CHECK(!(b.v(kMclkCc + mclkRcg(4)) & kRcgRootOff) && !(b.v(kMclkCc + mclkBranch(4)) & kCbcrClkOff));
  }
  for (bool camss : {false, true}) {
    Rig r;
    tabletC1(r.b);
    const auto before = r.b.snapshot();
    PowerOptions po;
    po.camss = camss;
    const SensorResult res = r.run(Sensor::front, po);
    for (auto &v : r.b.violations) std::printf("  violation: %s\n", v.c_str());
    CHECK(res.idOk && res.id == 0x5632 && res.cleanOff && r.b.violations.empty());
    CHECK(same(before, r.b.snapshot()));
    checkFrontTiming(r.b);
    CHECK(res.voted == camss && r.ops.mmCalls.size() == (camss ? 2u : 0u) && !r.b.mxcVoted);
    CHECK(r.b.cciStarts == 0);
  }
  // The tablet (c2fx, 2026-11-13): the minimal path's MCLK4 does not run (step 214, everything as found); level 3's path runs it.
  for (bool camss : {false, true}) {
    Rig r;
    tabletC1(r.b);
    r.b.mclkNeedsTitan = true;
    const auto before = r.b.snapshot();
    PowerOptions po;
    po.camss = camss;
    const SensorResult res = r.run(Sensor::front, po);
    CHECK(res.cleanOff && r.b.violations.empty() && same(before, r.b.snapshot()));
    CHECK(camss ? res.idOk : !res.idOk && (res.failure == Failure::branch || res.failure == Failure::rcg) && res.failedStep == -kStepMclk);
  }
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

// The tablet as the c1 survey found it (2026-11-12): GCC's camera AHB 0x88000002, camera XO 0x88000000 and MCLK-controller AHB
// 0x88000002 (all off; both AHBs hardware-gated), HF AXI 0x8a200000; CAMCC readable only with MXC voted (the strict model). Level 1
// reads neither controller; level 2 votes CAMCC's domains, enables the three branches (the hardware-gated ones counting on without
// CLK_OFF clearing, Linux's rule), finds both IDs, and leaves everything as found (votes withdrawn, the branches' enables cleared).
// With what the c2/c3 runs (2026-11-12) taught: an MCLK from the XO runs only with GCC's camera XO enabled (c2f's MCLK4 did not run
// with it off, c2r's MCLK1 did with it on: inferred), and the apps SMMU as the hypervisor presents it: IDR0 0x4c017e7f (127 groups,
// stage 1 only), IDR1 83 banks, five groups in use (c1: 122 of 127 free; their streams and banks stand in, NativeUfs's bank 82
// among them), every free group's S2CR reading 0x0300006e (c3r's, written back by the old release: the abort).
static void tabletC1(Board &b)
{
  b.r[kGcc + kGccCameraAhb] = 0x88000002u;
  b.r[kGcc + kGccCameraXo] = 0x88000000u;
  b.r[kGcc + kGccMclkAhb] = 0x88000002u;
  b.r[kGcc + kGccCameraHfAxi] = b.r[kGcc + kGccCameraSfAxi] = 0x8a200000u;
  b.requireMxc = true;
  b.mclkNeedsCameraXo = true;
  b.r[kSmmu + 0x20] = 0x4c017e7fu;
  const struct {
    uint16_t stream;
    unsigned bank;
    uint32_t sctlr;
  } used[5] = {{0x0400, 0, 1}, {0x0060, 82, 0}, {0x0800, 1, 0x27}, {0x0040, 3, 0xe0}, {0x0c00, 2, 0x27}};
  for (unsigned i = 0; i < 127; ++i) {
    b.r[kSmmu + 0x800 + 4 * i] = i < 5 ? 0x80000000u | used[i].stream : 0;
    b.r[kSmmu + 0xc00 + 4 * i] = i < 5 ? used[i].bank : 0x0300006eu;
  }
  for (const auto &u : used) b.r[kSmmu + (128 + u.bank) * 4096] = u.sctlr;
}
static void testTabletC1()
{
  Rig r;
  tabletC1(r.b);
  const Survey s = r.survey();
  CHECK(r.b.stores == 0 && r.b.violations.empty());
  CHECK(!s.cc.read && !s.mclk.read && !s.cci.read);
  CHECK(!gccClockOn(s.gcc.cameraAhb) && !gccClockOn(s.gcc.mclkAhb));
  BusChoice bus;
  const char *why = sensorRefusal(s, Sensor::rear, bus);
  CHECK(why && std::strstr(why, "GCC AHB is off"));   // the level 1 verdict; level 2's step 201 turns the AHB on first
  for (Sensor w : {Sensor::rear, Sensor::front}) {
    Rig q;
    tabletC1(q.b);
    const auto before = q.b.snapshot();
    const SensorResult res = q.run(w, PowerOptions());
    CHECK(res.idOk && res.cleanOff && q.b.violations.empty());
    for (auto &v : q.b.violations) std::printf("  violation: %s\n", v.c_str());
    CHECK(same(before, q.b.snapshot()));
    // Both sensors (the rear through CCI0, the front for TITAN_TOP, its power domain) vote CAMCC's domains first, withdraw them last.
    CHECK(q.ops.mmCalls.size() == 2 && q.ops.mmCalls[0] && !q.ops.mmCalls[1] && res.voted && !q.b.mxcVoted);
  }
  // c1b: level 2 stopped after step 202 (the rear's path): GCC's branches enabled and CAMCC's domains voted, both controllers read
  // (the survey handed to the kernel's log), then everything as found: no rail, pin or CAMCC store.
  {
    Rig q;
    tabletC1(q.b);
    const auto before = q.b.snapshot();
    PowerOptions po;
    po.stopAt = kStepResurvey;
    const SensorResult res = q.run(Sensor::rear, po);
    CHECK(res.failure == Failure::stopped && res.cleanOff && q.b.violations.empty() && same(before, q.b.snapshot()));
    CHECK(q.ops.surveys.size() == 1 && q.ops.surveys[0].cc.read && q.ops.surveys[0].mclk.read && q.b.railVotes == 0);
    CHECK(q.ops.mmCalls.size() == 2 && !q.b.mxcVoted);
  }
  // Without the votes (piano-camera-votes=0) the strict model stalls on CAMCC: the reason the votes come first.
  {
    Rig q;
    tabletC1(q.b);
    PowerOptions po;
    po.votes = false;
    q.run(Sensor::rear, po);
    CHECK(!q.b.violations.empty());
  }
  // A vote that is refused: nothing of CAMCC is touched, the step fails, the power-down is clean.
  {
    Rig q;
    tabletC1(q.b);
    q.ops.mmFail = true;
    const auto before = q.b.snapshot();
    const SensorResult res = q.run(Sensor::rear, PowerOptions());
    CHECK(!res.idOk && res.failure == Failure::votes && res.failedStep == -kStepGcc && res.cleanOff && q.b.violations.empty());
    CHECK(same(before, q.b.snapshot()));
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
  unsigned checked = 0, phyLines = 0;
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    char file[32], sha[80];
    int at = 0;
    if (std::sscanf(line, "%31s %79s %n", file, sha, &at) != 2) continue;
    std::string sensor(file);
    if (sensor == std::string(kCsiphyTableName) + ".tbl") {
      // The PHY table: one section, its entry count.
      char item[80];
      CHECK(std::sscanf(line + at, "%79s", item) == 1);
      CHECK(std::string(item) == "lanes:dphy-4lane=" + std::to_string(sizeof kCsiphyLaneRegs / sizeof kCsiphyLaneRegs[0]));
      ++phyLines;
      continue;
    }
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
  CHECK(phyLines == 1);
  // The PHY table: 103 entries (4 data lanes x 21 at 0x000/0x400/0x800/0xc00, the clock lane's 19 at 0xe00), five settle entries
  // (one per lane), five 209 us delays, 16 skew entries; the common block at 0x1000, its status words at +0x138.
  unsigned settle = 0, skew = 0, delays = 0, lanes[5] = {};
  for (const CsiphyLaneReg &e : kCsiphyLaneRegs) {
    settle += e.kind == 2;
    skew += e.kind == 7;
    delays += e.delayUs == 209;
    CHECK(e.offset < kCsiphyTableCommon && !(e.offset & 3) && (e.kind == 0 || e.kind == 2 || e.kind == 7));
    ++lanes[e.offset >= 0xe00 ? 4 : e.offset >> 10];
  }
  CHECK(settle == 5 && skew == 16 && delays == 5);
  CHECK(lanes[0] == 21 && lanes[1] == 21 && lanes[2] == 21 && lanes[3] == 21 && lanes[4] == 19);
  CHECK(kCsiphyTableCommon == 0x1000 && kCsiphyTableStatus == 0x138);
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

// ========================================================================================================== level 3
// The board as c1 found it, with the camera AHB's source on the XO (a consistent state: PLL0 is off).
struct L3Rig : Rig {
  L3Rig()
  {
    tabletC1(b);
    b.r[kCamCc + kCcSlowAhbRcg + 4] = 0;
  }
  StreamResult run(Sensor w, StreamOptions o = StreamOptions(), PowerOptions po = PowerOptions())
  {
    po.camss = true;
    po.votes = true;
    po.mclk24 = w == Sensor::rear;
    Survey s = survey();
    Session<TestOps> session(ops, po);
    Stream<TestOps> st(ops, session, o);
    StreamResult res = st.run(w, loader, pmic, so, s);
    plan = st.routePlan();
    return res;
  }
  RoutePlan plan;
};
// What level 3 leaves changed on purpose: CAMCC PLL0's and the MCLK PLL's configuration words (Linux writes them once, at its probe,
// and leaves the PLL configured in standby with RESET_N), the window's table pages.
static std::map<uint64_t, uint32_t> l3Snapshot(const Board &b)
{
  std::map<uint64_t, uint32_t> s = b.snapshot();
  for (uint32_t o = 0; o <= 0x30; o += 4) s.erase(kCamCc + kCcPll0 + o);
  for (uint32_t o : {0x0u, 0x10u, 0x1cu, 0x20u, 0x24u}) s.erase(kMclkCc + kMclkPll + o);
  // The SMMU's TLB invalidate and sync words are commands (write-only), not state.
  for (auto it = s.begin(); it != s.end();) {
    const uint64_t pa = it->first;
    const bool command = pa == kSmmu + 0x68 || pa == kSmmu + 0x70 ||
                         (pa >= kSmmu + 128 * 4096 && pa < kSmmu + kSmmuBytes && ((pa & 0xfff) == 0x618 || (pa & 0xfff) == 0x7f0));
    it = command ? s.erase(it) : std::next(it);
  }
  return s;
}
// The words the window's release keeps on purpose (CameraSmmu.h: a found value this SMMU would refuse): each must hold the kept value
// (S2CR fault; CBAR/CBA2R our stage-1 words), and they are then left out of the as-found comparison.
static std::map<uint64_t, uint32_t> withoutKept(std::map<uint64_t, uint32_t> s, const StreamResult &res, const RoutePlan &p, const Board &b)
{
  if (p.route != Route::window) return s;
  if (res.release.s2crKept) {
    CHECK(b.v(kSmmu + 0xc00 + 4 * p.slot) == kS2crFault);
    s.erase(kSmmu + 0xc00 + 4 * p.slot);
  }
  if (res.release.cbarKept) {
    CHECK(b.v(kSmmu + 0x1000 + 4 * p.bank) == iova::kCbarStage1 && b.v(kSmmu + 0x1800 + 4 * p.bank) == 1);
    s.erase(kSmmu + 0x1000 + 4 * p.bank);
    s.erase(kSmmu + 0x1800 + 4 * p.bank);
  }
  return s;
}
static bool asFound(const std::map<uint64_t, uint32_t> &before, const StreamResult &res, const RoutePlan &p, const Board &b)
{
  return same(withoutKept(before, res, p, b), withoutKept(l3Snapshot(b), res, p, b));
}
static void checkBars(const StreamResult &r, const Board &b)
{
  bool exact = true;
  for (unsigned band = 0; band < 8; ++band)
    for (unsigned p = 0; p < 4; ++p) exact = exact && r.stats.band[band][p] == b.rgb[band][p];
  CHECK(exact);
  CHECK(r.stats.ch[0].min == b.rgb[0][0] && r.stats.ch[3].max == b.rgb[7][3]);
}

static void testL3Rear()
{
  L3Rig r;
  const auto before = l3Snapshot(r.b);
  const StreamResult res = r.run(Sensor::rear);
  CHECK(res.ok && res.cleanOff && res.bufferFreed && res.failedStep == 0);
  for (auto &v : r.b.violations) std::printf("  violation: %s\n", v.c_str());
  CHECK(r.b.violations.empty() && res.refusedStores == 0);
  CHECK(res.route == Route::window && res.busAddr == kIovaWindowBase && res.pa == r.b.memPa && res.bytes == 16u << 20);
  CHECK(res.bufDone == 2 && res.sof >= 2 && res.eof >= 2 && res.lateFrames == 0 && r.b.smmuFaults == 0 && r.b.framesWritten >= 2);
  CHECK(res.settle == 29 && res.phyWritten == 87 && res.phySkipped == 16);
  CHECK(res.tableWrites == 7 + 30 + 278 + 1 + 1 && res.controlWrites == 5);   // start, init, the mode, on, off; 5 controls
  CHECK(res.stats.done && res.stats.guardIntact && res.stats.stride == 5100 && res.stats.written == 5100ull * 3072);
  checkBars(res, r.b);
  for (unsigned p = 0; p < 4; ++p) CHECK(res.stats.ch[p].count == 4080ull * 3072 / 4);
  // Everything as found: votes withdrawn, MM1 off, the SMMU's group and bank as found (but the found words this SMMU refuses: the
  // tablet's free S2CR 0x0300006e and a free bank's stage-2 CBAR 0, kept at fault and at our stage-1 words), TITAN_TOP collapsed
  // (the CAMSS blocks reset), the clocks, the branches, GCC, the pins and the rails.
  CHECK(res.release.s2crKept && res.release.cbarKept && !res.release.error);
  CHECK(r.plan.slot == 5 && r.plan.bank == 81 && r.plan.foundS2cr == 0x0300006eu);   // c3r's group and bank
  CHECK(asFound(before, res, r.plan, r.b));
  CHECK(!r.b.mm1On && !r.b.mxcVoted && r.ops.mmBusCalls.size() == 2 && r.ops.mmCalls.size() == 2);
  CHECK(r.b.allocs == 1 && r.b.frees == 1 && r.b.invalidates == 1 && r.b.cleans == 2);
  // The window's table: the buffer's eight 2 MiB blocks at 0x20000000, nothing else.
  unsigned blocks = 0;
  for (unsigned i = 0; i < 512; ++i) blocks += (r.b.tableMem[512 + i] & 3) == 1;
  CHECK(blocks == 8 && (r.b.tableMem[0] & 3) == 3);
  // The CSIPHY's version needs no store besides SHOW_REV_ID; the PLL0 configuration stays (standby, RESET_N).
  CHECK((r.b.v(kCamCc + kCcPll0) & kPllResetN) && !(r.b.v(kCamCc + kCcPll0) & kPllOutctrl) && r.b.v(kCamCc + kCcPll0 + kPllOpmode) == 0);
}

static void testL3Variants()
{
  // Frame mode pads the lines to 16 bytes: the statistics find the stride from the written extent.
  {
    L3Rig r;
    r.b.framePadded = true;
    const StreamResult res = r.run(Sensor::rear);
    CHECK(res.ok && res.stats.stride == 5104 && res.stats.written == 5104ull * 3072);
    checkBars(res, r.b);
  }
  // The scene (no test pattern): the flat grey of the model.
  {
    L3Rig r;
    StreamOptions o;
    o.pattern = false;
    o.frames = 3;
    const StreamResult res = r.run(Sensor::rear, o);
    CHECK(res.ok && res.bufDone == 3 && res.stats.ch[0].mean() == 300 && res.stats.ch[3].mean() == 303);
    CHECK(res.stats.ch[1].hist[300 >> 7] == res.stats.ch[1].count);
    // the flat grey ignores the controls: the exposure loop's second sample says so and ends it
    CHECK(res.aeVerdict == AeVerdict::noResponse && res.aeCount == 2 && res.ae[0].exposure == 3840 && res.ae[0].gain == 192);
    CHECK(res.readBack && res.readExposure == 3840 && res.readGain == 192);
  }
  // The front: CSIPHY4, its 3264x1836 mode over the software I2C (880 writes), MCLK4 from the XO.
  {
    L3Rig r;
    const auto before = l3Snapshot(r.b);
    const StreamResult res = r.run(Sensor::front);
    for (auto &v : r.b.violations) std::printf("  violation: %s\n", v.c_str());
    CHECK(res.ok && res.cleanOff && r.b.violations.empty() && res.bytes == 8u << 20);
    // its driver's control defaults (exposure 1024, gain 7936, the test pattern off), read back
    CHECK(res.tableWrites == 1 + 811 + 54 + 4 + 4 && res.controlWrites == 4 && res.settle == 29);
    CHECK(res.readBack && res.readExposure == 1024 && res.readGain == 7936 && r.b.frontSlave.reg[0x3019] == 0xd2);
    CHECK(res.aeVerdict == AeVerdict::noResponse);
    CHECK(res.stats.stride == 4080 && res.stats.ch[2].mean() == 302);
    CHECK(asFound(before, res, r.plan, r.b));
  }
  // A bypass the boot chain left: physical addresses, no SMMU store at all.
  {
    L3Rig r;
    r.b.r[kSmmu + 0x800 + 4 * 7] = 0x80000000u | kCamStream;
    r.b.r[kSmmu + 0xc00 + 4 * 7] = 1u << 16;
    const StreamResult res = r.run(Sensor::rear);
    CHECK(res.ok && res.route == Route::bypass && res.busAddr == r.b.memPa && r.b.tableMem.empty());
    CHECK(r.b.smmuStores == 0 && r.b.violations.empty());
    checkBars(res, r.b);
  }
  // The stream's own exact group left faulting by the boot chain (redirected in place, then given back as found: its fault S2CR is
  // a configuration this SMMU has).
  {
    L3Rig r;
    r.b.r[kSmmu + 0x800 + 4 * 6] = 0x80000000u | kCamStream;
    r.b.r[kSmmu + 0xc00 + 4 * 6] = kS2crFault;
    const auto before = l3Snapshot(r.b);
    const StreamResult res = r.run(Sensor::rear);
    CHECK(res.ok && res.route == Route::window && r.plan.window.reuse && r.plan.slot == 6 && !res.release.s2crKept && res.release.cbarKept);
    CHECK(r.b.violations.empty() && asFound(before, res, r.plan, r.b) && r.b.v(kSmmu + 0x800 + 4 * 6) == (0x80000000u | kCamStream));
  }
  // A group that translates through someone's bank: refused before any stream; nothing of the SMMU or the sensor's stream touched.
  {
    L3Rig r;
    r.b.r[kSmmu + 0x800 + 4 * 9] = 0x80000000u | kCamStream;
    r.b.r[kSmmu + 0xc00 + 4 * 9] = 12;
    r.b.r[kSmmu + (128 + 12) * 4096] = 1;   // the bank's SCTLR.M
    const auto before = l3Snapshot(r.b);
    const StreamResult res = r.run(Sensor::rear);
    CHECK(!res.ok && res.failure == StreamFailure::refused && res.failedStep == -kStepL3Smmu && res.cleanOff && r.b.allocs == 0);
    for (auto &v : r.b.violations) std::printf("  violation: %s\n", v.c_str());
    CHECK(asFound(before, res, r.plan, r.b) && r.b.violations.empty() && r.b.framesWritten == 0 && r.b.smmuStores == 0);
  }
}

static void testL3Failures()
{
  struct Case {
    const char *what;
    std::function<void(L3Rig &)> inject;
    int step;
    StreamFailure failure;
  };
  const Case cases[] = {
    {"PLL0 never locks", [](L3Rig &r) { r.b.pll0NoLock = true; }, kStepL3Pll, StreamFailure::pll},
    {"MM1 refused", [](L3Rig &r) { r.ops.mmBusFail = true; }, kStepL3Bus, StreamFailure::votes},
    {"no frame buffer", [](L3Rig &r) { r.b.dmaFail = true; }, kStepL3Memory, StreamFailure::memory},
    {"CSID reset stuck", [](L3Rig &r) { r.b.csidResetStuck = true; }, kStepL3CsidReset, StreamFailure::csidReset},
    {"the sensor never streams", [](L3Rig &r) { r.b.sensorMute = true; }, kStepL3Stream, StreamFailure::noFrame},
    {"the sensor is missing", [](L3Rig &r) { r.b.rearAbsent = true; }, kStepL3Sensor, StreamFailure::sensor},
  };
  for (const Case &k : cases) {
    L3Rig r;
    k.inject(r);
    const auto before = l3Snapshot(r.b);
    const StreamResult res = r.run(Sensor::rear);
    const bool ok = !res.ok && res.failedStep == -k.step && res.failure == k.failure && res.cleanOff && r.b.violations.empty() &&
                    asFound(before, res, r.plan, r.b) && !r.b.mm1On && !r.b.mxcVoted && r.b.frees == r.b.allocs;
    if (!ok)
      std::printf("  L3 %s: step %d failure %s clean %d violations %zu\n", k.what, -res.failedStep, streamFailureName(res.failure),
                  res.cleanOff, r.b.violations.size());
    CHECK(ok);
  }
  // TITAN_TOP already on (someone else's CAMSS): refused before any level 3 store.
  {
    L3Rig r;
    r.b.r[kCamCc + kCcGdscClk] = 1;
    r.b.r[kCamCc + kGdscTitanTop] = kGdscPwrOn | 0x00222000;   // powered and requested on (someone else's)
    r.b.r[kCamCc + kGdscTitanTop + 4] = kGdscCfgUpDone;
    const StreamResult res = r.run(Sensor::rear);
    CHECK(!res.ok && res.failure == StreamFailure::refused && res.failedStep == -kStepL3Sensor && res.writes == 0);
  }
  // An overflow into the guard (64 lines more than the mode): reported, not ok, still clean.
  {
    L3Rig r;
    r.b.extraLines = 64;
    const StreamResult res = r.run(Sensor::rear);
    CHECK(!res.ok && res.bufDone == 2 && !res.stats.guardIntact && res.cleanOff && r.b.violations.empty() && r.b.smmuFaults == 0);
  }
  // An overflow past the buffer: the window faults it (nothing reaches other memory), the guard says so.
  {
    L3Rig r;
    r.b.extraLines = 400;
    const StreamResult res = r.run(Sensor::rear);
    CHECK(!res.ok && !res.stats.guardIntact && r.b.smmuFaults > 0 && r.b.violations.empty() && res.cleanOff);
  }
}

static void testL3Stops()
{
  for (int step : {kStepL3Sensor, kStepL3Axi, kStepL3Pll, kStepL3Clocks, kStepL3Versions, kStepL3Smmu, kStepL3Memory, kStepL3CsidReset,
                   kStepL3Wm, kStepL3Csid, kStepL3Phy, kStepL3Sensor2}) {
    L3Rig r;
    StreamOptions o;
    o.stopAt = step;
    const auto before = l3Snapshot(r.b);
    const StreamResult res = r.run(Sensor::rear, o);
    const bool ok = res.failure == StreamFailure::stopped && res.failedStep == -step && res.cleanOff && r.b.violations.empty() &&
                    asFound(before, res, r.plan, r.b) && r.b.frees == r.b.allocs && !r.b.mm1On;
    if (!ok) std::printf("  L3 stop at %d: failure %s clean %d\n", step, streamFailureName(res.failure), res.cleanOff);
    CHECK(ok);
  }
}

// The model's SMMU rules themselves (the tablet's c1 SMMU): the old release's store of c3r's found S2CR, a stage-2 CBAR on a stage-1
// only SMMU, and TLB maintenance without the camera NoC are each caught as a violation (each was, or could be, an abort).
static void testSmmuRules()
{
  Board b;
  tabletC1(b);
  b.wr(kSmmu + 0xc00 + 4 * 5, 0x0300006eu);   // c3rp/c3r: "Unhandled implementation specific error" right after this store
  CHECK(b.violations.size() == 1 && b.violations[0].find("S2CR 5") != std::string::npos);
  b.wr(kSmmu + 0x1000 + 4 * 81, 0);           // CBAR "stage 2" (IDR0 has no S2TS)
  CHECK(b.violations.size() == 2);
  b.wr(kSmmu + 0x70, 0xffffffffu);            // TLB sync with MM1 off, HF AXI off, the camera NoC unclocked
  CHECK(b.violations.size() == 3);
  b.wr(kSmmu + 0xc00 + 4 * 5, kS2crFault);    // what the release now leaves
  b.wr(kSmmu + 0x1000 + 4 * 81, iova::kCbarStage1);
  CHECK(b.violations.size() == 3 && b.v(kSmmu + 0xc00 + 4 * 5) == kS2crFault);
  CHECK(!s2crRestorable(0x0300006eu, 83) && s2crRestorable(kS2crFault, 83) && s2crRestorable(1u << 16, 83) && s2crRestorable(82, 83));
  CHECK(!cbarRestorable(0, 0x4c017e7fu) && cbarRestorable(iova::kCbarStage1, 0x4c017e7fu) && cbarRestorable(0, 0x6c017e7fu));
}

// c3rp/c3r's teardown (2026-11-12): the window released while the camera NoC is voted and clocked, before anything it depends on goes;
// the found words this SMMU refuses kept; the report (the chip ID first) logged before the power-down; the order of the events.
static void testL3Teardown()
{
  for (int stop : {0, int(kStepL3Sensor2)}) {   // c3r, and c3rp (stopped before stream-on)
    L3Rig r;
    StreamOptions o;
    o.stopAt = stop;
    const auto before = l3Snapshot(r.b);
    const StreamResult res = r.run(Sensor::rear, o);
    for (auto &v : r.b.violations) std::printf("  violation: %s\n", v.c_str());
    CHECK(r.b.violations.empty() && res.cleanOff && res.bufferFreed && asFound(before, res, r.plan, r.b));
    CHECK(stop ? (res.failure == StreamFailure::stopped && res.failedStep == -kStepL3Sensor2 && !res.streamed) : res.ok);
    const Board &b = r.b;
    const long wm = b.lastEvent("wm off"), rdi = b.lastEvent("rdi stop"), sync = b.lastEvent("smmu sync"), camnoc = b.lastEvent("camnoc off"),
               hf = b.lastEvent("hf axi off"), mm1 = b.lastEvent("mm1 off"), titan = b.lastEvent("titan off"), mxc = b.lastEvent("mxc off");
    CHECK(wm >= 0 && wm < rdi && rdi < sync && sync < camnoc && camnoc < hf && hf < mm1 && mm1 < titan && titan < mxc);
    const long id = r.ops.logIndex("rear S5KJN1: chip ID 0x38e1 (expected 0x38e1): OK over CCI0 master 1"),
               report = r.ops.logIndex("level 3 (before the power-down): rear S5KJN1 4080x3072 RAW10"), down = r.ops.logIndex("step 340"),
               kept = r.ops.logIndex("smmu: group 5's S2CR left at fault (00020000): the found 0300006e"),
               found = r.ops.logIndex("smmu: window: group 5 (found smr 00000000 s2cr 0300006e: not restorable), bank 81 of 83");
    CHECK(id >= 0 && found > id && report > found && down > report && kept > down);
    CHECK(r.ops.logIndex("route: our translating window") > report && r.ops.logIndex("route: our translating window") < down);
    if (stop) CHECK(r.ops.logIndex("stopped before stream-on (piano-camera-stop=312)") > 0 && r.b.framesWritten == 0);
    else CHECK(r.ops.logIndex("frame: row 3071: 4080 of 5100 bytes not the fill") > report && r.ops.logIndex("stream: frame 2: SOF +") > report);
  }
}

// The stream's diagnostics: per-frame times, a stale BUF_DONE at stream-on (not a frame), a BUF_DONE left before stream-on (logged
// and cleared), the first rows' bytes.
static void testL3Diagnostics()
{
  {
    L3Rig r;
    const StreamResult res = r.run(Sensor::rear);
    CHECK(res.ok && res.bufDone == 2 && res.staleBufDone == 0 && res.preBuf == 0);
    CHECK(res.bufUs[0] >= 33000 && res.bufUs[1] >= res.bufUs[0] + 33000 && res.bufUs[1] <= res.bufUs[0] + 34000);
    CHECK(res.sofUs[0] && res.sofUs[0] <= res.bufUs[0] && res.eofUs[1] > res.bufUs[0] && res.eofUs[1] <= res.bufUs[1]);
    CHECK(res.phyFirstRead && res.firstFrameUs == res.bufUs[0] && res.lastIntervalUs >= 33000);
    // The bars' values have no low bits: each group's fifth byte is 0, so 4 of 5 bytes of a row are not the fill.
    CHECK(res.stats.rowIndex[0] == 0 && res.stats.rowIndex[4] == 3071 && res.stats.rowNonzero[0] == 4080 && res.stats.rowNonzero[4] == 4080);
    CHECK(!std::memcmp(res.stats.rowHead[0], res.stats.head, 16) && res.stats.rowHead[1][0] == uint8_t(r.b.rgb[0][2] >> 2));
  }
  {
    L3Rig r;
    r.b.staleBufDoneAtStart = true;
    const StreamResult res = r.run(Sensor::rear);
    CHECK(res.ok && res.staleBufDone == 1 && res.bufDone == 2 && res.bufUs[0] >= 33000 && r.b.violations.empty());
  }
  {
    L3Rig r;
    r.b.bufDoneAtRdiStart = true;
    const StreamResult res = r.run(Sensor::rear);
    CHECK(res.ok && res.preBuf == kCsidBufRdi0 && res.staleBufDone == 0 && res.bufDone == 2 && res.bufUs[0] >= 33000);
    CHECK(r.ops.logIndex("stream: before stream-on: top 00000000 buf-done 00000002") > 0);
  }
  {   // no frame at all: the deadline, the report still before the power-down
    L3Rig r;
    r.b.sensorMute = true;
    const StreamResult res = r.run(Sensor::rear);
    CHECK(res.failure == StreamFailure::noFrame && res.cleanOff && r.b.violations.empty() && res.streamUs >= 1000000);
    CHECK(r.ops.logIndex("level 3 (before the power-down): rear S5KJN1 4080x3072 RAW10: FAILED at step 320") <
          r.ops.logIndex("step 340"));
  }
}

static void testL3Allowlist()
{
  Board b;
  TestOps ops{b};
  Session<TestOps> s(ops, PowerOptions());
  Stream<TestOps> st(ops, s, StreamOptions());
  CHECK(st.writable(kGcc + kGccCameraHfAxi) && st.writable(csiphyBase(1) + 0x1014) && st.writable(kCsidLite0 + kCsidRdiCfg0));
  CHECK(st.writable(kTfeLite0 + kWm0 + kWmCfg) && st.writable(kCamCc + kCcCphyRxRcg + 4) && st.writable(kCamCc + kCcCsiphy1));
  CHECK(!st.writable(csiphyBase(4)) && !st.writable(kCamCc + kCcCsiphy4) && !st.writable(kTfeLite0 + kWm0 + 0x100));
  CHECK(!st.writable(kGcc + kGccCameraAhb) && !st.writable(kCamCc + kGdscTfe0) && !st.writable(kCamCc + kGdscTitanTop));
  CHECK(!st.writable(kSmmu + 0x800) && !st.writable(kSmmu + 0x68) && !st.writable(kBlocks[0].pa) && !st.writable(kCsidLite0 + 0xa00));
  CHECK(!st.writable(kCamCc + 0x1000) && !st.writable(kTfeLite0 + 0x1000) && !st.writable(kCci0));
}

// The statistics alone: RAW10 unpacking (the low bits in the fifth byte), the bands, the histogram, the guard.
static void testFrameStats()
{
  const unsigned w = 16, h = 4;
  std::vector<uint8_t> buf(4096, 0);
  for (unsigned y = 0; y < h; ++y)
    for (unsigned x = 0; x < w; x += 4) {
      uint8_t *g = buf.data() + y * raw10Bytes(w) + x / 4 * 5;
      for (unsigned k = 0; k < 4; ++k) {
        const unsigned v = (x + k) * 64 + y;   // 10 bits
        g[k] = uint8_t(v >> 2);
        g[4] |= uint8_t((v & 3u) << (2 * k));
      }
    }
  const uint32_t frame = raw10Bytes(w) * h;
  for (uint32_t i = frame; i < 4096; ++i) buf[i] = 0xa5;
  FrameStats s;
  frameStats(buf.data(), 4096, frame, w, h, s);
  CHECK(s.done && s.guardIntact && s.stride == 20 && s.written == frame);
  CHECK(s.ch[0].min == 0 && s.ch[1].max == 15 * 64 + 2 && s.ch[3].count == 16);
  CHECK(s.band[0][0] == 1 && s.band[0][1] == 65 && s.band[7][3] == 15 * 64 + 2);   // two pixels a band, rows 0 and 2 (or 1 and 3)
  CHECK(s.ch[0].hist[0] == 2 && s.ch[0].hist[7] == 2);   // x = 0, 2 (v < 128) and x = 14 (896..): the 8 bins of 128
  buf[frame + 10] = 0;
  frameStats(buf.data(), 4096, frame, w, h, s);
  CHECK(!s.guardIntact && s.guardFirstBad == 10);
}

// The exposure loop (CameraStream.h): on the model's lit scene (its value follows exposure x gain a frame late) both sensors reach
// the target from their drivers' defaults, the final frames' means with them; a black scene ends DARK at the longest exposure and
// the highest gain, a blinding one saturated at the shortest and lowest; the colour bars never run it; piano-camera-ae=0 neither.
static void testExposureLoop()
{
  const unsigned target = StreamOptions().aeTarget;
  auto near = [&](uint32_t mean) { return mean >= 64 + target - target / 4 && mean <= 64 + target + target / 4; };
  for (Sensor w : {Sensor::rear, Sensor::front}) {
    for (uint64_t scene : {20ull, 1ull, 400ull}) {   // too bright at the defaults, too dim, much too bright
      L3Rig r;
      r.b.scene[w == Sensor::rear ? 0 : 1] = scene;
      StreamOptions o;
      o.pattern = false;
      const StreamResult res = r.run(w, o);
      const bool ok = res.ok && res.aeVerdict == AeVerdict::converged && near(res.stats.ch[0].mean()) && near(res.ae[res.aeCount - 1].mean[0]);
      if (!ok)
        std::printf("  ae %s scene %llu: %s after %u samples, final mean %u (exposure %u gain %u)\n", w == Sensor::rear ? "rear" : "front",
                    (unsigned long long)scene, aeVerdictName(res.aeVerdict), res.aeCount, res.stats.ch[0].mean(), res.exposure, res.gain);
      if (!ok)
        for (unsigned i = 0; i < res.aeCount; ++i)
          std::printf("    sample %u: exposure %u gain %u means %u/%u/%u/%u above %u sat %u\n", i + 1, res.ae[i].exposure, res.ae[i].gain,
                      res.ae[i].mean[0], res.ae[i].mean[1], res.ae[i].mean[2], res.ae[i].mean[3], res.ae[i].above, res.ae[i].satPermille);
      CHECK(ok && res.cleanOff && r.b.violations.empty());
      CHECK(res.aeCount >= 2 && res.aeFrames >= 1 + 3 * (res.aeCount - 1) && res.bufDone == 2);
      const ExposureFacts &f = w == Sensor::rear ? kRearExposure : kFrontExposure;
      CHECK(res.ae[0].exposure == (w == Sensor::rear ? 3840u : f.eDefault) && res.ae[0].gain == f.gDefault);
      CHECK(res.exposure >= f.eMin && res.gain >= f.gMin && res.gain <= f.gMax);
    }
  }
  {   // the tablet's padded rear lines (5104): the samples find the stride
    L3Rig r;
    r.b.framePadded = true;
    r.b.scene[0] = 20;
    StreamOptions o;
    o.pattern = false;
    const StreamResult res = r.run(Sensor::rear, o);
    CHECK(res.ok && res.aeVerdict == AeVerdict::converged && near(res.stats.ch[0].mean()) && res.stats.stride == 5104);
    CHECK(res.ae[0].mean[0] + 3 == res.ae[0].mean[3]);   // whole pixel groups: the four positions 0..3 apart, as the model writes them
  }
  {   // no light
    L3Rig r;
    r.b.sceneBlack = true;
    const StreamResult res = r.run(Sensor::front);
    CHECK(res.ok && res.aeVerdict == AeVerdict::dark && res.exposure == 2820 - 12 && res.gain == kFrontExposure.gMax);
    CHECK(std::any_of(r.ops.log.begin(), r.ops.log.end(), [](const std::string &l) { return l.find("DARK at the longest") != std::string::npos; }));
  }
  {   // blinding
    L3Rig r;
    r.b.scene[0] = 1000000;
    StreamOptions o;
    o.pattern = false;
    const StreamResult res = r.run(Sensor::rear, o);
    CHECK(res.ok && res.aeVerdict == AeVerdict::bright && res.exposure == kRearExposure.eMin && res.gain == kRearExposure.gMin);
  }
  {   // the colour bars and piano-camera-ae=0: no loop; the bars keep the option's gain
    L3Rig r;
    r.b.scene[0] = 20;
    const StreamResult bars = r.run(Sensor::rear);
    CHECK(bars.ok && bars.aeVerdict == AeVerdict::none && bars.aeFrames == 0 && bars.gain == 64);
    L3Rig q;
    q.b.scene[1] = 20;
    StreamOptions o;
    o.ae = false;
    const StreamResult off = q.run(Sensor::front, o);
    CHECK(off.ok && off.aeVerdict == AeVerdict::none && off.exposure == 1024 && off.gain == 7936);
  }
  // the rule alone: converged inside a quarter, no response, the limits
  uint64_t next = 0;
  AeStep s;
  s.above = 150;
  CHECK(aeRule(s, nullptr, 1000, 0, 10, 100000, 160, next) == AeVerdict::converged);
  s.above = 40;
  CHECK(aeRule(s, nullptr, 1000, 0, 10, 100000, 160, next) == AeVerdict::none && next == 4000);
  AeStep p = s;
  CHECK(aeRule(s, &p, 4000, 1000, 10, 100000, 160, next) == AeVerdict::noResponse);
  s.above = 0;
  CHECK(aeRule(s, nullptr, 100000, 0, 10, 100000, 160, next) == AeVerdict::dark);
  s.above = 30;
  CHECK(aeRule(s, nullptr, 100000, 0, 10, 100000, 160, next) == AeVerdict::dim);
  s.above = 900;
  s.satPermille = 600;
  CHECK(aeRule(s, nullptr, 10, 0, 10, 100000, 160, next) == AeVerdict::bright);
  uint32_t e = 0, g = 0;
  aeSplit(kFrontExposure, 2808, 1024ull * 7936, e, g);
  CHECK(e == 2808 && g >= 2893 && g <= 2895);   // the exposure first, at the lowest gain, then the gain
  aeSplit(kRearExposure, 4266, 100ull * 32, e, g);
  CHECK(e == 100 && g == 32);
}

// The plan (CameraPlan.h): the names parse to their images' arguments; on one board (c1's tablet) c3rp, c2fx, c3r, c3rs and c3f run
// one after another, each from what the one before left (the SMMU words the release keeps, PLL0's configuration), each with its
// power-down complete, the board as found after the last.
static void testPlan()
{
  const Plan p = parsePlan("c3rp,c2f,c3r,c3rs,c3f");
  CHECK(!p.error && p.count == 5 && p.level == 3);
  CHECK(!std::strcmp(p.item[0]->name, "c3rp") && p.item[0]->level == 3 && p.item[0]->sensor == 1 && p.item[0]->stop == 312);
  CHECK(p.item[1]->level == 2 && p.item[1]->sensor == 2 && p.item[1]->stop == 0);
  CHECK(parsePlan("c2fx").error == -1);   // answered on the tablet (2026-11-13): the minimal path is gone
  CHECK(p.item[2]->pattern == 1 && p.item[2]->frames == 2 && p.item[3]->pattern == 0 && p.item[3]->frames == 4);
  CHECK(p.item[4]->level == 3 && p.item[4]->sensor == 2);
  const Plan bad = parsePlan("c3r,c3x");
  CHECK(bad.error == -1 && bad.bad == 4 && bad.count == 0);
  CHECK(parsePlan("").error == -3 && parsePlan(nullptr).error == -3 && parsePlan("c3r,").error == -1 && parsePlan(",c3r").error == -1);
  CHECK(parsePlan("c3r,c3r,c3r,c3r,c3r,c3r,c3r,c3r").count == 8 && parsePlan("c3r,c3r,c3r,c3r,c3r,c3r,c3r,c3r,c3r").error == -2);
  CHECK(parsePlan("c2f").level == 2 && parsePlan("c1b").item[0]->stop == 203 && parsePlan("c2r24").item[0]->mclk24 == 1);
  CHECK(parsePlan("c2rg").item[0]->rearBus == 1 && parsePlan("c3r extra").count == 1);
  CHECK(planGoesOn(true, false) && !planGoesOn(false, false) && !planGoesOn(true, true));

  L3Rig r;
  const auto before = l3Snapshot(r.b);
  StreamResult last, first;   // the first level-3 run kept the found words this SMMU refuses; the later ones find its words
  bool haveFirst = false;
  for (unsigned i = 0; i < p.count; ++i) {
    const PlanItem &it = *p.item[i];
    const Sensor w = it.sensor == 2 ? Sensor::front : Sensor::rear;
    if (it.level == 3) {
      StreamOptions o;
      o.frames = it.frames;
      o.pattern = it.pattern != 0;
      o.stopAt = it.stop >= 300 ? it.stop : 0;
      PowerOptions po;
      po.stopAt = it.stop < 300 ? it.stop : 0;
      last = r.run(w, o, po);
      if (!haveFirst) {
        first = last;
        haveFirst = true;
        CHECK(first.release.s2crKept && first.release.cbarKept);
      } else {
        CHECK(!last.release.s2crKept && !last.release.cbarKept && !last.release.error);   // its found words are configurations
      }
      const bool ok = it.stop ? last.failure == StreamFailure::stopped && last.failedStep == -int(it.stop) : last.ok;
      if (!ok) std::printf("  plan %s: failure %s at %d\n", it.name, streamFailureName(last.failure), -last.failedStep);
      CHECK(ok && last.cleanOff && last.bufferFreed && planGoesOn(last.cleanOff, last.pa && !last.bufferFreed));
      CHECK(last.route == Route::window && r.plan.slot == 5 && r.plan.bank == 81);   // the same free group and bank each time
    } else {
      PowerOptions po;
      po.mclk24 = it.mclk24 != 0;
      po.rearCci = !it.rearBus;
      po.stopAt = it.stop;
      const SensorResult res = r.Rig::run(w, po);
      CHECK(res.idOk && res.cleanOff && planGoesOn(res.cleanOff, false));
    }
  }
  for (auto &v : r.b.violations) std::printf("  violation: %s\n", v.c_str());
  CHECK(r.b.violations.empty() && !r.b.mm1On && !r.b.mxcVoted && r.b.allocs == r.b.frees && r.b.smmuFaults == 0);
  CHECK(haveFirst && asFound(before, first, r.plan, r.b));
}

int main()
{
  testSurvey();
  testRearCci();
  testRearGpio();
  testRearMclk24();
  testFront();
  testFrontCamss();
  testFailures();
  testTabletC1();
  testAllowlist();
  testCci();
  testTables();
  testFrameStats();
  testL3Allowlist();
  testL3Rear();
  testL3Variants();
  testL3Failures();
  testL3Stops();
  testSmmuRules();
  testL3Teardown();
  testL3Diagnostics();
  testExposureLoop();
  testPlan();
  if (failures) {
    std::printf("camera_host: %d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("camera_host: all checks passed\n");
  return 0;
}
