// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera's first software ISP (docs/drivers/camera.md section 4.4, its first version): one RAW10 MIPI-packed frame to 8-bit
// sRGB at a reduced size, on the CPU, in user space (tooling/native/piano-camera-grab; later the camera extension). Plain C (C99 and
// C++), no allocation: the caller gives the work buffer (3 floats per output pixel).
//   each 2x2 Bayer quad is one pixel (R, the mean of its two G, B), `scale` x `scale` quads averaged into one output pixel; the
//   black level subtracted; grey-world white balance (each channel's mean brought to green's, the gains within 1/4..4); a digital
//   gain that puts the 99th percentile of luma at 92 % of full scale (1..16: a dark frame is brightened, its noise with it); the
//   sRGB transfer curve. Output w = width / 2 / scale, h = height / 2 / scale, RGB bytes row by row.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  float mean[3];      // the linear means above black before the gains (R, G, B)
  float gain[3];      // the white balance gains
  float digital;      // the digital gain
  uint32_t width, height;
} PianoIspStats;

// One 10-bit pixel of a MIPI-packed RAW10 line.
static inline uint32_t pianoRaw10(const uint8_t *line, uint32_t x)
{
  const uint8_t *g = line + x / 4 * 5;
  const uint32_t k = x & 3u;
  return (uint32_t)g[k] << 2 | ((uint32_t)(g[4] >> (2 * k)) & 3u);
}

// The channel (0 R, 1 G, 2 B) at quad position (dy, dx) for a Bayer order (PianoCameraABI.h: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR).
static inline int pianoBayerChannel(uint32_t bayer, uint32_t dy, uint32_t dx)
{
  static const int order[4][4] = {{0, 1, 1, 2}, {1, 0, 2, 1}, {1, 2, 0, 1}, {2, 1, 1, 0}};
  return order[bayer & 3u][dy * 2 + dx];
}

static inline uint8_t pianoSrgb8(float v)
{
  if (v <= 0.f) return 0;
  if (v >= 1.f) return 255;
  const float s = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
  return (uint8_t)(s * 255.f + 0.5f);
}

// `work`: 3 floats per output pixel. Returns 0, or -1 for a size that does not fit.
static inline int pianoIsp(const uint8_t *raw, uint32_t width, uint32_t height, uint32_t stride, uint32_t bayer, uint32_t black,
                           uint32_t scale, float *work, uint8_t *rgb, PianoIspStats *st)
{
  if (!scale || width < 4 * scale || height < 2 * scale || stride < width / 4 * 5) return -1;
  const uint32_t ow = width / 2 / scale, oh = height / 2 / scale;
  const float full = (float)(1023u - black);
  double sum[3] = {0, 0, 0};
  for (uint32_t oy = 0; oy < oh; ++oy)
    for (uint32_t ox = 0; ox < ow; ++ox) {
      float c[3] = {0, 0, 0};
      for (uint32_t qy = 0; qy < scale; ++qy) {
        const uint32_t y = (oy * scale + qy) * 2;
        const uint8_t *l0 = raw + (uint64_t)y * stride, *l1 = l0 + stride;
        for (uint32_t qx = 0; qx < scale; ++qx) {
          const uint32_t x = (ox * scale + qx) * 2;
          const uint32_t v[4] = {pianoRaw10(l0, x), pianoRaw10(l0, x + 1), pianoRaw10(l1, x), pianoRaw10(l1, x + 1)};
          for (uint32_t k = 0; k < 4; ++k) {
            const float a = v[k] > black ? (float)(v[k] - black) : 0.f;
            const int ch = pianoBayerChannel(bayer, k / 2, k % 2);
            c[ch] += ch == 1 ? a * 0.5f : a;
          }
        }
      }
      float *w = work + ((uint64_t)oy * ow + ox) * 3;
      for (int ch = 0; ch < 3; ++ch) {
        w[ch] = c[ch] / (float)(scale * scale);
        sum[ch] += w[ch];
      }
    }
  const double n = (double)ow * oh;
  for (int ch = 0; ch < 3; ++ch) st->mean[ch] = (float)(sum[ch] / n);
  for (int ch = 0; ch < 3; ++ch) {
    float g = st->mean[ch] > 0.01f ? st->mean[1] / st->mean[ch] : 1.f;
    st->gain[ch] = g < 0.25f ? 0.25f : g > 4.f ? 4.f : g;
  }
  st->gain[1] = 1.f;
  // the 99th percentile of luma after white balance, from a histogram of 1024 bins over full scale
  uint32_t hist[1024];
  memset(hist, 0, sizeof hist);
  for (uint64_t i = 0; i < (uint64_t)ow * oh; ++i) {
    const float *w = work + i * 3;
    const float y = 0.2126f * w[0] * st->gain[0] + 0.7152f * w[1] + 0.0722f * w[2] * st->gain[2];
    uint32_t b = (uint32_t)(y / full * 1023.f);
    hist[b > 1023 ? 1023 : b]++;
  }
  uint64_t acc = 0, at = (uint64_t)(n * 0.99);
  uint32_t p99 = 1023;
  for (uint32_t b = 0; b < 1024; ++b)
    if ((acc += hist[b]) >= at) {
      p99 = b;
      break;
    }
  float k = 0.92f * 1023.f / (float)(p99 + 1);
  st->digital = k < 1.f ? 1.f : k > 16.f ? 16.f : k;
  for (uint64_t i = 0; i < (uint64_t)ow * oh; ++i)
    for (int ch = 0; ch < 3; ++ch) rgb[i * 3 + ch] = pianoSrgb8(work[i * 3 + ch] * st->gain[ch] * st->digital / full);
  st->width = ow;
  st->height = oh;
  return 0;
}

// A box-averaged thumbnail of an RGB image (tw x th, RGB bytes).
static inline void pianoThumb(const uint8_t *rgb, uint32_t w, uint32_t h, uint32_t tw, uint32_t th, uint8_t *out)
{
  for (uint32_t ty = 0; ty < th; ++ty)
    for (uint32_t tx = 0; tx < tw; ++tx) {
      const uint32_t x0 = tx * w / tw, x1 = (tx + 1) * w / tw, y0 = ty * h / th, y1 = (ty + 1) * h / th;
      uint64_t s[3] = {0, 0, 0}, c = 0;
      for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x, ++c)
          for (int ch = 0; ch < 3; ++ch) s[ch] += rgb[((uint64_t)y * w + x) * 3 + ch];
      for (int ch = 0; ch < 3; ++ch) out[((uint64_t)ty * tw + tx) * 3 + ch] = (uint8_t)(c ? s[ch] / c : 0);
    }
}
