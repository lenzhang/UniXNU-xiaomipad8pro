// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch's multi-contact tracker: TouchMatrix.h's points of each decoded frame to contacts with stable IDs, for the gestures
// (TouchGesture.h). New code for piano (UniXNU-piano), integer only, no allocation, no I/O: tests/touch_host.cpp runs it on
// synthetic frames. ShengNovatekTouch's and piano-thp's trackers (UPSTREAM) are not used; the rules here are:
//   - matching: each frame, the pairs (contact, point) within gatePx of where the contact was heading (its last position plus its
//     last per-frame motion, so two fingers that pass each other keep their IDs), nearest first, each contact and point once;
//   - hysteresis: only a point at the threshold (strong) starts a contact, which counts after `confirm` frames in a row (or at once
//     when its first peak reaches fastPeak: the first report one frame sooner); a contact it already follows continues on a weaker
//     point (TouchMatrix.h lowPercent), and ends after `hold` frames in a row without one (a dropout shorter than that keeps it, ID
//     and position), or at the first frame without one once it was last seen liftMs ago (frames that come slowly: the lift is not
//     held back by `hold` slow frames); its position moves only when it moved more than jitterPx (a resting finger sends nothing);
//   - palm rejection: a point whose blob has palmNodes nodes or more, or palmPeaks peaks or more, is a palm. A palm never starts a
//     contact; a contact whose point turns into one is a palm until it lifts (sticky), and so is a new contact inside a recent palm's
//     bounding box (palm zones, palmLingerMs after the palm was last seen there): a palm that lifts shrinks to a fingertip-sized
//     remnant before it goes, and that remnant must not click.
// IDs run 1..255 (0 is none), never two alike at once. Notes per update (a contact down, up, turned palm) are for the log.
#ifndef PIANO_TOUCH_TRACK_H
#define PIANO_TOUCH_TRACK_H
#include <stdint.h>
#include "TouchMatrix.h"

namespace pianotouch {

constexpr unsigned kMaxTracks = 8, kMaxPalmZones = 4, kMaxPairs = kMaxTracks * kMaxPoints;
constexpr int32_t kPalmZoneMargin = 2;   // nodes around a palm's blob that are still the palm's

struct TrackConfig {
  unsigned confirm = 2;          // frames in a row a new contact needs
  unsigned hold = 3;             // frames in a row without it before it ends
  uint32_t liftMs = 40;          // or one frame without it once it was last seen this long ago (slow frames: the lift seen at once)
  int32_t fastPeak = 400;        // a first frame this strong counts at once (0: always `confirm` frames)
  int32_t gatePx = 400;          // the farthest a contact is followed from one frame to the next
  int32_t jitterPx = 2;          // its position changes only past this
  uint32_t palmNodes = 48;       // a blob this large is a palm (0: no palm by size)
  uint32_t palmPeaks = 3;        // a blob with this many peaks is a palm (0: no palm by peaks)
  uint32_t palmLingerMs = 500;   // a palm zone lasts this long after the palm was last seen in it
};

enum class TrackState : uint8_t { free, pending, active };
struct Track {
  TrackState state = TrackState::free;
  uint8_t id = 0;
  bool palm = false, matched = false;
  uint8_t seen = 0, missed = 0;
  int32_t x = 0, y = 0;            // panel pixels, jitter filtered: what the gestures see
  int32_t rawX = 0, rawY = 0, vx = 0, vy = 0;
  int32_t peak = 0, maxPeak = 0;
  uint16_t area = 0, maxArea = 0;
  uint8_t blobPeaks = 0;
  uint32_t frames = 0;             // frames it was matched in
  uint64_t downMs = 0;             // when it became active
  uint64_t seenMs = 0;             // when it was last matched
};
struct PalmZone {
  bool live = false;
  uint8_t r0 = 0, c0 = 0, r1 = 0, c1 = 0;
  uint64_t lastMs = 0;
};

enum class TrackNoteKind : uint8_t { down, up, palm };
struct TrackNote {
  TrackNoteKind kind;
  uint8_t slot;
  Track track;   // the contact as it was at the note (an ended one is freed)
};

struct Tracker {
  TrackConfig cfg;
  Track t[kMaxTracks];
  PalmZone zones[kMaxPalmZones];
  uint8_t nextId = 1;
  uint32_t downs = 0, ups = 0, palms = 0, palmPoints = 0, noSlot = 0, frames = 0;
  TrackNote notes[3 * kMaxTracks];
  // trackerUpdate's scratch (2 KiB, here and not on a 16 KiB kernel stack): the pairs within the gate, nearest first.
  struct Pair {
    int64_t d2;
    uint8_t slot, point;
  };
  Pair pairs[kMaxPairs];
  unsigned noteCount = 0;
};

// In place (no Tracker temporary on the kernel stack): every field's default is zero but the configuration and the next ID.
inline void trackerReset(Tracker &tr, const TrackConfig &cfg)
{
  uint8_t *p = reinterpret_cast<uint8_t *>(&tr);
  for (size_t i = 0; i < sizeof tr; ++i) p[i] = 0;
  tr.cfg = cfg;
  tr.nextId = 1;
}

// Contacts in use (pending or active), active ones, and the active ones that are not palms.
inline unsigned trackerLive(const Tracker &tr)
{
  unsigned n = 0;
  for (const Track &k : tr.t) n += k.state != TrackState::free;
  return n;
}
inline unsigned trackerActive(const Tracker &tr, bool fingersOnly)
{
  unsigned n = 0;
  for (const Track &k : tr.t) n += k.state == TrackState::active && (!fingersOnly || !k.palm);
  return n;
}

namespace detail {
inline void note(Tracker &tr, TrackNoteKind kind, unsigned slot)
{
  if (tr.noteCount < sizeof tr.notes / sizeof tr.notes[0]) tr.notes[tr.noteCount++] = TrackNote{kind, uint8_t(slot), tr.t[slot]};
}
inline bool palmPoint(const TrackConfig &c, const MatrixPoint &p)
{
  return (c.palmNodes && p.area >= c.palmNodes) || (c.palmPeaks && p.blobPeaks >= c.palmPeaks);
}
inline uint8_t widen(int32_t v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
inline bool overlaps(const PalmZone &z, uint8_t r0, uint8_t c0, uint8_t r1, uint8_t c1)
{
  return z.live && r0 <= z.r1 && r1 >= z.r0 && c0 <= z.c1 && c1 >= z.c0;
}
// A palm's blob (widened by the margin) joins the zone it overlaps, else takes a free one, else the oldest.
inline void palmZone(Tracker &tr, const MatrixPoint &p, uint64_t now)
{
  const uint8_t r0 = widen(p.r0 - kPalmZoneMargin), c0 = widen(p.c0 - kPalmZoneMargin), r1 = widen(p.r1 + kPalmZoneMargin),
                c1 = widen(p.c1 + kPalmZoneMargin);
  PalmZone *z = nullptr;
  for (PalmZone &k : tr.zones)
    if (overlaps(k, r0, c0, r1, c1)) { z = &k; break; }
  if (z) {
    if (r0 < z->r0) z->r0 = r0;
    if (c0 < z->c0) z->c0 = c0;
    if (r1 > z->r1) z->r1 = r1;
    if (c1 > z->c1) z->c1 = c1;
    z->lastMs = now;
    return;
  }
  for (PalmZone &k : tr.zones)
    if (!k.live || !z || (z->live && k.lastMs < z->lastMs)) z = &k;
  *z = PalmZone{true, r0, c0, r1, c1, now};
}
inline bool inPalmZone(const Tracker &tr, const MatrixPoint &p)
{
  for (const PalmZone &z : tr.zones)
    if (z.live && p.row >= z.r0 && p.row <= z.r1 && p.col >= z.c0 && p.col <= z.c1) return true;
  return false;
}
inline uint8_t newId(Tracker &tr)
{
  for (;;) {
    const uint8_t id = tr.nextId;
    tr.nextId = uint8_t(tr.nextId == 255 ? 1 : tr.nextId + 1);
    bool used = false;
    for (const Track &k : tr.t) used = used || (k.state != TrackState::free && k.id == id);
    if (!used) return id;
  }
}
inline void activate(Tracker &tr, unsigned slot, uint64_t now)
{
  Track &k = tr.t[slot];
  k.state = TrackState::active;
  k.downMs = now;
  ++tr.downs;
  note(tr, TrackNoteKind::down, slot);
}
inline int32_t absolute(int32_t v) { return v < 0 ? -v : v; }
}  // namespace detail

// One decoded frame: its points (Matrix::points, pointCount) at nowMs.
inline void trackerUpdate(Tracker &tr, const MatrixPoint *pts, unsigned n, uint64_t now)
{
  using namespace detail;
  tr.noteCount = 0;
  ++tr.frames;
  if (n > kMaxPoints) n = kMaxPoints;
  bool palm[kMaxPoints], used[kMaxPoints];
  for (unsigned j = 0; j < n; ++j) {
    palm[j] = palmPoint(tr.cfg, pts[j]);
    used[j] = false;
    if (palm[j]) {
      ++tr.palmPoints;
      palmZone(tr, pts[j], now);
    }
  }
  for (PalmZone &z : tr.zones)
    if (z.live && now - z.lastMs > tr.cfg.palmLingerMs) z.live = false;
  // The pairs within the gate, nearest first.
  Tracker::Pair *pairs = tr.pairs;
  unsigned np = 0;
  for (unsigned s = 0; s < kMaxTracks; ++s) {
    Track &k = tr.t[s];
    k.matched = false;
    if (k.state == TrackState::free) continue;
    const int32_t steps = int32_t(k.missed) + 1;
    const int64_t px = k.rawX + int64_t(k.vx) * steps, py = k.rawY + int64_t(k.vy) * steps, gate = int64_t(tr.cfg.gatePx) * steps;
    for (unsigned j = 0; j < n; ++j) {
      if (k.state == TrackState::pending && !pts[j].strong) continue;
      const int64_t dx = pts[j].x - px, dy = pts[j].y - py, d2 = dx * dx + dy * dy;
      if (d2 > gate * gate) continue;
      unsigned at = np++;
      while (at > 0 && pairs[at - 1].d2 > d2) { pairs[at] = pairs[at - 1]; --at; }
      pairs[at] = Tracker::Pair{d2, uint8_t(s), uint8_t(j)};
    }
  }
  for (unsigned q = 0; q < np; ++q) {
    Track &k = tr.t[pairs[q].slot];
    const unsigned j = pairs[q].point;
    if (k.matched || used[j]) continue;
    k.matched = used[j] = true;
    const MatrixPoint &p = pts[j];
    const int32_t steps = int32_t(k.missed) + 1;
    k.vx = (p.x - k.rawX) / steps;
    k.vy = (p.y - k.rawY) / steps;
    k.rawX = p.x;
    k.rawY = p.y;
    if (absolute(p.x - k.x) > tr.cfg.jitterPx || absolute(p.y - k.y) > tr.cfg.jitterPx) {
      k.x = p.x;
      k.y = p.y;
    }
    k.peak = p.peak;
    if (p.peak > k.maxPeak) k.maxPeak = p.peak;
    k.area = p.area;
    if (p.area > k.maxArea) k.maxArea = p.area;
    k.blobPeaks = p.blobPeaks;
    k.missed = 0;
    k.seenMs = now;
    ++k.frames;
    if (palm[j] && !k.palm) {
      k.palm = true;
      ++tr.palms;
      if (k.state == TrackState::active) note(tr, TrackNoteKind::palm, pairs[q].slot);
    }
    if (k.state == TrackState::pending && ++k.seen >= tr.cfg.confirm) activate(tr, pairs[q].slot, now);
  }
  // Contacts without a point: a pending one was noise; an active one coasts at its place for hold - 1 frames, then ends.
  for (unsigned s = 0; s < kMaxTracks; ++s) {
    Track &k = tr.t[s];
    if (k.state == TrackState::free || k.matched) continue;
    if (k.state == TrackState::pending) {
      k = Track();
      continue;
    }
    if (++k.missed < tr.cfg.hold && !(tr.cfg.liftMs && now - k.seenMs >= tr.cfg.liftMs)) continue;
    ++tr.ups;
    note(tr, TrackNoteKind::up, s);
    k = Track();
  }
  // Points without a contact: a strong one starts one, a palm never (it only marks its zone); inside a palm zone it is the palm's.
  for (unsigned j = 0; j < n; ++j) {
    if (used[j] || !pts[j].strong || palm[j]) continue;
    unsigned s = 0;
    while (s < kMaxTracks && tr.t[s].state != TrackState::free) ++s;
    if (s == kMaxTracks) {
      ++tr.noSlot;
      continue;
    }
    Track &k = tr.t[s];
    k = Track();
    k.state = TrackState::pending;
    k.id = newId(tr);
    k.palm = inPalmZone(tr, pts[j]);
    if (k.palm) ++tr.palms;
    k.seen = 1;
    k.x = k.rawX = pts[j].x;
    k.y = k.rawY = pts[j].y;
    k.peak = k.maxPeak = pts[j].peak;
    k.area = k.maxArea = pts[j].area;
    k.blobPeaks = pts[j].blobPeaks;
    k.frames = 1;
    k.seenMs = now;
    k.matched = true;
    if (tr.cfg.confirm <= 1 || (tr.cfg.fastPeak > 0 && pts[j].peak >= tr.cfg.fastPeak)) activate(tr, s, now);
  }
}

// Frames lost (a transport error, a re-download): every contact ends at once.
inline void trackerCancel(Tracker &tr)
{
  tr.noteCount = 0;
  for (unsigned s = 0; s < kMaxTracks; ++s) {
    Track &k = tr.t[s];
    if (k.state == TrackState::active) {
      ++tr.ups;
      detail::note(tr, TrackNoteKind::up, s);
    }
    k = Track();
  }
  for (PalmZone &z : tr.zones) z.live = false;
}

}  // namespace pianotouch
#endif
