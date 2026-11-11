// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoDpu B1d (DpuPcc.h):
//   - macOS's fixed matrix: S31.32 words, both sign conventions, the +-4 limit, rounding to Q16.16;
//   - the matrix product, the linear-to-encoded gain table against pow(x, 1 / 2.2), the diagonal kept exact, others flagged;
//   - the PCC's S3.15 words: rounding, saturation, negative coefficients clamped unless allowed;
//   - the stores on the register model: both DSPPs of the path identical, enable, CTL_DSPP0/1_FLUSH bit 4, CTL_FLUSH bit 29,
//     latched at the next frame, read back, nothing else written; the survey's decoder reads the words back as written;
//   - what macOS has set: one matrix per function, their product, the identity clearing a slot, the gamma fit (scaled ramps =
//     a diagonal; curves, non-monotonic or empty tables refused).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_pcc_host.cpp -o /tmp/dpu_pcc_host && /tmp/dpu_pcc_host
#include "DpuPcc.h"
#include "dpu_model.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pianodpu;
using namespace pianodpu::pcc;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static uint64_t s3132(double v)   // two's complement
{
  return uint64_t(int64_t(std::llround(v * 4294967296.0)));
}
static uint64_t signMagnitude(double v)
{
  const uint64_t m = uint64_t(std::llround(std::fabs(v) * 4294967296.0));
  return v < 0 ? m | (1ull << 63) : m;
}

static void testDecode()
{
  uint64_t raw[9] = {1ull << 32, 0, 0, 0, 1ull << 32, 0, 0, 0, 1ull << 32};
  Decoded d = decodeS3132(raw);
  CHECK(d.ok && isIdentity(d.m));
  raw[4] = s3132(0.5);
  raw[8] = s3132(0.45);
  d = decodeS3132(raw);
  CHECK(d.ok && d.m.m[1][1] == 32768 && d.m.m[2][2] == 29491 && isDiagonal(d.m));   // 0.45 * 65536 = 29491.2
  raw[1] = s3132(-0.25);                          // output R from input G, two's complement
  raw[5] = signMagnitude(-0.125);                 // output G from input B, sign-magnitude (DRM's CTM)
  d = decodeS3132(raw);
  CHECK(d.ok && d.m.m[0][1] == -16384 && d.m.m[1][2] == -8192 && d.negativeTwos == 1 && d.negativeSignMagnitude == 1);
  CHECK(!isDiagonal(d.m));
  raw[1] = s3132(4.0);
  CHECK(decodeS3132(raw).ok);                     // exactly 4.0 is in range
  raw[1] = s3132(4.5);
  CHECK(!decodeS3132(raw).ok);
  raw[1] = s3132(-4.5);
  CHECK(!decodeS3132(raw).ok);
  raw[1] = 0x8000000500000000ull;                 // -5 sign-magnitude
  CHECK(!decodeS3132(raw).ok);
  raw[1] = 0x7fffffffffffffffull;
  CHECK(!decodeS3132(raw).ok);
  raw[1] = 0x8000;                                // half a Q16.16 step: rounds up
  CHECK(decodeS3132(raw).ok && decodeS3132(raw).m.m[0][1] == 1);
  raw[1] = 0x7fff;
  CHECK(decodeS3132(raw).m.m[0][1] == 0);
  raw[1] = 1ull << 63;                            // -0 in sign-magnitude
  CHECK(decodeS3132(raw).ok && decodeS3132(raw).m.m[0][1] == 0);
}

static void testMath()
{
  const Matrix a = diagonal(kOne, fromMilli(730), fromMilli(450)), b = diagonal(fromMilli(500), kOne, fromMilli(500));
  const Matrix ab = multiply(a, b);
  CHECK(std::abs(ab.m[0][0] - fromMilli(500)) <= 1 && std::abs(ab.m[2][2] - fromMilli(225)) <= 1 && isDiagonal(ab));
  CHECK(same(multiply(identity(), a), a) && same(multiply(a, identity()), a));
  Matrix p{{{kOne, kOne / 2, 0}, {0, kOne, 0}, {0, 0, kOne}}}, q{{{kOne, 0, 0}, {kOne / 4, kOne, 0}, {0, 0, 2 * kOne}}};
  const Matrix pq = multiply(p, q);   // [[1.125, .5, 0], [.25, 1, 0], [0, 0, 2]]
  CHECK(pq.m[0][0] == kOne + kOne / 8 && pq.m[0][1] == kOne / 2 && pq.m[1][0] == kOne / 4 && pq.m[2][2] == 2 * kOne);
  CHECK(milli(fromMilli(730)) == 730 && milli(-kOne / 2) == -500);
  // the gain table: pow(d, 1/2.2), monotonic, ends exact
  CHECK(encodeGain(0) == 0 && encodeGain(kOne) == kOne && encodeGain(kOne / 2) == 47824 && encodeGain(-5) == 0);
  CHECK(encodeGain(2 * kOne) == 2 * kOne);   // a gain above 1 is not a white-point reduction: kept
  int32_t last = -1;
  double worst = 0;
  for (int32_t d = 0; d <= kOne; d += 37) {
    const int32_t e = encodeGain(d);
    CHECK(e >= last);
    last = e;
    if (d >= kOne / 50) {
      const double want = std::pow(double(d) / kOne, 1 / 2.2), got = double(e) / kOne;
      worst = std::fmax(worst, std::fabs(got - want) / want);
    }
  }
  CHECK(worst < 0.005);
  // a Night Shift diagonal, linear 1.0 / 0.73 / 0.45 -> encoded 1.0 / 0.867 / 0.696, exact
  bool approximate = true;
  const Matrix e = encode(diagonal(kOne, fromMilli(730), fromMilli(450)), &approximate);
  CHECK(!approximate && e.m[0][0] == kOne && std::abs(milli(e.m[1][1]) - 867) <= 1 && std::abs(milli(e.m[2][2]) - 696) <= 1);
  const Matrix f = encode(p, &approximate);
  CHECK(approximate && same(f, p));
  encode(identity(), &approximate);
  CHECK(!approximate);
}

static void testWords()
{
  Words w = pccWords(identity(), false);
  CHECK(w.w[0][0] == 0x8000 && w.w[1][1] == 0x8000 && w.w[2][2] == 0x8000 && w.w[0][1] == 0 && w.w[2][0] == 0);
  CHECK(w.clampedNegative == 0 && w.saturated == 0);
  w = pccWords(diagonal(kOne / 2, 3, 4 * kOne), false);
  CHECK(w.w[0][0] == 0x4000 && w.w[1][1] == 2 && w.w[2][2] == 0x1ffff && w.saturated == 1);   // 3/2 rounds to 2; 4.0 saturates
  Matrix n = identity();
  n.m[0][1] = -kOne / 2;
  w = pccWords(n, false);
  CHECK(w.w[0][1] == 0 && w.clampedNegative == 1);
  w = pccWords(n, true);
  CHECK(w.w[0][1] == 0x3c000 && w.clampedNegative == 0 && pccMilli(w.w[0][1]) == -500);
  n.m[0][1] = -8 * kOne;
  CHECK(pccWords(n, true).w[0][1] == 0x20000 && pccWords(n, true).saturated == 1);
}

static void testApply()
{
  model::Dpu m;
  m.sleepMs(3);
  const LivePath p = readPath(m);
  CHECK(p.ok && p.dspps == 0x3);
  const size_t before = m.stores.size();
  Apply a = apply(m, p, identity(), false);
  CHECK(a.ok && a.refused == 0 && a.dspps == 0x3 && a.latchUs >= 0 && a.latchUs < 9000);
  CHECK(m.stores.size() - before == 2 * 10 + 3 && a.stores == 23);
  for (unsigned d = 0; d < 2; ++d) {
    CHECK(m.pccActive[d][0] == 1 && m.pccActive[d][0x10 / 4] == 0x8000 && m.pccActive[d][0x20 / 4] == 0x8000 &&
          m.pccActive[d][0x30 / 4] == 0x8000 && m.pccActive[d][0x14 / 4] == 0);
    CHECK(m.pccActive[d][1] == 0 && m.pccActive[d][2] == 0 && m.pccActive[d][3] == 0);   // the constants are not written
  }
  CHECK(m.storesTo(pccReg(2, 0)) == 0 && m.storesTo(pccReg(3, 0x10)) == 0);             // DSPP2/3 untouched
  CHECK(m.firstStore(ctlReg(0, kCtlDspp0Flush), kDsppFlushPcc) >= 0 && m.firstStore(ctlReg(0, kCtlDspp0Flush + 4), kDsppFlushPcc) >= 0);
  CHECK(m.firstStore(ctlReg(0, kCtlFlush), kFlushDspp) > m.firstStore(ctlReg(0, kCtlDspp0Flush + 4), kDsppFlushPcc));
  CHECK(m.firstStore(pccReg(0, 0), 1) > m.firstStore(pccReg(0, 0x30), 0x8000));   // the matrix before the enable
  // a warm white point with an off-diagonal term: both halves the same, read back by the survey's decoder as written
  Matrix warm = diagonal(kOne, fromMilli(867), fromMilli(696));
  warm.m[0][1] = kOne / 8;
  a = apply(m, p, warm, false);
  CHECK(a.ok);
  for (unsigned d = 0; d < 2; ++d) {
    DsppRegs r{};
    for (unsigned k = 0; k < 13; ++k) r.pcc[k] = m.pccActive[d][k];
    const PccView v = viewPcc(r);
    CHECK(v.enabled && !v.identity && v.m[0][0] == 1000 && v.m[0][1] == 125 && v.m[1][0] == 0 && v.m[1][1] == 867 && v.m[2][2] == 696);
  }
  CHECK(std::memcmp(m.pccActive[0], m.pccActive[1], sizeof m.pccActive[0]) == 0);
  CHECK(m.glitches == 0 && m.refused.empty());
  CHECK(!m.anyStoreIn(kTlmm, 0x200000) && !m.anyStoreIn(kDsi[0], 0x3000) && !m.anyStoreIn(kMdp + kIntfBase, 0x4000));
  {   // engines stopped: the flush cannot latch (the caller keeps the matrix for the wake)
    model::Dpu s;
    s.sleepMs(2);
    const LivePath q = readPath(s);
    s.wr(intfReg(1, kIntfEn), 0);
    s.wr(intfReg(2, kIntfEn), 0);
    s.sleepMs(20);
    const Apply b = apply(s, q, identity(), false);
    CHECK(!b.ok && std::strstr(b.why, "bit 29"));
    s.wr(intfReg(1, kIntfEn), 1);   // the start latches what was pending
    s.wr(intfReg(2, kIntfEn), 1);
    CHECK(s.pccActive[0][0] == 1 && s.ctlFlush == 0);
  }
  {   // a path whose LMs have no DSPP: refused, nothing written
    model::Dpu s;
    s.regs[ctlReg(0, kCtlLayerActive)] = 0x30;   // LM4 + LM5
    s.sleepMs(2);
    const LivePath q = readPath(s);
    CHECK(q.ok && q.dspps == 0);
    const Apply b = apply(s, q, identity(), false);
    CHECK(!b.ok && s.stores.empty());
  }
  {   // LM2 + LM3: DSPP2 + DSPP3
    model::Dpu s;
    s.regs[ctlReg(0, kCtlLayerActive)] = 0xc;
    s.sleepMs(2);
    const LivePath q = readPath(s);
    CHECK(q.dspps == 0xc && apply(s, q, identity(), false).ok && s.storesTo(pccReg(2, 0)) == 1 && s.storesTo(pccReg(0, 0)) == 0);
  }
}

static void testState()
{
  State s;
  bool approximate = true;
  CHECK(isIdentity(s.linear()) && isIdentity(s.pcc(true, &approximate)) && !approximate);
  CHECK(s.setMatrix(2, diagonal(kOne, fromMilli(730), fromMilli(450))));   // Night Shift (say)
  Matrix filter = identity();
  filter.m[1][1] = kOne / 2;
  CHECK(s.setMatrix(5, filter) && s.functions() == ((1u << 2) | (1u << 5)));
  CHECK(std::abs(milli(s.linear().m[1][1]) - 365) <= 1 && milli(s.linear().m[2][2]) == 450);
  const Matrix pc = s.pcc(true, &approximate);
  CHECK(!approximate && std::abs(milli(pc.m[1][1]) - 633) <= 1);   // 0.365 ^ (1 / 2.2)
  CHECK(s.setMatrix(5, identity()) && s.functions() == (1u << 2));
  CHECK(!s.setMatrix(16, filter));
  Matrix grey{{{kOne / 3, kOne / 3, kOne / 3}, {kOne / 3, kOne / 3, kOne / 3}, {kOne / 3, kOne / 3, kOne / 3}}};
  s.setMatrix(7, grey);
  s.pcc(true, &approximate);
  CHECK(approximate);
  s.setMatrix(7, identity());
  s.setMatrix(2, identity());
  s.setGamma(diagonal(kOne, fromMilli(800), fromMilli(500)));
  const Matrix g = s.pcc(true, &approximate);
  CHECK(!approximate && milli(g.m[1][1]) == 800 && milli(g.m[2][2]) == 500);   // the gamma fit is encoded already
}

static std::vector<uint32_t> table(double r, double g, double b, bool curveB = false)
{
  std::vector<uint32_t> t(kGammaWords, 0);
  const double gain[3] = {r, g, b};
  for (unsigned c = 0; c < 3; ++c) {
    t[c * kGammaChannelWords] = 0x100;   // the channel's first word (unknown meaning): ignored
    for (unsigned k = 0; k < kGammaEntries; ++k) {
      double x = double(k) / 255;
      if (curveB && c == 2) x = x * x;
      t[c * kGammaChannelWords + 1 + k] = uint32_t(std::lround(x * gain[c] * 1023));
    }
  }
  return t;
}

static void testGamma()
{
  std::vector<uint32_t> t = table(1.0, 0.8, 0.5);
  GammaFit f = fitGamma(t.data(), unsigned(t.size()));
  CHECK(f.ok && f.reference == 0 && milli(f.m.m[0][0]) == 1000 && std::abs(milli(f.m.m[1][1]) - 800) <= 2 &&
        std::abs(milli(f.m.m[2][2]) - 500) <= 2 && isDiagonal(f.m));
  t = table(0.6, 0.9, 0.3);   // the brightest channel is the reference
  f = fitGamma(t.data(), unsigned(t.size()));
  CHECK(f.ok && f.reference == 1 && milli(f.m.m[1][1]) == 1000 && std::abs(milli(f.m.m[0][0]) - 667) <= 2);
  t = table(1.0, 1.0, 1.0, true);
  f = fitGamma(t.data(), unsigned(t.size()));
  CHECK(!f.ok && std::strstr(f.why, "scaled copies"));
  t = table(1.0, 1.0, 1.0);
  t[1 + 100] = 0;
  CHECK(!fitGamma(t.data(), unsigned(t.size())).ok);   // not monotonic
  t.assign(kGammaWords, 0);
  CHECK(!fitGamma(t.data(), unsigned(t.size())).ok);
  CHECK(!fitGamma(t.data(), 64).ok && !fitGamma(nullptr, kGammaWords).ok);
  t = table(1.0, 1.0, 1.0);
  f = fitGamma(t.data(), unsigned(t.size()));
  CHECK(f.ok && isIdentity(f.m));
}

int main()
{
  testDecode();
  testMath();
  testWords();
  testApply();
  testState();
  testGamma();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("PianoDpu PCC host tests: all passed\n");
  return 0;
}
