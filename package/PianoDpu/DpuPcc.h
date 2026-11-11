// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu B1d: Night Light (macOS's Night Shift and colour filters) in the DSPPs' PCC (docs/drivers/dpu.md §4.4), portable
// (tests/dpu_pcc_host.cpp). No floating point: the kernel has none.
//
// The PCC is a 3x3 matrix in each DSPP (DpuHw.h: S3.15, 0x8000 = 1.0, output R from inputs R,G,B at +0x10/+0x14/+0x18, G at
// +0x1c.., B at +0x28..; enable +0 bit 0), applied to the blended picture before the gamma block, latched with the frame by
// CTL_DSPP_n_FLUSH bit 4 + CTL_FLUSH bit 29. Both halves of the panel (LM0 -> DSPP0, LM1 -> DSPP1) get the same matrix.
//
// What macOS sends (IOMobileFramebufferLegacy, the 27.0.1 ABI's slots 343 and 307; NativeGopDisplay forwards both with
// piano-dpu-pcc=1, after logging their raw bytes: B1d-0):
//   swap_set_color_matrix(IOMFBColorFixedMatrix *, IOMFBColorMatrixFunction, unsigned): the matrix is taken to be 3x3 64-bit
//     S31.32 words, row-major [output][input], 1.0 = 1 << 32: the layout Asahi Linux's DCP driver (GPL-2.0 OR MIT, read for the fact)
//     sends for the same-named type to Apple's display firmware (iomfb_set_matrix_req, identity 1 << 32 on the diagonal). Each
//     word's sign is accepted as two's complement or as sign-magnitude (DRM's CTM convention, which that driver passes through),
//     whichever puts it within the PCC's +-4; a word that is neither refuses the matrix. One matrix is kept per function (the
//     second argument, < 16), and the PCC gets their product (in function order): Night Shift and a colour filter both apply.
//   set_gamma_table(IOMFBGammaTable *): taken to be 3 channels of (1 word + 256 32-bit entries), 0xc0c bytes, the iOS gamma table
//     open-source night-light tools wrote (GammaThingy's data[j + 0x001/0x102/0x203]); fitted only with piano-dpu-pcc=2. Three
//     ramps that are scaled copies of each other are a PCC diagonal exactly (scale of the brightest channel = 1.0).
// The matrix is linear light (a display pipeline applies it between de-gamma and re-gamma); the PCC works on the encoded values. A
// diagonal is exact there: d in linear light is d^(1/2.2) encoded, which encodeDiagonal applies (piano-dpu-pcc-encode=0: as sent).
// Any other matrix is applied as sent (an approximation, logged). Negative coefficients are clamped to 0 unless piano-dpu-pcc-
// negative=1 (msm drops the sign when it converts a CTM, so how the PCC takes a negative S3.15 word is not established).
#pragma once
#include "DpuSlices.h"

namespace pianodpu {
namespace pcc {
using namespace slices;

constexpr int32_t kOne = 1 << 16;   // Q16.16
struct Matrix {
  int32_t m[3][3];   // [output][input], Q16.16
};
constexpr Matrix identity() { return Matrix{{{kOne, 0, 0}, {0, kOne, 0}, {0, 0, kOne}}}; }
inline bool same(const Matrix &a, const Matrix &b)
{
  for (unsigned o = 0; o < 3; ++o)
    for (unsigned i = 0; i < 3; ++i)
      if (a.m[o][i] != b.m[o][i]) return false;
  return true;
}
inline bool isDiagonal(const Matrix &a, int32_t slack = kOne / 256)
{
  for (unsigned o = 0; o < 3; ++o)
    for (unsigned i = 0; i < 3; ++i)
      if (o != i && (a.m[o][i] > slack || a.m[o][i] < -slack)) return false;
  return true;
}
inline bool isIdentity(const Matrix &a) { return same(a, identity()); }
inline Matrix diagonal(int32_t r, int32_t g, int32_t b) { return Matrix{{{r, 0, 0}, {0, g, 0}, {0, 0, b}}}; }
inline int32_t milli(int32_t q16) { return int32_t((int64_t(q16) * 1000 + (q16 >= 0 ? kOne / 2 : -kOne / 2)) / kOne); }
inline int32_t fromMilli(int32_t m) { return int32_t((int64_t(m) * kOne + (m >= 0 ? 500 : -500)) / 1000); }

// ------------------------------------------------------------------------------------------------- macOS's fixed matrix
constexpr uint64_t kS3132Limit = 4ull << 32;   // +-4.0, the PCC's range
struct Decoded {
  bool ok = false;
  const char *why = "";
  Matrix m{};
  unsigned negativeTwos = 0, negativeSignMagnitude = 0;   // how each negative word was read
};
inline Decoded decodeS3132(const uint64_t raw[9])
{
  Decoded d;
  for (unsigned k = 0; k < 9; ++k) {
    const uint64_t v = raw[k];
    uint64_t magnitude;
    bool negative = false;
    if (!(v >> 63)) {
      magnitude = v;
    } else if ((~v + 1) <= kS3132Limit) {   // two's complement
      magnitude = ~v + 1;
      negative = true;
      ++d.negativeTwos;
    } else if ((v & ~(1ull << 63)) <= kS3132Limit) {   // sign-magnitude
      magnitude = v & ~(1ull << 63);
      negative = true;
      ++d.negativeSignMagnitude;
    } else {
      d.why = "a word is outside +-4.0 read either way";
      return d;
    }
    if (magnitude > kS3132Limit) {
      d.why = "a coefficient is larger than 4.0";
      return d;
    }
    const int32_t q16 = int32_t((magnitude + 0x8000u) >> 16);
    d.m.m[k / 3][k % 3] = negative ? -q16 : q16;
  }
  d.ok = true;
  d.why = "ok";
  return d;
}

// a * b (b applied first), Q16.16, rounded, clamped to +-8.
inline Matrix multiply(const Matrix &a, const Matrix &b)
{
  Matrix r{};
  for (unsigned o = 0; o < 3; ++o)
    for (unsigned i = 0; i < 3; ++i) {
      int64_t s = 0;
      for (unsigned k = 0; k < 3; ++k) s += int64_t(a.m[o][k]) * b.m[k][i];
      s = (s + (s >= 0 ? kOne / 2 : -kOne / 2)) / kOne;
      if (s > 8 * kOne) s = 8 * kOne;
      if (s < -8 * kOne) s = -8 * kOne;
      r.m[o][i] = int32_t(s);
    }
  return r;
}

// x^(1/2.2) for x = i/256, Q16.16 (generated: round((i / 256) ** (1 / 2.2) * 65536), i = 0..256).
constexpr uint32_t kEncode[257] = {
  0, 5270, 7222, 8684, 9897, 10953, 11900, 12763, 13562, 14308, 15010, 15674,
  16307, 16911, 17490, 18047, 18585, 19104, 19607, 20094, 20569, 21030, 21479, 21918,
  22346, 22764, 23174, 23575, 23968, 24353, 24731, 25103, 25467, 25826, 26179, 26526,
  26868, 27205, 27537, 27864, 28186, 28504, 28818, 29128, 29434, 29736, 30035, 30330,
  30621, 30910, 31195, 31477, 31756, 32032, 32306, 32576, 32844, 33109, 33372, 33632,
  33890, 34146, 34399, 34650, 34899, 35146, 35391, 35634, 35874, 36113, 36350, 36585,
  36819, 37050, 37280, 37508, 37735, 37960, 38183, 38405, 38625, 38844, 39061, 39277,
  39491, 39704, 39916, 40126, 40335, 40543, 40749, 40954, 41158, 41361, 41563, 41763,
  41962, 42160, 42357, 42553, 42748, 42942, 43135, 43326, 43517, 43707, 43895, 44083,
  44270, 44456, 44641, 44825, 45008, 45190, 45371, 45552, 45731, 45910, 46088, 46265,
  46442, 46617, 46792, 46966, 47139, 47311, 47483, 47654, 47824, 47994, 48162, 48331,
  48498, 48665, 48831, 48996, 49160, 49324, 49488, 49650, 49812, 49974, 50135, 50295,
  50454, 50613, 50772, 50930, 51087, 51243, 51399, 51555, 51710, 51864, 52018, 52171,
  52324, 52476, 52628, 52779, 52930, 53080, 53229, 53378, 53527, 53675, 53823, 53970,
  54116, 54263, 54408, 54554, 54698, 54843, 54987, 55130, 55273, 55416, 55558, 55699,
  55840, 55981, 56122, 56262, 56401, 56540, 56679, 56817, 56955, 57093, 57230, 57367,
  57503, 57639, 57774, 57910, 58044, 58179, 58313, 58447, 58580, 58713, 58845, 58978,
  59110, 59241, 59372, 59503, 59634, 59764, 59893, 60023, 60152, 60281, 60409, 60538,
  60665, 60793, 60920, 61047, 61173, 61300, 61426, 61551, 61677, 61802, 61926, 62051,
  62175, 62299, 62422, 62545, 62668, 62791, 62913, 63035, 63157, 63279, 63400, 63521,
  63641, 63762, 63882, 64002, 64121, 64241, 64360, 64478, 64597, 64715, 64833, 64951,
  65069, 65186, 65303, 65420, 65536,
};
// A linear-light gain in [0, 1] (Q16.16) as the gain of the encoded value, by the table and linear interpolation.
inline int32_t encodeGain(int32_t d)
{
  if (d <= 0) return 0;
  if (d >= kOne) return d;   // gains above 1 stay linear (not a white-point reduction)
  const uint32_t i = uint32_t(d) >> 8, f = uint32_t(d) & 0xffu;
  return int32_t(kEncode[i] + ((kEncode[i + 1] - kEncode[i]) * f + 128) / 256);
}
// The matrix for the encoded space: a diagonal exactly, anything else as it is (approximate = true unless it is the identity).
inline Matrix encode(const Matrix &linear, bool *approximate)
{
  *approximate = false;
  if (!isDiagonal(linear)) {
    *approximate = !isIdentity(linear);
    return linear;
  }
  return diagonal(encodeGain(linear.m[0][0]), encodeGain(linear.m[1][1]), encodeGain(linear.m[2][2]));
}

// ------------------------------------------------------------------------------------------------------- the gamma table
constexpr unsigned kGammaEntries = 256, kGammaChannelWords = 257, kGammaWords = 3 * kGammaChannelWords;   // 0xc0c bytes
struct GammaFit {
  bool ok = false;
  const char *why = "";
  Matrix m = identity();   // a diagonal, encoded space
  unsigned reference = 0;  // the brightest channel (gain 1.0)
  uint32_t worstMilli = 0; // the largest deviation from a scaled copy, thousandths of the reference's top
};
inline GammaFit fitGamma(const uint32_t *w, unsigned words)
{
  GammaFit g;
  if (!w || words < kGammaWords) {
    g.why = "no 0xc0c-byte table";
    return g;
  }
  const uint32_t *ch[3] = {w + 1, w + 1 + kGammaChannelWords, w + 1 + 2 * kGammaChannelWords};
  for (unsigned c = 1; c < 3; ++c)
    if (ch[c][kGammaEntries - 1] > ch[g.reference][kGammaEntries - 1]) g.reference = c;
  const uint32_t *ref = ch[g.reference];
  const uint64_t top = ref[kGammaEntries - 1];
  if (!top) {
    g.why = "every ramp ends at 0";
    return g;
  }
  for (unsigned c = 0; c < 3; ++c)
    for (unsigned k = 1; k < kGammaEntries; ++k)
      if (ch[c][k] < ch[c][k - 1]) {
        g.why = "a ramp is not monotonic";
        return g;
      }
  int32_t gain[3];
  for (unsigned c = 0; c < 3; ++c) {
    uint64_t sc = 0, sr = 0;
    for (unsigned k = 0; k < kGammaEntries; ++k) {
      sc += ch[c][k];
      sr += ref[k];
    }
    gain[c] = sr ? int32_t((sc * kOne + sr / 2) / sr) : 0;
    for (unsigned k = 0; k < kGammaEntries; ++k) {
      const int64_t want = (int64_t(ref[k]) * gain[c] + kOne / 2) / kOne, dev = int64_t(ch[c][k]) - want;
      const uint32_t milli = uint32_t(((dev < 0 ? -dev : dev) * 1000 + top / 2) / top);
      if (milli > g.worstMilli) g.worstMilli = milli;
    }
  }
  if (g.worstMilli > 20) {
    g.why = "the ramps are not scaled copies of each other (a curve, not a white point)";
    return g;
  }
  g.m = diagonal(gain[0], gain[1], gain[2]);
  g.ok = true;
  g.why = "ok";
  return g;
}

// ------------------------------------------------------------------------------------------------------- the PCC words
// Q16.16 -> the PCC's S3.15 in 18 bits (two's complement, as DpuSurvey.h's pccMilli reads it), rounded, saturated at +-4.
struct Words {
  uint32_t w[3][3] = {};
  unsigned clampedNegative = 0, saturated = 0;
};
inline Words pccWords(const Matrix &m, bool allowNegative)
{
  Words r;
  for (unsigned o = 0; o < 3; ++o)
    for (unsigned i = 0; i < 3; ++i) {
      int32_t q = m.m[o][i];
      if (q < 0 && !allowNegative) {
        q = 0;
        ++r.clampedNegative;
      }
      int32_t v = q >= 0 ? (q + 1) / 2 : -((-q + 1) / 2);
      if (v > 0x1ffff) {
        v = 0x1ffff;
        ++r.saturated;
      }
      if (v < -0x20000) {
        v = -0x20000;
        ++r.saturated;
      }
      r.w[o][i] = uint32_t(v) & 0x3ffffu;
    }
  return r;
}

struct Apply {
  bool ok = false;
  const char *why = "";
  Words words;
  unsigned dspps = 0, stores = 0, refused = 0;
  int32_t latchUs = -1;
  uint32_t enable[kDsppCount] = {};
};
// The matrix (encoded space) into every DSPP of the path, enable, flush, latch, read back. The engines must run (the latch is a
// frame start): the caller keeps a matrix that arrives while the panel sleeps and applies it at the wake.
template <class Io>
Apply apply(Io &io, const LivePath &p, const Matrix &encoded, bool allowNegative)
{
  Apply a;
  a.dspps = p.dspps;
  if (!p.ok || p.ctl < 0) {
    a.why = p.why;
    return a;
  }
  if (!a.dspps) {
    a.why = "the CTL's LMs have no DSPP (LM4..7)";
    return a;
  }
  a.words = pccWords(encoded, allowNegative);
  auto wr = [&](uint64_t pa, uint32_t v) {
    ++a.stores;
    if (!io.wr(pa, v)) ++a.refused;
  };
  for (unsigned d = 0; d < kDsppCount; ++d) {
    if (!((a.dspps >> d) & 1u)) continue;
    for (unsigned o = 0; o < 3; ++o)
      for (unsigned i = 0; i < 3; ++i) wr(pccReg(d, kPccCoef[o][i]), a.words.w[o][i]);
    wr(pccReg(d, 0), 1);
  }
  for (unsigned d = 0; d < kDsppCount; ++d)
    if ((a.dspps >> d) & 1u) wr(ctlReg(unsigned(p.ctl), kCtlDspp0Flush + 4 * d), kDsppFlushPcc);
  wr(ctlReg(unsigned(p.ctl), kCtlFlush), kFlushDspp);
  a.latchUs = waitFlush(io, unsigned(p.ctl), kFlushDspp, 60000);
  bool same = true;
  for (unsigned d = 0; d < kDsppCount; ++d) {
    if (!((a.dspps >> d) & 1u)) continue;
    a.enable[d] = io.rd(pccReg(d, 0));
    same &= (a.enable[d] & 1u) != 0;
    for (unsigned o = 0; o < 3; ++o)
      for (unsigned i = 0; i < 3; ++i) same &= (io.rd(pccReg(d, kPccCoef[o][i])) & 0x3ffffu) == a.words.w[o][i];
  }
  if (a.refused) a.why = "the store guard refused a store";
  else if (a.latchUs < 0) a.why = "CTL_FLUSH bit 29 did not clear within 60 ms (are the engines running?)";
  else if (!same) a.why = "a PCC word did not read back";
  else {
    a.ok = true;
    a.why = "ok";
  }
  return a;
}

// ----------------------------------------------------------------------------------------------------- what macOS has set
// One matrix per IOMFBColorMatrixFunction value (0..15), the product applied; and the gamma table's fit.
class State {
 public:
  static constexpr unsigned kFunctions = 16;
  // A matrix for a function: false when the function is out of range. The identity clears the slot.
  bool setMatrix(unsigned function, const Matrix &m)
  {
    if (function >= kFunctions) return false;
    set_ &= ~(1u << function);
    if (!isIdentity(m)) {
      slots_[function] = m;
      set_ |= 1u << function;
    }
    return true;
  }
  void setGamma(const Matrix &diag) { gamma_ = diag; }
  // The linear-light matrix (the functions' product); the gamma fit is separate (it is in the encoded space already).
  Matrix linear() const
  {
    Matrix r = identity();
    for (unsigned f = 0; f < kFunctions; ++f)
      if ((set_ >> f) & 1u) r = multiply(slots_[f], r);
    return r;
  }
  // What the PCC gets: the functions' product (encoded, unless encodeDiag is off) times the gamma diagonal.
  Matrix pcc(bool encodeDiag, bool *approximate) const
  {
    const Matrix l = linear();
    Matrix e = l;
    *approximate = !isIdentity(l);   // linear light applied to encoded values ...
    if (encodeDiag) e = encode(l, approximate);   // ... except a diagonal, encoded exactly
    return multiply(gamma_, e);
  }
  unsigned functions() const { return set_; }

 private:
  Matrix slots_[kFunctions] = {};
  Matrix gamma_ = identity();
  uint32_t set_ = 0;
};

}  // namespace pcc
}  // namespace pianodpu
