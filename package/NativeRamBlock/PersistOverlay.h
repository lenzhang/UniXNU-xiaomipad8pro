// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Persistent overlay over a seeded sparse disk. The seed (the packed RAM disk the loader provides) stays the immutable base; the
// pages a running system changes are stored in a window of 4096-byte blocks on some other medium and put back on the next boot.
//
// Window layout (block = 4096 bytes, all integers little endian):
//   block 0                 header: bytes 0..63 "SHNGDATA", version 1, header bytes 64, page count, SHA-256 of the seed, generation
//                           (32 bit), crc32 of bytes 0..59; bytes 64..127 the extension (UniXNU-piano), all zero when absent:
//                           "SHNGEXT1", flags (32 bit), 0 (32 bit), pages the flatten wrote (64 bit), zeros, crc32 of bytes 0..59
//                           and 64..123 (extensionCrc)
//   blocks 1 .. B           presence bitmap, one bit per page of the disk, B = ceil(pages / 32768)
//   blocks B+1 .. B+pages   page i at B+1+i, valid only when its bit is set
// Ops write through: write() returns only when the blocks are on the medium (the UFS store uses WRITE(10) with FUA). A run of pages
// is written first and the bitmap blocks that gained a bit after it, both before the caller's write completes, so a bit never
// promises a page that did not reach the medium, and a page the system was told is written is never lost to a crash. An
// overwrite of a page whose bit is set changes only that page.
//
// Seed binding. A store is made against one seed (its digest in the header): a clear bit means "the seed's page". A header whose page
// count or seed digest differs is another seed's store (checkHeader 3). It is never applied, and (UniXNU-piano, the store guard) it is
// also KEPT: open() leaves it untouched and reports Kept, and the caller runs the disk volatile for that boot, unless the caller asks
// to replace it (data-store-replace) or to start over (sheng-data-reset). A blank (1) or damaged (2) window is initialised as before.
//
// Self-contained stores (kSelfContained, written by flatten()): every page the seed held is present in the window, so a clear bit
// means a zero page and the store no longer needs its seed: it is used with ANY seed of the same size (checkHeader 0). The flag lives
// in the extension, outside the first 64 bytes, which flatten() rewrites unchanged: a kernel without the extension sees a valid
// version 1 store for the seed it was flattened with (and reads exactly the same disk from it), and a torn header write can only lose
// the flag, never the store. The extension's CRC covers bytes 0..59 too, so it is void once anything rewrites the version 1 part.
//
// Ops: bool read(uint64_t block, uint32_t count, void *dst); bool write(uint64_t block, uint32_t count, const void *src);
//      bool sync();   (blocks are relative to the window; sync is only used while the header is made)
#include <stdint.h>
#include <string.h>

namespace persist {
constexpr uint32_t kBlock = 4096;
constexpr uint32_t kHeaderBytes = 64;
constexpr uint32_t kSelfContained = 1;   // extension flag: every seed page is in the window; a clear bit reads as zero

// zlib's crc32: crc32(b, n, crc32(a, m)) is the CRC of a followed by b.
inline uint32_t crc32 (const uint8_t *data, size_t bytes, uint32_t prior = 0)
{
  uint32_t crc = ~prior;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
// The extension's CRC: bytes 0..59 and 64..123, so it is void once the version 1 part changes. (Not bytes 0..123: a CRC over a block
// that ends in its own CRC is a constant, so bytes 0..63 would add nothing.)
inline uint32_t extensionCrc (const uint8_t *block) { return crc32 (block + 64, 60, crc32 (block, 60)); }
inline void put32 (uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t (v >> (8 * i)); }
inline void put64 (uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t (v >> (8 * i)); }
inline uint32_t get32 (const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | p[i]; return v; }
inline uint64_t get64 (const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }
// The first 4 bytes of a seed digest as one number: what the console prints as 8 hex digits and data-store-replace=0x... names.
inline uint32_t seedTag (const uint8_t seed[32]) { return uint32_t (seed[0]) << 24 | uint32_t (seed[1]) << 16 | uint32_t (seed[2]) << 8 | seed[3]; }

inline uint64_t bitmapBlocks (uint64_t pages) { return (pages + 8ULL * kBlock - 1) / (8ULL * kBlock); }
inline uint64_t windowBlocks (uint64_t pages) { return 1 + bitmapBlocks (pages) + pages; }
inline uint64_t pageBlock (uint64_t pages, uint64_t index) { return 1 + bitmapBlocks (pages) + index; }

// flags 0: a plain version 1 header (bytes 64.. zero), byte for byte what every earlier kernel wrote.
inline void encodeHeader (uint8_t block[kBlock], uint64_t pages, const uint8_t seed[32], uint64_t generation, uint32_t flags = 0,
                          uint64_t flattened = 0)
{
  memset (block, 0, kBlock);
  memcpy (block, "SHNGDATA", 8);
  put32 (block + 8, 1);
  put32 (block + 12, kHeaderBytes);
  put64 (block + 16, pages);
  memcpy (block + 24, seed, 32);
  put32 (block + 56, uint32_t (generation));
  // The CRC covers the first 60 bytes (the generation included); bytes 60..63 hold it.
  put32 (block + 60, crc32 (block, 60));
  if (!flags) return;
  memcpy (block + 64, "SHNGEXT1", 8);
  put32 (block + 72, flags);
  put64 (block + 80, flattened);
  put32 (block + 124, extensionCrc (block));
}

struct HeaderInfo {
  uint64_t pages = 0, generation = 0, flattened = 0;
  uint32_t flags = 0;              // 0 unless the extension is present and intact
  uint8_t seed[32] = {};
};
// 1 blank or foreign (no magic); 2 damaged (CRC, version, size); 0 a valid header, described in *info.
inline int parseHeader (const uint8_t block[kBlock], HeaderInfo *info)
{
  if (memcmp (block, "SHNGDATA", 8)) return 1;
  if (get32 (block + 8) != 1 || get32 (block + 12) != kHeaderBytes || get32 (block + 60) != crc32 (block, 60)) return 2;
  info->pages = get64 (block + 16);
  info->generation = get32 (block + 56);
  memcpy (info->seed, block + 24, 32);
  info->flags = 0;
  info->flattened = 0;
  if (!memcmp (block + 64, "SHNGEXT1", 8) && get32 (block + 124) == extensionCrc (block)) {
    info->flags = get32 (block + 72);
    info->flattened = get64 (block + 80);
  }
  return 0;
}
// 0 valid and usable with this seed (made for it, or self-contained and of this size); 1 blank or foreign (no magic); 2 damaged
// (CRC, version, size); 3 valid but another seed's (or another size's), not self-contained.
inline int checkHeader (const uint8_t block[kBlock], uint64_t pages, const uint8_t seed[32], uint64_t *generation, uint32_t *flags = nullptr)
{
  HeaderInfo info;
  int base = parseHeader (block, &info);
  if (base) return base;
  if (generation) *generation = info.generation;
  if (flags) *flags = info.flags;
  if (info.pages != pages) return 3;
  if (memcmp (info.seed, seed, 32) && !(info.flags & kSelfContained)) return 3;
  return 0;
}

// Resumed: a valid store (this seed's, or self-contained). Fresh: initialised now (blank, damaged, or replaced on request).
// Blank: read-only open of a window that is not a valid store. Kept: another seed's store, left untouched (the caller runs volatile).
enum class Open { Resumed, Fresh, Blank, Failed, Kept };

struct Overlay {
  uint64_t pages = 0, generation = 0, present = 0, flattened = 0;
  uint8_t *bitmap = nullptr;       // bitmapBlocks(pages) * kBlock bytes, owned by the caller
  bool readOnly = true, active = false;
  int header = -1;                 // checkHeader result at open
  uint32_t flags = 0;              // the header's extension flags
  bool selfContained = false;      // a clear bit reads as zero, not as the seed's page
  bool sameSeed = false;           // the header names this seed (and size)
  bool replaced = false;           // Fresh because a valid store of another seed (or this one) was replaced on request
  uint8_t storeSeed[32] = {};      // the seed digest in the header (zero when there is no valid header)

  bool has (uint64_t i) const { return bitmap[i >> 3] >> (i & 7) & 1; }

  // `ro`: never write; a stale or blank window is reported (Blank) and left alone. Otherwise a blank or damaged window is
  // initialised: bitmap first, header last, so a tear leaves no valid header. Another seed's valid store is Kept unless `discard`
  // (start over whatever the store holds: sheng-data-reset) or `replace` asks for it: 1 replaces any other seed's store, any other
  // nonzero value only the store whose seedTag it is (data-store-replace=0x<8 hex>).
  template <class Ops> Open open (Ops &ops, uint64_t pageCount, const uint8_t seed[32], bool ro, uint8_t *bits, bool discard = false,
                                  uint32_t replace = 0)
  {
    pages = pageCount; bitmap = bits; readOnly = ro; present = 0; active = false;
    flags = 0; flattened = 0; selfContained = sameSeed = replaced = false; generation = 0;
    memset (storeSeed, 0, sizeof storeSeed);
    uint64_t bb = bitmapBlocks (pages);
    uint8_t block[kBlock];
    if (!pages || !ops.read (0, 1, block)) return Open::Failed;
    HeaderInfo info;
    header = parseHeader (block, &info);
    if (!header) {
      generation = info.generation;
      flags = info.flags;
      flattened = info.flattened;
      memcpy (storeSeed, info.seed, 32);
      sameSeed = info.pages == pages && !memcmp (info.seed, seed, 32);
      header = checkHeader (block, pages, seed, nullptr);
      selfContained = header == 0 && (flags & kSelfContained);
    }
    const bool replaceIt = replace == 1 || (replace && replace == seedTag (storeSeed));
    const bool startOver = !ro && header != 1 && header != 2 && (discard || (header == 3 && replaceIt));
    if (header == 0 && !startOver) {
      for (uint64_t b = 0; b < bb; ++b) if (!ops.read (1 + b, 1, bitmap + b * kBlock)) return Open::Failed;
      // Bits past the last page must be clear, or the loader would read blocks outside the window.
      for (uint64_t i = pages; i < bb * 8ULL * kBlock; ++i) if (has (i)) return Open::Failed;
      for (uint64_t i = 0; i < pages; ++i) present += has (i);
      active = true;
      return Open::Resumed;
    }
    if (header == 3 && !startOver) {
      // Another seed's store: never applied, nothing written. Its bitmap is only read to report how many pages it holds (present),
      // so a later boot can show that the store is unchanged; then it is cleared again.
      bool counted = info.pages == pages;
      for (uint64_t b = 0; b < bb && counted; ++b) counted = ops.read (1 + b, 1, bitmap + b * kBlock);
      for (uint64_t i = 0; counted && i < pages; ++i) present += has (i);
      memset (bitmap, 0, bb * kBlock);
      return Open::Kept;
    }
    memset (bitmap, 0, bb * kBlock);
    if (ro) return Open::Blank;
    uint8_t zero[kBlock] = {};
    for (uint64_t b = 0; b < bb; ++b) if (!ops.write (1 + b, 1, zero)) return Open::Failed;
    if (!ops.sync ()) return Open::Failed;
    generation = (header == 3 || header == 0) ? generation + 1 : 1;
    replaced = startOver;
    encodeHeader (block, pages, seed, generation);
    if (!ops.write (0, 1, block) || !ops.sync ()) return Open::Failed;
    flags = 0; flattened = 0; selfContained = false; sameSeed = true;
    memcpy (storeSeed, seed, 32);
    active = true;
    return Open::Fresh;
  }

  // Calls load(index, page) for every present page, in order, reading runs of up to maxRun consecutive present pages with one
  // read into scratch (maxRun * kBlock bytes). Stops at the first failure.
  template <class Ops, class Load> bool forEachPresent (Ops &ops, Load &load, uint8_t *scratch, uint32_t maxRun)
  {
    if (!maxRun) return false;
    for (uint64_t i = 0; i < pages;) {
      if (!has (i)) { ++i; continue; }
      uint32_t n = 1;
      while (n < maxRun && i + n < pages && has (i + n)) ++n;
      if (!ops.read (pageBlock (pages, i), n, scratch)) return false;
      for (uint32_t k = 0; k < n; ++k) if (!load (i + k, scratch + (uint64_t) k * kBlock)) return false;
      i += n;
    }
    return true;
  }

  // Writes `count` consecutive pages starting at `index` (count * kBlock bytes at data), then the bitmap blocks that gained a bit.
  // On false the run may be partly on the medium but no bit names a page that is not; the caller must stop using the overlay.
  template <class Ops> bool writeRun (Ops &ops, uint64_t index, uint32_t count, const void *data)
  {
    if (readOnly || !active || !count || index >= pages || count > pages - index) return false;
    if (!ops.write (pageBlock (pages, index), count, data)) return false;
    uint64_t firstBlock = ~0ULL, lastBlock = 0;
    for (uint64_t i = index; i < index + count; ++i) {
      if (has (i)) continue;
      uint64_t b = i / (8ULL * kBlock);
      if (firstBlock == ~0ULL) firstBlock = b;
      lastBlock = b;
    }
    // Set the bits only after the pages are durable; the bitmap blocks follow before this call returns.
    for (uint64_t i = index; i < index + count; ++i)
      if (!has (i)) { bitmap[i >> 3] |= uint8_t (1 << (i & 7)); ++present; }
    if (firstBlock == ~0ULL) return true;
    for (uint64_t b = firstBlock; b <= lastBlock; ++b) if (!ops.write (1 + b, 1, bitmap + b * kBlock)) return false;
    return true;
  }
  template <class Ops> bool writePage (Ops &ops, uint64_t index, const void *page) { return writeRun (ops, index, 1, page); }

  // data-store-flatten: makes this seed's store self-contained. Every page the seed holds (seed.find(i) non-null: its nonzero pages)
  // whose bit is clear is written into the window with its bit, in runs of up to stagePages through writeRun (pages, then the bitmap
  // blocks that gained bits, each run durable before the next), so the disk reads the same at every point of the pass. When every
  // seed page is present the header is rewritten with kSelfContained, last; its first 64 bytes are the ones already on the medium.
  // A cut anywhere leaves a valid store of this seed with more pages present; the next flatten skips them and finishes.
  // Returns 1 done (the store is self-contained, *written pages written now), 0 nothing to do (not this seed's store, read-only, or
  // already self-contained: *written 0), -1 a store error (stop writing; the store stays valid for this seed).
  template <class Ops, class Seed> int flatten (Ops &ops, const Seed &seed, uint8_t *stage, uint32_t stagePages, uint64_t *written)
  {
    *written = 0;
    if (readOnly || !active || !sameSeed || selfContained || !stagePages) return 0;
    for (uint64_t i = 0; i < pages;) {
      if (has (i) || !seed.find (i)) { ++i; continue; }
      uint32_t n = 0;
      while (n < stagePages && i + n < pages && !has (i + n)) {
        const uint8_t *page = seed.find (i + n);
        if (!page) break;
        memcpy (stage + (uint64_t) n * kBlock, page, kBlock);
        ++n;
      }
      if (!writeRun (ops, i, n, stage)) return -1;
      *written += n;
      i += n;
    }
    for (uint64_t i = 0; i < pages; ++i) if (!has (i) && seed.find (i)) return -1;   // never claim what is not there
    uint8_t now[kBlock], block[kBlock];
    if (!ops.read (0, 1, now)) return -1;
    encodeHeader (block, pages, storeSeed, generation);
    if (memcmp (now, block, kHeaderBytes)) return -1;        // the header on the medium is not the one this store was opened with
    encodeHeader (block, pages, storeSeed, generation, flags | kSelfContained, flattened + *written);
    if (!ops.write (0, 1, block) || !ops.sync ()) return -1;
    flags |= kSelfContained;
    flattened += *written;
    selfContained = true;
    return 1;
  }
};
}  // namespace persist
