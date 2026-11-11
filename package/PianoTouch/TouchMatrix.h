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
//   - panel position: X along the 60 columns (3200 px), Y along the 40 rows (2136 px).
// The position, with this unit's calibration (Calibration, generated at build time from the private Ubuntu file, never committed):
// piano-thp's (xiaomipad-8pro-mainline device/piano-thp, Apache-2.0, "Modified from xiaomi-sheng-thp by ianchb"; UPSTREAM)
// domainContact over the peak's 3 x 3 nodes: the average of column_map / row_map (electrode centres, pixels x 10) weighted by
// max(delta - 25, 0), rounded; then mapEdgeAxis on each axis (the edge stretch: edge_size, edge_gain / edge_base; Y with its low
// edge bias), then the orientation, then / 10 to pixels. Without one: linear over the matrix (node centres at (i + 1/2) / n).
// Orientation (piano-thp orientation.hpp, MIT): X never flipped; Y as processor Y on a BOE panel, reversed on a CSOT one, unless
// the calibration says otherwise (invert_y); the caller picks flipX / flipY (PianoTouch.cpp: piano-touch-flip=auto).
// The tip on top: down after `confirm` frames in a row with a contact (ShengNovatekTouch confirms on the second scan), up after
// `hold` frames in a row without one, so a single noisy frame neither clicks nor breaks a drag.
#ifndef PIANO_TOUCH_MATRIX_H
#define PIANO_TOUCH_MATRIX_H
#include <stdint.h>
#include "Nt36532.h"

namespace pianotouch {

constexpr uint32_t kMaxNodes = 4096, kRefFrames = 32, kProbeFrames = 16, kMaxPoints = 10;
constexpr int32_t kPanelW = 3200, kPanelH = 2136;

// This unit's electrode calibration (the private Ubuntu file's fields, pixels x 10). Aggregate: the generated header initialises it.
struct Calibration {
  bool present;
  int32_t rowMap[40], columnMap[60];   // electrode centres, panel pixels (x 10 in the formulas)
  int32_t rowMaximum, columnMaximum;   // pixels x 10 (21360, 32000 on piano)
  int32_t edgeBase, edgeSize, edgeGain;
  int32_t projectVendorId;             // the stock thp_config.ini's: 26 BOE, 4 CSOT; -1 not recorded
  int32_t invertY;                     // -1 the panel's default, 0 / 1 the file's override
};
constexpr int32_t kUnits = 10, kCoordinateReduce = 25, kVendorBoe = 26, kVendorCsot = 4;

// piano-thp's Calibration::validate: maxima in 1..32767, strictly increasing maps inside them, edge sanity.
inline bool calibrationValid(const Calibration &c)
{
  if (!c.present) return false;
  auto axis = [](const int32_t *map, unsigned n, int32_t maximum) {
    if (maximum <= 0 || maximum > 32767) return false;
    for (unsigned i = 0; i < n; ++i) {
      if (map[i] < 0 || int64_t(map[i]) * kUnits >= maximum) return false;
      if (i && map[i] <= map[i - 1]) return false;
    }
    return true;
  };
  return axis(c.rowMap, 40, c.rowMaximum) && axis(c.columnMap, 60, c.columnMaximum) && c.edgeBase > 0 && c.edgeSize > 0 &&
         c.edgeGain >= 0 && int64_t(c.edgeSize) * 2 < (c.rowMaximum < c.columnMaximum ? c.rowMaximum : c.columnMaximum);
}

// piano-thp's mapEdgeAxis: within edge_size of either end the position is stretched toward that end.
inline int32_t mapEdgeAxis(int32_t value, int32_t maximum, bool lowEdgeBias, const Calibration &c)
{
  if (value < c.edgeSize) {
    int64_t mapped = int64_t(c.edgeGain) * (c.edgeSize - value) / c.edgeBase;
    if (mapped > c.edgeSize - 1) mapped = c.edgeSize - 1;
    return c.edgeSize - int32_t(mapped) - (lowEdgeBias ? 1 : 0);
  }
  const int32_t start = maximum - c.edgeSize;
  if (value > start) {
    int64_t mapped = int64_t(c.edgeGain) * (value - start) / c.edgeBase;
    if (mapped > c.edgeSize - 1) mapped = c.edgeSize - 1;
    return start + int32_t(mapped);
  }
  return value;
}

// piano-thp's domainContact over the 3 x 3 nodes around a peak at (r, c) of a 40 x 60 row-major delta matrix: the electrode
// positions (pixels x 10) weighted by max(delta - 25, 0), rounded to the nearest unit; a peak at or below the offset gives its own
// electrode. Before the edge mapping and the orientation.
inline void calibratedCentroid(const Calibration &cal, const int32_t *delta, int32_t r, int32_t c, int32_t *x10, int32_t *y10)
{
  int64_t w = 0, sx = 0, sy = 0;
  for (int32_t rr = r - 1; rr <= r + 1; ++rr)
    for (int32_t cc = c - 1; cc <= c + 1; ++cc) {
      if (rr < 0 || cc < 0 || rr >= 40 || cc >= 60 || delta[rr * 60 + cc] <= kCoordinateReduce) continue;
      const int64_t k = delta[rr * 60 + cc] - kCoordinateReduce;
      w += k;
      sx += k * cal.columnMap[cc] * kUnits;
      sy += k * cal.rowMap[rr] * kUnits;
    }
  if (!w) {
    *x10 = cal.columnMap[c] * kUnits;
    *y10 = cal.rowMap[r] * kUnits;
    return;
  }
  const int64_t x = (2 * sx + w) / (2 * w), y = (2 * sy + w) / (2 * w);
  *x10 = int32_t(x >= cal.columnMaximum ? cal.columnMaximum - 1 : x);
  *y10 = int32_t(y >= cal.rowMaximum ? cal.rowMaximum - 1 : y);
}

struct MatrixConfig {
  int32_t threshold = 200, trackPx = 400;
  bool flipX = false, flipY = false;
  const Calibration *cal = nullptr;   // used when present and valid, else the linear map
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
  bool calibrated = false;         // cfg.cal is used
  uint32_t frames = 0, badChecksum = 0, skipped = 0, otherType = 0;
  uint16_t lastFrameNo = 0;
};

inline void matrixReset(Matrix &m, const MatrixConfig &cfg)
{
  uint8_t *p = reinterpret_cast<uint8_t *>(&m);
  for (size_t i = 0; i < sizeof m; ++i) p[i] = 0;
  m.cfg = cfg;
  m.refType = -1;
  m.calibrated = cfg.cal && calibrationValid(*cfg.cal);
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
  int64_t x, y;     // 1/1024 node (the linear map)
  int32_t peak;
  int32_t cx, cy;   // with a calibration: pixels x 10, before the edge mapping and the orientation
};
inline int32_t clampTo(int64_t v, int32_t size) { return int32_t(v < 0 ? 0 : v >= size ? size - 1 : v); }
inline void toPanel(const Matrix &m, const Point &p, int32_t *x, int32_t *y)
{
  if (m.calibrated) {
    const Calibration &c = *m.cfg.cal;
    int32_t u = mapEdgeAxis(p.cx, c.columnMaximum, false, c), v = mapEdgeAxis(p.cy, c.rowMaximum, true, c);
    if (m.cfg.flipX) u = c.columnMaximum - 1 - u;
    if (m.cfg.flipY) v = c.rowMaximum - 1 - v;
    *x = clampTo(u / kUnits, kPanelW);
    *y = clampTo(v / kUnits, kPanelH);
    return;
  }
  int64_t u = p.x * kPanelW / (int64_t(m.cols) * 1024), v = p.y * kPanelH / (int64_t(m.rows) * 1024);
  if (m.cfg.flipX) u = kPanelW - u;
  if (m.cfg.flipY) v = kPanelH - v;
  *x = clampTo(u, kPanelW);
  *y = clampTo(v, kPanelH);
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
    if (!m.refCount) {
      m.rows = rows;
      m.cols = cols;
      m.nodes = nodes;
      if (rows != 40 || cols != 60) m.calibrated = false;   // the maps are piano's 40 x 60
    }
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
      if (count < kMaxPoints && sw > 0) {
        Point pt{sx / sw, sy / sw, v, 0, 0};
        if (m.calibrated) calibratedCentroid(*m.cfg.cal, m.delta, r, c, &pt.cx, &pt.cy);
        pts[count++] = pt;
      }
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

// ----------------------------------------------------------------------------------------------------------- orientation
// piano-touch-flip: 0..3 explicit (bit 0 X, bit 1 Y); anything else is auto. Auto follows the panel (piano-thp orientation.hpp:
// BOE as processor Y, CSOT reversed, X never flipped) or the calibration's invert_y. The panel for orientation is the calibration's
// (project_vendor_id 26 BOE, 4 CSOT), else the gpio100 one, else CSOT (piano-thp's assumption without a panel report); the
// firmware image always follows gpio100. mismatch: gpio100 and the calibration name different panels.
enum : int { kPanelUnknown = -1, kPanelCsot = 0, kPanelBoe = 1 };
struct Orientation {
  bool flipX = false, flipY = false, mismatch = false;
  int panel = kPanelCsot, calibrationPanel = kPanelUnknown;
  const char *why = "";
};
inline const char *panelName(int panel) { return panel == kPanelBoe ? "BOE" : panel == kPanelCsot ? "CSOT" : "unknown"; }
inline Orientation chooseOrientation(int gpioPanel, const Calibration *cal, uint32_t flipArg)
{
  Orientation o;
  const bool valid = cal && calibrationValid(*cal);
  if (valid) o.calibrationPanel = cal->projectVendorId == kVendorBoe ? kPanelBoe : cal->projectVendorId == kVendorCsot ? kPanelCsot : kPanelUnknown;
  o.mismatch = gpioPanel != kPanelUnknown && o.calibrationPanel != kPanelUnknown && gpioPanel != o.calibrationPanel;
  o.panel = o.calibrationPanel != kPanelUnknown ? o.calibrationPanel : gpioPanel != kPanelUnknown ? gpioPanel : kPanelCsot;
  if (flipArg <= 3) {
    o.flipX = flipArg & 1;
    o.flipY = flipArg & 2;
    o.why = "piano-touch-flip";
  } else if (valid && cal->invertY >= 0) {
    o.flipY = cal->invertY != 0;
    o.why = "the calibration's invert_y";
  } else {
    o.flipY = o.panel != kPanelBoe;
    o.why = o.panel == kPanelBoe ? "BOE panel: as processor Y" : "CSOT panel: Y reversed";
  }
  return o;
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
