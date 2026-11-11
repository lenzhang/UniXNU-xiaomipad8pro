// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoBluetooth levels 2 to 4 as portable sequences: what is written, in which order, with which read-backs, waits and timeouts, and
// the polled GENI UART that carries level 4's H4 traffic. Every hardware access goes through the Hw callbacks (a window and an offset,
// the shared rails, BT_EN's PMIC registers), so the host tests (tests/bt_bringup_host.cpp) run the whole ladder against register
// models wired to a simulated controller and can make any step fail. Nothing here maps memory, sleeps by itself or reaches an address
// outside the windows the owner mapped. docs/drivers/bluetooth.md section 8 is the reference.
//
// Level 2 (power), steps 201..205: the PMU lock; the shared rails (PianoWcnRails.h get, PIANO_WCN_BT); BT_EN's APID and owner (EE 0,
// else stop: the apps processor cannot write it), its kind (an LV/MV PMIC GPIO) and registers; the DT's configuration when it is not
// already there (BtHw.h: VIN, pull, mode, enable, level low first); low for 100 ms when it was found high; high, read back; the lock
// given back; the PMU's 50 ms; SW_CTRL read.
// Level 3 (UART), steps 301..306: the QUP2 votes (the five bits of 0x52010 under PianoA830's GCC vote lock, only ever added) and the
// five branches running; SE6's protocol (UART), FIFO interface and FIFO depths; the SE6 clock source (a DFS level when the boot chain
// enabled DFS, else the RCG programmed to its table entry for 115200 x 16); the engine's set-up at 115200 8N1 with hardware flow
// control (GeniUart.h planUart), polled, FIFO mode, and its receiver started; with the pins still not the engine's, the internal
// loopback (TX to RX, RFR to CTS) echoing a pattern; loopback off; then the four pins to qup2_se6 with the DT's pulls and 2 mA; the
// line state (IOS) read; whatever the controller sent meanwhile drained.
// Level 4 (controller), steps 400..412: QcaCodec.h's qcaSetup over this UART (send, drain, events, speed, flow control) and the
// firmware reader the owner gives.
//
// The rules every step keeps:
//   - the rails come before BT_EN rises, and both happen with the PMU lock held; this driver never touches WLAN_EN;
//   - a GCC vote bit is only ever added, and safeOff withdraws only the bits this driver added; the RCG is the SE6 clock's own;
//   - nothing is read or written in SE6 or the QUP2 wrapper before the five branches run;
//   - every PMIC write is one of BT_EN's five registers (BtHw.h), each read back; only on an APID owned by EE 0;
//   - every poll is bounded and its timeout an error; a failure stops the ladder there and leaves the hardware safe (safeOff: the
//     engine stopped, the pins back to inputs with pull-downs, BT_EN low, this driver's clock votes withdrawn, the rails put);
//   - a pending sleep or stop (Hw::cancelled) ends any poll at once with kBtCancelled.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "BtHw.h"
#include "GeniUart.h"
#include "HciH4.h"
#include "QcaCodec.h"

namespace pianobt {

// The windows the owner maps for levels 2 and up.
enum Win : unsigned { kWinGcc, kWinTlmm, kWinSe, kWinQup, kWinCount };
constexpr uint64_t kWinBase[kWinCount] = {kGcc, kTlmm, kSe6, kQup2};
constexpr uint64_t kWinBytes[kWinCount] = {kGccBytes, kTlmmBytes, kSe6Bytes, kQup2Bytes};

enum BringupStep : int {
  kStepRails = 201, kStepBtEnCheck = 202, kStepBtEnConfig = 203, kStepBtEnHigh = 204, kStepPowered = 205,
  kStepClocks = 301, kStepSeCheck = 302, kStepSource = 303, kStepSeInit = 304, kStepLoopback = 305, kStepPins = 306,
};
enum BringupError : int {
  kBtOk = 0, kBtStopped = -1, kBtArgument = -2, kBtRails = -3, kBtNoBtEn = -4, kBtBtEnOwner = -5, kBtBtEnKind = -6,
  kBtBtEnWrite = -7, kBtBranch = -8, kBtProtocol = -9, kBtFifo = -10, kBtRcg = -11, kBtClock = -12, kBtLoopback = -13,
  kBtCancelled = -14, kBtTx = -15, kBtSetup = -16, kBtPmuLock = -17,
};
inline const char *bringupErrorName(int e)
{
  switch (e) {
  case kBtOk: return "none";
  case kBtStopped: return "stopped here on request";
  case kBtArgument: return "bad arguments";
  case kBtRails: return "the shared rails did not come up";
  case kBtNoBtEn: return "BT_EN's PMIC peripheral was not found or not read";
  case kBtBtEnOwner: return "BT_EN's SPMI peripheral is not owned by the apps processor (EE 0)";
  case kBtBtEnKind: return "BT_EN is not an LV/MV PMIC GPIO";
  case kBtBtEnWrite: return "a BT_EN write failed or did not read back";
  case kBtBranch: return "a QUP2 / SE6 branch clock did not start";
  case kBtProtocol: return "SE6 does not carry the UART protocol";
  case kBtFifo: return "SE6's FIFO interface is off or its FIFOs are implausible";
  case kBtRcg: return "the SE6 clock source did not take its configuration";
  case kBtClock: return "no serial clock for the baud rate";
  case kBtLoopback: return "the loopback self test did not echo";
  case kBtCancelled: return "a system sleep or stop is pending";
  case kBtTx: return "the UART did not send (CTS never asserted?)";
  case kBtSetup: return "the controller set-up failed";
  case kBtPmuLock: return "the PMU lock stayed held by the other radio";
  default: return "?";
  }
}

struct BtEnInfo {
  int apid = -1;
  uint32_t owner = 0;
  uint8_t type = 0, subtype = 0;
};

struct Hw {
  void *ctx = nullptr;
  uint32_t (*read)(void *ctx, unsigned win, uint32_t off) = nullptr;
  // Writes and completes with a barrier; `what` names it for the log (the owner logs a kind once and counts repeats).
  void (*write)(void *ctx, unsigned win, uint32_t off, uint32_t value, const char *what) = nullptr;
  void (*udelay)(void *ctx, unsigned us) = nullptr;
  void (*msleep)(void *ctx, unsigned ms) = nullptr;
  uint64_t (*nowUs)(void *ctx) = nullptr;
  int (*voteLock)(void *ctx, int take) = nullptr;     // PianoA830's sm8750_gcc_vote_owner around 0x52010. 0: taken (or given)
  int (*pmuLock)(void *ctx, int take) = nullptr;      // PianoWcnRails.h piano_wcn_pmu_lock. 0, or negative (busy)
  int (*railsGet)(void *ctx) = nullptr;               // piano_wcn_rails_get(PIANO_WCN_BT). 0, or negative
  unsigned (*railsPut)(void *ctx, bool release) = nullptr;   // piano_wcn_rails_put(PIANO_WCN_BT [| PIANO_WCN_RELEASE]): owners left
  int (*btEnInfo)(void *ctx, BtEnInfo *out) = nullptr;       // BT_EN's APID, owner, TYPE/SUBTYPE. 0, or negative
  int (*pmicRead)(void *ctx, uint8_t reg, uint8_t *value, unsigned bytes) = nullptr;   // observer channel. 0, or negative
  int (*pmicWrite)(void *ctx, uint8_t reg, uint8_t value) = nullptr;   // the owner channel (EE 0 only). 0, or negative
  bool (*cancelled)(void *ctx) = nullptr;             // optional
  void (*step)(void *ctx, int step, const char *what) = nullptr;   // optional: one line as each step starts
};

// The firmware files (level 4): the owner reads them from the tablet's own bluetooth_a.
struct FwIo {
  void *ctx = nullptr;
  int (*size)(void *ctx, const char *name, uint32_t *bytes) = nullptr;
  int (*read)(void *ctx, const char *name, uint32_t offset, uint8_t *to, uint32_t n) = nullptr;
};

struct Options {
  unsigned level = 0;                  // piano-bt: 2 power, 3 + UART, 4 + controller
  bool loopback = true;                // piano-bt-loop=0: no self test
  bool release = true;                 // the rails put with PIANO_WCN_RELEASE when this driver gives them up
  unsigned btEnLowMs = kBtEnLowMs, pmuMs = kPmuPowerUpMs;
  int stopAfter = 0;                   // piano-bt-stop=N: return after step N (2xx/3xx here, 4xx in setup)
  int32_t maxClockErrorPpm = 25000;    // a serial clock further than 2.5 % from the baud rate is refused
  SetupOptions setup;                  // level 4
};

struct PowerState {
  int railsRc = 0;
  bool railsHeld = false;
  BtEnInfo bten;
  uint8_t regs[7] = {}, after[7] = {};   // 0x40..0x46 before level 2 and at its end
  bool configured = false, wasHigh = false, touched = false, high = false;
  unsigned pmicWrites = 0;
  uint8_t failedReg = 0, failedWant = 0, failedRead = 0;
  uint32_t swCtrlIo = 0, wlanEnIo = 0;
  int step = 0, error = 0;
};

struct UartState {
  uint32_t voteBefore = 0, voteAfter = 0, added = 0;   // `added`: the vote bits this driver set
  uint32_t cbcr[kBranchCount] = {};
  unsigned branchPolls = 0;
  bool clocked = false, engineUp = false, pinsMuxed = false;
  uint32_t qupVersion = 0, fwRevision = 0, ifDisable = 0, hwParam0 = 0, hwParam1 = 0;
  Fifo tx{}, rx{};
  bool dfs = false;
  uint32_t dfsCmd = 0, rcgCfgBefore = 0, rcgCfgAfter = 0, rcgHzBefore = 0, rcgHz = 0;
  unsigned rcgPolls = 0;
  bool rcgProgrammed = false;
  ClockPlan clock{};
  unsigned planWrites = 0, loopBytes = 0, loopEchoed = 0, loopMismatch = 0;
  uint32_t ios = 0;
  uint32_t pinCfg[4] = {}, pinIo[4] = {};
  unsigned drained = 0;
  int step = 0, error = 0;
};

// ------------------------------------------------------------------------------------------------------------ small helpers
// The TLMM control word of a pin state: pull (-1 down, 0 none, 1 up), function, drive in mA (2..16), output enable (PianoWifi's).
inline uint32_t pinConfig(int pull, unsigned function, unsigned driveMa, bool output)
{
  const uint32_t pullBits = pull < 0 ? 1u : pull > 0 ? 3u : 0u;
  const uint32_t drive = driveMa >= 2 && driveMa <= 16 ? driveMa / 2 - 1 : 0u;
  return pullBits | (function & 0xfu) << 2 | (drive & 7u) << 6 | (output ? 1u << 9 : 0u);
}
inline bool hwUsable(const Hw &hw) { return hw.read && hw.write && hw.udelay && hw.msleep && hw.nowUs; }
inline bool cancelled(const Hw &hw) { return hw.cancelled && hw.cancelled(hw.ctx); }
inline void announce(const Hw &hw, int step, const char *what)
{
  if (hw.step) hw.step(hw.ctx, step, what);
}

// ================================================================================================== the polled GENI UART
// FIFO mode, interrupts off at SE_IRQ_EN (the status bits latch and are polled). TX: TX_TRANS_LEN, the START_TX command, the FIFO
// filled as it has room (TX_FIFO_STATUS), M_CMD_DONE. RX: the START_READ command keeps the secondary sequencer receiving; the RX FIFO
// is popped by its status (word count, the last word's valid bytes); with hardware flow control the engine drops RFR before the
// FIFO fills, so a slow poll loses nothing. HCI events go through an H4 decoder into a small queue for qcaSetup.
struct Port {
  const Hw *hw = nullptr;
  uint32_t qupVersion = 0, txDepth = 0, rxDepth = 0;
  bool dfs = false;
  uint32_t levels[16] = {};
  unsigned levelCount = 0;
  uint32_t rcgHz = 0;                  // DFS off: what the RCG gives now
  uint32_t baud = 0;
  bool flow = true, rxOn = false;
  int32_t maxErrorPpm = 25000;
  int lastError = 0;
  uint64_t txBytes = 0, rxBytes = 0;
  unsigned txTimeouts = 0, cancels = 0, aborts = 0, droppedPackets = 0, dropped = 0, ibs = 0;
  H4Decoder dec;
  static constexpr unsigned kQueue = 4;
  uint8_t ev[kQueue][2 + 255] = {};
  uint16_t evLen[kQueue] = {};
  unsigned evHead = 0, evCount = 0;
  uint8_t *raw = nullptr;              // the loopback test's capture: raw bytes instead of the decoder
  size_t rawCap = 0, rawHave = 0;
};

namespace detail {
inline uint32_t seRd(const Port &p, uint32_t off) { return p.hw->read(p.hw->ctx, kWinSe, off); }
inline void seWr(const Port &p, uint32_t off, uint32_t v, const char *what) { p.hw->write(p.hw->ctx, kWinSe, off, v, what); }
inline uint32_t gccRd(const Hw &hw, uint32_t off) { return hw.read(hw.ctx, kWinGcc, off); }
// Polls `off` for `mask` set (or clear) every 1 us, at most `polls` times.
inline bool sePoll(const Port &p, uint32_t off, uint32_t mask, bool set, unsigned polls)
{
  for (unsigned i = 0; i < polls; ++i) {
    if (((seRd(p, off) & mask) != 0) == set) return true;
    p.hw->udelay(p.hw->ctx, 1);
  }
  return ((seRd(p, off) & mask) != 0) == set;
}
}  // namespace detail

// Hands each decoded event to the queue (oldest dropped when full), counts IBS bytes and other packets.
inline void portFeed(Port &p, const uint8_t *bytes, size_t n)
{
  if (p.raw) {
    for (size_t i = 0; i < n && p.rawHave < p.rawCap; ++i) p.raw[p.rawHave++] = bytes[i];
    return;
  }
  p.dec.feed(bytes, n, [&p](uint8_t type, const uint8_t *pkt, size_t len) {
    if (!pkt) {
      ++p.ibs;
      return;
    }
    if (type != kH4Event || len > sizeof p.ev[0]) {
      ++p.droppedPackets;
      return;
    }
    if (p.evCount == Port::kQueue) {
      p.evHead = (p.evHead + 1) % Port::kQueue;
      --p.evCount;
      ++p.droppedPackets;
    }
    const unsigned at = (p.evHead + p.evCount) % Port::kQueue;
    for (size_t i = 0; i < len; ++i) p.ev[at][i] = pkt[i];
    p.evLen[at] = uint16_t(len);
    ++p.evCount;
  });
}

// Pops what the RX FIFO holds. Returns the bytes taken.
inline size_t portPoll(Port &p)
{
  using namespace detail;
  const RxStatus rs = decodeRxStatus(seRd(p, kRxFifoStatus));
  if (!rs.words) return 0;
  size_t total = 0;
  for (uint32_t w = 0; w < rs.words; ++w) {
    uint8_t b[4];
    const size_t n = unpackWord(seRd(p, kRxFifo), b, (w + 1 == rs.words && rs.last) ? rs.lastBytes : 4);
    portFeed(p, b, n);
    total += n;
  }
  const uint32_t st = seRd(p, kSIrqStatus) & (kSRxFifoWatermark | kSRxFifoLast);
  if (st) seWr(p, kSIrqClear, st, "clear the RX FIFO status bits");
  p.rxBytes += total;
  return total;
}

inline void portStartRx(Port &p)
{
  using namespace detail;
  seWr(p, kSIrqClear, 0xffffffffu, "clear the secondary sequencer's status");
  seWr(p, kSCmd0, kUartStartRead << kOpcodeShift, "secondary sequencer: START_READ");
  p.rxOn = true;
}

// Cancel, else abort, the receiver (qcom_geni_serial's stop_rx order); what the FIFO still held goes to the decoder first.
inline void portStopRx(Port &p)
{
  using namespace detail;
  (void)portPoll(p);
  if (seRd(p, kGeniStatus) & kSCmdActive) {
    seWr(p, kSCmdCtrl, kCmdCancel, "secondary sequencer: cancel");
    ++p.cancels;
    const bool cancelledOk = sePoll(p, kSIrqStatus, kIrqCmdCancel, true, 1000);
    (void)portPoll(p);
    seWr(p, kSIrqClear, seRd(p, kSIrqStatus), "clear the secondary sequencer's status");
    if (!cancelledOk || (seRd(p, kGeniStatus) & kSCmdActive)) {
      seWr(p, kSCmdCtrl, kCmdAbort, "secondary sequencer: abort");
      ++p.aborts;
      (void)sePoll(p, kSCmdCtrl, kCmdAbort, false, 1000);
      seWr(p, kSIrqClear, 0xffffffffu, "clear the secondary sequencer's status");
      seWr(p, kForceDefault, 1, "force the IO defaults");
    }
  }
  p.rxOn = false;
}

// Cancel, else abort, a TX command still running (cancel_tx_cmd's order).
inline void portStopTx(Port &p)
{
  using namespace detail;
  if (!(seRd(p, kGeniStatus) & kMCmdActive)) return;
  seWr(p, kMCmdCtrl, kCmdCancel, "main sequencer: cancel");
  ++p.cancels;
  if (!sePoll(p, kMIrqStatus, kIrqCmdCancel, true, 1000)) {
    seWr(p, kMCmdCtrl, kCmdAbort, "main sequencer: abort");
    ++p.aborts;
    (void)sePoll(p, kMIrqStatus, kIrqCmdAbort, true, 1000);
    seWr(p, kMIrqClear, kIrqCmdAbort, "clear the main sequencer's abort");
  }
  seWr(p, kMIrqClear, kIrqCmdCancel | kMCmdDone, "clear the main sequencer's cancel");
}

// Sends n bytes as one TX command. 0, kBtTx (the deadline passed: the command cancelled) or kBtCancelled.
inline int portSend(Port &p, const uint8_t *b, size_t n, unsigned timeoutMs)
{
  using namespace detail;
  if (!n) return kBtOk;
  if (!b || n > 0xffffff) return kBtArgument;
  const Hw &hw = *p.hw;
  if (cancelled(hw)) return kBtCancelled;   // checked before every command too: an ELF download waits only at its end
  if (seRd(p, kGeniStatus) & kMCmdActive) portStopTx(p);
  seWr(p, kMIrqClear, kMCmdDone | kMTxFifoWatermark, "clear the main sequencer's done");
  seWr(p, kUartTxTransLen, uint32_t(n), "UART TX length");
  seWr(p, kMCmd0, kUartStartTx << kOpcodeShift, "main sequencer: START_TX");
  const uint64_t deadline = hw.nowUs(hw.ctx) + uint64_t(timeoutMs) * 1000;
  size_t i = 0;
  while (i < n) {
    if ((seRd(p, kTxFifoStatus) & kTxWordsMask) < p.txDepth) {
      const size_t take = n - i < 4 ? n - i : 4;
      seWr(p, kTxFifo, packWord(b + i, take), "TX FIFO word");
      i += take;
      continue;
    }
    if (cancelled(hw)) {
      portStopTx(p);
      return kBtCancelled;
    }
    if (hw.nowUs(hw.ctx) >= deadline) {
      ++p.txTimeouts;
      portStopTx(p);
      return kBtTx;
    }
    if (p.rxOn) (void)portPoll(p);
    hw.udelay(hw.ctx, 5);
  }
  while (!(seRd(p, kMIrqStatus) & kMCmdDone)) {
    if (cancelled(hw)) {
      portStopTx(p);
      return kBtCancelled;
    }
    if (hw.nowUs(hw.ctx) >= deadline) {
      ++p.txTimeouts;
      portStopTx(p);
      return kBtTx;
    }
    if (p.rxOn) (void)portPoll(p);
    hw.udelay(hw.ctx, 5);
  }
  seWr(p, kMIrqClear, kMCmdDone | kMTxFifoWatermark, "clear the main sequencer's done");
  p.txBytes += n;
  return kBtOk;
}

// The next HCI event: its length, 0 at the timeout, negative when cancelled.
inline int portEvent(Port &p, uint8_t *out, size_t cap, unsigned timeoutMs)
{
  const Hw &hw = *p.hw;
  const uint64_t deadline = hw.nowUs(hw.ctx) + uint64_t(timeoutMs) * 1000;
  for (;;) {
    if (p.evCount) {
      const unsigned at = p.evHead;
      p.evHead = (p.evHead + 1) % Port::kQueue;
      --p.evCount;
      const size_t n = p.evLen[at];
      if (n > cap) return -1;
      for (size_t i = 0; i < n; ++i) out[i] = p.ev[at][i];
      return int(n);
    }
    if (cancelled(hw)) return kBtCancelled;
    if (portPoll(p)) continue;
    if (hw.nowUs(hw.ctx) >= deadline) return 0;
    hw.udelay(hw.ctx, 20);
  }
}

// Every byte sent has left: no TX command running and the TX FIFO empty. 0, kBtTx or kBtCancelled.
inline int portDrain(Port &p, unsigned timeoutMs)
{
  using namespace detail;
  const Hw &hw = *p.hw;
  const uint64_t deadline = hw.nowUs(hw.ctx) + uint64_t(timeoutMs) * 1000;
  while ((seRd(p, kGeniStatus) & kMCmdActive) || (seRd(p, kTxFifoStatus) & kTxWordsMask)) {
    if (cancelled(hw)) return kBtCancelled;
    if (hw.nowUs(hw.ctx) >= deadline) return kBtTx;
    hw.udelay(hw.ctx, 10);
  }
  return kBtOk;
}

// The SE6 RCG to one of its table's rates (DFS off): M, N, D, CFG, then the update bit, at most 500 us for it to clear.
inline int rcgProgram(const Hw &hw, uint32_t hz, uint32_t *cfgAfter, unsigned *polls)
{
  const uint32_t r = kGccS6Rcg;
  const RcgPlan plan = planRcg(hz, detail::gccRd(hw, r + kRcgCfg), detail::gccRd(hw, r + kRcgM), detail::gccRd(hw, r + kRcgN),
                               detail::gccRd(hw, r + kRcgD));
  if (!plan.ok) return kBtClock;
  if (plan.mnd) {
    hw.write(hw.ctx, kWinGcc, r + kRcgM, plan.m, "SE6 clock source: M");
    hw.write(hw.ctx, kWinGcc, r + kRcgN, plan.n, "SE6 clock source: N");
    hw.write(hw.ctx, kWinGcc, r + kRcgD, plan.d, "SE6 clock source: D");
  }
  hw.write(hw.ctx, kWinGcc, r + kRcgCfg, plan.cfg, "SE6 clock source: CFG");
  hw.write(hw.ctx, kWinGcc, r, detail::gccRd(hw, r) | kRcgCmdUpdate, "SE6 clock source: update");
  unsigned n = 0;
  for (; n < kRcgUpdatePolls && (detail::gccRd(hw, r) & kRcgCmdUpdate); ++n) hw.udelay(hw.ctx, 1);
  if (polls) *polls = n;
  if (cfgAfter) *cfgAfter = detail::gccRd(hw, r + kRcgCfg);
  if (detail::gccRd(hw, r) & kRcgCmdUpdate) return kBtRcg;
  return rcgRate(detail::gccRd(hw, r + kRcgCfg), detail::gccRd(hw, r + kRcgM), detail::gccRd(hw, r + kRcgN)) == hz ? kBtOk : kBtRcg;
}

// The line to another speed: the receiver stopped, the source (DFS: a level; else the RCG), both serial clocks and the clock
// select (Linux's index, also with DFS off), the receiver started again.
inline int portSetBaud(Port &p, uint32_t baud)
{
  using namespace detail;
  const ClockPlan c = planClock(baud, p.qupVersion, p.levels, p.levelCount);
  if (!c.ok || c.errorPpm > p.maxErrorPpm || c.errorPpm < -p.maxErrorPpm) return p.lastError = kBtClock;
  if (portDrain(p, 100)) return p.lastError = kBtTx;
  portStopRx(p);
  if (!p.dfs && c.sourceHz != p.rcgHz) {
    const int rc = rcgProgram(*p.hw, c.sourceHz, nullptr, nullptr);
    if (rc) {
      portStartRx(p);
      return p.lastError = rc;
    }
    p.rcgHz = c.sourceHz;
  }
  seWr(p, kSerMClkCfg, c.serClkCfg, "serial clock (main)");
  seWr(p, kSerSClkCfg, c.serClkCfg, "serial clock (secondary)");
  seWr(p, kClkSel, c.level & 7u, "the source level");
  p.baud = c.baud;
  portStartRx(p);
  return kBtOk;
}

// Flow control on (CTS honoured, RFR by the engine) or off (CTS ignored, RFR held not ready): the receiver stopped around it, as
// set_termios does.
inline int portSetFlow(Port &p, bool on)
{
  using namespace detail;
  if (portDrain(p, 100)) return p.lastError = kBtTx;
  const bool rx = p.rxOn;
  if (rx) portStopRx(p);
  seWr(p, kUartTxTransCfg, on ? 0u : kUartCtsMask, on ? "TX: CTS honoured" : "TX: CTS ignored");
  seWr(p, kUartManualRfr, on ? 0u : (kRfrManual | kRfrNotReady), on ? "RFR by the engine" : "RFR held not ready");
  p.flow = on;
  if (rx) portStartRx(p);
  return kBtOk;
}

// qcaSetup's view of the port.
struct PortSetup {
  Port *port;
  const FwIo *fw;
};
inline SetupIo portSetupIo(PortSetup &ps)
{
  SetupIo io;
  io.ctx = &ps;
  io.send = [](void *c, const uint8_t *b, size_t n) {
    PortSetup *s = static_cast<PortSetup *>(c);
    return portSend(*s->port, b, n, 2000) ? -1 : 0;
  };
  io.drain = [](void *c, unsigned ms) { return portDrain(*static_cast<PortSetup *>(c)->port, ms) ? -1 : 0; };
  io.event = [](void *c, uint8_t *ev, size_t cap, unsigned ms) {
    const int n = portEvent(*static_cast<PortSetup *>(c)->port, ev, cap, ms);
    return n < 0 ? -1 : n;
  };
  io.hostBaud = [](void *c, uint32_t baud) { return portSetBaud(*static_cast<PortSetup *>(c)->port, baud) ? -1 : 0; };
  io.hostFlow = [](void *c, bool on) { return portSetFlow(*static_cast<PortSetup *>(c)->port, on) ? -1 : 0; };
  io.sleepMs = [](void *c, unsigned ms) {
    const Hw &hw = *static_cast<PortSetup *>(c)->port->hw;
    hw.msleep(hw.ctx, ms);
  };
  io.fwSize = [](void *c, const char *name, uint32_t *bytes) {
    const FwIo *f = static_cast<PortSetup *>(c)->fw;
    return f && f->size ? f->size(f->ctx, name, bytes) : -1;
  };
  io.fwRead = [](void *c, const char *name, uint32_t off, uint8_t *to, uint32_t n) {
    const FwIo *f = static_cast<PortSetup *>(c)->fw;
    return f && f->read ? f->read(f->ctx, name, off, to, n) : -1;
  };
  io.step = [](void *c, int step, const char *what) { announce(*static_cast<PortSetup *>(c)->port->hw, step, what); };
  return io;
}

// ================================================================================================== the state of it all
struct Bringup {
  Port port;
  PowerState power;
  UartState uart;
  SetupResult setup;
  int step = 0, error = 0;
  bool up = false;                       // the ladder reached the asked level
};

// ============================================================================================================ safe off
// From any state: the engine stopped (its commands cancelled, loopback off, interrupt outputs off) and the pins back to GPIO inputs
// with pull-downs (no pad drives an unpowered chip), BT_EN low (only if this driver touched it), the vote bits this driver added
// withdrawn (the SE first, its clocks after), the rails put (released when this driver may: Options::release and no other owner).
// Used on every failure, for system sleep and at shutdown.
inline void safeOff(Bringup &b, const Hw &hw, const Options &o)
{
  if (!hwUsable(hw)) return;
  Port &p = b.port;
  UartState &u = b.uart;
  PowerState &s = b.power;
  if (u.engineUp && u.clocked) {
    p.hw = &hw;
    portStopTx(p);
    portStopRx(p);
    hw.write(hw.ctx, kWinSe, kUartLoopback, 0, "no loopback");
    hw.write(hw.ctx, kWinSe, kSeIrqEn, 0, "interrupt outputs off (polled)");
    u.engineUp = false;
  }
  if (u.pinsMuxed) {
    for (unsigned i = kPinCts; i <= kPinRx; ++i)
      hw.write(hw.ctx, kWinTlmm, kPins[i].gpio * kTlmmStride, pinConfig(-1, 0, kUartDriveMa, false), "UART pin: GPIO input, pull-down (off)");
    u.pinsMuxed = false;
  }
  if (s.touched && hw.pmicWrite) {
    uint8_t now = 0;
    if (!hw.pmicRead || hw.pmicRead(hw.ctx, kPgOutSrc, &now, 1) || (now & kPgOutLevel)) hw.pmicWrite(hw.ctx, kPgOutSrc, 0);
    s.high = false;
  }
  if (u.added) {
    const int locked = hw.voteLock ? hw.voteLock(hw.ctx, 1) : 0;
    const uint32_t v = hw.read(hw.ctx, kWinGcc, kGccQup2Vote);
    hw.write(hw.ctx, kWinGcc, kGccQup2Vote, v & ~u.added, "GCC QUP2 votes: withdraw this driver's");
    if (hw.voteLock && !locked) hw.voteLock(hw.ctx, 0);
    u.added = 0;
    u.clocked = false;
  }
  if (s.railsHeld && hw.railsPut) {
    hw.railsPut(hw.ctx, o.release);
    s.railsHeld = false;
  }
  b.up = false;
}

// ================================================================================================================ level 2
inline int powerOn(Bringup &b, const Hw &hw, const Options &o)
{
  PowerState &s = b.power;
  s = PowerState();
  auto failed = [&](int step, int error, bool locked) {
    s.step = -step;
    s.error = error;
    if (locked && hw.pmuLock) hw.pmuLock(hw.ctx, 0);
    safeOff(b, hw, o);
    return error;
  };
  if (!hwUsable(hw) || !hw.pmuLock || !hw.railsGet || !hw.railsPut || !hw.btEnInfo || !hw.pmicRead || !hw.pmicWrite)
    return s.step = -kStepRails, s.error = kBtArgument;

  announce(hw, kStepRails, "the PMU lock and the shared rails (PianoWcnRails.h)");
  if (hw.pmuLock(hw.ctx, 1)) return s.step = -kStepRails, s.error = kBtPmuLock;
  s.railsRc = hw.railsGet(hw.ctx);
  if (s.railsRc) return failed(kStepRails, kBtRails, true);
  s.railsHeld = true;
  s.step = kStepRails;
  if (o.stopAfter == kStepRails) return hw.pmuLock(hw.ctx, 0), s.error = kBtStopped;

  announce(hw, kStepBtEnCheck, "BT_EN (pm8550ve-f gpio3): its APID's owner, its kind, its registers");
  if (hw.btEnInfo(hw.ctx, &s.bten) || s.bten.apid < 0) return failed(kStepBtEnCheck, kBtNoBtEn, true);
  if (s.bten.owner != 0) return failed(kStepBtEnCheck, kBtBtEnOwner, true);
  if (s.bten.type != kPgTypeGpio || !pgLvMv(s.bten.subtype)) return failed(kStepBtEnCheck, kBtBtEnKind, true);
  if (hw.pmicRead(hw.ctx, kPgMode, s.regs, 7)) return failed(kStepBtEnCheck, kBtNoBtEn, true);
  s.configured = (s.regs[0] & 3u) == kPgModeOutput && (s.regs[1] & 7u) == kPgVin1 && (s.regs[2] & 7u) == kPgPullNone &&
                 (s.regs[6] & kPgEnabled) && (s.regs[4] & 0xfu) == 0;
  s.wasHigh = s.configured && (s.regs[4] & kPgOutLevel);
  s.step = kStepBtEnCheck;
  if (o.stopAfter == kStepBtEnCheck) return hw.pmuLock(hw.ctx, 0), s.error = kBtStopped;

  // One register, written and read back; the failed one is kept for the report.
  auto put = [&](uint8_t reg, uint8_t value, uint8_t mask) {
    s.touched = true;
    ++s.pmicWrites;
    uint8_t got = 0;
    if (hw.pmicWrite(hw.ctx, reg, value) || hw.pmicRead(hw.ctx, reg, &got, 1) || (got & mask) != (value & mask)) {
      s.failedReg = reg;
      s.failedWant = value;
      s.failedRead = got;
      return false;
    }
    return true;
  };
  announce(hw, kStepBtEnConfig, "BT_EN to the DT's state (output, VIN1, no bias, normal), low first");
  if (s.wasHigh) {
    if (!put(kPgOutSrc, 0, 0x8fu)) return failed(kStepBtEnConfig, kBtBtEnWrite, true);
    hw.msleep(hw.ctx, o.btEnLowMs);
  }
  if (!s.configured) {
    if (!put(kPgOutSrc, 0, 0x8fu) || !put(kPgVin, kPgVin1, 7u) || !put(kPgPull, kPgPullNone, 7u) ||
        !put(kPgMode, kPgModeOutput, 3u) || !put(kPgEn, kPgEnabled, kPgEnabled))
      return failed(kStepBtEnConfig, kBtBtEnWrite, true);
  }
  s.step = kStepBtEnConfig;
  if (o.stopAfter == kStepBtEnConfig) return hw.pmuLock(hw.ctx, 0), s.error = kBtStopped;

  announce(hw, kStepBtEnHigh, "BT_EN high (rails up, the PMU lock held), read back");
  if (!put(kPgOutSrc, kPgOutLevel, 0x8fu)) return failed(kStepBtEnHigh, kBtBtEnWrite, true);
  s.high = true;
  hw.pmicRead(hw.ctx, kPgMode, s.after, 7);
  hw.pmuLock(hw.ctx, 0);
  s.step = kStepBtEnHigh;
  if (o.stopAfter == kStepBtEnHigh) return s.error = kBtStopped;

  announce(hw, kStepPowered, "the PMU's power-up time; SW_CTRL read");
  hw.msleep(hw.ctx, o.pmuMs + 10);
  s.swCtrlIo = hw.read(hw.ctx, kWinTlmm, kPins[kPinSwCtrl].gpio * kTlmmStride + 4);
  s.wlanEnIo = hw.read(hw.ctx, kWinTlmm, kPins[kPinWlanEn].gpio * kTlmmStride + 4);
  s.step = kStepPowered;
  if (o.stopAfter == kStepPowered) return s.error = kBtStopped;
  return kBtOk;
}

// ================================================================================================================ level 3
inline int uartUp(Bringup &b, const Hw &hw, const Options &o)
{
  UartState &u = b.uart;
  Port &p = b.port;
  u = UartState();
  p.hw = &hw;
  p.maxErrorPpm = o.maxClockErrorPpm;
  auto failed = [&](int step, int error) {
    u.step = -step;
    u.error = error;
    safeOff(b, hw, o);
    return error;
  };
  auto stop = [&](int step) {
    u.step = step;
    if (o.stopAfter != step) return false;
    u.error = kBtStopped;
    return true;
  };
  if (!hwUsable(hw)) return u.step = -kStepClocks, u.error = kBtArgument;

  // The five votes (shared word: the read-modify-write under the GCC vote lock; bits are only ever set), then the branches.
  announce(hw, kStepClocks, "GCC: the QUP2 wrapper's and SE6's votes (0x52010), the five branches running");
  u.voteBefore = hw.read(hw.ctx, kWinGcc, kGccQup2Vote);
  if ((u.voteBefore & kVoteBits) != kVoteBits) {
    const int locked = hw.voteLock ? hw.voteLock(hw.ctx, 1) : 0;
    const uint32_t v = hw.read(hw.ctx, kWinGcc, kGccQup2Vote);
    u.added = kVoteBits & ~v;
    hw.write(hw.ctx, kWinGcc, kGccQup2Vote, v | kVoteBits, "GCC QUP2 votes: core, s-ahb, m-ahb, core2x, s6");
    if (hw.voteLock && !locked) hw.voteLock(hw.ctx, 0);
  }
  u.voteAfter = hw.read(hw.ctx, kWinGcc, kGccQup2Vote);
  bool on = false;
  for (u.branchPolls = 0; u.branchPolls < 200; ++u.branchPolls) {
    on = true;
    for (unsigned i = 0; i < kBranchCount; ++i) {
      u.cbcr[i] = hw.read(hw.ctx, kWinGcc, kBranches[i].cbcr);
      on = on && branchRuns(kBranches[i], u.cbcr[i], u.voteAfter, true);
    }
    if (on) break;
    hw.udelay(hw.ctx, 10);
  }
  if (!on) return failed(kStepClocks, kBtBranch);
  u.clocked = true;
  if (stop(kStepClocks)) return u.error;

  announce(hw, kStepSeCheck, "SE6: the UART protocol, the FIFO interface, the FIFO depths; the QUP2 version");
  u.qupVersion = hw.read(hw.ctx, kWinQup, kQupHwVersion);
  u.fwRevision = hw.read(hw.ctx, kWinSe, kFwRevision);
  u.ifDisable = hw.read(hw.ctx, kWinSe, kIfDisable);
  u.hwParam0 = hw.read(hw.ctx, kWinSe, kHwParam0);
  u.hwParam1 = hw.read(hw.ctx, kWinSe, kHwParam1);
  u.tx = decodeFifo(u.hwParam0, u.qupVersion);
  u.rx = decodeFifo(u.hwParam1, u.qupVersion);
  if (decodeFwRevision(u.fwRevision).protocol != kProtoUart) return failed(kStepSeCheck, kBtProtocol);
  if ((u.ifDisable & kFifoIfDisable) || u.tx.depth < 4 || u.rx.depth < 4 || u.tx.widthBits != 32 || u.rx.widthBits != 32)
    return failed(kStepSeCheck, kBtFifo);
  p.qupVersion = u.qupVersion;
  p.txDepth = u.tx.depth;
  p.rxDepth = u.rx.depth;
  if (stop(kStepSeCheck)) return u.error;

  // The source: a DFS level the clock select picks, or (DFS off) the RCG's table entry.
  announce(hw, kStepSource, "the SE6 clock source for 115200 x 16 (a DFS level, else the RCG's table entry)");
  const uint32_t r = kGccS6Rcg;
  u.dfsCmd = hw.read(hw.ctx, kWinGcc, r + kRcgDfsCmd);
  u.dfs = (u.dfsCmd & 1u) != 0;
  u.rcgCfgBefore = hw.read(hw.ctx, kWinGcc, r + kRcgCfg);
  u.rcgHzBefore = rcgRate(u.rcgCfgBefore, hw.read(hw.ctx, kWinGcc, r + kRcgM), hw.read(hw.ctx, kWinGcc, r + kRcgN));
  p.dfs = u.dfs;
  p.levelCount = 0;
  if (u.dfs) {
    for (unsigned l = 0; l < kRcgDfsLevels; ++l) {
      const uint32_t hz = rcgRate(hw.read(hw.ctx, kWinGcc, r + kRcgDfsPerf + 4 * l), hw.read(hw.ctx, kWinGcc, r + kRcgDfsM + 4 * l),
                                  hw.read(hw.ctx, kWinGcc, r + kRcgDfsN + 4 * l));
      p.levels[p.levelCount++] = hz;   // index = the clock select's level (0: an unknown rate, never chosen)
    }
  } else {
    for (unsigned i = 0; i < kS6RateCount; ++i) p.levels[p.levelCount++] = kS6Rates[i];
  }
  u.clock = planClock(kInitBaud, u.qupVersion, p.levels, p.levelCount);
  if (!u.clock.ok || u.clock.errorPpm > o.maxClockErrorPpm || u.clock.errorPpm < -o.maxClockErrorPpm)
    return failed(kStepSource, kBtClock);
  p.rcgHz = u.rcgHzBefore;
  if (!u.dfs) {
    const int rc = rcgProgram(hw, u.clock.sourceHz, &u.rcgCfgAfter, &u.rcgPolls);
    u.rcgProgrammed = true;
    if (rc) return failed(kStepSource, rc);
    p.rcgHz = u.clock.sourceHz;
  }
  u.rcgHz = u.dfs ? u.clock.sourceHz : p.rcgHz;
  if (stop(kStepSource)) return u.error;

  // The engine: Linux's port set-up and line settings (planUart), polled; the receiver started. With loopback when the self test
  // runs (the pins are not the engine's yet, so nothing reaches the controller).
  announce(hw, kStepSeInit, "SE6: FIFO mode, packing, 115200 8N1, hardware flow control, polled; the receiver started");
  UartOptions uo;
  uo.baud = kInitBaud;
  uo.hwFlow = true;
  uo.loopback = o.loopback;
  uo.interrupts = false;
  const UartPlan plan = planUart(uo, u.qupVersion, u.rx.depth, p.levels, p.levelCount);
  if (!plan.ok) return failed(kStepSeInit, kBtClock);
  for (unsigned i = 0; i < plan.count; ++i) {
    const Write &w = plan.writes[i];
    const uint32_t v = planOrs(w.offset) ? hw.read(hw.ctx, kWinSe, w.offset) | w.value : w.value;
    hw.write(hw.ctx, kWinSe, w.offset, v, w.what);
  }
  u.planWrites = plan.count;
  hw.write(hw.ctx, kWinSe, kUartManualRfr, 0, "RFR by the engine");
  u.engineUp = true;
  p.baud = plan.clock.baud;
  p.flow = true;
  p.evHead = p.evCount = 0;
  p.dec.reset();
  portStartRx(p);
  if (stop(kStepSeInit)) return u.error;

  if (o.loopback) {
    announce(hw, kStepLoopback, "the internal loopback: TX to RX, RFR to CTS (the pins are not the engine's yet)");
    uint8_t pattern[48], echo[48] = {};
    for (unsigned i = 0; i < sizeof pattern; ++i) pattern[i] = uint8_t(0x5a ^ (i * 29));
    p.raw = echo;
    p.rawCap = sizeof echo;
    p.rawHave = 0;
    int rc = portSend(p, pattern, sizeof pattern, 500);
    const uint64_t deadline = hw.nowUs(hw.ctx) + 100000;
    while (!rc && p.rawHave < sizeof echo && hw.nowUs(hw.ctx) < deadline) {
      if (!portPoll(p)) hw.udelay(hw.ctx, 20);
      if (cancelled(hw)) rc = kBtCancelled;
    }
    u.loopBytes = sizeof pattern;
    u.loopEchoed = unsigned(p.rawHave);
    for (unsigned i = 0; i < sizeof pattern; ++i) u.loopMismatch += i >= p.rawHave || echo[i] != pattern[i];
    p.raw = nullptr;
    if (rc) return failed(kStepLoopback, rc);
    if (u.loopMismatch) return failed(kStepLoopback, kBtLoopback);
    portStopRx(p);
    hw.write(hw.ctx, kWinSe, kUartLoopback, 0, "no loopback");
    portStartRx(p);
    if (stop(kStepLoopback)) return u.error;
  }

  // The pins to the engine: CTS/RTS pull-down, TX/RX pull-up, 2 mA, function 1 (qup2_se6). Then the line as the engine sees it.
  announce(hw, kStepPins, "gpio24..27 to qup2_se6 (the DT's pulls, 2 mA); the line state; what the controller sent drained");
  for (unsigned i = kPinCts; i <= kPinRx; ++i) {
    const Pin &pin = kPins[i];
    hw.write(hw.ctx, kWinTlmm, pin.gpio * kTlmmStride, pinConfig(pin.pull, pin.function, kUartDriveMa, false),
             "UART pin to qup2_se6");
    u.pinsMuxed = true;
    u.pinCfg[i] = hw.read(hw.ctx, kWinTlmm, pin.gpio * kTlmmStride);
    u.pinIo[i] = hw.read(hw.ctx, kWinTlmm, pin.gpio * kTlmmStride + 4);
    if (!pinAsDt(pin, decodePin(u.pinCfg[i], u.pinIo[i]))) return failed(kStepPins, kBtArgument);
  }
  hw.udelay(hw.ctx, 200);
  u.ios = hw.read(hw.ctx, kWinSe, kGeniIos);
  p.raw = nullptr;
  for (unsigned i = 0; i < 4; ++i) u.drained += unsigned(portPoll(p));
  p.evHead = p.evCount = 0;
  p.dec.reset();
  u.step = kStepPins;
  if (o.stopAfter == kStepPins) return u.error = kBtStopped;
  return kBtOk;
}

// ================================================================================================================ level 4
inline int controllerUp(Bringup &b, const Hw &hw, const Options &o, const FwIo &fw, uint8_t *nvm, size_t nvmCap)
{
  b.setup = SetupResult();
  b.port.hw = &hw;
  PortSetup ps{&b.port, &fw};
  const SetupIo io = portSetupIo(ps);
  const int rc = qcaSetup(io, o.setup, b.setup, nvm, nvmCap);
  if (rc == kSetupStopped) return kBtStopped;
  if (rc) {
    safeOff(b, hw, o);
    return kBtSetup;
  }
  return kBtOk;
}

// The ladder up to o.level (2..4): each level only after the previous one; the first failure leaves everything safe.
inline int bringUp(Bringup &b, const Hw &hw, const Options &o, const FwIo &fw, uint8_t *nvm, size_t nvmCap)
{
  b.up = false;
  b.step = 0;
  b.error = 0;
  if (o.level < 2) return kBtOk;
  int rc = powerOn(b, hw, o);
  b.step = b.power.step;
  if (rc || o.level < 3) return b.error = rc, b.up = !rc, rc;
  rc = uartUp(b, hw, o);
  b.step = b.uart.step;
  if (rc || o.level < 4) return b.error = rc, b.up = !rc, rc;
  rc = controllerUp(b, hw, o, fw, nvm, nvmCap);
  b.step = b.setup.step;
  b.error = rc;
  b.up = !rc;
  return rc;
}

}  // namespace pianobt
