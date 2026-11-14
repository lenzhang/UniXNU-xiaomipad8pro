// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi: the WLAN firmware's crash dump (RDDM) read back over BHIe, the way the MHI host collects it, and a compact decode for
// the log (host tests: tests/qmi_host.cpp). New code for this repository. The register offsets and the dump's size are WifiHw.h's
// facts (the MHI specification's names as the mainline host uses them; ath12k's configuration of this chip for the size); the
// layout of the dump's header is the vendor MHI host's (read for the layout only: nothing copied).
//
// How the dump travels. Before the device's power-up the host zeroes BHIe's receive block and gives it a vector table, entries of
// {address, size} as for the image, over segments of host memory, with a session number in the receive doorbell (rddmPlan,
// rddmArm). When the firmware dies it reports a system error and its crash dump environment (EE RDDM), writes its memory into those
// segments itself, and then sets the receive status: 31:30 = 2 (complete) with the session in 29:0, or 3 on an error. The host
// only waits for that status (rddmPoll) and then reads its own memory.
//
// The dump is a header and then files back to back, across the segments as if they were one buffer: the header is a version, its
// own size in bytes, and one 64-byte entry per file {the region's device address (8), its physical address (8), its size (8), a
// 20-character description, a 20-character file name}; file 0 begins right after the header. One file, named with "SFR", is the
// subsystem failure reason: the firmware's own one-line account of why it died.
//
// What the log gets from it (rddmDecode), never the dump itself: the receive status, how many segments hold data and where the
// data ends; the header's version and size; each file's name, description, size and offset (at most kRddmMaxFiles); the failure
// reason's text, at most kRddmReasonBytes printable characters; and only when there is no reason, at most kRddmMaxStrings printable
// strings of the first segment that read like the account of an error (an assertion, a fatal error, a file and line; never a
// format string), at most kRddmStringBytes each. Every printed string has anything that reads as the chip's serial number masked.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "WifiHw.h"
#include "WifiMhi.h"

namespace pianowifi {

constexpr uint32_t kRddmMaxSegments = 16, kRddmTableBytes = 4096;
constexpr uint32_t kRddmEntryBytes = 64, kRddmNameBytes = 20, kRddmMaxFiles = 16, kRddmHeaderWords = 8;
constexpr uint32_t kRddmHeaderLimit = kRddmHeaderWords + 64 * kRddmEntryBytes;   // larger is not a header
constexpr uint32_t kRddmReasonBytes = 200, kRddmMaxStrings = 4, kRddmStringBytes = 120, kRddmMinString = 16;
constexpr uint32_t kRddmScratchBytes = 4096;   // the caller's bounce buffer for every read of the dump

struct Rddm {
  uint8_t *seg[kRddmMaxSegments] = {};
  uint64_t segBus[kRddmMaxSegments] = {};
  uint32_t segments = 0, segBytes = 0;
  uint8_t *table = nullptr;
  uint64_t tableBus = 0;
  uint32_t tableBytes = 0;
  uint32_t seq = 0, arms = 0, status = 0;   // the session of the last arm; the receive status last read
};

namespace rddmdetail {
inline void put64(uint8_t *at, uint64_t v)
{
  for (unsigned i = 0; i < 8; ++i) at[i] = uint8_t(v >> (8 * i));
}
inline uint32_t get32(const uint8_t *at)
{
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) v |= uint32_t(at[i]) << (8 * i);
  return v;
}
inline uint64_t get64(const uint8_t *at) { return uint64_t(get32(at)) | uint64_t(get32(at + 4)) << 32; }
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
inline bool contains(const char *text, const char *word)
{
  for (const char *t = text; *t; ++t) {
    const char *a = t, *b = word;
    while (*a && *b && lower(*a) == lower(*b)) ++a, ++b;
    if (!*b) return true;
  }
  return false;
}
inline bool printable(uint8_t c) { return c >= 0x20 && c < 0x7f; }
}  // namespace rddmdetail

// The segments (each aligned to its size by the arena's rule) and the vector table (a page of its own, as the image's is) out of
// the arena, the table filled with every segment at its whole size. False when it does not fit; what was taken stays taken (the
// arena is a bump allocator).
inline bool rddmPlan(Rddm &r, Arena &arena, uint32_t bytes, uint32_t segBytes = kRddmSegBytes)
{
  using namespace rddmdetail;
  renew(r);
  if (!bytes || !segBytes) return false;
  const uint32_t n = (bytes + segBytes - 1) / segBytes;
  if (n > kRddmMaxSegments || n * 16 > kRddmTableBytes) return false;
  Rddm plan;
  for (uint32_t i = 0; i < n; ++i) {
    plan.seg[i] = arenaAlloc(arena, segBytes, &plan.segBus[i]);
    if (!plan.seg[i]) return false;
  }
  plan.table = arenaAlloc(arena, kRddmTableBytes, &plan.tableBus);
  if (!plan.table) return false;
  plan.segments = n;
  plan.segBytes = segBytes;
  plan.tableBytes = n * 16;
  for (uint32_t i = 0; i < n; ++i) {
    put64(plan.table + 16 * i, plan.segBus[i]);
    put64(plan.table + 16 * i + 8, segBytes);
  }
  r = plan;
  return true;
}

// The table given to the device: its address high then low, its length, then the session in the doorbell's 29:0 (the doorbell's
// other bits kept), the order the MHI host writes them in. `bhie` is the BHIe block's offset.
inline void rddmArm(Rddm &r, const MhiIo &io, uint32_t bhie, uint32_t seq)
{
  r.seq = seq & kMhiSeqMask;
  if (!r.seq) r.seq = 1;
  io.write(io.ctx, bhie + kBhieRxVecAddrHi, uint32_t(r.tableBus >> 32));
  io.write(io.ctx, bhie + kBhieRxVecAddrLo, uint32_t(r.tableBus));
  io.write(io.ctx, bhie + kBhieRxVecSize, r.tableBytes);
  io.write(io.ctx, bhie + kBhieRxVecDb, (io.read(io.ctx, bhie + kBhieRxVecDb) & ~kMhiSeqMask) | r.seq);
  ++r.arms;
}

enum RddmPoll : uint32_t { kRddmWaiting = 0, kRddmComplete, kRddmError };
inline uint32_t rddmPoll(Rddm &r, const MhiIo &io, uint32_t bhie)
{
  r.status = io.read(io.ctx, bhie + kBhieRxVecStatus);
  if (r.status == 0xffffffffu) return kRddmWaiting;   // a device that does not answer: not a status
  const uint32_t state = r.status >> 30;
  return state == kMhiXferSuccess ? kRddmComplete : state == kMhiXferError ? kRddmError : kRddmWaiting;
}
inline bool rddmSessionMatches(const Rddm &r) { return (r.status & kMhiSeqMask) == r.seq; }
inline const char *rddmPollName(uint32_t poll)
{
  return poll == kRddmComplete ? "complete" : poll == kRddmError ? "an error" : "not complete";
}

// Up to n bytes of the dump at `offset`, as if the segments were one buffer, through copy(to, from, bytes). Returns the count.
template <class Copy>
inline uint32_t rddmRead(const Rddm &r, uint64_t offset, uint8_t *to, uint32_t n, Copy copy)
{
  const uint64_t total = uint64_t(r.segments) * r.segBytes;
  if (!r.segments || offset >= total) return 0;
  if (offset + n > total) n = uint32_t(total - offset);
  uint32_t done = 0;
  while (done < n) {
    const uint64_t at = offset + done;
    const uint32_t s = uint32_t(at / r.segBytes), within = uint32_t(at % r.segBytes);
    uint32_t k = r.segBytes - within;
    if (k > n - done) k = n - done;
    copy(to + done, r.seg[s] + within, k);
    done += k;
  }
  return n;
}

// Anything in `text` that reads as the chip's serial number (its eight hex digits in either case, or its decimal form when that is
// at least six digits) is overwritten with '-'. A serial of 0 masks nothing.
inline void rddmMask(char *text, uint32_t serial)
{
  if (!serial || !text) return;
  char forms[3][12] = {};
  static const char hexLower[] = "0123456789abcdef", hexUpper[] = "0123456789ABCDEF";
  for (unsigned i = 0; i < 8; ++i) {
    forms[0][i] = hexLower[(serial >> (28 - 4 * i)) & 0xfu];
    forms[1][i] = hexUpper[(serial >> (28 - 4 * i)) & 0xfu];
  }
  char digits[12] = {};
  unsigned n = 0;
  for (uint32_t v = serial; v && n < 10; v /= 10) digits[n++] = char('0' + v % 10);
  for (unsigned i = 0; i < n; ++i) forms[2][i] = digits[n - 1 - i];
  if (n < 6) forms[2][0] = 0;
  for (const char *form : forms) {
    const size_t len = __builtin_strlen(form);
    if (!len) continue;
    for (char *t = text; *t; ++t) {
      size_t k = 0;
      while (k < len && t[k] && t[k] == form[k]) ++k;
      if (k == len)
        for (size_t j = 0; j < len; ++j) t[j] = '-';
    }
  }
}

struct RddmFile {
  uint64_t base = 0, phys = 0, size = 0, offset = 0;
  char description[kRddmNameBytes + 1] = {}, name[kRddmNameBytes + 1] = {};
  bool inDump = false;   // the whole file lies inside the segments
};
struct RddmString {
  uint64_t at = 0;
  char text[kRddmStringBytes + 1] = {};
};
struct RddmReport {
  uint32_t word0 = 0, word1 = 0;     // the dump's first two words as read
  uint32_t version = 0, headerBytes = 0, entries = 0, files = 0;
  bool header = false, sizeFirst = false;
  RddmFile file[kRddmMaxFiles];
  int reasonFile = -1;
  char reason[kRddmReasonBytes + 1] = {};
  unsigned strings = 0, candidates = 0;   // strings kept, error-like strings seen in all
  RddmString string[kRddmMaxStrings];
  uint32_t segmentsWithData = 0;
  uint64_t dataEnd = 0;              // one past the last non-zero byte (0: the dump is all zeros)
  const char *why = nullptr;         // why the header was not taken
};

namespace rddmdetail {
// A fixed-width field as a printable string: up to `bytes` characters, ending at the first NUL; false when it is empty or holds a
// byte that is not printable before its end.
inline bool field(const uint8_t *at, uint32_t bytes, char *out)
{
  uint32_t n = 0;
  for (; n < bytes && at[n]; ++n) {
    if (!printable(at[n])) {
      out[0] = 0;
      return false;
    }
    out[n] = char(at[n]);
  }
  out[n] = 0;
  return n > 0;
}
// Text as the log may show it: up to the first NUL, white space as one space, any other unprintable byte as '.', trimmed.
inline uint32_t text(const uint8_t *at, uint32_t bytes, char *out, uint32_t room)
{
  uint32_t n = 0;
  bool space = true;
  for (uint32_t i = 0; i < bytes && at[i] && n + 1 < room; ++i) {
    const uint8_t c = at[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!space) out[n++] = ' ';
      space = true;
      continue;
    }
    out[n++] = printable(c) ? char(c) : '.';
    space = false;
  }
  while (n && out[n - 1] == ' ') --n;
  out[n] = 0;
  return n;
}
// How strongly a printable string reads as the account of an error: 2 for an assertion, a fatal error, an exception, an abort,
// a crash or a file and line; 1 for an error, a fault, a failure, a watchdog or a timeout; 0 otherwise, and always 0 for a format
// string (the firmware's own text before it was filled in is not the account of anything).
inline unsigned errorScore(const char *s)
{
  for (const char *t = s; *t; ++t)
    if (*t == '%') return 0;
  static const char *const strong[] = {"assert", "fatal", "exception", "abort", "panic", "crash", ".c:", "sfr"};
  static const char *const weak[] = {"error", "err_", "fault", "fail", "watchdog", "wdog", "timeout", "timed out"};
  for (const char *w : strong)
    if (contains(s, w)) return 2;
  for (const char *w : weak)
    if (contains(s, w)) return 1;
  return 0;
}
inline bool sfrName(const char *name) { return contains(name, "sfr"); }
}  // namespace rddmdetail

// The decode. `scratch` is a buffer of kRddmScratchBytes; copy(to, from, bytes) reads the segments (the window is write-combined:
// one copy per chunk, never a byte loop over it). `serial` is masked in every string kept. `out` is about 2 KiB: the kext keeps it
// on the heap, and it is cleared here in place (no temporary on a kernel stack).
template <class Copy>
inline void rddmDecode(const Rddm &r, uint8_t *scratch, Copy copy, uint32_t serial, RddmReport &out)
{
  using namespace rddmdetail;
  static_assert(__is_trivially_copyable(RddmReport), "the report is cleared bytewise");
  __builtin_memset(static_cast<void *>(&out), 0, sizeof out);
  out.reasonFile = -1;
  if (!r.segments || !scratch) {
    out.why = "no dump was planned";
    return;
  }
  const uint64_t total = uint64_t(r.segments) * r.segBytes;
  // Where the data is: a segment with any non-zero word, and one past the last non-zero byte.
  for (uint32_t s = 0; s < r.segments; ++s) {
    bool any = false;
    for (uint32_t off = 0; off < r.segBytes; off += kRddmScratchBytes) {
      const uint32_t n = r.segBytes - off < kRddmScratchBytes ? r.segBytes - off : kRddmScratchBytes;
      copy(scratch, r.seg[s] + off, n);
      for (uint32_t i = n; i-- > 0;)
        if (scratch[i]) {
          any = true;
          out.dataEnd = uint64_t(s) * r.segBytes + off + i + 1;
          break;
        }
    }
    if (any) ++out.segmentsWithData;
  }
  if (!out.dataEnd) {
    out.why = "the dump is all zeros: the device wrote nothing into the segments";
    return;
  }
  // The header: the version and the size, in either order (a size is 8 bytes plus whole 64-byte entries and at most
  // kRddmHeaderLimit; the version-first order wins a tie).
  const uint32_t head = rddmRead(r, 0, scratch, kRddmScratchBytes < kRddmHeaderLimit ? kRddmScratchBytes : kRddmHeaderLimit, copy);
  out.word0 = head >= 8 ? get32(scratch) : 0;
  out.word1 = head >= 8 ? get32(scratch + 4) : 0;
  auto sizeOk = [&](uint32_t h) {
    return h >= kRddmHeaderWords + kRddmEntryBytes && h <= kRddmHeaderLimit && h <= head &&
           (h - kRddmHeaderWords) % kRddmEntryBytes == 0;
  };
  if (sizeOk(out.word1)) {
    out.version = out.word0;
    out.headerBytes = out.word1;
  } else if (sizeOk(out.word0)) {
    out.version = out.word1;
    out.headerBytes = out.word0;
    out.sizeFirst = true;
  }
  if (out.headerBytes) {
    out.entries = (out.headerBytes - kRddmHeaderWords) / kRddmEntryBytes;
    uint64_t offset = out.headerBytes;
    unsigned named = 0;
    for (uint32_t i = 0; i < out.entries && out.files < kRddmMaxFiles; ++i) {
      const uint8_t *e = scratch + kRddmHeaderWords + i * kRddmEntryBytes;
      RddmFile &f = out.file[out.files++];
      f.base = get64(e);
      f.phys = get64(e + 8);
      f.size = get64(e + 16);
      f.offset = offset;
      f.inDump = f.size <= total && offset <= total - f.size;
      field(e + 24, kRddmNameBytes, f.description);
      if (field(e + 44, kRddmNameBytes, f.name)) ++named;
      rddmMask(f.description, serial);
      rddmMask(f.name, serial);
      offset = f.size > total ? total + 1 : offset + f.size;   // an impossible size ends the walk's sense, not the table
    }
    if (named) {
      out.header = true;
    } else {
      out.why = "the header's entries have no file names";
      out.files = 0;
    }
  } else {
    out.why = "the first words are not a header's version and size";
  }
  // The failure reason: the file whose name says SFR, read from where the walk puts it.
  if (out.header)
    for (uint32_t i = 0; i < out.files; ++i)
      if (sfrName(out.file[i].name) && out.file[i].offset < total) {
        out.reasonFile = int(i);
        const uint32_t want = out.file[i].size < kRddmReasonBytes ? uint32_t(out.file[i].size) : kRddmReasonBytes;
        const uint32_t got = rddmRead(r, out.file[i].offset, scratch, want, copy);
        text(scratch, got, out.reason, sizeof out.reason);
        rddmMask(out.reason, serial);
        break;
      }
  if (out.reason[0]) return;
  // No reason: the first segment's strings that read like an error, the strong ones first.
  RddmString weak[kRddmMaxStrings];
  unsigned weakCount = 0;
  char run[kRddmStringBytes + 1];
  uint32_t runLength = 0, runKept = 0;
  uint64_t runAt = 0;
  auto endRun = [&]() {
    if (runLength >= kRddmMinString) {
      run[runKept] = 0;
      const unsigned score = errorScore(run);
      if (score) ++out.candidates;
      RddmString *slot = nullptr;
      if (score == 2 && out.strings < kRddmMaxStrings) slot = &out.string[out.strings++];
      else if (score == 1 && weakCount < kRddmMaxStrings) slot = &weak[weakCount++];
      if (slot) {
        slot->at = runAt;
        text(reinterpret_cast<const uint8_t *>(run), runKept, slot->text, sizeof slot->text);
        rddmMask(slot->text, serial);
      }
    }
    runLength = runKept = 0;
  };
  const uint32_t from = out.header ? out.headerBytes : 0;
  for (uint32_t off = from; off < r.segBytes; off += kRddmScratchBytes) {
    const uint32_t n = r.segBytes - off < kRddmScratchBytes ? r.segBytes - off : kRddmScratchBytes;
    copy(scratch, r.seg[0] + off, n);
    for (uint32_t i = 0; i < n; ++i) {
      if (printable(scratch[i])) {
        if (!runLength) runAt = off + i;
        if (runKept < kRddmStringBytes) run[runKept++] = char(scratch[i]);
        ++runLength;
      } else {
        endRun();
      }
    }
  }
  endRun();
  for (unsigned i = 0; i < weakCount && out.strings < kRddmMaxStrings; ++i) out.string[out.strings++] = weak[i];
}

}  // namespace pianowifi
