// SPDX-License-Identifier: BSD-2-Clause-Patent
// From the NT36532E's raw THP matrices to one contact on piano's 3200 x 2136 panel, then to a debounced tip (down / move / up).
// Integer only, no allocation, no I/O: tests/touch_host.cpp runs it on synthetic frames. The steps are Project-SunUEFI's
// PianoTouchMatrix.c (UPSTREAM), itself an integer port of piano's Linux touch-view reader:
//   - lock onto the dominant non-pen data type over the first 16 frames with a good checksum (pen types 6, 7, 9, 0x1d never);
//   - the reference is the per-node median of the next 32 frames of that type;
//   - each later frame: delta = raw - reference, then the per-column median and the per-row median removed (line noise);
//     while no node is near the threshold the reference follows the raw values by 1/8 (drift);
//   - peaks: nodes >= threshold (200) that no 3 x 3 neighbour beats (ties to the first in scan order), each the weighted centroid
//     of its positive 3 x 3 neighbourhood (1/1024 node), widened toward a stronger neighbour's side (centroidWindow, new here);
//   - the contact nearest the last one (within 400 px) while touching, else the strongest peak;
//   - panel position: X along the 60 columns (3200 px), Y along the 40 rows (2136 px).
// The position, with this unit's calibration (Calibration, generated at build time from the private Ubuntu file, never committed):
// piano-thp's (xiaomipad-8pro-mainline device/piano-thp, Apache-2.0, "Modified from xiaomi-sheng-thp by ianchb"; UPSTREAM)
// domainContact over the peak's 3 x 3 nodes (here centroidWindow's): the average of column_map / row_map (electrode centres,
// pixels x 10) weighted by max(delta - 25, 0), rounded; then mapEdgeAxis on each axis (the edge stretch: edge_size, edge_gain /
// edge_base; Y with its low edge bias), then the orientation, then / 10 to pixels. Without one: linear over the matrix (node
// centres at (i + 1/2) / n).
// Orientation (piano-thp orientation.hpp, MIT): X never flipped; Y as processor Y on a BOE panel, reversed on a CSOT one, unless
// the calibration says otherwise (invert_y) or the build gives this unit's own (PIANO_TOUCH_INVERT_Y, before both); the caller
// picks flipX / flipY (PianoTouch.cpp: piano-touch-flip=auto).
// The tip on top: down after `confirm` frames in a row with a contact (ShengNovatekTouch confirms on the second scan), up after
// `hold` frames in a row without one, so a single noisy frame neither clicks nor breaks a drag.
// Every contact of the frame as well (points, new for the multi-contact tracker TouchTrack.h): the peaks down to lowPercent of the
// threshold (a contact already followed continues on them, only one at the threshold starts one: the hysteresis); two peaks two
// nodes apart with no dip between them (the node between at saddlePercent of the weaker or more) are one contact; each peak's blob,
// the 8-connected nodes at blobPercent of the threshold around it, gives the contact's size (nodes), its peak count and its
// bounding box, which the palm rejection uses. At most kMaxPoints points, the strongest first and at most kBlobPoints per blob (a
// palm's many peaks cannot crowd out a finger). The single contact above is unchanged: the strongest or nearest of the points at
// the threshold.
#ifndef PIANO_TOUCH_MATRIX_H
#define PIANO_TOUCH_MATRIX_H
#include <stdint.h>
#include "Nt36532.h"

namespace pianotouch {

constexpr uint32_t kMaxNodes = 4096, kRefFrames = 32, kProbeFrames = 16, kMaxPoints = 16, kMaxCandidates = 64, kBlobPoints = 4;
constexpr uint32_t kMaxBlobs = 254;   // blob labels 1..254; 255 collects every blob past them (counted as one, large)
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

// The centroid's window around a peak at (r, c): its 3 x 3 nodes, widened on each axis by one node toward the strictly stronger
// neighbour when that node is at most half of the neighbour (this contact's own tail, not the flank of another one beside it). A
// contact between two electrodes, whose peak node flickers between them from frame to frame, so keeps one window and one position
// (with the plain 3 x 3 window it jumped by about 15 px each time); on a node, and at the matrix's border, the window is the 3 x 3.
struct CentroidWindow {
  int32_t r0, r1, c0, c1;
};
inline CentroidWindow centroidWindow(const int32_t *delta, int32_t rows, int32_t cols, int32_t r, int32_t c)
{
  auto at = [&](int32_t rr, int32_t cc) { return delta[rr * cols + cc]; };
  CentroidWindow w{r - 1, r + 1, c - 1, c + 1};
  if (c > 0 && c + 1 < cols) {
    const int32_t left = at(r, c - 1), right = at(r, c + 1);
    if (right > left && c + 2 < cols && 2 * int64_t(at(r, c + 2)) <= right) w.c1 = c + 2;
    else if (left > right && c >= 2 && 2 * int64_t(at(r, c - 2)) <= left) w.c0 = c - 2;
  }
  if (r > 0 && r + 1 < rows) {
    const int32_t up = at(r - 1, c), down = at(r + 1, c);
    if (down > up && r + 2 < rows && 2 * int64_t(at(r + 2, c)) <= down) w.r1 = r + 2;
    else if (up > down && r >= 2 && 2 * int64_t(at(r - 2, c)) <= up) w.r0 = r - 2;
  }
  if (w.r0 < 0) w.r0 = 0;
  if (w.c0 < 0) w.c0 = 0;
  if (w.r1 >= rows) w.r1 = rows - 1;
  if (w.c1 >= cols) w.c1 = cols - 1;
  return w;
}

// piano-thp's domainContact over the nodes around a peak at (r, c) of a 40 x 60 row-major delta matrix (its 3 x 3 nodes, here in
// centroidWindow's window): the electrode positions (pixels x 10) weighted by max(delta - 25, 0), rounded to the nearest unit; a
// peak at or below the offset gives its own electrode. Before the edge mapping and the orientation.
inline void calibratedCentroid(const Calibration &cal, const int32_t *delta, int32_t r, int32_t c, int32_t *x10, int32_t *y10)
{
  int64_t w = 0, sx = 0, sy = 0;
  const CentroidWindow win = centroidWindow(delta, 40, 60, r, c);
  for (int32_t rr = win.r0; rr <= win.r1; ++rr)
    for (int32_t cc = win.c0; cc <= win.c1; ++cc) {
      if (delta[rr * 60 + cc] <= kCoordinateReduce) continue;
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
  int32_t lowPercent = 60;      // points down to this share of the threshold (continuing contacts only, TouchTrack.h)
  int32_t blobPercent = 50;     // a blob: the connected nodes at this share of the threshold
  int32_t saddlePercent = 80;   // two peaks two nodes apart: one contact unless the node between dips below this share of the weaker
  bool flipX = false, flipY = false;
  const Calibration *cal = nullptr;   // used when present and valid, else the linear map
};
inline int32_t lowLevel(const MatrixConfig &c) { const int32_t v = c.threshold * c.lowPercent / 100; return v < 1 ? 1 : v > c.threshold ? c.threshold : v; }
inline int32_t blobLevel(const MatrixConfig &c) { const int32_t v = c.threshold * c.blobPercent / 100; return v < 1 ? 1 : v; }

enum class Feed : uint8_t { badChecksum, skipped, learning, otherType, decoded };

// One contact of the last decoded frame (Matrix::points), TouchTrack.h's input.
struct MatrixPoint {
  int32_t x, y;               // panel pixels, as the single contact's
  int32_t peak;               // the peak's delta
  uint16_t row, col;          // the peak's node
  uint16_t area;              // nodes of its blob (0xffff at most)
  uint8_t blobPeaks;          // peaks in that blob (after the saddle merge)
  bool strong;                // peak >= threshold: may start a contact
  uint8_t r0, c0, r1, c1;     // the blob's bounding box (nodes)
};
struct MatrixBlob {
  uint16_t area;
  uint8_t peaks, taken;
  uint8_t r0, c0, r1, c1;
};

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
  // Every contact of the last decoded frame (strongest first), and its blobs.
  MatrixPoint points[kMaxPoints];
  uint32_t pointCount = 0, blobCount = 0, blobNodes = 0, merged = 0;   // merged: peaks merged since the reset
  MatrixBlob blobs[kMaxBlobs + 2];
  uint8_t label[kMaxNodes];
  uint16_t queue[kMaxNodes];
  // matrixFeed's scratch (here, not on a 16 KiB kernel stack): the peak candidates.
  struct Candidate {
    int32_t r, c, v;
    bool dropped;
  };
  Candidate cand[kMaxCandidates];
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
  const int32_t R = int32_t(rows), C = int32_t(cols);
  auto at = [&](int32_t r, int32_t c) { return m.delta[r * C + c]; };
  auto inside = [&](int32_t r, int32_t c) { return r >= 0 && c >= 0 && r < R && c < C; };
  // Blobs: the 8-connected nodes at blobLevel, labelled in scan order (breadth first through m.queue).
  const int32_t low = lowLevel(m.cfg), level = blobLevel(m.cfg);
  m.blobCount = m.blobNodes = 0;
  m.blobs[kMaxBlobs + 1] = MatrixBlob{};
  for (uint32_t n = 0; n < nodes; ++n) m.label[n] = 0;
  for (uint32_t n = 0; n < nodes; ++n) {
    if (m.label[n] || m.delta[n] < level) continue;
    const uint8_t id = m.blobCount < kMaxBlobs ? uint8_t(++m.blobCount) : uint8_t(kMaxBlobs + 1);
    MatrixBlob &b = m.blobs[id];
    if (id <= kMaxBlobs || !b.area) b = MatrixBlob{0, 0, 0, uint8_t(n / cols), uint8_t(n % cols), uint8_t(n / cols), uint8_t(n % cols)};
    uint32_t head = 0, tail = 0;
    m.label[n] = id;
    m.queue[tail++] = uint16_t(n);
    while (head < tail) {
      const uint32_t node = m.queue[head++];
      const int32_t r = int32_t(node / cols), c = int32_t(node % cols);
      if (b.area < 0xffff) ++b.area;
      ++m.blobNodes;
      if (r < b.r0) b.r0 = uint8_t(r);
      if (r > b.r1) b.r1 = uint8_t(r);
      if (c < b.c0) b.c0 = uint8_t(c);
      if (c > b.c1) b.c1 = uint8_t(c);
      for (int32_t dr = -1; dr <= 1; ++dr)
        for (int32_t dc = -1; dc <= 1; ++dc) {
          const int32_t rr = r + dr, cc = c + dc;
          if (!inside(rr, cc) || m.label[rr * C + cc] || at(rr, cc) < level) continue;
          m.label[rr * C + cc] = id;
          m.queue[tail++] = uint16_t(rr * C + cc);
        }
    }
  }
  // Peaks: nodes at the low level that no 3 x 3 neighbour beats (ties to the first in scan order).
  using Candidate = Matrix::Candidate;
  Candidate *cand = m.cand;
  uint32_t nc = 0;
  for (int32_t r = 0; r < R; ++r)
    for (int32_t c = 0; c < C; ++c) {
      const int32_t v = at(r, c);
      if (v < low) continue;
      bool peak = true;
      for (int32_t dr = -1; dr <= 1 && peak; ++dr)
        for (int32_t dc = -1; dc <= 1; ++dc) {
          const int32_t rr = r + dr, cc = c + dc;
          if ((!dr && !dc) || !inside(rr, cc)) continue;
          const int32_t w = at(rr, cc);
          if (w > v || (w == v && (dr < 0 || (dr == 0 && dc < 0)))) { peak = false; break; }
        }
      if (peak && nc < kMaxCandidates) cand[nc++] = Candidate{r, c, v, false};
    }
  // Two peaks two nodes apart without a dip between them are one contact (the weaker goes; m.merged counts them).
  for (uint32_t i = 0; i < nc; ++i)
    for (uint32_t j = i + 1; j < nc && !cand[i].dropped; ++j) {
      if (cand[j].dropped) continue;
      const int32_t dr = cand[j].r - cand[i].r, dc = cand[j].c - cand[i].c;
      const int32_t ar = dr < 0 ? -dr : dr, ac = dc < 0 ? -dc : dc;
      if ((ar > ac ? ar : ac) != 2) continue;
      // The nodes between them: the middle row (two rows apart) or both rows (one apart), the same for the columns.
      const int32_t rA = ar == 2 ? cand[i].r + dr / 2 : cand[i].r, rB = ar == 2 ? rA : cand[j].r;
      const int32_t cA = ac == 2 ? cand[i].c + dc / 2 : cand[i].c, cB = ac == 2 ? cA : cand[j].c;
      const int32_t rs[2] = {rA, rB}, cs[2] = {cA, cB};
      int32_t between = INT32_MIN;
      for (unsigned a = 0; a < 2; ++a)
        for (unsigned b = 0; b < 2; ++b) {
          const int32_t r = rs[a], c = cs[b];
          if (!(r == cand[i].r && c == cand[i].c) && !(r == cand[j].r && c == cand[j].c) && at(r, c) > between) between = at(r, c);
        }
      const int32_t weaker = cand[i].v < cand[j].v ? cand[i].v : cand[j].v;
      if (int64_t(between) * 100 < int64_t(m.cfg.saddlePercent) * weaker) continue;
      (cand[j].v > cand[i].v ? cand[i] : cand[j]).dropped = true;
      ++m.merged;
    }
  for (uint32_t i = 0; i < nc; ++i) {
    const uint8_t id = m.label[cand[i].r * C + cand[i].c];
    if (!cand[i].dropped && id && m.blobs[id].peaks < 0xff) ++m.blobs[id].peaks;
  }
  // The strongest first (insertion sort of indices, stable), at most kBlobPoints a blob.
  uint8_t order[kMaxCandidates];
  uint32_t no = 0;
  for (uint32_t i = 0; i < nc; ++i) {
    if (cand[i].dropped) continue;
    uint32_t k = no++;
    while (k > 0 && cand[order[k - 1]].v < cand[i].v) { order[k] = order[k - 1]; --k; }
    order[k] = uint8_t(i);
  }
  Point pts[kMaxPoints];
  uint32_t count = 0, strong = 0;
  for (uint32_t k = 0; k < no && count < kMaxPoints; ++k) {
    const Candidate &cd = cand[order[k]];
    const int32_t r = cd.r, c = cd.c, v = cd.v;
    const uint8_t id = m.label[r * C + c];
    if (id && m.blobs[id].taken >= kBlobPoints) continue;
    int64_t sw = 0, sx = 0, sy = 0;
    const CentroidWindow win = centroidWindow(m.delta, R, C, r, c);
    for (int32_t rr = win.r0; rr <= win.r1; ++rr)
      for (int32_t cc = win.c0; cc <= win.c1; ++cc) {
        const int32_t w = at(rr, cc);
        if (w <= 0) continue;
        sw += w;
        sx += int64_t(w) * (cc * 1024 + 512);
        sy += int64_t(w) * (rr * 1024 + 512);
      }
    if (sw <= 0) continue;
    if (id) ++m.blobs[id].taken;
    Point pt{sx / sw, sy / sw, v, 0, 0};
    if (m.calibrated) calibratedCentroid(*m.cfg.cal, m.delta, r, c, &pt.cx, &pt.cy);
    MatrixPoint &mp = m.points[count];
    toPanel(m, pt, &mp.x, &mp.y);
    mp.peak = v;
    mp.row = uint16_t(r);
    mp.col = uint16_t(c);
    mp.strong = v >= m.cfg.threshold;
    const MatrixBlob b = id ? m.blobs[id] : MatrixBlob{1, 1, 0, uint8_t(r), uint8_t(c), uint8_t(r), uint8_t(c)};
    mp.area = b.area;
    mp.blobPeaks = b.peaks;
    mp.r0 = b.r0;
    mp.c0 = b.c0;
    mp.r1 = b.r1;
    mp.c1 = b.c1;
    pts[count++] = pt;
    strong += mp.strong;
  }
  m.pointCount = count;
  ++m.frames;
  // The single contact: of the points at the threshold, the nearest to the last one while touching, else the strongest.
  m.peaks = int32_t(strong);
  if (!strong) {
    m.touching = false;
    return Feed::decoded;
  }
  int32_t bestX = 0, bestY = 0, bestPeak = -1;
  bool found = false;
  int64_t bestD = int64_t(m.cfg.trackPx) * m.cfg.trackPx;
  for (uint32_t i = 0; i < count; ++i) {
    if (!m.points[i].strong) continue;
    const int32_t x = m.points[i].x, y = m.points[i].y;
    if (m.touching) {
      const int64_t d = int64_t(x - m.x) * (x - m.x) + int64_t(y - m.y) * (y - m.y);
      if (d < bestD) { bestD = d; bestX = x; bestY = y; found = true; }
    } else if (pts[i].peak > bestPeak) {
      bestPeak = pts[i].peak; bestX = x; bestY = y; found = true;
    }
  }
  if (!found)
    for (uint32_t i = 0; i < count; ++i)
      if (m.points[i].strong && pts[i].peak > bestPeak) { bestPeak = pts[i].peak; bestX = m.points[i].x; bestY = m.points[i].y; found = true; }
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
// invertY: this unit's Y orientation from the build (gen-touch-calibration.py, PIANO_TOUCH_INVERT_Y; -1 none), taken before the
// calibration's invert_y and the panel: piano's Ubuntu needed a per-device invert_y on a BOE unit, and this BOE unit's Y ran
// reversed with the panel's default (2026-11-11, level 2 on the tablet).
inline Orientation chooseOrientation(int gpioPanel, const Calibration *cal, uint32_t flipArg, int32_t invertY = -1)
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
  } else if (invertY >= 0) {
    o.flipY = invertY != 0;
    o.why = invertY ? "this unit: Y reversed (PIANO_TOUCH_INVERT_Y=1 at build)" : "this unit: as processor Y (PIANO_TOUCH_INVERT_Y=0 at build)";
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
