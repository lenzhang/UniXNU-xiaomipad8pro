// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano host test of the store guard and of flatten (PersistOverlay.h, DirectDisk.h) on a model store:
//   - guard: another seed's valid store is Kept, byte for byte, read-only or not; data-store-replace (1, or the store's seed tag) and
//     sheng-data-reset start it over; a tag of another seed does not; blank and damaged windows are initialised as before
//   - flatten: complete; cut by a power loss before every single write of the pass (and inside the header write, torn at many byte
//     offsets), each time followed by a reboot with the same seed (the disk reads exactly as before), a second flatten (completes, the
//     disk still the same), and a boot with ANOTHER seed (self-contained: the same disk again, the new seed not read)
//   - a self-contained store under another seed: resumed, replace does not touch it, writes work, sheng-data-reset starts over, a
//     different page count is Kept
//   - header compatibility: a version 1 header without the extension (every earlier kernel's) resumes; flags 0 encode byte for byte the
//     old header; a kernel without the extension (legacyCheck, the previous checkHeader) reads a flattened store as this seed's valid
//     store and, reading the seed for clear bits, sees the same disk
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../DirectDisk.h"
using namespace persist;

static unsigned failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Medium {                                   // write-through store: a write is durable when it returns (FUA)
  std::vector<uint8_t> blocks;
  uint64_t writes = 0, reads = 0, syncs = 0;
  uint64_t cutAt = ~0ULL;                         // the power goes at this write (0-based): it and every later one never land...
  int tearBytes = -1;                             // ...except, when >= 0, the first tearBytes bytes of the cut write
  explicit Medium(uint64_t n) : blocks(n * kBlock, 0xA5) {}       // a never-written window holds junk
  bool read(uint64_t b, uint32_t n, void *d) { ++reads; if ((b + n) * kBlock > blocks.size()) return false;
    memcpy(d, &blocks[b * kBlock], (size_t) n * kBlock); return true; }
  bool write(uint64_t b, uint32_t n, const void *s) {
    uint64_t k = writes++;
    if ((b + n) * kBlock > blocks.size()) return false;
    if (k >= cutAt) { if (k == cutAt && tearBytes >= 0) memcpy(&blocks[b * kBlock], s, (size_t) tearBytes); return false; }
    memcpy(&blocks[b * kBlock], s, (size_t) n * kBlock); return true; }
  bool sync() { ++syncs; return writes <= cutAt; }
  Medium fork() const { Medium m = *this; m.writes = m.reads = m.syncs = 0; return m; }   // the same blocks, counters at zero
};
struct Buffer {
  std::vector<uint8_t> &data;
  uint64_t read(uint64_t at, void *p, uint64_t n) { memcpy(p, &data[at], n); return n; }
  uint64_t copy(uint64_t at, const void *p, uint64_t n) { memcpy(&data[at], p, n); return n; }
};
struct Seed {                                      // a RamSparseDisk seed and its pages
  RamSparseDisk disk = {};
  std::vector<uint8_t *> pages;
  uint8_t digest[32];
  Seed(uint64_t n, unsigned salt) : pages(n, nullptr) {
    disk.bytes = n * 4096; disk.pages = pages.data();
    srand(salt);
    for (uint64_t i = 0; i < n; ++i) {
      bool nonzero = (i % 29 == (salt % 29)) || (i >= 200 && i < 420) || i == n - 1;   // scattered pages and one long run
      if (!nonzero) continue;
      pages[i] = (uint8_t *) malloc(4096);
      for (int k = 0; k < 4096; ++k) pages[i][k] = (uint8_t) (rand() | 1);
    }
    for (int k = 0; k < 32; ++k) digest[k] = (uint8_t) (salt * 37 + k);
  }
  ~Seed() { for (auto p : pages) free(p); }
  Seed(const Seed &) = delete;
};

// The disk as an overlay over a seed shows it: every page through directDiskIO.
static std::vector<uint8_t> image(const Seed &seed, Overlay &ov, Medium &m) {
  std::vector<uint8_t> out(seed.disk.bytes), stage(8 * kBlock);
  Buffer b{out}; bool err = false;
  bool ro = ov.readOnly; ov.readOnly = false;      // reads only; directDiskIO refuses a read-only overlay for writes alone
  uint64_t got = directDiskIO(seed.disk, ov, m, stage.data(), 8, false, 0, seed.disk.bytes, b, &err);
  ov.readOnly = ro;
  if (got != seed.disk.bytes || err) out.clear();
  return out;
}
static void writeAt(const Seed &seed, Overlay &ov, Medium &m, std::vector<uint8_t> &oracle, uint64_t offset, uint64_t length, unsigned salt) {
  std::vector<uint8_t> io(length), stage(8 * kBlock);
  for (uint64_t k = 0; k < length; ++k) io[k] = (uint8_t) (salt + k * 13);
  Buffer b{io}; bool err = false;
  CHECK(directDiskIO(seed.disk, ov, m, stage.data(), 8, true, offset, length, b, &err) == length && !err);
  memcpy(&oracle[offset], io.data(), length);
}
static std::vector<uint8_t> flat(const Seed &seed) {
  std::vector<uint8_t> out(seed.disk.bytes, 0);
  for (uint64_t i = 0; i < seed.pages.size(); ++i) if (seed.pages[i]) memcpy(&out[i * 4096], seed.pages[i], 4096);
  return out;
}
// The previous kernel's checkHeader (before the extension), for the compatibility checks.
static int legacyCheck(const uint8_t *block, uint64_t pages, const uint8_t seed[32]) {
  if (memcmp(block, "SHNGDATA", 8)) return 1;
  if (get32(block + 8) != 1 || get32(block + 12) != kHeaderBytes || get32(block + 60) != crc32(block, 60)) return 2;
  if (get64(block + 16) != pages || memcmp(block + 24, seed, 32)) return 3;
  return 0;
}

int main() {
  const uint64_t pages = 1500;
  Seed X(pages, 3), Y(pages, 5);
  const size_t bitmapBytes = bitmapBlocks(pages) * kBlock;
  std::vector<uint8_t> bits(bitmapBytes), bits2(bitmapBytes);

  // --- header compatibility: flags 0 is the old header byte for byte; an old header resumes ------------------------------------
  {
    uint8_t a[kBlock], b[kBlock] = {};
    encodeHeader(a, pages, X.digest, 7);
    memcpy(b, "SHNGDATA", 8); put32(b + 8, 1); put32(b + 12, 64); put64(b + 16, pages); memcpy(b + 24, X.digest, 32); put32(b + 56, 7);
    put32(b + 60, crc32(b, 60));                   // as tooling/persist/test-persist.py and every earlier kernel build it
    CHECK(!memcmp(a, b, kBlock));
    uint64_t g = 0; uint32_t f = 9;
    CHECK(checkHeader(b, pages, X.digest, &g, &f) == 0 && g == 7 && f == 0);
    CHECK(checkHeader(b, pages, Y.digest, &g) == 3 && checkHeader(b, pages + 1, X.digest, &g) == 3);
    Medium m(windowBlocks(pages)); memcpy(m.blocks.data(), b, kBlock); memset(&m.blocks[kBlock], 0, bitmapBytes);
    Overlay o; CHECK(o.open(m, pages, X.digest, false, bits.data()) == Open::Resumed && o.sameSeed && !o.selfContained && o.generation == 7);
    // an extension with a bad CRC, or left over after the first 64 bytes changed, is ignored
    encodeHeader(a, pages, X.digest, 7, kSelfContained, 5);
    CHECK(checkHeader(a, pages, Y.digest, &g, &f) == 0 && f == kSelfContained);
    a[100] ^= 1; CHECK(checkHeader(a, pages, Y.digest, &g, &f) == 3 && f == 0); a[100] ^= 1;
    put32(a + 56, 8); put32(a + 60, crc32(a, 60));  // a tool rewrote the version 1 part without clearing the extension
    CHECK(checkHeader(a, pages, X.digest, &g, &f) == 0 && f == 0 && checkHeader(a, pages, Y.digest, &g) == 3);
  }

  // --- the guard ---------------------------------------------------------------------------------------------------------------
  Medium base(windowBlocks(pages));
  std::vector<uint8_t> oracle = flat(X);
  {
    Overlay o; CHECK(o.open(base, pages, X.digest, false, bits.data()) == Open::Fresh && o.header == 1 && o.generation == 1 && !o.replaced);
    srand(11);
    for (int op = 0; op < 120; ++op) {             // the owner's changes: whole pages, parts of pages, runs, on seed and zero pages
      uint64_t len = 1 + (uint64_t) rand() % 20000, off = (uint64_t) rand() % (X.disk.bytes - len);
      if (op % 3 == 0) off &= ~4095ULL;
      writeAt(X, o, base, oracle, off, len, (unsigned) op);
    }
    CHECK(image(X, o, base) == oracle);
  }
  const uint64_t presentBefore = [&] { Overlay o; o.open(base, pages, X.digest, true, bits.data()); return o.present; }();
  CHECK(presentBefore > 0);
  {
    Medium m = base.fork(); Overlay o;
    CHECK(o.open(m, pages, Y.digest, false, bits.data()) == Open::Kept && o.header == 3 && !o.active && !o.sameSeed);
    CHECK(m.writes == 0 && m.blocks == base.blocks && !memcmp(o.storeSeed, X.digest, 32) && o.generation == 1);
    CHECK(o.open(m, pages, Y.digest, true, bits.data()) == Open::Kept && m.writes == 0);          // read-only too
    CHECK(o.open(m, pages, Y.digest, false, bits.data(), false, seedTag(Y.digest)) == Open::Kept && m.writes == 0);  // another tag
    uint8_t other[32]; memcpy(other, X.digest, 32); other[31] ^= 1;   // same tag, other seed: the tag names the store's seed
    CHECK(seedTag(other) == seedTag(X.digest));
    CHECK(o.open(m, pages, Y.digest, true, bits.data(), false, 1) == Open::Kept && m.writes == 0); // read-only never replaces
    uint64_t flattened = 0; CHECK(o.flatten(m, Y.disk, bits2.data(), 1, &flattened) == 0 && m.writes == 0);  // nothing to flatten
    // the next boot with the store's own seed resumes it, exactly as before
    Overlay r; CHECK(r.open(m, pages, X.digest, false, bits.data()) == Open::Resumed && r.present == presentBefore && image(X, r, m) == oracle);
  }
  for (uint32_t replace : {1u, seedTag(X.digest)}) {
    Medium m = base.fork(); Overlay o;
    CHECK(o.open(m, pages, Y.digest, false, bits.data(), false, replace) == Open::Fresh && o.replaced && o.generation == 2 && o.present == 0);
    CHECK(o.sameSeed && !memcmp(o.storeSeed, Y.digest, 32) && image(Y, o, m) == flat(Y));
    Overlay r; CHECK(r.open(m, pages, X.digest, false, bits.data()) == Open::Kept);              // now it is Y's
  }
  {
    Medium m = base.fork(); Overlay o;                    // sheng-data-reset: start over, this seed's store or another's
    CHECK(o.open(m, pages, Y.digest, false, bits.data(), true) == Open::Fresh && o.replaced && o.generation == 2);
    Medium n = base.fork(); Overlay p;
    CHECK(p.open(n, pages, X.digest, false, bits.data(), true) == Open::Fresh && p.replaced && p.present == 0);
    Medium q = base.fork(); Overlay r;                    // replace has no effect on this seed's own store
    CHECK(r.open(q, pages, X.digest, false, bits.data(), false, 1) == Open::Resumed && q.writes == 0 && r.present == presentBefore);
  }
  {
    Medium m(windowBlocks(pages)); Overlay o;      // blank: read-only reports it, read-write initialises it
    CHECK(o.open(m, pages, Y.digest, true, bits.data()) == Open::Blank && o.header == 1 && m.writes == 0);
    CHECK(o.open(m, pages, Y.digest, false, bits.data()) == Open::Fresh && o.header == 1 && !o.replaced && o.generation == 1);
    Medium d = base; d.blocks[20] ^= 1; Overlay p;  // damaged: initialised as before
    CHECK(p.open(d, pages, Y.digest, true, bits.data()) == Open::Blank && p.header == 2);
    CHECK(p.open(d, pages, Y.digest, false, bits.data()) == Open::Fresh && p.header == 2 && !p.replaced);
  }

  // --- flatten, complete ---------------------------------------------------------------------------------------------------------
  Medium flatDone = base;
  uint64_t totalWrites = 0, seedPagesMissing = 0;
  {
    Overlay o; CHECK(o.open(flatDone, pages, X.digest, false, bits.data()) == Open::Resumed);
    for (uint64_t i = 0; i < pages; ++i) seedPagesMissing += X.disk.find(i) && !o.has(i);
    CHECK(seedPagesMissing > 50);
    uint64_t before = flatDone.writes, written = 0;
    std::vector<uint8_t> stage(8 * kBlock);
    CHECK(o.flatten(flatDone, X.disk, stage.data(), 8, &written) == 1 && written == seedPagesMissing && o.selfContained);
    totalWrites = flatDone.writes - before;
    CHECK(o.flags == kSelfContained && o.flattened == written && image(X, o, flatDone) == oracle);
    CHECK(o.flatten(flatDone, X.disk, stage.data(), 8, &written) == 0 && written == 0);     // once is enough
    CHECK(legacyCheck(flatDone.blocks.data(), pages, X.digest) == 0);                       // an old kernel: this seed's store
    CHECK(legacyCheck(flatDone.blocks.data(), pages, Y.digest) == 3);                       // (and another seed's for any other)
    CHECK(!memcmp(flatDone.blocks.data(), base.blocks.data(), kHeaderBytes));              // first 64 bytes as they were
    Overlay legacy; CHECK(legacy.open(flatDone, pages, X.digest, true, bits2.data()) == Open::Resumed);
    legacy.selfContained = false;                  // an old kernel reads the seed for clear bits: the same disk
    CHECK(image(X, legacy, flatDone) == oracle);
    Overlay y; CHECK(y.open(flatDone, pages, Y.digest, false, bits2.data()) == Open::Resumed && y.selfContained && !y.sameSeed);
    CHECK(image(Y, y, flatDone) == oracle && !memcmp(y.storeSeed, X.digest, 32));
    Overlay z; CHECK(z.open(flatDone, pages, Y.digest, false, bits2.data(), false, 1) == Open::Resumed && z.selfContained);  // not replaced
  }

  // --- flatten cut by a power loss before each of its writes, then: same seed, flatten again, another seed -------------------------
  unsigned cuts = 0;
  for (uint64_t cut = 0; cut <= totalWrites; ++cut) {
    std::vector<int> tears = {-1};
    if (cut + 1 == totalWrites) tears = {-1, 0, 8, 60, 64, 72, 100, 124, 127, 128, 512, 4096};   // the header write itself
    for (int tear : tears) {
      Medium m = base.fork(); Overlay o; std::vector<uint8_t> stage(8 * kBlock); uint64_t written = 0;
      CHECK(o.open(m, pages, X.digest, false, bits.data()) == Open::Resumed);
      m.cutAt = m.writes + cut; m.tearBytes = tear;
      int r = o.flatten(m, X.disk, stage.data(), 8, &written);
      CHECK(cut == totalWrites ? r == 1 : r == -1);
      m.cutAt = ~0ULL; m.tearBytes = -1;           // the next boot
      Overlay a; CHECK(a.open(m, pages, X.digest, false, bits.data()) == Open::Resumed);
      bool done = a.selfContained;
      CHECK((cut == totalWrites || tear >= 128) ? done : tear <= 64 ? !done : true);   // a tear inside the extension: either
      CHECK(image(X, a, m) == oracle);
      Overlay y; Open oy = y.open(m, pages, Y.digest, false, bits2.data());
      CHECK(oy == (done ? Open::Resumed : Open::Kept));
      if (done) CHECK(image(Y, y, m) == oracle);
      CHECK(a.flatten(m, X.disk, stage.data(), 8, &written) == (done ? 0 : 1) && a.selfContained);
      CHECK(image(X, a, m) == oracle);
      Overlay b; CHECK(b.open(m, pages, Y.digest, false, bits2.data()) == Open::Resumed && b.selfContained && image(Y, b, m) == oracle);
      ++cuts;
    }
  }
  CHECK(cuts > 40);

  // --- a self-contained store in use under another seed ----------------------------------------------------------------------------
  {
    Medium m = flatDone; Overlay o; std::vector<uint8_t> o2 = oracle;
    CHECK(o.open(m, pages, Y.digest, false, bits.data()) == Open::Resumed && o.selfContained);
    srand(5);
    for (int op = 0; op < 60; ++op) {
      uint64_t len = 1 + (uint64_t) rand() % 9000, off = (uint64_t) rand() % (Y.disk.bytes - len);
      writeAt(Y, o, m, o2, off, len, 200u + (unsigned) op);
    }
    CHECK(image(Y, o, m) == o2);
    Overlay again; CHECK(again.open(m, pages, X.digest, false, bits2.data()) == Open::Resumed && image(X, again, m) == o2);
    uint64_t written = 1; std::vector<uint8_t> stage(8 * kBlock);
    CHECK(again.flatten(m, X.disk, stage.data(), 8, &written) == 0 && written == 0);
    Overlay size; CHECK(size.open(m, pages + 1, Y.digest, false, bits2.data()) == Open::Kept);  // another size: not this disk
    Overlay reset; CHECK(reset.open(m, pages, Y.digest, false, bits2.data(), true) == Open::Fresh && !reset.selfContained);
    CHECK(image(Y, reset, m) == flat(Y));
  }

  // --- flatten of a fresh store, and with runs that cross a bitmap block -------------------------------------------------------------
  {
    const uint64_t big = 32768 + 300;
    Seed S(big, 9);
    std::vector<uint8_t> b3(bitmapBlocks(big) * kBlock), stage(64 * kBlock);
    Medium m(windowBlocks(big)); Overlay o;
    CHECK(o.open(m, big, S.digest, false, b3.data()) == Open::Fresh);
    uint64_t written = 0, nonzero = 0;
    for (uint64_t i = 0; i < big; ++i) nonzero += S.disk.find(i) != nullptr;
    // seed pages around the first bitmap block boundary, so a run spans two bitmap blocks
    for (uint64_t i = 32768 - 20; i < 32768 + 20; ++i) if (!S.pages[i]) { S.pages[i] = (uint8_t *) calloc(1, 4096); S.pages[i][0] = 1; ++nonzero; }
    CHECK(o.flatten(m, S.disk, stage.data(), 64, &written) == 1 && written == nonzero && o.present == nonzero);
    Seed T(big, 2);
    Overlay r; std::vector<uint8_t> b4(b3.size());
    CHECK(r.open(m, big, T.digest, true, b4.data()) == Open::Resumed && r.selfContained && r.present == nonzero);
    std::vector<uint8_t> out(T.disk.bytes), st(8 * kBlock); Buffer bb{out}; bool err = false;
    r.readOnly = false;
    CHECK(directDiskIO(T.disk, r, m, st.data(), 8, false, 0, T.disk.bytes, bb, &err) == T.disk.bytes && out == flat(S));
  }

  printf("%s: %u checks, %u failures (flatten: %llu writes, %u power cuts)\n", failures ? "FAIL" : "PASS", checks, failures,
         (unsigned long long) totalWrites, cuts);
  return failures ? 1 : 0;
}
