// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano (2026-11-14): native-ufs-direct's command path against the model (UfsModel.h): the PRDT built from a client buffer's
// physical segments (hostPrdsFromSegments: what qualifies, merging, the 256 KiB entry limit, the 64-entry table, the stream's reach)
// and the READ(10) that DMAs into them (hostReadPrd: the table in the command descriptor, no cache maintenance of the data by the
// command path, busy retries, bad arguments refused before anything reaches the controller).
#include "UfsModel.h"

namespace {
struct Segment { uint64_t at, address, length; };
struct Segments {
  std::vector<Segment> list;
  uint64_t operator() (uint64_t at, uint64_t *length) {
    for (const Segment &s : list)
      if (at >= s.at && at < s.at + s.length) { *length = s.at + s.length - at; return s.address ? s.address + (at - s.at) : 0; }
    *length = 0;
    return 0;
  }
};
// `count` segments of `each` bytes, at addresses `stride` apart from `base` (stride == each: one contiguous run).
Segments run (unsigned count, uint64_t each, uint64_t base, uint64_t stride)
{
  Segments s;
  for (unsigned i = 0; i < count; ++i) s.list.push_back ({ i * each, base + i * stride, each });
  return s;
}
constexpr uint64_t kLimit = 1ULL << 36;
}  // namespace

int main ()
{
  UfsPrd prd[kMaxPrds];
  // One contiguous MiB: four entries of 256 KiB.
  Segments contiguous = run (64, 16384, 0x880000000ULL, 16384);
  assert (hostPrdsFromSegments (contiguous, 0, kXferMax, prd, kLimit) == 4);
  for (unsigned i = 0; i < 4; ++i) assert (prd[i].addr == 0x880000000ULL + i * kPrdMax && prd[i].bytes == kPrdMax);
  // 64 scattered 16 KiB pages: 64 entries; at an offset, the pieces it covers.
  Segments scattered = run (128, 16384, 0x880000000ULL, 65536);
  assert (hostPrdsFromSegments (scattered, 0, kXferMax, prd, kLimit) == 64);
  for (unsigned i = 0; i < 64; ++i) assert (prd[i].addr == 0x880000000ULL + i * 65536ULL && prd[i].bytes == 16384);
  assert (hostPrdsFromSegments (scattered, 4096, 8192, prd, kLimit) == 1 && prd[0].addr == 0x880001000ULL && prd[0].bytes == 8192);
  assert (hostPrdsFromSegments (scattered, 12288, 8192, prd, kLimit) == 2 && prd[1].addr == 0x880010000ULL && prd[1].bytes == 4096);
  // 65 scattered pages do not fit one table; a request beyond one command or not whole pages never qualifies.
  Segments pages = run (300, 4096, 0x880000000ULL, 8192);
  assert (hostPrdsFromSegments (pages, 0, 64 * 4096, prd, kLimit) == 64);
  assert (hostPrdsFromSegments (pages, 0, 65 * 4096, prd, kLimit) == 0);
  assert (hostPrdsFromSegments (contiguous, 0, kXferMax + 4096, prd, kLimit) == 0);
  assert (hostPrdsFromSegments (contiguous, 0, 6144, prd, kLimit) == 0);
  assert (hostPrdsFromSegments (contiguous, 0, 0, prd, kLimit) == 0);
  // A segment off a page, of part of a page, without an address, or beyond the stream's reach: refused.
  Segments odd = run (4, 16384, 0x880000200ULL, 16384);
  assert (hostPrdsFromSegments (odd, 0, 16384, prd, kLimit) == 0);
  Segments partial;
  partial.list = { { 0, 0x880000000ULL, 6144 }, { 6144, 0x880010000ULL, 10240 } };
  assert (hostPrdsFromSegments (partial, 0, 16384, prd, kLimit) == 0);
  assert (hostPrdsFromSegments (partial, 0, 4096, prd, kLimit) == 1);   // its first page alone is fine
  Segments hole;
  hole.list = { { 0, 0x880000000ULL, 4096 }, { 4096, 0, 4096 } };
  assert (hostPrdsFromSegments (hole, 0, 8192, prd, kLimit) == 0);
  assert (hostPrdsFromSegments (contiguous, 0, 65536, prd, 0x880008000ULL) == 0);
  assert (hostPrdsFromSegments (contiguous, 0, 32768, prd, 0x880008000ULL) == 1);
  // Adjacent segments merge up to the entry limit, then a new entry starts.
  Segments adjacent = run (20, 65536, 0x900000000ULL, 65536);
  assert (hostPrdsFromSegments (adjacent, 0, 20 * 65536ULL > kXferMax ? kXferMax : 20 * 65536ULL, prd, kLimit) == 4);
  puts ("PASS: direct PRDT: contiguous and scattered pages, offsets, the 64-entry table and 256 KiB entries, whole pages only, the "
        "stream's reach, merging");

  // hostReadPrd: one READ(10) with the table in the command descriptor; the command path cleans only its own entry and descriptor.
  reset ();
  UfsPrd two[2] = { { 0x880000000ULL, 16384 }, { 0x880100000ULL, 4096 } };
  const unsigned before = m.releases;
  assert (hostReadPrd (0, 9, 5, two, 2) == 0 && !gHost.poisoned);
  assert (m.releases - before == 2);   // the request list entry and the command descriptor, not the data
  const uint8_t *ucd = memory + ufsSlotUcdOffset (m.lastSlot), *utrd = memory + ufsSlotUtrdOffset (m.lastSlot);
  const uint32_t *table = reinterpret_cast<const uint32_t *> (ucd + kPrdtOffset);
  assert (table[0] == 0x80000000U && table[1] == 8 && table[3] == 16383 && table[4] == 0x80100000U && table[5] == 8 && table[7] == 4095);
  assert ((reinterpret_cast<const uint32_t *> (utrd)[7] & 0xffff) == 2);
  assert (ucd[16] == 0x28 && ucd[21] == 9 && ucd[24] == 5 && loadBe32 (ucd + 12) == 5 * 4096);
  assert (gSlotFree == (1U << (kRegionSlots - 1)) - 1);
  // The same read through the slot's data area cleans the data too (the control).
  const unsigned before2 = m.releases;
  std::array<uint8_t, 4096> out;
  assert (hostRead4k (0, 9, 1, out.data ()) == 0 && m.releases - before2 == 3);
  // Refused before the controller: a table that does not add up, none, too many blocks, past the last LBA.
  const unsigned writes = m.writes;
  assert (hostReadPrd (0, 9, 4, two, 2) == kIOReturnBadArgument);
  assert (hostReadPrd (0, 9, 5, two, 0) == kIOReturnBadArgument);
  assert (hostReadPrd (0, 9, 5, nullptr, 2) == kIOReturnBadArgument);
  assert (hostReadPrd (0, 9, 257, two, 2) == kIOReturnBadArgument);
  assert (hostReadPrd (0, 0xffffffffULL, 5, two, 2) == kIOReturnBadArgument);
  UfsPrd big[1] = { { 0x880000000ULL, kPrdMax + 4096 } };
  assert (hostReadPrd (0, 9, (kPrdMax + 4096) / 4096, big, 1) == kIOReturnBadArgument);   // an entry over 256 KiB
  assert (m.writes == writes && !gHost.poisoned);
  // Busy and task-set-full answers are sent again, as for hostRead4k; a timeout quarantines the transport.
  reset (Fault::Busy); m.busyLeft = 3; assert (hostReadPrd (0, 9, 5, two, 2) == 0 && !gHost.poisoned);
  reset (Fault::Timeout); assert (hostReadPrd (0, 9, 5, two, 2) != 0); assertQuarantined ();
  // Review H1 (2026-11-14): *live only when the read's own command was rung and did not complete; a refusal before the doorbell or a
  // completed command with a bad response or a SCSI error is an ordinary failure (the pages are the caller's again).
  bool live = true;
  reset (Fault::None); assert (hostReadPrd (0, 9, 5, two, 2, &live) == 0 && !live);
  reset (Fault::Timeout); live = false; assert (hostReadPrd (0, 9, 5, two, 2, &live) != 0 && live); assertQuarantined ();
  reset (Fault::Type); live = true; assert (hostReadPrd (0, 9, 5, two, 2, &live) != 0 && !live && gHost.poisoned);
  reset (Fault::CheckCondition); live = true; assert (hostReadPrd (0, 9, 5, two, 2, &live) != 0 && !live);
  reset (Fault::None); gHost.poisoned = true; live = true;
  assert (hostReadPrd (0, 9, 5, two, 2, &live) == kIOReturnNotReady && !live);   // another command's quarantine: never rung
  puts ("PASS: direct READ(10): the table in the descriptor, no data maintenance in the command path, bad tables refused before the "
        "controller, busy retries, quarantine on timeout; live only for a rung command that did not complete (review H1)");
}
