// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GENI serial engine as a UART (QUP2 SE6 on piano), portable: the register facts, decoders, PianoBluetooth's level 1 survey
// (loads only, behind its gates), and the plans of the later levels (the serial clock for a baud rate, the UART set-up writes,
// FIFO word packing) as pure functions that the host tests check and that level 1 never applies. Written for UniXNU-piano from the
// behaviour of the Ubuntu piano kernel's qcom_geni_serial.c, qcom-geni-se.c and geni-se.h (GPL-2.0, read as the specification
// only: register offsets, bit positions and the order of operations are facts; nothing is copied).
//
// Level 1's rules (docs/drivers/bluetooth.md section 8.1):
//   always       GCC (the QUP2 vote word, the five branch CBCRs, the SE6 RCG and its DFS table) and TLMM (the UART's four pins,
//                WLAN_EN, SW_CTRL): always-on blocks
//   SE clocked   (the five branches running; an AHB branch under hardware gating counts only with piano-bt-hwcg=1 and its vote
//                bit set) the wrapper's version word and SE6's registers, except the FIFO windows: a load of the RX FIFO pops a
//                word, so 0x700..0x7ff is never loaded by anything in this file
//   SMMU         the stream-match groups and stream-to-context words of stream 0x423, and the bank of a translating match
// A load from an unclocked block can stall the bus: every gated load happens after its gate read true in the same pass.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "BtHw.h"

namespace pianobt {

// ------------------------------------------------------------------------------------------------- serial engine registers
// Common GENI (geni-se.h): offsets from the SE base.
constexpr uint32_t kForceDefault = 0x20, kOutputCtrl = 0x24, kCgcCtrl = 0x28, kGeniStatus = 0x40, kSerMClkCfg = 0x48,
                   kSerSClkCfg = 0x4c, kIfDisable = 0x64, kFwRevision = 0x68, kClkSel = 0x7c, kByteGran = 0x254,
                   kDmaModeEn = 0x258, kTxPack0 = 0x260, kTxPack1 = 0x264, kRxPack0 = 0x284, kRxPack1 = 0x288, kMCmd0 = 0x600,
                   kMCmdCtrl = 0x604, kMIrqStatus = 0x610, kMIrqEn = 0x614, kMIrqClear = 0x618, kSCmd0 = 0x630,
                   kSCmdCtrl = 0x634, kSIrqStatus = 0x640, kSIrqEn = 0x644, kSIrqClear = 0x648, kTxFifo = 0x700, kRxFifo = 0x780,
                   kTxFifoStatus = 0x800, kRxFifoStatus = 0x804, kTxWatermark = 0x80c, kRxWatermark = 0x810,
                   kRxRfrWatermark = 0x814, kGeniIos = 0x908, kMGpLength = 0x910, kSGpLength = 0x914, kDmaTxIrqStat = 0xc40,
                   kDmaTxIrqClr = 0xc44, kDmaRxIrqStat = 0xd40, kDmaRxIrqClr = 0xd44, kDmaRxLenIn = 0xd54, kGsiEventEn = 0xe18,
                   kSeIrqEn = 0xe1c, kHwParam0 = 0xe24, kHwParam1 = 0xe28, kDmaGeneralCfg = 0xe30;
// The UART's own (qcom_geni_serial.c).
constexpr uint32_t kUartLoopback = 0x22c, kUartIoMacro = 0x240, kUartTxTransCfg = 0x25c, kUartTxWordLen = 0x268,
                   kUartTxStopBits = 0x26c, kUartTxTransLen = 0x270, kUartRxTransCfg = 0x280, kUartRxWordLen = 0x28c,
                   kUartRxStale = 0x294, kUartTxParity = 0x2a4, kUartRxParity = 0x2a8, kUartManualRfr = 0x2ac;
// Fields.
constexpr uint32_t kMCmdActive = 1u << 0, kSCmdActive = 1u << 12;             // SE_GENI_STATUS
constexpr uint32_t kSerClkEnable = 1u << 0, kClkDivShift = 4, kClkDivMax = 0xfff;   // GENI_SER_{M,S}_CLK_CFG, 15:4 the divider
constexpr uint32_t kFifoIfDisable = 1u << 0;                                   // GENI_IF_DISABLE_RO: FIFO interface off (GSI only)
constexpr uint32_t kDmaModeOn = 1u << 0;                                       // SE_GENI_DMA_MODE_EN: SE DMA instead of FIFO
constexpr uint32_t kUartTxParityEn = 1u << 0, kUartCtsMask = 1u << 1;          // SE_UART_TX_TRANS_CFG (CTS mask: ignore CTS)
constexpr uint32_t kUartRxParityEn = 1u << 4;                                  // SE_UART_RX_TRANS_CFG
constexpr uint32_t kParityCalc = 1u << 0;                                      // SE_UART_{TX,RX}_PARITY_CFG
constexpr uint32_t kRfrManual = 1u << 31, kRfrNotReady = 1u << 1, kRfrReady = 1u << 0;   // SE_UART_MANUAL_RFR
constexpr uint32_t kLoopRxTx = 1u << 0, kLoopCtsRts = 1u << 1;                 // SE_UART_LOOPBACK_CFG
constexpr uint32_t kIosRx = 1u << 0, kIosCts = 1u << 1;                        // SE_GENI_IOS: RX line level, CTS (low = asserted)
constexpr uint32_t kOpcodeShift = 27, kUartStartTx = 1, kUartStartRead = 1;    // M/S_CMD0: opcode 31:27, parameters 26:0
constexpr uint32_t kMCmdDone = 1u << 0, kMTxFifoWatermark = 1u << 30;          // M_IRQ_STATUS
constexpr uint32_t kSRxFifoWatermark = 1u << 26, kSRxFifoLast = 1u << 27;      // S_IRQ_STATUS
constexpr uint32_t kRxLast = 1u << 31, kRxLastValidShift = 28, kRxWordsMask = 0x1ffffff;   // RX_FIFO_STATUS
constexpr uint32_t kTxWordsMask = 0x0fffffff;                                  // TX_FIFO_STATUS
// geni-se.h's common interrupt enables (what geni_se_init ORs in), and the CGC / output defaults of geni_se_io_init.
constexpr uint32_t kMCommonIrqs = 0x7eu | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29);
constexpr uint32_t kSCommonIrqs = 0x3eu | 0x3e00u | (1u << 24) | (1u << 25);
constexpr uint32_t kDefaultCgc = 0x7f, kDefaultOutputCtrl = 0x7f, kDmaCgcOn = 0xf, kSeIrqGeni = (1u << 2) | (1u << 3);
// The QUP version from which the UART samples 16 times a bit instead of 32 (QUP_SE_VERSION_2_5).
constexpr uint32_t kQupVersion25 = 0x20050000;
constexpr uint32_t kProtoUart = 2;

// ------------------------------------------------------------------------------------------------------------- decoders
struct FwRevision {
  uint32_t protocol, version;
};
inline FwRevision decodeFwRevision(uint32_t v) { return {(v >> 8) & 0xffu, v & 0xffu}; }
inline const char *protocolName(uint32_t p)
{
  switch (p) {
  case 0: return "none";
  case 1: return "SPI";
  case 2: return "UART";
  case 3: return "I2C";
  case 4: return "I3C";
  case 5: return "SPI slave";
  case 0xff: return "invalid (no firmware)";
  default: return "?";
  }
}
inline uint32_t qupMajor(uint32_t v) { return v >> 28; }
inline uint32_t qupMinor(uint32_t v) { return (v >> 16) & 0xfffu; }
// SE_HW_PARAM_0 / _1: FIFO width 29:24 (bits), depth 21:16 in words, 23:16 from QUP 3.10 on (geni-se.h's two masks).
struct Fifo {
  uint32_t depth, widthBits;
};
inline Fifo decodeFifo(uint32_t hwParam, uint32_t qupVersion)
{
  const bool wide = (qupMajor(qupVersion) == 3 && qupMinor(qupVersion) >= 10) || qupMajor(qupVersion) > 3;
  return {(hwParam >> 16) & (wide ? 0xffu : 0x3fu), (hwParam >> 24) & 0x3fu};
}
struct SerClk {
  bool enabled;
  uint32_t divider;
};
inline SerClk decodeSerClk(uint32_t v) { return {(v & kSerClkEnable) != 0, (v >> kClkDivShift) & kClkDivMax}; }
inline unsigned sampling(uint32_t qupVersion) { return qupVersion >= kQupVersion25 ? 16u : 32u; }

// An RCG's output from CFG, M and the N register (~(N - M)), for parent map 5 (BtHw.h): 0 when the source is unknown or M/N is
// degenerate (clk-rcg2's calc_rate: rate * 2 / (hid + 1), then * M / N in M/N mode).
inline uint32_t rcgRate(uint32_t cfg, uint32_t m, uint32_t nRegister)
{
  uint64_t hz = kRcgParentHz[(cfg >> 8) & 7u];
  const uint32_t hid = cfg & 0x1fu, mode = (cfg >> 12) & 3u;
  if (!hz) return 0;
  if (hid) hz = hz * 2 / (hid + 1);
  if (mode) {
    const uint32_t mm = m & 0xffffu, n = ((~nRegister) & 0xffffu) + mm;
    if (!mm || !n || mm > n) return 0;
    hz = hz * mm / n;
  }
  return uint32_t(hz);
}

// ----------------------------------------------------------------------------------------------------------- the survey
struct Clocks {
  uint32_t vote = 0;
  uint32_t cbcr[kBranchCount] = {};
  bool on[kBranchCount] = {};
  uint32_t rcgCmd = 0, rcgCfg = 0, rcgM = 0, rcgN = 0, rcgD = 0, rcgHz = 0;
  uint32_t dfsCmd = 0, dfsCfg[kRcgDfsLevels] = {}, dfsM[kRcgDfsLevels] = {}, dfsN[kRcgDfsLevels] = {}, dfsHz[kRcgDfsLevels] = {};
  bool dfs = false;
  unsigned dfsLevel = 0;
  bool seOn = false;   // all five branches running: SE6's and the wrapper's registers may be loaded
};
// A branch runs when its CLK_OFF bit is clear; with `hwcg`, an AHB branch under hardware gating (HW_CTL) whose vote bit is set
// counts as running too (PianoWifi's piano-wifi-hwcg rule: off by default).
inline bool branchRuns(const Branch &b, uint32_t cbcr, uint32_t vote, bool hwcg)
{
  if (!(cbcr & kCbcrClkOff)) return true;
  return hwcg && b.hwcg && (cbcr & kCbcrHwCtl) && (vote & b.vote);
}
template <class Load> Clocks surveyClocks(Load &&load, bool hwcg)
{
  Clocks c;
  c.vote = load(kGcc + kGccQup2Vote);
  c.seOn = true;
  for (unsigned i = 0; i < kBranchCount; ++i) {
    c.cbcr[i] = load(kGcc + kBranches[i].cbcr);
    c.on[i] = branchRuns(kBranches[i], c.cbcr[i], c.vote, hwcg);
    c.seOn = c.seOn && c.on[i];
  }
  const uint64_t r = kGcc + kGccS6Rcg;
  c.rcgCmd = load(r);
  c.rcgCfg = load(r + kRcgCfg);
  c.rcgM = load(r + kRcgM);
  c.rcgN = load(r + kRcgN);
  c.rcgD = load(r + kRcgD);
  c.rcgHz = rcgRate(c.rcgCfg, c.rcgM, c.rcgN);
  c.dfsCmd = load(r + kRcgDfsCmd);
  c.dfs = (c.dfsCmd & 1u) != 0;
  c.dfsLevel = (c.dfsCmd >> 1) & 0xfu;
  if (c.dfs) {
    for (unsigned l = 0; l < kRcgDfsLevels; ++l) {
      c.dfsCfg[l] = load(r + kRcgDfsPerf + 4 * l);
      c.dfsM[l] = load(r + kRcgDfsM + 4 * l);
      c.dfsN[l] = load(r + kRcgDfsN + 4 * l);
      c.dfsHz[l] = rcgRate(c.dfsCfg[l], c.dfsM[l], c.dfsN[l]);
    }
  }
  return c;
}
// The rates the SE's clock select can choose from: the DFS levels when DFS is on, else the RCG's frequency table (GCC).
inline unsigned clockLevels(const Clocks &c, uint32_t *rates, unsigned cap)
{
  unsigned n = 0;
  if (c.dfs) {
    for (unsigned l = 0; l < kRcgDfsLevels && n < cap; ++l)
      if (c.dfsHz[l]) rates[n++] = c.dfsHz[l];
  } else {
    for (unsigned i = 0; i < kS6RateCount && n < cap; ++i) rates[n++] = kS6Rates[i];
  }
  return n;
}

struct PinWords {
  uint32_t cfg = 0, io = 0;
};
struct PinDecode {
  unsigned pull, function, driveMa;
  bool outputEnable, in, out;
};
inline PinDecode decodePin(uint32_t cfg, uint32_t io)
{
  PinDecode d;
  d.pull = cfg & 3u;
  d.function = (cfg >> 2) & 0xfu;
  d.driveMa = (((cfg >> 6) & 7u) + 1) * 2;
  d.outputEnable = (cfg >> 9) & 1u;
  d.in = io & 1u;
  d.out = (io >> 1) & 1u;
  return d;
}
inline const char *pullName(unsigned pull) { return pull == 0 ? "none" : pull == 1 ? "down" : pull == 2 ? "keeper" : "up"; }
// What the DT's state asks for (BtHw.h kPins): the function, and the pull it names.
inline bool pinAsDt(const Pin &p, const PinDecode &d)
{
  const unsigned pull = p.pull < 0 ? 1u : p.pull > 0 ? 3u : 0u;
  return d.function == p.function && (p.function == 0 || d.pull == pull);
}
struct Pins {
  PinWords pin[kPinCount];
};
template <class Load> Pins surveyPins(Load &&load)
{
  Pins p;
  for (unsigned i = 0; i < kPinCount; ++i) p.pin[i] = {load(tlmmCfg(kPins[i].gpio)), load(tlmmIo(kPins[i].gpio))};
  return p;
}

// SE6 and the wrapper's version: loaded only when Clocks::seOn. Never the FIFO windows.
struct Se {
  bool read = false;
  uint32_t qupVersion = 0;
  uint32_t forceDefault = 0, outputCtrl = 0, cgc = 0, status = 0, mClk = 0, sClk = 0, ifDisable = 0, fwRevision = 0, clkSel = 0;
  uint32_t loopback = 0, ioMacro = 0, txTrans = 0, txWordLen = 0, txStopBits = 0, txTransLen = 0, rxTrans = 0, rxWordLen = 0,
           rxStale = 0, txParity = 0, rxParity = 0, manualRfr = 0;
  uint32_t byteGran = 0, dmaMode = 0, txPack0 = 0, txPack1 = 0, rxPack0 = 0, rxPack1 = 0;
  uint32_t mCmd0 = 0, mIrqStatus = 0, mIrqEn = 0, sCmd0 = 0, sIrqStatus = 0, sIrqEn = 0;
  uint32_t txFifoStatus = 0, rxFifoStatus = 0, txWm = 0, rxWm = 0, rxRfrWm = 0, ios = 0, mGpLength = 0, sGpLength = 0;
  uint32_t dmaTxIrq = 0, dmaRxIrq = 0, dmaRxLenIn = 0, gsiEventEn = 0, seIrqEn = 0, hwParam0 = 0, hwParam1 = 0, dmaGeneral = 0;
};
// The SE offsets level 1 loads, in one place, so the host test can check that none of them is in a FIFO window.
constexpr uint32_t kSeSurveyOffsets[] = {
  kForceDefault, kOutputCtrl, kCgcCtrl, kGeniStatus, kSerMClkCfg, kSerSClkCfg, kIfDisable, kFwRevision, kClkSel, kUartLoopback,
  kUartIoMacro, kUartTxTransCfg, kUartTxWordLen, kUartTxStopBits, kUartTxTransLen, kUartRxTransCfg, kUartRxWordLen, kUartRxStale,
  kUartTxParity, kUartRxParity, kUartManualRfr, kByteGran, kDmaModeEn, kTxPack0, kTxPack1, kRxPack0, kRxPack1, kMCmd0, kMIrqStatus,
  kMIrqEn, kSCmd0, kSIrqStatus, kSIrqEn, kTxFifoStatus, kRxFifoStatus, kTxWatermark, kRxWatermark, kRxRfrWatermark, kGeniIos,
  kMGpLength, kSGpLength, kDmaTxIrqStat, kDmaRxIrqStat, kDmaRxLenIn, kGsiEventEn, kSeIrqEn, kHwParam0, kHwParam1, kDmaGeneralCfg};
constexpr bool inFifoWindow(uint32_t off) { return off >= kTxFifo && off < kTxFifoStatus; }
template <class Load> Se surveySe(const Clocks &c, bool wanted, Load &&load)
{
  Se s;
  if (!c.seOn || !wanted) return s;
  s.read = true;
  s.qupVersion = load(kQup2 + kQupHwVersion);
  const auto r = [&](uint32_t off) { return inFifoWindow(off) ? 0u : load(kSe6 + off); };
  s.forceDefault = r(kForceDefault);
  s.outputCtrl = r(kOutputCtrl);
  s.cgc = r(kCgcCtrl);
  s.status = r(kGeniStatus);
  s.mClk = r(kSerMClkCfg);
  s.sClk = r(kSerSClkCfg);
  s.ifDisable = r(kIfDisable);
  s.fwRevision = r(kFwRevision);
  s.clkSel = r(kClkSel);
  s.loopback = r(kUartLoopback);
  s.ioMacro = r(kUartIoMacro);
  s.txTrans = r(kUartTxTransCfg);
  s.txWordLen = r(kUartTxWordLen);
  s.txStopBits = r(kUartTxStopBits);
  s.txTransLen = r(kUartTxTransLen);
  s.rxTrans = r(kUartRxTransCfg);
  s.rxWordLen = r(kUartRxWordLen);
  s.rxStale = r(kUartRxStale);
  s.txParity = r(kUartTxParity);
  s.rxParity = r(kUartRxParity);
  s.manualRfr = r(kUartManualRfr);
  s.byteGran = r(kByteGran);
  s.dmaMode = r(kDmaModeEn);
  s.txPack0 = r(kTxPack0);
  s.txPack1 = r(kTxPack1);
  s.rxPack0 = r(kRxPack0);
  s.rxPack1 = r(kRxPack1);
  s.mCmd0 = r(kMCmd0);
  s.mIrqStatus = r(kMIrqStatus);
  s.mIrqEn = r(kMIrqEn);
  s.sCmd0 = r(kSCmd0);
  s.sIrqStatus = r(kSIrqStatus);
  s.sIrqEn = r(kSIrqEn);
  s.txFifoStatus = r(kTxFifoStatus);
  s.rxFifoStatus = r(kRxFifoStatus);
  s.txWm = r(kTxWatermark);
  s.rxWm = r(kRxWatermark);
  s.rxRfrWm = r(kRxRfrWatermark);
  s.ios = r(kGeniIos);
  s.mGpLength = r(kMGpLength);
  s.sGpLength = r(kSGpLength);
  s.dmaTxIrq = r(kDmaTxIrqStat);
  s.dmaRxIrq = r(kDmaRxIrqStat);
  s.dmaRxLenIn = r(kDmaRxLenIn);
  s.gsiEventEn = r(kGsiEventEn);
  s.seIrqEn = r(kSeIrqEn);
  s.hwParam0 = r(kHwParam0);
  s.hwParam1 = r(kHwParam1);
  s.dmaGeneral = r(kDmaGeneralCfg);
  return s;
}
// The baud rate the SE's serial clock gives (0: not derivable), from the source rate the clock select points at.
inline uint32_t seBaud(const Se &s, uint32_t sourceHz)
{
  const SerClk k = decodeSerClk(s.mClk);
  if (!k.enabled || !k.divider || !sourceHz) return 0;
  return sourceHz / k.divider / sampling(s.qupVersion);
}

// The apps SMMU's entries for one stream (ARM SMMUv2: SMR valid bit 31, mask 30:16, id 14:0; S2CR type 17:16 (0 translate through
// context bank 7:0, 1 bypass, 2 fault)), and the bank's SCTLR when it translates. Read only; re-expressed from PianoWifi's
// surveySmmu (same repository).
struct SmmuMatch {
  unsigned group = 0;
  uint32_t smr = 0, s2cr = 0, sctlr = 0;
};
struct SmmuStream {
  uint32_t idr0 = 0, idr1 = 0, groups = 0, banks = 0, pages = 0;
  const char *error = nullptr;
  SmmuMatch found[4];
  unsigned count = 0;
};
template <class Read> SmmuStream surveySmmu(uint16_t stream, Read &&rd)
{
  SmmuStream s;
  s.idr0 = rd(0x20);
  s.idr1 = rd(0x24);
  s.groups = s.idr0 & 0xffu;
  if (s.groups > 128) s.groups = 128;
  s.banks = s.idr1 & 0xffu;
  s.pages = 1u << (((s.idr1 >> 28) & 7u) + 1);
  if ((s.idr1 >> 31) || !s.groups || !s.banks || s.banks > 128 || uint64_t(s.pages + s.banks) * 4096 > kSmmuBytes) {
    s.error = "unsupported SMMU geometry";
    return s;
  }
  for (unsigned g = 0; g < s.groups && s.count < 4; ++g) {
    const uint32_t smr = rd(0x800 + 4 * g);
    if (!(smr & 0x80000000u)) continue;
    const uint32_t mask = (smr >> 16) & 0x7fffu;
    if (((smr ^ stream) & 0x7fffu & ~mask) != 0) continue;
    SmmuMatch &m = s.found[s.count++];
    m.group = g;
    m.smr = smr;
    m.s2cr = rd(0xc00 + 4 * g);
    if (((m.s2cr >> 16) & 3u) == 0 && (m.s2cr & 0xffu) < s.banks) m.sctlr = rd((s.pages + (m.s2cr & 0xffu)) * 4096);
  }
  return s;
}

// ------------------------------------------------------------------------------------- the later levels' plans (pure, not applied)
// The serial clock for a baud rate: Linux's choice (geni_serial_set_rate + geni_se_clk_freq_match, inexact allowed): for each
// source rate the divider rounds up, the rate it gives is never above the one asked for, the closest wins and an exact one ends
// the search. `levels` are the rates the clock select can choose (clockLevels).
struct ClockPlan {
  bool ok = false;
  const char *why = nullptr;
  unsigned sampling = 0, level = 0;
  uint32_t sourceHz = 0, divider = 0, serClkCfg = 0, baud = 0;
  int32_t errorPpm = 0;
};
inline ClockPlan planClock(uint32_t baud, uint32_t qupVersion, const uint32_t *levels, unsigned count)
{
  ClockPlan p;
  p.sampling = sampling(qupVersion);
  if (!baud || !count) {
    p.why = "no baud rate or no source rate";
    return p;
  }
  const uint64_t want = uint64_t(baud) * p.sampling;
  uint64_t best = ~0ull;
  for (unsigned i = 0; i < count; ++i) {
    if (!levels[i]) continue;
    const uint64_t div = (levels[i] + want - 1) / want;
    const uint64_t delta = want - levels[i] / div;
    if (delta < best) {
      best = delta;
      p.level = i;
      p.sourceHz = levels[i];
      p.divider = uint32_t(div);
      if (!delta) break;
    }
  }
  if (best == ~0ull) {
    p.why = "no usable source rate";
    return p;
  }
  if (p.divider > kClkDivMax) {
    p.why = "the divider does not fit in 12 bits";
    return p;
  }
  p.serClkCfg = kSerClkEnable | p.divider << kClkDivShift;
  p.baud = uint32_t(p.sourceHz / p.divider / p.sampling);
  p.errorPpm = int32_t((int64_t(p.baud) - int64_t(baud)) * 1000000 / int64_t(baud));
  p.ok = true;
  return p;
}

// The FIFO packing vectors for 8-bit words, four to a FIFO word, least significant byte first (what qcom_geni_serial asks
// geni_se_config_packing for): four vectors of (start bit 9:5, direction 4, length - 1 3:1, stop 0), two to a register.
struct Packing {
  uint32_t cfg0, cfg1, byteGran;
};
inline Packing uartPacking()
{
  uint32_t v[4];
  for (unsigned i = 0; i < 4; ++i) v[i] = (8u * i) << 5 | 7u << 1;
  v[3] |= 1u;
  return {v[0] | v[1] << 10, v[2] | v[3] << 10, 0};
}

// The UART set-up of level 3, as a list of writes (offset, value, what) in Linux's order: port set-up (stale count, packing,
// geni_se_init with interrupts off for polling, FIFO mode), then the line settings (serial clock, 8N1, flow control, loopback).
struct Write {
  uint32_t offset, value;
  const char *what;
};
struct UartOptions {
  uint32_t baud = kInitBaud;
  bool hwFlow = true;        // CTS honoured, RFR driven by the engine (both sides of the link use RTS/CTS)
  bool loopback = false;     // TX to RX and RTS to CTS inside the engine: the level 3 self test before BT_EN rises
  bool interrupts = false;   // SE_IRQ_EN's GENI M/S outputs (polled: off; the status registers still latch)
};
struct UartPlan {
  bool ok = false;
  const char *why = nullptr;
  ClockPlan clock;
  Write writes[40];
  unsigned count = 0;
  bool overflow = false;
};
inline UartPlan planUart(const UartOptions &o, uint32_t qupVersion, uint32_t rxFifoDepth, const uint32_t *levels, unsigned levelCount)
{
  UartPlan p;
  p.clock = planClock(o.baud, qupVersion, levels, levelCount);
  if (!p.clock.ok) {
    p.why = p.clock.why;
    return p;
  }
  if (rxFifoDepth < 4) {
    p.why = "the RX FIFO depth is not plausible";
    return p;
  }
  const Packing k = uartPacking();
  auto add = [&](uint32_t off, uint32_t value, const char *what) {
    if (p.count < sizeof p.writes / sizeof p.writes[0]) p.writes[p.count++] = {off, value, what};
    else p.overflow = true;
  };
  add(kUartRxStale, 10 * 16, "RX stale count: 16 characters of 10 bits");
  add(kTxPack0, k.cfg0, "TX packing 0");
  add(kTxPack1, k.cfg1, "TX packing 1");
  add(kRxPack0, k.cfg0, "RX packing 0");
  add(kRxPack1, k.cfg1, "RX packing 1");
  add(kByteGran, k.byteGran, "byte granularity: four 8-bit words a FIFO word");
  add(kGsiEventEn, 0, "no GSI events");
  add(kMIrqClear, 0xffffffffu, "clear the main sequencer's status");
  add(kSIrqClear, 0xffffffffu, "clear the secondary sequencer's status");
  add(kDmaTxIrqClr, 0xffffffffu, "clear the TX DMA status");
  add(kDmaRxIrqClr, 0xffffffffu, "clear the RX DMA status");
  add(kCgcCtrl, kDefaultCgc, "clock gating: the default enables (ORed in)");
  add(kDmaGeneralCfg, kDmaCgcOn, "DMA clock gating on (ORed in)");
  add(kOutputCtrl, kDefaultOutputCtrl, "drive the IO outputs");
  add(kForceDefault, 1, "force the IO defaults");
  add(kSeIrqEn, o.interrupts ? kSeIrqGeni : 0u, o.interrupts ? "GENI M/S interrupt outputs on" : "interrupt outputs off (polled)");
  add(kDmaModeEn, 0, "FIFO mode");
  add(kRxWatermark, 2, "RX watermark");
  add(kRxRfrWatermark, rxFifoDepth - 2, "RFR deasserted two words before the RX FIFO is full");
  add(kMIrqEn, kMCommonIrqs | kMCmdDone, "main sequencer status bits (latched, polled)");
  add(kSIrqEn, kSCommonIrqs | kSRxFifoWatermark | kSRxFifoLast, "secondary sequencer status bits (latched, polled)");
  add(kSerMClkCfg, p.clock.serClkCfg, "serial clock (main)");
  add(kSerSClkCfg, p.clock.serClkCfg, "serial clock (secondary)");
  add(kClkSel, p.clock.level & 7u, "the source level");
  add(kUartLoopback, o.loopback ? (kLoopRxTx | kLoopCtsRts) : 0u, o.loopback ? "internal loopback" : "no loopback");
  add(kUartTxTransCfg, o.hwFlow ? 0u : kUartCtsMask, o.hwFlow ? "no parity, CTS honoured" : "no parity, CTS ignored");
  add(kUartTxParity, 0, "TX parity off");
  add(kUartRxTransCfg, 0, "no RX parity");
  add(kUartRxParity, 0, "RX parity off");
  add(kUartTxWordLen, 8, "8 data bits (TX)");
  add(kUartRxWordLen, 8, "8 data bits (RX)");
  add(kUartTxStopBits, 0, "1 stop bit");
  p.ok = !p.overflow;
  if (!p.ok) p.why = "the write list overflowed";
  return p;
}
// Writes the plan ORs into the register instead of replacing it (geni_se_io_init's and geni_se_init's read-modify-writes). The
// status bits are enabled in M/S_IRQ_EN while SE_IRQ_EN keeps them from the GIC: they latch and are polled (PianoTouch's rule).
constexpr bool planOrs(uint32_t offset)
{
  return offset == kCgcCtrl || offset == kDmaGeneralCfg || offset == kMIrqEn || offset == kSIrqEn;
}

// FIFO words: four bytes each, the first byte in bits 7:0 (uartPacking). RX: RX_FIFO_STATUS gives the word count and, with
// RX_LAST, how many bytes of the last word are valid (0 = all four).
inline uint32_t packWord(const uint8_t *bytes, size_t n)
{
  uint32_t w = 0;
  for (size_t i = 0; i < n && i < 4; ++i) w |= uint32_t(bytes[i]) << (8 * i);
  return w;
}
inline size_t unpackWord(uint32_t w, uint8_t *bytes, size_t n)
{
  size_t i = 0;
  for (; i < n && i < 4; ++i) bytes[i] = uint8_t(w >> (8 * i));
  return i;
}
struct RxStatus {
  uint32_t words;
  bool last;
  unsigned lastBytes;   // valid bytes in the last word when `last`
};
inline RxStatus decodeRxStatus(uint32_t v)
{
  const unsigned valid = (v >> kRxLastValidShift) & 7u;
  return {v & kRxWordsMask, (v & kRxLast) != 0, valid ? valid : 4u};
}
// The bytes waiting in the RX FIFO.
inline uint32_t rxBytes(const RxStatus &r)
{
  if (!r.words) return 0;
  return r.last ? (r.words - 1) * 4 + r.lastBytes : r.words * 4;
}

}  // namespace pianobt
