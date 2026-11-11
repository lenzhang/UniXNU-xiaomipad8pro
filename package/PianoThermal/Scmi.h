// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: SCMI performance protocol (0x13) over the shared-memory channel the CPUCP firmware serves, with the CPUCP mailbox
// as the doorbell. Portable (no IOKit): the kext gives the loads/stores, tests/thermal_host.cpp a model of the platform.
//
// Sources (docs/drivers/thermal.md section 2):
//   - the protocol: Arm System Control and Management Interface, DEN0056 (public specification): the shared-memory transport
//     (channel status at 0x04 bit 0 "free" / bit 1 "error", channel flags 0x10 bit 0 "interrupt on completion", length 0x14 =
//     header + payload bytes, message header 0x18, payload 0x1c; a response's first payload word is the int32 status), the
//     message header (message id [7:0], type [9:8] = 0 command, protocol id [17:10], token [27:18]) and the performance domain
//     management protocol's messages (PROTOCOL_VERSION 0x0, PROTOCOL_ATTRIBUTES 0x1, PERFORMANCE_DOMAIN_ATTRIBUTES 0x3,
//     PERFORMANCE_DESCRIBE_LEVELS 0x4, PERFORMANCE_LEVEL_SET 0x7, PERFORMANCE_LEVEL_GET 0x8) with their layouts, v3 (3-word
//     level entries) and v4 (5 words: value, power cost, attributes, indicative frequency kHz, level index; domain attribute bit 25
//     "level indexing mode": LEVEL_SET/GET then take/return the index);
//   - piano's DT: scmi { mboxes = <&cpucp 0>, <&cpucp 2>; shmem = <&scp-sram-section@0>, <@200> }, the SRAM at 0x17b4e000 (two
//     0x200 sections: tx, rx), mailbox@16430000 "qcom,sm8750-cpucp-mbox" reg <0x16430000 0x8000> <0x17830000 0x8000>; the cpus'
//     power-domains "perf" = <&scmi_perf 0> for cpu0-5, <&scmi_perf 1> for cpu6-7 (verified 2026-11-11);
//   - the doorbell: Linux drivers/mailbox/qcom-cpucp-mbox.c read as the specification (GPL: no code taken): a send writes any
//     value to the TX range + 0x100 + 8 * channel + 4; channel 0 -> 0x17830104;
//   - the same SoC, same addresses and the same polled use, hardware-verified on the REDMI K90 (SM8750) XNU port
//     (annibale_cpufreq.c: protocol 4.0, domains "NCC0" 16 levels 384-3532 MHz and "NCC1" 16 levels 1017-4320 MHz, level
//     indexing): facts only, see UPSTREAM.
// Every wait is bounded (Channel::freeWaitUs, replyWaitUs). No message other than the ones above is ever sent.
#ifndef PIANO_THERMAL_SCMI_H
#define PIANO_THERMAL_SCMI_H
#include <stddef.h>
#include <stdint.h>

namespace pianothermal {
namespace scmi {

constexpr uint64_t kShmemTx = 0x17b4e000;   // scp-sram-section@0 ("tx")
constexpr uint32_t kShmemBytes = 0x200;
constexpr uint64_t kMboxTx = 0x17830000;     // mailbox@16430000's second reg
constexpr uint32_t kDoorbell = 0x100 + 8 * 0 + 4;   // channel 0

constexpr uint32_t kChanStatus = 0x04, kChanFlags = 0x10, kLength = 0x14, kHeader = 0x18, kPayload = 0x1c;
constexpr uint32_t kStatusFree = 1u << 0, kStatusError = 1u << 1;
constexpr unsigned kPayloadWords = (kShmemBytes - kPayload) / 4;   // 121

constexpr uint8_t kProtocolPerf = 0x13;
enum : uint8_t {
  kVersion = 0x0,
  kAttributes = 0x1,
  kDomainAttributes = 0x3,
  kDescribeLevels = 0x4,
  kLevelSet = 0x7,
  kLevelGet = 0x8,
};
// PERFORMANCE_DOMAIN_ATTRIBUTES attributes
constexpr uint32_t kDomSetLimits = 1u << 31, kDomSetLevel = 1u << 30, kDomFastChannel = 1u << 27, kDomLevelIndexing = 1u << 25;

// Transport results beside the SCMI status codes (0 success, -1 not supported, -2 invalid parameters, -3 denied, -4 not found,
// -5 out of range, -6 busy, -7 comms error, -8 generic error, -9 hardware error, -10 protocol error).
enum : int32_t {
  kErrBusy = -100,        // the channel never came free (someone else's message, or the firmware is not serving it)
  kErrTimeout = -101,     // no answer in replyWaitUs
  kErrChannel = -102,     // the channel's error bit
  kErrWrongAnswer = -103, // the answer's header is not our message's
  kErrShort = -104,       // fewer return words than the message has
  kErrNoChannel = -105,
};

inline const char *statusName(int32_t s)
{
  switch (s) {
    case 0: return "success";
    case -1: return "not supported";
    case -2: return "invalid parameters";
    case -3: return "denied";
    case -4: return "not found";
    case -5: return "out of range";
    case -6: return "busy";
    case -7: return "comms error";
    case -8: return "generic error";
    case -9: return "hardware error";
    case -10: return "protocol error";
    case kErrBusy: return "channel never free";
    case kErrTimeout: return "no answer";
    case kErrChannel: return "channel error";
    case kErrWrongAnswer: return "answer to another message";
    case kErrShort: return "short answer";
    case kErrNoChannel: return "no channel";
  }
  return "unknown status";
}

inline uint32_t header(uint8_t message, uint8_t protocol, uint32_t token)
{
  return uint32_t(message) | uint32_t(protocol) << 10 | (token & 0x3ff) << 18;
}
struct Header {
  uint8_t message, type, protocol;
  uint16_t token;
};
inline Header decodeHeader(uint32_t h)
{
  return {uint8_t(h & 0xff), uint8_t((h >> 8) & 3), uint8_t((h >> 10) & 0xff), uint16_t((h >> 18) & 0x3ff)};
}

// The kext's access to the channel (offsets within the tx section) and the doorbell; timing from the caller.
struct Io {
  void *context;
  uint32_t (*read)(void *context, uint32_t offset);
  void (*write)(void *context, uint32_t offset, uint32_t value);
  void (*ring)(void *context);                 // the doorbell (after a barrier)
  uint64_t (*nowUs)(void *context);
  void (*pause)(void *context, uint32_t us);   // short wait while polling
};

class Channel {
 public:
  Io io{};
  uint32_t freeWaitUs = 10000, replyWaitUs = 50000;
  // statistics
  uint32_t messages = 0, failures = 0, maxUs = 0;
  uint64_t totalUs = 0;
  int32_t lastError = 0;

  bool ready() const { return io.read && io.write && io.ring && io.nowUs && io.pause; }

  // One command of protocol 0x13: in[0..inWords) out; on return *outWords (at most what it was) return words after the status in
  // out. The SCMI status, or one of kErr*. A transport failure (busy, timeout, channel error, wrong answer) is sticky in
  // lastError so the caller can stop using the channel.
  int32_t call(uint8_t message, const uint32_t *in, unsigned inWords, uint32_t *out, unsigned *outWords)
  {
    unsigned room = outWords ? *outWords : 0;
    if (outWords) *outWords = 0;
    if (!ready()) return kErrNoChannel;
    if (inWords > kPayloadWords) return -2;
    const uint64_t start = io.nowUs(io.context);
    const uint32_t h = header(message, kProtocolPerf, token_++);
    int32_t result = 0;
    if (!waitFree(start, freeWaitUs)) {
      result = kErrBusy;
    } else {
      io.write(io.context, kChanStatus, 0);   // ours now: not free, no error
      io.write(io.context, kChanFlags, 0);    // polled: no completion interrupt
      io.write(io.context, kLength, 4 + 4 * inWords);
      io.write(io.context, kHeader, h);
      for (unsigned i = 0; i < inWords; ++i) io.write(io.context, kPayload + 4 * i, in[i]);
      io.ring(io.context);
      if (!waitFree(io.nowUs(io.context), replyWaitUs)) {
        result = kErrTimeout;
      } else if (io.read(io.context, kChanStatus) & kStatusError) {
        result = kErrChannel;
      } else if (io.read(io.context, kHeader) != h) {
        result = kErrWrongAnswer;
      } else {
        const uint32_t length = io.read(io.context, kLength);
        unsigned got = length >= 8 ? (length - 8) / 4 : 0;
        if (got > kPayloadWords - 1) got = kPayloadWords - 1;
        if (got > room) got = room;
        for (unsigned i = 0; i < got; ++i) out[i] = io.read(io.context, kPayload + 4 + 4 * i);
        if (outWords) *outWords = got;
        result = length >= 8 ? int32_t(io.read(io.context, kPayload)) : kErrShort;
      }
    }
    const uint64_t us = io.nowUs(io.context) - start;
    ++messages;
    totalUs += us;
    if (us > maxUs) maxUs = uint32_t(us);
    if (result != 0) ++failures;
    if (result <= kErrBusy && result >= kErrWrongAnswer) lastError = result;
    return result;
  }

 private:
  bool waitFree(uint64_t from, uint32_t limitUs)
  {
    for (;;) {
      if (io.read(io.context, kChanStatus) & kStatusFree) return true;
      if (io.nowUs(io.context) - from >= limitUs) return false;
      io.pause(io.context, 10);
    }
  }
  uint32_t token_ = 0;
};

// ------------------------------------------------------------------------------------------------------------- perf domains
constexpr unsigned kMaxLevels = 32;

struct Level {
  uint32_t key;        // what LEVEL_SET takes and LEVEL_GET returns (the index in indexing mode, else the value)
  uint32_t value;      // performance level value
  uint32_t khz;        // indicative frequency (v4), else value scaled by sustained kHz / sustained level, else value
  uint32_t latencyUs;  // attributes [15:0]
};

struct Domain {
  char name[17];
  uint32_t attributes, rateLimitUs, sustainedKhz, sustainedLevel;
  bool indexing, canSet;
  uint8_t count;           // levels, slowest first
  Level levels[kMaxLevels];
  uint8_t sustained;       // index of the fastest level at or under the sustained frequency
};

struct Protocol {
  uint32_t version;        // major [31:16], minor [15:0]
  uint32_t domains;
};

inline int32_t queryProtocol(Channel &ch, Protocol &p)
{
  uint32_t out[4];
  unsigned n = 1;
  p = {};
  int32_t r = ch.call(kVersion, nullptr, 0, out, &n);
  if (r) return r;
  if (n < 1) return kErrShort;
  p.version = out[0];
  n = 4;
  r = ch.call(kAttributes, nullptr, 0, out, &n);
  if (r) return r;
  if (n < 1) return kErrShort;
  p.domains = out[0] & 0xffff;
  return 0;
}

// Insert one level, keeping them slowest first.
inline void addLevel(Domain &d, const Level &l)
{
  if (d.count >= kMaxLevels) return;
  unsigned j = d.count++;
  for (; j > 0 && d.levels[j - 1].khz > l.khz; --j) d.levels[j] = d.levels[j - 1];
  d.levels[j] = l;
}

// The levels of one DESCRIBE_LEVELS answer (out[0] = number of levels: returned [11:0], remaining [31:16]; then the entries).
// Returns the number of entries taken, or -1 when the answer is malformed. remaining is set from the answer.
inline int parseLevels(Domain &d, uint32_t version, const uint32_t *out, unsigned words, uint32_t &remaining)
{
  remaining = 0;
  if (words < 1) return -1;
  const unsigned entry = version >= 0x40000 ? 5 : 3;
  const unsigned returned = out[0] & 0xfff;
  remaining = out[0] >> 16;
  if (1 + returned * entry > words) return -1;
  for (unsigned i = 0; i < returned; ++i) {
    const uint32_t *e = out + 1 + i * entry;
    Level l{};
    l.value = e[0];
    l.latencyUs = e[2] & 0xffff;
    if (entry == 5 && e[3]) l.khz = e[3];
    else if (d.sustainedLevel && d.sustainedKhz) l.khz = uint32_t(uint64_t(e[0]) * d.sustainedKhz / d.sustainedLevel);
    else l.khz = e[0];
    l.key = (entry == 5 && d.indexing) ? e[4] : e[0];
    addLevel(d, l);
  }
  return int(returned);
}

inline int32_t describeDomain(Channel &ch, uint32_t version, uint32_t domain, Domain &d)
{
  d = {};
  uint32_t out[kPayloadWords];
  unsigned n = 8;
  int32_t r = ch.call(kDomainAttributes, &domain, 1, out, &n);
  if (r) return r;
  if (n < 4) return kErrShort;
  d.attributes = out[0];
  d.rateLimitUs = out[1] & 0xfffff;
  d.sustainedKhz = out[2];
  d.sustainedLevel = out[3];
  d.indexing = version >= 0x40000 && (d.attributes & kDomLevelIndexing);
  d.canSet = (d.attributes & kDomSetLevel) != 0;
  if (n >= 8)
    for (unsigned i = 0; i < 16; ++i) d.name[i] = char((out[4 + i / 4] >> (8 * (i % 4))) & 0xff);
  d.name[16] = 0;
  for (uint32_t index = 0; d.count < kMaxLevels;) {
    const uint32_t in[2] = {domain, index};
    n = kPayloadWords - 1;
    r = ch.call(kDescribeLevels, in, 2, out, &n);
    if (r) return r;
    uint32_t remaining = 0;
    const int got = parseLevels(d, version, out, n, remaining);
    if (got <= 0) break;
    index += uint32_t(got);
    if (!remaining) break;
  }
  if (!d.count) return kErrShort;
  d.sustained = uint8_t(d.count - 1);
  if (d.sustainedKhz) {
    d.sustained = 0;
    for (unsigned i = 0; i < d.count; ++i)
      if (d.levels[i].khz <= d.sustainedKhz) d.sustained = uint8_t(i);
  }
  return 0;
}

// The index of a level key; -1 when the domain has no such level.
inline int indexOfKey(const Domain &d, uint32_t key)
{
  for (unsigned i = 0; i < d.count; ++i)
    if (d.levels[i].key == key) return int(i);
  return -1;
}

inline int32_t levelGet(Channel &ch, const Domain &d, uint32_t domain, int &index)
{
  uint32_t out[1];
  unsigned n = 1;
  index = -1;
  const int32_t r = ch.call(kLevelGet, &domain, 1, out, &n);
  if (r) return r;
  if (n < 1) return kErrShort;
  index = indexOfKey(d, out[0]);
  return 0;
}

inline int32_t levelSet(Channel &ch, const Domain &d, uint32_t domain, unsigned index)
{
  if (index >= d.count) return -2;
  const uint32_t in[2] = {domain, d.levels[index].key};
  unsigned n = 0;
  return ch.call(kLevelSet, in, 2, nullptr, &n);
}

// The fastest level at or under khz (0 when even the slowest is faster), the slowest at or over khz (the fastest when none is).
inline unsigned fastestAtMost(const Domain &d, uint32_t khz)
{
  unsigned found = 0;
  for (unsigned i = 0; i < d.count; ++i)
    if (d.levels[i].khz <= khz) found = i;
  return found;
}
inline unsigned slowestAtLeast(const Domain &d, uint32_t khz)
{
  for (unsigned i = 0; i < d.count; ++i)
    if (d.levels[i].khz >= khz) return i;
  return d.count ? d.count - 1 : 0;
}

}  // namespace scmi
}  // namespace pianothermal

#endif
