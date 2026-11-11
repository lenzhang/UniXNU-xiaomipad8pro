// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano host test of VolatileDisk.h: the volatile disk over a large compact seed (SeedDisk.h) against a sparse oracle: random
// reads and writes (whole pages, parts, page-crossing runs, both ends of the disk), the seed never modified, memory only for the pages
// written, allocation failures (a page, a leaf) reported with the bytes done, short buffer copies, out-of-range requests, release.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include "../SeedDisk.h"
#include "../VolatileDisk.h"

static unsigned failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Ops {
  long budget = -1; unsigned blocks = 0;          // budget: allocations left (-1 unlimited)
  uint8_t *allocate() { if (budget == 0) return nullptr; if (budget > 0) --budget; ++blocks; return (uint8_t *) calloc(1, 4096); }
};
struct Buffer {
  std::vector<uint8_t> &data; uint64_t limit = ~0ULL;
  uint64_t read(uint64_t at, void *p, uint64_t n) { if (at + n > limit) n = at > limit ? 0 : limit - at; memcpy(p, &data[at], n); return n; }
  uint64_t copy(uint64_t at, const void *p, uint64_t n) { if (at + n > limit) n = at > limit ? 0 : limit - at; memcpy(&data[at], p, n); return n; }
};
struct Release { unsigned freed = 0; void operator()(uint8_t *p) { ++freed; free(p); } };

int main() {
  const uint64_t logical = 12ULL << 30, pages = logical / 4096;          // the installed Data disk's size: 12 GiB, 3145728 pages
  SeedDisk seed; seed.bytes = logical;
  std::vector<uint64_t> index; std::vector<uint8_t *> page;
  for (uint64_t i : {0ULL, 1ULL, 2ULL, 511ULL, 512ULL, 513ULL, 70000ULL, pages / 2, pages - 2, pages - 1}) {
    index.push_back(i); uint8_t *p = (uint8_t *) malloc(4096); memset(p, (int) (0x10 + index.size()), 4096); page.push_back(p);
  }
  seed.index = index.data(); seed.page = page.data(); seed.count = seed.capacity = index.size(); seed.resident = seed.count * 4096;
  std::vector<std::vector<uint8_t>> seedCopy;
  for (auto p : page) seedCopy.emplace_back(p, p + 4096);
  CHECK(VolatileDisk::entries(logical) == 6144);

  VolatileDisk d; d.bytes = logical;
  std::vector<uint8_t **> dir(VolatileDisk::entries(logical), nullptr); d.dir = dir.data();
  std::map<uint64_t, std::vector<uint8_t>> oracle;                       // pages that differ from the seed
  auto expect = [&](uint64_t at, uint64_t len) {
    std::vector<uint8_t> out(len);
    for (uint64_t k = 0; k < len;) {
      uint64_t a = at + k, i = a / 4096, n = std::min<uint64_t>(4096 - (a & 4095), len - k);
      auto it = oracle.find(i); const uint8_t *src = it != oracle.end() ? it->second.data() : seed.find(i);
      if (src) memcpy(&out[k], src + (a & 4095), n); else memset(&out[k], 0, n);
      k += n;
    }
    return out;
  };
  Ops ops; std::vector<uint8_t> io(300000); bool oom = false; srand(17);
  for (int op = 0; op < 3000; ++op) {
    uint64_t len = 1 + (uint64_t) rand() % (op % 9 == 0 ? 250000 : 9000), where;
    switch (rand() % 5) {
      case 0: where = 0; break;
      case 1: where = logical - 300000; break;
      case 2: where = (pages / 2 - 40) * 4096; break;
      case 3: where = 500 * 4096; break;
      default: where = ((uint64_t) rand() * 7919 % (pages - 100)) * 4096;
    }
    uint64_t offset = where + (rand() & 1 ? 0 : (uint64_t) rand() % 8192);
    if (offset + len > logical) len = logical - offset;
    Buffer b{io};
    if (rand() & 1) {
      for (uint64_t k = 0; k < len; ++k) io[k] = (uint8_t) rand();
      CHECK(volatileDiskIO(seed, d, true, offset, len, b, ops, oom) == len && !oom);
      for (uint64_t k = 0; k < len;) {
        uint64_t a = offset + k, i = a / 4096, n = std::min<uint64_t>(4096 - (a & 4095), len - k);
        if (!oracle.count(i)) { const uint8_t *s = seed.find(i); oracle[i] = s ? std::vector<uint8_t>(s, s + 4096) : std::vector<uint8_t>(4096, 0); }
        memcpy(&oracle[i][a & 4095], &io[k], n);
        k += n;
      }
    } else {
      memset(io.data(), 0xEE, len);
      CHECK(volatileDiskIO(seed, d, false, offset, len, b, ops, oom) == len && !oom);
      CHECK(std::vector<uint8_t>(io.begin(), io.begin() + len) == expect(offset, len));
    }
  }
  for (size_t k = 0; k < page.size(); ++k) CHECK(!memcmp(page[k], seedCopy[k].data(), 4096));   // the seed is never written
  CHECK(d.resident == oracle.size() * 4096 && ops.blocks == oracle.size() + d.leaves);       // memory only for written pages
  { Buffer b{io};                                   // out of range and empty requests do nothing
    CHECK(volatileDiskIO(seed, d, false, logical, 1, b, ops, oom) == 0);
    CHECK(volatileDiskIO(seed, d, true, logical - 1, 2, b, ops, oom) == 0);
    CHECK(volatileDiskIO(seed, d, true, 0, 0, b, ops, oom) == 0); }
  { // allocation failures: a fresh leaf, then a page; the bytes before are done, the rest untouched
    uint64_t at = (pages - 5000) * 4096;            // a leaf nobody wrote yet
    while (d.dir[(at / 4096) / VolatileDisk::kLeafPages]) at -= 4096 * VolatileDisk::kLeafPages;
    Buffer b{io}; for (int k = 0; k < 8192; ++k) io[k] = 0x44;
    ops.budget = 0;
    CHECK(volatileDiskIO(seed, d, true, at, 8192, b, ops, oom) == 0 && oom);
    ops.budget = 1;                                 // the leaf, then no page
    CHECK(volatileDiskIO(seed, d, true, at, 8192, b, ops, oom) == 0 && oom);
    ops.budget = 1;                                 // the leaf is there now: one page, not the second
    CHECK(volatileDiskIO(seed, d, true, at, 8192, b, ops, oom) == 4096 && oom);
    ops.budget = -1;
    memset(io.data(), 0, 8192);
    CHECK(volatileDiskIO(seed, d, false, at, 8192, b, ops, oom) == 8192 && !oom);
    for (int k = 0; k < 8192; ++k) if (io[k] != (k < 4096 ? 0x44 : 0)) { CHECK(false); break; } }
  { // a short buffer copy on a write into a seed page: the page keeps the seed's bytes past the short copy
    Buffer b{io, 100}; memset(io.data(), 0x99, 4096);
    uint64_t at = 513 * 4096;
    if (!d.find(513)) {
      CHECK(volatileDiskIO(seed, d, true, at, 4096, b, ops, oom) == 100 && !oom);
      CHECK(d.find(513)[99] == 0x99 && d.find(513)[100] == seed.find(513)[100]);
    } }
  Release release; uint64_t leaves = d.leaves, held = d.resident / 4096;
  volatileDiskRelease(d, release);
  CHECK(release.freed == leaves + held && d.resident == 0 && d.leaves == 0);
  for (auto e : dir) CHECK(e == nullptr);
  for (auto p : page) free(p);
  printf("%s: %u checks, %u failures (%zu pages written, %llu leaves)\n", failures ? "FAIL" : "PASS", checks, failures, oracle.size(),
         (unsigned long long) leaves);
  return failures ? 1 : 0;
}
