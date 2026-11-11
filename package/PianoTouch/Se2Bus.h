// SPDX-License-Identifier: BSD-2-Clause-Patent
// The touch SPI bus of piano: QUP1 serial engine 2 (spi@a88000, `qup1_se2`) on GPIO40 MISO, 41 MOSI, 42 CLK, 43 CS, and the same
// four pins driven by software. Facts only (no I/O here, tests/touch_host.cpp checks the decoding); sources in UPSTREAM:
//   GCC (0x100000): the apps branch vote register 0x52008 (bits 18-21 the QUP1 wrapper's clocks, bit 24 gcc_qupv3_wrap1_s2_clk),
//     CBCRs (bit 31 = the clock is off) 0x23140 / 0x23144 (wrapper master/slave AHB), 0x23148 / 0x2315c (the other two wrapper
//     clocks the 13T's QUP1 code polls), 0x1827c (the SE2 branch); the SE2 clock is the QSPI reference RCG (CMD_RCGR 0x188c0: CFG
//     +4 [4:0] 2 x divider - 1, [10:8] source, [13:12] M/N mode; M +8, N +0xc as ~(N - M); DFS +0x14) through the divider 0x1828c
//     ([3:0] + 1); the RCG's sources 0 XO 19.2 MHz, 1 GPLL0 600 MHz, 2 500 MHz, 6 GPLL0 even 300 MHz
//       (Redmi K90's goodix touch on the same SE of the same SoC, OnePlus 13T's QUP1 S4 code, mainline gcc-sm8750 names: inferred)
//   TLMM: the pins' function 1 is qup1_se2, 6 mA, no bias (the DT's qup-spi2-*-state); the lock word of 0x52008 (the 13T/A830
//     protocol: 0 free, else the holder's tag, compare-exchange, 10 us steps, at most 50 ms) is PianoA830's sm8750_gcc_vote_owner
//   SMMU: QUP1's stream 0xa3 (DT geniqup@ac0000 iommus), through a bypass context bank (package/PianoDwc3/SmmuStream.h)
#ifndef PIANO_TOUCH_SE2_BUS_H
#define PIANO_TOUCH_SE2_BUS_H
#include <stdint.h>

namespace pianotouch {

constexpr uint64_t kGcc = 0x100000, kGccBytes = 0x60000, kSe2 = 0xa88000, kSe2Bytes = 0x4000, kQup1 = 0xac0000, kQup1Bytes = 0x2000;
constexpr uint64_t kTlmm = 0x0f100000, kSmmu = 0x15000000, kSmmuBytes = 0x100000;
constexpr uint16_t kQup1Stream = 0xa3;
constexpr uint32_t kGccVote = 0x52008, kVoteWrapper = 0xfu << 18, kVoteSe2 = 1u << 24, kVoteBits = kVoteWrapper | kVoteSe2;
constexpr uint32_t kGccVoteTag = 0x54434831;   // "TCH1", PianoTouch's tag in the lock word
constexpr uint32_t kQspiRefRcg = 0x188c0, kSe2Divider = 0x1828c, kClockOff = 1u << 31;
struct Branch {
  const char *name;
  uint32_t cbcr;
};
constexpr Branch kBranches[5] = {{"wrap1 m-ahb", 0x23140}, {"wrap1 s-ahb", 0x23144}, {"wrap1 0x23148", 0x23148},
                                 {"wrap1 0x2315c", 0x2315c}, {"wrap1 s2", 0x1827c}};
// The pins, in the engine's order.
enum PinIndex : unsigned { kMiso, kMosi, kClk, kCs, kIrq, kLcdId, kPins };
constexpr unsigned kGpio[kPins] = {40, 41, 42, 43, 162, 100};
constexpr uint32_t kFunctionQup = 1, kDrive6mA = 2;   // CTL [5:2] function, [8:6] drive (mA / 2 - 1)

// The QSPI reference RCG's rate from CFG / M / N(~(N - M)); 0 when the source is not a known one or M/N is degenerate.
inline uint32_t rcgRate(uint32_t cfg, uint32_t m, uint32_t notNMinusM)
{
  static const uint32_t kParent[8] = {19200000, 600000000, 500000000, 0, 0, 0, 300000000, 0};
  uint64_t hz = kParent[(cfg >> 8) & 7];
  const uint32_t div = cfg & 0x1f, mode = (cfg >> 12) & 3;
  if (div) hz = hz * 2 / (div + 1);
  if (mode) {
    const uint32_t mm = m & 0xffff, n = ((~notNMinusM) & 0xffff) + mm;
    if (!mm || !n || mm > n) return 0;
    hz = hz * mm / n;
  }
  return uint32_t(hz);
}

struct Se2Clock {
  uint32_t cmd = 0, cfg = 0, m = 0, n = 0, dfs = 0, divider = 0;
  uint32_t sourceHz = 0, seHz = 0;   // the RCG's output, and after the SE2 divider
};
// read(offset) reads a GCC register. Read only.
template <class Read>
Se2Clock se2Clock(Read rd)
{
  Se2Clock c;
  c.cmd = rd(kQspiRefRcg);
  c.dfs = rd(kQspiRefRcg + 0x14);
  // With DFS on, the rate the engine picks is performance level 0's (GENI CLK_SEL 0).
  c.cfg = rd(kQspiRefRcg + (c.dfs & 1 ? 0x1c : 0x4));
  c.m = rd(kQspiRefRcg + (c.dfs & 1 ? 0x5c : 0x8));
  c.n = rd(kQspiRefRcg + (c.dfs & 1 ? 0x9c : 0xc));
  c.divider = rd(kSe2Divider);
  c.sourceHz = rcgRate(c.cfg, c.m, c.n);
  c.seHz = c.sourceHz / ((c.divider & 0xf) + 1);
  return c;
}

// The GENI divider for a requested SPI rate: the smallest that does not exceed it (oversampling 1 on this QUP version).
inline uint32_t geniDivider(uint32_t seHz, uint32_t wantHz)
{
  if (!seHz || !wantHz) return 0;
  const uint32_t div = (seHz + wantHz - 1) / wantHz;
  return div > 0xfff ? 0 : div;
}

// ----------------------------------------------------------------------------------------------------- SPI by hand (GPIO)
// Mode 0, MSB first: MOSI set while CLK is low, MISO sampled after CLK rises, CLK low again; CS low around the whole transfer.
// The GPIO transport is slow (about 1 us a bit with a barrier after every pin write): enough for the ID, the download and frames
// at a few per second, not for 120 Hz (Project-SunUEFI measured 45 ms for a BOE frame).
struct PinIo {
  void *ctx;
  void (*mosi)(void *ctx, bool high);
  void (*clk)(void *ctx, bool high);
  void (*cs)(void *ctx, bool high);
  bool (*miso)(void *ctx);
  void (*delayUs)(void *ctx, unsigned us);
};
inline void bitSpiTransfer(const PinIo &io, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
  io.cs(io.ctx, false);
  io.delayUs(io.ctx, 2);
  for (uint32_t i = 0; i < len; ++i) {
    uint8_t out = tx ? tx[i] : 0, in = 0;
    for (unsigned b = 0; b < 8; ++b) {
      io.mosi(io.ctx, out & 0x80);
      out = uint8_t(out << 1);
      io.clk(io.ctx, true);
      in = uint8_t(in << 1 | (io.miso(io.ctx) ? 1 : 0));
      io.clk(io.ctx, false);
    }
    if (rx) rx[i] = in;
  }
  io.mosi(io.ctx, false);
  io.cs(io.ctx, true);
  io.delayUs(io.ctx, 2);
}

}  // namespace pianotouch
#endif
