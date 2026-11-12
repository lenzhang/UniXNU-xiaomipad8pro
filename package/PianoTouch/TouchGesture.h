// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch's gestures: the tracked contacts (TouchTrack.h, in desktop pixels: the caller maps them through the display's rotation
// first, so every direction below is the desktop's) to the HID pointer reports of TouchHid.h. macOS has no touch screen interface;
// it understands an absolute pointer with buttons, a wheel and AC Pan, so a touch becomes:
//   one finger   lands: the pointer moves there (no button: the first report, as soon as the contact counts);
//                lifted within holdMs, having moved at most slopPx: a left click where it landed (tap);
//                held holdMs within slopPx: a right click there (the menu opens under the finger; the contact then only moves the
//                pointer, its lift sends nothing);
//                moved past slopPx first: a left drag, pressed where it landed, then following the finger, released at the lift;
//                the drag starts only once the finger stayed down dragDelayMs after it first passed slopPx (or went past three
//                times slopPx): lifted before, it is the tap after all (2026-11-12: on this panel a lifting finger's centre moves
//                30-70 px in its last frames, and attend1's short taps became drags: 11 of 16 drags under 310 ms, several ending
//                within 6-32 px of where they started, with slopPx 32);
//   two fingers  (the second seen in minFrames frames while the first was still undecided; the pointer moves between them):
//                both lifted within twoTapMs of the first landing, neither the middle nor the spread moved past slopPx: a right
//                click between them;
//                the middle moved past slopPx: scrolling, one wheel / AC Pan count per scrollPx of the middle's travel, the content
//                following the fingers under macOS's natural scrolling (fingers up = wheel up, right = AC Pan right, as
//                PianoKeyboard's touchpad; scrollInvert flips both), the axis locked when one is at least twice the other at the
//                start (axisLock), the pointer kept where it was so the window under it scrolls; it ends when a finger lifts;
//                the spread changed past slopPx and more than the middle moved: a pinch, recognised and by default sending nothing
//                (pinch 0); pinch 1 holds Command and pinch 2 Control while one wheel count goes per pinchPx of spread (apart: up,
//                zoom in); the modifier is released when it ends;
//   three or more fingers, or a followed contact that turns out a palm: whatever was going on ends (a held button released, a
//                modifier released) and nothing more happens until every finger has lifted.
// A tap within doubleTapMs and doubleTapPx of the previous tap lands exactly where that one did (macOS counts a double click only
// for clicks at nearly the same place). Frames lost (gestureCancel): everything released at once.
// gestures = false is the plain pointer of before (the first contact is the tip: down presses, moves drag, up releases).
// Integer only, no allocation, no I/O: tests/touch_host.cpp runs it on synthetic frames.
#ifndef PIANO_TOUCH_GESTURE_H
#define PIANO_TOUCH_GESTURE_H
#include <stdint.h>
#include "TouchHid.h"
#include "TouchTrack.h"

namespace pianotouch {

constexpr unsigned kMaxReports = 8, kMaxGestureNotes = 4;

struct GestureContact {
  uint8_t id;
  bool palm;
  int32_t x, y;      // desktop pixels
  uint32_t frames;   // frames it was seen in (TouchTrack.h Track::frames; a contact coasting through a dropout keeps its count)
};

struct GestureConfig {
  bool gestures = true;
  uint32_t holdMs = 500;         // still this long: a right click (0: never)
  int32_t slopPx = 64;           // movement that makes a drag, a scroll or a pinch (~4.7 mm; the sensor's nodes are 53 px apart)
  uint32_t dragDelayMs = 80;     // past slopPx this long before the drag starts; a lift before is a tap (0: at once)
  uint32_t twoTapMs = 700;       // a two-finger tap: every finger lifted within this of the first landing
  unsigned minFrames = 2;        // frames a contact needs for a tap, or to count as the second finger
  int32_t scrollPx = 4;          // the middle's travel per wheel / AC Pan count
  bool scrollInvert = false, axisLock = true;
  uint8_t pinch = 0;             // 0 nothing sent, 1 Command + wheel, 2 Control + wheel
  int32_t pinchPx = 16;          // spread change per wheel count while pinching
  uint32_t doubleTapMs = 500;
  int32_t doubleTapPx = 48;
};

enum class GestureMode : uint8_t { idle, one, drag, held, two, scroll, pinch, inert, plain };
enum class GestureKind : uint8_t {
  tap, holdRight, dragStart, dragEnd, twoTap, scrollStart, scrollEnd, pinchStart, pinchEnd, cancelPalm, cancelFingers, cancelLost,
  plainDown, plainUp
};
// For the log: where (desktop pixels), how long since the first finger landed, and two numbers: tap the farthest the finger got
// from its landing and 1 when that was past slopPx (a drag that did not start); dragEnd distance; scroll start the axis (0 free,
// 1 vertical, 2 horizontal), scroll end wheel and pan counts; pinch end the spread change and the zoom counts.
struct GestureNote {
  GestureKind kind;
  int32_t x, y;
  uint32_t ms;
  int32_t a, b;
};
struct GestureOut {
  PointerReport r[kMaxReports];
  unsigned n = 0;
  GestureNote notes[kMaxGestureNotes];
  unsigned noteCount = 0;
};

struct Gesture {
  GestureConfig cfg;
  GestureMode mode = GestureMode::idle;
  uint8_t buttons = 0, modifiers = 0;
  int32_t px = 0, py = 0;                  // the pointer as last reported
  uint8_t id1 = 0, id2 = 0, cand = 0;      // the first finger, the second, a second not yet counted
  unsigned frames1 = 0, candFrames = 0;
  int32_t landX = 0, landY = 0, anchorX = 0, anchorY = 0;
  uint64_t downMs = 0;
  bool pending = false;                    // one finger past slopPx, the drag not started yet (dragDelayMs)
  uint64_t pendingMs = 0;
  int32_t maxDev = 0;                      // one finger's farthest distance from its landing (the tap's note)
  int32_t c0x = 0, c0y = 0, d0 = 0, lastCx = 0, lastCy = 0, lastD = 0, maxTravel = 0, maxSpread = 0;
  uint8_t axis = 0;
  bool lifting = false;
  int32_t accX = 0, accY = 0, accZ = 0, wheelSum = 0, panSum = 0, zoomSum = 0;
  bool haveTap = false;
  uint64_t lastTapMs = 0;
  int32_t lastTapX = 0, lastTapY = 0;
  uint32_t taps = 0, rights = 0, drags = 0, scrolls = 0, pinches = 0, cancels = 0;
};

inline void gestureReset(Gesture &g, const GestureConfig &cfg)
{
  g = Gesture();
  g.cfg = cfg;
}
inline bool gestureIdle(const Gesture &g) { return g.mode == GestureMode::idle; }

namespace detail {
inline int64_t sq(int64_t v) { return v * v; }
inline int64_t dist2(int32_t ax, int32_t ay, int32_t bx, int32_t by) { return sq(int64_t(ax) - bx) + sq(int64_t(ay) - by); }
inline int32_t isqrt(int64_t v)
{
  if (v <= 0) return 0;
  int64_t r = 0, bit = int64_t(1) << 62;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return int32_t(r);
}
inline int findContact(const GestureContact *v, unsigned n, uint8_t id)
{
  for (unsigned i = 0; i < n; ++i)
    if (v[i].id == id) return int(i);
  return -1;
}
inline void emit(Gesture &g, GestureOut &o, uint8_t buttons, int32_t x, int32_t y, int32_t wheel = 0, int32_t pan = 0)
{
  g.buttons = buttons;
  g.px = x;
  g.py = y;
  if (o.n < kMaxReports) o.r[o.n++] = PointerReport{buttons, x, y, wheel, pan, g.modifiers};
}
inline void gnote(GestureOut &o, GestureKind k, int32_t x, int32_t y, uint64_t ms, int32_t a = 0, int32_t b = 0)
{
  if (o.noteCount < kMaxGestureNotes) o.notes[o.noteCount++] = GestureNote{k, x, y, uint32_t(ms), a, b};
}
// Whole counts of unit out of acc (toward zero), at most 127 a report; the rest stays for the next frame.
inline int32_t takeCounts(int32_t &acc, int32_t unit)
{
  if (unit < 1) unit = 1;
  int32_t n = acc / unit;
  if (n > 127) n = 127;
  if (n < -127) n = -127;
  acc -= n * unit;
  return n;
}
inline bool following(const Gesture &g, uint8_t id)
{
  switch (g.mode) {
  case GestureMode::one: case GestureMode::drag: case GestureMode::held: case GestureMode::plain: return id == g.id1;
  case GestureMode::two: case GestureMode::scroll: case GestureMode::pinch: return id == g.id1 || id == g.id2;
  default: return false;
  }
}
// Ends what is going on: a held button and a held modifier released; nothing more until every finger lifted.
inline void cancelTo(Gesture &g, GestureOut &o, uint64_t now, GestureKind why)
{
  if (g.mode == GestureMode::idle || g.mode == GestureMode::inert) return;
  const bool release = g.buttons || g.modifiers;
  g.modifiers = 0;
  if (release) emit(g, o, 0, g.px, g.py);
  ++g.cancels;
  gnote(o, why, g.px, g.py, now - g.downMs);
  g.mode = GestureMode::inert;
  g.cand = 0;
}
inline void enterTwo(Gesture &g, GestureOut &o, const GestureContact &a, const GestureContact &b)
{
  g.mode = GestureMode::two;
  g.id2 = b.id;
  g.cand = 0;
  g.c0x = g.lastCx = int32_t((int64_t(a.x) + b.x) / 2);
  g.c0y = g.lastCy = int32_t((int64_t(a.y) + b.y) / 2);
  g.d0 = g.lastD = isqrt(dist2(a.x, a.y, b.x, b.y));
  g.maxTravel = g.maxSpread = 0;
  g.lifting = false;
  g.axis = 0;
  g.accX = g.accY = g.accZ = g.wheelSum = g.panSum = g.zoomSum = 0;
  emit(g, o, 0, g.c0x, g.c0y);
}
inline void scrollStep(Gesture &g, GestureOut &o, int32_t cx, int32_t cy)
{
  int32_t dx = cx - g.lastCx, dy = cy - g.lastCy;
  g.lastCx = cx;
  g.lastCy = cy;
  if (g.axis == 1) dx = 0;
  if (g.axis == 2) dy = 0;
  const int32_t s = g.cfg.scrollInvert ? -1 : 1;
  g.accY -= s * dy;   // fingers up (y falling): wheel up
  g.accX += s * dx;   // fingers right: AC Pan right
  const int32_t w = takeCounts(g.accY, g.cfg.scrollPx), p = takeCounts(g.accX, g.cfg.scrollPx);
  if (!w && !p) return;
  g.wheelSum += w;
  g.panSum += p;
  emit(g, o, 0, g.px, g.py, w, p);
}
inline void pinchStep(Gesture &g, GestureOut &o, int32_t d)
{
  g.accZ += d - g.lastD;
  g.lastD = d;
  if (!g.cfg.pinch) {
    g.zoomSum += takeCounts(g.accZ, g.cfg.pinchPx);   // counted for the log only
    return;
  }
  if (!g.modifiers) {
    g.modifiers = g.cfg.pinch == 1 ? kModifierCommand : kModifierControl;
    emit(g, o, 0, g.px, g.py);
  }
  const int32_t z = takeCounts(g.accZ, g.cfg.pinchPx);
  if (!z) return;
  g.zoomSum += z;
  emit(g, o, 0, g.px, g.py, z, 0);
}
inline void endTwoFinger(Gesture &g, GestureOut &o, uint64_t now, unsigned visible)
{
  if (g.mode == GestureMode::scroll) gnote(o, GestureKind::scrollEnd, g.px, g.py, now - g.downMs, g.wheelSum, g.panSum);
  if (g.mode == GestureMode::pinch) {
    if (g.modifiers) {
      g.modifiers = 0;
      emit(g, o, 0, g.px, g.py);
    }
    gnote(o, GestureKind::pinchEnd, g.px, g.py, now - g.downMs, g.lastD - g.d0, g.zoomSum);
  }
  g.mode = visible ? GestureMode::inert : GestureMode::idle;
}
}  // namespace detail

// One frame's active contacts (palms included, flagged) at nowMs: the reports to send (in order) and notes for the log in o.
inline void gestureUpdate(Gesture &g, const GestureContact *all, unsigned count, uint64_t now, GestureOut &o)
{
  using namespace detail;
  o.n = o.noteCount = 0;
  GestureContact v[kMaxTracks];
  unsigned nv = 0;
  bool palmFollowed = false;
  for (unsigned i = 0; i < count; ++i) {
    if (all[i].palm) {
      palmFollowed = palmFollowed || following(g, all[i].id);
      if (all[i].id == g.cand) g.cand = 0;
      continue;
    }
    if (nv < kMaxTracks) v[nv++] = all[i];
  }
  if (palmFollowed) cancelTo(g, o, now, GestureKind::cancelPalm);
  const int32_t slop = g.cfg.slopPx;
  switch (g.mode) {
  case GestureMode::idle: {
    if (!nv) return;
    g.downMs = now;
    if (nv >= 3) {   // three at once: nothing until they lift
      g.mode = GestureMode::inert;
      gnote(o, GestureKind::cancelFingers, v[0].x, v[0].y, 0);
      return;
    }
    const GestureContact &c = v[0];
    g.id1 = c.id;
    g.id2 = 0;
    g.frames1 = c.frames;
    g.landX = g.anchorX = c.x;
    g.landY = g.anchorY = c.y;
    g.pending = false;
    g.maxDev = 0;
    g.cand = nv > 1 ? v[1].id : 0;
    g.candFrames = nv > 1 ? v[1].frames : 0;
    if (g.haveTap && now - g.lastTapMs <= g.cfg.doubleTapMs && dist2(c.x, c.y, g.lastTapX, g.lastTapY) <= sq(g.cfg.doubleTapPx)) {
      g.anchorX = g.lastTapX;
      g.anchorY = g.lastTapY;
    }
    if (!g.cfg.gestures) {
      g.mode = GestureMode::plain;
      emit(g, o, 0, g.anchorX, g.anchorY);
      emit(g, o, kButtonLeft, g.anchorX, g.anchorY);
      gnote(o, GestureKind::plainDown, g.anchorX, g.anchorY, 0);
      return;
    }
    g.mode = GestureMode::one;
    emit(g, o, 0, g.anchorX, g.anchorY);   // the pointer under the finger at once
    return;
  }
  case GestureMode::plain: {
    const int i = findContact(v, nv, g.id1);
    if (i < 0) {
      emit(g, o, 0, g.px, g.py);
      gnote(o, GestureKind::plainUp, g.px, g.py, now - g.downMs);
      g.mode = nv ? GestureMode::inert : GestureMode::idle;
      return;
    }
    if (v[i].x != g.px || v[i].y != g.py) emit(g, o, kButtonLeft, v[i].x, v[i].y);
    return;
  }
  case GestureMode::one: {
    const int i = findContact(v, nv, g.id1);
    if (g.cand && findContact(v, nv, g.cand) < 0) g.cand = 0;
    if (!g.cand)
      for (unsigned k = 0; k < nv; ++k)
        if (v[k].id != g.id1) {
          g.cand = v[k].id;
          break;
        }
    if (g.cand) g.candFrames = v[findContact(v, nv, g.cand)].frames;
    if (i < 0) {   // lifted
      if (g.frames1 >= g.cfg.minFrames && (!g.cfg.holdMs || now - g.downMs < g.cfg.holdMs)) {
        emit(g, o, kButtonLeft, g.anchorX, g.anchorY);
        emit(g, o, 0, g.anchorX, g.anchorY);
        g.haveTap = true;
        g.lastTapMs = now;
        g.lastTapX = g.anchorX;
        g.lastTapY = g.anchorY;
        ++g.taps;
        gnote(o, GestureKind::tap, g.anchorX, g.anchorY, now - g.downMs, g.maxDev, g.pending ? 1 : 0);
      }
      g.pending = false;
      g.mode = nv ? GestureMode::inert : GestureMode::idle;
      g.cand = 0;
      return;
    }
    g.frames1 = v[i].frames;
    if (nv >= 3) {
      ++g.cancels;
      gnote(o, GestureKind::cancelFingers, g.px, g.py, now - g.downMs);
      g.mode = GestureMode::inert;
      return;
    }
    if (g.cand && g.candFrames >= g.cfg.minFrames) {
      enterTwo(g, o, v[i], v[findContact(v, nv, g.cand)]);
      return;
    }
    const GestureContact &c = v[i];
    const int64_t dev2 = dist2(c.x, c.y, g.landX, g.landY);
    const int32_t dev = isqrt(dev2);
    if (dev > g.maxDev) g.maxDev = dev;
    if (dev2 > sq(slop) && !g.pending) {
      g.pending = true;   // past the slop: a drag unless the finger lifts within dragDelayMs (a lifting finger's drift)
      g.pendingMs = now;
    }
    if (g.pending) {
      if (g.cfg.dragDelayMs && now - g.pendingMs < g.cfg.dragDelayMs && dev2 <= sq(int64_t(3) * slop)) return;
      g.pending = false;
      emit(g, o, kButtonLeft, g.anchorX, g.anchorY);   // pressed where it landed
      if (c.x != g.anchorX || c.y != g.anchorY) emit(g, o, kButtonLeft, c.x, c.y);
      g.mode = GestureMode::drag;
      gnote(o, GestureKind::dragStart, g.anchorX, g.anchorY, now - g.downMs);
      return;
    }
    if (g.cfg.holdMs && now - g.downMs >= g.cfg.holdMs && g.frames1 >= g.cfg.minFrames && !g.cand) {
      emit(g, o, kButtonRight, g.anchorX, g.anchorY);
      emit(g, o, 0, g.anchorX, g.anchorY);
      ++g.rights;
      g.mode = GestureMode::held;
      gnote(o, GestureKind::holdRight, g.anchorX, g.anchorY, now - g.downMs);
    }
    return;
  }
  case GestureMode::drag: {
    const int i = findContact(v, nv, g.id1);
    if (i < 0) {
      emit(g, o, 0, g.px, g.py);
      ++g.drags;
      gnote(o, GestureKind::dragEnd, g.px, g.py, now - g.downMs, isqrt(dist2(g.px, g.py, g.anchorX, g.anchorY)));
      g.mode = nv ? GestureMode::inert : GestureMode::idle;
      return;
    }
    if (v[i].x != g.px || v[i].y != g.py) emit(g, o, kButtonLeft, v[i].x, v[i].y);
    return;
  }
  case GestureMode::held: {
    const int i = findContact(v, nv, g.id1);
    if (i < 0) {
      g.mode = nv ? GestureMode::inert : GestureMode::idle;
      return;
    }
    if (dist2(v[i].x, v[i].y, g.landX, g.landY) > sq(slop) && (v[i].x != g.px || v[i].y != g.py)) emit(g, o, 0, v[i].x, v[i].y);
    return;
  }
  case GestureMode::two: {
    const int i1 = findContact(v, nv, g.id1), i2 = findContact(v, nv, g.id2);
    if (g.lifting || i1 < 0 || i2 < 0) {
      g.lifting = true;
      if (nv) return;
      if (now - g.downMs <= g.cfg.twoTapMs && g.maxTravel <= slop && g.maxSpread <= slop) {
        emit(g, o, kButtonRight, g.c0x, g.c0y);
        emit(g, o, 0, g.c0x, g.c0y);
        ++g.rights;
        gnote(o, GestureKind::twoTap, g.c0x, g.c0y, now - g.downMs);
      }
      g.mode = GestureMode::idle;
      return;
    }
    if (nv > 2) {
      ++g.cancels;
      gnote(o, GestureKind::cancelFingers, g.px, g.py, now - g.downMs);
      g.mode = GestureMode::inert;
      return;
    }
    const GestureContact &a = v[i1], &b = v[i2];
    const int32_t cx = int32_t((int64_t(a.x) + b.x) / 2), cy = int32_t((int64_t(a.y) + b.y) / 2);
    const int32_t d = isqrt(dist2(a.x, a.y, b.x, b.y)), travel = isqrt(dist2(cx, cy, g.c0x, g.c0y));
    const int32_t spread = d > g.d0 ? d - g.d0 : g.d0 - d;
    if (travel > g.maxTravel) g.maxTravel = travel;
    if (spread > g.maxSpread) g.maxSpread = spread;
    if (spread > slop && spread > travel) {
      g.mode = GestureMode::pinch;
      ++g.pinches;
      gnote(o, GestureKind::pinchStart, g.px, g.py, now - g.downMs, g.cfg.pinch);
      pinchStep(g, o, d);
      return;
    }
    if (travel > slop) {
      const int32_t ax = cx > g.c0x ? cx - g.c0x : g.c0x - cx, ay = cy > g.c0y ? cy - g.c0y : g.c0y - cy;
      g.axis = !g.cfg.axisLock ? 0 : ay >= 2 * ax ? 1 : ax >= 2 * ay ? 2 : 0;
      g.mode = GestureMode::scroll;
      ++g.scrolls;
      gnote(o, GestureKind::scrollStart, g.px, g.py, now - g.downMs, g.axis);
      scrollStep(g, o, cx, cy);
    }
    return;
  }
  case GestureMode::scroll:
  case GestureMode::pinch: {
    const int i1 = findContact(v, nv, g.id1), i2 = findContact(v, nv, g.id2);
    if (i1 < 0 || i2 < 0 || nv > 2) {
      endTwoFinger(g, o, now, nv);
      return;
    }
    const GestureContact &a = v[i1], &b = v[i2];
    if (g.mode == GestureMode::scroll) scrollStep(g, o, int32_t((int64_t(a.x) + b.x) / 2), int32_t((int64_t(a.y) + b.y) / 2));
    else pinchStep(g, o, isqrt(dist2(a.x, a.y, b.x, b.y)));
    return;
  }
  case GestureMode::inert:
    if (!nv) g.mode = GestureMode::idle;
    return;
  }
}

// Frames lost: everything released at once, the next contact starts afresh.
inline void gestureCancel(Gesture &g, GestureOut &o, uint64_t now)
{
  using namespace detail;
  o.n = o.noteCount = 0;
  const GestureMode was = g.mode;
  g.mode = GestureMode::idle;
  g.cand = 0;
  if (was == GestureMode::idle || was == GestureMode::inert) return;
  if (was == GestureMode::scroll) gnote(o, GestureKind::scrollEnd, g.px, g.py, now - g.downMs, g.wheelSum, g.panSum);
  if (was == GestureMode::pinch) gnote(o, GestureKind::pinchEnd, g.px, g.py, now - g.downMs, g.lastD - g.d0, g.zoomSum);
  const bool release = g.buttons || g.modifiers;
  g.modifiers = 0;
  if (release) emit(g, o, 0, g.px, g.py);
  ++g.cancels;
  gnote(o, GestureKind::cancelLost, g.px, g.py, now - g.downMs);
}

}  // namespace pianotouch
#endif
