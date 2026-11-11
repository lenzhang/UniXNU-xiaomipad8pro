// SPDX-License-Identifier: BSD-2-Clause-Patent
// From the NT36532E's raw THP matrices to one contact on piano's 3200 x 2136 panel, then to a debounced tip (down / move / up).
// Integer only, no allocation, no I/O: tests/touch_host.cpp runs it on synthetic frames. The steps are Project-SunUEFI's
// PianoTouchMatrix.c (UPSTREAM), itself an integer port of piano's Linux touch-view reader:
//   - lock onto the dominant non-pen data type over the first 16 frames with a good checksum (pen types 6, 7, 9, 0x1d never);
//   - the reference is the per-node median of the next 32 frames of that type;
//   - each later frame: delta = raw - reference, then the per-column median and the per-row median removed (line noise);
//     while no node is near the threshold the reference follows the raw values by 1/8 (drift);
//   - peaks: nodes >= threshold (200) that no 3 x 3 neighbour beats (ties to the first in scan order), each the weighted centroid
//     of its positive 3 x 3 neighbourhood (1/1024 node);
//   - the contact nearest the last one (within 400 px) while touching, else the strongest peak;
//   - screen: X along the 60 columns (3200 px), Y along the 40 rows (2136 px), Y flipped (piano's raw Y runs up the panel).
// The tip on top: down after `confirm` frames in a row with a contact (ShengNovatekTouch confirms on the second scan), up after
// `hold` frames in a row without one, so a single noisy frame neither clicks nor breaks a drag. No calibration maps (the per-unit
// edge compensation of piano's Ubuntu THP stays private and is not used here): the mapping is linear over the matrix.
#ifndef PIANO_TOUCH_MATRIX_H
#define PIANO_TOUCH_MATRIX_H
#include <stdint.h>
#include "Nt36532.h"

namespace pianotouch {

constexpr uint32_t kMaxNodes = 4096, kRefFrames = 32, kProbeFrames = 16, kMaxPoints = 10;
constexpr int32_t kPanelW = 3200, kPanelH = 2136;

struct MatrixConfig {
  int32_t threshold = 200, trackPx = 400;
  bool flipX = false, flipY = true;
};

enum class Feed : uint8_t { badChecksum, skipped, learning, otherType, decoded };

struct Matrix {
  MatrixConfig cfg;
  int32_t refType = -1;
  uint32_t probeCount = 0, probeTypes[256] = {}, refCount = 0, rows = 0, cols = 0, nodes = 0;
  bool haveReference = false;
  int16_t samples[kRefFrames][kMaxNodes];
  int32_t reference[kMaxNodes], delta[kMaxNodes];
  int32_t maxDelta = 0, peaks = 0;
  bool touching = false;
  int32_t x = 0, y = 0;            // panel pixels
  uint32_t frames = 0, badChecksum = 0, skipped = 0, otherType = 0;
  uint16_t lastFrameNo = 0;
};

inline void matrixReset(Matrix &m, const MatrixConfig &cfg)
{
  uint8_t *p = reinterpret_cast<uint8_t *>(&m);
  for (size_t i = 0; i < sizeof m; ++i) p[i] = 0;
  m.cfg = cfg;
  m.refType = -1;
}

namespace detail {
inline void sort(int32_t *v, uint32_t n)
{
  for (uint32_t i = 1; i < n; ++i) {
    const int32_t x = v[i];
    uint32_t j = i;
    while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; --j; }
    v[j] = x;
  }
}
struct Point {
  int64_t x, y;   // 1/1024 node
  int32_t peak;
};
inline void toPanel(const Matrix &m, const Point &p, int32_t *x, int32_t *y)
{
  int64_t u = p.x * kPanelW / (int64_t(m.cols) * 1024), v = p.y * kPanelH / (int64_t(m.rows) * 1024);
  if (m.cfg.flipX) u = kPanelW - u;
  if (m.cfg.flipY) v = kPanelH - v;
  *x = int32_t(u < 0 ? 0 : u >= kPanelW ? kPanelW - 1 : u);
  *y = int32_t(v < 0 ? 0 : v >= kPanelH ? kPanelH - 1 : v);
}
}  // namespace detail

// p: the THP packet (frame + kTransportBytes), len its bytes. decoded: touching / x / y are this frame's.
inline Feed matrixFeed(Matrix &m, const uint8_t *p, uint32_t len)
{
  using namespace detail;
  if (len < 64) { ++m.skipped; return Feed::skipped; }
  if (packetChecksum(p, len) <= 0) { ++m.badChecksum; return Feed::badChecksum; }
  const uint32_t type = p[56], cols = p[48], rows = p[49], nodes = rows * cols;
  if (!nodes || nodes > kMaxNodes || 64 + uint64_t(nodes) * 2 > len || rows > 256 || cols > 256) { ++m.skipped; return Feed::skipped; }
  m.lastFrameNo = le16(p + 28);
  if (m.refType < 0) {
    if (type == 6 || type == 7 || type == 9 || type == 0x1d) return Feed::learning;
    ++m.probeTypes[type];
    if (++m.probeCount < kProbeFrames) return Feed::learning;
    for (uint32_t n = 0; n < 256; ++n)
      if (m.refType < 0 || m.probeTypes[n] > m.probeTypes[m.refType]) m.refType = int32_t(n);
    return Feed::learning;
  }
  if (int32_t(type) != m.refType) { ++m.otherType; return Feed::otherType; }
  if (!m.haveReference) {
    if (!m.refCount) { m.rows = rows; m.cols = cols; m.nodes = nodes; }
    if (rows != m.rows || cols != m.cols) { ++m.skipped; return Feed::skipped; }
    for (uint32_t n = 0; n < nodes; ++n) m.samples[m.refCount][n] = int16_t(le16(p + 64 + n * 2));
    if (++m.refCount >= kRefFrames) {
      int32_t column[kRefFrames];
      for (uint32_t n = 0; n < m.nodes; ++n) {
        for (uint32_t k = 0; k < m.refCount; ++k) column[k] = m.samples[k][n];
        sort(column, m.refCount);
        m.reference[n] = column[m.refCount / 2];
      }
      m.haveReference = true;
    }
    return Feed::learning;
  }
  if (rows != m.rows || cols != m.cols) { ++m.skipped; return Feed::skipped; }
  for (uint32_t n = 0; n < nodes; ++n) m.delta[n] = int16_t(le16(p + 64 + n * 2)) - m.reference[n];
  int32_t line[256];
  for (uint32_t c = 0; c < cols; ++c) {
    for (uint32_t r = 0; r < rows; ++r) line[r] = m.delta[r * cols + c];
    sort(line, rows);
    const int32_t common = line[rows / 2];
    for (uint32_t r = 0; r < rows; ++r) m.delta[r * cols + c] -= common;
  }
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < cols; ++c) line[c] = m.delta[r * cols + c];
    sort(line, cols);
    const int32_t common = line[cols / 2];
    for (uint32_t c = 0; c < cols; ++c) m.delta[r * cols + c] -= common;
  }
  m.maxDelta = 0;
  for (uint32_t n = 0; n < nodes; ++n)
    if (m.delta[n] > m.maxDelta) m.maxDelta = m.delta[n];
  if (m.maxDelta < m.cfg.threshold / 2)
    for (uint32_t n = 0; n < nodes; ++n) m.reference[n] += (int16_t(le16(p + 64 + n * 2)) - m.reference[n]) / 8;
  Point pts[kMaxPoints];
  uint32_t count = 0;
  for (int32_t r = 0; r < int32_t(rows); ++r)
    for (int32_t c = 0; c < int32_t(cols); ++c) {
      const int32_t v = m.delta[r * int32_t(cols) + c];
      if (v < m.cfg.threshold) continue;
      bool peak = true;
      for (int32_t dr = -1; dr <= 1 && peak; ++dr)
        for (int32_t dc = -1; dc <= 1; ++dc) {
          const int32_t rr = r + dr, cc = c + dc;
          if ((!dr && !dc) || rr < 0 || cc < 0 || rr >= int32_t(rows) || cc >= int32_t(cols)) continue;
          const int32_t w = m.delta[rr * int32_t(cols) + cc];
          if (w > v || (w == v && (dr < 0 || (dr == 0 && dc < 0)))) { peak = false; break; }
        }
      if (!peak) continue;
      int64_t sw = 0, sx = 0, sy = 0;
      for (int32_t dr = -1; dr <= 1; ++dr)
        for (int32_t dc = -1; dc <= 1; ++dc) {
          const int32_t rr = r + dr, cc = c + dc;
          if (rr < 0 || cc < 0 || rr >= int32_t(rows) || cc >= int32_t(cols)) continue;
          const int32_t w = m.delta[rr * int32_t(cols) + cc];
          if (w <= 0) continue;
          sw += w;
          sx += int64_t(w) * (cc * 1024 + 512);
          sy += int64_t(w) * (rr * 1024 + 512);
        }
      if (count < kMaxPoints && sw > 0) pts[count++] = Point{sx / sw, sy / sw, v};
    }
  ++m.frames;
  m.peaks = int32_t(count);
  if (!count) {
    m.touching = false;
    return Feed::decoded;
  }
  int32_t bestX = 0, bestY = 0, bestPeak = -1;
  bool found = false;
  int64_t bestD = int64_t(m.cfg.trackPx) * m.cfg.trackPx;
  for (uint32_t i = 0; i < count; ++i) {
    int32_t x, y;
    toPanel(m, pts[i], &x, &y);
    if (m.touching) {
      const int64_t d = int64_t(x - m.x) * (x - m.x) + int64_t(y - m.y) * (y - m.y);
      if (d < bestD) { bestD = d; bestX = x; bestY = y; found = true; }
    } else if (pts[i].peak > bestPeak) {
      bestPeak = pts[i].peak; bestX = x; bestY = y; found = true;
    }
  }
  if (!found)
    for (uint32_t i = 0; i < count; ++i)
      if (pts[i].peak > bestPeak) { bestPeak = pts[i].peak; toPanel(m, pts[i], &bestX, &bestY); found = true; }
  m.touching = found;
  m.x = bestX;
  m.y = bestY;
  return Feed::decoded;
}

// ----------------------------------------------------------------------------------------------------------------- the tip
enum class TipEvent : uint8_t { none, down, move, up };
struct Tip {
  unsigned confirm = 2, hold = 3;
  bool down = false;
  int32_t x = 0, y = 0;
  unsigned seen = 0, missed = 0;
  uint32_t downs = 0, ups = 0;
};
// One decoded frame: touching and its position. Returns what changed for the pointer.
inline TipEvent tipUpdate(Tip &t, bool touching, int32_t x, int32_t y)
{
  if (touching) {
    t.missed = 0;
    if (!t.down) {
      if (++t.seen < t.confirm) return TipEvent::none;
      t.down = true;
      t.x = x;
      t.y = y;
      ++t.downs;
      return TipEvent::down;
    }
    if (x == t.x && y == t.y) return TipEvent::none;
    t.x = x;
    t.y = y;
    return TipEvent::move;
  }
  t.seen = 0;
  if (!t.down || ++t.missed < t.hold) return TipEvent::none;
  t.down = false;
  t.missed = 0;
  ++t.ups;
  return TipEvent::up;
}
// Lost frames (a transport error, a re-download): the tip goes up at once if it was down.
inline TipEvent tipCancel(Tip &t)
{
  t.seen = t.missed = 0;
  if (!t.down) return TipEvent::none;
  t.down = false;
  ++t.ups;
  return TipEvent::up;
}

}  // namespace pianotouch
#endif
