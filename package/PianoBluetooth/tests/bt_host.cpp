// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoBluetooth's portable code:
//   GeniUart.h  what level 1 loads and when (GCC/TLMM always; the wrapper and SE6 only with all five branches running, never a FIFO
//               window; the SMMU stream scan), the decoders (RCG and DFS rates, FIFO geometry, serial clock), the clock plan for
//               115200 and 3.2 Mbit/s on both QUP generations, the UART set-up plan, FIFO word packing and the RX status decode
//   HciH4.h     the encoder and the byte-stream decoder (split packets, IBS bytes between packets, resynchronisation, oversized)
//   QcaCodec.h  the commands' bytes, the reply parsers, the file names, the NVM tag patcher and its bounds, the download plan, and
//               the whole set-up sequence against a simulated controller, with faults (no answer at the new speed, a missing board
//               NVM, no NVM at all, a hardware error during the download, a failing reset, a malformed version, a stop request)
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. bt_host.cpp -o /tmp/bt_host && /tmp/bt_host
#include "GeniUart.h"
#include "HciH4.h"
#include "QcaCodec.h"
#include "QcaSim.h"
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace pianobt;
using namespace pianobt::sim;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------------ the MMIO model
struct Soc {
  std::map<uint64_t, uint32_t> regs;
  std::vector<uint64_t> loads;
  uint32_t load(uint64_t pa)
  {
    loads.push_back(pa);
    auto it = regs.find(pa);
    return it == regs.end() ? 0 : it->second;
  }
  unsigned in(uint64_t base, uint64_t bytes) const
  {
    unsigned n = 0;
    for (uint64_t pa : loads) n += pa >= base && pa < base + bytes;
    return n;
  }
  void clocks(bool on, bool ahbHwGated = false)
  {
    regs[kGcc + kGccQup2Vote] = on ? kVoteBits : 0;
    for (const Branch &b : kBranches) {
      uint32_t v = on ? 1u : kCbcrClkOff;
      if (ahbHwGated && b.hwcg) v = kCbcrClkOff | kCbcrHwCtl | 1u;
      regs[kGcc + b.cbcr] = v;
    }
  }
};
static auto loader(Soc &s)
{
  return [&s](uint64_t pa) { return s.load(pa); };
}

static void testSurveyGates()
{
  Soc s;
  s.clocks(false);
  Clocks c = surveyClocks(loader(s), false);
  CHECK(!c.seOn);
  Pins p = surveyPins(loader(s));
  Se se = surveySe(c, true, loader(s));
  CHECK(!se.read);
  CHECK(s.in(kSe6, kSe6Bytes) == 0);
  CHECK(s.in(kQup2, kQup2Bytes) == 0);
  CHECK(s.in(kTlmm, 0x200000) == 2 * kPinCount);
  for (uint64_t pa : s.loads) CHECK((pa >= kGcc && pa < kGcc + 0x100000) || (pa >= kTlmm && pa < kTlmm + 0x200000));
  (void)p;

  // Hardware-gated AHB branches: not running by default, running with the hwcg rule (their vote bits are set).
  s.loads.clear();
  s.clocks(true, true);
  c = surveyClocks(loader(s), false);
  CHECK(!c.on[kBrMAhb] && !c.on[kBrSAhb] && c.on[kBrS6] && !c.seOn);
  c = surveyClocks(loader(s), true);
  CHECK(c.on[kBrMAhb] && c.on[kBrSAhb] && c.seOn);
  s.regs[kGcc + kGccQup2Vote] = kVoteBits & ~kVoteMAhb;   // not voted: hardware gating alone does not count
  c = surveyClocks(loader(s), true);
  CHECK(!c.on[kBrMAhb] && !c.seOn);

  // All on: the SE is read, the FIFO windows never.
  s.loads.clear();
  s.clocks(true);
  s.regs[kQup2 + kQupHwVersion] = 0x30090000;
  s.regs[kSe6 + kFwRevision] = 0x0204;
  s.regs[kSe6 + kHwParam0] = 0x20100000;   // 32-bit words, depth 16
  c = surveyClocks(loader(s), false);
  CHECK(c.seOn);
  se = surveySe(c, true, loader(s));
  CHECK(se.read);
  CHECK(decodeFwRevision(se.fwRevision).protocol == kProtoUart && decodeFwRevision(se.fwRevision).version == 4);
  CHECK(s.in(kSe6 + kTxFifo, kTxFifoStatus - kTxFifo) == 0);
  CHECK(s.in(kQup2, kQup2Bytes) == 1);
  for (uint32_t off : kSeSurveyOffsets) CHECK(!inFifoWindow(off));
  // Not wanted (piano-bt-se=0): nothing of the SE.
  s.loads.clear();
  se = surveySe(c, false, loader(s));
  CHECK(!se.read && s.in(kSe6, kSe6Bytes) == 0);
}

static void testDecoders()
{
  // 7.3728 MHz: GPLL0 even (300 MHz) x 384 / 15625, half divider field 1 (divide by 1), dual-edge M/N mode.
  const uint32_t cfg = (6u << 8) | (2u << 12) | 1u, m = 384, n = ~(15625u - 384u);
  CHECK(rcgRate(cfg, m, n) == 7372800);
  CHECK(rcgRate((6u << 8) | (2u << 12) | 1u, 64, ~(375u - 64u)) == 51200000);
  CHECK(rcgRate(0, 0, 0) == 19200000);                         // TCXO, no divider
  CHECK(rcgRate((1u << 8) | 11u, 0, 0) == 100000000);          // GPLL0 / 6 (field 2 x 6 - 1)
  CHECK(rcgRate(2u << 8, 0, 0) == 0);                          // GPLL7: unknown rate
  CHECK(rcgRate((6u << 8) | (2u << 12), 0, 0) == 0);           // M/N mode with M = 0
  // DFS: levels read only when enabled.
  Soc s;
  s.clocks(false);
  const uint64_t r = kGcc + kGccS6Rcg;
  s.regs[r + kRcgDfsCmd] = 1u | (3u << 1);
  s.regs[r + kRcgDfsPerf + 0] = cfg;
  s.regs[r + kRcgDfsM + 0] = m;
  s.regs[r + kRcgDfsN + 0] = n;
  s.regs[r + kRcgDfsPerf + 4] = (6u << 8) | (2u << 12) | 1u;
  s.regs[r + kRcgDfsM + 4] = 64;
  s.regs[r + kRcgDfsN + 4] = ~(375u - 64u);
  Clocks c = surveyClocks(loader(s), false);
  CHECK(c.dfs && c.dfsLevel == 3 && c.dfsHz[0] == 7372800 && c.dfsHz[1] == 51200000);
  uint32_t levels[16];
  CHECK(clockLevels(c, levels, 16) == 8);                      // levels 2..7 are TCXO (all-zero registers): 19.2 MHz
  s.regs[r + kRcgDfsCmd] = 0;
  s.loads.clear();
  c = surveyClocks(loader(s), false);
  CHECK(!c.dfs && s.in(r + kRcgDfsPerf, 0x80) == 0);
  CHECK(clockLevels(c, levels, 16) == 16 && levels[0] == 7372800 && levels[6] == 51200000);
  // FIFO geometry: the 6-bit depth before QUP 3.10, the 8-bit one from it on.
  CHECK(decodeFifo(0x20400000, 0x30090000).depth == 0 && decodeFifo(0x20400000, 0x300a0000).depth == 64);
  CHECK(decodeFifo(0x20100000, 0x20050000).depth == 16 && decodeFifo(0x20100000, 0x20050000).widthBits == 32);
  CHECK(decodeSerClk(1u | (4u << 4)).enabled && decodeSerClk(1u | (4u << 4)).divider == 4);
  CHECK(sampling(0x20050000) == 16 && sampling(0x20040000) == 32);
  Se se;
  se.qupVersion = 0x30090000;
  se.mClk = 1u | (4u << 4);
  CHECK(seBaud(se, 7372800) == 115200);
  // Pins: the DT's states.
  CHECK(pinAsDt(kPins[kPinTx], decodePin((1u << 2) | 3u, 0)));
  CHECK(!pinAsDt(kPins[kPinTx], decodePin(0, 0)));
  CHECK(pinAsDt(kPins[kPinWlanEn], decodePin(0x201, 0)));
}

static void testSmmu()
{
  Soc s;
  s.regs[kSmmu + 0x20] = 4;            // 4 stream-match groups
  s.regs[kSmmu + 0x24] = 0x60000053;   // 83 banks, 128 pages
  auto rd = [&](uint32_t off) { return s.load(kSmmu + off); };
  SmmuStream m = surveySmmu(kStreamQup2, rd);
  CHECK(!m.error && m.count == 0 && m.banks == 83 && m.pages == 128);
  s.regs[kSmmu + 0x800 + 4] = 0x80000000u | kStreamQup2;
  s.regs[kSmmu + 0xc00 + 4] = 1u << 16;   // bypass
  s.regs[kSmmu + 0x800 + 8] = 0x80000000u | (0x3u << 16) | 0x420;   // a masked group covering 0x420..0x423
  s.regs[kSmmu + 0xc00 + 8] = 5;          // translate through bank 5
  s.regs[kSmmu + (128 + 5) * 4096] = 1;   // its SCTLR: translation on
  m = surveySmmu(kStreamQup2, rd);
  CHECK(m.count == 2 && m.found[0].group == 1 && ((m.found[0].s2cr >> 16) & 3) == 1);
  CHECK(m.found[1].group == 2 && m.found[1].sctlr == 1);
  s.regs[kSmmu + 0x24] = 0x80000053;      // unsupported geometry
  CHECK(surveySmmu(kStreamQup2, rd).error != nullptr);
}

static void testClockPlan()
{
  const ClockPlan a = planClock(115200, 0x30090000, kS6Rates, kS6RateCount);
  CHECK(a.ok && a.sampling == 16 && a.sourceHz == 7372800 && a.divider == 4 && a.baud == 115200 && a.errorPpm == 0);
  CHECK(a.serClkCfg == (1u | (4u << 4)) && a.level == 0);
  const ClockPlan b = planClock(3200000, 0x30090000, kS6Rates, kS6RateCount);
  CHECK(b.ok && b.sourceHz == 51200000 && b.divider == 1 && b.baud == 3200000 && b.level == 6);
  const ClockPlan c = planClock(115200, 0x20040000, kS6Rates, kS6RateCount);   // an older QUP: 32 samples a bit
  CHECK(c.ok && c.sampling == 32 && c.sourceHz == 7372800 && c.divider == 2);
  const ClockPlan d = planClock(3200000, 0x20040000, kS6Rates, kS6RateCount);
  CHECK(d.ok && d.sourceHz == 102400000 && d.divider == 1);
  const uint32_t fast[1] = {128000000};
  CHECK(!planClock(300, 0x30090000, fast, 1).ok);   // 128 MHz / 4800 Hz needs a divider above 4095
  CHECK(!planClock(115200, 0x30090000, fast, 0).ok);
  const ClockPlan e = planClock(1000000, 0x30090000, kS6Rates, kS6RateCount);
  CHECK(e.ok && e.baud == 1000000 && e.errorPpm == 0);
}

static void testUartPlan()
{
  const Packing k = uartPacking();
  CHECK(k.cfg0 == 0x4380e && k.cfg1 == 0xc3e0e && k.byteGran == 0);
  UartOptions o;
  UartPlan p = planUart(o, 0x30090000, 16, kS6Rates, kS6RateCount);
  CHECK(p.ok && p.count > 20);
  auto value = [&](uint32_t off) -> long long {
    for (unsigned i = 0; i < p.count; ++i)
      if (p.writes[i].offset == off) return p.writes[i].value;
    return -1;
  };
  for (unsigned i = 0; i < p.count; ++i) CHECK(!inFifoWindow(p.writes[i].offset));
  CHECK(value(kUartTxTransCfg) == 0 && value(kUartManualRfr) == -1 && value(kRxRfrWatermark) == 14);
  CHECK(value(kSerMClkCfg) == (1 | (4 << 4)) && value(kClkSel) == 0 && value(kSeIrqEn) == 0 && value(kDmaModeEn) == 0);
  CHECK(value(kUartTxWordLen) == 8 && value(kUartTxStopBits) == 0 && value(kUartLoopback) == 0 && value(kUartRxStale) == 160);
  CHECK(planOrs(kCgcCtrl) && planOrs(kMIrqEn) && !planOrs(kUartTxTransCfg));
  o.hwFlow = false;
  o.loopback = true;
  o.baud = 3200000;
  p = planUart(o, 0x30090000, 16, kS6Rates, kS6RateCount);
  CHECK(p.ok && value(kUartTxTransCfg) == kUartCtsMask && value(kUartLoopback) == 3 && value(kClkSel) == 6);
  CHECK(!planUart(o, 0x30090000, 2, kS6Rates, kS6RateCount).ok);
  // FIFO words and the RX status.
  const uint8_t bytes[5] = {0x04, 0x0e, 0x04, 0x01, 0x99};
  CHECK(packWord(bytes, 4) == 0x01040e04 && packWord(bytes + 4, 1) == 0x99);
  uint8_t back[4] = {};
  CHECK(unpackWord(0x01040e04, back, 4) == 4 && back[0] == 0x04 && back[3] == 0x01);
  RxStatus rs = decodeRxStatus(kRxLast | (2u << kRxLastValidShift) | 3);
  CHECK(rs.words == 3 && rs.last && rs.lastBytes == 2 && rxBytes(rs) == 10);
  rs = decodeRxStatus(kRxLast | 2);
  CHECK(rs.lastBytes == 4 && rxBytes(rs) == 8);
  CHECK(rxBytes(decodeRxStatus(5)) == 20 && rxBytes(decodeRxStatus(0)) == 0);
}

// -------------------------------------------------------------------------------------------------------------- H4
struct Seen {
  std::vector<std::pair<uint8_t, std::vector<uint8_t>>> packets;
  std::vector<uint8_t> ibs;
};
static auto sinkFor(Seen &seen)
{
  return [&seen](uint8_t type, const uint8_t *p, size_t n) {
    if (!p) seen.ibs.push_back(type);
    else seen.packets.push_back({type, std::vector<uint8_t>(p, p + n)});
  };
}

static void testH4()
{
  uint8_t f[16];
  const uint8_t sub = 0x19;
  CHECK(h4Command(f, sizeof f, 0xfc00, &sub, 1) == 5 && f[0] == 1 && f[1] == 0x00 && f[2] == 0xfc && f[3] == 1 && f[4] == 0x19);
  CHECK(h4Command(f, 4, 0xfc00, &sub, 1) == 0);
  const uint8_t d[3] = {1, 2, 3};
  CHECK(h4Acl(f, sizeof f, 0x002a, 2, d, 3) == 8 && f[1] == 0x2a && f[2] == 0x20 && f[3] == 3 && f[4] == 0);
  CHECK(h4Acl(f, sizeof f, 0xf00, 0, d, 3) == 0);

  H4Decoder dec;
  Seen seen;
  // A Command Complete split in three, an IBS sleep between packets, a junk byte, an ACL packet, a zero-length event.
  const uint8_t a[] = {0x04, 0x0e, 0x04};
  const uint8_t b[] = {0x01, 0x03, 0x0c};
  const uint8_t c[] = {0x00, 0xfe, 0x77, 0x02, 0x2a, 0x20, 0x02, 0x00, 0xaa, 0xbb, 0x04, 0xff, 0x00, 0xfd};
  dec.feed(a, sizeof a, sinkFor(seen));
  CHECK(seen.packets.empty() && !dec.idle());
  dec.feed(b, sizeof b, sinkFor(seen));
  dec.feed(c, sizeof c, sinkFor(seen));
  CHECK(seen.packets.size() == 3 && dec.idle());
  CHECK(seen.packets[0].first == kH4Event && seen.packets[0].second.size() == 6 && seen.packets[0].second[0] == 0x0e);
  CommandComplete cc;
  CHECK(parseCommandComplete(seen.packets[0].second.data(), 6, cc) && cc.opcode == 0x0c03 && cc.retLen == 1 && cc.ret[0] == 0);
  CHECK(seen.packets[1].first == kH4Acl && seen.packets[1].second.size() == 6);
  CHECK(seen.packets[2].first == kH4Event && seen.packets[2].second.size() == 2);
  CHECK(seen.ibs.size() == 2 && seen.ibs[0] == kIbsSleepInd && seen.ibs[1] == kIbsWakeInd);
  CHECK(dec.stats().resyncs == 1 && dec.stats().packets == 3 && dec.stats().ibs == 2);
  // An ACL packet longer than the buffer is skipped whole; the next packet decodes.
  std::vector<uint8_t> big = {0x02, 0x01, 0x00, 0x00, 0x08};   // 2048 bytes
  big.resize(big.size() + 2048, 0x55);
  big.insert(big.end(), {0x04, 0x10, 0x01, 0x42});
  seen = {};
  dec.feed(big.data(), big.size(), sinkFor(seen));
  CHECK(dec.stats().oversized == 1 && seen.packets.size() == 1 && seen.packets[0].second[0] == kEvHardwareError);
  CommandStatus cs;
  const uint8_t st[6] = {0x0f, 0x04, 0x0c, 0x01, 0x48, 0xfc};
  CHECK(parseCommandStatus(st, 6, cs) && cs.status == 0x0c && cs.opcode == 0xfc48);
  const uint8_t bad[5] = {0x0e, 0x04, 0x01, 0x03, 0x0c};
  CHECK(!parseCommandComplete(bad, 5, cc));   // its length byte says 4 parameters, only 3 are there
}

// ------------------------------------------------------------------------------------------------------------ QCA codec
static void testCodec()
{
  CHECK(baudCode(115200) == 0 && baudCode(3200000) == 0x11 && baudCode(3500000) == 0x12 && baudCode(12345) == -1);
  uint8_t f[300];
  CHECK(cmdEdl(f, sizeof f, kEdlVersion) == 5 && f[4] == 0x19);
  CHECK(cmdSetBaud(f, sizeof f, 0x11) == 5 && f[1] == 0x48 && f[2] == 0xfc && f[4] == 0x11);
  CHECK(cmdSetBaud(f, sizeof f, 0x12) == 0);   // above 3.2 Mbit/s: refused, as Linux does
  CHECK(periBaudFrame(f, sizeof f, 0x11) == 7 && f[0] == 0x31 && f[2] == 0xf1 && f[3] == 0xff && f[6] == 0x11);
  CHECK(cmdDisableLogging(f, sizeof f) == 6 && f[1] == 0x17 && f[2] == 0xfc && f[3] == 2 && f[4] == 0x14 && f[5] == 0);
  uint8_t seg[243];
  for (unsigned i = 0; i < 243; ++i) seg[i] = uint8_t(i);
  CHECK(cmdSegment(f, sizeof f, seg, 243) == 249 && f[3] == 245 && f[4] == 0x1e && f[5] == 243 && f[6] == 0 && f[248] == 242);
  CHECK(cmdSegment(f, sizeof f, seg, 0) == 0 && cmdSegment(f, 100, seg, 243) == 0);
  const uint8_t addr[6] = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6};
  CHECK(cmdWriteBdAddr(f, sizeof f, addr) == 10 && f[1] == 0x14 && f[4] == 0xa1 && f[9] == 0xa6);

  // The version reply, as Command Complete return parameters.
  const uint8_t ver[] = {0x0e, 18, 0x01, 0x00, 0xfc, 0x00, 0x19, 0x55, 0x13, 0x00, 0x00, 0x00, 0x01, 0x00,
                         0x00, 0x02, 0x00, 0x02, 0x21, 0x40};
  CommandComplete cc;
  QcaVersion v;
  CHECK(parseCommandComplete(ver, sizeof ver, cc) && parseVersion(cc, v));
  CHECK(v.product == 0x13 && v.patch == 1 && v.rom == 0x0200 && v.soc == 0x40210200 && v.romCode == 0x20);
  CHECK(v.controller == 0x02000200);
  cc.retLen = 14;
  CHECK(!parseVersion(cc, v));
  char name[24];
  CHECK(rampatchName(name, sizeof name, 0x20) && !strcmp(name, "gngbtfw20.mbn"));
  CHECK(nvmName(name, sizeof name, 0x20, 0x2e) && !strcmp(name, "gngbtnv20.b2e"));
  CHECK(nvmName(name, sizeof name, 0x20, 0x0b) && !strcmp(name, "gngbtnv20.b0b"));
  CHECK(nvmName(name, sizeof name, 0x20, 0x102) && !strcmp(name, "gngbtnv20.b102"));
  CHECK(nvmName(name, sizeof name, 0x20, 0) && !strcmp(name, "gngbtnv20.bin"));
  CHECK(nvmName(name, sizeof name, 0x20, 0xffff) && !strcmp(name, "gngbtnv20.bin"));
  CHECK(!nvmName(name, 8, 0x20, 0x2e));
  const uint8_t bid[] = {0x0e, 8, 0x01, 0x00, 0xfc, 0x00, 0x23, 0x02, 0x00, 0x2e};
  uint16_t board = 0;
  CHECK(parseCommandComplete(bid, sizeof bid, cc) && parseBoardId(cc, board) && board == 0x2e);
  const uint8_t bi[] = {0x0e, 10, 0x01, 0x00, 0xfc, 0x00, 0x20, 0x04, 'B', 'T', 0x01, 'X'};
  char build[8];
  CHECK(parseCommandComplete(bi, sizeof bi, cc) && parseBuildInfo(cc, build, sizeof build) && !strcmp(build, "BT?X"));
  const uint8_t ack[] = {0x0e, 5, 0x01, 0x00, 0xfc, 0x00, 0x1e};
  CHECK(parseCommandComplete(ack, sizeof ack, cc) && segmentAck(cc));
  const uint8_t lv[] = {0x0e, 12, 0x01, 0x01, 0x10, 0x00, 0x0d, 0x00, 0x00, 0x0d, 0x1d, 0x00, 0x07, 0x00};
  LocalVersion l;
  CHECK(parseCommandComplete(lv, sizeof lv, cc) && parseLocalVersion(cc, l) && l.hci == 0x0d && l.manufacturer == kManufacturerQualcomm);

  // Download plans.
  const uint8_t elf[16] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
  DownloadPlan p = planDownload(elf, 16, kStockRampatchBytes, false);
  CHECK(p.kind == FwKind::elf && p.mode == 3 && p.segments == 957);
  CHECK(!segmentWaits(p, 0, 243) && !segmentWaits(p, 955, 243) && segmentWaits(p, 956, kStockRampatchBytes - 956 * 243));
  uint8_t tlv[16] = {1, 0, 0, 0};
  tlv[14] = 0;
  p = planDownload(tlv, 16, 1000, false);
  CHECK(p.kind == FwKind::tlvPatch && p.mode == 0 && segmentWaits(p, 0, 243));
  tlv[14] = 1;
  CHECK(planDownload(tlv, 16, 1000, false).mode == 1 && !segmentWaits(planDownload(tlv, 16, 1000, false), 0, 243));
  CHECK(planDownload(tlv, 16, 20, false).kind == FwKind::unknown);
  p = planDownload(elf, 16, kStockNvmBytes, true);   // an NVM is a tag file whatever its first bytes
  CHECK(p.kind == FwKind::tlvNvm && p.segments == 56 && segmentWaits(p, 0, 243));
  CHECK(planDownload(elf, 16, 0, false).kind == FwKind::unknown);

  // The NVM patcher.
  for (bool envelope : {false, true}) {
    std::vector<uint8_t> n = makeNvm(envelope, 0x83, 0x00);
    NvmInfo info = patchNvm(n.data(), n.size(), 0x11, 0, 1);
    CHECK(info.ok && info.tags == 4 && info.hci && info.sleep && info.address);
    CHECK(info.hciBefore == 0x83 && info.hciAfter == 0x03 && info.baudAfter == 0x11 && info.sleepAfter == 0x01);
    CHECK(n[info.addressAt] == 0x11 && n[info.addressAt + 5] == 0x66);
    info = patchNvm(n.data(), n.size(), 0x11, 1, -1);
    CHECK(info.ok && info.hciAfter == 0x83 && info.sleepAfter == 0x01);
    info = patchNvm(n.data(), n.size(), 0x00, -1, 0);
    CHECK(info.ok && info.hciAfter == 0x83 && info.baudAfter == 0x00 && info.sleepAfter == 0x00);
  }
  std::vector<uint8_t> n = makeNvm(false, 0, 0);
  CHECK(!patchNvm(n.data(), n.size() - 1, 0x11, 0, 0).ok);    // the list is longer than the file
  n[4 + 12 + 2 + 2] = 0x40;                                   // the address tag's length: 0x40 bytes, past the list
  CHECK(!patchNvm(n.data(), n.size(), 0x11, 0, 0).ok);
  n = makeNvm(false, 0, 0);
  n[0] = 3;                                                   // not a tag list: sent as it is
  NvmInfo other = patchNvm(n.data(), n.size(), 0x11, 0, 0);
  CHECK(other.ok && other.tags == 0);
  CHECK(!patchNvm(n.data(), 3, 0x11, 0, 0).ok);
}

// ----------------------------------------------------------------------------------------------- the simulated controller
// tests/QcaSim.h (shared with bt_bringup_host.cpp), driven here a frame at a time.
static SetupIo simIo(Sim &s)
{
  SetupIo io;
  io.ctx = &s;
  io.send = [](void *c, const uint8_t *b, size_t n) { return static_cast<Sim *>(c)->send(b, n); };
  io.drain = [](void *, unsigned) { return 0; };
  io.event = [](void *c, uint8_t *ev, size_t cap, unsigned) { return static_cast<Sim *>(c)->event(ev, cap); };
  io.hostBaud = [](void *c, uint32_t baud) {
    static_cast<Sim *>(c)->hostBaud = baud;
    static_cast<Sim *>(c)->log.push_back("host baud");
    return 0;
  };
  io.hostFlow = [](void *c, bool on) {
    static_cast<Sim *>(c)->flow = on;
    static_cast<Sim *>(c)->log.push_back(on ? "flow on" : "flow off");
    return 0;
  };
  io.sleepMs = [](void *c, unsigned) { ++static_cast<Sim *>(c)->sleeps; };
  io.fwSize = [](void *c, const char *name, uint32_t *bytes) {
    auto &f = static_cast<Sim *>(c)->files;
    auto it = f.find(name);
    if (it == f.end()) return -1;
    *bytes = uint32_t(it->second.size());
    return 0;
  };
  io.fwRead = [](void *c, const char *name, uint32_t off, uint8_t *to, uint32_t n) {
    auto &f = static_cast<Sim *>(c)->files;
    auto it = f.find(name);
    if (it == f.end() || size_t(off) + n > it->second.size()) return -1;
    std::memcpy(to, it->second.data() + off, n);
    return 0;
  };
  return io;
}

static void testSetup()
{
  static uint8_t nvm[16384];
  {
    Sim s;
    stock(s);
    const uint8_t mine[6] = {0x02, 0x00, 0x00, 0xab, 0xcd, 0xef};   // a made-up address, display order
    SetupOptions o;
    o.address = mine;
    SetupResult r;
    const int e = qcaSetup(simIo(s), o, r, nvm, sizeof nvm);
    CHECK(e == kSetupOk && r.error == kSetupOk && r.step == kStepLocalVersion);
    CHECK(r.version.romCode == 0x20 && r.versionAtSpeed.soc == 0x40210200 && r.baud == 3200000);
    CHECK(!strcmp(r.rampatch, "gngbtfw20.mbn") && !strcmp(r.nvm, "gngbtnv20.b2e") && r.boardId == 0x2e && !r.nvmFallback);
    CHECK(s.segments[Sim::kPatch] == 957 && s.received[Sim::kPatch] == s.files["gngbtfw20.mbn"]);
    CHECK(r.rampatchPlan.kind == FwKind::elf && r.nvmPlan.segments == s.segments[Sim::kNvm]);
    CHECK(r.acks == 1 + s.segments[Sim::kNvm] && r.unmatched >= 1);   // the vendor event after the last rampatch segment
    // The NVM the controller got: the HCI tag's in-band sleep bit cleared (ibs = 0) and its baud byte 0x11.
    std::vector<uint8_t> sent = s.received[Sim::kNvm];
    CHECK(sent.size() == s.files["gngbtnv20.b2e"].size());
    CHECK(r.nvmInfo.ok && r.nvmInfo.hciBefore == 0x83 && r.nvmInfo.hciAfter == 0x03 && r.nvmInfo.baudAfter == 0x11);
    CHECK(!strcmp(r.build, "BTFW1") && r.local.manufacturer == kManufacturerQualcomm);
    CHECK(r.addressRead && r.addressIsNvmDefault && r.addressSet && r.addressKept && !r.addressRewritten);
    CHECK(r.reset0 == kSetupOk && r.reset0Status == 0 && s.resets == 3);
    CHECK(s.address[0] == 0xef && s.address[5] == 0x02);
    CHECK(s.periFrames == 1 && s.wakeInds == 1);
    // The order around the speed change: peri frame, flow off, the baud command, the host's speed, flow on.
    std::vector<std::string> want = {"cmd 0c03", "cmd fc00", "peri", "flow off", "cmd fc48", "host baud", "flow on", "cmd fc00"};
    CHECK(s.log.size() > want.size() && std::vector<std::string>(s.log.begin(), s.log.begin() + long(want.size())) == want);
  }
  {
    Sim s;   // the controller does not come to the new speed
    stock(s);
    s.deafAfterBaud = true;
    SetupResult r;
    CHECK(qcaSetup(simIo(s), SetupOptions(), r, nvm, sizeof nvm) == kSetupBaud && r.step == -kStepBaud);
  }
  {
    Sim s;   // no board-specific NVM: the default one; no address given: the default stays and is reported as such
    stock(s);
    s.files.erase("gngbtnv20.b2e");
    s.expect[Sim::kNvm] = uint32_t(s.files["gngbtnv20.bin"].size());
    SetupOptions o;
    o.peri = false;
    o.ibs = 1;
    SetupResult r;
    CHECK(qcaSetup(simIo(s), o, r, nvm, sizeof nvm) == kSetupOk);
    CHECK(r.nvmFallback && !strcmp(r.nvm, "gngbtnv20.bin") && s.periFrames == 0);
    CHECK(r.nvmInfo.hciAfter == 0x83 && r.addressIsNvmDefault && !r.addressSet);
  }
  {
    Sim s;   // no NVM at all
    stock(s);
    s.files.erase("gngbtnv20.b2e");
    s.files.erase("gngbtnv20.bin");
    SetupResult r;
    CHECK(qcaSetup(simIo(s), SetupOptions(), r, nvm, sizeof nvm) == kSetupNoFile && r.step == -kStepNvm);
  }
  {
    Sim s;   // the board id cannot be read: the default NVM
    stock(s);
    s.noBoardId = true;
    s.expect[Sim::kNvm] = uint32_t(s.files["gngbtnv20.bin"].size());
    SetupResult r;
    CHECK(qcaSetup(simIo(s), SetupOptions(), r, nvm, sizeof nvm) == kSetupOk && !r.boardIdRead && !strcmp(r.nvm, "gngbtnv20.bin"));
  }
  {
    Sim s;   // a hardware error in the middle of the rampatch (an ELF: the error is the reply to the last segment's wait)
    stock(s);
    s.hwErrorInPatch = true;
    SetupResult r;
    CHECK(qcaSetup(simIo(s), SetupOptions(), r, nvm, sizeof nvm) == kSetupHardwareError && r.hardwareError == 0x0b);
    CHECK(r.step == -kStepRampatch);
  }
  {
    Sim s;   // the reset fails
    stock(s);
    s.resetFails = true;
    SetupResult r;
    CHECK(qcaSetup(simIo(s), SetupOptions(), r, nvm, sizeof nvm) == kSetupStatus && r.lastStatus == 0x03 && r.step == -kStepReset);
  }
  {
    Sim s;   // a malformed version reply
    stock(s);
    s.badVersion = true;
    SetupResult r;
    CHECK(qcaSetup(simIo(s), SetupOptions(), r, nvm, sizeof nvm) == kSetupBadReply && r.step == -kStepVersion);
  }
  {
    Sim s;   // stay at 115200 and stop after the rampatch
    stock(s);
    SetupOptions o;
    o.operBaud = 115200;
    o.stopAfter = kStepRampatch;
    SetupResult r;
    CHECK(qcaSetup(simIo(s), o, r, nvm, sizeof nvm) == kSetupStopped && r.baud == 115200 && s.periFrames == 0);
    CHECK(s.segments[Sim::kPatch] == 957 && s.segments[Sim::kNvm] == 0);
  }
  {
    Sim s;   // an NVM larger than the buffer
    stock(s);
    SetupResult r;
    CHECK(qcaSetup(simIo(s), SetupOptions(), r, nvm, 16) == kSetupBadFile && r.step == -kStepNvm);
  }
  {
    SetupIo none;
    SetupResult r;
    CHECK(qcaSetup(none, SetupOptions(), r, nvm, sizeof nvm) == kSetupArgument);
  }
  {
    Sim s;   // the ROM does not answer HCI_Reset before its patch: noted, the set-up goes on
    stock(s);
    s.romIgnoresReset = true;
    SetupResult r;
    CHECK(qcaSetup(simIo(s), SetupOptions(), r, nvm, sizeof nvm) == kSetupOk && r.reset0 == kSetupTimeout && r.reset0Status == 0xff);
    CHECK(s.resets == 3 && r.step == kStepLocalVersion);
  }
  {
    Sim s;   // without the first reset: the version is the first command
    stock(s);
    SetupOptions o;
    o.resetFirst = false;
    SetupResult r;
    CHECK(qcaSetup(simIo(s), o, r, nvm, sizeof nvm) == kSetupOk && s.log[0] == "cmd fc00" && s.resets == 2);
  }
  {
    Sim s;   // the second reset brings the NVM's default address back: written again once, then it holds
    stock(s);
    s.forgetsAddress = true;
    const uint8_t mine[6] = {0x02, 0x00, 0x00, 0x12, 0x34, 0x56};   // made up, display order
    SetupOptions o;
    o.address = mine;
    SetupResult r;
    CHECK(qcaSetup(simIo(s), o, r, nvm, sizeof nvm) == kSetupOk && r.addressSet && !r.addressKept && r.addressRewritten);
    CHECK(s.address[0] == 0x56 && s.address[5] == 0x02);
  }
  {
    Sim s;   // stop after the first reset, and after the address
    stock(s);
    SetupOptions o;
    o.stopAfter = kStepReset0;
    SetupResult r;
    CHECK(qcaSetup(simIo(s), o, r, nvm, sizeof nvm) == kSetupStopped && s.log.size() == 1);
    Sim t;
    stock(t);
    o.stopAfter = kStepAddress;
    CHECK(qcaSetup(simIo(t), o, r, nvm, sizeof nvm) == kSetupStopped && t.resets == 2);
  }
}

// The SE6 RCG's table: every entry gives its rate through rcgRate, and planRcg's registers say the same.
static void testRcgTable()
{
  for (unsigned i = 0; i < kS6RateCount; ++i) {
    const RcgEntry &e = kS6Table[i];
    CHECK(e.hz == kS6Rates[i]);
    const RcgPlan p = planRcg(e.hz, 0xfff00000u, 0xabcd0000u, 0x12340000u, 0);
    CHECK(p.ok && p.index == i && (p.cfg & 0xfff00000u) == 0xffe00000u);   // bit 20 (hardware control) cleared, the rest kept
    CHECK(rcgRate(p.cfg, p.m, p.n) == e.hz);
    if (p.mnd) CHECK((p.m >> 16) == 0xabcd && (p.n >> 16) == 0x1234);
  }
  const RcgPlan a = planRcg(7372800, 0, 0, 0, 0);
  CHECK(a.mnd && a.m == 384 && a.n == (~(15625u - 384u) & 0xffffu) && a.d == (~15625u & 0xffffu) && a.cfg == ((6u << 8) | 1u | (2u << 12)));
  const RcgPlan b = planRcg(51200000, 0, 0, 0, 0);
  CHECK(b.mnd && b.m == 64 && b.d == (~375u & 0xffffu));
  const RcgPlan c = planRcg(100000000, 0x3000, 0, 0, 0);
  CHECK(c.ok && !c.mnd && c.cfg == ((1u << 8) | 11u) && c.m == 0);
  CHECK(!planRcg(12345, 0, 0, 0, 0).ok);
}

int main()
{
  testSurveyGates();
  testDecoders();
  testSmmu();
  testClockPlan();
  testUartPlan();
  testH4();
  testCodec();
  testSetup();
  testRcgTable();
  if (failures) {
    std::printf("bt_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("bt_host: all tests passed\n");
  return 0;
}
