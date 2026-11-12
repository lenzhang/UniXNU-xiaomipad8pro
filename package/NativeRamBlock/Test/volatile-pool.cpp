// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano host test of VolatilePool.h's boot reservation and fallbacks, against the 2026-11-12 desktop session: at 1080 MiB held
// the refill thread's 2 MiB IOMallocAligned failed 87474 times (the kernel's shared data range, which the GPU's kernel buffers fill,
// had no 2 MiB left), 13 writes were refused with no memory, and APFS read blocks of those writes back as zeros. Here each allocator
// kind is an address range with a size and a largest hole (Range below), so a range can be exhausted or fragmented.
//   R. the incident, scaled: the old pool (one range, 2 MiB only, no reservation) refuses writes once the GPU has taken its range;
//      the new one (reserved at start) refuses none; the new one without a reservation still gets by on the other ranges
//   F. fragmentation: no hole of 2 MiB anywhere, ranges with 32 KiB and single-page holes: the whole budget is still had (records
//      grow), every write under the budget succeeds, data checked, release gives every chunk back to its own range
//   P. a provider that never gives, writers waiting: the refill thread backs off instead of the old busy loop (attempts counted)
//   B. bursts with a slow provider and no reservation: the reserve deepens after waits; nothing refused under a generous deadline
//   U. unmap and reads after it: a written page reads as written until unmapped, then as the seed's (or zeros); reused pages are
//      zeroed and do not alias; partial-page unmaps keep data; unmap across never-written leaves and the disk's end
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include "../SeedDisk.h"
#include "../VolatileDisk.h"
#include "../VolatilePool.h"

static std::atomic<unsigned> failures{0}, checks{0};
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint64_t kLogical = 12ULL << 30, kPages = kLogical / 4096;

struct Seed {
  SeedDisk disk;
  std::vector<uint64_t> index;
  std::vector<uint8_t *> page;
  Seed() {
    for (uint64_t i = 0; i < kPages; i += 4999) {
      index.push_back(i);
      uint8_t *p = (uint8_t *) malloc(4096);
      for (int k = 0; k < 4096; ++k) p[k] = (uint8_t) (i * 17 + k + 1);
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
struct Oracle {
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
  void unmap(uint64_t at, uint64_t len) { for (uint64_t i = (at + 4095) / 4096; i < (at + len) / 4096; ++i) pages.erase(i); }
};

// An address range of the kernel (kmem's private data, shared data, pointer ranges): `room` pages left, no hole larger than
// `largest` pages. Others can take from it (the GPU). The backing is malloc, filled with garbage (take() must zero).
struct Range {
  uint64_t room = 0, largest = 0, given = 0, freed = 0, attempts = 0;
  std::map<uint8_t *, uint32_t> live;
  uint8_t *alloc(uint32_t pages) {
    ++attempts;
    if (pages > largest || pages > room) return nullptr;
    room -= pages;
    uint8_t *c = (uint8_t *) malloc(pages * 4096ULL);
    memset(c, 0x5C, pages * 4096ULL);
    live[c] = pages; ++given;
    return c;
  }
  bool release(uint8_t *c, uint32_t pages) {
    auto it = live.find(c);
    if (it == live.end() || it->second != pages) return false;
    live.erase(it); free(c); room += pages; ++freed;
    return true;
  }
  ~Range() { for (auto &e : live) free(e.first); }
};
struct Ranges {                                   // kinds in the kernel's order: 0 private data, 1 shared data, 2 default heap
  std::mutex m;
  Range r[3];
  uint32_t kinds = 3;
  bool only2MiB = false;                          // the old pool asked for nothing else
  std::atomic<int> stallMs{0};
  std::atomic<uint64_t> attempts{0};
  bool badFree = false;
  uint8_t *alloc(uint32_t pages, uint32_t kind) {
    ++attempts;
    if (int ms = stallMs.load()) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    std::lock_guard<std::mutex> g(m);
    if (only2MiB && pages != VolatilePool::kChunkPages) return nullptr;
    return kind < kinds ? r[kind].alloc(pages) : nullptr;
  }
  void free(uint8_t *c, uint32_t pages, uint32_t kind) { std::lock_guard<std::mutex> g(m); if (kind >= kinds || !r[kind].release(c, pages)) badFree = true; }
};

static void setupPool(VolatilePool &p, uint64_t budget, uint64_t prefill) {
  p = VolatilePool();
  p.budgetPages = budget;
  p.prefillPages = prefill;
  p.chunkCapacity = VolatilePool::capacityFor(budget);
  p.chunks = (VolatileChunk *) calloc(p.chunkCapacity, sizeof(VolatileChunk));
}
struct NoLock {                                   // the refill Env for one thread (no lock to drop)
  Ranges &rs;
  bool stopping() { return false; }
  void unlock() {}
  void lock() {}
  uint32_t kinds() { return rs.kinds; }
  uint8_t *allocChunk(uint32_t pages, uint32_t kind) { return rs.alloc(pages, kind); }
  void freeChunk(uint8_t *c, uint32_t pages, uint32_t kind) { rs.free(c, pages, kind); }
  void wakeWriters() {}
};
static bool growRecords(VolatilePool &p) {
  uint64_t cap = p.recordsWanted();
  VolatileChunk *n = (VolatileChunk *) calloc(cap, sizeof *n);
  if (!n) return false;
  memcpy(n, p.chunks, p.chunkCount * sizeof *n);
  free(p.chunks); p.chunks = n; p.chunkCapacity = cap; ++p.recordGrowths;
  return true;
}
static void refillNow(VolatilePool &p, Ranges &rs) {   // the refill thread's work until it would idle or back off
  NoLock env{rs};
  while (p.wantsRefill()) {
    if (p.needsRecords()) { if (!growRecords(p)) break; continue; }
    if (!volatileRefillOnce(p, env)) { ++p.exhaustedRounds; break; }
  }
}
struct SyncEnv {
  VolatilePool &p; Ranges &rs; VolatileDisk &d; uint64_t off, len; int sleepsLeft;
  uint64_t need() { return volatileNeed(d, off, len); }
  void wakeRefill() {}
  bool sleep() { refillNow(p, rs); return --sleepsLeft > 0; }
};
struct Fixture {
  const Seed &seed;
  VolatilePool p; Ranges rs;
  VolatileDisk d; std::vector<uint8_t **> dir;
  Oracle o;
  unsigned nulls = 0;
  explicit Fixture(const Seed &s) : seed(s), o(s) { d.bytes = kLogical; dir.assign(VolatileDisk::entries(kLogical), nullptr); d.dir = dir.data(); }
  ~Fixture() {
    struct Free { Ranges &rs; void operator()(uint8_t *c, uint32_t pages, uint32_t kind) { rs.free(c, pages, kind); } } f{rs};
    p.release(f);
    free(p.chunks);
  }
  VolatileAdmit write(uint64_t off, uint64_t len, unsigned salt) {
    std::vector<uint8_t> io(len);
    for (uint64_t k = 0; k < len; ++k) io[k] = (uint8_t) (salt * 7 + k * 13 + 3);
    SyncEnv env{p, rs, d, off, len, 3};
    VolatileAdmit a = volatileAdmit(p, env);
    if (a != VolatileAdmit::Ok) return a;
    struct Ops { VolatilePool &p; unsigned &nulls; uint8_t *allocate() { uint8_t *x = p.take(); if (!x) ++nulls; return x; } } ops{p, nulls};
    Buffer b{io}; bool noMem = false;
    CHECK(volatileDiskIO(seed.disk, d, true, off, len, b, ops, noMem) == len && !noMem);
    o.write(off, io, len);
    return a;
  }
  bool read(uint64_t off, uint64_t len) {
    std::vector<uint8_t> io(len, 0xEE); Buffer b{io}; bool noMem = false;
    struct Ops { uint8_t *allocate() { return nullptr; } } ops;
    return volatileDiskIO(seed.disk, d, false, off, len, b, ops, noMem) == len && io == o.expect(off, len);
  }
  uint64_t unmap(uint64_t off, uint64_t len) {
    struct Give { VolatilePool &p; void operator()(uint8_t *x) { p.give(x); } } give{p};
    uint64_t n = volatileUnmap(d, off, len, give);
    p.unmapped += n;
    o.unmap(off, len);
    return n;
  }
  bool consistent() { return d.resident / 4096 + d.leaves == p.inUse && p.inUse <= p.ownedPages && p.ownedPages <= p.budgetPages; }
};

// ---- R: the incident, scaled 1:8 (budget 256 MiB; the shared range has 135 MiB for us before the GPU takes the rest) ------------
struct Outcome { unsigned ok = 0, noMemory = 0, noSpace = 0; uint64_t attempts = 0, owned = 0; };
static Outcome incident(const Seed &seed, bool oldPool, bool reserve) {
  Fixture f(seed);
  const uint64_t budget = 65536;
  setupPool(f.p, budget, reserve ? budget : 0);
  // At boot: plenty everywhere.
  for (auto &r : f.rs.r) { r.room = 1 << 20; r.largest = 1 << 20; }
  if (oldPool) {
    // The old pool: one allocator (IOMallocAligned, the shared range: kind 0 here), 2 MiB only, 8 chunks kept ready.
    f.rs.kinds = 1;
    f.rs.only2MiB = true;
    f.p.reservePages = f.p.reserveMaxPages = 8 * VolatilePool::kChunkPages;
  }
  refillNow(f.p, f.rs);                            // the refill thread right after attach: the reservation (or the first reserve)
  // The desktop: the GPU's buffers take the shared range (kind 1; for the old pool its only range, kind 0): 135 MiB more for us,
  // in holes below 2 MiB.
  Range &shared = oldPool ? f.rs.r[0] : f.rs.r[1];
  shared.room = 34560; shared.largest = 384;
  std::mt19937_64 rng(11);
  Outcome out;
  for (int op = 0; op < 12000; ++op) {           // the session's writes: new data, scattered, up to ~240 MiB live
    uint64_t len = 4096 * (1 + rng() % 64), off = (rng() % (kPages / 8)) * 4096;
    VolatileAdmit a = f.write(off, len, (unsigned) op);
    if (a == VolatileAdmit::Ok) ++out.ok; else if (a == VolatileAdmit::NoMemory) ++out.noMemory; else ++out.noSpace;
    if (f.p.inUse > budget - 2048) break;        // stay under the budget: a refusal here is the incident's kind
  }
  CHECK(f.nulls == 0 && f.consistent());
  for (auto &e : f.o.pages) if (e.first % 29 == 0) CHECK(f.read(e.first * 4096, 4096));
  out.owned = f.p.ownedPages;
  out.attempts = f.rs.attempts;
  return out;
}
static void partR(const Seed &seed) {
  Outcome old = incident(seed, true, false), neu = incident(seed, false, true), lazy = incident(seed, false, false);
  CHECK(old.noMemory > 0);                         // the model reproduces the incident's refusals
  CHECK(neu.noMemory == 0 && neu.noSpace == 0 && neu.owned == 65536);
  CHECK(lazy.noMemory == 0 && lazy.noSpace == 0);  // without a reservation the other ranges and smaller chunks carry it
  printf("  R incident: old pool %u ok / %u refused (no memory); reserved %u ok / %u refused; unreserved %u ok / %u refused\n",
         old.ok, old.noMemory, neu.ok, neu.noMemory, lazy.ok, lazy.noMemory);
}

// ---- F: fragmentation: no 2 MiB hole anywhere ----------------------------------------------------------------------------------
static void partF(const Seed &seed) {
  Fixture f(seed);
  const uint64_t budget = 32768;                   // 128 MiB
  setupPool(f.p, budget, budget);
  f.rs.r[0].room = 20000; f.rs.r[0].largest = 8;   // private data: holes of 32 KiB, not enough room for all
  f.rs.r[1].room = 0;     f.rs.r[1].largest = 0;   // shared data: exhausted
  f.rs.r[2].room = 1 << 20; f.rs.r[2].largest = 1; // default heap: single pages only
  refillNow(f.p, f.rs);
  CHECK(f.p.ownedPages == budget && f.p.prefilled());
  CHECK(f.p.fallbackChunks == f.p.chunkCount && f.p.recordGrowths > 0 && f.p.chunkCount > VolatilePool::capacityFor(budget));
  CHECK(f.rs.r[0].given > 0 && f.rs.r[2].given > 0 && f.rs.r[1].given == 0);
  std::mt19937_64 rng(3);
  unsigned ok = 0, refused = 0;
  for (int op = 0; op < 20000; ++op) {
    uint64_t len = 1 + rng() % 70000, off = rng() % (kLogical - len);
    if (f.p.inUse + volatileNeed(f.d, off, len) > budget) break;
    VolatileAdmit a = f.write(off, len, (unsigned) op);
    if (a == VolatileAdmit::Ok) ++ok; else ++refused;
    if (op % 97 == 0) CHECK(f.read(off, len));
  }
  CHECK(refused == 0 && ok > 100 && f.nulls == 0 && f.consistent() && f.p.waits == 0);
  // Past the budget: refused with no space, nothing changed.
  {
    uint64_t before = f.p.inUse, off = 0;
    while (f.p.inUse + volatileNeed(f.d, off, 4 << 20) <= budget) off += 4 << 20;
    CHECK(f.write(off, 4 << 20, 1) == VolatileAdmit::NoSpace && f.p.inUse == before && f.read(off, 4 << 20));
  }
  for (auto &e : f.o.pages) if (e.first % 7 == 0) CHECK(f.read(e.first * 4096, 4096));
  printf("  F fragmentation: %llu chunks (%llu fallbacks, %llu from private 32 KiB holes, %llu single pages), %llu record growths, "
         "%u writes ok, 0 refused under the budget\n", (unsigned long long) f.p.chunkCount, (unsigned long long) f.p.fallbackChunks,
         (unsigned long long) f.rs.r[0].given, (unsigned long long) f.rs.r[2].given, (unsigned long long) f.p.recordGrowths, ok);
  // Release: every chunk back to its own range, with its own size (Ranges::free checks both).
  uint64_t given = f.rs.r[0].given + f.rs.r[2].given;
  struct Free { Ranges &rs; void operator()(uint8_t *c, uint32_t pages, uint32_t kind) { rs.free(c, pages, kind); } } fr{f.rs};
  f.p.release(fr);
  CHECK(!f.rs.badFree && f.rs.r[0].freed + f.rs.r[2].freed == given && f.rs.r[0].live.empty() && f.rs.r[2].live.empty());
}

// ---- threads: the kernel's arrangement --------------------------------------------------------------------------------------------
struct Disk {
  std::mutex gate;
  std::condition_variable spare, refill, backoff;
  VolatilePool pool; Ranges rs;
  VolatileDisk d; std::vector<uint8_t **> dir;
  bool stop = false;
  Disk() { d.bytes = kLogical; dir.assign(VolatileDisk::entries(kLogical), nullptr); d.dir = dir.data(); }
  ~Disk() {
    struct Free { Ranges &rs; void operator()(uint8_t *c, uint32_t pages, uint32_t kind) { rs.free(c, pages, kind); } } f{rs};
    pool.release(f);
    free(pool.chunks);
  }
};
static void refillThread(Disk &k) {
  std::unique_lock<std::mutex> l(k.gate);
  struct Env {
    Disk &k; std::unique_lock<std::mutex> &l;
    bool stopping() { return k.stop; }
    void idle() { k.refill.wait_for(l, std::chrono::milliseconds(200)); }
    void unlock() { l.unlock(); }
    void lock() { l.lock(); }
    uint32_t kinds() { return k.rs.kinds; }
    uint8_t *allocChunk(uint32_t pages, uint32_t kind) { return k.rs.alloc(pages, kind); }
    void freeChunk(uint8_t *c, uint32_t pages, uint32_t kind) { k.rs.free(c, pages, kind); }
    VolatileChunk *allocRecords(uint64_t n) { return (VolatileChunk *) calloc(n, sizeof(VolatileChunk)); }
    void freeRecords(VolatileChunk *r, uint64_t) { free(r); }
    void wakeWriters() { k.spare.notify_all(); }
    void backoff(uint32_t ms) { k.backoff.wait_for(l, std::chrono::milliseconds(ms)); }   // writers never notify this one
  } env{k, l};
  volatileRefill(k.pool, env);
}
static void stopRefill(Disk &k, std::thread &t) {
  { std::lock_guard<std::mutex> g(k.gate); k.stop = true; }
  k.refill.notify_all(); k.backoff.notify_all();
  t.join();
}
struct Writer {                                   // a write in the kernel's shape: admit with a deadline, then the I/O
  static VolatileAdmit write(Disk &k, const Seed &seed, uint64_t off, uint64_t len, uint8_t fill, int deadlineMs, bool &bad) {
    std::unique_lock<std::mutex> l(k.gate);
    struct Env {
      Disk &k; std::unique_lock<std::mutex> &l; uint64_t off, len; std::chrono::steady_clock::time_point deadline;
      uint64_t need() { return volatileNeed(k.d, off, len); }
      void wakeRefill() { k.refill.notify_one(); }
      bool sleep() { return k.spare.wait_until(l, deadline) == std::cv_status::no_timeout || std::chrono::steady_clock::now() < deadline; }
    } env{k, l, off, len, std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs)};
    VolatileAdmit a = volatileAdmit(k.pool, env);
    if (a != VolatileAdmit::Ok) return a;
    std::vector<uint8_t> io(len, fill); Buffer b{io}; bool noMem = false;
    struct Ops { VolatilePool &p; uint8_t *allocate() { return p.take(); } } ops{k.pool};
    if (volatileDiskIO(seed.disk, k.d, true, off, len, b, ops, noMem) != len || noMem) bad = true;
    return a;
  }
};

// ---- P: nothing to be had, writers waiting: a bounded number of attempts, not a busy loop ---------------------------------------
static void partP(const Seed &seed) {
  Disk k;
  setupPool(k.pool, 65536, 0);
  for (auto &r : k.rs.r) { r.room = 0; r.largest = 0; }
  std::thread t(refillThread, std::ref(k));
  std::atomic<bool> bad{false};
  std::atomic<unsigned> refused{0};
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> ws;
  for (int w = 0; w < 4; ++w) ws.emplace_back([&, w] {
    bool b = false;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500))
      if (Writer::write(k, seed, (uint64_t) w << 30, 65536, 1, 50, b) == VolatileAdmit::NoMemory) ++refused;
    if (b) bad = true;
  });
  for (auto &w : ws) w.join();
  stopRefill(k, t);
  // Each exhausted round tries 5 sizes x 3 kinds = 15; backoff 10, 20, 40 ... 1000 ms: about 8 rounds in 1.5 s. The old loop
  // (backoff woken by every waiting writer) made tens of thousands.
  const uint64_t attempts = k.rs.attempts;
  CHECK(!bad && refused > 0 && attempts < 400 && k.pool.exhaustedRounds < 30);
  printf("  P nothing to be had: %llu allocation attempts in 1.5 s (%llu rounds), %u writes refused (no memory)\n",
         (unsigned long long) attempts, (unsigned long long) k.pool.exhaustedRounds, refused.load());
}

// ---- B: bursts, no reservation, a slow provider ----------------------------------------------------------------------------------
static void partB(const Seed &seed) {
  Disk k;
  setupPool(k.pool, 131072, 0);                   // 512 MiB budget, nothing reserved
  for (auto &r : k.rs.r) { r.room = 1 << 20; r.largest = 1 << 20; }
  k.rs.stallMs = 2;                                // 2 ms per chunk: 1 GiB/s at 2 MiB, much less at fallback sizes
  std::thread t(refillThread, std::ref(k));
  std::atomic<bool> bad{false};
  std::atomic<unsigned> refused{0}, ok{0};
  std::vector<std::thread> ws;
  for (int w = 0; w < 4; ++w) ws.emplace_back([&, w] {
    bool b = false;
    for (int burst = 0; burst < 6; ++burst) {
      for (int i = 0; i < 20; ++i) {               // 20 MiB per burst and writer (480 MiB of the 512), as fast as it goes
        uint64_t off = ((uint64_t) w << 31) + ((uint64_t) burst * 20 + i) * (1 << 20);
        if (Writer::write(k, seed, off, 1 << 20, (uint8_t) (w + burst), 10000, b) == VolatileAdmit::Ok) ++ok; else ++refused;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (b) bad = true;
  });
  for (auto &w : ws) w.join();
  stopRefill(k, t);
  CHECK(!bad && refused == 0 && ok == 4 * 6 * 20);
  CHECK(k.pool.waits == 0 || k.pool.reservePages > 16384);   // a wait deepened the reserve
  // Every byte reads back.
  bool readBad = false;
  for (int w = 0; w < 4 && !readBad; ++w)
    for (int burst = 0; burst < 6 && !readBad; ++burst)
      for (int i = 0; i < 20; i += 3) {
        uint64_t off = ((uint64_t) w << 31) + ((uint64_t) burst * 20 + i) * (1 << 20);
        std::vector<uint8_t> out(1 << 20); Buffer b{out}; bool noMem = false;
        struct Ops { uint8_t *allocate() { return nullptr; } } ops;
        if (volatileDiskIO(seed.disk, k.d, false, off, out.size(), b, ops, noMem) != out.size() ||
            out != std::vector<uint8_t>(1 << 20, (uint8_t) (w + burst))) { readBad = true; break; }
      }
  CHECK(!readBad);
  printf("  B bursts: %u writes of 1 MiB ok, %u refused, %llu waits, reserve now %llu MiB, %llu MiB held\n", ok.load(), refused.load(),
         (unsigned long long) k.pool.waits, (unsigned long long) k.pool.reservePages / 256, (unsigned long long) k.pool.ownedPages / 256);
}

// ---- U: unmap and the reads after it ----------------------------------------------------------------------------------------------
static void partU(const Seed &seed) {
  Fixture f(seed);
  setupPool(f.p, 16384, 16384);
  for (auto &r : f.rs.r) { r.room = 1 << 20; r.largest = 1 << 20; }
  refillNow(f.p, f.rs);
  uint64_t seeded = seed.index[3], plain = seeded + 1;            // a page with seed content and one without
  CHECK(seed.disk.find(seeded) && !seed.disk.find(plain));
  CHECK(f.write(seeded * 4096 + 100, 50, 1) == VolatileAdmit::Ok && f.read(seeded * 4096, 8192));   // partial: seed around it
  CHECK(f.write(plain * 4096, 4096, 2) == VolatileAdmit::Ok && f.read(seeded * 4096, 8192));
  // Unmapped whole pages read as the seed's (or zeros), never as stale data or garbage.
  CHECK(f.unmap(seeded * 4096, 8192) == 2 && f.read(seeded * 4096, 8192));
  { std::vector<uint8_t> io(4096); Buffer b{io}; bool nm = false; struct Ops { uint8_t *allocate() { return nullptr; } } ops;
    CHECK(volatileDiskIO(seed.disk, f.d, false, plain * 4096, 4096, b, ops, nm) == 4096 && io == std::vector<uint8_t>(4096, 0));
    CHECK(volatileDiskIO(seed.disk, f.d, false, seeded * 4096, 4096, b, ops, nm) == 4096 && !memcmp(io.data(), seed.disk.find(seeded), 4096)); }
  // The freed pages are reused for other blocks: zeroed (and the seed copied) first, no alias with the unmapped blocks.
  uint64_t elsewhere = seed.index[7];
  CHECK(f.write(elsewhere * 4096 + 7, 10, 3) == VolatileAdmit::Ok && f.write((elsewhere + 1) * 4096 + 9, 5, 4) == VolatileAdmit::Ok);
  CHECK(f.p.freePages == 0 && f.read(elsewhere * 4096, 8192) && f.read(seeded * 4096, 8192));
  // A partial-page unmap keeps the page; a rewrite after unmap reads as the rewrite.
  CHECK(f.write(seeded * 4096, 4096, 5) == VolatileAdmit::Ok);
  CHECK(f.unmap(seeded * 4096 + 512, 3072) == 0 && f.read(seeded * 4096, 4096));
  // Unmap across never-written leaves, the disk's end, and nothing written: no effect, no crash.
  CHECK(f.unmap(4ULL << 30, 512ULL << 20) == 0 && f.unmap(kLogical - 8192, 8192) == 0);
  CHECK(f.write(kLogical - 4096, 4096, 6) == VolatileAdmit::Ok && f.unmap(kLogical - 6000, 6000) == 1 && f.read(kLogical - 8192, 8192));
  // Random writes, unmaps and reads against the oracle.
  std::mt19937_64 rng(77);
  bool bad = false;
  for (int op = 0; op < 20000 && !bad; ++op) {
    uint64_t len = 1 + rng() % 40000, off = (rng() % (256ULL << 20)) + (op & 1 ? 0 : (6ULL << 30));
    int what = (int) (rng() % 3);
    if (what == 0) { if (f.write(off, len, (unsigned) op) != VolatileAdmit::Ok) f.unmap(0, kLogical); }
    else if (what == 1) f.unmap(off & ~511ULL, (len + 511) & ~511ULL);
    else if (!f.read(off, len)) bad = true;
  }
  CHECK(!bad && f.nulls == 0 && f.consistent());
  for (auto &e : f.o.pages) CHECK(f.read(e.first * 4096, 4096));
  printf("  U unmap: %llu pages unmapped, %llu resident at the end, reads after unmap as the seed's\n",
         (unsigned long long) f.p.unmapped, (unsigned long long) f.d.resident / 4096);
}

int main() {
  Seed seed;
  partR(seed);
  partF(seed);
  partU(seed);
  partP(seed);
  partB(seed);
  printf("%s: %u checks, %u failures\n", failures ? "FAIL" : "PASS", checks.load(), failures.load());
  return failures ? 1 : 0;
}
