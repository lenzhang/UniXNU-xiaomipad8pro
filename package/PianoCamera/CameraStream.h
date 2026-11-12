// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera level 3 (piano-camera=3): RAW10 frames from one sensor through CSIPHY -> CSID-lite0 -> TFE-lite0's RDI0 write master
// into one physically contiguous buffer, a bounded number of them, one frame's statistics logged, everything down again in
// reverse. No ISP. Portable over level 2's ops class O (CameraPower.h), which adds for this level:
//   int mmBus(bool on);                          MM1 (the camera NoC) through the shared multimedia votes (PianoMmVotes.h)
//   bool dmaAlloc(uint64_t bytes, bool tables, Dma &d);  void dmaFree(Dma &d);   the frame buffer (and the window's table pages)
//   void cacheClean(const void *p, uint64_t n);  void cacheInvalidate(const void *p, uint64_t n);   to/from the point of coherency
//   uint64_t nowUs();
// Every store goes through put()/putQuiet(), which refuse an address outside level 3's allowlist (writable(): GCC's camera HF AXI
// branch; CAMCC PLL0 and the stream's clock sources and branches; the sensor's CSIPHY; CSID-lite0; TFE-lite0's bus words and write
// master 0; the SMMU words of the route's own group and bank and the global TLB invalidate); level 2's Session keeps its own
// allowlist for the sensor's power and bus. Every wait is bounded; the stream has a hard deadline.
//
// The sequence (Linux's order: camss.c, camss-csiphy-3ph-1-0.c, camss-csid-980.c, camss-vfe-gen4.c, the sensor drivers; facts in
// CameraHw.h, the PHY's lane table and the sensors' tables data in data/camera):
//   300 the sensor powered and its chip ID read (level 2's steps 201-230, with TITAN_TOP and the camera AHB for either bus, CAMCC's
//       domains voted at 201; the rear's MCLK at 24 MHz); refused if TITAN_TOP was already on (someone else's CAMSS)
//   301 MM1 on (the shared votes)                           302 GCC's camera HF AXI branch (halt check skipped, as Linux)
//   303 CAMCC PLL0: Linux's configuration when not running (a PLL0 found running must be at L 0x3e), RUN, LOCK_DET <= 1.5 ms,
//       its outputs, OUTCTRL                                 304 the clock sources (cphy_rx, the PHY's timer, ife_lite and its CSID
//       at 400 MHz, camnoc_rt_axi at 200 MHz; each switched with its root forced on) and the eleven branches
//   305 CSID-lite0's and TFE-lite0's HW_VERSION              306 the SMMU route of stream 0x1c00 (CameraSmmu.h: bypass as the boot
//       chain left it, else our window; refused otherwise)
//   307 the frame buffer: the mode's stride x height + a guard of at least 256 KiB (a canary), 2 MiB aligned; the window applied
//   308 CSID-lite0 reset (RST_DONE <= 500 ms)                309 write master 0: frame mode, the frame increment, the address
//   310 CSID-lite0: RX (4 lanes, the PHY, the lane map), RDI0 (RAW10, payload only, MIPI packing), the interrupt masks, the
//       register updates (AUP, RUP, twice), RDI0 start       311 the CSIPHY: reset, version, lanes, the 103-entry table with the
//       settle count, interrupts masked                       312 the sensor's start/init/mode tables, the rear's controls (test
//       pattern, orientation, exposure, gain, frame length), stream on last
//   320 the stream: the four status words read and cleared just before stream-on (logged: what was set before any frame), then
//       BUF_DONE polled (first frame <= 1 s, each next <= 200 ms, all <= piano-camera-timeout-ms); a BUF_DONE less than 1 ms after
//       stream-on or after the previous frame is counted as stale, not as a frame; the write master re-armed with the same buffer
//       after each frame; per frame the times of SOF, EOF and BUF_DONE; the first CSI2 RX and RDI error words with their times; the
//       CSIPHY's status words at the first frame and at the end
//   330 stop: write master off, RDI0 stopped, the PHY's lanes off, the sensor's off table; late frames counted for 100 ms (three
//       frame times: the write master and the CSID are idle after it)
//   331 the statistics (min/max/mean and an 8-bin histogram per Bayer position, eight vertical bands, the written extent, the guard,
//       the first rows' bytes); everything level 3 found is logged here, before the power-down (c3rp/c3r, 2026-11-12: an abort in
//       the old power-down lost every line of the report, the chip ID's included)
//   340 down, nothing touched after its clock, power or vote is gone (c3rp/c3r): the SMMU route first, while MM1, GCC's HF AXI and
//       the camera NoC's clocks still run (its TLB maintenance reaches the camera's TBU, inferred); then the branches, the clock
//       sources (before PLL0), PLL0, HF AXI, MM1; level 2's power-down (the sensor's bus, TITAN_TOP, the MCLK, the rails, GCC,
//       CAMCC's domains last); the buffer freed only once the write master read back stopped, its clock off and the route released
#pragma once
#include <stdint.h>
#include <string.h>
#include "CameraPower.h"
#include "CameraSmmu.h"
#include "CameraTables.h"   // generated from data/camera (tooling/camera/sensor-tables.py header)

namespace pianocamera {

struct StreamOptions {
  unsigned frames = 2;            // BUF_DONEs to collect (piano-camera-frames, 1..8)
  bool pattern = true;            // the rear's colour bars (piano-camera-pattern; 0: the scene)
  unsigned timeoutMs = 3000;      // the stream's hard bound from stream-on (piano-camera-timeout-ms, 500..10000)
  unsigned firstFrameMs = 1000, frameMs = 200, pollUs = 100, lateMs = 100;
  unsigned staleUs = 1000;        // a BUF_DONE this soon after stream-on or the previous frame is stale, not a frame (30 fps: 33 ms)
  unsigned gain = 64;             // the rear's analogue gain code (32 = 1x; piano-camera-gain)
  int stopAt = 0;                 // piano-camera-stop: power down after this step (300..331)
};

enum : int {
  kStepL3Sensor = 300, kStepL3Bus = 301, kStepL3Axi = 302, kStepL3Pll = 303, kStepL3Clocks = 304, kStepL3Versions = 305,
  kStepL3Smmu = 306, kStepL3Memory = 307, kStepL3CsidReset = 308, kStepL3Wm = 309, kStepL3Csid = 310, kStepL3Phy = 311,
  kStepL3Sensor2 = 312, kStepL3Stream = 320, kStepL3Stop = 330, kStepL3Stats = 331, kStepL3Down = 340,
};

enum class StreamFailure : uint8_t {
  none, sensor, refused, allowlist, votes, pll, rcg, branch, smmu, memory, csidReset, table, noFrame, frameTimeout, stopped,
};
inline const char *streamFailureName(StreamFailure f)
{
  switch (f) {
  case StreamFailure::none: return "none";
  case StreamFailure::sensor: return "the sensor did not power up or answer (level 2)";
  case StreamFailure::refused: return "refused";
  case StreamFailure::allowlist: return "a store outside the allowlist";
  case StreamFailure::votes: return "MM1 was not voted";
  case StreamFailure::pll: return "CAMCC PLL0 did not lock (or runs at another rate)";
  case StreamFailure::rcg: return "a clock source did not update";
  case StreamFailure::branch: return "a clock branch did not run";
  case StreamFailure::smmu: return "the SMMU route could not be set up";
  case StreamFailure::memory: return "no frame buffer";
  case StreamFailure::csidReset: return "CSID-lite0 did not reset";
  case StreamFailure::table: return "a sensor table write failed";
  case StreamFailure::noFrame: return "no frame arrived";
  case StreamFailure::frameTimeout: return "the frames stopped (or the stream's deadline passed)";
  case StreamFailure::stopped: return "stopped (piano-camera-stop)";
  }
  return "?";
}

// The owner's buffer: contiguous, 2 MiB aligned, a cacheable kernel view; with `tables`, two more 4 KiB pages (contiguous) for the
// window's level-1 and level-2 tables.
struct Dma {
  uint64_t pa = 0, bytes = 0;
  uint8_t *cpu = nullptr;
  uint64_t tablePa = 0;
  uint64_t *tables = nullptr;
  void *cookie = nullptr;
};

struct ChannelStats {
  uint32_t min = 1023, max = 0;
  uint64_t sum = 0, count = 0;
  uint32_t hist[8] = {};
  uint32_t mean() const { return count ? uint32_t(sum / count) : 0; }
};
struct FrameStats {
  bool done = false;
  unsigned width = 0, height = 0;
  uint32_t bpl = 0, stride = 0;
  ChannelStats ch[4];           // Bayer positions (row 0 col 0), (0, 1), (1, 0), (1, 1)
  uint32_t band[8][4] = {};     // each position's mean in eight vertical bands
  uint64_t written = 0;         // one past the last byte that is not the fill (0x00), rounded up to 16 bytes
  bool guardIntact = false;
  uint32_t guardFirstBad = 0;   // offset into the guard of its first changed byte (when not intact)
  uint8_t head[32] = {};
  // The first rows' bytes (rows 0..3 and the last, at the stride found): each row's first 16 bytes and how many of its bytes per line
  // (bpl) are not the fill: a row the write master never reached reads 0 of bpl.
  unsigned rowIndex[5] = {};
  uint8_t rowHead[5][16] = {};
  uint32_t rowNonzero[5] = {};
};

struct StreamResult {
  bool attempted = false, streamed = false, ok = false, cleanOff = false, bufferFreed = false, wmStopped = false;
  Sensor which = Sensor::rear;
  int failedStep = 0;
  StreamFailure failure = StreamFailure::none;
  const char *why = nullptr;
  Route route = Route::refused;
  const char *routeWhy = nullptr;
  uint64_t busAddr = 0, pa = 0, bytes = 0;
  uint32_t frameBytes = 0, width = 0, height = 0;
  uint32_t csidVersion = 0, tfeVersion = 0, phyVersion = 0;
  uint8_t settle = 0;
  unsigned tableWrites = 0, controlWrites = 0, phyWritten = 0, phySkipped = 0;
  unsigned bufDone = 0, sof = 0, eof = 0, rupDone = 0, lateFrames = 0, polls = 0, staleBufDone = 0;
  uint32_t topOr = 0, bufOr = 0, rxOr = 0, rdiOr = 0;
  uint32_t preTop = 0, preBuf = 0, preRx = 0, preRdi = 0;   // the status words just before stream-on (then cleared)
  // Per frame (up to kFrameLog): the time from stream-on of its SOF, EOF and BUF_DONE (the poll that saw them; 0: not seen), the
  // RDI0 and CSI2 RX words seen since the previous frame.
  static constexpr unsigned kFrameLog = 8;
  uint64_t sofUs[kFrameLog] = {}, eofUs[kFrameLog] = {}, bufUs[kFrameLog] = {};
  uint32_t frameRdi[kFrameLog] = {}, frameRx[kFrameLog] = {};
  uint32_t rxFirst = 0, rdiErrFirst = 0;       // the first nonzero CSI2 RX word and the first RDI0 error bits
  uint64_t rxFirstUs = 0, rdiErrFirstUs = 0;
  bool phyFirstRead = false;
  uint32_t phyStatusFirst[11] = {};            // the CSIPHY's status words at the first frame (or the stream's end without one)
  uint32_t phyStatus[11] = {};
  uint64_t firstFrameUs = 0, lastIntervalUs = 0, streamUs = 0;
  unsigned writes = 0, quietWrites = 0, refusedStores = 0, offWarnings = 0;
  uint32_t lastValue = 0;
  iova::Fault fault;            // the window's bank before its release (FSR nonzero: the write master went outside the buffer)
  bool faultRead = false;
  Release release;              // what the window's release kept (CameraSmmu.h)
  FrameStats stats;
  SensorResult sensor;
};

struct StreamUndo {
  bool mm1 = false, hfAxi = false, pll = false, pllConfigured = false;
  bool rcg[kL3RcgCount] = {};
  uint32_t rcgCfg[kL3RcgCount] = {};
  bool branch[kL3BranchCount] = {};
  bool route = false, dma = false, wm = false, csid = false, phy = false, streaming = false;
};

// One frame's statistics: RAW10 MIPI-packed (4 pixels in 5 bytes: the 8 high bits of each, then a byte of their 2 low bits, pixel 0
// in bits 1:0); the line stride is the packed line rounded up to 16 bytes when the written extent says so (frame mode's padding is
// what the first dump settles), else the packed line.
inline void frameStats(const uint8_t *buf, uint64_t bytes, uint32_t frameBytes, unsigned width, unsigned height, FrameStats &s)
{
  s = FrameStats();
  s.width = width;
  s.height = height;
  s.bpl = raw10Bytes(width);
  const uint32_t padded = (s.bpl + 15u) & ~15u;
  uint64_t last = 0;
  for (uint64_t i = frameBytes; i-- > 0;)
    if (buf[i]) {
      last = i + 1;
      break;
    }
  s.written = (last + 15u) & ~uint64_t(15);   // in 16-byte units (a pixel group may end in a zero byte)
  s.stride = last > uint64_t(s.bpl) * height ? padded : s.bpl;
  s.guardIntact = true;
  for (uint64_t i = frameBytes; i < bytes; ++i)
    if (buf[i] != 0xa5) {
      s.guardIntact = false;
      s.guardFirstBad = uint32_t(i - frameBytes);
      break;
    }
  memcpy(s.head, buf, sizeof s.head);
  for (unsigned k = 0; k < 5; ++k) {
    const unsigned y = k < 4 ? k : (height ? height - 1 : 0);
    s.rowIndex[k] = y;
    const uint64_t at = uint64_t(y) * s.stride;
    if (at + s.bpl > frameBytes) continue;
    memcpy(s.rowHead[k], buf + at, s.bpl < 16 ? s.bpl : 16);
    for (uint32_t i = 0; i < s.bpl; ++i) s.rowNonzero[k] += buf[at + i] != 0;
  }
  uint64_t bandSum[8][4] = {}, bandCount[8][4] = {};
  for (unsigned y = 0; y < height; ++y) {
    const uint8_t *line = buf + uint64_t(y) * s.stride;
    if (uint64_t(y) * s.stride + s.bpl > frameBytes) break;
    for (unsigned x = 0; x + 3 < width; x += 4) {
      const uint8_t *g = line + x / 4 * 5;
      for (unsigned k = 0; k < 4; ++k) {
        const uint32_t v = uint32_t(g[k]) << 2 | ((g[4] >> (2 * k)) & 3u);
        const unsigned px = x + k, pos = (y & 1u) * 2 + (px & 1u), band = px * 8 / width;
        ChannelStats &c = s.ch[pos];
        if (v < c.min) c.min = v;
        if (v > c.max) c.max = v;
        c.sum += v;
        ++c.count;
        ++c.hist[v >> 7];
        bandSum[band][pos] += v;
        ++bandCount[band][pos];
      }
    }
  }
  for (unsigned b = 0; b < 8; ++b)
    for (unsigned p = 0; p < 4; ++p) s.band[b][p] = bandCount[b][p] ? uint32_t(bandSum[b][p] / bandCount[b][p]) : 0;
  s.done = true;
}

// The sensor tables' sections (the generated header).
inline const CameraTableSection *tableSection(const char *sensor, const char *kind, const char *name)
{
  for (const CameraTableSection &t : kCameraTableSections)
    if (!strcmp(t.sensor, sensor) && !strcmp(t.kind, kind) && (!name || !strcmp(t.name, name))) return &t;
  return nullptr;
}
inline const CameraTableMode *tableMode(const char *sensor, unsigned width, unsigned height)
{
  for (const CameraTableMode &m : kCameraTableModes)
    if (!strcmp(m.sensor, sensor) && m.width == width && m.height == height) return &m;
  return nullptr;
}

// Bytes as hex digits (no printf in the portable part: the kernel and the host tests format the rest).
inline const char *hexBytes(char *out, const uint8_t *p, unsigned n)
{
  static const char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < n; ++i) {
    out[2 * i] = digits[p[i] >> 4];
    out[2 * i + 1] = digits[p[i] & 15u];
  }
  out[2 * n] = 0;
  return out;
}

// Everything level 3 found, through ops.note: Stream::run logs it after the statistics and before its power-down, so that a fault in
// the power-down cannot lose it (c3rp/c3r, 2026-11-12, lost every line, the chip ID's too); the kernel adds the power-down's lines.
template <class O>
inline void reportStream(O &ops, const StreamResult &r, bool pattern, unsigned gain)
{
  const SensorFacts &f = kSensors[r.which == Sensor::rear ? 0 : 1];
  const uint32_t bpl = raw10Bytes(r.width), padded = (bpl + 15u) & ~15u;
  if (r.failedStep)
    ops.note("level 3 (before the power-down): %s %s %ux%u RAW10: FAILED at step %d: %s%s%s", f.name, f.part, r.width, r.height,
             -r.failedStep, streamFailureName(r.failure), r.why ? ": " : "", r.why ? r.why : "");
  else
    ops.note("level 3 (before the power-down): %s %s %ux%u RAW10: %u frame(s) collected", f.name, f.part, r.width, r.height, r.bufDone);
  ops.note("route: %s (%s); bus %#llx -> pa %#llx, %llu bytes (frame %u = %u x %u + a guard)", routeName(r.route), r.routeWhy ? r.routeWhy : "-",
           (unsigned long long)r.busAddr, (unsigned long long)r.pa, (unsigned long long)r.bytes, r.frameBytes, padded, r.height);
  ops.note("versions: csid-lite0 %08x tfe-lite0 %08x csiphy%u %08x; settle %u; lane table %u written, %u skipped; sensor %u table writes, "
           "%u controls (pattern %u, gain %u)",
           r.csidVersion, r.tfeVersion, f.csiphy, r.phyVersion, r.settle, r.phyWritten, r.phySkipped, r.tableWrites, r.controlWrites,
           pattern ? 1u : 0u, gain);
  if (!r.streamed) return;
  ops.note("stream: before stream-on: top %08x buf-done %08x csi2-rx %08x rdi0 %08x (cleared)", r.preTop, r.preBuf, r.preRx, r.preRdi);
  ops.note("stream: %u frame(s) (BUF_DONE) in %llu us: the first after %llu us, the last interval %llu us; SOF %u EOF %u RUP_DONE %u; "
           "stale BUF_DONE %u; late %u; %u polls",
           r.bufDone, (unsigned long long)r.streamUs, (unsigned long long)r.firstFrameUs, (unsigned long long)r.lastIntervalUs, r.sof,
           r.eof, r.rupDone, r.staleBufDone, r.lateFrames, r.polls);
  for (unsigned i = 0; i < r.bufDone && i < StreamResult::kFrameLog; ++i)
    ops.note("stream: frame %u: SOF +%llu us, EOF +%llu us, BUF_DONE +%llu us; rdi0 %08x csi2-rx %08x since the frame before", i + 1,
             (unsigned long long)r.sofUs[i], (unsigned long long)r.eofUs[i], (unsigned long long)r.bufUs[i], r.frameRdi[i], r.frameRx[i]);
  ops.note("irq seen: top %08x buf-done %08x csi2-rx %08x rdi0 %08x (error bits %08x); first csi2-rx word %08x at +%llu us, first rdi0 "
           "error %08x at +%llu us",
           r.topOr, r.bufOr, r.rxOr, r.rdiOr, r.rdiOr & kRdiErrors, r.rxFirst, (unsigned long long)r.rxFirstUs, r.rdiErrFirst,
           (unsigned long long)r.rdiErrFirstUs);
  for (unsigned k = 0; k < 2; ++k) {
    const uint32_t *w = k ? r.phyStatus : r.phyStatusFirst;
    if (!k && !r.phyFirstRead) continue;
    ops.note("csiphy%u status 0-10 %s: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", f.csiphy,
             k ? "at the end" : "at the first frame", w[0] & 0xff, w[1] & 0xff, w[2] & 0xff, w[3] & 0xff,
             w[4] & 0xff, w[5] & 0xff, w[6] & 0xff, w[7] & 0xff, w[8] & 0xff, w[9] & 0xff, w[10] & 0xff);
  }
  if (r.faultRead)
    ops.note("smmu: the window's bank FSR %08x FAR %08x%08x FSYNR0 %08x GFSR %08x (%s)", r.fault.fsr, r.fault.farHi, r.fault.farLo,
             r.fault.fsynr0, r.fault.gfsr, (r.fault.fsr & iova::kFsrFaults) ? "a fault: the write master left the buffer" : "no fault");
  const FrameStats &s = r.stats;
  if (!s.done) return;
  ops.note("frame: written %llu bytes (%u unpadded, %u padded lines), stride %u; guard %s", (unsigned long long)s.written, bpl * r.height,
           padded * r.height, s.stride, s.guardIntact ? "intact" : "DAMAGED");
  if (!s.guardIntact) ops.note("frame: the guard's first changed byte at +%u: the write master wrote past the frame", s.guardFirstBad);
  static const char *const pos[4] = {"r0c0", "r0c1", "r1c0", "r1c1"};
  for (unsigned p = 0; p < 4; ++p)
    ops.note("frame: %s min %u mean %u max %u; histogram (128 a bin) %u %u %u %u %u %u %u %u", pos[p], s.ch[p].min, s.ch[p].mean(), s.ch[p].max,
             s.ch[p].hist[0], s.ch[p].hist[1], s.ch[p].hist[2], s.ch[p].hist[3], s.ch[p].hist[4], s.ch[p].hist[5], s.ch[p].hist[6],
             s.ch[p].hist[7]);
  // (each line under 240 characters: the console cuts a line at 255 with its prefix, and loses its newline: c3r's camcc lines)
  ops.note("frame: bands r0c0/r0c1/r1c0/r1c1: b0 %u/%u/%u/%u b1 %u/%u/%u/%u b2 %u/%u/%u/%u b3 %u/%u/%u/%u "
           "b4 %u/%u/%u/%u b5 %u/%u/%u/%u b6 %u/%u/%u/%u b7 %u/%u/%u/%u",
           s.band[0][0], s.band[0][1], s.band[0][2], s.band[0][3], s.band[1][0], s.band[1][1], s.band[1][2], s.band[1][3], s.band[2][0],
           s.band[2][1], s.band[2][2], s.band[2][3], s.band[3][0], s.band[3][1], s.band[3][2], s.band[3][3], s.band[4][0], s.band[4][1],
           s.band[4][2], s.band[4][3], s.band[5][0], s.band[5][1], s.band[5][2], s.band[5][3], s.band[6][0], s.band[6][1], s.band[6][2],
           s.band[6][3], s.band[7][0], s.band[7][1], s.band[7][2], s.band[7][3]);
  char hex[2 * 32 + 1];
  ops.note("frame: first %u bytes %s", unsigned(sizeof s.head), hexBytes(hex, s.head, sizeof s.head));
  for (unsigned k = 0; k < 5; ++k)
    ops.note("frame: row %u: %u of %u bytes not the fill; first 16 %s", s.rowIndex[k], s.rowNonzero[k], s.bpl, hexBytes(hex, s.rowHead[k], 16));
}

template <class O>
class Stream {
 public:
  Stream(O &ops, Session<O> &session, const StreamOptions &opt) : ops(ops), s(session), opt(opt) {}
  O &ops;
  Session<O> &s;
  StreamOptions opt;

  StreamResult run(Sensor which, const Loader &loader, const Pmic &pmic, const SurveyOptions &so, Survey &survey)
  {
    r_ = StreamResult();
    u_ = StreamUndo();
    d_ = Dma();
    plan_ = RoutePlan();
    rup_ = 0;
    wmEver_ = false;
    r_.attempted = true;
    r_.which = which;
    rear_ = which == Sensor::rear;
    f_ = &kSensors[rear_ ? 0 : 1];
    r_.width = rear_ ? kRearL3Width : kFrontL3Width;
    r_.height = rear_ ? kRearL3Height : kFrontL3Height;
    if (up(which, loader, pmic, so, survey)) {
      stream();
      stop();
      statistics();
    } else if (u_.streaming || u_.wm) {
      stop();
    }
    if (u_.route) {   // the window's bank while it is live (FSR nonzero: the write master went outside the buffer)
      r_.fault = iova::fault(plan_.window, [this](uint32_t off) { return ops.rd(kSmmu + off); });
      r_.faultRead = true;
    }
    reportStream(ops, r_, opt.pattern, opt.gain);   // before the power-down: nothing found is lost if it goes wrong
    down();
    r_.sensor = s.result();
    r_.ok = !r_.failedStep && r_.bufDone >= opt.frames && r_.stats.done && r_.stats.guardIntact && !(r_.fault.fsr & iova::kFsrFaults) &&
            r_.cleanOff;
    return r_;
  }

  // ------------------------------------------------------------------------------------------------------ the allowlist
  bool writable(uint64_t pa) const
  {
    if (pa == kGcc + kGccCameraHfAxi) return true;
    if (pa >= kCamCc + kCcPll0 && pa <= kCamCc + kCcPll0 + 0x30 && !(pa & 3)) return true;
    for (unsigned i = 0; i < kL3RcgCount; ++i) {
      const uint64_t base = kCamCc + rcgOf(i);
      if (pa == base || pa == base + kRcgCfg) return true;
    }
    for (unsigned i = 0; i < kL3BranchCount; ++i)
      if (pa == kCamCc + branchOf(i)) return true;
    const uint64_t phy = csiphyBase(f_->csiphy);
    if (pa >= phy && pa < phy + kCsiphyBytes && !(pa & 3)) return true;
    if (pa >= kCsidLite0 && pa < kCsidLite0 + kCsidLiteBytes && !(pa & 3)) return true;
    if (pa == kTfeLite0 + kTfeCgcOverride || pa == kTfeLite0 + kTfeTestBus) return true;
    if (pa >= kTfeLite0 + kWm0 && pa < kTfeLite0 + kWm0 + 0x100 && !(pa & 3)) return true;
    if (pa >= kSmmu && pa < kSmmu + kSmmuBytes) return smmuWritable(plan_, uint32_t(pa - kSmmu));
    return false;
  }
  const RoutePlan &routePlan() const { return plan_; }

 private:
  // --------------------------------------------------------------------------------------------------------- stores
  bool put(uint64_t pa, uint32_t v, const char *what)
  {
    if (!writable(pa)) {
      ++r_.refusedStores;
      ops.note("REFUSED store %08x to %#llx (%s): not on level 3's allowlist", v, (unsigned long long)pa, what);
      return false;
    }
    ++r_.writes;
    ops.wr(pa, v, what);
    return true;
  }
  bool putQuiet(uint64_t pa, uint32_t v)
  {
    if (!writable(pa)) {
      ++r_.refusedStores;
      return false;
    }
    ++r_.quietWrites;
    ops.wrQuiet(pa, v);
    return true;
  }
  bool poll(uint64_t pa, uint32_t mask, uint32_t want, unsigned us)
  {
    for (unsigned waited = 0;; ++waited) {
      r_.lastValue = ops.rd(pa);
      if ((r_.lastValue & mask) == want) return true;
      if (waited >= us) return false;
      ops.udelay(1);
    }
  }
  bool fail(int step, StreamFailure f, const char *why = nullptr)
  {
    if (!r_.failedStep) {
      r_.failedStep = -step;
      r_.failure = f;
      r_.why = why;
    }
    return false;
  }
  bool stopHere(int step) { return opt.stopAt && step >= opt.stopAt ? !fail(step, StreamFailure::stopped) : false; }

  uint32_t rcgOf(unsigned i) const { return i == kL3RcgTimer ? csiTimerRcg(f_->csiphy) : kL3Rcgs[i].off; }
  uint32_t branchOf(unsigned i) const
  {
    return i == kL3BranchPhy ? csiphyBranch(f_->csiphy) : i == kL3BranchTimer ? csiTimerBranch(f_->csiphy) : kL3Branches[i].off;
  }
  uint64_t common() const { return csiphyBase(f_->csiphy) + kCsiphyTableCommon; }
  uint64_t ctrl(unsigned n) const { return common() + 4u * n; }
  uint64_t status(unsigned n) const { return common() + kCsiphyTableStatus + 4u * n; }

  // The shared-RCG switch (level 2's rcgSet): the root forced on, CFG, UPDATE, the root released.
  bool rcgSet(uint64_t base, uint32_t cfg, const char *what)
  {
    if (!put(base, ops.rd(base) | kRcgRootEn, "RCG CMD: ROOT_EN (forced on for the switch)")) return false;
    bool ok = poll(base, kRcgRootOff, 0, kRcgUpdatePolls);
    if (ok) {
      ok = put(base + kRcgCfg, (ops.rd(base + kRcgCfg) & ~kRcgCfgMask) | cfg, what);
      ok = ok && put(base, ops.rd(base) | kRcgUpdate, "RCG CMD: UPDATE");
      ok = ok && poll(base, kRcgUpdate, 0, kRcgUpdatePolls);
    }
    put(base, ops.rd(base) & ~kRcgRootEn, "RCG CMD: ROOT_EN released");
    return ok;
  }

  // --------------------------------------------------------------------------------------------------------------- up
  bool up(Sensor which, const Loader &loader, const Pmic &pmic, const SurveyOptions &so, Survey &survey)
  {
    ops.step(kStepL3Sensor, "the sensor: level 2's power-up and chip ID (TITAN_TOP, CAMCC's domains, the rear's MCLK at 24 MHz)");
    if (!s.begin(which, loader, pmic, so, survey)) return fail(kStepL3Sensor, StreamFailure::sensor, failureName(s.result().failure));
    {   // the chip ID now (level 2's line comes after the power-down: c3rp/c3r never printed it)
      const SensorResult &id = s.result();
      ops.note("%s %s: chip ID %#06x (expected %#06x): OK over %s after %u tr%s; MCLK %u Hz", f_->name, f_->part, id.id, f_->idValue, id.bus,
               id.tries, id.tries == 1 ? "y" : "ies", id.mclkHz);
    }
    if (!s.ownsTitan()) return fail(kStepL3Sensor, StreamFailure::refused, "TITAN_TOP was already on (someone else's CAMSS)");
    if (stopHere(kStepL3Sensor)) return false;

    ops.step(kStepL3Bus, "MM1 on (the camera NoC; the shared multimedia votes)");
    if (ops.mmBus(true)) return fail(kStepL3Bus, StreamFailure::votes);
    u_.mm1 = true;
    ops.step(kStepL3Axi, "GCC camera HF AXI on (where off; halt check skipped, as Linux)");
    const uint32_t axi = ops.rd(kGcc + kGccCameraHfAxi);
    if (!branchEnabled(axi)) {
      ops.gccLock(true);
      const bool ok = put(kGcc + kGccCameraHfAxi, axi | kCbcrEnable, "GCC camera HF AXI: CLK_ENABLE");
      ops.gccLock(false);
      if (!ok) return fail(kStepL3Axi, StreamFailure::allowlist);
      u_.hfAxi = true;
    }
    if (stopHere(kStepL3Axi)) return false;

    ops.step(kStepL3Pll, "CAMCC PLL0 1200 MHz (Linux's configuration when not running), RUN, LOCK_DET, outputs, OUTCTRL");
    if (!pll0On()) return false;
    if (stopHere(kStepL3Pll)) return false;

    ops.step(kStepL3Clocks, "the clock sources (cphy_rx, the PHY timer, ife_lite, ife_lite_csid 400 MHz; camnoc_rt_axi 200 MHz), the branches");
    for (unsigned i = 0; i < kL3RcgCount; ++i) {
      const uint64_t base = kCamCc + rcgOf(i);
      const uint32_t cfg = ops.rd(base + kRcgCfg);
      if ((cfg & kRcgCfgMask) == kL3Rcgs[i].cfg) continue;
      u_.rcg[i] = true;
      u_.rcgCfg[i] = cfg;
      if (!rcgSet(base, kL3Rcgs[i].cfg, kL3Rcgs[i].name)) return fail(kStepL3Clocks, StreamFailure::rcg, kL3Rcgs[i].name);
    }
    for (unsigned i = 0; i < kL3BranchCount; ++i) {
      const uint64_t cbcr = kCamCc + branchOf(i);
      const uint32_t v = ops.rd(cbcr);
      if (branchEnabled(v) && (branchRunning(v) || !kL3Branches[i].halt)) continue;
      u_.branch[i] = true;
      if (!put(cbcr, v | kCbcrEnable, kL3Branches[i].name)) return fail(kStepL3Clocks, StreamFailure::allowlist);
      if (kL3Branches[i].halt && !poll(cbcr, kCbcrClkOff, 0, kBranchPollUs))
        return fail(kStepL3Clocks, StreamFailure::branch, kL3Branches[i].name);
    }
    if (stopHere(kStepL3Clocks)) return false;

    ops.step(kStepL3Versions, "CSID-lite0 and TFE-lite0 HW_VERSION");
    r_.csidVersion = ops.rd(kCsidLite0);
    r_.tfeVersion = ops.rd(kTfeLite0);
    ops.note("csid-lite0 HW_VERSION %08x, tfe-lite0 HW_VERSION %08x", r_.csidVersion, r_.tfeVersion);
    if (stopHere(kStepL3Versions)) return false;

    ops.step(kStepL3Smmu, "the SMMU route of stream 0x1c00 on apps_smmu 0x15000000 (as the boot chain left it, else our window)");
    plan_ = planRoute(kSmmuBytes, [this](uint32_t off) { return ops.rd(kSmmu + off); });
    r_.route = plan_.route;
    r_.routeWhy = plan_.why;
    if (plan_.route == Route::window) {   // what the release puts back (CameraSmmu.h: only what this SMMU accepts)
      ops.note("smmu: window: group %u (found smr %08x s2cr %08x%s), bank %u of %u (found sctlr %08x cbar %08x%s cba2r %08x; tcr %08x "
               "ttbr0 %08x%08x mair0 %08x)",
               plan_.slot, plan_.foundSmr, plan_.foundS2cr, s2crRestorable(plan_.foundS2cr, plan_.window.banks) ? "" : ": not restorable",
               plan_.bank, plan_.window.banks, plan_.foundSctlr, plan_.foundCbar, cbarRestorable(plan_.foundCbar, plan_.window.idr0) ? "" : ": not restorable",
               plan_.foundCba2r, plan_.foundBank[1], plan_.foundBank[3], plan_.foundBank[2], plan_.foundBank[7]);
    } else {
      ops.note("smmu: route %s (%s): group %u smr %08x s2cr %08x", routeName(plan_.route), plan_.why ? plan_.why : "-", plan_.slot,
               plan_.smr, plan_.s2cr);
    }
    if (plan_.route == Route::refused) return fail(kStepL3Smmu, StreamFailure::refused, plan_.why);
    if (stopHere(kStepL3Smmu)) return false;

    ops.step(kStepL3Memory, "the frame buffer (stride x height + a guard, 2 MiB aligned), filled, cleaned to the PoC; the window applied");
    const uint32_t stride = (raw10Bytes(r_.width) + 15u) & ~15u;
    r_.frameBytes = stride * r_.height;
    const uint64_t guard = 256u << 10, block = kIovaBlockBytes;
    r_.bytes = (uint64_t(r_.frameBytes) + guard + block - 1) & ~(block - 1);
    if (!ops.dmaAlloc(r_.bytes, plan_.route == Route::window, d_) || !d_.cpu || (d_.pa & (block - 1)) || d_.bytes < r_.bytes ||
        d_.pa + d_.bytes > kWmAddressLimit || (plan_.route == Route::window && (!d_.tables || !d_.tablePa || (d_.tablePa & 0xfff))))
      return fail(kStepL3Memory, StreamFailure::memory);
    u_.dma = true;
    r_.pa = d_.pa;
    memset(d_.cpu, 0, r_.frameBytes);
    memset(d_.cpu + r_.frameBytes, 0xa5, size_t(r_.bytes - r_.frameBytes));
    ops.cacheClean(d_.cpu, r_.bytes);
    if (plan_.route == Route::window) {
      iova::Window w;
      if (!iova::buildWindow(w, d_.tables, d_.tables + 512, d_.tablePa, d_.pa, uint32_t(r_.bytes), kIovaWindowBase) ||
          iova::translate(w, kIovaWindowBase) != d_.pa || iova::translate(w, kIovaWindowBase + r_.bytes - 1) != d_.pa + r_.bytes - 1 ||
          iova::translate(w, kIovaWindowBase + r_.bytes) != 0)
        return fail(kStepL3Memory, StreamFailure::smmu, "the window's table does not translate the buffer");
      ops.cacheClean(d_.tables, iova::kTableBytes);
      u_.route = true;
      unsigned spins = 0;
      const char *e = iova::apply(plan_.window, kCamStream, d_.tablePa, [this](uint32_t off) { return ops.rd(kSmmu + off); },
                                  [this](uint32_t off, uint32_t v) { put(kSmmu + off, v, "SMMU: the window"); },
                                  [this, &spins]() {
                                    ops.udelay(1);
                                    return ++spins > 10000;
                                  });
      if (e) return fail(kStepL3Memory, StreamFailure::smmu, e);
      r_.busAddr = kIovaWindowBase;
    } else {
      r_.busAddr = d_.pa;
    }
    if (stopHere(kStepL3Memory)) return false;

    ops.step(kStepL3CsidReset, "CSID-lite0 reset (RST_DONE within 500 ms)");
    const uint64_t c = kCsidLite0;
    u_.csid = true;
    put(c + kCsidTopClear, kCsidRstDone | kCsidTopBufDone, "CSID TOP_IRQ_CLEAR: RST_DONE, BUF_DONE");
    put(c + kCsidTopMask, kCsidRstDone | kCsidTopBufDone, "CSID TOP_IRQ_MASK: RST_DONE, BUF_DONE");
    put(c + kCsidBufClear, kCsidBufRdi0, "CSID BUF_DONE_IRQ_CLEAR: RDI0");
    put(c + kCsidBufMask, kCsidBufRdi0, "CSID BUF_DONE_IRQ_MASK: RDI0");
    put(c + kCsidIrqCmd, 1, "CSID IRQ_CMD: clear");
    put(c + kCsidRstCfg, kCsidRstValue, "CSID RST_CFG: mode, location");
    put(c + kCsidRstCmd, kCsidRstCmdValue, "CSID RST_CMD: hardware and IRQ reset");
    bool reset = false;
    for (unsigned ms = 0; ms <= kCsidResetMs && !reset; ++ms) {
      reset = (ops.rd(c + kCsidTopStatus) & kCsidRstDone) != 0;
      if (!reset) ops.msleep(1);
    }
    if (!reset) return fail(kStepL3CsidReset, StreamFailure::csidReset);
    put(c + kCsidTopClear, kCsidRstDone, "CSID TOP_IRQ_CLEAR: RST_DONE");
    put(c + kCsidIrqCmd, 1, "CSID IRQ_CMD: clear");
    if (stopHere(kStepL3CsidReset)) return false;

    ops.step(kStepL3Wm, "TFE-lite0 write master 0: frame mode, the frame increment, the buffer's address");
    const uint64_t t = kTfeLite0, wm = kTfeLite0 + kWm0;
    u_.wm = true;
    wmEver_ = true;
    put(t + kTfeCgcOverride, kWmCgcAll, "TFE bus CGC override: all");
    put(t + kTfeTestBus, 0, "TFE bus test bus: off");
    put(wm + kWmFrameIncr, r_.frameBytes >> 8, "WM0 FRAME_INCR: stride x height >> 8");
    put(wm + kWmImageCfg0, 0xffff, "WM0 IMAGE_CFG_0: the default width");
    put(wm + kWmImageCfg2, 0xffff, "WM0 IMAGE_CFG_2: the default stride");
    put(wm + kWmPacker, 0, "WM0 PACKER_CFG: 0");
    put(wm + kWmSubPeriod, 0, "WM0 IRQ subsample period 0");
    put(wm + kWmSubPattern, 1, "WM0 IRQ subsample pattern 1");
    put(wm + kWmPrefetch, 1, "WM0 MMU prefetch on");
    put(wm + kWmPrefetchMax, 0xffffffffu, "WM0 MMU prefetch max offset");
    put(wm + kWmCfg, kWmEnable | kWmFrameMode, "WM0 CFG: enable, frame mode");
    put(wm + kWmImageAddr, uint32_t(r_.busAddr >> 8), "WM0 IMAGE_ADDR: the buffer >> 8");
    if (stopHere(kStepL3Wm)) return false;

    ops.step(kStepL3Csid, "CSID-lite0: RX (4 lanes, the PHY, lane map 3210), RDI0 RAW10 payload-only MIPI-packed, masks, AUP/RUP x2, start");
    put(c + kCsidRxCfg0, csidRxCfg0(f_->lanes, f_->csiphy), "CSID RX_CFG0: lanes - 1, lane map, PHY + 1");
    put(c + kCsidRxCfg1, 1, "CSID RX_CFG1: ECC correction");
    put(c + kCsidRdiCfg0, kRdiCfg0Base, "CSID RDI0 CFG0: RAW10 (0x2b), payload only, VC 0, timestamps");
    put(c + kCsidRdiCfg1, kRdiCfg1Mipi, "CSID RDI0 CFG1: MIPI packing");
    put(c + kCsidRdiCtrl, 0, "CSID RDI0 CTRL: 0");
    put(c + kCsidRdiCfg0, ops.rd(c + kCsidRdiCfg0) | kRdiCfg0Enable, "CSID RDI0 CFG0: enable");
    put(c + kCsidRxMask, 0xffffffffu, "CSID CSI2 RX IRQ mask: all (polled; no interrupt is attached)");
    put(c + kCsidRdiMask, kRdiMaskL3, "CSID RDI0 IRQ mask: SOF/EOF/RUP_DONE/errors (polled)");
    for (unsigned k = 0; k < 2; ++k) {
      rup_ |= kCsidAupRdi0;
      put(c + kCsidRupAup, rup_, "CSID RUP_AUP: AUP RDI0");
      rup_ |= kCsidRupRdi0;
      put(c + kCsidRupAup, rup_, "CSID RUP_AUP: RUP RDI0");
    }
    put(c + kCsidRdiCtrl, 1, "CSID RDI0 CTRL: start");
    if (stopHere(kStepL3Csid)) return false;

    ops.step(kStepL3Phy, "the CSIPHY: reset, version, lanes, the lane table with the settle count, interrupts masked");
    u_.phy = true;
    put(ctrl(0), 1, "CSIPHY CTRL0: reset");
    ops.udelay(kCsiphyResetUs);
    put(ctrl(0), 0, "CSIPHY CTRL0: out of reset");
    put(ctrl(6), kCsiphyShowRevId, "CSIPHY CTRL6: SHOW_REV_ID");
    r_.phyVersion = (ops.rd(status(12)) & 0xffu) | (ops.rd(status(13)) & 0xffu) << 8 | (ops.rd(status(14)) & 0xffu) << 16 |
                    (ops.rd(status(15)) & 0xffu) << 24;
    r_.settle = csiphySettle(f_->linkHz, kL3Rcgs[kL3RcgTimer].hz);
    put(ctrl(5), csiphyLaneMask(f_->lanes), "CSIPHY CTRL5: clock enable, data lanes 0-3");
    put(ctrl(6), kCsiphyPwrdnB, "CSIPHY CTRL6: COMMON_PWRDN_B");
    put(ctrl(7), kCsiphyCtrl7, "CSIPHY CTRL7: 2");
    put(ctrl(0), 0, "CSIPHY CTRL0: 0");
    const uint64_t phy = csiphyBase(f_->csiphy);
    for (const CsiphyLaneReg &e : kCsiphyLaneRegs) {
      if (e.kind == 4 || e.kind == 7) {   // dnp, skew calibration: not written (as the driver)
        ++r_.phySkipped;
        continue;
      }
      if (!putQuiet(phy + e.offset, e.kind == 2 ? r_.settle : e.value)) return fail(kStepL3Phy, StreamFailure::allowlist);
      ++r_.phyWritten;
      if (e.delayUs) ops.udelay(e.delayUs);
    }
    for (unsigned n = 11; n <= 21; ++n) putQuiet(ctrl(n), 0);
    ops.note("csiphy%u: version %08x; the lane table: %u entries written (settle %u for %u Hz), %u skipped (skew calibration); "
             "interrupts masked",
             f_->csiphy, r_.phyVersion, r_.phyWritten, r_.settle, f_->linkHz, r_.phySkipped);
    if (stopHere(kStepL3Phy)) return false;

    ops.step(kStepL3Sensor2, rear_ ? "the S5KJN1's tables (start, init, mode 4080x3072), its controls, stream on"
                                   : "the OV32D40's tables (start, init, mode 3264x1836), stream on");
    if (!sensorTables()) return false;
    return true;
  }

  bool pll0On()
  {
    const uint64_t p = kCamCc + kCcPll0;
    const uint32_t mode = ops.rd(p), opmode = ops.rd(p + kPllOpmode);
    if (pllRunning(mode, opmode)) {
      if ((ops.rd(p + kTaycanL) & 0xffffu) != (kPll0LVal & 0xffffu))
        return fail(kStepL3Pll, StreamFailure::pll, "PLL0 runs at another rate (someone else's)");
      ops.note("pll0: found running at Linux's rate: used as it is");
      return true;
    }
    if (ops.rd(p + kTaycanUserCtl) & (1u << 25)) return fail(kStepL3Pll, StreamFailure::pll, "PLL0 in FSM vote mode");
    put(p + kTaycanL, kPll0LVal, "PLL0 L_VAL: 0x3e, CAL_L 0x44");
    put(p + kTaycanAlpha, kPll0Alpha, "PLL0 ALPHA: 0x8000 (1200 MHz)");
    put(p + kTaycanConfigCtl, kPll0ConfigCtl, "PLL0 CONFIG_CTL");
    put(p + kTaycanConfigCtlU, kPll0ConfigCtlU, "PLL0 CONFIG_CTL_U");
    put(p + kTaycanConfigCtlU1, kPll0ConfigCtlU1, "PLL0 CONFIG_CTL_U1");
    put(p + kTaycanUserCtl, kPll0UserCtl, "PLL0 USER_CTL: even /2, odd /3");
    put(p + kTaycanUserCtlU, kPll0UserCtlU, "PLL0 USER_CTL_U");
    put(p, ops.rd(p) & ~kPllOutctrl, "PLL0 MODE: OUTCTRL off");
    put(p + kPllOpmode, 0, "PLL0 OPMODE: standby");
    put(p, ops.rd(p) | kPllResetN, "PLL0 MODE: RESET_N");
    u_.pllConfigured = true;
    u_.pll = true;
    put(p, ops.rd(p) | kPllResetN, "PLL0 MODE: RESET_N");
    put(p + kPllOpmode, 1, "PLL0 OPMODE: run");
    if (!poll(p, kPllLockDet, kPllLockDet, kPllLockUs)) return fail(kStepL3Pll, StreamFailure::pll, "no LOCK_DET within 1.5 ms");
    put(p + kTaycanUserCtl, ops.rd(p + kTaycanUserCtl) | kPllOutputs, "PLL0 USER_CTL: outputs on");
    put(p, ops.rd(p) | kPllOutctrl, "PLL0 MODE: OUTCTRL");
    return true;
  }
  void pll0Off()
  {
    if (!u_.pll) return;
    const uint64_t p = kCamCc + kCcPll0;
    put(p, ops.rd(p) & ~kPllOutctrl, "PLL0 MODE: OUTCTRL off");
    put(p + kTaycanUserCtl, ops.rd(p + kTaycanUserCtl) & ~kPllOutputs, "PLL0 USER_CTL: outputs off");
    put(p + kPllOpmode, 0, "PLL0 OPMODE: standby");
    u_.pll = false;
  }

  // One table section over the sensor's bus: writes (1..3 bytes) and delays. `soft`: a failure is only counted (the off table).
  bool section(const char *kind, const char *name, bool soft = false)
  {
    const CameraTableSection *t = tableSection(rear_ ? "s5kjn1" : "ov32d40", kind, name);
    if (!t) return soft ? false : fail(kStepL3Sensor2, StreamFailure::table, "a table section is missing");
    for (uint32_t k = t->first; k < t->first + t->words; k += 2) {
      const uint32_t a = kCameraTableWords[k], v = kCameraTableWords[k + 1];
      if ((a >> 16) == 0xffffu) {
        ops.udelay(v);
        continue;
      }
      if (!s.sensorWrite(uint16_t(a & 0xffffu), a >> 30, v)) return soft ? false : fail(kStepL3Sensor2, StreamFailure::table, kind);
      ++r_.tableWrites;
    }
    return true;
  }
  bool control(uint16_t reg, unsigned bytes, uint32_t value)
  {
    if (!s.sensorWrite(reg, bytes, value)) return fail(kStepL3Sensor2, StreamFailure::table, "a control");
    ++r_.controlWrites;
    return true;
  }
  static unsigned decimal(char *out, unsigned v)
  {
    char d[10];
    unsigned k = 0, n = 0;
    do d[k++] = char('0' + v % 10);
    while ((v /= 10) && k < sizeof d);
    while (k) out[n++] = d[--k];
    return n;
  }
  bool sensorTables()
  {
    char mode[24];
    unsigned n = decimal(mode, r_.width);
    mode[n++] = 'x';
    n += decimal(mode + n, r_.height);
    mode[n] = 0;
    const CameraTableMode *m = tableMode(rear_ ? "s5kjn1" : "ov32d40", r_.width, r_.height);
    if (!m) return fail(kStepL3Sensor2, StreamFailure::table, "the mode's parameters are missing");
    if (!section("start", nullptr) || !section("init", nullptr) || !section("mode", mode)) return false;
    if (rear_) {   // exposure (the mode's default lines), analogue gain, frame length, native orientation, the test pattern
      if (!control(kS5kOrientation, 1, 0) || !control(0x0202, 2, m->c) || !control(0x0204, 2, opt.gain) || !control(0x0340, 2, m->b) ||
          !control(kS5kTestPattern, 2, opt.pattern ? 2u : 0u))
        return false;
    }
    if (stopHere(kStepL3Sensor2)) {
      ops.note("stopped before stream-on (piano-camera-stop=%d): the tables and controls written, the stream-on table not", opt.stopAt);
      return false;
    }
    // What CSID-lite0 holds before any frame (a BUF_DONE or error set now is not the sensor's), logged and cleared, so the stream
    // counts only what comes after stream-on.
    const uint64_t c = kCsidLite0;
    r_.preTop = ops.rd(c + kCsidTopStatus);
    r_.preBuf = ops.rd(c + kCsidBufStatus);
    r_.preRx = ops.rd(c + kCsidRxStatus);
    r_.preRdi = ops.rd(c + kCsidRdiStatus);
    if (r_.preTop | r_.preBuf | r_.preRx | r_.preRdi) {
      putQuiet(c + kCsidTopClear, r_.preTop);
      putQuiet(c + kCsidBufClear, r_.preBuf);
      putQuiet(c + kCsidRxClear, r_.preRx);
      putQuiet(c + kCsidRdiClear, r_.preRdi);
      putQuiet(c + kCsidIrqCmd, 1);
    }
    u_.streaming = true;
    if (!section("on", nullptr)) return false;
    r_.streamed = true;
    return true;
  }

  // ------------------------------------------------------------------------------------------------------------- stream
  // Every poll reads the four status words (each cleared once seen), so nothing set between polls is lost; a frame is a BUF_DONE of
  // RDI0 at least opt.staleUs after stream-on and after the previous frame (a stale one is counted apart and ends nothing). Bounded:
  // the first frame within firstFrameMs, each next within frameMs, all within timeoutMs.
  void stream()
  {
    ops.step(kStepL3Stream, "the stream: SOF/EOF/BUF_DONE polled, the write master re-armed after each frame");
    const uint64_t c = kCsidLite0, wm = kTfeLite0 + kWm0;
    const uint64_t t0 = ops.nowUs(), deadline = t0 + uint64_t(opt.timeoutMs) * 1000;
    uint64_t frameDeadline = t0 + uint64_t(opt.firstFrameMs) * 1000, last = t0;
    uint32_t rdiSince = 0, rxSince = 0;
    unsigned sofLogged = 0, eofLogged = 0;
    while (r_.bufDone < opt.frames) {
      ++r_.polls;
      const uint32_t top = ops.rd(c + kCsidTopStatus), buf = ops.rd(c + kCsidBufStatus), rx = ops.rd(c + kCsidRxStatus),
                     rdi = ops.rd(c + kCsidRdiStatus);
      const uint64_t now = ops.nowUs(), at = now - t0;
      r_.topOr |= top;
      r_.bufOr |= buf;
      r_.rxOr |= rx;
      r_.rdiOr |= rdi;
      rdiSince |= rdi;
      rxSince |= rx;
      if (rx && !r_.rxFirst) {
        r_.rxFirst = rx;
        r_.rxFirstUs = at;
      }
      if ((rdi & kRdiErrors) && !r_.rdiErrFirst) {
        r_.rdiErrFirst = rdi & kRdiErrors;
        r_.rdiErrFirstUs = at;
      }
      if (rdi & kRdiInputSof) {
        ++r_.sof;
        if (sofLogged < StreamResult::kFrameLog && sofLogged <= r_.bufDone) r_.sofUs[sofLogged++] = at;
      }
      if (rdi & kRdiInputEof) {
        ++r_.eof;
        if (eofLogged < StreamResult::kFrameLog && eofLogged <= r_.bufDone) r_.eofUs[eofLogged++] = at;
      }
      if (rdi & kRdiRupDone) {
        ++r_.rupDone;
        rup_ &= ~(kCsidRupRdi0 | kCsidAupRdi0);   // the hardware consumed them (the driver clears its copy here)
      }
      if (top | buf | rx | rdi) {
        putQuiet(c + kCsidTopClear, top);
        putQuiet(c + kCsidBufClear, buf);
        putQuiet(c + kCsidRxClear, rx);
        putQuiet(c + kCsidRdiClear, rdi);
        putQuiet(c + kCsidIrqCmd, 1);
      }
      if (buf & kCsidBufRdi0) {
        if (now - last < opt.staleUs) {   // too soon after stream-on or the last frame: not a frame of the sensor's
          ++r_.staleBufDone;
        } else {
          const unsigned i = r_.bufDone++;
          if (i < StreamResult::kFrameLog) {
            r_.bufUs[i] = at;
            r_.frameRdi[i] = rdiSince;
            r_.frameRx[i] = rxSince;
          }
          rdiSince = rxSince = 0;
          if (sofLogged < r_.bufDone) sofLogged = r_.bufDone;   // keep the per-frame columns aligned when an SOF was not seen
          if (eofLogged < r_.bufDone) eofLogged = r_.bufDone;
          if (r_.bufDone == 1) {
            r_.firstFrameUs = at;
            readPhyStatus(r_.phyStatusFirst);
            r_.phyFirstRead = true;
          } else {
            r_.lastIntervalUs = now - last;
          }
          last = now;
          frameDeadline = now + uint64_t(opt.frameMs) * 1000;
          if (r_.bufDone < opt.frames) {   // the same buffer again (only the last frame is analysed)
            putQuiet(wm + kWmImageAddr, uint32_t(r_.busAddr >> 8));
            rup_ |= kCsidAupRdi0;
            putQuiet(c + kCsidRupAup, rup_);
          }
          continue;
        }
      }
      if (now >= frameDeadline || now >= deadline) {
        fail(kStepL3Stream, r_.bufDone ? StreamFailure::frameTimeout : StreamFailure::noFrame);
        break;
      }
      ops.udelay(opt.pollUs);
    }
    r_.streamUs = ops.nowUs() - t0;
    readPhyStatus(r_.phyStatus);
  }
  void readPhyStatus(uint32_t *w)
  {
    for (unsigned n = 0; n < 11; ++n) w[n] = ops.rd(status(n));
  }

  // The write master first (nothing more reaches memory), then RDI0, the PHY's lanes, the sensor; late frames counted.
  void stop()
  {
    ops.step(kStepL3Stop, "stop: write master off, RDI0 stopped, the PHY's lanes off, the sensor's off table");
    const uint64_t c = kCsidLite0, wm = kTfeLite0 + kWm0;
    if (u_.wm) {
      put(wm + kWmCfg, 0, "WM0 CFG: off");
      r_.wmStopped = (ops.rd(wm + kWmCfg) & kWmEnable) == 0;
      u_.wm = false;
    } else {
      r_.wmStopped = true;
    }
    if (u_.csid) {
      put(c + kCsidRdiCtrl, 0, "CSID RDI0 CTRL: stop");
      put(c + kCsidRdiCfg0, ops.rd(c + kCsidRdiCfg0) & ~kRdiCfg0Enable, "CSID RDI0 CFG0: disable");
    }
    if (u_.phy) {
      put(ctrl(5), 0, "CSIPHY CTRL5: 0 (lanes off)");
      put(ctrl(6), 0, "CSIPHY CTRL6: 0 (powered down)");
      u_.phy = false;
    }
    if (u_.streaming) {
      if (!section("off", nullptr, true)) ++r_.offWarnings;
      u_.streaming = false;
    }
    if (u_.csid) {
      for (unsigned ms = 0; ms < opt.lateMs; ++ms) {
        const uint32_t buf = ops.rd(c + kCsidBufStatus);
        if (buf & kCsidBufRdi0) {
          ++r_.lateFrames;
          putQuiet(c + kCsidBufClear, buf);
          putQuiet(c + kCsidIrqCmd, 1);
        }
        ops.msleep(1);
      }
      u_.csid = false;
    }
  }

  void statistics()
  {
    if (!r_.bufDone || !u_.dma || !r_.wmStopped) return;
    ops.step(kStepL3Stats, "the frame's statistics (the buffer invalidated from the caches first)");
    ops.cacheInvalidate(d_.cpu, r_.bytes);
    frameStats(d_.cpu, r_.bytes, r_.frameBytes, r_.width, r_.height, r_.stats);
  }

  // ---------------------------------------------------------------------------------------------------------------- down
  // Nothing is touched after its clock, power or vote is gone (c3rp/c3r): the SMMU window first, while the camera NoC is still voted
  // (MM1) and clocked (GCC's HF AXI, the CAMNOC branches): its release ends with the TLB invalidate and sync, which reach the camera's
  // TBU (inferred); the write master and CSID-lite0 were stopped and left idle for opt.lateMs by stop(). Then the clocks in reverse,
  // PLL0, HF AXI, MM1; then level 2's power-down (the sensor's bus, TITAN_TOP collapsed, the MCLK, the rails, GCC, CAMCC's domains).
  void down()
  {
    ops.step(kStepL3Down, "power down: the SMMU window first (MM1 and the camera NoC's clocks on), the clocks, PLL0, HF AXI, MM1, then "
                          "level 2's");
    if (u_.route) {
      unsigned spins = 0;
      r_.release = releaseRoute(plan_, [this](uint32_t off) { return ops.rd(kSmmu + off); },
                                [this](uint32_t off, uint32_t v) { put(kSmmu + off, v, "SMMU: the window released"); },
                                [this, &spins]() {
                                  ops.udelay(1);
                                  return ++spins > 10000;
                                });
      if (r_.release.s2crKept)
        ops.note("smmu: group %u's S2CR left at fault (%08x): the found %08x is not a configuration of this SMMU (%u banks)", plan_.slot,
                 r_.release.s2cr, plan_.foundS2cr, plan_.window.banks);
      if (r_.release.cbarKept)
        ops.note("smmu: bank %u's CBAR/CBA2R left at %08x/%08x (stage 1, translation off): the found %08x/%08x is a context type "
                 "this SMMU lacks (IDR0 %08x)",
                 plan_.bank, r_.release.cbar, r_.release.cba2r, plan_.foundCbar, plan_.foundCba2r, plan_.window.idr0);
      if (r_.release.error) {
        ++r_.offWarnings;
        ops.note("smmu: release: %s", r_.release.error);
      } else {
        u_.route = false;
      }
    }
    bool ifeLiteOff = true;
    for (unsigned i = kL3BranchCount; i-- > 0;) {
      if (!u_.branch[i]) continue;
      const uint64_t cbcr = kCamCc + branchOf(i);
      put(cbcr, ops.rd(cbcr) & ~kCbcrEnable, kL3Branches[i].name);
      if (kL3Branches[i].halt && !poll(cbcr, kCbcrClkOff, kCbcrClkOff, kBranchPollUs)) {
        ++r_.offWarnings;
        if (branchOf(i) == kCcIfeLite) ifeLiteOff = false;
      }
      u_.branch[i] = false;
    }
    for (unsigned i = kL3RcgCount; i-- > 0;) {   // back to their sources as found, before PLL0 stops
      if (!u_.rcg[i]) continue;
      if (!rcgSet(kCamCc + rcgOf(i), u_.rcgCfg[i] & kRcgCfgMask, "RCG CFG: as found")) ++r_.offWarnings;
      u_.rcg[i] = false;
    }
    pll0Off();
    if (u_.hfAxi) {
      ops.gccLock(true);
      put(kGcc + kGccCameraHfAxi, ops.rd(kGcc + kGccCameraHfAxi) & ~kCbcrEnable, "GCC camera HF AXI: off (as found)");
      ops.gccLock(false);
      u_.hfAxi = false;
    }
    if (u_.mm1) {
      if (ops.mmBus(false)) ++r_.offWarnings;
      else u_.mm1 = false;
    }
    s.end();   // level 2's power-down: the sensor, its bus, TITAN_TOP collapsed, the MCLK, the rails, GCC, CAMCC's domains
    if (u_.dma) {
      if ((!wmEver_ || r_.wmStopped) && ifeLiteOff && !u_.route) {
        ops.dmaFree(d_);
        u_.dma = false;
        r_.bufferFreed = true;
      } else {
        ops.note("dma: the frame buffer %#llx (%llu bytes) is KEPT: the write master was not confirmed stopped or its route not released",
                 (unsigned long long)d_.pa, (unsigned long long)d_.bytes);
      }
    }
    r_.cleanOff = s.result().cleanOff && !u_.mm1 && !u_.hfAxi && !u_.pll && !u_.route && !u_.dma && !u_.streaming && !u_.phy && !u_.wm;
    for (bool b : u_.branch) r_.cleanOff = r_.cleanOff && !b;
    for (bool b : u_.rcg) r_.cleanOff = r_.cleanOff && !b;
  }

  StreamResult r_;
  StreamUndo u_;
  Dma d_;
  RoutePlan plan_;
  uint32_t rup_ = 0;
  bool wmEver_ = false;
  bool rear_ = true;
  const SensorFacts *f_ = &kSensors[0];
};

}  // namespace pianocamera
