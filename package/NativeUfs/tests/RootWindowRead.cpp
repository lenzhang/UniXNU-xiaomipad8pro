// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano (2026-11-14): the production window reader (RootWindowRead.h) with the bounce sizes native-ufs-bounce gives it. A
// window of 512-byte sectors on 4096-byte device blocks: every bounce size must hand up the same bytes, verify the same blocks in the
// same order, never read more than one bounce per call, and report the same partial progress when a device read fails.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = 0xe00002c2, kIOReturnIOError = 0xe00002ca;
#include "../RootTransportLayout.h"
#include "../RootWindowRead.h"
constexpr uint32_t kXferMax = 1U << 20;   // RootCommands.h: one command's most, the biggest bounce native-ufs-bounce gives

namespace {
uint8_t deviceByte (uint64_t offset) { return uint8_t ((offset * 2654435761ULL) >> 13); }

struct Ops {
  std::vector<uint8_t> *out;
  std::vector<uint64_t> verified;
  uint32_t largestRead = 0, reads = 0, failAt = 0;   // failAt: the read (1-based) that fails, 0 none
  int locks = 0;
  void lock () { ++locks; }
  void unlock () { --locks; }
  IOReturn read (uint64_t lba, uint32_t blocks, void *bounce) {
    ++reads;
    if (failAt && reads == failAt) return kIOReturnIOError;
    if (blocks * 4096U > largestRead) largestRead = blocks * 4096U;
    for (uint64_t i = 0; i < uint64_t (blocks) * 4096; ++i) static_cast<uint8_t *> (bounce)[i] = deviceByte (lba * 4096 + i);
    return kIOReturnSuccess;
  }
  IOReturn verify (uint64_t index, const void *, size_t) { verified.push_back (index); return kIOReturnSuccess; }
  uint64_t copy (uint64_t offset, const void *data, uint64_t length) {
    assert (offset + length <= out->size ());
    memcpy (out->data () + offset, data, length);
    return length;
  }
};

struct Run { UfsWindowResult result; Ops ops; std::vector<uint8_t> out; };

Run readWith (uint64_t absolute, uint64_t bytes, uint64_t block, uint64_t count, uint32_t chunk, uint32_t failAt = 0)
{
  Run run;
  run.out.assign (count * 512, 0xee);
  run.ops.out = &run.out;
  run.ops.failAt = failAt;
  std::vector<uint8_t> bounce (chunk > kDataMax ? chunk : kDataMax);
  run.result = ufsReadWindow (run.ops, absolute, bytes, block, count, bounce.data (), chunk);
  run.ops.out = nullptr;
  return run;
}
}  // namespace

int main ()
{
  const uint64_t absolute = 0x12345000ULL, bytes = 64ULL << 20;
  const uint32_t chunks[] = { 4096, kDataMax, 512U << 10, kXferMax };
  uint64_t seed = 1;
  auto next = [&seed] { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return seed >> 33; };
  unsigned cases = 0;
  for (unsigned trial = 0; trial < 300; ++trial) {
    const uint64_t count = 1 + next () % (trial % 3 == 0 ? 8192 : 600);   // up to 4 MiB
    const uint64_t block = next () % (bytes / 512 - count);
    Run reference = readWith (absolute, bytes, block, count, kDataMax);
    assert (reference.result.status == kIOReturnSuccess && reference.result.done == count * 512 && reference.ops.locks == 0);
    for (uint64_t i = 0; i < count * 512; ++i) assert (reference.out[i] == deviceByte (absolute + block * 512 + i));
    for (uint32_t chunk : chunks) {
      Run run = readWith (absolute, bytes, block, count, chunk);
      assert (run.result.status == kIOReturnSuccess && run.result.done == count * 512 && run.ops.locks == 0);
      assert (run.out == reference.out && run.ops.verified == reference.ops.verified);
      assert (run.ops.largestRead <= chunk);
      // One read per bounce: the device blocks the request touches, in pieces of the bounce.
      const uint64_t first = (absolute + block * 512) / 4096, last = (absolute + (block + count) * 512 - 1) / 4096;
      assert (run.ops.reads == (last - first + 1 + chunk / 4096 - 1) / (chunk / 4096));
      // A failed device read: nothing after it is handed up, and what was is reported.
      if (run.ops.reads > 1) {
        Run failed = readWith (absolute, bytes, block, count, chunk, 2);
        assert (failed.result.status == kIOReturnIOError && failed.result.done > 0 && failed.result.done < count * 512);
        assert (memcmp (failed.out.data (), reference.out.data (), failed.result.done) == 0);
        for (uint64_t i = failed.result.done; i < count * 512; ++i) assert (failed.out[i] == 0xee);
      }
      ++cases;
    }
  }
  // Refused: a bounce size that is not a multiple of 4096 or smaller than one block; the window's own bounds as before.
  std::vector<uint8_t> bounce (kXferMax);
  Ops ops;
  std::vector<uint8_t> out (4096);
  ops.out = &out;
  assert (ufsReadWindow (ops, absolute, bytes, 0, 8, bounce.data (), 0).status == kIOReturnBadArgument);
  assert (ufsReadWindow (ops, absolute, bytes, 0, 8, bounce.data (), 2048).status == kIOReturnBadArgument);
  assert (ufsReadWindow (ops, absolute, bytes, 0, 8, bounce.data (), kDataMax + 512).status == kIOReturnBadArgument);
  assert (ufsReadWindow (ops, absolute, bytes, bytes / 512, 1, bounce.data ()).status == kIOReturnBadArgument);
  assert (ufsReadWindow (ops, absolute, bytes, 0, 8, nullptr).status == kIOReturnBadArgument);
  assert (ops.reads == 0);
  printf ("PASS: window reads with bounces of 4 KiB, 128 KiB, 512 KiB and 1 MiB (%u cases): the same bytes and verified blocks as 128 KiB, "
          "one device read per bounce, partial progress on a failed read; bad bounce sizes refused\n", cases);
}
