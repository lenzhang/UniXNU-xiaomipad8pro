// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// UniXNU-piano: the volatile Data disk over a large seed (SeedDisk.h), for every boot whose store is not open read-write
// (data-persist=0 or 1, a store kept because it belongs to another seed, a refused or failed store). The pages written during the boot
// live in RAM only and are gone at the next boot; the seed stays immutable, and the store is never touched. Before this, a seed above
// 2 GiB had no write path at all without the store (writes failed with kIOReturnNotReady).
// Memory follows the pages written: a directory of one pointer per 512 pages (8 bytes per 2 MiB of disk, allocated once), leaves of 512
// page pointers and the pages themselves, both 4096 bytes and taken on first write. In the kernel they come from VolatilePool.h's
// ready pages: allocate() never blocks, and the caller admits a write's need (volatileNeed) before it calls volatileDiskIO.
//   Seed:  const uint8_t *find(uint64_t page) const          (SeedDisk, RamSparseDisk)
//   Ops:   uint8_t *allocate()                               a zeroed 4096-byte block, or null (never waits)
//   Buf:   uint64_t read(at, p, n) / copy(at, p, n)          as DirectDisk.h
// Caller serializes I/O.
#include <stdint.h>
#include <string.h>

struct VolatileDisk {
  uint64_t bytes = 0, resident = 0, leaves = 0;
  uint8_t ***dir = nullptr;                  // entries(bytes) pointers, owned by the caller
  static constexpr uint64_t kLeafPages = 512;
  static uint64_t entries(uint64_t bytes) { return (bytes / 4096 + kLeafPages - 1) / kLeafPages; }
  uint8_t *find(uint64_t i) const { uint8_t **leaf = dir[i / kLeafPages]; return leaf ? leaf[i % kLeafPages] : nullptr; }
};

// Returns the bytes completed. noMemory: a page could not be allocated (the request stops there; what was done is reported).
template<class Seed, class Ops, class Buf>
static uint64_t volatileDiskIO(const Seed &seed, VolatileDisk &d, bool write, uint64_t offset, uint64_t length, Buf &buf, Ops &ops,
                               bool &noMemory)
{
  static const uint8_t zero[4096] = {};
  noMemory = false;
  if (!d.dir || !length || offset > d.bytes || length > d.bytes - offset) return 0;
  uint64_t done = 0;
  while (done < length) {
    uint64_t at = offset + done, index = at / 4096, skip = at & 4095, n = 4096 - skip;
    if (n > length - done) n = length - done;
    uint8_t *page = d.find(index);
    if (!write) {
      const uint8_t *src = page ? page : seed.find(index);
      uint64_t got = buf.copy(done, (src ? src : zero) + skip, n);
      done += got;
      if (got != n) break;
      continue;
    }
    if (!page) {
      uint8_t **&leaf = d.dir[index / VolatileDisk::kLeafPages];
      if (!leaf) {
        leaf = reinterpret_cast<uint8_t **>(ops.allocate());
        if (!leaf) { noMemory = true; break; }
        ++d.leaves;
      }
      page = ops.allocate();
      if (!page) { noMemory = true; break; }
      // The seed's content first, so a short or partial write leaves the page as it read before.
      if (const uint8_t *src = seed.find(index)) memcpy(page, src, 4096);
      leaf[index % VolatileDisk::kLeafPages] = page;
      d.resident += 4096;
    }
    uint64_t got = buf.read(done, page + skip, n);
    if (got > n) break;
    done += got;
    if (got != n) break;
  }
  return done;
}

// The pages (data pages and leaves) a write of [offset, offset + length) may take: what volatileDiskIO allocates for it at most.
// The caller admits that many from its pool first (VolatilePool.h), so the I/O itself never waits for memory.
static uint64_t volatileNeed(const VolatileDisk &d, uint64_t offset, uint64_t length)
{
  if (!d.dir || !length || offset > d.bytes || length > d.bytes - offset) return 0;
  uint64_t need = 0, lastLeaf = ~0ULL;
  for (uint64_t i = offset / 4096, last = (offset + length - 1) / 4096; i <= last; ++i) {
    uint64_t e = i / VolatileDisk::kLeafPages;
    if (!d.dir[e]) { if (e != lastLeaf) { ++need; lastLeaf = e; } ++need; continue; }
    if (!d.dir[e][i % VolatileDisk::kLeafPages]) ++need;
  }
  return need;
}

// Unmap (TRIM) of [offset, offset + length): every page it covers whole goes back through give(page) and reads as the seed's page
// again (an unmapped block's content is undefined; APFS writes before it reads). Parts of pages are kept. Leaves stay. Returns the
// pages given back.
template<class Give> static uint64_t volatileUnmap(VolatileDisk &d, uint64_t offset, uint64_t length, Give &give)
{
  if (!d.dir || !length || offset > d.bytes || length > d.bytes - offset) return 0;
  uint64_t first = (offset + 4095) / 4096, end = (offset + length) / 4096, freed = 0;   // whole pages only
  for (uint64_t i = first; i < end; ++i) {
    uint8_t **leaf = d.dir[i / VolatileDisk::kLeafPages];
    if (!leaf) { i = (i / VolatileDisk::kLeafPages + 1) * VolatileDisk::kLeafPages - 1; continue; }   // a whole leaf never written
    uint8_t *&page = leaf[i % VolatileDisk::kLeafPages];
    if (!page) continue;
    give(page);
    page = nullptr;
    d.resident -= 4096;
    ++freed;
  }
  return freed;
}

// Frees every leaf and page (not the directory). Free: void free(void *block) for one 4096-byte block.
template<class Free> static void volatileDiskRelease(VolatileDisk &d, Free &release)
{
  if (!d.dir) return;
  for (uint64_t e = 0; e < VolatileDisk::entries(d.bytes); ++e) {
    uint8_t **leaf = d.dir[e];
    if (!leaf) continue;
    for (uint64_t k = 0; k < VolatileDisk::kLeafPages; ++k) if (leaf[k]) release(leaf[k]);
    release(reinterpret_cast<uint8_t *>(leaf));
    d.dir[e] = nullptr;
  }
  d.resident = 0;
  d.leaves = 0;
}
