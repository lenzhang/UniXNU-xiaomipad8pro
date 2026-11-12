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
// Why the reservation and the fallbacks (2026-11-12, the first desktop session, 26 minutes): at 1080 MiB held the refill thread's
// 2 MiB IOMallocAligned failed 87474 times in a row, 13 writes were refused (no memory), and APFS then read blocks of those writes
// back as zeros (cksum 0, oid 0: container corruption). IOMallocAligned takes kernel VA from kmem's shared data range (4536 MiB on
// this kernel), the range the GPU's kernel buffers (IOBufferMemoryDescriptor: its 1 GiB BO pool, 920 MiB of BOs outside it) live in,
// and an allocation short of pages waits (VM_PAGE_WAIT) instead of failing: the fast failures were that range, not RAM. And each
// failure woke the writers, whose wake cut the refill thread's backoff short: a busy loop, no status line.
//
// Now:
//   - The pages come from chunks taken by a refill thread that holds no lock while it allocates and may wait for memory as long as it
//     needs. At start it takes prefillPages (the boot reservation, data-volatile-reserve-mib) at once, while memory and the kernel's
//     address ranges are still unfragmented; past that it keeps reservePages ready ahead of the writers (deeper after every wait).
//   - A chunk is 2 MiB when it can be; when that fails it falls back to 512, 128, 32 KiB and single pages, and over the allocator
//     kinds the caller offers (the kernel: private data range, shared data range, the default heap): a failure there is fragmentation
//     or one exhausted range, not the end. Only when every size of every kind failed it backs off (10 ms doubling to 1 s), on an event
//     the writers do not signal.
//   - The I/O path only takes pages that are already there (take(), O(1)). A write first asks admit() for every page it may need:
//     it is refused at once past the budget, and waits at most until a deadline for the refill thread (with the lock released while
//     it sleeps), then is refused. Reads never need pages. (The caller turns a refusal into an I/O error.)
//   - The budget bounds what the disk holds (pages and leaves) whatever the system does.
//   - Pages that unmap gives back go on a free list and are taken first.
// No locking here: the caller's lock (NativeRamBlock's gate) protects the pool; admit() and refill() release it only through Env.
#include <stdint.h>
#include <string.h>

struct VolatileChunk {
  uint8_t *base;
  uint32_t pages;      // 1 .. kChunkPages
  uint32_t kind;       // the allocator it came from (the caller's numbering: freeChunk gives it back there)
};

struct VolatilePool {
  static constexpr uint32_t kChunkPages = 512;                    // 2 MiB chunks when they can be had
  static constexpr uint64_t kChunkBytes = kChunkPages * 4096ULL;
  static constexpr uint32_t kLevels = 5;                          // the fallback sizes: 2 MiB, 512 KiB, 128 KiB, 32 KiB, a page
  static uint32_t levelPages(uint32_t level) { static const uint32_t pages[kLevels] = {512, 128, 32, 8, 1}; return pages[level]; }
  uint64_t budgetPages = 0;          // the most pages handed out at once (data pages and leaves)
  uint64_t prefillPages = 0;         // taken at once when the disk starts (the boot reservation), at most the budget
  uint64_t reservePages = 16384;     // kept ready ahead of the writers (64 MiB) ...
  uint64_t reserveMaxPages = 65536;  // ... doubled after every wait, up to this (256 MiB)
  VolatileChunk *chunks = nullptr;   // chunkCapacity records, owned by the caller (the refill thread grows them): every chunk taken
  uint64_t chunkCapacity = 0, chunkCount = 0;
  uint64_t nextChunk = 0;            // chunk nextChunk - 1 is being carved (none while 0); [nextChunk, chunkCount) are untouched
  uint32_t carved = 0;               // pages handed out of chunk nextChunk - 1
  uint64_t ownedPages = 0;           // pages in every chunk taken
  uint64_t untouchedPages = 0;       // pages in chunks [nextChunk, chunkCount)
  uint8_t *freeList = nullptr;       // pages given back (unmap); the first 8 bytes of each link the next
  uint64_t freePages = 0;
  uint64_t inUse = 0, peak = 0;      // pages handed out and not given back
  uint64_t wanted = 0;               // the largest need a writer waits for (the refill thread's target beyond the reserve)
  uint32_t level = 0, levelWins = 0; // the refill thread's place on the fallback sizes, and chunks taken there since
  uint32_t backoffMs = 0;            // after a round where nothing could be had (0: none)
  uint64_t refusedNoSpace = 0, refusedNoMemory = 0, waits = 0, chunkFailures = 0, unmapped = 0;
  uint64_t fallbackChunks = 0;       // chunks below 2 MiB or not from the first kind
  uint64_t exhaustedRounds = 0;      // rounds in which every size of every kind failed
  uint64_t recordGrowths = 0;

  // The records a budget starts with: every chunk it needs at 2 MiB, and room for some fallbacks (the refill thread grows them).
  static uint64_t capacityFor(uint64_t budget) { return (budget + kChunkPages - 1) / kChunkPages + 64; }
  uint64_t currentLeft() const { return nextChunk ? chunks[nextChunk - 1].pages - carved : 0; }
  uint64_t available() const { return freePages + currentLeft() + untouchedPages; }
  uint64_t heldBytes() const { return ownedPages * 4096; }
  bool prefilled() const { return ownedPages >= prefillPages || ownedPages >= budgetPages; }
  bool wantsRefill() const {
    return ownedPages < budgetPages && (ownedPages < prefillPages || available() < reservePages || available() < wanted);
  }
  bool needsRecords() const { return chunkCount >= chunkCapacity; }
  // The next record capacity: double, and never more than a record per page of the budget (a chunk has at least one page).
  uint64_t recordsWanted() const {
    uint64_t cap = chunkCapacity * 2 + 64;
    return cap > budgetPages + 1 ? budgetPages + 1 : cap;
  }
  bool addChunk(uint8_t *base, uint32_t pages, uint32_t kind) {
    if (!base || !pages || pages > kChunkPages || chunkCount >= chunkCapacity || ownedPages + pages > budgetPages) return false;
    chunks[chunkCount++] = VolatileChunk{base, pages, kind};
    ownedPages += pages;
    untouchedPages += pages;
    if (pages < kChunkPages || kind) ++fallbackChunks;
    return true;
  }
  // A zeroed page, or null when none is ready (the caller has admitted its need, so null means a bug, not a wait).
  uint8_t *take() {
    uint8_t *page = nullptr;
    if (freeList) {
      page = freeList;
      memcpy(&freeList, page, sizeof freeList);
      --freePages;
    } else {
      if (!nextChunk || carved >= chunks[nextChunk - 1].pages) {
        if (nextChunk >= chunkCount) return nullptr;
        ++nextChunk;
        carved = 0;
        untouchedPages -= chunks[nextChunk - 1].pages;
      }
      page = chunks[nextChunk - 1].base + (uint64_t) carved++ * 4096;
    }
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
  // Every chunk back to the system (the disk's pages with them), not the records. Free: void operator()(uint8_t *, uint32_t pages,
  // uint32_t kind).
  template <class Free> void release(Free &free) {
    for (uint64_t k = 0; k < chunkCount; ++k) free(chunks[k].base, chunks[k].pages, chunks[k].kind);
    chunkCount = nextChunk = 0; carved = 0; ownedPages = untouchedPages = 0; freeList = nullptr; freePages = inUse = 0;
  }
};

enum class VolatileAdmit { Ok, NoSpace, NoMemory };

// A write that may take up to env.need() new pages (evaluated again after every sleep: the lock was released). Env (called with the
// caller's lock held):
//   uint64_t need();                the pages the write may take now (VolatileDisk.h volatileNeed)
//   void wakeRefill();              wake the refill thread
//   bool sleep();                   sleep, the lock released, until woken or the request's deadline; false once the deadline is past
// Bounded: every pass either returns or sleeps toward the one deadline. A wait deepens the reserve (a burst is coming).
template <class Env> VolatileAdmit volatileAdmit(VolatilePool &p, Env &env)
{
  uint64_t need = env.need();
  if (!need || p.available() >= need) return VolatileAdmit::Ok;
  ++p.waits;
  if (p.reservePages < p.reserveMaxPages) p.reservePages = p.reservePages * 2 > p.reserveMaxPages ? p.reserveMaxPages : p.reservePages * 2;
  for (bool more = true;;) {
    // inUse + need <= budget can always be owned: the refill thread takes chunks up to the budget.
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

// One chunk, as large as can be had: from the refill thread's level down the fallback sizes, every kind at each size. True when a
// chunk was taken (or the disk is stopping), false when everything failed. Entered and left with the lock held.
template <class Env> bool volatileRefillOnce(VolatilePool &p, Env &env)
{
  const uint64_t room = p.budgetPages - p.ownedPages;
  for (uint32_t l = p.level; l < VolatilePool::kLevels; ++l) {
    const uint32_t pages = VolatilePool::levelPages(l) > room ? (uint32_t) room : VolatilePool::levelPages(l);
    for (uint32_t kind = 0; kind < env.kinds(); ++kind) {
      env.unlock();
      uint8_t *chunk = env.allocChunk(pages, kind);
      env.lock();
      if (!chunk) { ++p.chunkFailures; continue; }
      if (env.stopping() || !p.addChunk(chunk, pages, kind)) { env.unlock(); env.freeChunk(chunk, pages, kind); env.lock(); return true; }
      // Stay at the size that worked; after 64 chunks below 2 MiB, try the next size up again.
      if (l != p.level) { p.level = l; p.levelWins = 0; }
      else if (l && ++p.levelWins >= 64) { p.level = l - 1; p.levelWins = 0; }
      p.backoffMs = 0;
      env.wakeWriters();
      return true;
    }
  }
  p.level = 0;
  p.levelWins = 0;
  return false;
}

// The refill thread's loop, entered and left with the caller's lock held. Env:
//   bool stopping();                the disk is going away
//   void idle();                    sleep, the lock released, until woken or the status interval passes
//   void unlock(); void lock();
//   uint32_t kinds();               how many allocators to try, in order
//   uint8_t *allocChunk(uint32_t pages, uint32_t kind);   pages * 4096 bytes, may wait for memory (without the lock), null on failure
//   void freeChunk(uint8_t *, uint32_t pages, uint32_t kind);   (without the lock)
//   VolatileChunk *allocRecords(uint64_t n); void freeRecords(VolatileChunk *, uint64_t n);   (without the lock)
//   void wakeWriters();
//   void backoff(uint32_t ms);      after a round in which nothing could be had: sleep, the lock released, woken only by stop
template <class Env> void volatileRefill(VolatilePool &p, Env &env)
{
  while (!env.stopping()) {
    if (!p.wantsRefill()) { env.idle(); continue; }
    if (p.needsRecords()) {
      const uint64_t cap = p.recordsWanted();
      env.unlock();
      VolatileChunk *grown = env.allocRecords(cap);
      env.lock();
      if (grown) {
        // Only this thread changes the records, so chunkCount is what it was before the unlock.
        VolatileChunk *old = p.chunks;
        const uint64_t oldCap = p.chunkCapacity;
        memcpy(grown, old, p.chunkCount * sizeof *old);
        p.chunks = grown;
        p.chunkCapacity = cap;
        ++p.recordGrowths;
        env.unlock();
        env.freeRecords(old, oldCap);
        env.lock();
        continue;
      }
      ++p.chunkFailures;
    } else if (volatileRefillOnce(p, env)) {
      continue;
    }
    ++p.exhaustedRounds;
    p.backoffMs = p.backoffMs ? (p.backoffMs * 2 > 1000 ? 1000 : p.backoffMs * 2) : 10;
    env.backoff(p.backoffMs);
  }
}
