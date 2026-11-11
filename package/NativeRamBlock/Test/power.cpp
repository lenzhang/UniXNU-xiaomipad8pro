// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano host test of PowerHold.h (the Data disk's part of a system sleep, native-ufs-suspend=1) on a store model with the
// device's volatile write cache: a write is durable at once with FUA, else only after a sync; a power cut (the tablet never waking
// from the sleep: battery, watchdog) drops what the cache held.
//   - the store as NativeRamBlock writes it (FUA, DirectDisk.h): a cut at any point loses no acknowledged write; off() syncs, holds
//   - writes without FUA (native_ufs_store_write_cached): a cut without the sync loses them, after off() none
//   - a sync that fails: off() vetoes once with the reason, does not hold; a later sleep tries again
//   - nothing writable: held without a sync; a second off() changes nothing; on() releases once
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../DirectDisk.h"
#include "../PowerHold.h"
using namespace persist;

static unsigned failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct CachedMedium {
  std::vector<uint8_t> durable, cache;            // the medium, and what the device answers reads with (its cache over it)
  std::vector<bool> dirty;                        // blocks in the cache not yet on the medium
  bool fua = true, failSync = false;
  unsigned syncs = 0, vetoes = 0;
  const char *vetoWho = nullptr;
  explicit CachedMedium(uint64_t n) : durable(n * kBlock, 0xA5), cache(n * kBlock, 0xA5), dirty(n, false) {}
  bool read(uint64_t b, uint32_t n, void *d) { memcpy(d, &cache[b * kBlock], (size_t) n * kBlock); return true; }
  bool write(uint64_t b, uint32_t n, const void *s) {
    memcpy(&cache[b * kBlock], s, (size_t) n * kBlock);
    for (uint32_t k = 0; k < n; ++k) {
      if (fua) memcpy(&durable[(b + k) * kBlock], &cache[(b + k) * kBlock], kBlock);
      dirty[b + k] = !fua;
    }
    return true;
  }
  bool sync() {
    ++syncs;
    if (failSync) return false;
    for (size_t b = 0; b < dirty.size(); ++b)
      if (dirty[b]) { memcpy(&durable[b * kBlock], &cache[b * kBlock], kBlock); dirty[b] = false; }
    return true;
  }
  void veto(const char *who) { ++vetoes; vetoWho = who; }
  void cut() { cache = durable; dirty.assign(dirty.size(), false); }   // the power goes: the cache is lost
};
struct Buffer {
  std::vector<uint8_t> &data;
  uint64_t read(uint64_t at, void *p, uint64_t n) { memcpy(p, &data[at], n); return n; }
  uint64_t copy(uint64_t at, const void *p, uint64_t n) { memcpy(&data[at], p, n); return n; }
};

static const uint64_t kPages = 96;
static uint8_t gDigest[32] = { 7, 7, 7 };

// The disk a reboot would find: the store reopened (read only) over an empty seed.
static std::vector<uint8_t> afterReboot(CachedMedium &medium)
{
  RamSparseDisk seed = {};
  std::vector<uint8_t *> pages(kPages, nullptr);
  seed.bytes = kPages * 4096;
  seed.pages = pages.data();
  std::vector<uint8_t> bits(bitmapBlocks(kPages) * kBlock), stage(8 * kBlock), disk(seed.bytes);
  Overlay ov;
  if (ov.open(medium, kPages, gDigest, true, bits.data()) != Open::Resumed) return {};
  ov.readOnly = false;   // reads only
  Buffer b{disk};
  bool err = false;
  if (directDiskIO(seed, ov, medium, stage.data(), 8, false, 0, seed.bytes, b, &err) != seed.bytes || err) return {};
  return disk;
}

// Writes `count` random requests through DirectDisk.h onto the store; the oracle is the disk the system was told it has.
static void writeSome(CachedMedium &medium, Overlay &ov, std::vector<uint8_t> &oracle, unsigned count, unsigned salt)
{
  RamSparseDisk seed = {};
  std::vector<uint8_t *> pages(kPages, nullptr);
  seed.bytes = kPages * 4096;
  seed.pages = pages.data();
  std::vector<uint8_t> stage(8 * kBlock), io(64 * 1024);
  srand(salt);
  for (unsigned k = 0; k < count; ++k) {
    const uint64_t length = 1 + (uint64_t) rand() % 20000, offset = (uint64_t) rand() % (seed.bytes - length);
    for (uint64_t i = 0; i < length; ++i) io[i] = (uint8_t) rand();
    Buffer b{io};
    bool err = false;
    CHECK(directDiskIO(seed, ov, medium, stage.data(), 8, true, offset, length, b, &err) == length && !err);
    memcpy(&oracle[offset], io.data(), length);
  }
}

static void scenario(bool fua, bool sleep, bool expectIntact)
{
  CachedMedium medium(windowBlocks(kPages));
  medium.fua = fua;
  std::vector<uint8_t> bits(bitmapBlocks(kPages) * kBlock), oracle(kPages * 4096, 0);
  Overlay ov;
  CHECK(ov.open(medium, kPages, gDigest, false, bits.data()) == Open::Fresh);   // the header and bitmap: synced by open()
  writeSome(medium, ov, oracle, 40, fua ? 11 : 12);
  DataPowerHold power;
  if (sleep) {
    const unsigned before = medium.syncs;
    CHECK(power.off(medium, true) && power.held && medium.syncs == before + 1 && !medium.vetoes);
  }
  medium.cut();
  const std::vector<uint8_t> found = afterReboot(medium);
  CHECK(!found.empty() || !expectIntact);
  CHECK((found == oracle) == expectIntact);
}

int main()
{
  // The store as NativeRamBlock writes it (FUA): nothing acknowledged is lost, with or without the sleep's sync.
  scenario(true, false, true);
  scenario(true, true, true);
  // Without FUA the cache keeps acknowledged writes: a cut loses them, unless off() synced first.
  scenario(false, false, false);
  scenario(false, true, true);
  // A sync that fails: one veto with the reason, nothing held; the next sleep tries again.
  {
    CachedMedium medium(4);
    DataPowerHold power;
    medium.failSync = true;
    CHECK(!power.off(medium, true) && !power.held && medium.vetoes == 1 && power.vetoes == 1);
    CHECK(medium.vetoWho && !strcmp(medium.vetoWho, "NativeRamBlock: the Data store's cache sync failed"));
    CHECK(!power.on());                                 // nothing was held: nobody to wake
    medium.failSync = false;
    CHECK(power.off(medium, true) && power.held && medium.syncs == 2 && medium.vetoes == 1);
    CHECK(power.on() && !power.held && !power.on());
  }
  // Nothing writable (Data volatile, or the store read only): held without a sync; a second off() is the same sleep; on() once.
  {
    CachedMedium medium(4);
    DataPowerHold power;
    CHECK(power.off(medium, false) && power.held && medium.syncs == 0 && !medium.vetoes);
    CHECK(power.off(medium, true) && medium.syncs == 0 && power.holds == 1);
    CHECK(power.on() && !power.on() && power.holds == 1);
  }
  if (failures) { printf("%u of %u checks failed\n", failures, checks); return 1; }
  printf("PASS: Data disk power hold (%u checks): FUA store intact across a cut with or without the sleep; cached writes kept only "
         "by the sync; a failed sync vetoes once and holds nothing; nothing writable holds without a sync; on() releases once\n", checks);
  return 0;
}
