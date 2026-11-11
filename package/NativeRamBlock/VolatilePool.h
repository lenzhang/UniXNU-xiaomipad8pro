// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// UniXNU-piano: the memory of the volatile Data disk (VolatileDisk.h), so that its I/O path never allocates and never waits without a
// bound.
//
// Why (2026-11-11, three attended runs with data-persist=0 hard-reset 1-2 minutes after the login, under Spotlight's first import): the
// first VolatileDisk took every page with IOMallocZero(4096) inside doAsyncReadWrite, with the disk's lock held. That is a kalloc
// zone allocation (KHEAP_DEFAULT): the zone maps are bounded (this kernel: 1843 MiB per general map, 2457 MiB data), and a zalloc that
// waits for free pages waits for the pageout daemon, whose writeback of dirty Data pages needs this disk's lock. Nothing ever failed
// a write; the system stopped.
//
// Now:
//   - The pages come from 2 MiB chunks (IOMallocAligned of at least a page is kernel_memory_allocate in the kernel map's data range,
//     not a zone), allocated by a refill thread that holds no lock while it allocates and may wait for memory as long as it needs.
//   - The I/O path only takes pages that are already there (take(), O(1)). A write first asks admit() for every page it may need:
//     it is refused at once (kIOReturnNoSpace) past the budget, and waits at most until a deadline for the refill thread (with the lock
//     released while it sleeps), then is refused (kIOReturnNoMemory). Reads never need pages.
//   - The budget bounds what the disk holds (pages and leaves) whatever the system does; the chunk array is sized for it once.
//   - Pages that unmap gives back go on a free list and are taken first.
// No locking here: the caller's lock (NativeRamBlock's gate) protects the pool; admit() and refill() release it only through Env.
#include <stdint.h>
#include <string.h>

struct VolatilePool {
  static constexpr uint32_t kChunkPages = 512;                    // 2 MiB chunks
  static constexpr uint64_t kChunkBytes = kChunkPages * 4096ULL;
  uint64_t budgetPages = 0;          // the most pages handed out at once (data pages and leaves)
  uint32_t reserveChunks = 8;        // chunks kept ready ahead of the writers (16 MiB)
  uint8_t **chunks = nullptr;        // chunkCapacity entries, owned by the caller: every chunk taken from the system
  uint64_t chunkCapacity = 0, chunkCount = 0;
  uint64_t nextChunk = 0;            // chunks [nextChunk, chunkCount) are untouched spares
  uint32_t carved = kChunkPages;     // pages handed out of chunk nextChunk - 1 (kChunkPages: none left there)
  uint8_t *freeList = nullptr;       // pages given back (unmap); the first 8 bytes of each link the next
  uint64_t freePages = 0;
  uint64_t inUse = 0, peak = 0;      // pages handed out and not given back
  uint64_t wanted = 0;               // the largest need a writer waits for (the refill thread's target beyond the reserve)
  uint64_t refusedNoSpace = 0, refusedNoMemory = 0, waits = 0, chunkFailures = 0, unmapped = 0;

  // chunkCapacity for a budget: every chunk the budget can need (pages given back are reused before a new chunk is taken).
  static uint64_t capacityFor(uint64_t budget) { return (budget + kChunkPages - 1) / kChunkPages; }
  uint64_t spareChunks() const { return chunkCount - nextChunk; }
  uint64_t available() const { return freePages + (kChunkPages - carved) + spareChunks() * kChunkPages; }
  uint64_t heldBytes() const { return chunkCount * kChunkBytes; }
  bool wantsRefill() const { return chunkCount < chunkCapacity && (spareChunks() < reserveChunks || available() < wanted); }
  bool addChunk(uint8_t *chunk) {
    if (!chunk || chunkCount >= chunkCapacity) return false;
    chunks[chunkCount++] = chunk;
    return true;
  }
  // A zeroed page, or null when none is ready (the caller has admitted its need, so null means a bug, not a wait).
  uint8_t *take() {
    uint8_t *page = nullptr;
    if (freeList) {
      page = freeList;
      memcpy(&freeList, page, sizeof freeList);
      --freePages;
    } else if (carved < kChunkPages) {
      page = chunks[nextChunk - 1] + (uint64_t) carved++ * 4096;
    } else if (nextChunk < chunkCount) {
      carved = 0;
      ++nextChunk;
      page = chunks[nextChunk - 1] + (uint64_t) carved++ * 4096;
    }
    if (!page) return nullptr;
    memset(page, 0, 4096);
    if (++inUse > peak) peak = inUse;
    return page;
  }
  void give(uint8_t *page) {
    memcpy(page, &freeList, sizeof freeList);
    freeList = page;
    ++freePages;
    --inUse;
  }
  // Every chunk back to the system (the disk's pages with them). Free: void operator()(uint8_t *chunk) for kChunkBytes.
  template <class Free> void release(Free &free) {
    for (uint64_t k = 0; k < chunkCount; ++k) free(chunks[k]);
    chunkCount = nextChunk = 0; carved = kChunkPages; freeList = nullptr; freePages = inUse = 0;
  }
};

enum class VolatileAdmit { Ok, NoSpace, NoMemory };

// A write that may take up to env.need() new pages (evaluated again after every sleep: the lock was released). Env (called with the
// caller's lock held):
//   uint64_t need();                the pages the write may take now (VolatileDisk.h volatileNeed)
//   void wakeRefill();              wake the refill thread
//   bool sleep();                   sleep, the lock released, until woken or the request's deadline; false once the deadline is past
// Bounded: every pass either returns or sleeps toward the one deadline.
template <class Env> VolatileAdmit volatileAdmit(VolatilePool &p, Env &env)
{
  uint64_t need = env.need();
  if (!need || p.available() >= need) return VolatileAdmit::Ok;
  ++p.waits;
  for (bool more = true;;) {
    // inUse + need <= budget always fits in chunkCapacity chunks: what is owned is inUse + available.
    if (p.inUse + need > p.budgetPages) { ++p.refusedNoSpace; return VolatileAdmit::NoSpace; }
    if (!more) { ++p.refusedNoMemory; return VolatileAdmit::NoMemory; }
    if (need > p.wanted) p.wanted = need;
    env.wakeRefill();
    more = env.sleep();
    if (p.wanted == need) p.wanted = 0;
    need = env.need();
    if (!need || p.available() >= need) return VolatileAdmit::Ok;
  }
}

// The refill thread's loop, entered and left with the caller's lock held. Env:
//   bool stopping();                the disk is going away
//   void idle();                    sleep, the lock released, until woken or the status interval passes
//   void unlock(); void lock();
//   uint8_t *allocChunk();          kChunkBytes, may wait for memory (called without the lock), null on failure
//   void freeChunk(uint8_t *);      (without the lock)
//   void wakeWriters();
//   void backoff();                 after a failed allocation: sleep a little, the lock released
template <class Env> void volatileRefill(VolatilePool &p, Env &env)
{
  while (!env.stopping()) {
    if (!p.wantsRefill()) { env.idle(); continue; }
    env.unlock();
    uint8_t *chunk = env.allocChunk();
    env.lock();
    if (!chunk) { ++p.chunkFailures; env.wakeWriters(); env.backoff(); continue; }
    if (env.stopping() || !p.addChunk(chunk)) { env.unlock(); env.freeChunk(chunk); env.lock(); continue; }
    env.wakeWriters();
  }
}
