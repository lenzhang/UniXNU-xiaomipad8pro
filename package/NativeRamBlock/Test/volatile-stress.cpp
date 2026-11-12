// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano host stress test of the volatile Data disk's memory (VolatilePool.h with VolatileDisk.h), the code that hung the tablet
// under Spotlight's first import when every page was a blocking zone allocation inside the I/O lock:
//   A. deterministic, one thread: sustained writes until the budget is reached (then refused with no space, never blocked, nothing
//      half written), overwrites that still succeed, unmap giving room back, a memory provider that stops giving (writes refused with
//      no memory once the deadline passes, reads and overwrites unaffected), data against an oracle throughout, accounting checked
//   B. threads as in the kernel: one lock (the gate), a refill thread that allocates without it and stalls as VM_PAGE_WAIT does,
//      writers and readers on their own regions, unmaps, a provider that runs out of memory; a watchdog fails the test on a hang
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>
#include "../SeedDisk.h"
#include "../VolatileDisk.h"
#include "../VolatilePool.h"

static std::atomic<unsigned> failures{0}, checks{0};
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint64_t kLogical = 12ULL << 30, kPages = kLogical / 4096;   // the installed Data disk's size

struct Seed {
  SeedDisk disk;
  std::vector<uint64_t> index;
  std::vector<uint8_t *> page;
  Seed() {
    for (uint64_t i = 0; i < kPages; i += 7919) {                         // ~400 nonzero seed pages spread over the disk
      index.push_back(i);
      uint8_t *p = (uint8_t *) malloc(4096);
      for (int k = 0; k < 4096; ++k) p[k] = (uint8_t) (i * 31 + k);
      page.push_back(p);
    }
    disk.bytes = kLogical; disk.index = index.data(); disk.page = page.data(); disk.count = disk.capacity = index.size();
  }
  ~Seed() { for (auto p : page) free(p); }
};
struct Buffer {
  std::vector<uint8_t> &data;
  uint64_t read(uint64_t at, void *p, uint64_t n) { memcpy(p, &data[at], n); return n; }
  uint64_t copy(uint64_t at, const void *p, uint64_t n) { memcpy(&data[at], p, n); return n; }
};
struct Oracle {                                   // page index -> content, for pages that differ from the seed
  const Seed &seed;
  std::map<uint64_t, std::vector<uint8_t>> pages;
  explicit Oracle(const Seed &s) : seed(s) {}
  std::vector<uint8_t> expect(uint64_t at, uint64_t len) const {
    std::vector<uint8_t> out(len);
    for (uint64_t k = 0; k < len;) {
      uint64_t a = at + k, i = a / 4096, n = std::min<uint64_t>(4096 - (a & 4095), len - k);
      auto it = pages.find(i); const uint8_t *src = it != pages.end() ? it->second.data() : seed.disk.find(i);
      if (src) memcpy(&out[k], src + (a & 4095), n); else memset(&out[k], 0, n);
      k += n;
    }
    return out;
  }
  void write(uint64_t at, const std::vector<uint8_t> &data, uint64_t len) {
    for (uint64_t k = 0; k < len;) {
      uint64_t a = at + k, i = a / 4096, n = std::min<uint64_t>(4096 - (a & 4095), len - k);
      if (!pages.count(i)) { const uint8_t *s = seed.disk.find(i); pages[i] = s ? std::vector<uint8_t>(s, s + 4096) : std::vector<uint8_t>(4096, 0); }
      memcpy(&pages[i][a & 4095], &data[k], n);
      k += n;
    }
  }
  void unmap(uint64_t at, uint64_t len) {         // whole pages read as the seed's again
    for (uint64_t i = (at + 4095) / 4096; i < (at + len) / 4096; ++i) pages.erase(i);
  }
};
struct Provider {                                 // the system's memory for chunks (any size: the pool falls back to smaller ones)
  std::mutex m;
  uint64_t limitChunks = ~0ULL, given = 0, refusals = 0;
  std::atomic<int> stallMs{0};                    // each allocation waits this long (VM_PAGE_WAIT)
  std::vector<uint8_t *> live;
  uint8_t *alloc(uint32_t pages = VolatilePool::kChunkPages) {
    if (int ms = stallMs.load()) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    std::lock_guard<std::mutex> g(m);
    if (given >= limitChunks) { ++refusals; return nullptr; }
    ++given;
    uint8_t *c = (uint8_t *) malloc(pages * 4096ULL);
    memset(c, 0xA5, pages * 4096ULL);              // not zeroed: take() must zero
    live.push_back(c);
    return c;
  }
  ~Provider() { for (auto c : live) free(c); }
};
static void setupPool(VolatilePool &p, uint64_t budgetPages) {
  free(p.chunks);
  p = VolatilePool();
  p.budgetPages = budgetPages;
  p.chunkCapacity = VolatilePool::capacityFor(budgetPages);
  p.chunks = (VolatileChunk *) calloc(p.chunkCapacity, sizeof(VolatileChunk));   // records grow with calloc/free (the envs below)
}

// ---- A: one thread; "sleep" runs the refill thread's work synchronously, and the deadline is a number of sleeps -------------------
struct SyncRefill {                               // the refill thread's Env with no thread: no lock, one kind
  Provider &prov;
  bool stopping() { return false; }
  void unlock() {}
  void lock() {}
  uint32_t kinds() { return 1; }
  uint8_t *allocChunk(uint32_t pages, uint32_t) { return prov.alloc(pages); }
  void freeChunk(uint8_t *, uint32_t, uint32_t) {}
  void wakeWriters() {}
};
static void refillNow(VolatilePool &pool, Provider &prov) {   // what the refill thread does until it would idle or back off
  SyncRefill r{prov};
  while (pool.wantsRefill()) {
    if (pool.needsRecords()) {
      uint64_t cap = pool.recordsWanted();
      VolatileChunk *n = (VolatileChunk *) calloc(cap, sizeof *n);
      memcpy(n, pool.chunks, pool.chunkCount * sizeof *n);
      free(pool.chunks); pool.chunks = n; pool.chunkCapacity = cap;
      continue;
    }
    if (!volatileRefillOnce(pool, r)) break;
  }
}
struct SyncEnv {
  VolatilePool &pool; Provider &prov; VolatileDisk &d; uint64_t off, len; int sleepsLeft;
  uint64_t need() { return volatileNeed(d, off, len); }
  void wakeRefill() {}
  bool sleep() { refillNow(pool, prov); return --sleepsLeft > 0; }
};
static VolatileAdmit writeA(const Seed &seed, VolatilePool &pool, Provider &prov, VolatileDisk &d, Oracle &o, uint64_t off, uint64_t len,
                            unsigned salt, unsigned *nulls) {
  std::vector<uint8_t> io(len);
  for (uint64_t k = 0; k < len; ++k) io[k] = (uint8_t) (salt + k * 7);
  SyncEnv env{pool, prov, d, off, len, 3};
  VolatileAdmit a = volatileAdmit(pool, env);
  if (a != VolatileAdmit::Ok) return a;
  struct Ops { VolatilePool &p; unsigned *nulls; uint8_t *allocate() { uint8_t *x = p.take(); if (!x) ++*nulls; return x; } } ops{pool, nulls};
  Buffer b{io}; bool noMem = false;
  CHECK(volatileDiskIO(seed.disk, d, true, off, len, b, ops, noMem) == len && !noMem);
  o.write(off, io, len);
  return a;
}
static bool readA(const Seed &seed, VolatileDisk &d, const Oracle &o, uint64_t off, uint64_t len) {
  std::vector<uint8_t> io(len, 0xEE); Buffer b{io}; bool noMem = false;
  struct Ops { uint8_t *allocate() { return nullptr; } } ops;
  return volatileDiskIO(seed.disk, d, false, off, len, b, ops, noMem) == len && io == o.expect(off, len);
}

static void partA(const Seed &seed) {
  VolatilePool pool; Provider prov;
  const uint64_t budget = 16384;                                          // 64 MiB
  setupPool(pool, budget);
  pool.reservePages = pool.reserveMaxPages = 2048;                       // the old 8-chunk reserve: the budget is reached by writes
  VolatileDisk d; d.bytes = kLogical; std::vector<uint8_t **> dir(VolatileDisk::entries(kLogical), nullptr); d.dir = dir.data();
  Oracle o(seed);
  std::mt19937_64 rng(5);
  unsigned nulls = 0, ok = 0, noSpace = 0, noMem = 0;
  // Sustained new writes, scattered over the whole 12 GiB (a leaf for almost every page: the worst case for the budget).
  for (int op = 0; op < 40000 && !noSpace; ++op) {
    uint64_t len = 1 + rng() % 200000, off = rng() % (kLogical - len);
    if (op % 3 == 0) off &= ~4095ULL;
    VolatileAdmit a = writeA(seed, pool, prov, d, o, off, len, (unsigned) op, &nulls);
    if (a == VolatileAdmit::Ok) ++ok; else if (a == VolatileAdmit::NoSpace) ++noSpace; else ++noMem;
    CHECK(pool.inUse <= pool.ownedPages && pool.ownedPages <= budget && pool.chunkCount <= pool.chunkCapacity);
    if (op % 500 == 0) CHECK(readA(seed, d, o, off, len));
  }
  CHECK(noSpace == 1 && noMem == 0 && nulls == 0 && ok > 100);
  CHECK(pool.inUse + 1000 > budget && pool.heldBytes() <= budget * 4096);
  CHECK(d.resident / 4096 + d.leaves == pool.inUse);
  // At the budget: a write that needs a new page is refused at once, nothing changed; overwrites of resident pages still work.
  uint64_t resident = 0; while (!d.find(resident)) ++resident;
  {
    uint64_t before = pool.inUse;
    VolatileAdmit a = writeA(seed, pool, prov, d, o, kLogical - 8192, 8192, 1, &nulls);  // likely needs pages
    if (a == VolatileAdmit::NoSpace) CHECK(pool.inUse == before && readA(seed, d, o, kLogical - 8192, 8192));
    CHECK(writeA(seed, pool, prov, d, o, resident * 4096 + 100, 3000, 2, &nulls) == VolatileAdmit::Ok);   // no new page
    CHECK(readA(seed, d, o, resident * 4096, 4096));
  }
  // Unmap gives room back: unmap the first half of the disk, then writes fit again; unmapped pages read as the seed's.
  {
    struct Give { VolatilePool &p; void operator()(uint8_t *x) { p.give(x); } } give{pool};
    uint64_t before = pool.inUse;
    uint64_t freed = volatileUnmap(d, 0, kLogical / 2, give);
    o.unmap(0, kLogical / 2);
    CHECK(freed > 0 && pool.inUse == before - freed && pool.freePages == freed);
    for (uint64_t i = 0; i < kPages / 2; i += 9973) CHECK(readA(seed, d, o, i * 4096, 4096));
    CHECK(writeA(seed, pool, prov, d, o, 4096 * 100, 4096 * 64, 3, &nulls) == VolatileAdmit::Ok);
    CHECK(readA(seed, d, o, 4096 * 100, 4096 * 64) && nulls == 0);
    // a partial-page unmap keeps the page
    uint64_t r = 0; while (!d.find(r)) ++r;
    CHECK(volatileUnmap(d, r * 4096 + 1, 4094, give) == 0 && d.find(r));
  }
  // Everything written and not unmapped reads back.
  for (auto &e : o.pages) if (e.first % 37 == 0) CHECK(readA(seed, d, o, e.first * 4096, 4096));
  // Low memory, far below the budget: the provider gives 3 chunks and then nothing; new pages are refused with no memory once the
  // deadline passes (never no space: the budget has room), overwrites and reads go on.
  {
    VolatilePool p2; Provider prov2; prov2.limitChunks = 3;
    setupPool(p2, 262144);                                             // 1 GiB budget
    VolatileDisk d2; d2.bytes = kLogical; std::vector<uint8_t **> dir2(VolatileDisk::entries(kLogical), nullptr); d2.dir = dir2.data();
    Oracle o2(seed);
    unsigned memRefused = 0, okHere = 0, spaceRefused = 0;
    for (int op = 0; op < 3000; ++op) {
      uint64_t len = 4096 * (1 + rng() % 64), off = (rng() % (kPages / 64 - 64)) * 4096;
      VolatileAdmit a = writeA(seed, p2, prov2, d2, o2, off, len, 900u + (unsigned) op, &nulls);
      if (a == VolatileAdmit::NoMemory) ++memRefused; else if (a == VolatileAdmit::Ok) ++okHere; else ++spaceRefused;
    }
    CHECK(memRefused > 0 && okHere > 0 && spaceRefused == 0 && nulls == 0 && p2.chunkCount == 3 && prov2.refusals > 0);
    uint64_t r = 0; while (!d2.find(r)) ++r;
    CHECK(writeA(seed, p2, prov2, d2, o2, r * 4096, 4096, 5, &nulls) == VolatileAdmit::Ok);         // an overwrite needs nothing
    for (int k = 0; k < 300; ++k) { uint64_t off = (rng() % (kPages / 64)) * 4096; CHECK(readA(seed, d2, o2, off, 4096)); }
    printf("  A low memory: %u writes ok, %u refused with no memory, %llu provider refusals\n", okHere, memRefused,
           (unsigned long long) prov2.refusals);
    free(p2.chunks);
  }
  free(pool.chunks);
  printf("  A: %u writes ok, refused %llu no space / %llu no memory, %llu waits, peak %llu pages, %llu chunks, %llu unmapped\n",
         ok, (unsigned long long) pool.refusedNoSpace, (unsigned long long) pool.refusedNoMemory, (unsigned long long) pool.waits,
         (unsigned long long) pool.peak, (unsigned long long) pool.chunkCount, (unsigned long long) pool.freePages);
}

// ---- B: threads ----------------------------------------------------------------------------------------------------------------
struct Disk {                                    // the kernel's arrangement: one lock over pool and disk
  std::mutex gate;
  std::condition_variable spare, refill, backoff;   // writers wait on spare, the refill thread on refill (backoff: only stop)
  VolatilePool pool;
  VolatileDisk d; std::vector<uint8_t **> dir;
  bool stop = false;
  Provider prov;
  ~Disk() { free(pool.chunks); }
};
static void refillThread(Disk &k) {
  std::unique_lock<std::mutex> l(k.gate);
  struct Env {
    Disk &k; std::unique_lock<std::mutex> &l;
    bool stopping() { return k.stop; }
    void idle() { k.refill.wait_for(l, std::chrono::milliseconds(200)); }
    void unlock() { l.unlock(); }
    void lock() { l.lock(); }
    uint32_t kinds() { return 1; }
    uint8_t *allocChunk(uint32_t pages, uint32_t) { return k.prov.alloc(pages); }
    void freeChunk(uint8_t *, uint32_t, uint32_t) {}   // the provider frees at the end
    VolatileChunk *allocRecords(uint64_t n) { return (VolatileChunk *) calloc(n, sizeof(VolatileChunk)); }
    void freeRecords(VolatileChunk *r, uint64_t) { free(r); }
    void wakeWriters() { k.spare.notify_all(); }
    void backoff(uint32_t ms) { k.backoff.wait_for(l, std::chrono::milliseconds(ms)); }
  } env{k, l};
  volatileRefill(k.pool, env);
}
struct Stats { std::atomic<uint64_t> ok{0}, noSpace{0}, noMemory{0}, reads{0}, unmaps{0}, nulls{0}; };
static void writerThread(Disk &k, const Seed &seed, unsigned id, uint64_t regionPages, int ops, Stats &st, std::atomic<bool> &bad) {
  const uint64_t base = (uint64_t) id * regionPages * 4096;  // a region of its own, so its oracle is exact
  Oracle o(seed);
  std::mt19937_64 rng(100 + id);
  std::vector<uint8_t> io(256 * 1024);
  for (int op = 0; op < ops; ++op) {
    uint64_t len = 1 + rng() % (op % 10 == 0 ? 256 * 1024 : 16384), off = base + rng() % (regionPages * 4096 - len);
    if (rng() & 1) off &= ~4095ULL;
    int what = (int) (rng() % 10);
    std::unique_lock<std::mutex> l(k.gate);
    if (what < 6) {                                // write
      for (uint64_t x = 0; x < len; ++x) io[x] = (uint8_t) (id * 13 + op + x);
      struct Env {
        Disk &k; std::unique_lock<std::mutex> &l; uint64_t off, len; std::chrono::steady_clock::time_point deadline;
        uint64_t need() { return volatileNeed(k.d, off, len); }
        void wakeRefill() { k.refill.notify_one(); }
        bool sleep() { return k.spare.wait_until(l, deadline) == std::cv_status::no_timeout || std::chrono::steady_clock::now() < deadline; }
      } env{k, l, off, len, std::chrono::steady_clock::now() + std::chrono::milliseconds(20)};
      VolatileAdmit a = volatileAdmit(k.pool, env);
      if (a == VolatileAdmit::NoSpace) { ++st.noSpace; continue; }
      if (a == VolatileAdmit::NoMemory) { ++st.noMemory; continue; }
      struct Ops { VolatilePool &p; Stats &st; uint8_t *allocate() { uint8_t *x = p.take(); if (!x) ++st.nulls; return x; } } ops{k.pool, st};
      Buffer b{io}; bool noMem = false;
      if (volatileDiskIO(seed.disk, k.d, true, off, len, b, ops, noMem) != len || noMem) { bad = true; continue; }
      std::vector<uint8_t> copy(io.begin(), io.begin() + len);
      o.write(off, copy, len);
      ++st.ok;
    } else if (what < 9) {                         // read and compare
      std::vector<uint8_t> out(len, 0xEE); Buffer b{out}; bool noMem = false;
      struct Ops { uint8_t *allocate() { return nullptr; } } ops;
      if (volatileDiskIO(seed.disk, k.d, false, off, len, b, ops, noMem) != len || out != o.expect(off, len)) bad = true;
      ++st.reads;
    } else {                                       // unmap part of the region
      struct Give { VolatilePool &p; void operator()(uint8_t *x) { p.give(x); } } give{k.pool};
      volatileUnmap(k.d, off, len, give);
      o.unmap(off, len);
      ++st.unmaps;
    }
  }
  std::unique_lock<std::mutex> l(k.gate);          // the whole region against the oracle at the end
  for (uint64_t at = base; at < base + regionPages * 4096; at += 128 * 1024) {
    std::vector<uint8_t> out(128 * 1024); Buffer b{out}; bool noMem = false;
    struct Ops { uint8_t *allocate() { return nullptr; } } ops;
    if (volatileDiskIO(seed.disk, k.d, false, at, out.size(), b, ops, noMem) != out.size() || out != o.expect(at, out.size())) { bad = true; break; }
  }
}
static bool partB(const Seed &seed, const char *name, uint64_t budget, uint64_t limitChunks, int stallMs, int threads, int ops) {
  Disk k;
  setupPool(k.pool, budget);
  k.d.bytes = kLogical; k.dir.assign(VolatileDisk::entries(kLogical), nullptr); k.d.dir = k.dir.data();
  k.prov.limitChunks = limitChunks; k.prov.stallMs = stallMs;
  Stats st; std::atomic<bool> bad{false}, done{false};
  std::thread refill(refillThread, std::ref(k));
  std::vector<std::thread> writers;
  const uint64_t region = kPages / (uint64_t) threads;
  auto t0 = std::chrono::steady_clock::now();
  for (int t = 0; t < threads; ++t) writers.emplace_back(writerThread, std::ref(k), std::cref(seed), (unsigned) t, region / 64, ops, std::ref(st), std::ref(bad));
  std::thread watchdog([&] {                       // a hang fails the test (and the process: a hang never returns)
    for (int s = 0; s < 120 && !done; ++s) std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!done) { printf("FAIL %s: no progress for 120 s: a hang\n", name); fflush(stdout); _exit(2); }
  });
  for (auto &w : writers) w.join();
  { std::lock_guard<std::mutex> g(k.gate); k.stop = true; }
  k.refill.notify_all(); k.backoff.notify_all();
  refill.join();
  done = true;
  watchdog.join();
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  bool ok = !bad && st.nulls == 0 && k.pool.inUse <= k.pool.ownedPages && k.pool.ownedPages <= budget &&
            k.pool.chunkCount <= k.pool.chunkCapacity && k.pool.chunkCount <= limitChunks &&
            k.d.resident / 4096 + k.d.leaves == k.pool.inUse;
  CHECK(ok);
  printf("  B %s: %.1f s, %llu writes ok, refused %llu no space / %llu no memory, %llu reads, %llu unmaps, peak %llu MiB of %llu, "
         "%llu chunks (%llu provider refusals)\n", name, secs, (unsigned long long) st.ok.load(), (unsigned long long) st.noSpace.load(),
         (unsigned long long) st.noMemory.load(), (unsigned long long) st.reads.load(), (unsigned long long) st.unmaps.load(),
         (unsigned long long) k.pool.peak / 256, (unsigned long long) budget / 256, (unsigned long long) k.pool.chunkCount,
         (unsigned long long) k.prov.refusals);
  return ok;
}

// ---- C: the tablet's hang as a model, and the same model against the pool --------------------------------------------------------
// The system has no free memory; it gets some back only when "pageout" has written a dirty Data page through this disk, which needs
// the disk's lock. The first VolatileDisk allocated inside the lock and waited there for memory (zalloc's VM_PAGE_WAIT): deadlock.
// With the pool the writer waits with the lock released (bounded), the refill thread waits for memory without the lock, pageout
// gets through, memory comes back, the writer goes on.
struct Memory {
  std::mutex m; std::condition_variable cv; uint64_t freePages = 0; bool flood = false;
  void waitFor(uint64_t pages) {                  // VM_PAGE_WAIT: no timeout
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return flood || freePages >= pages; });
    if (!flood) freePages -= pages;
  }
  void give(uint64_t pages) { { std::lock_guard<std::mutex> g(m); freePages += pages; } cv.notify_all(); }
  void release() { { std::lock_guard<std::mutex> g(m); flood = true; } cv.notify_all(); }   // the test's way out
};
static bool partC(const Seed &seed, bool oldDesign) {
  Disk k; Memory mem;
  setupPool(k.pool, 262144);
  k.d.bytes = kLogical; k.dir.assign(VolatileDisk::entries(kLogical), nullptr); k.d.dir = k.dir.data();
  std::atomic<uint64_t> progress{0}; std::atomic<bool> finished{false};
  std::vector<uint8_t> io(64 * 4096, 0x5A);
  std::thread refill;
  if (!oldDesign) refill = std::thread([&] {
    std::unique_lock<std::mutex> l(k.gate);
    struct Env {
      Disk &k; Memory &mem; std::unique_lock<std::mutex> &l;
      bool stopping() { return k.stop; }
      void idle() { k.refill.wait_for(l, std::chrono::milliseconds(50)); }
      void unlock() { l.unlock(); }
      void lock() { l.lock(); }
      uint32_t kinds() { return 1; }
      uint8_t *allocChunk(uint32_t pages, uint32_t) { mem.waitFor(pages); return k.prov.alloc(pages); }
      void freeChunk(uint8_t *, uint32_t, uint32_t) {}
      VolatileChunk *allocRecords(uint64_t n) { return (VolatileChunk *) calloc(n, sizeof(VolatileChunk)); }
      void freeRecords(VolatileChunk *r, uint64_t) { free(r); }
      void wakeWriters() { k.spare.notify_all(); }
      void backoff(uint32_t ms) { k.backoff.wait_for(l, std::chrono::milliseconds(ms)); }
    } env{k, mem, l};
    volatileRefill(k.pool, env);
  });
  std::thread writer([&] {                         // sustained writes of new pages, 64 at a time
    for (uint64_t w = 0; w < 200 && !finished; ++w) {
      uint64_t off = w * 64 * 4096;
      std::unique_lock<std::mutex> l(k.gate);
      if (oldDesign) {
        struct Ops { Memory &mem; uint8_t *allocate() { mem.waitFor(1); return (uint8_t *) calloc(1, 4096); } } ops{mem};   // in the lock
        Buffer b{io}; bool noMem = false;
        volatileDiskIO(seed.disk, k.d, true, off, io.size(), b, ops, noMem);
      } else {
        struct Env {
          Disk &k; std::unique_lock<std::mutex> &l; uint64_t off, len; std::chrono::steady_clock::time_point deadline;
          uint64_t need() { return volatileNeed(k.d, off, len); }
          void wakeRefill() { k.refill.notify_one(); }
          bool sleep() { return k.spare.wait_until(l, deadline) == std::cv_status::no_timeout || std::chrono::steady_clock::now() < deadline; }
        } env{k, l, off, io.size(), std::chrono::steady_clock::now() + std::chrono::milliseconds(200)};
        if (volatileAdmit(k.pool, env) == VolatileAdmit::Ok) {
          struct Ops { VolatilePool &p; uint8_t *allocate() { return p.take(); } } ops{k.pool};
          Buffer b{io}; bool noMem = false;
          volatileDiskIO(seed.disk, k.d, true, off, io.size(), b, ops, noMem);
        }
      }
      ++progress;
    }
  });
  std::thread pageout([&] {                        // each write-back through the disk frees one chunk's worth of memory
    while (!finished) {
      { std::lock_guard<std::mutex> g(k.gate); }  // the I/O itself (a resident page: needs no new one)
      mem.give(VolatilePool::kChunkPages);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  uint64_t last = ~0ULL; int still = 0; bool hung = false;
  while (progress < 200) {                         // the watchdog: 2 s without a single write done is a hang
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (progress == last) { if (++still >= 20) { hung = true; break; } } else { still = 0; last = progress; }
  }
  finished = true;
  mem.release();                                   // let a deadlocked writer out so the test can end
  writer.join(); pageout.join();
  { std::lock_guard<std::mutex> g(k.gate); k.stop = true; }
  k.refill.notify_all(); k.backoff.notify_all();
  if (refill.joinable()) refill.join();
  if (oldDesign) {                                 // the old pages were calloc'ed one by one
    struct Free { void operator()(uint8_t *p) { free(p); } } f;
    volatileDiskRelease(k.d, f);
  }
  printf("  C %s design: %s after %llu of 200 writes\n", oldDesign ? "old (allocation in the lock)" : "pool",
         hung ? "DEADLOCK" : "done", (unsigned long long) progress.load());
  return hung;
}

int main() {
  Seed seed;
  CHECK(partC(seed, true));                        // the model reproduces the tablet's hang with the first design
  CHECK(!partC(seed, false));                      // and the pool does not hang in it
  partA(seed);
  // plenty of memory, budget never reached
  CHECK(partB(seed, "ample", 262144, ~0ULL, 0, 6, 3000));
  // sustained writes into a small budget: refusals with no space, never a block
  CHECK(partB(seed, "budget", 4096, ~0ULL, 0, 6, 3000));
  // the system runs out of memory before the budget (and every allocation stalls 20 ms, as VM_PAGE_WAIT): refusals with no memory
  CHECK(partB(seed, "low-memory", 262144, 6, 20, 6, 800));
  // a provider that never gives anything: every new page refused, reads and the end check still fine
  CHECK(partB(seed, "no-memory", 262144, 0, 1, 4, 200));
  printf("%s: %u checks, %u failures\n", failures ? "FAIL" : "PASS", checks.load(), failures.load());
  return failures ? 1 : 0;
}
