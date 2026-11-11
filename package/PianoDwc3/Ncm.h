// SPDX-License-Identifier: BSD-2-Clause-Patent
// CDC-NCM for the piano network function (PianoDwc3, piano-usb-net=1): NTB16 framing of the bulk transfers, the NTB parameters
// the host reads, the class request and notification codes, and the frame queue between the USB thread and the network side.
// From the USB CDC NCM 1.0 specification (public): section 3 (NTH16, NDP16, datagram alignment), 6.2 (requests), 6.3
// (notifications), 7 (alternate settings); no other source. Portable (no kernel headers): built into the kext and into
// tests/ncm_host.cpp and tests/usb_gadget_host.cpp.
#pragma once
#include <stdint.h>

namespace pianousb {
namespace ncm {

constexpr uint32_t kNth16 = 0x484d434eu;      // "NCMH"
constexpr uint32_t kNdp16 = 0x304d434eu;      // "NCM0": datagrams without CRC (the only kind accepted)
constexpr uint32_t kNdp16Crc = 0x314d434eu;   // "NCM1"
constexpr uint32_t kNth16Bytes = 12, kNdp16Min = 16;
constexpr uint32_t kNtbMax = 0x4000;   // dwNtbInMaxSize and dwNtbOutMaxSize: the 16 KiB DMA buffers (Dwc3Gadget.h Layout)
constexpr uint32_t kNtbMinIn = 2048;   // the smallest NTB input size a host may set (6.2.7)
constexpr uint16_t kDivisor = 4, kRemainder = 0, kNdpAlign = 4;   // datagram and NDP alignment, both directions
constexpr uint16_t kMaxFrame = 1514, kMinFrame = 14;              // an Ethernet frame without FCS; at least its header
constexpr unsigned kMaxDatagrams = 32;                            // per NTB this side sends
constexpr unsigned kMaxNdps = 8;                                  // per NTB this side parses (a cycle ends there)

// Class requests to the communications interface (CDC 1.2 / ECM 1.2 / NCM 1.0, bRequest)
enum : uint8_t {
  SET_ETHERNET_MULTICAST_FILTERS = 0x40, SET_ETHERNET_PACKET_FILTER = 0x43, GET_ETHERNET_STATISTIC = 0x44,
  GET_NTB_PARAMETERS = 0x80, GET_NET_ADDRESS = 0x81, SET_NET_ADDRESS = 0x82, GET_NTB_FORMAT = 0x83, SET_NTB_FORMAT = 0x84,
  GET_NTB_INPUT_SIZE = 0x85, SET_NTB_INPUT_SIZE = 0x86, GET_MAX_DATAGRAM_SIZE = 0x87, SET_MAX_DATAGRAM_SIZE = 0x88,
  GET_CRC_MODE = 0x89, SET_CRC_MODE = 0x8a,
};
// Notifications on the interrupt endpoint (bmRequestType 0xa1)
constexpr uint8_t kNetworkConnection = 0x00, kSpeedChange = 0x2a;
constexpr uint32_t kNtbParametersBytes = 28;

inline uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline void wr16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

// GET_NTB_PARAMETERS (6.2.1): NTB16 only, the sizes of this side's buffers, 4-byte alignment both ways, no datagram limit.
inline unsigned ntbParameters(uint8_t *o, uint32_t inMax, uint32_t outMax)
{
  wr16(o + 0, kNtbParametersBytes);
  wr16(o + 2, 0x0001);   // bmNtbFormatsSupported: NTB16
  wr32(o + 4, inMax);    // dwNtbInMaxSize
  wr16(o + 8, kDivisor); wr16(o + 10, kRemainder); wr16(o + 12, kNdpAlign); wr16(o + 14, 0);
  wr32(o + 16, outMax);  // dwNtbOutMaxSize
  wr16(o + 20, kDivisor); wr16(o + 22, kRemainder); wr16(o + 24, kNdpAlign);
  wr16(o + 26, 0);       // wNtbOutMaxDatagrams: no limit
  return kNtbParametersBytes;
}

// One NTB16 to the host, built in place: NTH16, the datagrams (each at offset % kDivisor == kRemainder), then one NDP16 (4-byte
// aligned) that lists them. Padding is zeroed: nothing stale from the buffer goes out.
//   w.begin(buf, size); while ((room = w.room()) && (n = copy frame to buf + w.offset(), at most room)) w.add(n); w.finish(seq)
class NtbWriter {
public:
  void begin(uint8_t *buffer, uint32_t capacity)
  {
    buf_ = buffer;
    cap_ = (capacity > 0xffff ? 0xffff : capacity) & ~3u;
    end_ = kNth16Bytes;
    count_ = 0;
  }
  uint32_t offset() const { return align(end_); }
  // The largest datagram that still fits with its NDP entry (0: the NTB is full).
  uint32_t room() const
  {
    if (count_ >= kMaxDatagrams) return 0;
    const uint32_t at = offset(), ndp = ndpBytes(count_ + 1);
    return cap_ > at + ndp ? cap_ - at - ndp : 0;
  }
  void add(uint32_t length)
  {
    const uint32_t at = offset();
    for (uint32_t i = end_; i < at; ++i) buf_[i] = 0;
    index_[count_] = uint16_t(at);
    length_[count_] = uint16_t(length);
    end_ = at + length;
    ++count_;
  }
  unsigned count() const { return count_; }
  // The NDP and the header; returns the block length (0 without datagrams).
  uint32_t finish(uint16_t sequence)
  {
    if (!count_) return 0;
    const uint32_t ndp = (end_ + kNdpAlign - 1) & ~uint32_t(kNdpAlign - 1), ndpLength = ndpBytes(count_);
    for (uint32_t i = end_; i < ndp; ++i) buf_[i] = 0;
    wr32(buf_ + ndp, kNdp16);
    wr16(buf_ + ndp + 4, ndpLength);
    wr16(buf_ + ndp + 6, 0);   // wNextNdpIndex: the only one
    for (unsigned i = 0; i < count_; ++i) {
      wr16(buf_ + ndp + 8 + 4 * i, index_[i]);
      wr16(buf_ + ndp + 10 + 4 * i, length_[i]);
    }
    wr32(buf_ + ndp + 8 + 4 * count_, 0);   // the null entry ends the list
    const uint32_t total = ndp + ndpLength;
    wr32(buf_, kNth16);
    wr16(buf_ + 4, kNth16Bytes);
    wr16(buf_ + 6, sequence);
    wr16(buf_ + 8, total);
    wr16(buf_ + 10, ndp);
    return total;
  }

private:
  static uint32_t ndpBytes(uint32_t datagrams) { return 8 + 4 * (datagrams + 1); }   // header, entries, null entry
  static uint32_t align(uint32_t at) { return at + (kRemainder + kDivisor - at % kDivisor) % kDivisor; }
  uint8_t *buf_ = nullptr;
  uint32_t cap_ = 0, end_ = 0;
  unsigned count_ = 0;
  uint16_t index_[kMaxDatagrams] = {}, length_[kMaxDatagrams] = {};
};

struct NtbResult {
  unsigned datagrams = 0;        // passed to the sink
  unsigned bad = 0;              // entries outside the block or of an impossible length (skipped)
  const char *error = nullptr;   // the NTB as a whole refused there (datagrams before it were delivered)
};

// One NTB16 from the host (got: the bytes of the transfer). sink(offset, length) for every datagram that lies inside the block and
// has the length of an Ethernet frame. A block length of 0 means "up to the short packet", the transfer's length (3.2.1).
template <class Sink> NtbResult parseNtb16(const uint8_t *b, uint32_t got, Sink &&sink)
{
  NtbResult r;
  if (got < kNth16Bytes) { r.error = "shorter than an NTH16"; return r; }
  if (rd32(b) != kNth16) { r.error = "no NTH16 signature"; return r; }
  if (rd16(b + 4) != kNth16Bytes) { r.error = "NTH16 header length"; return r; }
  uint32_t block = rd16(b + 8);
  if (!block) block = got;
  if (block > got) { r.error = "block longer than the transfer"; return r; }
  uint32_t ndp = rd16(b + 10);
  if (!ndp) { r.error = "no NDP"; return r; }
  for (unsigned n = 0; ndp; ++n) {
    if (n == kMaxNdps) { r.error = "too many NDPs"; return r; }
    if (ndp % 4 || ndp < kNth16Bytes || ndp + kNdp16Min > block) { r.error = "NDP outside the block"; return r; }
    const uint32_t signature = rd32(b + ndp), length = rd16(b + ndp + 4);
    if (signature != kNdp16) { r.error = signature == kNdp16Crc ? "NDP with CRC" : "no NDP16 signature"; return r; }
    if (length < kNdp16Min || length % 4 || ndp + length > block) { r.error = "NDP16 length"; return r; }
    for (uint32_t e = ndp + 8; e + 4 <= ndp + length; e += 4) {
      const uint32_t at = rd16(b + e), size = rd16(b + e + 2);
      if (!at || !size) break;   // the null entry
      if (at < kNth16Bytes || at + size > block || size < kMinFrame || size > kMaxFrame) { ++r.bad; continue; }
      ++r.datagrams;
      sink(at, size);
    }
    ndp = rd16(b + ndp + 6);
  }
  return r;
}

// What the gadget core counts for the network function (USB thread; others only read). "tx": to the host (bulk IN), "rx": from it.
struct NetStats {
  uint32_t txNtbs, txFrames, rxNtbs, rxFrames, ntbErrors, badDatagrams, rxDrops, notifications, altChanges, zlps;
  uint64_t txBytes, rxBytes;
  uint32_t ntbInSize;   // the host's NTB input size (SET_NTB_INPUT_SIZE), or kNtbMax
  uint16_t filter;      // the host's packet filter (SET_ETHERNET_PACKET_FILTER), accepted and not applied
  uint8_t alt;          // the data interface's alternate setting: 1 = the host runs the link
  const char *lastError;
};

// Whole frames in a byte ring: a 16-bit length, then the bytes. One producer and one consumer, serialized by the owner's lock.
// size must be a power of two; the buffer is the owner's.
struct FrameRing {
  uint8_t *buf = nullptr;
  uint32_t size = 0, head = 0, tail = 0, frames = 0;

  uint32_t used() const { return head - tail; }
  uint32_t space() const { return size - used(); }
  bool fits(uint32_t n) const { return n && n <= 0xffff && space() >= n + 2; }
  // copy(dst, offset, length) fills the frame's bytes [offset, offset + length) at dst, in at most two pieces; false: dropped.
  template <class Copy> bool putWith(uint32_t n, Copy &&copy)
  {
    if (!buf || !fits(n)) return false;
    byte(head) = uint8_t(n);
    byte(head + 1) = uint8_t(n >> 8);
    const uint32_t at = (head + 2) & (size - 1), first = size - at < n ? size - at : n;
    if (!copy(buf + at, 0u, first)) return false;
    if (first < n && !copy(buf, first, n - first)) return false;
    head += n + 2;
    ++frames;
    return true;
  }
  bool put(const uint8_t *s, uint32_t n)
  {
    return putWith(n, [s](uint8_t *d, uint32_t off, uint32_t len) { __builtin_memcpy(d, s + off, len); return true; });
  }
  uint32_t peek() const { return used() >= 2 ? uint32_t(byte(tail)) | uint32_t(byte(tail + 1)) << 8 : 0; }
  // The next frame into d if it is at most cap bytes; 0 when there is none or it does not fit (it stays queued).
  uint32_t get(uint8_t *d, uint32_t cap)
  {
    const uint32_t n = peek();
    if (!n || n > cap) return 0;
    const uint32_t at = (tail + 2) & (size - 1), first = size - at < n ? size - at : n;
    __builtin_memcpy(d, buf + at, first);
    if (first < n) __builtin_memcpy(d + first, buf, n - first);
    tail += n + 2;
    --frames;
    return n;
  }
  void clear() { tail = head; frames = 0; }

private:
  uint8_t &byte(uint32_t i) const { return buf[i & (size - 1)]; }
};

}  // namespace ncm
}  // namespace pianousb
