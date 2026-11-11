// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoBluetooth levels 2..4 (BtBringup.h) and the RSC transaction (BtRpmh.h), the whole ladder against models:
//   - the board: GCC (the QUP2 vote word, five branches that run only when voted, the SE6 RCG with its update bit and DFS registers),
//     TLMM (the four UART pins, SW_CTRL), the QUP2 wrapper and SE6 as a GENI UART (TX command and FIFO, RX sequencer and FIFO with its
//     status, cancel/abort, internal loopback, manual RFR, CTS masking, the serial clock from the source and the divider), BT_EN's
//     PMIC GPIO over SPMI (owner, kind, five registers), the PMU's rails through the real shared helper (PianoWcnRails.h) on a PMIC
//     model, and a clock;
//   - the wire: the engine's TX pad reaches the simulated controller (tests/QcaSim.h) only when gpio26 is the engine's, at the speed
//     the registers give, and only once the controller is powered (rails up, BT_EN high) for 50 ms; the controller's events come back
//     through gpio27 when the engine listens and RFR is ready, at the controller's speed; CTS is the controller's readiness;
//   - success at 3.2 Mbit/s with DFS off (the RCG programmed) and on (a level chosen), at 115200 only, levels 2 and 3 alone, a stop
//     request, a suspend (safeOff) and a resume, Wi-Fi owning the rails first;
//   - faults at every level: BT_EN owned by another EE, not an LV/MV GPIO, a denied PMIC write, a refused rail vote, a branch that
//     never runs, SE6 without the UART protocol, an RCG that never updates, a broken loopback, a controller whose RTS never asserts,
//     one deaf at the new speed, a missing rampatch, a pending sleep in the middle of the download;
//   - and the invariants: no SE6/QUP2 access while unclocked, no byte to the controller before the pins are the engine's, only BT_EN's
//     five registers written, only this driver's vote bits withdrawn, the rails put on every failure.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. -I../../PianoKeyboard -I../../include bt_bringup_host.cpp -o /tmp/bt_bringup_host
#include "BtBringup.h"
#include "BtRpmh.h"
#include "PianoWcnRails.h"
#include "QcaSim.h"
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace pianobt;
using namespace pianobt::sim;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------------ the rails
struct Rails {
  std::map<std::string, bool> enabled;
  std::map<std::string, uint16_t> mv;
  std::map<uint16_t, std::string> byPpid;
  bool refuse = false;
  unsigned txs = 0;
  Rails()
  {
    for (const PianoWcnRail &t : piano_wcn_rail_table) {
      enabled[t.cmd_db] = !t.order;   // the PHY LDOs on, the six PMU rails off
      mv[t.cmd_db] = t.min_mv;
      byPpid[uint16_t(t.sid << 8 | (t.smps ? piano_wcn_smps_page(t.smps) : piano_wcn_ldo_page(t.ldo)))] = t.cmd_db;
    }
  }
  bool allOn() const
  {
    for (const PianoWcnRail &t : piano_wcn_rail_table)
      if (t.order && !enabled.at(t.cmd_db)) return false;
    return true;
  }
};

// ------------------------------------------------------------------------------------------------------------ the board
struct Board {
  uint64_t us = 0;
  // GCC
  std::map<uint32_t, uint32_t> gcc;
  bool stuckBranch = false, rcgStuck = false, rcgPending = false;
  unsigned rcgWrites = 0;
  // TLMM
  std::map<uint32_t, uint32_t> tlmm;
  // QUP2 / SE6
  std::map<uint32_t, uint32_t> se;
  uint32_t fwRevision = 0x0204, qupVersion = 0x30090000, hwParam = 0x20100000;   // 16 words of 32 bits each way
  bool mActive = false, sActive = false;
  uint32_t txRemaining = 0, mIrq = 0, sIrq = 0;
  std::deque<uint32_t> txFifo;
  std::deque<uint8_t> rxFifo, chipTx;
  static constexpr size_t kRxBytes = 64;
  // the controller and the wire
  Sim chip;
  bool powered = false, rtsStuck = false, loopCorrupt = false;
  uint64_t poweredAt = 0;
  unsigned toChip = 0, garbage = 0, lostBeforeReady = 0, lostEvents = 0, unclocked = 0, loopBytes = 0;
  // BT_EN
  uint8_t bten[0x50] = {};
  BtEnInfo btenInfo;
  bool pmicDenied = false;
  std::vector<std::pair<uint8_t, uint8_t>> pmicWrites;
  unsigned pmicBadRegs = 0;
  // the rails and the two radios
  Rails rails;
  volatile uint32_t railState = 0, pmuOwner = 0;
  PianoWcnRailsOps railOps{};
  PianoWcnRails bt{}, wifi{};
  // the rest
  uint64_t cancelAtToChip = 0;
  std::vector<int> steps;
  std::set<std::string> kinds;

  Board()
  {
    stock(chip);
    // BT_EN at hand-over: the stock state, low (level 1's expectation).
    bten[0x04] = kPgTypeGpio;
    bten[0x05] = 0x11;
    bten[0x40] = kPgModeOutput;
    bten[0x41] = kPgVin1;
    bten[0x42] = kPgPullNone;
    bten[0x45] = 0x01;
    bten[0x46] = kPgEnabled;
    btenInfo = {77, 0, kPgTypeGpio, 0x11};
    // The RCG as the boot chain might leave it: 19.2 MHz from TCXO, DFS off.
    gcc[kGccS6Rcg + kRcgCfg] = 1;
    railOps.context = this;
    railOps.vrm = [](void *c, const char *res, const PianoWcnVrmCmd *cmds, unsigned n) {
      Board *b = static_cast<Board *>(c);
      ++b->rails.txs;
      if (b->rails.refuse || !b->rails.enabled.count(res)) return -5;
      for (unsigned i = 0; i < n; ++i) {
        if (cmds[i].reg == PianoWcnVrmEnable) b->rails.enabled[res] = cmds[i].value != 0;
        if (cmds[i].reg == PianoWcnVrmVoltage) b->rails.mv[res] = uint16_t(cmds[i].value);
      }
      b->tick();
      return 0;
    };
    railOps.pmic = [](void *c, uint16_t ppid, uint8_t *st, uint8_t *en, uint8_t *mode, uint16_t *mv) {
      Board *b = static_cast<Board *>(c);
      auto it = b->rails.byPpid.find(ppid);
      if (it == b->rails.byPpid.end()) return -5;
      *st = *en = b->rails.enabled[it->second] ? 0x80 : 0;
      *mode = 7;
      *mv = b->rails.mv[it->second];
      return 0;
    };
    railOps.pmic_type = [](void *c, uint16_t ppid, uint8_t *type) {
      Board *b = static_cast<Board *>(c);
      auto it = b->rails.byPpid.find(ppid);
      if (it == b->rails.byPpid.end()) return -19;
      *type = it->second.rfind("ldo", 0) == 0 ? 0x04 : 0x21;
      return 0;
    };
    railOps.sleep_ms = [](void *c, uint32_t ms) { static_cast<Board *>(c)->us += uint64_t(ms) * 1000; };
    railOps.mutex_lock = [](void *) {};
    railOps.mutex_unlock = [](void *) {};
    for (PianoWcnRails *r : {&bt, &wifi}) {
      r->state = &railState;
      r->lock = &pmuOwner;
      r->ops = &railOps;
    }
    bt.tag = PianoWcnTagBt;
    wifi.tag = PianoWcnTagWifi;
  }

  // ---- derived state
  bool voted(uint32_t bit) const { auto it = gcc.find(kGccQup2Vote); return it != gcc.end() && (it->second & bit); }
  bool clocked() const
  {
    for (const Branch &b : kBranches)
      if (!voted(b.vote) || stuckBranch) return false;
    return true;
  }
  bool pinIsEngine(unsigned gpio) const
  {
    auto it = tlmm.find(gpio * kTlmmStride);
    return it != tlmm.end() && ((it->second >> 2) & 0xfu) == 1;
  }
  bool loopback() const { auto it = se.find(kUartLoopback); return it != se.end() && (it->second & kLoopRxTx); }
  uint32_t reg(uint32_t off) const { auto it = se.find(off); return it == se.end() ? 0 : it->second; }
  uint32_t g(uint32_t off) const { auto it = gcc.find(off); return it == gcc.end() ? 0 : it->second; }
  uint32_t sourceHz() const
  {
    const uint32_t r = kGccS6Rcg;
    if (g(r + kRcgDfsCmd) & 1) {
      const unsigned l = reg(kClkSel) & 7u;
      return rcgRate(g(r + kRcgDfsPerf + 4 * l), g(r + kRcgDfsM + 4 * l), g(r + kRcgDfsN + 4 * l));
    }
    return rcgRate(g(r + kRcgCfg), g(r + kRcgM), g(r + kRcgN));
  }
  uint32_t hostBaud() const
  {
    const SerClk k = decodeSerClk(reg(kSerMClkCfg));
    return k.enabled && k.divider ? sourceHz() / k.divider / 16 : 0;
  }
  bool chipReady() const { return powered && us >= poweredAt + 50000; }
  bool rfrReady() const
  {
    const uint32_t m = reg(kUartManualRfr);
    if (m & kRfrManual) return !(m & kRfrNotReady);
    return rxFifo.size() + 8 <= kRxBytes;
  }
  bool ctsOk() const
  {
    if (reg(kUartTxTransCfg) & kUartCtsMask) return true;
    if (loopback()) return rfrReady();
    return chipReady() && !rtsStuck && pinIsEngine(24);
  }

  // ---- the controller's power: rails up and BT_EN an enabled output driven high
  void power()
  {
    const bool on = rails.allOn() && (bten[0x46] & kPgEnabled) && (bten[0x40] & 3u) == kPgModeOutput && (bten[0x44] & kPgOutLevel);
    if (on && !powered) poweredAt = us;
    if (!on && powered) {   // off: the controller forgets everything
      Sim fresh;
      fresh.files = chip.files;
      fresh.expect[0] = chip.expect[0];
      fresh.expect[1] = chip.expect[1];
      std::memcpy(fresh.address, chip.nvmDefault, 6);
      std::memcpy(fresh.nvmDefault, chip.nvmDefault, 6);
      fresh.deafAfterBaud = chip.deafAfterBaud;
      chip = fresh;
      chipTx.clear();
    }
    powered = on;
  }
  // ---- the line moves on every access
  void wireOut(uint8_t b)
  {
    if (loopback()) {
      ++loopBytes;
      rxFifo.push_back(loopCorrupt && loopBytes % 7 == 0 ? uint8_t(b ^ 0x10) : b);
      return;
    }
    if (!pinIsEngine(26)) return;   // the pad is not the engine's: nothing reaches the controller
    if (!chipReady()) {
      ++lostBeforeReady;
      return;
    }
    if (hostBaud() != chip.chipBaud) {
      ++garbage;
      return;
    }
    chip.flow = !(reg(kUartTxTransCfg) & kUartCtsMask);
    ++toChip;
    chip.rxByte(b);
  }
  void tick()
  {
    power();
    while (mActive && !txFifo.empty() && ctsOk()) {
      const uint32_t w = txFifo.front();
      txFifo.pop_front();
      const unsigned n = txRemaining < 4 ? txRemaining : 4;
      for (unsigned i = 0; i < n; ++i) wireOut(uint8_t(w >> (8 * i)));
      txRemaining -= n;
      if (!txRemaining) {
        mActive = false;
        mIrq |= kMCmdDone;
      }
    }
    if (!sActive || !pinIsEngine(27) || loopback()) return;
    for (;;) {
      if (chipTx.empty()) {
        if (chip.events.empty()) break;
        auto e = chip.events.front();
        chip.events.pop_front();
        if (e.first != hostBaud()) {
          ++lostEvents;
          continue;
        }
        chipTx.insert(chipTx.end(), e.second.begin(), e.second.end());
        chipTx.push_front(kH4Event);
      }
      if (!rfrReady()) break;
      rxFifo.push_back(chipTx.front());
      chipTx.pop_front();
    }
  }

  // ---- the windows
  uint32_t read(unsigned win, uint32_t off)
  {
    tick();
    if (win == kWinGcc) {
      for (const Branch &b : kBranches)
        if (off == b.cbcr) return clocked() || (voted(b.vote) && !stuckBranch) ? (b.hwcg ? kCbcrClkOff | kCbcrHwCtl : 0u) : kCbcrClkOff;
      if (off == kGccS6Rcg) {
        const uint32_t v = g(off) | (rcgPending ? kRcgCmdUpdate : 0u);
        if (!rcgStuck) rcgPending = false;
        return v;
      }
      return g(off);
    }
    if (win == kWinTlmm) {
      if (off == kPins[kPinSwCtrl].gpio * kTlmmStride + 4) return powered ? 1u : 0u;
      auto it = tlmm.find(off);
      return it == tlmm.end() ? 0 : it->second;
    }
    if (!clocked()) {
      ++unclocked;
      return 0xffffffffu;
    }
    if (win == kWinQup) return off == kQupHwVersion ? qupVersion : 0;
    switch (off) {
    case kFwRevision: return fwRevision;
    case kHwParam0:
    case kHwParam1: return hwParam;
    case kGeniStatus: return (mActive ? kMCmdActive : 0u) | (sActive ? kSCmdActive : 0u);
    case kMIrqStatus: return mIrq;
    case kSIrqStatus: return sIrq | (rxFifo.empty() ? 0u : kSRxFifoLast);
    case kTxFifoStatus: return uint32_t(txFifo.size());
    case kRxFifoStatus: {
      const size_t n = rxFifo.size();
      return uint32_t((n + 3) / 4) | (n % 4 ? kRxLast | uint32_t(n % 4) << kRxLastValidShift : 0u);
    }
    case kRxFifo: {
      uint32_t w = 0;
      for (unsigned i = 0; i < 4 && !rxFifo.empty(); ++i) {
        w |= uint32_t(rxFifo.front()) << (8 * i);
        rxFifo.pop_front();
      }
      return w;
    }
    case kGeniIos: return (powered && pinIsEngine(27) ? kIosRx : 0u) | (ctsOk() ? 0u : kIosCts);
    case kMCmdCtrl:
    case kSCmdCtrl: return 0;
    default: return reg(off);
    }
  }
  void write(unsigned win, uint32_t off, uint32_t v, const char *what)
  {
    kinds.insert(what);
    if (win == kWinGcc) {
      if (off == kGccS6Rcg) {
        if (v & kRcgCmdUpdate) rcgPending = true;
        gcc[off] = v & ~kRcgCmdUpdate;
      } else {
        if (off >= kGccS6Rcg && off < kGccS6Rcg + 0x14) ++rcgWrites;
        gcc[off] = v;
      }
      tick();
      return;
    }
    if (win == kWinTlmm) {
      tlmm[off] = v;
      tick();
      return;
    }
    if (!clocked()) {
      ++unclocked;
      return;
    }
    if (win == kWinQup) return;
    switch (off) {
    case kMCmd0:
      if ((v >> kOpcodeShift) == kUartStartTx) {
        mActive = true;
        txRemaining = reg(kUartTxTransLen);
      }
      break;
    case kTxFifo:
      if (mActive) txFifo.push_back(v);
      break;
    case kSCmd0:
      if ((v >> kOpcodeShift) == kUartStartRead) sActive = true;
      break;
    case kMCmdCtrl:
      if (v & (kCmdCancel | kCmdAbort)) {
        mActive = false;
        txFifo.clear();
        mIrq |= (v & kCmdCancel) ? kIrqCmdCancel : kIrqCmdAbort;
      }
      break;
    case kSCmdCtrl:
      if (v & (kCmdCancel | kCmdAbort)) {
        sActive = false;
        sIrq |= (v & kCmdCancel) ? kIrqCmdCancel : kIrqCmdAbort;
      }
      break;
    case kMIrqClear: mIrq &= ~v; break;
    case kSIrqClear: sIrq &= ~v; break;
    default: se[off] = v; break;
    }
    tick();
  }
};

// ------------------------------------------------------------------------------------------------------------ the Hw
static Hw hwFor(Board &b)
{
  Hw hw;
  hw.ctx = &b;
  hw.read = [](void *c, unsigned w, uint32_t off) { return static_cast<Board *>(c)->read(w, off); };
  hw.write = [](void *c, unsigned w, uint32_t off, uint32_t v, const char *what) { static_cast<Board *>(c)->write(w, off, v, what); };
  hw.udelay = [](void *c, unsigned us) {
    static_cast<Board *>(c)->us += us;
    static_cast<Board *>(c)->tick();
  };
  hw.msleep = [](void *c, unsigned ms) {
    static_cast<Board *>(c)->us += uint64_t(ms) * 1000;
    static_cast<Board *>(c)->tick();
  };
  hw.nowUs = [](void *c) { return static_cast<Board *>(c)->us; };
  hw.voteLock = [](void *, int) { return 0; };
  hw.pmuLock = [](void *c, int take) { return piano_wcn_pmu_lock(&static_cast<Board *>(c)->bt, take); };
  hw.railsGet = [](void *c) { return piano_wcn_rails_get(&static_cast<Board *>(c)->bt, PIANO_WCN_BT); };
  hw.railsPut = [](void *c, bool release) {
    Board *b = static_cast<Board *>(c);
    const unsigned left = piano_wcn_rails_put(&b->bt, PIANO_WCN_BT | (release ? unsigned(PIANO_WCN_RELEASE) : 0u));
    b->tick();
    return left;
  };
  hw.btEnInfo = [](void *c, BtEnInfo *out) {
    *out = static_cast<Board *>(c)->btenInfo;
    return out->apid < 0 ? -19 : 0;
  };
  hw.pmicRead = [](void *c, uint8_t reg, uint8_t *v, unsigned n) {
    Board *b = static_cast<Board *>(c);
    if (unsigned(reg) + n > sizeof b->bten) return -5;
    std::memcpy(v, b->bten + reg, n);
    return 0;
  };
  hw.pmicWrite = [](void *c, uint8_t reg, uint8_t v) {
    Board *b = static_cast<Board *>(c);
    if (reg != kPgMode && reg != kPgVin && reg != kPgPull && reg != kPgOutSrc && reg != kPgEn) ++b->pmicBadRegs;
    b->pmicWrites.push_back({reg, v});
    if (b->pmicDenied || b->btenInfo.owner) return -13;
    b->bten[reg] = v;
    b->tick();
    return 0;
  };
  hw.cancelled = [](void *c) {
    Board *b = static_cast<Board *>(c);
    return b->cancelAtToChip && b->toChip >= b->cancelAtToChip;
  };
  hw.step = [](void *c, int step, const char *) { static_cast<Board *>(c)->steps.push_back(step); };
  return hw;
}

static FwIo fwFor(Board &b)
{
  FwIo f;
  f.ctx = &b;
  f.size = [](void *c, const char *name, uint32_t *bytes) {
    auto &files = static_cast<Board *>(c)->chip.files;
    auto it = files.find(name);
    if (it == files.end()) return -4;
    *bytes = uint32_t(it->second.size());
    return 0;
  };
  f.read = [](void *c, const char *name, uint32_t off, uint8_t *to, uint32_t n) {
    auto &files = static_cast<Board *>(c)->chip.files;
    auto it = files.find(name);
    if (it == files.end() || size_t(off) + n > it->second.size()) return -5;
    std::memcpy(to, it->second.data() + off, n);
    return 0;
  };
  return f;
}

static const uint8_t kMadeUpAddress[6] = {0x02, 0x00, 0x00, 0x5e, 0x10, 0x01};   // a locally administered address, display order
static uint8_t gNvm[kNvmMaxBytes];
static Bringup gB;   // 3 KiB of port buffers, as the kext keeps it: not on the stack

static Options options(unsigned level)
{
  Options o;
  o.level = level;
  o.setup.address = kMadeUpAddress;
  return o;
}

// After a failure or a suspend: the engine stopped, the pins inputs with pull-downs, BT_EN low, the votes withdrawn, the rails put.
static void checkSafe(Board &b, uint32_t votesBefore = 0)
{
  CHECK(!(b.bten[kPgOutSrc] & kPgOutLevel) && !b.powered);
  CHECK(b.g(kGccQup2Vote) == votesBefore);
  for (unsigned i = kPinCts; i <= kPinRx; ++i)
    if (b.tlmm.count(kPins[i].gpio * kTlmmStride)) CHECK(b.tlmm[kPins[i].gpio * kTlmmStride] == pinConfig(-1, 0, kUartDriveMa, false));
  CHECK(!(b.railState & PIANO_WCN_BT) && b.pmuOwner == 0);
  CHECK(!gB.up);
}

// ------------------------------------------------------------------------------------------------------------ the tests
static void testSuccess(bool dfs)
{
  Board b;
  if (dfs) {   // the boot chain enabled DFS: level 0 7.3728 MHz, level 3 51.2 MHz, the rest TCXO
    const uint32_t r = kGccS6Rcg;
    b.gcc[r + kRcgDfsCmd] = 1;
    b.gcc[r + kRcgDfsPerf + 0] = (6u << 8) | (2u << 12) | 1u;
    b.gcc[r + kRcgDfsM + 0] = 384;
    b.gcc[r + kRcgDfsN + 0] = ~(15625u - 384u);
    b.gcc[r + kRcgDfsPerf + 12] = (6u << 8) | (2u << 12) | 1u;
    b.gcc[r + kRcgDfsM + 12] = 64;
    b.gcc[r + kRcgDfsN + 12] = ~(375u - 64u);
  }
  const Hw hw = hwFor(b);
  const FwIo fw = fwFor(b);
  Options o = options(4);
  CHECK(bringUp(gB, hw, o, fw, gNvm, sizeof gNvm) == kBtOk && gB.up && gB.error == 0 && gB.step == kStepLocalVersion);
  // Level 2: the rails voted by the helper, BT_EN high (only its level written: the stock configuration was there).
  CHECK(b.rails.allOn() && b.railState == (PIANO_WCN_BT | PIANO_WCN_UP | (0x3fu << PIANO_WCN_ADDED_SHIFT)) && b.pmuOwner == 0);
  CHECK(b.pmicWrites.size() == 1 && b.pmicWrites[0].first == kPgOutSrc && b.pmicWrites[0].second == kPgOutLevel && !b.pmicBadRegs);
  CHECK(gB.power.configured && !gB.power.wasHigh && gB.power.high && (gB.power.swCtrlIo & 1));
  // Level 3: the five votes, the engine at 115200 first, the loopback, the pins after it.
  CHECK(b.g(kGccQup2Vote) == kVoteBits && gB.uart.added == kVoteBits && b.unclocked == 0);
  CHECK(gB.uart.loopEchoed == 48 && gB.uart.loopMismatch == 0 && b.loopBytes == 48);
  CHECK(gB.uart.clock.sourceHz == 7372800 && gB.uart.clock.divider == 4 && gB.uart.dfs == dfs);
  CHECK(dfs ? b.rcgWrites == 0 : gB.uart.rcgProgrammed);
  CHECK(!b.chip.junk && b.garbage == 0 && b.lostBeforeReady == 0);   // nothing reached the controller unframed or too early
  for (unsigned i = kPinCts; i <= kPinRx; ++i) CHECK(b.pinIsEngine(kPins[i].gpio));
  CHECK(b.tlmm[26 * kTlmmStride] == pinConfig(1, 1, 2, false) && b.tlmm[24 * kTlmmStride] == pinConfig(-1, 1, 2, false));
  // Level 4: at 3.2 Mbit/s, the firmware sent whole, the address the unit's own and kept over the second reset.
  const SetupResult &r = gB.setup;
  CHECK(r.error == kSetupOk && r.baud == 3200000 && b.hostBaud() == 3200000 && b.chip.chipBaud == 3200000);
  CHECK(b.sourceHz() == 51200000 && decodeSerClk(b.reg(kSerMClkCfg)).divider == 1);
  CHECK(b.reg(kClkSel) == (dfs ? 3u : 6u));
  CHECK(b.chip.received[Sim::kPatch] == b.chip.files["gngbtfw20.mbn"] && !strcmp(r.nvm, "gngbtnv20.b2e"));
  CHECK(r.nvmInfo.baudAfter == 0x11 && r.nvmInfo.hciAfter == 0x03 && !strcmp(r.build, "BTFW1"));
  CHECK(r.reset0 == kSetupOk && b.chip.resets == 3 && r.local.manufacturer == kManufacturerQualcomm);
  CHECK(r.addressIsNvmDefault && r.addressSet && r.addressKept && b.chip.address[0] == 0x01 && b.chip.address[5] == 0x02);
  CHECK(b.chip.periFrames == 1 && b.chip.wakeInds == 1 && b.chip.junk == 0);
  CHECK(gB.port.txTimeouts == 0 && gB.port.rxBytes > 0);
  // The steps in order.
  const std::vector<int> want = {201, 202, 203, 204, 205, 301, 302, 303, 304, 305, 306, 400, 401, 402, 403, 404, 405, 406,
                                 407, 408, 409, 410, 411, 412};
  CHECK(b.steps == want);

  // System sleep: safeOff; the controller off; then the wake: the same ladder again from the start.
  safeOff(gB, hw, o);
  checkSafe(b);
  CHECK(!b.rails.allOn() && b.railState == 0);   // nobody else on the rails: this driver's enables withdrawn
  b.steps.clear();
  CHECK(bringUp(gB, hw, o, fw, gNvm, sizeof gNvm) == kBtOk && gB.up && b.chip.received[Sim::kPatch].size() == kStockRampatchBytes);
  CHECK(b.unclocked == 0 && b.chip.junk == 0);
}

static void testLevels()
{
  {   // level 2 alone: the rails and BT_EN, no clock and no SE6 access
    Board b;
    const Hw hw = hwFor(b);
    CHECK(bringUp(gB, hw, options(2), fwFor(b), gNvm, sizeof gNvm) == kBtOk && gB.up && b.powered);
    CHECK(b.gcc.find(kGccQup2Vote) == b.gcc.end() && b.se.empty() && b.tlmm.empty());
    safeOff(gB, hw, options(2));
    checkSafe(b);
  }
  {   // level 3 alone: the UART up, not a byte to the controller
    Board b;
    const Hw hw = hwFor(b);
    CHECK(bringUp(gB, hw, options(3), fwFor(b), gNvm, sizeof gNvm) == kBtOk && gB.up && b.toChip == 0 && b.chip.log.empty());
    CHECK(gB.uart.step == kStepPins && b.sActive && !b.loopback());
  }
  {   // level 3 without the self test
    Board b;
    Options o = options(3);
    o.loopback = false;
    CHECK(bringUp(gB, hwFor(b), o, fwFor(b), gNvm, sizeof gNvm) == kBtOk && b.loopBytes == 0);
  }
  {   // 115200 only: the RCG stays at 7.3728 MHz, no peripheral frame
    Board b;
    Options o = options(4);
    o.setup.operBaud = 115200;
    CHECK(bringUp(gB, hwFor(b), o, fwFor(b), gNvm, sizeof gNvm) == kBtOk && b.hostBaud() == 115200 && b.chip.periFrames == 0);
    CHECK(b.sourceHz() == 7372800);
  }
  {   // a stop request after the speed change: the state of that step stays
    Board b;
    Options o = options(4);
    o.setup.stopAfter = kStepBaud;
    CHECK(bringUp(gB, hwFor(b), o, fwFor(b), gNvm, sizeof gNvm) == kBtStopped && b.powered && b.hostBaud() == 3200000);
    CHECK(b.chip.segments[Sim::kPatch] == 0);
  }
  {   // a stop at 204: BT_EN high, the PMU lock given back
    Board b;
    Options o = options(4);
    o.stopAfter = kStepBtEnHigh;
    CHECK(bringUp(gB, hwFor(b), o, fwFor(b), gNvm, sizeof gNvm) == kBtStopped && b.powered && b.pmuOwner == 0);
  }
  {   // BT_EN found high: low for 100 ms first
    Board b;
    b.bten[kPgOutSrc] = kPgOutLevel;
    for (auto &[n, on] : b.rails.enabled) on = true;
    CHECK(bringUp(gB, hwFor(b), options(2), fwFor(b), gNvm, sizeof gNvm) == kBtOk && gB.power.wasHigh);
    CHECK(b.pmicWrites.size() == 2 && b.pmicWrites[0].second == 0 && b.pmicWrites[1].second == kPgOutLevel);
  }
  {   // BT_EN not configured as the DT says (an input): the five registers in PGPIO's order, each read back
    Board b;
    b.bten[kPgMode] = 0;
    b.bten[kPgPull] = 4;
    CHECK(bringUp(gB, hwFor(b), options(2), fwFor(b), gNvm, sizeof gNvm) == kBtOk && !gB.power.configured);
    const std::vector<std::pair<uint8_t, uint8_t>> want = {{kPgOutSrc, 0},       {kPgVin, kPgVin1},  {kPgPull, kPgPullNone},
                                                           {kPgMode, kPgModeOutput}, {kPgEn, kPgEnabled}, {kPgOutSrc, kPgOutLevel}};
    CHECK(b.pmicWrites == want && b.pmicBadRegs == 0 && b.powered);
  }
}

static void testWifiFirst()
{
  Board b;
  CHECK(piano_wcn_rails_get(&b.wifi, PIANO_WCN_WLAN) == PianoWcnOk && b.rails.txs == 6);
  const Hw hw = hwFor(b);
  const Options o = options(4);
  CHECK(bringUp(gB, hw, o, fwFor(b), gNvm, sizeof gNvm) == kBtOk && b.rails.txs == 6);   // a read-back, no vote
  CHECK(b.railState == (PIANO_WCN_WLAN | PIANO_WCN_BT | PIANO_WCN_UP | (0x3fu << PIANO_WCN_ADDED_SHIFT)));
  safeOff(gB, hw, o);   // Bluetooth sleeps: Wi-Fi still owns the rails, nothing withdrawn
  CHECK(b.rails.allOn() && b.railState == (PIANO_WCN_WLAN | PIANO_WCN_UP | (0x3fu << PIANO_WCN_ADDED_SHIFT)) && !b.powered);
  // Wi-Fi holding the PMU lock past the bound: level 2 stops before anything, nothing written.
  Board c;
  c.pmuOwner = PianoWcnTagWifi;
  c.bt.bound_ms = 20;
  CHECK(bringUp(gB, hwFor(c), options(4), fwFor(c), gNvm, sizeof gNvm) == kBtPmuLock && gB.power.step == -kStepRails);
  CHECK(c.rails.txs == 0 && c.pmicWrites.empty() && c.pmuOwner == PianoWcnTagWifi);
}

static void testFaults()
{
  struct Case {
    const char *what;
    void (*setup)(Board &);
    int error, step;
    bool pmicTouched;
  };
  static const Case cases[] = {
    {"BT_EN owned by EE 2", [](Board &b) { b.btenInfo.owner = 2; }, kBtBtEnOwner, -kStepBtEnCheck, false},
    {"BT_EN not found", [](Board &b) { b.btenInfo.apid = -1; }, kBtNoBtEn, -kStepBtEnCheck, false},
    {"BT_EN an older GPIO", [](Board &b) { b.btenInfo.subtype = 0x01; }, kBtBtEnKind, -kStepBtEnCheck, false},
    {"PMIC write denied", [](Board &b) { b.pmicDenied = true; }, kBtBtEnWrite, -kStepBtEnHigh, true},
    {"a rail vote refused", [](Board &b) { b.rails.refuse = true; }, kBtRails, -kStepRails, false},
    {"a branch never runs", [](Board &b) { b.stuckBranch = true; }, kBtBranch, -kStepClocks, true},
    {"SE6 without UART firmware", [](Board &b) { b.fwRevision = 0xff00; }, kBtProtocol, -kStepSeCheck, true},
    {"the RCG never updates", [](Board &b) { b.rcgStuck = true; }, kBtRcg, -kStepSource, true},
    {"the loopback corrupts", [](Board &b) { b.loopCorrupt = true; }, kBtLoopback, -kStepLoopback, true},
    {"FIFO interface off", [](Board &b) { b.se[kIfDisable] = kFifoIfDisable; }, kBtFifo, -kStepSeCheck, true},
  };
  for (const Case &c : cases) {
    Board b;
    c.setup(b);
    const Hw hw = hwFor(b);
    const int rc = bringUp(gB, hw, options(4), fwFor(b), gNvm, sizeof gNvm);
    const int step = gB.step;
    if (rc != c.error || step != c.step) std::printf("  case '%s': rc %d step %d\n", c.what, rc, step);
    CHECK(rc == c.error && step == c.step);
    CHECK(!b.pmicWrites.empty() == c.pmicTouched);
    CHECK(b.unclocked == 0 && b.toChip == 0 && b.pmicBadRegs == 0);
    checkSafe(b);
    if (!b.rails.refuse) CHECK(!b.rails.allOn());   // the helper's enables withdrawn (nobody else on the rails)
  }
  {   // the controller's RTS never asserts: the first command cannot leave (L4, step 400)
    Board b;
    b.rtsStuck = true;
    const int rc = bringUp(gB, hwFor(b), options(4), fwFor(b), gNvm, sizeof gNvm);
    CHECK(rc == kBtSetup && gB.setup.step == -kStepReset0 && gB.setup.error == kSetupTransport && gB.port.txTimeouts == 1);
    checkSafe(b);
  }
  {   // deaf at the new speed
    Board b;
    b.chip.deafAfterBaud = true;
    CHECK(bringUp(gB, hwFor(b), options(4), fwFor(b), gNvm, sizeof gNvm) == kBtSetup && gB.setup.error == kSetupBaud);
    CHECK(gB.setup.step == -kStepBaud);
    checkSafe(b);
  }
  {   // no rampatch on the partition
    Board b;
    b.chip.files.erase("gngbtfw20.mbn");
    CHECK(bringUp(gB, hwFor(b), options(4), fwFor(b), gNvm, sizeof gNvm) == kBtSetup && gB.setup.error == kSetupNoFile);
    checkSafe(b);
  }
  {   // a system sleep pending in the middle of the download: out at once, then safe
    Board b;
    b.cancelAtToChip = 20000;
    const Hw hw = hwFor(b);
    CHECK(bringUp(gB, hw, options(4), fwFor(b), gNvm, sizeof gNvm) == kBtSetup && gB.setup.step == -kStepRampatch);
    CHECK(b.toChip < 20000 + 300 && b.chip.segments[Sim::kPatch] < 957);
    checkSafe(b);
  }
  {   // no private address: the NVM's default stays and is said to be the default
    Board b;
    Options o = options(4);
    o.setup.address = nullptr;
    CHECK(bringUp(gB, hwFor(b), o, fwFor(b), gNvm, sizeof gNvm) == kBtOk && gB.setup.addressIsNvmDefault && !gB.setup.addressSet);
  }
  {   // votes another driver holds stay when this one withdraws its own
    Board b;
    b.gcc[kGccQup2Vote] = kVoteCore | kVoteSAhb | (1u << 17);
    b.stuckBranch = false;
    b.fwRevision = 0xff00;
    CHECK(bringUp(gB, hwFor(b), options(4), fwFor(b), gNvm, sizeof gNvm) == kBtProtocol);
    CHECK(b.g(kGccQup2Vote) == (kVoteCore | kVoteSAhb | (1u << 17)));
  }
}

// The RSC transaction against a TCS model: an idle TCS taken (2 first), the commands, the trigger, completion, the TCS back to idle;
// a TCS that never completes is left and skipped next time.
struct RscModel {
  std::map<uint32_t, uint32_t> regs;
  RscLayout l = pianokeyboard::rscLayout(0x00030000);
  bool complete = true;
  unsigned triggers = 0;
  uint32_t lastAddr = 0, lastData = 0;
};
static void testRsc()
{
  RscModel m;
  for (unsigned t = 0; t < 3; ++t) m.regs[m.l.tcsStride * t + m.l.status] = 1;   // idle
  RscIo io;
  io.ctx = &m;
  io.rd = [](void *c, uint32_t off) {
    RscModel *x = static_cast<RscModel *>(c);
    auto it = x->regs.find(off);
    return it == x->regs.end() ? 0u : it->second;
  };
  io.wr = [](void *c, uint32_t off, uint32_t v) {
    RscModel *x = static_cast<RscModel *>(c);
    x->regs[off] = v;
    for (unsigned t = 0; t < 3; ++t) {
      const uint32_t at = x->l.tcsStride * t;
      if (off == at + x->l.control && v == (kAmcEnable | kAmcTrigger)) {
        ++x->triggers;
        x->lastAddr = x->regs[at + x->l.cmdAddr];
        x->lastData = x->regs[at + x->l.cmdData];
        if (x->complete) x->regs[x->l.irqStatus] |= 1u << t;
      }
      if (off == x->l.irqClear) x->regs[x->l.irqStatus] &= ~v;
    }
  };
  io.sleepMs = [](void *, unsigned) {};
  Rsc rsc;
  rsc.layout = m.l;
  const PianoWcnVrmCmd cmds[3] = {{PianoWcnVrmMode, 7}, {PianoWcnVrmVoltage, 1800}, {PianoWcnVrmEnable, 1}};
  VrmResult res;
  CHECK(vrmSend(rsc, io, 0x30100, cmds, 3, res) == 0 && res.tcs == 2 && res.done && m.triggers == 1);
  const uint32_t at = m.l.tcsStride * 2;
  CHECK(m.regs[at + m.l.cmdAddr] == 0x30108 && m.regs[at + m.l.cmdStride * 2 + m.l.cmdAddr] == 0x30104);
  CHECK(m.regs[at + m.l.cmdStride + m.l.cmdData] == 1800 && m.regs[at + m.l.waitForCmpl] == 7 && m.regs[at + m.l.control] == 0);
  CHECK(m.regs[at + m.l.cmdEnable] == 0 && !(m.regs[m.l.irqStatus] & 4u));
  m.complete = false;
  CHECK(vrmSend(rsc, io, 0x30100, cmds, 1, res) == -60 && res.tcs == 2 && rsc.stuck[2] && res.waited == kTcsPollMs);
  m.complete = true;
  CHECK(vrmSend(rsc, io, 0x30100, cmds, 1, res) == 0 && res.tcs == 1);   // the stuck one is skipped
  m.regs[m.l.tcsStride * 1 + m.l.status] = 0;
  m.regs[m.l.tcsStride * 0 + m.l.status] = 0;
  CHECK(vrmSend(rsc, io, 0x30100, cmds, 1, res) == -16);
  CHECK(vrmSend(rsc, io, 0, cmds, 1, res) == -22 && vrmSend(rsc, io, 0x30100, cmds, 4, res) == -22);
}

int main()
{
  testSuccess(false);
  testSuccess(true);
  testLevels();
  testWifiFirst();
  testFaults();
  testRsc();
  if (failures) {
    std::printf("bt_bringup_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("bt_bringup_host: all tests passed\n");
  return 0;
}
