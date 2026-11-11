// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the network function's portable parts: NTB16 encode (Ncm.h NtbWriter) and decode (parseNtb16) against this file's
// own reading of CDC NCM 1.0 section 3 (an independent encoder that puts the NDP first, a strict decoder), round trips, malformed
// blocks; the NTB parameters; the frame queue; the TX FIFO plan with piano's measured core (T1 probe) and its failure cases.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. ncm_host.cpp -o /tmp/ncm_host && /tmp/ncm_host
#include "Ncm.h"
#include "TxFifoPlan.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pianousb;
using namespace pianousb::ncm;

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using Bytes = std::vector<uint8_t>;
using Frames = std::vector<Bytes>;

static Bytes frame(unsigned n, uint8_t seed)
{
  Bytes f(n);
  for (unsigned i = 0; i < n; ++i) f[i] = uint8_t(seed + i * 7);
  return f;
}
static uint16_t le16(const Bytes &b, size_t at) { return uint16_t(b.at(at) | b.at(at + 1) << 8); }
static uint32_t le32(const Bytes &b, size_t at) { return le16(b, at) | uint32_t(le16(b, at + 2)) << 16; }
static void put16(Bytes &b, size_t at, uint32_t v) { b.at(at) = uint8_t(v); b.at(at + 1) = uint8_t(v >> 8); }
static void put32(Bytes &b, size_t at, uint32_t v) { put16(b, at, v); put16(b, at + 2, v >> 16); }

// Strict decoder, written from the spec: one or more NDP16 ("NCM0"), datagrams inside wBlockLength, alignment as this side
// advertises for its IN direction (datagram offset % 4 == 0, NDP offset % 4 == 0). Returns false on any violation.
static bool specDecode(const Bytes &b, Frames &out, uint16_t *sequence = nullptr)
{
  if (b.size() < 12 || le32(b, 0) != 0x484d434e || le16(b, 4) != 12) return false;
  const uint32_t block = le16(b, 8);
  if (block != b.size()) return false;
  if (sequence) *sequence = le16(b, 6);
  uint32_t ndp = le16(b, 10);
  for (unsigned guard = 0; ndp; ++guard) {
    if (guard > 4 || ndp % 4 || ndp + 16 > block || le32(b, ndp) != 0x304d434e) return false;
    const uint32_t length = le16(b, ndp + 4);
    if (length < 16 || length % 4 || ndp + length > block) return false;
    bool terminated = false;
    for (uint32_t e = ndp + 8; e + 4 <= ndp + length; e += 4) {
      const uint32_t at = le16(b, e), n = le16(b, e + 2);
      if (!at && !n) { terminated = true; break; }
      if (!at || !n || at % 4 || at + n > block || at < 12) return false;
      out.emplace_back(b.begin() + at, b.begin() + at + n);
    }
    if (!terminated) return false;
    ndp = le16(b, ndp + 6);
  }
  return true;
}

// Independent encoder: NTH16, then the NDP16, then the datagrams (the layout most hosts send), optionally split over two NDPs.
static Bytes specEncode(const Frames &frames, uint16_t sequence, bool twoNdps = false, unsigned pad = 0)
{
  Bytes b(12, 0);
  const size_t n = frames.size(), first = twoNdps ? n / 2 : n;
  const size_t ndp1 = 12, ndp1Len = 8 + 4 * (first + 1), ndp2 = ndp1 + ndp1Len, ndp2Len = twoNdps ? 8 + 4 * (n - first + 1) : 0;
  b.resize(ndp2 + ndp2Len, 0);
  std::vector<std::pair<uint32_t, uint32_t>> at;
  for (const Bytes &f : frames) {
    while (b.size() % 4) b.push_back(0);
    for (unsigned i = 0; i < pad; ++i) b.push_back(0xee);
    at.emplace_back(uint32_t(b.size()), uint32_t(f.size()));
    b.insert(b.end(), f.begin(), f.end());
  }
  put32(b, ndp1, 0x304d434e);
  put16(b, ndp1 + 4, uint32_t(ndp1Len));
  put16(b, ndp1 + 6, twoNdps ? uint32_t(ndp2) : 0);
  for (size_t i = 0; i < first; ++i) { put16(b, ndp1 + 8 + 4 * i, at[i].first); put16(b, ndp1 + 10 + 4 * i, at[i].second); }
  if (twoNdps) {
    put32(b, ndp2, 0x304d434e);
    put16(b, ndp2 + 4, uint32_t(ndp2Len));
    put16(b, ndp2 + 6, 0);
    for (size_t i = first; i < n; ++i) { put16(b, ndp2 + 8 + 4 * (i - first), at[i].first); put16(b, ndp2 + 10 + 4 * (i - first), at[i].second); }
  }
  put32(b, 0, 0x484d434e);
  put16(b, 4, 12);
  put16(b, 6, sequence);
  put16(b, 8, uint32_t(b.size()));
  put16(b, 10, uint32_t(ndp1));
  return b;
}

static NtbResult decode(const Bytes &b, Frames &out, uint32_t got = 0)
{
  return parseNtb16(b.data(), got ? got : uint32_t(b.size()), [&](uint32_t at, uint32_t n) { out.emplace_back(b.begin() + at, b.begin() + at + n); });
}

// The writer as the gadget drives it: frames while they fit; returns the block and how many frames it took.
static Bytes encode(const Frames &frames, uint32_t capacity, uint16_t sequence, size_t *taken)
{
  Bytes buf(capacity + 64, 0xa5);   // stale bytes: the writer must zero its padding
  NtbWriter w;
  w.begin(buf.data(), capacity);
  size_t i = 0;
  for (; i < frames.size(); ++i) {
    const uint32_t room = w.room();
    if (!room || frames[i].size() > room) break;
    std::memcpy(buf.data() + w.offset(), frames[i].data(), frames[i].size());
    w.add(uint32_t(frames[i].size()));
  }
  *taken = i;
  const uint32_t total = w.finish(sequence);
  buf.resize(total);
  return buf;
}

static void writerTests()
{
  // Round trip through the strict decoder and through parseNtb16.
  const Frames frames = {frame(60, 1), frame(1514, 2), frame(42, 3), frame(14, 4), frame(1001, 5)};
  size_t taken = 0;
  Bytes b = encode(frames, kNtbMax, 7, &taken);
  CHECK(taken == frames.size());
  Frames got;
  uint16_t seq = 0;
  CHECK(specDecode(b, got, &seq) && got == frames && seq == 7);
  Frames again;
  NtbResult r = decode(b, again);
  CHECK(!r.error && r.datagrams == frames.size() && r.bad == 0 && again == frames);
  {
    // Every byte that is neither header, NDP nor datagram is zero (the 0xa5 fill of the buffer never goes out).
    std::vector<bool> owned(b.size(), false);
    for (unsigned i = 0; i < 12; ++i) owned[i] = true;
    const uint32_t ndp = le16(b, 10), ndpLen = le16(b, ndp + 4);
    for (uint32_t i = ndp; i < ndp + ndpLen; ++i) owned[i] = true;
    for (uint32_t e = ndp + 8; le16(b, e); e += 4)
      for (uint32_t i = le16(b, e); i < uint32_t(le16(b, e) + le16(b, e + 2)); ++i) owned[i] = true;
    bool padZero = true;
    for (size_t i = 0; i < b.size(); ++i) padZero &= owned[i] || b[i] == 0;
    CHECK(padZero);
    CHECK(ndp % 4 == 0 && ndpLen == 8 + 4 * (frames.size() + 1) && ndp + ndpLen == b.size());
  }
  // The smallest NTB input size a host may set still carries a full frame; a second big one goes into the next block.
  b = encode({frame(1514, 9), frame(1514, 10)}, kNtbMinIn, 1, &taken);
  CHECK(taken == 1 && b.size() <= kNtbMinIn);
  got.clear();
  CHECK(specDecode(b, got) && got.size() == 1 && got[0] == frame(1514, 9));
  // Exactly full: 1000 + 1016 bytes fill a 2048-byte block to the byte (the zero-length-packet rule's boundary case).
  b = encode({frame(1000, 1), frame(1016, 2)}, 2048, 2, &taken);
  CHECK(taken == 2 && b.size() == 2048);
  b = encode({frame(1000, 1), frame(1017, 2)}, 2048, 2, &taken);
  CHECK(taken == 1);
  // kMaxDatagrams per block, then full.
  Frames small(kMaxDatagrams + 5, frame(60, 3));
  b = encode(small, kNtbMax, 3, &taken);
  CHECK(taken == kMaxDatagrams);
  got.clear();
  CHECK(specDecode(b, got) && got.size() == kMaxDatagrams);
  // No frame: no block.
  {
    uint8_t buf[64];
    NtbWriter w;
    w.begin(buf, sizeof buf);
    CHECK(w.finish(0) == 0);
  }
  // 484 bytes: a 512-byte block (one packet; the gadget adds a zero-length packet).
  b = encode({frame(484, 1)}, kNtbMax, 0, &taken);
  CHECK(b.size() == 512);
}

static void parserTests()
{
  const Frames frames = {frame(98, 1), frame(1514, 2), frame(54, 3)};
  // Host layout (NDP first), one and two NDPs, odd padding before datagrams.
  for (bool two : {false, true})
    for (unsigned pad : {0u, 2u, 6u}) {
      const Bytes b = specEncode(frames, 5, two, pad);
      Frames got;
      const NtbResult r = decode(b, got);
      CHECK(!r.error && r.datagrams == 3 && r.bad == 0 && got == frames);
    }
  Bytes b = specEncode(frames, 5);
  // wBlockLength 0: up to the end of the transfer.
  {
    Bytes z = b;
    put16(z, 8, 0);
    Frames got;
    CHECK(!decode(z, got).error && got == frames);
  }
  // A longer transfer than the block: only the block counts.
  {
    Bytes z = b;
    z.resize(b.size() + 100, 0x77);
    Frames got;
    CHECK(!decode(z, got).error && got == frames);
  }
  struct Bad { const char *what; size_t at; unsigned bytes; uint32_t value; };
  const uint32_t ndp = le16(b, 10);
  const Bad bad[] = {
    {"NTH signature", 0, 4, 0x12345678}, {"header length", 4, 2, 16}, {"block > transfer", 8, 2, uint32_t(b.size() + 4)},
    {"no NDP", 10, 2, 0}, {"NDP misaligned", 10, 2, ndp + 2}, {"NDP beyond", 10, 2, uint32_t(b.size() - 8)},
    {"NDP signature", ndp, 4, 0x314d434e}, {"NDP length short", ndp + 4, 2, 12}, {"NDP length odd", ndp + 4, 2, 18},
    {"NDP length beyond", ndp + 4, 2, 0x4000}, {"NDP points at itself", ndp + 6, 2, ndp},
  };
  for (const Bad &x : bad) {
    Bytes z = b;
    if (x.bytes == 4) put32(z, x.at, x.value); else put16(z, x.at, x.value);
    Frames got;
    const NtbResult r = decode(z, got);
    if (!r.error) std::printf("FAIL: %s accepted\n", x.what);
    CHECK(r.error != nullptr);
    if (std::strcmp(x.what, "NDP points at itself") == 0) CHECK(got.size() == 3 * kMaxNdps);   // delivered until the cycle limit
  }
  // Bad entries are skipped, the rest delivered: past the block, shorter than a header, longer than a frame, inside the NTH.
  {
    Bytes z = b;
    put16(z, ndp + 8 + 2, 0x3fff);             // entry 0 length: beyond the block
    put16(z, ndp + 12 + 2, 13);                // entry 1: shorter than an Ethernet header
    Frames got;
    NtbResult r = decode(z, got);
    CHECK(!r.error && r.bad == 2 && r.datagrams == 1 && got.size() == 1 && got[0] == frames[2]);
    z = b;
    put16(z, ndp + 8, 4);                      // entry 0 inside the NTH16
    got.clear();
    r = decode(z, got);
    CHECK(!r.error && r.bad == 1 && got.size() == 2);
    const Frames big = {frame(1515, 1)};
    z = specEncode(big, 1);
    got.clear();
    r = decode(z, got);
    CHECK(!r.error && r.bad == 1 && got.empty());
  }
  // Truncated transfers.
  for (uint32_t n : {0u, 4u, 11u}) {
    Frames got;
    CHECK(decode(b, got, n ? n : 1).error != nullptr);
  }
  // Round trip of writer output through the parser for many sizes (alignment of every datagram start).
  for (unsigned n = 14; n <= 1514; n += 97) {
    size_t taken = 0;
    const Frames f = {frame(n, uint8_t(n)), frame(1514 - n + 14, uint8_t(n + 1)), frame(n / 2 + 14, 0)};
    Bytes e = encode(f, kNtbMax, uint16_t(n), &taken);
    Frames got;
    CHECK(taken == 3 && !decode(e, got).error && got == f);
    for (uint32_t ent = le16(e, 10) + 8; le16(e, ent); ent += 4) CHECK(le16(e, ent) % kDivisor == kRemainder);
  }
}

static void parameterTests()
{
  Bytes p(kNtbParametersBytes, 0xff);
  CHECK(ntbParameters(p.data(), kNtbMax, 8192) == 28);
  CHECK(le16(p, 0) == 28 && le16(p, 2) == 1 && le32(p, 4) == kNtbMax && le16(p, 8) == 4 && le16(p, 10) == 0 && le16(p, 12) == 4 &&
        le16(p, 14) == 0 && le32(p, 16) == 8192 && le16(p, 20) == 4 && le16(p, 22) == 0 && le16(p, 24) == 4 && le16(p, 26) == 0);
}

static void ringTests()
{
  std::vector<uint8_t> mem(256);
  FrameRing r;
  r.buf = mem.data();
  r.size = 256;
  uint8_t out[300];
  CHECK(r.get(out, sizeof out) == 0 && r.peek() == 0);
  // Fill with frames that wrap many times; order and content survive.
  unsigned put = 0, got = 0;
  for (unsigned round = 0; round < 200; ++round) {
    const unsigned n = 14 + (round * 37) % 90;
    const Bytes f = frame(n, uint8_t(round));
    while (!r.fits(n)) {
      const unsigned m = r.get(out, sizeof out);
      CHECK(m && Bytes(out, out + m) == frame(m, uint8_t(got)));
      ++got;
    }
    CHECK(r.put(f.data(), n));
    ++put;
  }
  while (const unsigned m = r.get(out, sizeof out)) { CHECK(Bytes(out, out + m) == frame(m, uint8_t(got))); ++got; }
  CHECK(put == got && r.used() == 0 && r.frames == 0);
  // Too big for the room, too big for the caller's buffer (stays queued), zero length, putWith refusing.
  const Bytes f = frame(200, 1);
  CHECK(!r.put(f.data(), 255) && r.put(f.data(), 200) && !r.put(f.data(), 60));
  CHECK(r.get(out, 100) == 0 && r.peek() == 200 && r.get(out, 200) == 200 && Bytes(out, out + 200) == f);
  CHECK(!r.put(f.data(), 0));
  CHECK(!r.putWith(20, [](uint8_t *, uint32_t, uint32_t) { return false; }) && r.used() == 0);
  r.put(f.data(), 20);
  r.clear();
  CHECK(r.used() == 0 && r.frames == 0 && r.peek() == 0);
}

static void fifoTests()
{
  // piano's core (T1 probe, logs of 2026-11-11): GHWPARAMS0 4020400a 1 01092486 6 1835003f 7 08a00d86, DWC_usb31; GTXFIFOSIZ0
  // as the firmware left it 00000043. RAM1: 3462 words of 8 bytes.
  const uint32_t hp0 = 0x4020400a, hp1 = 0x01092486, hp6 = 0x1835003f, hp7 = 0x08a00d86;
  const unsigned word = txFifoWordBytes(hp0, hp6, false);
  const uint32_t ram = txFifoRamWords(hp1, hp6, hp7);
  CHECK(word == 8 && ram == 3462);
  CHECK(txFifoRamWords(hp1 | 1u << 23, hp6, hp7) == 0x1835);   // single-port RAM: RAM0
  CHECK(txFifoWordBytes(0x4020400a, 0x100, true) == (0x140 / 8));
  // The plan of PianoDwc3.cpp: ports 0..2 (notification 1 packet, bulk IN 4), the network function (7: 1, 8: 12).
  for (bool hid : {false, true}) {
    uint8_t packets[16] = {0, 1, 4, 1, 4, uint8_t(hid ? 1 : 0), uint8_t(hid ? 4 : 0), 1, 12};
    const TxFifoPlan p = planTxFifos(packets, 16, 0x00000043, ram, word, true);
    CHECK(!p.error && p.fifos == 9 && p.begin == 0x43);
    uint32_t at = 0x43;
    for (unsigned n = 1; n < 16; ++n) {
      if (!packets[n]) { CHECK(p.value[n] == 0); continue; }
      const uint32_t depth = packets[n] * 65u + 1;
      CHECK(p.value[n] == (at << 16 | depth));
      at += depth;
    }
    CHECK(p.end == at && p.end <= ram);
    CHECK(p.end == (hid ? 0x43u + 2 * (66 + 261) + 66 + 261 + 66 + 781 : 0x43u + 2 * (66 + 261) + 66 + 781));
    std::printf("TX FIFO plan (%s): %u of %u words, FIFO 8 %08x\n", hid ? "3 ports + net" : "2 ports + net", p.end, p.ram, p.value[8]);
  }
  uint8_t packets[16] = {0, 1, 4, 1, 4, 0, 0, 1, 12};
  CHECK(planTxFifos(packets, 16, 0x43, 1000, 8, true).error != nullptr);        // too little RAM
  CHECK(planTxFifos(packets, 16, 0x00430000, ram, 8, true).error != nullptr);   // FIFO 0 without depth
  CHECK(planTxFifos(packets, 16, 0x43, ram, 6, true).error != nullptr);         // nonsense width
  CHECK(planTxFifos(packets, 16, 0x43, ram, 16, true).value[8] == ((0x43u + 2 * (34 + 133) + 34) << 16 | (12 * 33 + 1)));   // 128-bit core
  uint8_t huge[16] = {0, 255};   // 255 * 129 + 1 words of 4 bytes
  CHECK(planTxFifos(huge, 16, 0x43, 0xffff, 4, true).error != nullptr);          // a depth beyond DWC_usb31's 15 bits
  CHECK(!planTxFifos(huge, 16, 0x43, 0xffff, 4, false).error);                   // DWC_usb3: 16 bits
}

int main()
{
  writerTests();
  parserTests();
  parameterTests();
  ringTests();
  fifoTests();
  std::printf("ncm: %s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
  return failures ? 1 : 0;
}
