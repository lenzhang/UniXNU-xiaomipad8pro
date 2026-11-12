// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of NativeGopDisplay's composition and swap pipeline (GopCompose.h):
//   - geometry: rectangles, clamping, clipping at the panel's four edges, the cursor's damage (one box or two rectangles);
//   - pixels: round(a * b / 255) exhaustively, premultiplied 'BGRA' over an opaque pixel against a reference, saturation, the half
//     float and extended-range conversions against references, every base format;
//   - the overlay: the glyph rectangle from IOSurface's origin-and-extents words, its place on the panel (off every edge, far away),
//     the row spans, and whole composed rows and damage rectangles against a per-pixel reference composition;
//   - the layers: mask bits, a null surface turning a layer off, unchanged layers, the surfaces taken and released, the cursor's origin;
//   - the jobs: the slot (refresh never over a swap's job, merges), the completion rule (admitSwap) in each mode, take and finish;
//   - the panel plan: copy mode (whole panel, the cursor's old and new rectangles) and flip mode (a cursor forces the copy and the boot
//     framebuffer back; flips resume once it hides; refreshes of a flipped surface do nothing);
//   - refresh copies: the ticks 1, 2, 4, 8, 16, 30, each policy, none while a swap waits;
//   - a simulation of WindowServer (three framebuffers in turn, drawn by a GPU that finishes after the swap, a moving and hiding
//     cursor at the panel's edges), the family (queue, current swap, completion handing the replaced surfaces back, which WindowServer
//     draws into at once), the VBL timer, the GPU's fences and the copy thread (20 ms full copies): with native-gop-display-defer=1 or
//     2 no copy ever reads a surface WindowServer is drawing, the panel equals the reference composition whenever everything is idle and
//     at the end; with defer=0 (before 2026-11-12) the same workload tears (the race of 2026-11-11, reproduced); a slow GPU (fence
//     waits that give up) still ends with the right frame; flip mode never shows a flipped surface while a cursor is on;
//   - the panel before the first frame (startPlan): the rule's table, XNU's console choice, and a model of the text console's painting
//     rules over twenty screens of log: the framebuffer kept under -v is exactly the console's text; the old fill (#203050) stays
//     behind the text with black stroke lines through it (the bluish background of 2026-11-12, reproduced).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. gop_host.cpp -o /tmp/gop_host && /tmp/gop_host
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <queue>
#include <random>
#include <vector>
#include "GopCompose.h"

using namespace gopcompose;
static int failures, checks;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------------------- geometry
static void testGeometry()
{
  CHECK(makeRect(0, 0, 0, 5).empty());
  CHECK(makeRect(5, 0, 4, 5).empty());
  CHECK((makeRect(-5, -5, 5, 5) == Rect{-5, -5, 10, 10}));
  const Rect panel = panelRect(3200, 2136);
  CHECK((panel == Rect{0, 0, 3200, 2136}));
  CHECK((intersect(makeRect(-10, -10, 20, 20), panel) == Rect{0, 0, 20, 20}));
  CHECK((intersect(makeRect(3190, 2130, 3300, 2200), panel) == Rect{3190, 2130, 10, 6}));
  CHECK(intersect(makeRect(4000, 0, 4100, 10), panel).empty());
  CHECK(intersect(makeRect(-200, 0, -72, 10), panel).empty());
  CHECK((intersect(makeRect(-(int64_t(1) << 40), 0, int64_t(1) << 40, 10), panel) == Rect{0, 0, 3200, 10}));   // clamped, no overflow
  CHECK((unite(Rect{}, Rect{1, 2, 3, 4}) == Rect{1, 2, 3, 4}));
  CHECK((unite(Rect{1, 2, 3, 4}, Rect{}) == Rect{1, 2, 3, 4}));
  CHECK((unite(Rect{0, 0, 2, 2}, Rect{10, 10, 2, 2}) == Rect{0, 0, 12, 12}));
  CHECK(Rect{}.area() == 0 && (Rect{0, 0, 3, 4}.area() == 12));
  Damage d = cursorDamage(Rect{}, Rect{});
  CHECK(d.count == 0);
  d = cursorDamage(Rect{}, Rect{10, 10, 64, 64});
  CHECK(d.count == 1 && (d.rect[0] == Rect{10, 10, 64, 64}));
  d = cursorDamage(Rect{10, 10, 64, 64}, Rect{});
  CHECK(d.count == 1 && (d.rect[0] == Rect{10, 10, 64, 64}));
  d = cursorDamage(Rect{10, 10, 64, 64}, Rect{20, 15, 64, 64});   // a small move: one box
  CHECK(d.count == 1 && (d.rect[0] == Rect{10, 10, 74, 69}));
  d = cursorDamage(Rect{10, 10, 64, 64}, Rect{80, 10, 64, 64});   // apart but close: still one box (134 x 64 <= 2 x 2 x 4096)
  CHECK(d.count == 1 && (d.rect[0] == Rect{10, 10, 134, 64}));
  d = cursorDamage(Rect{0, 0, 64, 64}, Rect{2000, 1500, 64, 64});  // a jump: the two rectangles, not the box between
  CHECK(d.count == 2 && (d.rect[0] == Rect{0, 0, 64, 64}) && (d.rect[1] == Rect{2000, 1500, 64, 64}));
}

// --------------------------------------------------------------------------------------------------------------------- pixels
static uint32_t referenceBlend(uint32_t dst, uint32_t src)
{
  const unsigned a = src >> 24;
  uint32_t out = 0xff000000u;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    const double v = double(src >> shift & 255) + double(dst >> shift & 255) * (255 - a) / 255.0;
    const long r = std::lround(std::floor(v + 0.5));
    out |= uint32_t(r > 255 ? 255 : r) << shift;
  }
  return a == 255 ? src : src == 0 ? dst : out;
}
static double halfValue(uint16_t h)
{
  const int e = h >> 10 & 31, m = h & 1023;
  const double s = h & 0x8000 ? -1 : 1;
  if (e == 31) return m ? NAN : s * INFINITY;
  if (!e) return s * std::ldexp(m, -24);
  return s * std::ldexp(1024 + m, e - 25);
}
static void testPixels()
{
  bool exact = true;
  for (uint32_t a = 0; a < 256; a++)
    for (uint32_t b = 0; b < 256; b++)
      if (mul255(a, b) != (2 * a * b + 255) / 510) exact = false;
  CHECK(exact);
  CHECK(blendOver(0xff123456, 0xffabcdef) == 0xffabcdef);   // opaque source
  CHECK(blendOver(0xff123456, 0) == 0xff123456);            // transparent
  CHECK(blendOver(0xffffffff, 0x80000000) == 0xff7f7f7f);   // half black over white
  CHECK(blendOver(0xffffffff, 0x80ffffff) == 0xffffffff);   // not premultiplied: saturates, does not wrap
  CHECK(blendOver(0xff000000, 0x40404040) == 0xff404040);   // premultiplied quarter grey over black
  std::mt19937 random(1);
  bool matches = true;
  for (int i = 0; i < 200000; i++) {
    const uint32_t a = random() % 256, dst = 0xff000000u | (random() & 0xffffff);
    uint32_t src = a << 24;
    for (unsigned shift = 0; shift < 24; shift += 8) src |= (a ? random() % (a + 1) : 0) << shift;   // premultiplied: channel <= alpha
    if (i % 7 == 0) src = random();                                                             // and anything at all
    if (blendOver(dst, src) != referenceBlend(dst, src)) matches = false;
  }
  CHECK(matches);
  uint32_t row[5] = {0xff000000, 0xffffffff, 0xff808080, 0xff102030, 0xff0000ff}, over[5] = {0, 0xff00ff00, 0x80000000, 0x01010101, 0x80800000};
  blendSpan(row, over, 5);
  CHECK(row[0] == 0xff000000 && row[1] == 0xff00ff00 && row[2] == 0xff404040);
  CHECK(row[3] == referenceBlend(0xff102030, 0x01010101) && row[4] == referenceBlend(0xff0000ff, 0x80800000));
  // half floats: every one of the 65536 against the decoded value
  bool halves = true;
  for (uint32_t h = 0; h < 65536; h++) {
    const double v = halfValue(uint16_t(h));
    const int want = std::isnan(v) || v <= 0 ? 0 : v >= 1 ? 255 : int(std::floor(v * 255 + 0.5));
    if (halfToByte(uint16_t(h)) != want) halves = false;
  }
  CHECK(halves);
  CHECK(halfToByte(0x3c00) == 255 && halfToByte(0x3800) == 128 && halfToByte(0) == 0 && halfToByte(0xbc00) == 0 && halfToByte(0x7e00) == 0);
  static Tables t;
  t.init();
  CHECK(t.xr10[384] == 0 && t.xr10[383] == 0 && t.xr10[894] == 255 && t.xr10[1023] == 255 && t.xr10[639] == 127);
  // the three base formats
  uint32_t bgra[4] = {0x80112233, 0xffaabbcc, 0, 0xff000000}, out[4];
  convertSpan(kFormatBGRA, bgra, out, 4, t);
  CHECK(!memcmp(out, bgra, sizeof(out)));
  uint32_t b3a8[3] = {894u << 20 | 384u << 10 | 639u, 0, 1023u << 20 | 1023u << 10 | 1023u};
  convertSpan(kFormatB3A8, b3a8, b3a8, 3, t);   // in place
  CHECK(b3a8[0] == 0xffff007f && b3a8[1] == 0xff000000 && b3a8[2] == 0xffffffff);
  uint64_t rgha[2] = {0x3c00ull << 48 | 0x0000ull << 32 | 0x3800ull << 16 | 0x3c00ull, 0xbc00ull << 32 | 0x7bffull << 16 | 0x2e66ull};
  convertSpan(kFormatRGhA, rgha, out, 2, t);
  CHECK(out[0] == (0xff000000u | 255u << 16 | uint32_t(t.half[0x3800 >> 2]) << 8 | 0));
  CHECK(out[1] == (0xff000000u | uint32_t(t.half[0x2e66 >> 2]) << 16 | 255u << 8 | 0));
  CHECK(baseFormat(kFormatBGRA) && baseFormat(kFormatRGhA) && baseFormat(kFormatB3A8) && !baseFormat('420v'));
  CHECK(bytesPerPixel(kFormatRGhA) == 8 && bytesPerPixel(kFormatBGRA) == 4 && bytesPerPixel(kFormatB3A8) == 4);
}

// --------------------------------------------------------------------------------------------------------------------- overlay
static void testOverlay()
{
  CHECK((glyphRect(128, 128, nullptr) == Rect{0, 0, 128, 128}));
  const uint32_t inside[6] = {16, 8, 32, 48, 20, 10}, past[6] = {120, 100, 32, 64, 121, 101}, outside[6] = {200, 0, 8, 8, 0, 0},
                 none[6] = {0, 0, 0, 0, 0, 0}, flat[6] = {4, 4, 0, 8, 4, 4}, hotOut[6] = {16, 8, 32, 48, 60, 10},
                 hotPast[6] = {120, 100, 32, 64, 121, 130};
  CHECK((glyphRect(128, 128, inside) == Rect{16, 8, 32, 48}));
  CHECK((glyphRect(128, 128, past) == Rect{120, 100, 8, 28}));
  CHECK((glyphRect(128, 128, outside) == Rect{0, 0, 128, 128}));
  CHECK((glyphRect(128, 128, none) == Rect{0, 0, 128, 128}));
  CHECK((glyphRect(128, 128, flat) == Rect{0, 0, 128, 128}));
  CHECK((glyphRect(128, 128, hotOut) == Rect{0, 0, 128, 128}));    // a hot spot outside the glyph: the words are not trusted
  CHECK((glyphRect(128, 128, hotPast) == Rect{0, 0, 128, 128}));   // ... nor one outside the part of it inside the surface
  CHECK(glyphRect(0, 0, inside).empty());
  int32_t hx = -1, hy = -1;
  hotSpot(inside, &hx, &hy);
  CHECK(hx == 20 && hy == 10);
  Overlay o;
  o.visible = true;
  o.glyph = {0, 0, 128, 128};
  o.x = -10;
  o.y = -20;
  CHECK((overlayOnPanel(o, 3200, 2136) == Rect{0, 0, 118, 108}));   // the top left corner
  o.x = 3150;
  o.y = 2100;
  CHECK((overlayOnPanel(o, 3200, 2136) == Rect{3150, 2100, 50, 36}));   // the bottom right corner
  o.x = -128;
  o.y = 500;
  CHECK(overlayOnPanel(o, 3200, 2136).empty());   // just off the left edge
  o.x = 3200;
  CHECK(overlayOnPanel(o, 3200, 2136).empty());   // just off the right edge
  o.x = 100;
  o.y = -128;
  CHECK(overlayOnPanel(o, 3200, 2136).empty());   // off the top
  o.y = 2136;
  CHECK(overlayOnPanel(o, 3200, 2136).empty());   // off the bottom
  o.x = INT32_MIN;
  o.y = INT32_MAX;
  CHECK(overlayOnPanel(o, 3200, 2136).empty());   // nonsense words: nothing, no overflow
  o.x = 1000;
  o.y = 600;
  o.glyph = {16, 8, 32, 48};
  CHECK((overlayOnPanel(o, 3200, 2136) == Rect{1016, 608, 32, 48}));   // only the glyph
  o.visible = false;
  CHECK(overlayOnPanel(o, 3200, 2136).empty());
  o.visible = true;
  o.x = -20;
  o.y = 0;
  o.glyph = {0, 0, 64, 64};
  const Rect on = overlayOnPanel(o, 3200, 2136);
  Span s = overlaySpan(o, on, 10, 0, 3200);
  CHECK(s.from == 0 && s.to == 44 && s.sx == 20 && s.sy == 10);
  s = overlaySpan(o, on, 10, 30, 40);
  CHECK(s.from == 30 && s.to == 40 && s.sx == 50 && s.sy == 10);
  CHECK(overlaySpan(o, on, 64, 0, 3200).empty() && overlaySpan(o, on, 10, 44, 3200).empty() && overlaySpan(o, Rect{}, 10, 0, 50).empty());
}

// A test surface: pixels and its RowSource.
struct TestSurface {
  uint32_t width = 0, height = 0, format = kFormatBGRA;
  uint64_t rowBytes = 0;
  std::vector<uint8_t> bytes;
  RowSource source() const {return {bytes.data(), rowBytes, width, height, format};}
  void make(uint32_t w, uint32_t h, uint32_t f, uint64_t pad)
  {
    width = w;
    height = h;
    format = f;
    rowBytes = uint64_t(w) * bytesPerPixel(f) + pad;
    bytes.assign(size_t(rowBytes * h), 0xcd);
  }
  uint32_t *word(uint32_t x, uint32_t y) {return reinterpret_cast<uint32_t *>(bytes.data() + y * rowBytes + uint64_t(x) * 4);}
  uint64_t *dword(uint32_t x, uint32_t y) {return reinterpret_cast<uint64_t *>(bytes.data() + y * rowBytes + uint64_t(x) * 8);}
};
// The reference: pixel by pixel from the definitions (base converted, black outside it, the glyph blended where it lies).
static uint32_t referencePixel(int32_t x, int32_t y, TestSurface *base, TestSurface *cursor, const Overlay &o, const Tables &t)
{
  uint32_t p = kBlack;
  if (base && x >= 0 && y >= 0 && uint32_t(x) < base->width && uint32_t(y) < base->height) {
    if (base->format == kFormatRGhA)
      convertSpan(kFormatRGhA, base->dword(x, y), &p, 1, t);
    else
      convertSpan(base->format, base->word(x, y), &p, 1, t);
  }
  if (cursor && o.visible) {
    const int64_t sx = int64_t(x) - o.x, sy = int64_t(y) - o.y;
    if (sx >= o.glyph.x && sx < o.glyph.right() && sy >= o.glyph.y && sy < o.glyph.bottom()) p = blendOver(p, *cursor->word(uint32_t(sx), uint32_t(sy)));
  }
  return p;
}
static void testCompose()
{
  static Tables t;
  t.init();
  std::mt19937 random(7);
  const uint32_t pw = 64, ph = 40;
  const uint32_t formats[3] = {kFormatBGRA, kFormatRGhA, kFormatB3A8};
  bool same = true;
  unsigned scenes = 0, blended = 0;
  for (int scene = 0; scene < 600; scene++) {
    TestSurface base, cursor;
    const uint32_t format = formats[scene % 3];
    const bool narrow = scene % 5 == 1, noBase = scene % 11 == 3;
    base.make(narrow ? 50 : pw, narrow ? 30 : ph, format, (scene % 4) * 16);
    for (uint32_t y = 0; y < base.height; y++)
      for (uint32_t x = 0; x < base.width; x++) {
        if (format == kFormatRGhA)
          *base.dword(x, y) = uint64_t(random()) << 32 | random();
        else
          *base.word(x, y) = format == kFormatBGRA ? (0xff000000u | (random() & 0xffffff)) : random();
      }
    cursor.make(16, 16, kFormatBGRA, (scene % 3) * 8);
    for (uint32_t y = 0; y < 16; y++)
      for (uint32_t x = 0; x < 16; x++) {
        const uint32_t a = random() % 4 == 0 ? 0 : random() % 4 == 0 ? 255 : random() % 256;
        uint32_t px = a << 24;
        for (unsigned shift = 0; shift < 24; shift += 8) px |= (a ? random() % (a + 1) : 0) << shift;
        *cursor.word(x, y) = px;
      }
    Overlay o;
    o.visible = scene % 9 != 4;
    o.x = int32_t(random() % (pw + 40)) - 20;
    o.y = int32_t(random() % (ph + 40)) - 20;
    uint32_t words[6] = {random() % 6, random() % 6, 4 + random() % 14, 4 + random() % 14, 0, 0};
    words[4] = words[0] + 1;
    words[5] = words[1] + 1;
    o.glyph = glyphRect(16, 16, scene % 2 ? words : nullptr);
    const Rect on = overlayOnPanel(o, pw, ph);
    const RowSource bs = base.source(), cs = cursor.source();
    // a whole panel, then a few rectangles of it (the damage path)
    std::vector<uint32_t> line(pw, 0x12345678);
    std::vector<uint8_t> scratch(pw * 8);
    for (int pass = 0; pass < 4; pass++) {
      Rect r = pass == 0 ? panelRect(pw, ph) : intersect(makeRect(random() % pw, random() % ph, random() % (pw + 1) + 1, random() % (ph + 1) + 1), panelRect(pw, ph));
      if (pass == 3) r = on.empty() ? panelRect(pw, ph) : intersect(makeRect(on.x - 3, on.y - 3, on.right() + 3, on.bottom() + 3), panelRect(pw, ph));
      if (r.empty()) continue;
      for (int32_t y = r.y; y < r.bottom(); y++) {
        composeRow(line.data(), scratch.data(), y, r.x, int32_t(r.right()), noBase ? nullptr : &bs, &cs, o, on, t);
        for (int32_t x = r.x; x < r.right(); x++)
          if (line[x] != referencePixel(x, y, noBase ? nullptr : &base, &cursor, o, t)) same = false;
      }
    }
    scenes++;
    if (!on.empty()) blended++;
  }
  CHECK(same);
  CHECK(scenes == 600 && blended > 200);
  // an overlay that is not 'BGRA' is not drawn; one whose geometry runs outside the surface is refused rather than read past
  TestSurface yuv;
  yuv.make(16, 16, '420v', 0);
  Overlay o;
  o.visible = true;
  o.glyph = {0, 0, 16, 16};
  const RowSource ys = yuv.source();
  uint32_t line[64];
  uint8_t scratch[512];
  for (auto &p : line) p = 0xff00ff00;
  CHECK(blendOverlayRow(line, scratch, 0, 0, 64, &ys, o, overlayOnPanel(o, 64, 40)) == 0 && line[0] == 0xff00ff00);
  TestSurface small;
  small.make(8, 8, kFormatBGRA, 0);
  const RowSource ss = small.source();
  CHECK(blendOverlayRow(line, scratch, 0, 0, 64, &ss, o, overlayOnPanel(o, 64, 40)) == 0);   // the glyph says 16 wide, the surface is 8
}

// --------------------------------------------------------------------------------------------------------------------- layers
static void testLayers()
{
  int a, b, c, cur, cur2;
  const void *A = &a, *B = &b, *C = &c, *K = &cur, *K2 = &cur2;
  LayerState s;
  SwapLayers w;
  w.mask = 0x7;   // WindowServer's first swap: the frame, layers 1 and 2 off
  w.surface[0] = A;
  LayerChange ch = applySwap(s, w);
  CHECK(ch.base && !ch.baseOff && !ch.overlay && ch.other && ch.taken[0] == A && !ch.released[0] && s.surface[0] == A);
  w = {};
  w.mask = 0x2;   // the cursor shows up at (100, 50)
  w.surface[1] = K;
  w.x1 = 100;
  w.y1 = 50;
  ch = applySwap(s, w);
  CHECK(!ch.base && ch.overlay && ch.taken[1] == K && s.surface[0] == A && s.surface[1] == K && s.x1 == 100 && s.y1 == 50);
  w.x1 = 110;   // it moves (same surface): a change, nothing taken or released
  ch = applySwap(s, w);
  CHECK(ch.overlay && !ch.taken[1] && !ch.released[1] && s.x1 == 110);
  w = {};
  w.mask = 0x1;   // a new frame; the cursor stays where it is
  w.surface[0] = B;
  ch = applySwap(s, w);
  CHECK(ch.base && !ch.overlay && ch.released[0] == A && ch.taken[0] == B && s.surface[1] == K && s.x1 == 110);
  w = {};
  w.mask = 0x3;   // a frame and the cursor hidden (typing): the position is not read
  w.surface[0] = C;
  w.x1 = 9999;
  ch = applySwap(s, w);
  CHECK(ch.base && ch.overlay && ch.released[0] == B && ch.released[1] == K && !s.surface[1] && s.x1 == 110);
  w = {};
  w.mask = 0x2;   // hidden again: nothing changes
  ch = applySwap(s, w);
  CHECK(!ch.overlay && !ch.base && !ch.released[1]);
  w.surface[1] = K2;   // another cursor image
  w.x1 = -30;
  w.y1 = -40;
  ch = applySwap(s, w);
  CHECK(ch.overlay && ch.taken[1] == K2 && s.x1 == -30 && s.y1 == -40);
  w = {};
  w.mask = 0x5;   // the display goes to sleep: layers 0 and 2 off
  ch = applySwap(s, w);
  CHECK(!ch.base && ch.baseOff && ch.other && ch.released[0] == C && !s.surface[0] && s.surface[1] == K2);
  ch = applySwap(s, w);   // off again: no change on layer 0
  CHECK(!ch.baseOff && !ch.released[0]);
  w = {};
  w.mask = 0x1;   // the same surface twice in a row is a new frame of it
  w.surface[0] = A;
  applySwap(s, w);
  ch = applySwap(s, w);
  CHECK(ch.base && !ch.taken[0] && !ch.released[0]);
}

// --------------------------------------------------------------------------------------------------------------------- jobs
static void testJobs()
{
  int a, b, k;
  const void *A = &a, *B = &b, *K = &k;
  Slot slot;
  Job displaced, j1, j2, r;
  j1.kind = Job::kFull;
  j1.base = A;
  j1.serial = 1;
  j1.fenced = true;
  j1.fence = 10;
  CHECK(offer(slot, j1, &displaced) == Offer::Taken && slot.busy);
  r.kind = Job::kFull;
  r.refresh = true;
  r.base = A;
  r.serial = 2;
  CHECK(offer(slot, r, &displaced) == Offer::Dropped && slot.job.serial == 1);   // a refresh never displaces a swap's job
  j2.kind = Job::kCursor;
  j2.base = A;
  j2.overlay = K;
  j2.serial = 3;
  CHECK(offer(slot, j2, &displaced) == Offer::Merged && displaced.serial == 1);
  CHECK(slot.job.kind == Job::kFull && slot.job.overlay == K && slot.job.serial == 3 && slot.job.fenced && slot.job.fence == 10 && !slot.job.refresh);
  Slot refreshSlot;
  offer(refreshSlot, r, &displaced);
  Job r2 = r;
  r2.serial = 4;
  r2.last = true;
  CHECK(offer(refreshSlot, r2, &displaced) == Offer::Merged && refreshSlot.job.refresh && refreshSlot.job.last);
  j2.serial = 5;
  CHECK(offer(refreshSlot, j2, &displaced) == Offer::Merged && !refreshSlot.job.refresh && !refreshSlot.job.last &&
        refreshSlot.job.kind == Job::kFull && refreshSlot.job.serial == 5);   // the swap's job takes the waiting refresh over
  // the completion rule
  Busy busy;
  LayerChange change;
  change.released[0] = A;
  Slot s2;
  Job next;
  next.kind = Job::kFull;
  next.base = B;
  next.serial = 7;
  Admission ad = admitSwap(s2, busy, next, change, kDeferWhenRead);
  CHECK(ad.hadJob && ad.offer == Offer::Taken && ad.waitSerial == 0);   // the copy thread idle: at once
  Job taken;
  CHECK(takeJob(s2, busy, &taken) && taken.serial == 7 && busy.serial == 7 && busy.base == B && !s2.busy);
  CHECK(!takeJob(s2, busy, &taken));
  LayerChange change2;
  change2.released[0] = B;   // the next swap takes B off the screen while the copy thread reads it
  Job third;
  third.kind = Job::kFull;
  third.base = A;
  third.serial = 8;
  ad = admitSwap(s2, busy, third, change2, kDeferWhenRead);
  CHECK(ad.waitSerial == 7);
  CHECK(admitSwap(s2, busy, third, change2, kDeferNever).waitSerial == 0);
  CHECK(admitSwap(s2, busy, third, change2, kDeferAlways).waitSerial == 8);
  LayerChange unrelated;
  unrelated.released[2] = K;
  CHECK(admitSwap(s2, busy, third, unrelated, kDeferWhenRead).waitSerial == 0);
  busy.overlay = K;   // the cursor surface it reads counts too
  CHECK(admitSwap(s2, busy, third, unrelated, kDeferWhenRead).waitSerial == 7);
  busy.overlay = nullptr;
  Job none;   // a swap with no job (layer 2 only) still waits when it releases what is read
  ad = admitSwap(s2, busy, none, change2, kDeferAlways);
  CHECK(!ad.hadJob && ad.waitSerial == 7);
  // a refresh in the way stops at its next row; a swap's job in the way is waited for
  Busy refreshing;
  refreshing.serial = 6;
  refreshing.base = B;
  refreshing.refresh = true;
  ad = admitSwap(s2, refreshing, third, change2, kDeferWhenRead);
  CHECK(ad.waitSerial == 6 && ad.abortBusy);
  ad = admitSwap(s2, refreshing, third, unrelated, kDeferWhenRead);
  CHECK(ad.waitSerial == 0 && !ad.abortBusy);   // nothing it reads is released: it finishes, the swap completes at once
  ad = admitSwap(s2, refreshing, third, unrelated, kDeferAlways);
  CHECK(ad.waitSerial == 8 && ad.abortBusy);    // the swap waits for its own job: the refresh before it stops
  CHECK(!admitSwap(s2, busy, third, change2, kDeferWhenRead).abortBusy);   // a frame's copy is never cut short
  Slot s3;
  Job r3;
  r3.kind = Job::kFull;
  r3.refresh = true;
  r3.serial = 11;
  offer(s3, r3, &displaced);
  Busy b3;
  CHECK(takeJob(s3, b3, &taken) && b3.refresh);
  uint64_t done = 0;
  CHECK(!finishJob(busy, &done, 0) && done == 7 && !busy.serial);
  Busy b2;
  b2.serial = 9;
  CHECK(finishJob(b2, &done, 9) && done == 9);
  b2.serial = 8;
  CHECK(!finishJob(b2, &done, 9) && done == 8);   // an earlier job: the swap waits on
}

// --------------------------------------------------------------------------------------------------------------------- panel
static void testPanel()
{
  int a, k;
  const void *A = &a, *K = &k;
  const Rect whole = panelRect(3200, 2136), cursorA{100, 100, 64, 64}, cursorB{110, 104, 64, 64};
  PanelState p;
  Job j;
  j.kind = Job::kCursor;   // a cursor before any frame: only its rectangle, never the whole panel over the boot console
  j.overlay = K;
  Plan plan = planJob(p, j, cursorA, whole);
  CHECK(plan.count == 1 && !plan.full && (plan.rect[0] == cursorA));
  commitJob(p, plan, false, cursorA);
  CHECK(!p.current && (p.cursor == cursorA));
  j.kind = Job::kFull;   // the first frame
  j.base = A;
  plan = planJob(p, j, cursorA, whole);
  CHECK(plan.count == 1 && plan.full && (plan.rect[0] == whole) && !plan.tryFlip && !plan.bootBack);
  commitJob(p, plan, false, cursorA);
  CHECK(p.current && (p.cursor == cursorA));
  j.kind = Job::kCursor;   // a move: the old and new rectangles only
  plan = planJob(p, j, cursorB, whole);
  CHECK(plan.count == 1 && !plan.full && (plan.rect[0] == unite(cursorA, cursorB)));
  commitJob(p, plan, false, cursorB);
  CHECK((p.cursor == cursorB));
  plan = planJob(p, j, Rect{}, whole);   // hidden: its old rectangle redrawn without it
  CHECK(plan.count == 1 && (plan.rect[0] == cursorB));
  commitJob(p, plan, false, Rect{});
  CHECK(p.cursor.empty());
  plan = planJob(p, j, Rect{}, whole);   // hidden -> hidden
  CHECK(plan.count == 0);
  Job none;
  CHECK(planJob(p, none, cursorA, whole).count == 0);
  // flip mode
  PanelState f;
  Job fj;
  fj.kind = Job::kFull;
  fj.base = A;
  fj.flip = true;
  plan = planJob(f, fj, Rect{}, whole);   // no cursor: offered to the DPU
  CHECK(plan.tryFlip && !plan.cursorForced && plan.bootBack && plan.full);
  commitJob(f, plan, true, Rect{});
  CHECK(f.flipped && !f.current && f.cursor.empty());
  Job refresh = fj;
  refresh.refresh = true;
  refresh.flip = false;
  CHECK(planJob(f, refresh, Rect{}, whole).count == 0);   // the DPU reads the flipped surface itself
  Job cj;
  cj.kind = Job::kCursor;
  cj.base = A;
  cj.overlay = K;
  cj.flip = true;
  plan = planJob(f, cj, cursorA, whole);   // the cursor appears over a flipped frame: the whole composition, the boot framebuffer back
  CHECK(!plan.tryFlip && plan.full && plan.bootBack && plan.cursorForced);
  commitJob(f, plan, false, cursorA);
  CHECK(!f.flipped && f.current && (f.cursor == cursorA));
  fj.overlay = K;
  plan = planJob(f, fj, cursorA, whole);   // a new frame with the cursor on: copied, not flipped
  CHECK(!plan.tryFlip && plan.cursorForced && plan.full);
  commitJob(f, plan, false, cursorA);
  plan = planJob(f, cj, cursorB, whole);   // the cursor moves over the copied frame: its rectangles only
  CHECK(!plan.full && plan.count == 1 && !plan.bootBack);
  commitJob(f, plan, false, cursorB);
  plan = planJob(f, cj, Rect{}, whole);   // it hides: erased in the boot framebuffer
  CHECK(plan.count == 1 && (plan.rect[0] == cursorB));
  commitJob(f, plan, false, Rect{});
  fj.overlay = nullptr;
  plan = planJob(f, fj, Rect{}, whole);   // the next frame flips again
  CHECK(plan.tryFlip);
  commitJob(f, plan, false, Rect{});   // refused (e.g. 'RGhA'): composed, the boot framebuffer back
  CHECK(!f.flipped && f.current);
  plan = planJob(f, fj, Rect{}, whole);
  commitJob(f, plan, true, Rect{});
  Job hidden = cj;
  hidden.overlay = nullptr;
  CHECK(planJob(f, hidden, Rect{}, whole).count == 0);   // still no cursor over the flipped frame
}

// --------------------------------------------------------------------------------------------------------------------- refresh
static void testRefresh()
{
  unsigned left = kRefreshTicks;
  std::vector<unsigned> due;
  bool last = false, sawLast = false;
  for (unsigned tick = 1; tick <= 40; tick++)
    if (refreshTick(left, &last)) {
      due.push_back(tick);
      if (last) sawLast = tick == 30;
    }
  CHECK((due == std::vector<unsigned>{1, 2, 4, 8, 16, 30}) && sawLast && left == 0);
  // The VBL tick while the display is off: it stops only dark, with nothing queued, waiting, refreshed or retried, and only with the
  // argument; every other combination keeps it.
  for (int stop = 0; stop < 2; stop++)
    for (int dark = 0; dark < 2; dark++)
      for (unsigned refresh : {0u, 1u, kRefreshTicks})
        for (int unfinished = 0; unfinished < 2; unfinished++)
          for (int waiting = 0; waiting < 2; waiting++)
            for (int queued = 0; queued < 2; queued++)
              CHECK(vblStops(stop, dark, refresh, unfinished, waiting, queued) ==
                    (stop && dark && !refresh && !unfinished && !waiting && !queued));
  // The queue at a tick: a swap behind one that waits for its copy is back-pressure, not a stall; only one with none current is.
  CHECK(queueTick(false, false) == QueueTick::Idle && queueTick(false, true) == QueueTick::Idle);
  CHECK(queueTick(true, true) == QueueTick::Behind && queueTick(true, false) == QueueTick::Stalled);
  CHECK(!refreshNeeded(kRefreshOff, false, true));
  CHECK(refreshNeeded(kRefreshUncovered, false, false) && !refreshNeeded(kRefreshUncovered, true, false) && refreshNeeded(kRefreshUncovered, true, true));
  CHECK(refreshNeeded(kRefreshAlways, true, false));
  // the fence rule: never a frame the GPU has not finished
  Job frame;
  frame.kind = Job::kFull;
  frame.fenced = true;
  CHECK(verdict(frame, true, kRefreshUncovered, true, true) == Verdict::Draw);
  CHECK(verdict(frame, false, kRefreshUncovered, false, false) == Verdict::Unfinished);
  frame.fenced = false;   // no GPU fences (no PianoA830): drawn at once, as before
  CHECK(verdict(frame, false, kRefreshUncovered, false, true) == Verdict::Draw);
  Job again;
  again.kind = Job::kFull;
  again.refresh = true;
  CHECK(verdict(again, true, kRefreshUncovered, true, true) == Verdict::NotNeeded);
  CHECK(verdict(again, true, kRefreshUncovered, false, true) == Verdict::Draw);
  CHECK(verdict(again, true, kRefreshUncovered, false, false) == Verdict::Unfinished);   // the frame on screen still drawn
  again.last = true;
  CHECK(verdict(again, true, kRefreshUncovered, true, true) == Verdict::Draw);
  int a, k;
  Pipeline pipe;
  SwapLayers w;
  w.mask = 1;
  w.surface[0] = &a;
  LayerChange ch;
  Job job = pipe.swapJob(w, true, 5, false, &ch);
  CHECK(job.kind == Job::kFull && job.serial == 1 && job.fenced && job.fence == 5 && job.base == &a && pipe.refreshLeft == kRefreshTicks);
  pipe.pending = true;
  Job r;
  CHECK(!pipe.refreshJob(&r) && pipe.refreshLeft == kRefreshTicks && !pipe.forcedRefreshJob(&r));   // nothing while a swap waits
  pipe.swapDone(false);
  CHECK(pipe.refreshJob(&r) && r.refresh && !r.last && r.kind == Job::kFull && r.serial == 2 && !r.fenced && !r.flip);
  w = {};
  w.mask = 2;
  w.surface[1] = &k;
  w.x1 = 5;
  job = pipe.swapJob(w, false, 0, false, &ch);
  CHECK(job.kind == Job::kCursor && job.overlay == &k && job.base == &a && job.x1 == 5 && pipe.refreshLeft == kRefreshTicks - 1);
  w = {};
  w.mask = 4;   // layer 2 only: no job
  job = pipe.swapJob(w, false, 0, false, &ch);
  CHECK(job.kind == Job::kNone && job.serial == 0 && ch.other);
  pipe.swapDone(true);   // a flipped frame: no refresh copies
  CHECK(pipe.refreshLeft == 0 && !pipe.refreshJob(&r) && pipe.forcedRefreshJob(&r) && r.last);
  CHECK(!pipe.retryJob(false, &r) && pipe.retryJob(true, &r) && r.refresh && r.last && r.base == &a);
  w = {};
  w.mask = 1;   // layer 0 off: a full job (black), the countdown ends
  pipe.refreshLeft = 7;
  job = pipe.swapJob(w, false, 0, false, &ch);
  CHECK(job.kind == Job::kFull && !job.base && pipe.refreshLeft == 0 && !pipe.refreshJob(&r) && !pipe.forcedRefreshJob(&r));
}

// ------------------------------------------------------------------------------------------------------------------ simulation
namespace sim {
static constexpr int64_t kVbl = 16667;   // us
static constexpr uint32_t kGarbage = 0x01ffffffu;   // what a surface holds while the GPU draws it (white, alpha 1: the bands)

struct Surface {
  int id = 0;
  TestSurface pixels;
  bool cursor = false;
  bool writing = false;
  bool claimed = false;   // WindowServer is drawing it for a swap not yet submitted
  int64_t readFrom = -1, readUntil = -1;
  int frame = 0;
};
struct Config {
  uint32_t defer = kDeferAlways;
  uint32_t policy = kRefreshUncovered;
  bool flipMode = false;
  int64_t gpuMin = 2000, gpuMax = 15000;   // GPU time of a frame
  int64_t frameMin = 10000, frameMax = 60000;
  unsigned buffers = 3;                    // WindowServer's framebuffers in turn (the least recently used free one is drawn)
  int64_t fenceTimeout = 50000;           // the hang guard of the fence wait
  int64_t fullCopy = 20000, cursorCopy = 500;
  int64_t duration = 3000000;
  unsigned seed = 3;
  bool flipRefusesSome = false;
};
struct Result {
  unsigned torn = 0, garbage = 0, idleChecks = 0, idleMismatches = 0, timeouts = 0, frames = 0, swaps = 0, deferred = 0;
  unsigned copies = 0, cursorJobs = 0, skippedRefresh = 0, flips = 0, flippedWithCursor = 0;
  unsigned unfinished = 0, retries = 0, aborted = 0, earlyCompletions = 0;
  unsigned behind = 0, stalled = 0;   // swap_begin_gated left a swap queued behind a waiting one / with none current
  bool finalMatches = false;
};

class World {
public:
  explicit World(const Config &c) : cfg(c), random(c.seed)
  {
    tables.init();
    for (int i = 0; i < int(cfg.buffers); i++) {
      Surface s;
      s.id = i;
      s.pixels.make(kW, kH, kFormatBGRA, 0);
      fill(s, 0);
      surfaces.push_back(s);
    }
    for (int i = 0; i < 2; i++) {
      Surface s;
      s.id = 10 + i;
      s.cursor = true;
      s.pixels.make(16, 16, kFormatBGRA, 0);
      drawCursor(s, i);
      surfaces.push_back(s);
    }
    panel.assign(kW * kH, 0xff203050);
  }
  Result run()
  {
    at(0, [this] {wsFrame();});
    at(1000, [this] {wsCursor();});
    armVbl();
    while (!events.empty()) {
      const Event e = events.top();
      events.pop();
      now = e.time;
      e.action();
      checkIdle();
      if (now > cfg.duration + 3000000) break;
    }
    result.finalMatches = idle() && panelMatches();
    return result;
  }

private:
  static constexpr uint32_t kW = 48, kH = 32;
  struct Event {
    int64_t time;
    uint64_t order;
    std::function<void()> action;
    bool operator<(const Event &o) const {return time != o.time ? time > o.time : order > o.order;}
  };
  struct FamilySwap {
    SwapLayers layers;
  };
  Config cfg;
  std::mt19937 random;
  Tables tables;
  std::vector<Surface> surfaces;
  std::vector<uint32_t> panel;
  std::priority_queue<Event> events;
  uint64_t order = 0;
  int64_t now = 0;
  Result result;
  // the family
  std::deque<FamilySwap> queue;
  bool hasCurrent = false;
  FamilySwap current;
  LayerState shown;   // what the family has on screen (completed swaps)
  // NativeGopDisplay
  Pipeline pipe;
  Slot slot;
  Busy busy;
  uint64_t doneSerial = 0;
  PanelState panelState;
  bool frameCovered = false;
  bool copyBusy = false;
  uint64_t vblGeneration = 0;
  unsigned framesThisCall = 0;
  const void *flippedSurface = nullptr;
  // the GPU
  uint32_t submitted = 0, retired = 0;
  int64_t gpuBusyUntil = 0;
  std::map<uint32_t, int64_t> retireTime;
  // WindowServer
  int frameNumber = 0;
  int cursorX = -20, cursorY = 3, cursorDx = 3, cursorDy = 2, cursorImage = 0;
  bool cursorVisible = true;
  std::vector<int64_t> lastUse = std::vector<int64_t>(8, 0);

  void at(int64_t t, std::function<void()> f) {events.push({t, order++, std::move(f)});}
  Surface &surf(const void *p) {return *static_cast<Surface *>(const_cast<void *>(p));}
  static uint32_t pattern(int id, int frame, uint32_t x, uint32_t y)
  {
    return 0xff000000u | ((uint32_t(frame) * 2654435761u + x * 40503u + y * 2246822519u + uint32_t(id) * 97u) & 0x00fefefe);
  }
  void fill(Surface &s, int frame)
  {
    s.frame = frame;
    for (uint32_t y = 0; y < s.pixels.height; y++)
      for (uint32_t x = 0; x < s.pixels.width; x++) *s.pixels.word(x, y) = pattern(s.id, frame, x, y);
  }
  void drawCursor(Surface &s, int image)
  {
    for (uint32_t y = 0; y < 16; y++)
      for (uint32_t x = 0; x < 16; x++) {
        const bool glyph = x >= 2 && x < 14 && y >= 1 && y < 15;
        const uint32_t a = glyph ? (x + y + image) % 3 == 0 ? 255 : (x * 16 + y * 5) % 200 : 0;
        *s.pixels.word(x, y) = glyph ? (a << 24 | (a / 2) << 16 | (a / 3) << 8 | a / 4) : 0xff00ff00;   // outside the glyph: opaque green
      }
  }
  void markWrite(Surface &s)
  {
    if (now >= s.readFrom && now < s.readUntil) result.torn++;   // the copy thread is reading it
    if (&s == flippedSurface) result.torn++;                      // the DPU scans it
  }
  // ---------------------------------------------------------------- WindowServer
  bool inUse(const Surface *s)
  {
    for (unsigned i = 0; i < kLayers; i++)
      if (shown.surface[i] == s) return true;
    if (hasCurrent)
      for (unsigned i = 0; i < kLayers; i++)
        if (current.layers.surface[i] == s) return true;
    for (const FamilySwap &q : queue)
      for (unsigned i = 0; i < kLayers; i++)
        if (q.layers.surface[i] == s) return true;
    return s->claimed || s->writing;
  }
  void wsFrame()
  {
    if (now > cfg.duration) return;
    Surface *pick = nullptr;
    for (int i = 0; i < int(cfg.buffers); i++)
      if (!inUse(&surfaces[i]) && (!pick || lastUse[i] < lastUse[pick->id])) pick = &surfaces[i];
    if (pick) {
      Surface &s = *pick;
      lastUse[s.id] = now;
      s.claimed = true;
      const int frame = ++frameNumber;
      const int64_t start = std::max(now, gpuBusyUntil), end = start + cfg.gpuMin + int64_t(random() % (cfg.gpuMax - cfg.gpuMin + 1));
      gpuBusyUntil = end;
      const uint32_t fence = ++submitted;
      retireTime[fence] = end;
      at(start, [this, &s] {
        markWrite(s);
        s.writing = true;
        for (uint32_t y = 0; y < kH; y++)
          for (uint32_t x = 0; x < kW; x++) *s.pixels.word(x, y) = kGarbage;
      });
      at(end, [this, &s, frame, fence] {
        fill(s, frame);
        s.writing = false;
        retired = std::max(retired, fence);
        wakeFence();
      });
      // the swap goes when the command buffer is scheduled, before the GPU finished it
      at(now + 200, [this, &s] {
        s.claimed = false;
        SwapLayers l;
        l.mask = 1;
        l.surface[0] = &s;
        submit(l);
      });
      result.frames++;
    }
    at(now + cfg.frameMin + int64_t(random() % (cfg.frameMax - cfg.frameMin + 1)), [this] {wsFrame();});
  }
  void wsCursor()
  {
    if (now > cfg.duration) return;
    SwapLayers l;
    l.mask = 2;
    const unsigned roll = random() % 40;
    if (roll == 0) cursorVisible = !cursorVisible;
    if (roll == 1) {   // another image, drawn by the CPU into the cursor surface not in use
      Surface &next = surfaces[cfg.buffers + (cursorImage ^ 1)];
      if (!inUse(&next)) {
        markWrite(next);
        drawCursor(next, int(random() % 5));
        cursorImage ^= 1;
      }
    }
    cursorX += cursorDx;
    cursorY += cursorDy;
    if (cursorX < -20 || cursorX > int(kW) + 4) cursorDx = -cursorDx;
    if (cursorY < -20 || cursorY > int(kH) + 4) cursorDy = -cursorDy;
    l.surface[1] = cursorVisible ? &surfaces[cfg.buffers + cursorImage] : nullptr;
    l.x1 = cursorX;
    l.y1 = cursorY;
    submit(l);
    at(now + 8000, [this] {wsCursor();});
  }
  // ---------------------------------------------------------------- the family
  void submit(const SwapLayers &l)
  {
    queue.push_back({l});
    result.swaps++;
    swapBegin();
  }
  void complete()
  {
    // the rule of defer=2 (and flip mode): the swap's own job is done before the swap completes
    if ((cfg.defer == kDeferAlways || cfg.flipMode) && currentJob && doneSerial < currentJob) result.earlyCompletions++;
    for (unsigned i = 0; i < kLayers; i++)
      if (current.layers.mask >> i & 1) shown.surface[i] = current.layers.surface[i];
    if (current.layers.mask & 2 && current.layers.surface[1]) {
      shown.x1 = current.layers.x1;
      shown.y1 = current.layers.y1;
    }
    hasCurrent = false;
  }
  // ---------------------------------------------------------------- NativeGopDisplay (swap_begin_gated, program_hardware_gated)
  void swapBegin()
  {
    framesThisCall = 0;
    while (!hasCurrent && !queue.empty()) {
      current = queue.front();
      queue.pop_front();
      hasCurrent = true;
      program();
    }
    // the periodic line's counters, as swap_begin_gated keeps them
    const QueueTick q = queueTick(!queue.empty(), hasCurrent);
    if (q == QueueTick::Behind) result.behind++;
    if (q == QueueTick::Stalled) result.stalled++;
    Job r;
    Job displaced;
    if (!framesThisCall && pipe.refreshJob(&r)) {
      offer(slot, r, &displaced);
      kick();
    }
    if (pipe.retryJob(unfinished, &r)) {   // a swap's job was left undrawn: the whole panel again once the GPU is done
      offer(slot, r, &displaced);
      result.retries++;
      kick();
    }
    armVbl();
  }
  void armVbl()
  {
    const uint64_t generation = ++vblGeneration;
    at(now + kVbl, [this, generation] {
      if (generation == vblGeneration) swapBegin();
    });
  }
  void program()
  {
    LayerChange change;
    const Job job = pipe.swapJob(current.layers, true, submitted, cfg.flipMode, &change);
    currentJob = job.kind != Job::kNone ? job.serial : 0;
    if (change.base) framesThisCall++;
    const Admission a = admitSwap(slot, busy, job, change, cfg.flipMode ? uint32_t(kDeferAlways) : cfg.defer);
    if (a.abortBusy) abortRefresh();
    if (a.waitSerial && doneSerial < a.waitSerial) {
      pipe.pending = true;
      pipe.waitSerial = a.waitSerial;
      result.deferred++;
    } else {
      complete();
    }
    kick();
  }
  void jobDone(bool flipped)
  {
    if (!pipe.pending || doneSerial < pipe.waitSerial) return;
    pipe.swapDone(flipped);
    complete();
    swapBegin();
  }
  // ---------------------------------------------------------------- the copy thread
  bool waitingFence = false;
  uint64_t fenceGeneration = 0, finishGeneration = 0, currentJob = 0;
  uint32_t fenceWanted = 0;
  Job job;
  bool unfinished = false;     // a swap's job was left undrawn (its fence had not passed): the ticks retry
  bool needFenced = false;     // the fence the panel's state needs: the newest swap job's
  uint32_t needFence = 0;
  bool refreshRunning = false, refreshAborted = false;   // a refresh being drawn: composed when it ends, as far as it got
  int64_t refreshStart = 0, refreshCost = 0;
  Plan refreshPlan;
  Overlay refreshOverlay;
  Rect refreshOn;
  Surface *refreshReads[2] = {};
  void kick()
  {
    if (copyBusy) return;
    if (!takeJob(slot, busy, &job)) return;
    copyBusy = true;
    if (job.fenced && retired < job.fence) {   // waitGpuFence: until retired, at most the hang guard
      waitingFence = true;
      fenceWanted = job.fence;
      const uint64_t generation = ++fenceGeneration;
      at(now + cfg.fenceTimeout, [this, generation] {
        if (waitingFence && generation == fenceGeneration) {
          waitingFence = false;
          result.timeouts++;
          draw(false);
        }
      });
      return;
    }
    draw(true);
  }
  void wakeFence()
  {
    if (waitingFence && retired >= fenceWanted) {
      waitingFence = false;
      ++fenceGeneration;
      draw(true);
    }
  }
  Overlay overlayFor(const Job &j, Surface **cursor)
  {
    Overlay o;
    *cursor = j.overlay ? &surf(j.overlay) : nullptr;
    if (*cursor) {
      o.visible = true;
      o.x = j.x1;
      o.y = j.y1;
      const uint32_t words[6] = {2, 1, 12, 14, 3, 2};
      o.glyph = glyphRect(16, 16, words);
    }
    return o;
  }
  // The plan's rows into the panel, the first rows of them only (a refresh stopped part way).
  void composeRows(const Plan &plan, const Job &j, const Overlay &o, const Rect &on, int64_t rows)
  {
    Surface *base = j.base ? &surf(j.base) : nullptr, *cursor = j.overlay ? &surf(j.overlay) : nullptr;
    const RowSource bs = base ? base->pixels.source() : RowSource(), cs = cursor ? cursor->pixels.source() : RowSource();
    std::vector<uint32_t> line(kW);
    std::vector<uint8_t> scratch(kW * 8);
    for (unsigned i = 0; i < plan.count; i++) {
      const Rect r = plan.rect[i];
      for (int32_t y = r.y; y < r.bottom() && rows > 0; y++, rows--) {
        composeRow(line.data(), scratch.data(), y, r.x, int32_t(r.right()), base ? &bs : nullptr, cursor ? &cs : nullptr, o, on, tables);
        memcpy(&panel[size_t(y) * kW + r.x], &line[r.x], size_t(r.w) * 4);
      }
    }
  }
  void draw(bool retiredNow)
  {
    const bool covered = job.fenced && retiredNow;
    if (job.kind == Job::kFull && !job.refresh) frameCovered = covered;
    if (!job.refresh) {
      needFenced = job.fenced;
      needFence = job.fence;
    }
    const Verdict v = verdict(job, retiredNow, cfg.policy, frameCovered, !needFenced || retired >= needFence);
    int64_t cost = 0;
    bool flipped = false;
    if (v == Verdict::NotNeeded) result.skippedRefresh++;
    if (v == Verdict::Unfinished) {   // never a frame the GPU is drawing: the previous composition stays, the ticks retry
      if (!job.refresh) {
        result.unfinished++;
        panelState.current = false;
      }
      unfinished = true;
    }
    if (v == Verdict::Draw) {
      Surface *cursor = nullptr;
      const Overlay o = overlayFor(job, &cursor);
      const Rect on = overlayOnPanel(o, kW, kH);
      const Plan plan = planJob(panelState, job, on, panelRect(kW, kH));
      if (plan.tryFlip) {
        flipped = !(cfg.flipRefusesSome && random() % 4 == 0);
        if (flipped) {
          flippedSurface = job.base;
          result.flips++;
          cost = 300;
        }
      }
      if (!flipped && plan.count) {
        Surface *base = job.base ? &surf(job.base) : nullptr;
        cost = plan.full ? cfg.fullCopy : cfg.cursorCopy;
        unsigned reads = 0;
        for (Surface *s : {base, cursor}) {
          if (!s) continue;
          if (s->writing) result.garbage++;   // read while the GPU draws it
          s->readFrom = now;
          s->readUntil = now + cost;
          refreshReads[reads++] = s;
        }
        if (job.refresh) {
          refreshRunning = true;
          refreshAborted = false;
          refreshStart = now;
          refreshCost = cost;
          refreshPlan = plan;
          refreshOverlay = o;
          refreshOn = on;
          if (reads < 2) refreshReads[reads] = nullptr;
        } else {
          composeRows(plan, job, o, on, INT64_MAX);
          commitJob(panelState, plan, false, on);
          if (plan.full) unfinished = false;
        }
        if (plan.full) result.copies++; else result.cursorJobs++;
        if (plan.bootBack) flippedSurface = nullptr;
      } else {
        commitJob(panelState, plan, flipped, on);
        if (flipped) unfinished = false;
      }
    }
    finishAt(now + cost, flipped);
  }
  void finishAt(int64_t t, bool flipped)
  {
    const uint64_t generation = ++finishGeneration;
    at(t, [this, generation, flipped] {
      if (generation != finishGeneration) return;
      if (refreshRunning) {
        int64_t total = 0;
        for (unsigned i = 0; i < refreshPlan.count; i++) total += refreshPlan.rect[i].h;
        const int64_t elapsed = now - refreshStart, rows = elapsed >= refreshCost ? total : total * elapsed / refreshCost;
        composeRows(refreshPlan, job, refreshOverlay, refreshOn, rows);
        if (refreshAborted) {
          abortedJob(panelState, refreshPlan, refreshOn);
        } else {
          commitJob(panelState, refreshPlan, false, refreshOn);
          if (refreshPlan.full) unfinished = false;
        }
        refreshRunning = false;
      }
      const bool signal = finishJob(busy, &doneSerial, pipe.pending ? pipe.waitSerial : 0);
      copyBusy = false;
      job = Job();
      if (signal) jobDone(flipped);
      kick();
    });
  }
  // admitSwap's abortBusy: the refresh in the way stops at its next row (here: 20 us from now)
  void abortRefresh()
  {
    if (!refreshRunning || refreshAborted) return;
    refreshAborted = true;
    result.aborted++;
    for (Surface *s : refreshReads)
      if (s && s->readUntil > now + 20) s->readUntil = now + 20;
    finishAt(now + 20, false);
  }
  // ---------------------------------------------------------------- checks
  bool idle() const
  {
    if (copyBusy || slot.busy || hasCurrent || !queue.empty() || pipe.pending || gpuBusyUntil > now || waitingFence || unfinished)
      return false;
    for (const Surface &s : surfaces)
      if (s.writing || s.claimed) return false;
    return true;
  }
  bool panelMatches()
  {
    // what the screen must show: the family's layers (ours equal them when nothing is pending)
    Surface *base = shown.surface[0] ? &surf(shown.surface[0]) : nullptr, *cursor = shown.surface[1] ? &surf(shown.surface[1]) : nullptr;
    Overlay o;
    if (cursor) {
      o.visible = true;
      o.x = shown.x1;
      o.y = shown.y1;
      const uint32_t words[6] = {2, 1, 12, 14, 3, 2};
      o.glyph = glyphRect(16, 16, words);
    }
    if (panelState.flipped) {   // the DPU shows the base itself: only while no cursor is on the panel (one entirely off it is not)
      const bool cursorOn = !overlayOnPanel(o, kW, kH).empty();
      if (cursorOn) result.flippedWithCursor++;
      return flippedSurface == shown.surface[0] && !cursorOn;
    }
    if (!base && !panelState.current) return true;   // nothing drawn yet
    for (uint32_t y = 0; y < kH; y++)
      for (uint32_t x = 0; x < kW; x++)
        if (panel[y * kW + x] != referencePixel(int32_t(x), int32_t(y), base ? &base->pixels : nullptr, cursor ? &cursor->pixels : nullptr, o, tables))
          return false;
    return true;
  }
  void checkIdle()
  {
    if (!idle() || pipe.refreshLeft) return;
    result.idleChecks++;
    if (!panelMatches()) result.idleMismatches++;
  }
};
}  // namespace sim

static void testSimulation()
{
  using sim::Config;
  using sim::Result;
  using sim::World;
  unsigned aborted = 0;
  for (unsigned seed = 1; seed <= 6; seed++) {
    for (unsigned buffers : {2u, 3u}) {
      for (uint32_t mode = 0; mode < 4; mode++) {
        const uint32_t defer = mode & 1 ? kDeferAlways : kDeferWhenRead;
        Config c;
        c.defer = defer;
        c.policy = mode & 2 ? kRefreshAlways : kRefreshUncovered;   // with every refresh copy, many are in a swap's way
        c.seed = seed;
        c.buffers = buffers;
        const Result r = World(c).run();
        CHECK(r.torn == 0);
        CHECK(r.garbage == 0);
        CHECK(r.timeouts == 0 && r.unfinished == 0);
        CHECK(r.earlyCompletions == 0);
        CHECK(r.idleChecks > 3 && r.idleMismatches == 0);
        CHECK(r.finalMatches);
        CHECK(r.frames > 30 && r.cursorJobs > 5 && r.deferred > 0);
        // queue6 (2026-11-12): swaps faster than the copies queue behind the one that waits ("behind"), the queue never stalls
        CHECK(r.stalled == 0);
        if (defer == kDeferAlways) CHECK(r.behind > 0);
        aborted += r.aborted;
        if (seed == 1)
          std::printf("sim %u buffers, defer=%u refresh=%u: %u frames, %u swaps, %u deferred, %u full copies, %u cursor updates, %u "
                      "refreshes skipped, %u cut short, %u idle checks, %u behind, %u stalled\n", buffers, defer, c.policy, r.frames, r.swaps,
                      r.deferred, r.copies, r.cursorJobs, r.skippedRefresh, r.aborted, r.idleChecks, r.behind, r.stalled);
      }
      // the race of 2026-11-11: completing every swap at once lets WindowServer draw into a surface the copy thread is reading
      Config old;
      old.defer = kDeferNever;
      old.policy = kRefreshAlways;
      old.seed = seed;
      old.buffers = buffers;
      const Result r = World(old).run();
      if (buffers == 2) CHECK(r.torn > 0);   // with two framebuffers the one handed back is drawn next, at once
      CHECK(r.deferred == 0);
      CHECK(r.behind == 0 && r.stalled == 0);   // every swap completes at once: nothing ever waits behind one
      CHECK(r.garbage == 0);    // the fence rule holds in every mode
      CHECK(r.finalMatches);    // the refresh copies hide it once everything stops
      if (seed == 1)
        std::printf("sim %u buffers, defer=0 (before 2026-11-12): %u torn reads in %u frames\n", buffers, r.torn, r.frames);
    }
  }
  CHECK(aborted > 0);   // refresh copies in the way of a swap were cut short
  // a slow GPU (frames of 30-90 ms, the fence wait's hang guard at 50 ms): a frame is never read before the GPU finished it (the
  // previous one stays), the swap completes all the same, the ticks retry, and the finished frame ends on the panel
  for (unsigned seed = 1; seed <= 3; seed++) {
    for (uint32_t defer : {uint32_t(kDeferWhenRead), uint32_t(kDeferAlways)}) {
      Config c;
      c.seed = seed;
      c.defer = defer;
      c.gpuMin = 30000;
      c.gpuMax = 90000;
      c.frameMin = 20000;
      c.frameMax = 80000;
      const Result r = World(c).run();
      CHECK(r.timeouts > 0 && r.unfinished > 0 && r.retries > 0);
      CHECK(r.garbage == 0);
      CHECK(r.torn == 0);
      CHECK(r.stalled == 0);   // a slow GPU holds swaps behind the waiting one (defer=2), it never stalls the queue
      if (defer == kDeferAlways) CHECK(r.behind > 0);
      CHECK(r.finalMatches);
      CHECK(r.idleMismatches == 0);
      if (seed == 1)
        std::printf("sim slow GPU, defer=%u: %u fence waits hit the guard, %u frames kept back, %u tick retries, %u reads of a frame "
                    "in progress\n", defer, r.timeouts, r.unfinished, r.retries, r.garbage);
    }
  }
  // flip mode: swaps wait for their own job; a cursor on the panel forces the copy; never a flipped surface under a visible cursor
  for (unsigned seed = 1; seed <= 4; seed++) {
    Config c;
    c.flipMode = true;
    c.seed = seed;
    c.flipRefusesSome = seed % 2 == 0;
    const Result r = World(c).run();
    CHECK(r.torn == 0 && r.garbage == 0);
    CHECK(r.flippedWithCursor == 0);
    CHECK(r.earlyCompletions == 0);
    CHECK(r.idleMismatches == 0 && r.finalMatches);
    CHECK(r.flips > 0 && r.copies > 0);
    CHECK(r.stalled == 0 && r.behind > 0);   // flip mode always waits for the swap's own job
    if (seed == 1) std::printf("sim flip mode: %u flips, %u copies (cursor on or refused), %u cursor updates\n", r.flips, r.copies, r.cursorJobs);
  }
}

// ------------------------------------------------------------------------------------------- the panel before the first frame
// A model of XNU's text console as it paints the boot framebuffer (the rules, from osfmk/console/video_console.c's behaviour; a
// synthetic font): the panel cleared to the background when it starts; a new character painted whole (all 8x16 pixels); a line feed
// on the last row scrolls, repainting only the cells whose character changed and in them only the pixels where the new glyph differs
// from the old one; the freed last row clears only the old glyphs' pixels. start() paints by startPlan at some point in the boot.
struct ConsoleModel {
  static constexpr int kCols = 60, kRows = 20, kCw = 8, kCh = 16;
  static constexpr uint32_t kFore = 0xffffffffu, kBack = 0;   // the console's white and black (vc_color_fore, vc_color_back)
  std::vector<uint8_t> glyph = std::vector<uint8_t>(256 * kCw * kCh);   // [ch][y][x], 1: a stroke
  std::vector<uint32_t> panel = std::vector<uint32_t>(kCols * kCw * kRows * kCh, kBack);
  std::vector<uint8_t> cells = std::vector<uint8_t>(kCols * kRows, ' ');
  int x = 0, y = 0;
  explicit ConsoleModel(uint32_t seed)
  {
    std::mt19937 rng(seed);
    for (int ch = 33; ch < 127; ch++)   // ' ' has no stroke; glyphs in a 5x9 box share stems and bars, as a font's do
      for (int py = 3; py < 12; py++)
        for (int px = 1; px < 6; px++)
          glyph[(ch * kCh + py) * kCw + px] = (px == 1 || px == 5 || py == 3 || py == 7 || py == 11) ? rng() % 3 != 0 : rng() % 8 == 0;
  }
  uint32_t &pixel(int col, int row, int px, int py) {return panel[(row * kCh + py) * kCols * kCw + col * kCw + px];}
  bool on(uint8_t ch, int px, int py) const {return glyph[(ch * kCh + py) * kCw + px] != 0;}
  void paint(int col, int row, uint8_t ch, int previous)   // previous < 0: the whole cell
  {
    for (int py = 0; py < kCh; py++)
      for (int px = 0; px < kCw; px++)
        if (previous < 0 || on(ch, px, py) != on(uint8_t(previous), px, py))
          pixel(col, row, px, py) = on(ch, px, py) ? kFore : kBack;
  }
  void scroll()
  {
    for (int row = 0; row + 1 < kRows; row++)
      for (int col = 0; col < kCols; col++) {
        uint8_t &cell = cells[row * kCols + col];
        const uint8_t below = cells[(row + 1) * kCols + col];
        if (cell != below) {
          paint(col, row, below, cell);
          cell = below;
        }
      }
    for (int col = 0; col < kCols; col++) {
      uint8_t &cell = cells[(kRows - 1) * kCols + col];
      if (cell != ' ') {
        paint(col, kRows - 1, ' ', cell);
        cell = ' ';
      }
    }
  }
  void newline()
  {
    if (y == kRows - 1) scroll();
    else y++;
    x = 0;
  }
  void put(uint8_t ch)
  {
    if (ch == '\n') return newline();
    if (x >= kCols) newline();
    cells[y * kCols + x] = ch;
    paint(x, y, ch, -1);
    x++;
  }
  void line(std::mt19937 &rng)   // a log line: words of 1-12 characters, sometimes longer than a row (it wraps)
  {
    const unsigned length = rng() % 4 == 0 ? rng() % (2 * kCols) : rng() % kCols;
    for (unsigned i = 0; i < length; i++)
      put(rng() % 7 == 0 ? ' ' : uint8_t(33 + rng() % 94));
    put('\n');
  }
  void start(const StartPlan &plan)   // NativeGopDisplay::start, as the kernel does it (bars: not modelled)
  {
    if (plan.kind == StartFill::Black || plan.kind == StartFill::Colour)
      for (uint32_t &p : panel) p = plan.pixel;
  }
  uint64_t differences() const   // pixels not as the console means them (its cells drawn whole); the scan-out ignores the alpha byte
  {
    uint64_t n = 0;
    for (int row = 0; row < kRows; row++)
      for (int col = 0; col < kCols; col++)
        for (int py = 0; py < kCh; py++)
          for (int px = 0; px < kCw; px++)
            n += ((panel[(row * kCh + py) * kCols * kCw + col * kCw + px] ^ (on(cells[row * kCols + col], px, py) ? kFore : kBack)) &
                  0xffffffu) != 0;
    return n;
  }
};

static void testStartFill()
{
  // the rule: bars, then an asked-for colour, then a text console kept, else black
  for (int bars = 0; bars < 2; bars++)
    for (int given = 0; given < 2; given++)
      for (int text = 0; text < 2; text++) {
        const StartPlan p = startPlan(bars, given, 0x123456, text);
        if (bars) CHECK(p.kind == StartFill::Bars);
        else if (given) CHECK(p.kind == StartFill::Colour && p.pixel == 0xff123456);
        else if (text) CHECK(p.kind == StartFill::Keep);
        else CHECK(p.kind == StartFill::Black && p.pixel == kBlack);
      }
  CHECK(startPlan(false, true, 0xff203050, false).pixel == 0xff203050);   // the alpha byte is the driver's, whatever was given
  CHECK(startPlan(false, true, 0, true).kind == StartFill::Colour && startPlan(false, true, 0, true).pixel == kBlack);
  // XNU's choice of console (PE_create_console): graphics only with a display mode and neither -v nor vc-text
  CHECK(!textConsole(1, false, 0));
  CHECK(!textConsole(2, false, 0));
  CHECK(textConsole(1, true, 0) && textConsole(1, false, 1) && textConsole(0, false, 0) && textConsole(0, true, 7));

  // A verbose boot: the console ran a while (the screen full of text), then the driver started, then the log went on.
  for (uint32_t seed = 1; seed <= 3; seed++) {
    struct Case { bool given; uint32_t rgb; } cases[] = {{false, 0}, {true, 0x203050}, {true, 0}};
    for (const Case &k : cases) {
      ConsoleModel console(seed);
      std::mt19937 rng(seed * 7919);
      for (int i = 0; i < 40; i++) console.line(rng);
      CHECK(console.differences() == 0);   // before the driver, the console's picture is its cells
      const StartPlan plan = startPlan(false, k.given, k.rgb, textConsole(1, true, 0));
      console.start(plan);
      for (int i = 0; i < 3; i++) console.line(rng);
      const uint64_t early = console.differences();
      for (int i = 3; i < 400; i++) console.line(rng);   // twenty screens of log
      uint64_t fill = 0, black = 0, other = 0;
      for (uint32_t p : console.panel) {
        fill += k.given && p == plan.pixel;
        black += p == ConsoleModel::kBack;
        other += p != ConsoleModel::kBack && p != ConsoleModel::kFore && !(k.given && p == plan.pixel);
      }
      const uint64_t total = console.panel.size();
      CHECK(other == 0);
      if (!k.given) {
        // the default under -v: the framebuffer kept, so the panel is exactly the console's text on black, all along
        CHECK(plan.kind == StartFill::Keep);
        CHECK(early == 0 && console.differences() == 0);
      } else if (k.rgb == 0x203050) {
        // the driver before 2026-11-12: the colour stays behind the text (most of the panel) and the strokes that went away left
        // black in it, the "bluish background with lines": reproduced
        CHECK(fill * 2 > total);
        CHECK(black > 0);
        CHECK(console.differences() > total / 2);
        if (seed == 1)
          std::printf("console model, fill #203050 under -v: %.1f%% of the panel keeps the fill, %.1f%% black after 20 screens of log\n",
                      100.0 * double(fill) / double(total), 100.0 * double(black) / double(total));
      } else {
        // a black fill under -v: no colour, but it erases the text on the screen, and the console redraws only what changes: strokes
        // are missing until their cells change (why the framebuffer is kept instead)
        CHECK(plan.pixel == kBlack);
        CHECK(early > 0);
        if (seed == 1)
          std::printf("console model, black fill under -v: %llu of %llu pixels not as the console means them 3 lines later, %llu after "
                      "20 screens\n", static_cast<unsigned long long>(early), static_cast<unsigned long long>(total),
                      static_cast<unsigned long long>(console.differences()));
      }
    }
  }
}

int main()
{
  testGeometry();
  testPixels();
  testOverlay();
  testCompose();
  testLayers();
  testJobs();
  testPanel();
  testRefresh();
  testSimulation();
  testStartFill();
  std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
  return failures ? 1 : 0;
}
