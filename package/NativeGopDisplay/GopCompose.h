// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeGopDisplay's composition and swap pipeline, portable and host-tested (tests/gop_host.cpp): no kernel header, no allocation,
// surfaces are opaque pointers. NativeGopDisplay.cpp is the kernel side: it reads the family's swap request, maps the IOSurfaces, holds
// the references and the lock, and calls these pieces (docs/drivers/native-gop-display.md §5a).
//
//   - Geometry: rectangles, clipping at the panel's edges, the damage of a cursor change (its old and new rectangles).
//   - Pixels: the base formats ('BGRA', 'RGhA' half float, 'b3a8' extended range) into the panel's 0xAARRGGBB, premultiplied 'BGRA'
//     over an opaque pixel, one composed row (the base, black where it has no pixel, the overlay blended over it).
//   - Layers: what a swap does to each of the family's four layers (bit set with a surface: shown; bit set without: off; bit clear:
//     unchanged) and which surfaces it takes off the screen.
//   - The pipeline: the copy thread's one waiting job (newest state wins, a refresh never displaces a swap's job), the job the copy
//     thread is drawing, and the rules that close the race of 2026-11-11: by default a swap completes only once its own job is done
//     (the family hands the surface the swap replaces back to WindowServer at the completion, and WindowServer draws its next frame
//     into it at once; no read of it is left by then); with native-gop-display-defer=1 only a swap that takes a surface off the
//     screen while the copy thread reads it waits. A refresh copy in the way stops at its next row.
//   - The fence rule: a job is drawn only once the GPU retired the fence of its swap (never a frame still being drawn: the previous
//     one stays on the panel); a frame whose wait hit the hang guard is retried on the next ticks.
//   - The panel (copy thread): flipped or not, whether the boot framebuffer holds the composition, where the cursor was drawn; the
//     plan of a job (flip it, compose the whole panel, or only the cursor's old and new rectangles).
//   - Refresh copies: the ticks after a frame (1, 2, 4, 8, 16, 30) and which of them a fence-covered frame still needs.
//   - Damage (2026-11-12): the swap's dirty region (IOMFBSwapRec's, as the family keeps it in the request), so a frame whose GPU work
//     was fence-covered recomposes only what changed since the frame before it; the audit points that check the region against the
//     panel, and the plan's rules for when the whole panel is composed instead.
//   - The panel before the first frame: left to a verbose boot's text console, black otherwise (startPlan).
#pragma once
#include <stdint.h>
#include <string.h>

namespace gopcompose {

static constexpr uint32_t kFormatBGRA = 'BGRA', kFormatRGhA = 'RGhA', kFormatB3A8 = 'b3a8';
// '&BGA': Apple's lossless-compressed spelling of BGRA, one plane. macOS 27.3's WindowServer takes it for its framebuffers on the
// Mac17,11 identity (2026-11-13, ram-k273-desk-dev-s1b: "a frame surface 3200x2136 format 0x26424741 row 3200 ... is not read").
// Nothing here compresses: the Metal plugin's frontend renders a planar surface at its plane's address and rows, so plane 0 holds
// plain BGRA rows (NativeGopDisplay::openSource reads it as such).
static constexpr uint32_t kFormatBGRALossless = '&BGA';
static constexpr uint32_t kBlack = 0xff000000u;
static constexpr unsigned kLayers = 4;         // the family's layers in a swap (IOMFBSwapIORequest: surfaces at 2944 + 8 * i)
static constexpr unsigned kRefreshTicks = 30;  // refresh copies on ticks 1, 2, 4, 8, 16 and 30 after a frame

inline int64_t min64(int64_t a, int64_t b) {return a < b ? a : b;}
inline int64_t max64(int64_t a, int64_t b) {return a > b ? a : b;}

// ------------------------------------------------------------------------------------------------------------------- geometry
struct Rect {
  int32_t x = 0, y = 0, w = 0, h = 0;
  bool empty() const {return w <= 0 || h <= 0;}
  int64_t right() const {return int64_t(x) + w;}
  int64_t bottom() const {return int64_t(y) + h;}
  int64_t area() const {return empty() ? 0 : int64_t(w) * h;}
  bool operator==(const Rect &o) const {return x == o.x && y == o.y && w == o.w && h == o.h;}
};
// [x0, x1) x [y0, y1), empty when either side is; coordinates far outside any panel are clamped first.
inline Rect makeRect(int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
  const int64_t limit = int64_t(1) << 29;   // a width or height up to 2^30 still fits int32_t
  x0 = max64(-limit, min64(limit, x0));
  y0 = max64(-limit, min64(limit, y0));
  x1 = max64(-limit, min64(limit, x1));
  y1 = max64(-limit, min64(limit, y1));
  if (x1 <= x0 || y1 <= y0) return {};
  return {int32_t(x0), int32_t(y0), int32_t(x1 - x0), int32_t(y1 - y0)};
}
inline Rect panelRect(uint32_t width, uint32_t height) {return makeRect(0, 0, width, height);}
inline Rect intersect(const Rect &a, const Rect &b)
{
  if (a.empty() || b.empty()) return {};
  return makeRect(max64(a.x, b.x), max64(a.y, b.y), min64(a.right(), b.right()), min64(a.bottom(), b.bottom()));
}
// The bounding box (an empty side is ignored).
inline Rect unite(const Rect &a, const Rect &b)
{
  if (a.empty()) return b;
  if (b.empty()) return a;
  return makeRect(min64(a.x, b.x), min64(a.y, b.y), max64(a.right(), b.right()), max64(a.bottom(), b.bottom()));
}

// What a cursor change redraws: where the cursor was drawn and where it is now (both on the panel already). One rectangle when they
// overlap or their bounding box is at most twice the two, else the two apart (a jump across the screen redraws 2 x 128 x 128 pixels,
// not the box between them).
struct Damage {
  unsigned count = 0;
  Rect rect[2];
};
inline Damage cursorDamage(const Rect &drawn, const Rect &now)
{
  Damage d;
  if (drawn.empty() && now.empty()) return d;
  if (drawn.empty() || now.empty()) {
    d.rect[d.count++] = drawn.empty() ? now : drawn;
    return d;
  }
  const Rect box = unite(drawn, now);
  if (!intersect(drawn, now).empty() || box.area() <= 2 * (drawn.area() + now.area())) {
    d.rect[d.count++] = box;
    return d;
  }
  d.rect[0] = drawn;
  d.rect[1] = now;
  d.count = 2;
  return d;
}

// --------------------------------------------------------------------------------------------------------------------- pixels
// round(a * b / 255) for a, b in [0, 255], exactly.
inline uint32_t mul255(uint32_t a, uint32_t b)
{
  const uint32_t t = a * b + 128;
  return (t + (t >> 8)) >> 8;
}
// A premultiplied 'BGRA' pixel (the little-endian word 0xAARRGGBB, what WindowServer's cursor surfaces hold) over an opaque panel pixel:
// each channel src + dst * (255 - alpha) / 255, saturated (a non-premultiplied pixel brighter than its alpha cannot wrap); the result
// is opaque. Fully transparent (all zero): dst as it is.
inline uint32_t blendOver(uint32_t dst, uint32_t src)
{
  const uint32_t alpha = src >> 24;
  if (alpha == 255) return src;
  if (!src) return dst;
  const uint32_t inverse = 255 - alpha;
  uint32_t out = kBlack;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    const uint32_t v = (src >> shift & 255) + mul255(dst >> shift & 255, inverse);
    out |= (v > 255 ? 255 : v) << shift;
  }
  return out;
}
inline void blendSpan(uint32_t *dst, const uint32_t *src, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++) dst[i] = blendOver(dst[i], src[i]);
}

// A half float's value in [0, 1] as a byte, in integer arithmetic (no floating point in the kernel).
inline uint8_t halfToByte(uint16_t half)
{
  const unsigned exponent = (half >> 10) & 31, mantissa = half & 1023;
  if (half & 0x8000 || (exponent == 31 && mantissa)) return 0;   // negative or NaN
  if (exponent >= 15) return 255;                                 // 1.0 and above
  const uint32_t scaled = (exponent ? 1024 + mantissa : mantissa) * 255;   // value * 255 = scaled * 2^(exponent - 25), subnormals 2^-24
  const unsigned shift = exponent ? 25 - exponent : 24;
  return static_cast<uint8_t>((scaled + (1u << (shift - 1))) >> shift);
}
// The conversion tables, made once.
struct Tables {
  uint8_t half[16384];   // 'RGhA': indexed by a half float >> 2 (8 bits of mantissa left, finer than a byte's step in [0, 1]);
                         // +infinity shares its index with NaNs and reads 0 (Core Animation's frames hold none)
  uint8_t xr10[1024];    // 'b3a8': a 10-bit extended-range channel (384 is 0.0, 894 is 1.0)
  void init()
  {
    for (uint32_t index = 0; index < 16384; index++) half[index] = halfToByte(static_cast<uint16_t>(index << 2 | 2));
    for (uint32_t value = 0; value < 1024; value++)
      xr10[value] = value <= 384 ? 0 : value >= 894 ? 255 : static_cast<uint8_t>((value - 384) * 255 / 510);
  }
};
inline bool baseFormat(uint32_t format) {return format == kFormatBGRA || format == kFormatRGhA || format == kFormatB3A8;}
inline unsigned bytesPerPixel(uint32_t format) {return format == kFormatRGhA ? 8 : 4;}
// count pixels as read from a base surface into out (0xAARRGGBB). 'BGRA' and 'b3a8' may convert in place (src == out).
inline void convertSpan(uint32_t format, const void *src, uint32_t *out, uint32_t count, const Tables &t)
{
  if (format == kFormatBGRA) {
    if (src != out) memmove(out, src, size_t(count) * 4);
    return;
  }
  if (format == kFormatB3A8) {   // blue in the low bits, as BGR10A2; alpha (the second plane) is not shown
    const uint32_t *p = static_cast<const uint32_t *>(src);
    for (uint32_t x = 0; x < count; x++) {
      const uint32_t bgr = p[x];
      out[x] = kBlack | uint32_t(t.xr10[bgr >> 20 & 1023]) << 16 | uint32_t(t.xr10[bgr >> 10 & 1023]) << 8 | t.xr10[bgr & 1023];
    }
    return;
  }
  uint64_t rgba;   // 'RGhA': red in the low half
  for (uint32_t x = 0; x < count; x++) {
    memcpy(&rgba, static_cast<const uint8_t *>(src) + size_t(x) * 8, 8);
    out[x] = kBlack | uint32_t(t.half[rgba >> 2 & 0x3fff]) << 16 | uint32_t(t.half[rgba >> 18 & 0x3fff]) << 8 | t.half[rgba >> 34 & 0x3fff];
  }
}

// A surface's pixels as the composition reads them.
struct RowSource {
  const uint8_t *bytes = nullptr;   // pixel (0, 0) (base offset, or plane 0's offset for 'b3a8', applied)
  uint64_t rowBytes = 0;
  uint32_t width = 0, height = 0, format = 0;
};

// The overlay: layer 1, WindowServer's cursor.
struct Overlay {
  bool visible = false;        // layer 1 has a surface
  int32_t x = 0, y = 0;        // where the surface's pixel (0, 0) is on the panel (the swap's layer-1 origin)
  Rect glyph;                  // the part of the surface drawn (surface coordinates)
  int32_t hotX = 0, hotY = 0;  // its hot spot (surface coordinates, inside the glyph): logged; drawing the glyph at its place puts it
                               // where WindowServer wants it
};
// IOSurface::getImageOriginAndExtents's six words, as AppleParavirtDisplay::program_hardware_cursor_gated uses them: the glyph's
// rectangle in the surface (x, y, width, height), then the hot spot (x, y) inside it. A missing or empty rectangle, one that starts
// outside the surface, or a hot spot outside it (AppleParavirtDisplay refuses such a cursor): the whole surface; one that runs past the
// surface is cut.
inline Rect glyphRect(uint32_t width, uint32_t height, const uint32_t *words)
{
  const Rect all = makeRect(0, 0, width, height);
  if (!words || !words[2] || !words[3] || words[0] >= width || words[1] >= height) return all;
  const Rect glyph = makeRect(words[0], words[1], int64_t(words[0]) + min64(words[2], int64_t(width) - words[0]),
                              int64_t(words[1]) + min64(words[3], int64_t(height) - words[1]));
  if (words[4] < words[0] || words[4] >= glyph.right() || words[5] < words[1] || words[5] >= glyph.bottom()) return all;
  return glyph;
}
inline void hotSpot(const uint32_t *words, int32_t *x, int32_t *y)
{
  *x = words ? int32_t(words[4] & 0x7fffffff) : 0;
  *y = words ? int32_t(words[5] & 0x7fffffff) : 0;
}
// The glyph's rectangle on the panel, clipped at its edges; empty when hidden or entirely off the panel.
inline Rect overlayOnPanel(const Overlay &o, uint32_t panelWidth, uint32_t panelHeight)
{
  if (!o.visible || o.glyph.empty()) return {};
  return intersect(makeRect(int64_t(o.x) + o.glyph.x, int64_t(o.y) + o.glyph.y, int64_t(o.x) + o.glyph.right(),
                            int64_t(o.y) + o.glyph.bottom()),
                   panelRect(panelWidth, panelHeight));
}
// Where row y of the panel meets the overlay within [x0, x1): the panel span [from, to) and the surface pixel (sx, sy) at from.
struct Span {
  int32_t from = 0, to = 0, sx = 0, sy = 0;
  bool empty() const {return to <= from;}
};
inline Span overlaySpan(const Overlay &o, const Rect &onPanel, int32_t y, int32_t x0, int32_t x1)
{
  Span s;
  if (onPanel.empty() || y < onPanel.y || y >= onPanel.bottom()) return s;
  s.from = int32_t(max64(x0, onPanel.x));
  s.to = int32_t(min64(x1, onPanel.right()));
  if (s.to <= s.from) return {};
  s.sx = int32_t(int64_t(s.from) - o.x);
  s.sy = int32_t(int64_t(y) - o.y);
  return s;
}

// One row y of a panel rectangle [x0, x1), in three steps the kernel times apart. line is indexed by panel x (the panel's width);
// scratch holds (x1 - x0) * 8 bytes. Uncached memory is read once per row, in one run (single uncached loads are the copy's cost).
// 1. The base's part of the row: into line for 4-byte formats, into scratch for 'RGhA'. Returns its pixels from x0 (0: none there).
inline uint32_t readBaseSpan(uint32_t *line, void *scratch, int32_t y, int32_t x0, int32_t x1, const RowSource *base)
{
  if (!base || !base->bytes || !baseFormat(base->format) || y < 0 || uint32_t(y) >= base->height || x0 < 0 || x1 <= x0 ||
      uint32_t(x0) >= base->width)
    return 0;
  const uint32_t count = uint32_t(min64(x1, base->width) - x0);
  const unsigned pixelBytes = bytesPerPixel(base->format);
  const uint8_t *src = base->bytes + uint64_t(y) * base->rowBytes + uint64_t(x0) * pixelBytes;
  memcpy(base->format == kFormatRGhA ? scratch : static_cast<void *>(line + x0), src, size_t(count) * pixelBytes);
  return count;
}
// 2. Converted in place; black where the base has no pixel.
inline void convertBaseSpan(uint32_t *line, const void *scratch, int32_t x0, int32_t x1, uint32_t count, const RowSource *base,
                            const Tables &t)
{
  if (count) convertSpan(base->format, base->format == kFormatRGhA ? scratch : line + x0, line + x0, count, t);
  for (int64_t x = int64_t(x0) + count; x < x1; x++) line[x] = kBlack;
}
// 3. The overlay over it: its part of the row read in one run into scratch (4 bytes a pixel), blended. Returns the pixels blended.
inline uint32_t blendOverlayRow(uint32_t *line, void *scratch, int32_t y, int32_t x0, int32_t x1, const RowSource *overlay,
                                const Overlay &o, const Rect &onPanel)
{
  if (!overlay || !overlay->bytes || overlay->format != kFormatBGRA) return 0;
  const Span s = overlaySpan(o, onPanel, y, x0, x1);
  if (s.empty() || s.sx < 0 || s.sy < 0 || uint32_t(s.sy) >= overlay->height || int64_t(s.sx) + (s.to - s.from) > overlay->width)
    return 0;
  const uint32_t count = uint32_t(s.to - s.from);
  memcpy(scratch, overlay->bytes + uint64_t(s.sy) * overlay->rowBytes + uint64_t(s.sx) * 4, size_t(count) * 4);
  blendSpan(line + s.from, static_cast<const uint32_t *>(scratch), count);
  return count;
}
// The three at once (the host tests' composition).
inline void composeRow(uint32_t *line, void *scratch, int32_t y, int32_t x0, int32_t x1, const RowSource *base, const RowSource *overlay,
                       const Overlay &o, const Rect &onPanel, const Tables &t)
{
  const uint32_t count = readBaseSpan(line, scratch, y, x0, x1, base);
  convertBaseSpan(line, scratch, x0, x1, count, base, t);
  blendOverlayRow(line, scratch, y, x0, x1, overlay, o, onPanel);
}

// --------------------------------------------------------------------------------------------------------------------- damage
// The swap's dirty region (2026-11-12): IOMobileFramebufferLegacy::swap_submit copies 16 bytes of the client's IOMFBSwapRec (+664)
// into the request at +417 and the low bit of rec +716 to +552; getCurrentDirtyRegion_gated() hands +417 out when that bit is set
// (the k27 collection's IOMobileGraphicsFamily, read with the k27 tree's scripts/kc-disasm.py). The 16 bytes are an IOMFBRect, the type
// of the layers' rectangles beside it (+17 + 16 i source, +81 + 16 i destination, whose words AppleParavirtDisplay passes as ints):
// x, y, width, height in panel pixels. What the region means is WindowServer's: taken as the part of the frame that differs from the
// frame swapped before it (a panel's partial update); the audit below checks that against the panel and turns damage off if not.
// Words that cannot be panel pixels (a coordinate or size past 2^16: a float, a flag word) are no region: the frame is composed whole.
struct DirtyRegion {
  bool known = false;   // the swap says what changed (its bit, sane words)
  Rect rect;            // with known: clipped to the panel; empty means nothing in layer 0 changed
};
inline DirtyRegion dirtyRegion(bool valid, const uint32_t *words, uint32_t panelWidth, uint32_t panelHeight)
{
  DirtyRegion d;
  if (!valid || !words) return d;
  const uint32_t limit = 1u << 16;
  if (words[0] >= limit || words[1] >= limit || words[2] > limit || words[3] > limit) return d;
  d.known = true;
  d.rect = intersect(makeRect(words[0], words[1], int64_t(words[0]) + words[2], int64_t(words[1]) + words[3]),
                     panelRect(panelWidth, panelHeight));
  return d;
}
// The swaps programmed must follow one another: a region names what changed since the frame before it, so a swap that never reached
// the driver (the family drops obsolete, aborted, no-op and dummy swaps in clean_queue_gated, the driver those with drop reasons; a
// cancelled one never comes) takes its change along. The family numbers the swaps it starts (+980, one after the other, cursor swaps
// included): a programmed swap whose number does not follow the last one makes the next frame whole (the gap may have been a frame
// even when the swap after it moves only the cursor; also the first swap, and a number that goes back). Spurious gaps (a cancelled
// swap) only cost a whole composition.
struct SwapSequence {
  bool have = false, gap = true;
  uint32_t last = 0;
  void next(uint32_t id)   // every programmed swap, in order
  {
    if (!have || id != last + 1) gap = true;
    have = true;
    last = id;
  }
  bool takeGap()           // a new frame: whether a swap went missing since the last frame (then it is whole)
  {
    const bool g = gap;
    gap = false;
    return g;
  }
};
// Up to kAuditPoints panel pixels outside a plan's rectangles and the cursor's old and new rectangles (where the panel must still
// show the frame before: the dirty region said nothing changed there), spread over the panel by a small generator whose state the
// caller keeps (each job samples other points). The caller compares each point's composition from the new frame with the panel.
static constexpr unsigned kAuditPoints = 32;
struct AuditPoints {
  unsigned count = 0;
  int32_t x[kAuditPoints], y[kAuditPoints];
};
inline uint32_t auditRandom(uint32_t &state)
{
  state = state * 1664525u + 1013904223u;   // Numerical Recipes' LCG; the high bits are the usable ones
  return state >> 8;
}
inline bool inside(const Rect &r, int32_t x, int32_t y) {return !r.empty() && x >= r.x && x < r.right() && y >= r.y && y < r.bottom();}
inline AuditPoints auditPoints(uint32_t &state, uint32_t panelWidth, uint32_t panelHeight, const Rect *exclude, unsigned excludeCount,
                               unsigned want = kAuditPoints)
{
  AuditPoints p;
  if (!panelWidth || !panelHeight) return p;
  if (want > kAuditPoints) want = kAuditPoints;
  for (unsigned tries = 0; tries < 4 * want && p.count < want; tries++) {
    const int32_t x = int32_t(auditRandom(state) % panelWidth), y = int32_t(auditRandom(state) % panelHeight);
    bool excluded = false;
    for (unsigned i = 0; i < excludeCount && !excluded; i++) excluded = inside(exclude[i], x, y);
    if (excluded) continue;
    p.x[p.count] = x;
    p.y[p.count] = y;
    p.count++;
  }
  return p;
}

// --------------------------------------------------------------------------------------------------------------------- layers
// One swap as the family hands it over (IOMFBSwapIORequest): bit i of the mask (+984) says layer i is part of it; its surface (+2944
// + 8 * i) is what the layer shows from then on, nullptr turning it off (AppleParavirtDisplay: a null layer-1 surface hides its host
// cursor); a layer whose bit is clear stays as it is. Layer 1's origin on the panel (+97, +101) is read when bit 1 is set. dirty: the
// swap's dirty region (above), used for a new frame on layer 0.
struct SwapLayers {
  uint32_t mask = 0;
  const void *surface[kLayers] = {};
  int32_t x1 = 0, y1 = 0;
  DirtyRegion dirty;
};
struct LayerState {   // what each layer shows
  const void *surface[kLayers] = {};
  int32_t x1 = 0, y1 = 0;
};
struct LayerChange {
  bool base = false;        // layer 0 has a new frame (a surface, possibly the same one again)
  bool baseOff = false;     // layer 0 was turned off: the panel shows black under the overlay
  bool overlay = false;     // layer 1 was shown, moved, given another image or hidden
  bool other = false;       // layer 2 or 3 is part of the swap (not composited: no position is known for them)
  const void *taken[kLayers] = {};      // the surface each layer takes on (the state's new references)
  const void *released[kLayers] = {};   // the surface each layer gives up: off the screen once the swap completes
};
inline LayerChange applySwap(LayerState &state, const SwapLayers &swap)
{
  LayerChange c;
  for (unsigned i = 0; i < kLayers; i++) {
    if (!(swap.mask >> i & 1)) continue;
    const void *next = swap.surface[i], *previous = state.surface[i];
    if (next != previous) {
      c.taken[i] = next;
      c.released[i] = previous;
    }
    if (i == 0) {
      c.base = next != nullptr;
      c.baseOff = !next && previous;
    } else if (i == 1) {
      c.overlay = next || previous;   // hidden -> hidden changes nothing
      if (next) {
        state.x1 = swap.x1;
        state.y1 = swap.y1;
      }
    } else {
      c.other = true;
    }
    state.surface[i] = next;
  }
  return c;
}

// --------------------------------------------------------------------------------------------------------------------- the jobs
// What the copy thread draws: a snapshot of the layers (the caller holds a reference to base and overlay for the job's life).
struct Job {
  enum Kind : uint8_t {kNone = 0, kCursor = 1, kFull = 2};
  Kind kind = kNone;
  bool refresh = false;   // a refresh copy (no swap waits for it)
  bool last = false;      // the countdown's last refresh (tick 30): it runs even after a fence-covered frame
  bool fenced = false;    // wait for the GPU's fence first
  uint32_t fence = 0;
  bool flip = false;      // flip mode: a full job may be shown by the DPU itself
  // What of layer 0 the job must compose (damage, 2026-11-12): frameAll, the whole panel (a frame without a dirty region, layer 0
  // off, a forced refresh); else only frameDamage (a frame's dirty region, possibly empty; a cursor job's is empty). A refresh of the
  // countdown has neither: the copy thread recomposes what it drew since its last whole composition, or the whole panel (planJob).
  bool frameAll = true;
  Rect frameDamage;
  bool overlayChanged = false;   // layer 1 was shown, moved, given another image or hidden (a damage plan then redraws its rectangles)
  const void *base = nullptr;      // layer 0 (nullptr: black)
  const void *overlay = nullptr;   // layer 1 (nullptr: hidden)
  int32_t x1 = 0, y1 = 0;
  uint64_t serial = 0;    // the order of the jobs: they run in increasing serials
};
// A waiting job and a newer one: the newer state, the larger kind, a refresh only if both were, the fence of whichever has one, the
// union of what both had to compose of layer 0 (the frames merged were never drawn, so each one's change is still to show).
inline Job mergeJobs(const Job &older, const Job &newer)
{
  Job j = newer;
  if (older.kind > j.kind) j.kind = older.kind;
  j.refresh = older.refresh && newer.refresh;
  j.last = j.refresh && (older.last || newer.last);
  if (!newer.fenced && older.fenced) {
    j.fenced = true;
    j.fence = older.fence;
  }
  j.overlayChanged = older.overlayChanged || newer.overlayChanged;
  j.frameAll = older.frameAll || newer.frameAll;
  j.frameDamage = j.frameAll ? Rect() : unite(older.frameDamage, newer.frameDamage);
  return j;
}
struct Slot {   // the copy thread's one waiting job (under the caller's lock)
  bool busy = false;
  Job job;
};
enum class Offer : uint8_t {Taken, Merged, Dropped};
// A refresh never displaces a swap's job (that job draws a newer state anyway); anything else is merged into what waits. Merged: the
// waiting job's references are the caller's to drop (*displaced); Dropped: the new job's.
inline Offer offer(Slot &slot, const Job &job, Job *displaced)
{
  if (!slot.busy) {
    slot.busy = true;
    slot.job = job;
    return Offer::Taken;
  }
  if (job.refresh && !slot.job.refresh) return Offer::Dropped;
  *displaced = slot.job;
  slot.job = mergeJobs(slot.job, job);
  return Offer::Merged;
}
struct Busy {   // the job the copy thread is drawing (under the caller's lock)
  uint64_t serial = 0;   // 0: none
  const void *base = nullptr, *overlay = nullptr;
  bool refresh = false;  // a refresh copy: it may stop at the next row (abortBusy)
};
// Whether completing the swap now would hand a surface back to WindowServer that the copy thread is reading.
inline bool readsReleased(const Busy &busy, const LayerChange &change)
{
  if (!busy.serial) return false;
  for (unsigned i = 0; i < kLayers; i++)
    if (change.released[i] && (change.released[i] == busy.base || change.released[i] == busy.overlay)) return true;
  return false;
}
// native-gop-display-defer: when a swap completes.
enum DeferMode : uint32_t {
  kDeferNever = 0,    // at once (before 2026-11-12): the copy thread may still read a surface WindowServer is drawing again
  kDeferWhenRead = 1, // at once, unless the copy thread is reading a surface the swap takes off the screen: then once that read is done
  kDeferAlways = 2,   // (default) once the swap's own job is done (flip mode: the replaced surface is free once the new one latched)
};
struct Admission {
  bool hadJob = false;
  Offer offer = Offer::Taken;
  Job displaced;            // with Offer::Merged: its references to drop
  uint64_t waitSerial = 0;  // the swap completes once the job with this serial (and so every earlier one) is done; 0: now
  bool abortBusy = false;   // the job in the way is a refresh copy: it stops at its next row (it re-reads the frame on screen, and
                            // this swap's job draws a newer state), so the swap waits for one row, not a whole copy
};
// A swap's job into the slot and its completion rule in one step under the caller's lock, so the copy thread cannot take a waiting
// job (that may read a surface this swap releases) between the check and the offer: the offer merges it into this swap's job, whose
// snapshot holds only the surfaces on screen after the swap.
inline Admission admitSwap(Slot &slot, const Busy &busy, const Job &job, const LayerChange &change, uint32_t defer)
{
  Admission a;
  if (job.kind != Job::kNone) {
    a.hadJob = true;
    a.offer = offer(slot, job, &a.displaced);
  }
  if (defer == kDeferNever) return a;
  if (defer == kDeferAlways && a.hadJob) a.waitSerial = job.serial;
  if (readsReleased(busy, change) && busy.serial > a.waitSerial) a.waitSerial = busy.serial;
  a.abortBusy = busy.refresh && a.waitSerial && a.waitSerial >= busy.serial;
  return a;
}
// The copy thread takes the waiting job (under the lock) and is busy with it until finishJob.
inline bool takeJob(Slot &slot, Busy &busy, Job *job)
{
  if (!slot.busy) return false;
  *job = slot.job;
  slot.busy = false;
  slot.job = Job();
  busy.serial = job->serial;
  busy.base = job->base;
  busy.overlay = job->overlay;
  busy.refresh = job->refresh;
  return true;
}
// The job is done (under the lock): *done is its serial; true when a swap waits for it (or for an earlier one).
inline bool finishJob(Busy &busy, uint64_t *done, uint64_t waitSerial)
{
  *done = busy.serial;
  busy = Busy();
  return waitSerial && *done >= waitSerial;
}

// --------------------------------------------------------------------------------------------------------------------- the panel
struct PanelState {   // the copy thread's
  bool flipped = false;   // the DPU scans a WindowServer surface, not the boot framebuffer
  bool current = false;   // the boot framebuffer holds the composition (false before the first and while a surface is flipped)
  Rect cursor;            // where the overlay is drawn in the boot framebuffer (empty: nowhere)
  Rect sinceFull;         // the bounding box of the frames' damage composed since the last whole composition (a refresh's area)
};
struct Plan {
  bool tryFlip = false;      // offer the base to the DPU first (flip mode, a swap's full job, no cursor on the panel)
  unsigned count = 0;        // the rectangles to compose into the boot framebuffer (when not flipped)
  Rect rect[3];
  bool full = false;         // rect[0] is the whole panel (statistics and thumbnails come from those)
  bool damage = false;       // a frame (or its refresh) composed from its damage: frameRect and the cursor's rectangles
  Rect frameRect;            // with damage: the part of layer 0 recomposed (possibly empty)
  bool bootBack = false;     // then ask the DPU for the boot framebuffer back (flip mode)
  bool cursorForced = false; // flip mode composes because a cursor is on the panel (the DPU has no cursor pipe yet)
};
// One more rectangle of a damage plan: every rectangle it overlaps is folded into it (again while it grows), then it is added. Composing
// a pixel twice would be harmless, but costs; a plan gets at most three (the frame's damage and the cursor's two).
inline void addRect(Plan &plan, const Rect &r)
{
  if (r.empty()) return;
  Rect merged = r;
  for (bool grew = true; grew;) {
    grew = false;
    for (unsigned i = 0; i < plan.count; i++)
      if (!intersect(plan.rect[i], merged).empty()) {
        merged = unite(merged, plan.rect[i]);
        plan.rect[i] = plan.rect[--plan.count];
        grew = true;
        break;
      }
  }
  if (plan.count < 3)
    plan.rect[plan.count++] = merged;
  else
    plan.rect[2] = unite(plan.rect[2], merged);
}
// cursor: the overlay's rectangle on the panel for this job (overlayOnPanel; empty when hidden). damageOk (the copy thread's verdict,
// 2026-11-12): a frame may be composed from its damage: damage copies are on for the boot, the frame's GPU work was fence-covered (a
// refresh: the frame on screen was), no whole composition is due. Then a frame whose job knows its damage, on a boot framebuffer that
// is on the panel and holds the composition, recomposes only that damage and the cursor's old and new rectangles; every other frame
// the whole panel, as before.
inline Plan planJob(const PanelState &panel, const Job &job, const Rect &cursor, const Rect &whole, bool damageOk = false)
{
  Plan plan;
  if (job.kind == Job::kNone) return plan;
  if (panel.flipped && job.refresh) return plan;   // the DPU reads the surface itself every frame
  const bool fullJob = job.kind == Job::kFull;
  if (fullJob || panel.flipped || (!panel.current && job.base)) {
    if (panel.flipped && !fullJob && cursor.empty()) return plan;   // still no cursor over the flipped surface
    if (job.flip && !job.refresh && fullJob && job.base && cursor.empty()) plan.tryFlip = true;
    plan.cursorForced = job.flip && !cursor.empty();
    if (damageOk && fullJob && !job.frameAll && panel.current && !panel.flipped) {
      plan.damage = true;
      plan.frameRect = intersect(job.refresh ? panel.sinceFull : job.frameDamage, whole);
      addRect(plan, plan.frameRect);
      if (job.overlayChanged || !(panel.cursor == cursor)) {   // an unchanged cursor stays drawn where the frame did not change
        const Damage d = cursorDamage(panel.cursor, cursor);
        for (unsigned i = 0; i < d.count; i++) addRect(plan, d.rect[i]);
      }
      return plan;
    }
    plan.rect[0] = whole;
    plan.count = 1;
    plan.full = true;
    plan.bootBack = job.flip || panel.flipped;
    return plan;
  }
  const Damage damage = cursorDamage(panel.cursor, cursor);
  plan.count = damage.count;
  for (unsigned i = 0; i < damage.count; i++) plan.rect[i] = damage.rect[i];
  return plan;
}
// The pixels a plan composes (its rectangles may overlap only by a merge's bounding box, so this is what it costs).
inline int64_t planArea(const Plan &plan)
{
  int64_t area = 0;
  for (unsigned i = 0; i < plan.count; i++) area += plan.rect[i].area();
  return area;
}
// A refresh stopped part way (abortBusy): its rows were the same frame again, the cursor drawn where it was or where the job had it;
// the next damage covers both.
inline void abortedJob(PanelState &panel, const Plan &plan, const Rect &cursor)
{
  if (plan.count) panel.cursor = unite(panel.cursor, cursor);
}
inline void commitJob(PanelState &panel, const Plan &plan, bool flipped, const Rect &cursor)
{
  if (plan.tryFlip && flipped) {
    panel.flipped = true;
    panel.current = false;
    panel.cursor = Rect();
    panel.sinceFull = Rect();
    return;
  }
  if (!plan.count) return;
  if (plan.full) {
    panel.current = true;
    panel.sinceFull = Rect();
  }
  if (plan.damage) panel.sinceFull = unite(panel.sinceFull, plan.frameRect);
  panel.cursor = cursor;
  if (plan.bootBack) panel.flipped = false;
}
// The copy thread's damage verdict for one job (planJob's damageOk). enabled: native-gop-display-damage and no failed audit;
// covered: the job's frame (a refresh: the frame on screen) was fence-covered; fullDue: the periodic whole composition is due.
inline bool damageAllowed(bool enabled, bool covered, bool fullDue)
{
  return enabled && covered && !fullDue;
}

// ------------------------------------------------------------------------------------------------------------- refresh copies
// native-gop-display-refresh: which of a frame's refresh copies run.
enum RefreshPolicy : uint32_t {
  kRefreshOff = 0,        // none
  kRefreshUncovered = 1,  // all six after a frame no GPU fence covered (none known, or the wait gave up); only the last after one it did
  kRefreshAlways = 2,     // all six after every frame (before 2026-11-12)
};
// One VBL tick of a countdown (left > 0 after a frame): decrements it; true when this tick is one of 1, 2, 4, 8, 16, 30; *last for 30.
inline bool refreshTick(unsigned &left, bool *last)
{
  if (!left) return false;
  const unsigned elapsed = kRefreshTicks - --left;
  *last = !left;
  return !(elapsed & (elapsed - 1)) || !left;
}
// The work loop's VBL tick while the display is off (2026-11-12): none once nothing needs it. darkStop: native-gop-display-vbl not 0;
// dark: the display's power is known and off. A swap queued or waiting, a refresh countdown or an undrawn job's retry keep it.
inline bool vblStops(bool darkStop, bool dark, unsigned refreshLeft, bool unfinished, bool waiting, bool queued)
{
  return darkStop && dark && !refreshLeft && !unfinished && !waiting && !queued;
}
// The tick while the display is on and nothing moves (2026-11-12, native-gop-display-vbl=2, flip mode's rule since 2026-11-11): none
// once no refresh is counting down, no undrawn job waits for a retry, and the queue is empty or its head waits behind a swap whose
// completion (jobDoneAction) programs it. The family calls swap_begin_gated itself when a swap becomes ready
// (swap_queue_finalize_gated: on_swap_queued, then swap_begin_gated), so nothing waits for a tick.
inline bool vblIdleStops(bool idleStop, unsigned refreshLeft, bool unfinished, bool waiting, bool queued)
{
  return idleStop && !refreshLeft && !unfinished && (waiting || !queued);
}
// The family's queue as swap_begin_gated leaves it (a VBL tick, a submission, a deferred completion; the periodic line's counters).
// Behind: a swap queued behind the current one, which waits for its completion rule (native-gop-display-defer, flip mode):
// WindowServer swapping faster than the copies go, the back-pressure that rule exists for (expected in animations and while the GPU
// finishes slow first frames). Stalled: a swap queued while none is current, which nothing in progress will program (0 expected).
enum class QueueTick : uint8_t {Idle, Behind, Stalled};
inline QueueTick queueTick(bool queued, bool current)
{
  if (!queued) return QueueTick::Idle;
  return current ? QueueTick::Behind : QueueTick::Stalled;
}
// On the copy thread, when a refresh job runs: whether it copies (covered: the last frame's copy waited for its fence and saw it
// retired, so it read the finished frame).
inline bool refreshNeeded(uint32_t policy, bool covered, bool last)
{
  if (policy == kRefreshOff) return false;
  if (policy == kRefreshUncovered && covered && !last) return false;
  return true;
}

// What the copy thread does with a job (after the fence wait of a fenced one). retired: the job's fence passed (true without one, or
// when the GPU does not answer). panelFinished: the fence the panel's state needs (the newest drawn or refused swap job's) passed.
// A frame the GPU is still drawing is never read: the previous composition stays, and the work loop retries on the next ticks.
enum class Verdict : uint8_t {Draw, NotNeeded, Unfinished};
inline Verdict verdict(const Job &job, bool retired, uint32_t policy, bool covered, bool panelFinished)
{
  if (job.refresh) {
    if (!panelFinished) return Verdict::Unfinished;
    return refreshNeeded(policy, covered, job.last) ? Verdict::Draw : Verdict::NotNeeded;
  }
  if (job.fenced && !retired) return Verdict::Unfinished;
  return Verdict::Draw;
}

// ------------------------------------------------------------------------------------------------------- the work loop's side
// Under the family's gate: the layers as the swaps left them, the jobs they need, the refresh countdown, the swap that waits.
struct Pipeline {
  LayerState layers;
  uint64_t serial = 0;
  unsigned refreshLeft = 0;
  bool pending = false;     // the current swap waits for its completion rule
  uint64_t waitSerial = 0;  // ... the job it waits for (admitSwap)
  bool damage = false;      // native-gop-display-damage: a new frame's job takes its swap's dirty region
  bool afterOff = false;    // layer 0 was turned off since the last frame: the panel shows black, not the frame the region is about

  void snapshot(Job &job) const
  {
    job.base = layers.surface[0];
    job.overlay = layers.surface[1];
    job.x1 = layers.x1;
    job.y1 = layers.y1;
  }
  // The job a swap needs (kind kNone: none; it then completes at once) and what it did to the layers. A new frame starts the refresh
  // countdown; layer 0 off ends it. A new frame with a dirty region (and damage on) names what of layer 0 changed; one without, the
  // first after layer 0 was off (the panel is black then, not the frame before), or layer 0 turned off, needs the whole panel; a cursor
  // change alone, nothing of layer 0.
  Job swapJob(const SwapLayers &swap, bool fenced, uint32_t fence, bool flip, LayerChange *change)
  {
    *change = applySwap(layers, swap);
    Job job;
    if (change->base || change->baseOff) {
      job.kind = Job::kFull;
      job.frameAll = !(change->base && damage && swap.dirty.known && !afterOff);
      if (!job.frameAll) job.frameDamage = swap.dirty.rect;
      afterOff = change->baseOff;
    } else if (change->overlay) {
      job.kind = Job::kCursor;
      job.frameAll = false;
    }
    if (change->base)
      refreshLeft = kRefreshTicks;
    else if (change->baseOff)
      refreshLeft = 0;
    if (job.kind == Job::kNone) return job;
    job.overlayChanged = change->overlay;
    job.fenced = fenced;
    job.fence = fence;
    job.flip = flip;
    snapshot(job);
    job.serial = ++serial;
    return job;
  }
  // A VBL tick that programmed no new frame: the refresh job due, if any (none while a swap waits, or with nothing on layer 0). It
  // names no area: the copy thread recomposes the whole panel, or after a fence-covered frame only what it drew since its last whole
  // composition (planJob).
  bool refreshJob(Job *job)
  {
    if (pending || !layers.surface[0] || !refreshLeft) return false;
    bool last = false;
    if (!refreshTick(refreshLeft, &last)) return false;
    *job = Job();
    job->kind = Job::kFull;
    job->refresh = true;
    job->last = last;
    job->frameAll = false;
    snapshot(*job);
    job->serial = ++serial;
    return true;
  }
  // A refresh asked for out of the countdown (a thumbnail that is due on a static screen): it always runs, over the whole panel.
  bool forcedRefreshJob(Job *job)
  {
    if (pending || !layers.surface[0]) return false;
    *job = Job();
    job->kind = Job::kFull;
    job->refresh = true;
    job->last = true;
    snapshot(*job);
    job->serial = ++serial;
    return true;
  }
  // A tick while the copy thread left a swap's job undrawn (its fence had not passed): the whole panel again from the current state,
  // drawn once the GPU is done.
  bool retryJob(bool unfinished, Job *job)
  {
    return unfinished && forcedRefreshJob(job);
  }
  // The swap that waited completes; a flipped frame needs no refresh copies.
  void swapDone(bool flipped)
  {
    pending = false;
    waitSerial = 0;
    if (flipped) refreshLeft = 0;
  }
};

// ------------------------------------------------------------------------------------------- the panel before the first frame
// What start() leaves on the boot framebuffer until WindowServer's first frame (2026-11-12). XNU's text console (a verbose boot)
// cleared the panel to its background, black, when it started (gc_enable) and from then on writes only what changes: a scroll
// repaints only the cells whose character changed (gc_scroll_up), and in those only the pixels where the new glyph differs from the
// old one (vc_paint_char_32); a new character is painted whole. A colour painted under the console therefore stays wherever no glyph
// pixel changed, and the strokes that scroll away leave black in it at the glyphs' common stem and bar positions: the bluish
// background with horizontal and vertical lines of 2026-11-12 (the fill 0xff203050 of before). So a text console's framebuffer is
// left as it is; a graphics boot's (nothing draws on it) is cleared to black. native-gop-display-fill=0xRRGGBB asks for a colour
// (0x203050, the old one, shows that the mapping reaches the panel; under a text console it leaves those lines), the bars win over it.
enum class StartFill : uint8_t {Keep, Black, Colour, Bars};
struct StartPlan {
  StartFill kind;
  uint32_t pixel;   // Black, Colour: the panel's 0xAARRGGBB
};
inline StartPlan startPlan(bool bars, bool fillGiven, uint32_t fillRgb, bool textConsole)
{
  if (bars) return {StartFill::Bars, 0};
  if (fillGiven) return {StartFill::Colour, kBlack | (fillRgb & 0xffffffu)};
  if (textConsole) return {StartFill::Keep, 0};
  return {StartFill::Black, kBlack};
}
// XNU's own choice (PE_create_console, pexpert/arm/pe_init.c of the k27 tree): the graphics console only when the boot framebuffer
// has a display mode (boot_args Video.v_display, DarwinBoot's) and neither -v nor vc-text is set; the text console otherwise.
inline bool textConsole(uint64_t display, bool verbose, uint32_t vcText)
{
  return !display || verbose || vcText;
}

}  // namespace gopcompose
