// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <IOKit/IOBSD.h>
#include <sys/param.h>
#include <libkern/crypto/sha2.h>
#include <mach/machine.h>
#include <machine/machine_routines.h>
#include <vm/vm_kern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <libkern/libkern.h>
#include "SparseDisk.h"
#include "PersistOverlay.h"
#include "DirectDisk.h"
#include "VolatileDisk.h"
#include "VolatilePool.h"
#include <IOKit/storage/IOStorage.h>
#include "PowerHold.h"
#include <PianoPowerContract.h>   // UniXNU-piano: the Data disk's part of a system sleep (native-ufs-suspend=1, PowerHold.h)

extern "C" {
extern unsigned long gPhysBase, gPhysSize;
int native_ram_block_volume_device(const char *, dev_t *, char *, size_t);
volatile uint32_t native_ram_block_ready = 0;
volatile uint64_t native_ram_block_bytes = 0;
volatile uint64_t native_ram_block_reads = 0;
volatile uint64_t native_ram_block_writes = 0;
volatile uint64_t native_ram_block_failures = 0;
volatile uint64_t native_ram_block_resident_bytes = 0;
volatile uint32_t native_ram_block_sparse = 0;
uint8_t native_ram_block_seed_sha256[32];
// Optional persistence of the changed pages through a board-provided store window (see PersistOverlay.h). All weak: a build
// without the provider keeps the volatile disk.
unsigned native_ufs_store_mode(void);
int native_ufs_store_open(uint64_t, uint64_t *, const char **);
int native_ufs_store_read(uint64_t, uint32_t, void *);
int native_ufs_store_write(uint64_t, uint32_t, const void *);
int native_ufs_store_sync(void);
unsigned native_ufs_store_gpt_check(void);
// UniXNU-piano (NativeUfs, native-ufs-suspend=1): the Data disk's power parent (nullptr: no part in a system sleep), and the veto a
// failed store sync sends it (its suspend then leaves the link up).
IOService *native_ufs_power_parent(void);
void native_ufs_power_veto(const char *who);
// Diagnostics. state: 0 off, 1 blank (read-only probe), 2 resumed, 3 fresh, 4 refused/failed, 5 degraded after an I/O error,
// 6 resumed read-only (probe), 7 kept (another seed's store, untouched; Data volatile). header: persist::checkHeader result.
// flags: the header's extension flags (1 self-contained); store_tag: the first 4 bytes of the store's seed digest;
// flattened: pages data-store-flatten wrote this boot; volatile_bytes: RAM held by the volatile disk's written pages.
volatile uint32_t native_data_persist_state = 0, native_data_persist_header = 9, native_data_persist_gpt = 0xff, native_data_persist_gpt_after = 0xff,
                  native_data_persist_flags = 0, native_data_persist_store_tag = 0;
volatile uint64_t native_data_persist_pages = 0, native_data_persist_loaded = 0, native_data_persist_writes = 0,
                  native_data_persist_syncs = 0, native_data_persist_errors = 0, native_data_persist_first_lba = 0,
                  native_data_persist_present = 0, native_data_persist_flattened = 0, native_ram_block_volatile_bytes = 0;
}

// UniXNU-piano (2026-11-11, docs/forensics-2026-11-11.md): native-ram-trace=1 prints a short console line before and after each step
// of the Data store's set-up: the waits for IOBSD and for NativeUfs's root layout, the GPT check, the window, the header and bitmap
// I/O, the cache syncs, the disk's registration. Each line carries the milliseconds since the publish thread started, so a set-up
// that stops names the step it stopped in and how long the steps before it took. Without the argument nothing is printed and the
// set-up is unchanged (the same I/O in the same order; the trace only observes it).
namespace {
bool gStoreTrace;
uint64_t gStoreTraceStart;
__attribute__((format(printf, 1, 2))) void storeTrace(const char *format, ...)
{
  if (!gStoreTrace) return;
  char line[200];
  va_list ap;
  va_start(ap, format);
  vsnprintf(line, sizeof line, format, ap);
  va_end(ap);
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time() - gStoreTraceStart, &ns);
  IOLog("NativeRamBlock: store +%llu ms: %s\n", ns / 1000000, line);
}
// One store call of the set-up, before (done false) or after it (done true, ok its result). The header block is described once read;
// the bitmap gets a line at its first block, every 32nd and its last; the page area its first call; every failure its own line.
void storeTraceIo(char op, uint64_t block, uint32_t count, uint64_t bitmapBlocks, bool done, bool ok, const void *data)
{
  if (!gStoreTrace) return;
  const char *what = op == 'r' ? "read" : "write";
  if (done && !ok) {
    storeTrace("%s of block %llu (%u blocks) FAILED", what, block, count);
    return;
  }
  if (block == 0) {
    if (!done) {
      storeTrace("%s the header (block 0)", op == 'r' ? "reading" : "writing");
    } else if (op == 'w') {
      storeTrace("header written");
    } else {
      persist::HeaderInfo info;
      const int parsed = data ? persist::parseHeader(static_cast<const uint8_t *>(data), &info) : 1;
      if (parsed == 1) storeTrace("header read: no store header (blank or foreign)");
      else if (parsed == 2) storeTrace("header read: damaged (version, size or CRC)");
      else
        storeTrace("header read: SHNGDATA v1, %llu pages, seed %08x, generation %llu, flags %u", info.pages, persist::seedTag(info.seed),
                   info.generation, info.flags);
    }
    return;
  }
  if (block <= bitmapBlocks) {
    if (!done && block == 1)
      storeTrace(op == 'r' ? "reading the bitmap (%llu blocks)" : "clearing the bitmap (%llu blocks, one FUA write each)", bitmapBlocks);
    else if (done && block == bitmapBlocks)
      storeTrace(op == 'r' ? "bitmap read (%llu blocks)" : "bitmap cleared (%llu blocks)", bitmapBlocks);
    else if (done && block % 32 == 0)
      storeTrace("bitmap block %llu of %llu %s", block, bitmapBlocks, op == 'r' ? "read" : "written");
    return;
  }
  static bool pagesSeen;
  if (!pagesSeen) {
    if (done) pagesSeen = true;
    storeTrace("%s page area block %llu (%u blocks)%s", what, block, count, done ? ": done (later page calls get no line)" : "");
  }
}
}  // namespace

class NativeRamBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeRamBlock)
  IOLock *gate = nullptr;
  uint8_t *memory = nullptr;
  uint64_t capacity = 0;
  bool online = false;
  RamSparseDisk sparse = {};
  // A disk above 2 GiB keeps only the nonzero pages of its seed (SeedDisk.h), in memory proportional to the content, and is served by the
  // direct (store-backed) path when the persistent store is open read-write; otherwise (UniXNU-piano) by the volatile disk below.
  SeedDisk big = {};
  bool isBig = false;
  // UniXNU-piano: the volatile disk of a large seed (VolatileDisk.h), set up by attachStore whenever the store is not open read-write.
  // Its pages come from `pool` (VolatilePool.h), which a refill thread fills in 2 MiB chunks; the I/O path never allocates.
  VolatileDisk vol = {};
  VolatilePool pool;
  bool refillStop = false, unmapOn = false;
  uint8_t spareEvent = 0;                          // writers wait on &spareEvent, the refill thread on &pool
  uint64_t statusShown = ~0ULL, refusalsShown = 0, statusAt = 0;
  static constexpr uint32_t kAdmitMs = 2000;       // the longest a write waits for pages before it is refused
  static uint64_t volatileBudgetPages() {
    uint32_t mib = 0;
    if (PE_parse_boot_argn("data-volatile-mib", &mib, sizeof mib) && mib >= 64 && mib <= 65536) return mib * 256ULL;
    uint64_t pages = gPhysSize / 4 / 4096;          // a quarter of the memory this kernel manages
    return pages < 65536 ? 65536 : pages > 1048576 ? 1048576 : pages;   // 256 MiB .. 4 GiB
  }
  static void volatileRefillThread(void *argument, wait_result_t);
  void volatileStatus(bool always);
  uint8_t seedHash[32] = {};
  persist::Overlay overlay;
  uint8_t *overlayBits = nullptr, *stage = nullptr;   // stage: kStagePages pages, one write or read run
  static constexpr uint32_t kStagePages = 32;
  bool storeAttached = false;
  // UniXNU-piano: a system sleep (native-ufs-suspend=1): off syncs the store and holds new requests, on releases them (PowerHold.h).
  DataPowerHold power;
  bool powerJoined = false, powerSeenOn = false;   // powerSeenOn: the first on came (an off before it is no system sleep)
  uint64_t powerWaits = 0;
  // Direct mode (the store is open read-write): changed pages live in the store window only, never in RAM; reads of them go to the
  // window. A failed store write stops all further writes (a page the system was told is written must be on the medium).
  bool direct = false, directFailed = false;
  struct StoreOps {
    // UniXNU-piano: the set-up's calls get native-ram-trace=1's lines (attachStore sets traceBitmap to the bitmap's size); 0: none.
    uint64_t traceBitmap = 0;
    bool read(uint64_t b, uint32_t n, void *d) {
      __atomic_fetch_add(&native_data_persist_loaded, n, __ATOMIC_RELAXED);
      if (!traceBitmap) return native_ufs_store_read(b, n, d);
      storeTraceIo('r', b, n, traceBitmap, false, false, nullptr);
      const bool ok = native_ufs_store_read(b, n, d);
      storeTraceIo('r', b, n, traceBitmap, true, ok, d);
      return ok;
    }
    bool write(uint64_t b, uint32_t n, const void *s) {
      if (!traceBitmap) return native_ufs_store_write(b, n, s);
      storeTraceIo('w', b, n, traceBitmap, false, false, nullptr);
      const bool ok = native_ufs_store_write(b, n, s);
      storeTraceIo('w', b, n, traceBitmap, true, ok, s);
      return ok;
    }
    bool sync() {
      if (!traceBitmap) return native_ufs_store_sync();
      storeTrace("cache sync");
      const bool ok = native_ufs_store_sync();
      storeTrace(ok ? "synced" : "cache sync FAILED");
      return ok;
    }
  };
  void persistFailed() { overlay.active = false; native_data_persist_state = 5; __atomic_fetch_add(&native_data_persist_errors, 1, __ATOMIC_RELAXED); }
public:
  // Called once, before the Data volume is mounted (native_ram_block_volume_seed): open the store window, put the pages that
  // earlier boots changed back into the sparse disk, and from then on write changed pages through. The seed identity
  // (seedHash) is not affected. Any refusal or error leaves the disk volatile, exactly as without persistence.
  // Boot arguments (docs/data-store.md):
  //   sheng-data-reset=1|2   start the store over, whatever it holds (the old pages are dropped, not applied); 2 also asks for Setup
  //                          (BootWatchdogPolicy.c)
  //   data-store-replace=1   UniXNU-piano: replace a store made for another seed. Without it such a store is kept untouched and Data
  //                          runs volatile for this boot (state 7). =0x<8 hex>: only the store whose seed begins with those digits.
  //   data-store-flatten=1   UniXNU-piano: write every seed page the store lacks into it, then mark it self-contained (resumable)
  static bool discardRequested() { uint32_t v = 0; return PE_parse_boot_argn("sheng-data-reset", &v, sizeof v) && (v == 1 || v == 2); }
  static uint32_t replaceRequested() { uint32_t v = 0; return PE_parse_boot_argn("data-store-replace", &v, sizeof v) ? v : 0; }
  static bool flattenRequested() { uint32_t v = 0; return PE_parse_boot_argn("data-store-flatten", &v, sizeof v) && v == 1; }
  static void seedText(const uint8_t digest[32], char out[9]) { snprintf(out, 9, "%08x", persist::seedTag(digest)); }
  static constexpr uint32_t kFlattenPages = 256;   // 1 MiB per flatten run
  // data-store-flatten (PersistOverlay.h flatten): only this seed's store, open read-write. Runs before the disk is published.
  void flattenStore() {
    char ours[9], theirs[9];
    seedText(seedHash, ours);
    seedText(overlay.storeSeed, theirs);
    if (!direct || !overlay.active || overlay.readOnly) {
      IOLog("NativeRamBlock: data-store-flatten=1: the store is not open read-write this boot (state %u); nothing flattened\n",
            native_data_persist_state);
      return;
    }
    if (overlay.selfContained) {
      IOLog("NativeRamBlock: data-store-flatten=1: the store is self-contained already (made with seed %s); nothing to do\n", theirs);
      return;
    }
    uint8_t *buffer = static_cast<uint8_t *>(IOMallocZero(kFlattenPages * persist::kBlock));
    const uint32_t runPages = buffer ? kFlattenPages : kStagePages;
    const uint64_t before = overlay.present;
    uint64_t written = 0;
    IOLog("NativeRamBlock: flatten: seed %s, %llu of %llu pages present; writing the seed's other pages into the store\n",
          ours, overlay.present, overlay.pages);
    StoreOps ops;
    int result = isBig ? overlay.flatten(ops, big, buffer ? buffer : stage, runPages, &written)
                       : overlay.flatten(ops, sparse, buffer ? buffer : stage, runPages, &written);
    if (buffer) IOFree(buffer, kFlattenPages * persist::kBlock);
    native_data_persist_flattened = written;
    native_data_persist_present = overlay.present;
    native_data_persist_flags = overlay.flags;
    if (result > 0) {
      IOLog("NativeRamBlock: flatten done: %llu seed pages written now (%llu present before, %llu now); the store is self-contained: "
            "any seed boots it from now on, a seed only starts a new store\n", written, before, overlay.present);
    } else {
      // Every run that reached the medium carries its bits; what is on the medium is still this seed's valid store.
      directFailed = true;
      native_data_persist_state = 5;
      __atomic_fetch_add(&native_data_persist_errors, 1, __ATOMIC_RELAXED);
      IOLog("NativeRamBlock: flatten stopped by a store error after %llu pages: the store stays seed %s's and valid; Data writes are off "
            "this boot; data-store-flatten=1 again resumes it\n", written, ours);
    }
  }
  void attachStore() {
    if (!gate) return;
    IOLockLock(gate);
    if (storeAttached) { IOLockUnlock(gate); return; }
    storeAttached = true;
    char ours[9], theirs[9] = "-";
    seedText(seedHash, ours);
    unsigned mode = &native_ufs_store_mode ? native_ufs_store_mode() : 0;
    storeTrace("set-up: data-persist=%u, seed %s, %s disk", mode, ours, isBig ? "large" : sparse.pages ? "sparse" : "flat");
    if (mode && (sparse.pages || isBig) && &native_ufs_store_open && &native_ufs_store_read && &native_ufs_store_write &&
        &native_ufs_store_sync && &native_ufs_store_gpt_check) {
      uint64_t pages = (isBig ? big.bytes : sparse.bytes) / 4096, first = 0;
      const char *why = "";
      native_data_persist_pages = pages;
      storeTrace("GPT check");
      native_data_persist_gpt = native_ufs_store_gpt_check();
      storeTrace("GPT check: %u; opening the window (%llu blocks)", native_data_persist_gpt, persist::windowBlocks(pages));
      if (!native_ufs_store_open(persist::windowBlocks(pages), &first, &why)) {
        native_data_persist_state = 4;
        storeTrace("window refused: %s", why);
        IOLog("NativeRamBlock: persistent store refused: %s\n", why);
      } else {
        native_data_persist_first_lba = first;
        uint64_t bb = persist::bitmapBlocks(pages);
        overlayBits = static_cast<uint8_t *>(IOMallocZero(bb * persist::kBlock));
        stage = static_cast<uint8_t *>(IOMallocZero(kStagePages * persist::kBlock));
        StoreOps ops;
        if (gStoreTrace) ops.traceBitmap = bb;
        storeTrace("window open at LBA %llu; opening the store (%s, data-store-replace %#x, sheng-data-reset %d)", first,
                   mode == 1 ? "read only" : "read-write", replaceRequested(), discardRequested() ? 1 : 0);
        persist::Open result = overlayBits && stage ? overlay.open(ops, pages, seedHash, mode == 1, overlayBits, discardRequested(),
                                                                   replaceRequested())
                                                    : persist::Open::Failed;
        storeTrace("store opened: %s (header check %d, generation %llu, present %llu)",
                   result == persist::Open::Resumed ? "resumed" : result == persist::Open::Fresh ? "fresh"
                   : result == persist::Open::Blank ? "blank" : result == persist::Open::Kept ? "kept" : "FAILED",
                   overlay.header, overlay.generation, overlay.present);
        native_data_persist_header = static_cast<uint32_t>(overlay.header);
        native_data_persist_flags = overlay.flags;
        native_data_persist_store_tag = persist::seedTag(overlay.storeSeed);
        if (overlay.header == 0 || overlay.header == 3) seedText(overlay.storeSeed, theirs);
        if (result == persist::Open::Failed) {
          native_data_persist_state = 4;
          overlay.active = false;
        } else if (result == persist::Open::Blank) {
          native_data_persist_state = 1;
        } else if (result == persist::Open::Kept) {
          // UniXNU-piano store guard: another seed's store (a test image, a rebuilt seed) is never replaced without being asked to.
          native_data_persist_state = 7;
          overlay.active = false;
          IOLog("NativeRamBlock: the store belongs to seed %s (this seed %s): kept; Data is volatile this boot; data-store-replace=1 starts over\n",
                theirs, ours);
        } else if (result == persist::Open::Fresh) {
          native_data_persist_state = 3;
          direct = mode == 2;
          if (overlay.replaced)
            IOLog("NativeRamBlock: the store was started over on request (data-store-replace or sheng-data-reset): a new store for seed %s, "
                  "generation %llu\n", ours, overlay.generation);
        } else if (mode == 2) {
          // Resumed read-write: nothing is copied into RAM. The window answers for every page whose bit is set.
          direct = true;
          native_data_persist_state = 2;
        } else if (isBig) {
          // A large disk has no RAM copy to load the store into (the loader below would need sparse.pages): the probe proves the
          // header and bitmap and leaves them alone.
          native_data_persist_state = 6;
          overlay.active = false;
          IOLog("NativeRamBlock: read-only probe: the store's %llu pages are not applied to a large disk; Data is the seed's this boot\n",
                overlay.present);
        } else {
          if (overlay.selfContained && !overlay.sameSeed) {
            // The store holds every page of its own seed; this seed's pages must not show through its clear bits.
            for (uint64_t i = 0; i < sparse.bytes / 4096; ++i)
              if (sparse.pages[i]) { IOFree(sparse.pages[i], 4096); sparse.pages[i] = nullptr; sparse.resident -= 4096; }
          }
          struct Loader {
            NativeRamBlock *self;
            bool operator()(uint64_t i, const uint8_t *page) {
              if (i >= self->sparse.bytes / 4096) return false;
              if (!self->sparse.pages[i]) {
                uint8_t *fresh = static_cast<uint8_t *>(IOMallocZero(4096));
                if (!fresh) return false;
                self->sparse.pages[i] = fresh;
                self->sparse.resident += 4096;
              }
              memcpy(self->sparse.pages[i], page, 4096);
              __atomic_fetch_add(&native_data_persist_loaded, 1, __ATOMIC_RELAXED);
              return true;
            }
          } load = { this };
          if (overlay.forEachPresent(ops, load, stage, kStagePages)) {
            native_data_persist_state = mode == 1 ? 6 : 2;
            native_ram_block_resident_bytes = sparse.resident;
          } else {
            // A partly restored disk would be worse than a volatile one, but the pages already copied are consistent
            // overlay content; stop writing back and let the system decide what it can mount.
            native_data_persist_state = 4;
            overlay.active = false;
          }
        }
        if (result == persist::Open::Resumed && overlay.selfContained)
          IOLog("NativeRamBlock: self-contained store (made with seed %s, %llu pages flattened; this seed %s): the seed is not read\n",
                theirs, overlay.flattened, ours);
        if (flattenRequested()) {
          if (result == persist::Open::Kept)
            IOLog("NativeRamBlock: data-store-flatten=1: the store is seed %s's and flattens only under that seed; nothing written\n", theirs);
          else flattenStore();
        }
        native_data_persist_present = overlay.present;
        native_data_persist_gpt_after = native_ufs_store_gpt_check();
      }
    }
    if (isBig && !direct) {
      // UniXNU-piano: a large seed without the store open read-write runs volatile (VolatileDisk.h) instead of refusing writes, its
      // pages from a budgeted pool that a refill thread fills (VolatilePool.h): the I/O path never allocates.
      vol.bytes = big.bytes;
      pool.budgetPages = volatileBudgetPages();
      pool.chunkCapacity = VolatilePool::capacityFor(pool.budgetPages);
      pool.chunks = static_cast<uint8_t **>(IOMallocZero(pool.chunkCapacity * sizeof(uint8_t *)));
      vol.dir = static_cast<uint8_t ***>(IOMallocZero(VolatileDisk::entries(vol.bytes) * sizeof(uint8_t **)));
      thread_t refill = nullptr;
      if (vol.dir && pool.chunks) {
        retain();                                     // the refill thread's; released when it ends (stop())
        if (kernel_thread_start(volatileRefillThread, this, &refill) != KERN_SUCCESS) { release(); refill = nullptr; }
        else thread_deallocate(refill);
      }
      if (!refill) {
        if (vol.dir) IOFree(vol.dir, VolatileDisk::entries(vol.bytes) * sizeof(uint8_t **));
        if (pool.chunks) IOFree(pool.chunks, pool.chunkCapacity * sizeof(uint8_t *));
        vol.dir = nullptr; pool.chunks = nullptr; pool.chunkCapacity = 0;
        IOLog("NativeRamBlock: the volatile disk could not be set up: Data writes are refused this boot\n");
      } else {
        // Unmap (TRIM) gives APFS's freed blocks back to the pool, so the disk holds about the live data, not every block ever written.
        uint32_t unmap = 1;
        PE_parse_boot_argn("data-volatile-unmap", &unmap, sizeof unmap);
        unmapOn = unmap != 0;
        if (unmapOn) {
          if (OSDictionary *features = OSDictionary::withCapacity(1)) {
            features->setObject(kIOStorageFeatureUnmap, kOSBooleanTrue);
            setProperty(kIOStorageFeaturesKey, features);
            features->release();
          }
        }
        IOLog("NativeRamBlock: volatile: budget %llu MiB (data-volatile-mib), in 2 MiB chunks; writes past it are refused (no space), "
              "a write that waits %u ms for pages is refused (no memory); unmap %s\n",
              pool.budgetPages / 256, kAdmitMs, unmapOn ? "on" : "off (data-volatile-unmap=0)");
      }
    }
    if (!direct)
      IOLog("NativeRamBlock: Data is volatile this boot (state %u, data-persist=%u): its changes stay in RAM and are gone at the next boot\n",
            native_data_persist_state, mode);
    // generation and present (the store's, as found or made): a boot after a kept one shows the same two numbers if nothing wrote.
    IOLog("NativeRamBlock: Data store state %u (1 blank, 2 resumed, 3 fresh, 4 failed, 7 kept), header %u, first LBA %llu, %llu pages, "
          "direct %d, seed %s, store seed %s, flags %u, generation %llu, present %llu\n",
          native_data_persist_state, native_data_persist_header, native_data_persist_first_lba, native_data_persist_pages, direct,
          ours, theirs, native_data_persist_flags, overlay.generation, overlay.present);
    storeTrace("set-up done");
    IOLockUnlock(gate);
  }

  bool available() const { return online && (memory || sparse.pages || isBig) && capacity; }
  bool seedInfo(uint64_t *base,uint64_t *bytes,uint8_t hash[32]) {
    if(!gate || !base || !bytes || !hash)return false;
    IOLockLock(gate);bool ready=available();
    if(ready){*base=(sparse.pages||isBig)?0:reinterpret_cast<uint64_t>(memory);*bytes=capacity*512;memcpy(hash,seedHash,32);}
    IOLockUnlock(gate);return ready;
  }
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  void joinPower();
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("Native"); }
  char *getProductString() override { return const_cast<char *>("Owned RAM block storage"); }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>("volatile storage; no persistent trust claim"); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = capacity - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override {
    if (!online) return kIOReturnNotReady;
    // Every changed page is on the medium, with its bitmap bit, before its write completed (PersistOverlay.h): nothing to flush.
    return kIOReturnSuccess;
  }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
  IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
  IOReturn doUnmap(IOBlockStorageDeviceExtent *, UInt32, IOStorageUnmapOptions) override;
};
OSDefineMetaClassAndStructors(NativeRamBlock, IOBlockStorageDevice)

#include <kern/thread.h>
extern "C" int native_ufs_store_ready(void);   // NativeUfs
// UniXNU-piano (UPSTREAM): publish once IOResources has matched IOBSD
static void publishWhenBSDMatched(void *argument, wait_result_t)
{
  NativeRamBlock *ram = static_cast<NativeRamBlock *>(argument);
  uint32_t trace = 0;   // UniXNU-piano: native-ram-trace=1, the set-up's lines (storeTrace above)
  gStoreTrace = PE_parse_boot_argn("native-ram-trace", &trace, sizeof trace) && trace == 1;
  gStoreTraceStart = mach_absolute_time();
  storeTrace("publish thread: waiting for IOBSD");
  if (OSDictionary *matching = IOService::resourceMatching(kIOBSDKey)) {
    if (IOService *resources = IOService::waitForMatchingService(matching)) resources->release();
    matching->release();
  }
  // UniXNU-piano: the store is part of NativeUfs's root layout: wait for it (60 s at most), open the store, then publish the disk,
  // so APFS never reads Data before its changed pages are attached.
  if (native_ufs_store_mode()) {
    storeTrace("IOBSD matched; waiting for NativeUfs's root layout (data-persist=%u)", native_ufs_store_mode());
    unsigned waited = 0;
    for (; waited < 600 && !native_ufs_store_ready(); ++waited) IOSleep(100);
    storeTrace("root layout: ready %d after %u ms", native_ufs_store_ready(), waited * 100);
  } else {
    storeTrace("IOBSD matched; data-persist=0: no store to open");
  }
  ram->attachStore();
  ram->joinPower();   // UniXNU-piano: native-ufs-suspend=1 with the store open read-write (PowerHold.h); else nothing
  storeTrace("registering the disk");
  ram->registerService();
  storeTrace("disk registered");
  ram->release();
}

bool NativeRamBlock::start(IOService *provider)
{
  if (!IOBlockStorageDevice::start(provider)) return false;
  IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
  if (!map) return false;
  OSData *packed = OSDynamicCast(OSData, map->getProperty("PackedRAMDisk"));
  OSData *range = OSDynamicCast(OSData, map->getProperty("RAMDisk"));
  if(packed && range){map->release();return false;}
  if(packed)range=packed;
  bool packedInput=packed!=nullptr;
  uint64_t physical = 0, bytes = 0;
  if (range && range->getLength() == 16) {
    memcpy(&physical, range->getBytesNoCopy(), 8);
    memcpy(&bytes, static_cast<const uint8_t *>(range->getBytesNoCopy()) + 8, 8);
  }
  map->release();
  // Boot memory belongs to the loader/kernel; never allocate or free it here.
  if (!bytes || bytes > 0x80000000 || (physical & 4095) || (!packedInput && (bytes & 4095)) ||
      physical < gPhysBase || bytes > gPhysSize || physical - gPhysBase > gPhysSize - bytes) return false;
  memory = reinterpret_cast<uint8_t *>(ml_static_ptovirt(physical));
  if (!memory || !(gate = IOLockAlloc())) return false;
  uint64_t logical=bytes;
  if(packedInput){
    if(!ramPackedLayout(memory,bytes,&logical))return false;
    struct ImportOps {uint8_t *allocate(){return static_cast<uint8_t *>(IOMallocZero(4096));}} ops;
    if(logical>0x80000000ULL){
      uint64_t nonzero=0;
      if(!seedCountNonzero(memory,bytes,&nonzero))return false;
      big.bytes=logical;big.capacity=nonzero;
      big.index=static_cast<uint64_t *>(IOMallocZero((nonzero?nonzero:1)*sizeof(uint64_t)));
      big.page=static_cast<uint8_t **>(IOMallocZero((nonzero?nonzero:1)*sizeof(uint8_t *)));
      if(!big.index||!big.page||!seedImportSparse(big,memory,bytes,ops))return false;
      isBig=true;sparse.bytes=logical;
    }else{
      sparse.bytes=logical;
      sparse.pages=static_cast<uint8_t **>(IOMallocZero((logical/4096)*sizeof(uint8_t *)));
      if(!ramSparseImport(sparse,memory,bytes,ops))return false;
    }
  }
  capacity = logical / 512;
  SHA256_CTX context;
  SHA256_Init(&context);
  if(isBig){
    seedIdentity(big,context,[](SHA256_CTX &c){SHA256_Init(&c);},[](SHA256_CTX &c,const void *p,size_t n){SHA256_Update(&c,p,n);},
                 [](SHA256_CTX &c,uint8_t *out){SHA256_Final(out,&c);},seedHash);
    memory=nullptr;
  }else{
  if(sparse.pages){
    static const uint8_t zero[4096]={};
    for(uint64_t i=0;i<logical/4096;++i)SHA256_Update(&context,sparse.pages[i]?sparse.pages[i]:zero,4096);
    memory=nullptr;
  }else SHA256_Update(&context, memory, bytes);
  SHA256_Final(seedHash, &context);
  }
  memcpy(native_ram_block_seed_sha256,seedHash,32);
  native_ram_block_bytes = logical;
  native_ram_block_sparse=(sparse.pages||isBig)?1:0;
  native_ram_block_resident_bytes=isBig?big.resident:sparse.pages?sparse.resident:logical;
  online = true;
  native_ram_block_ready = 1;
  retain();
  thread_t thread;
  if (kernel_thread_start(publishWhenBSDMatched, this, &thread) != KERN_SUCCESS) {
    release();
    return false;
  }
  thread_deallocate(thread);
  return true;
}

IOReturn NativeRamBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= capacity ||
      count > capacity - block || buffer->getLength() < count * 512 ||
      (buffer->getDirection() != kIODirectionIn && buffer->getDirection() != kIODirectionOut)) return kIOReturnBadArgument;
  if (!gate) return kIOReturnNotReady;
  const uint64_t bytes = count * 512;
  IOLockLock(gate);
  // UniXNU-piano: a system sleep holds new requests from the store's sync until the UFS link is back (PowerHold.h).
  while (power.held) { ++powerWaits; IOLockSleep(gate, &power, THREAD_UNINT); }
  if (!online) { IOLockUnlock(gate); return kIOReturnNotReady; }
  uint64_t actual;bool noMemory=false;IOReturn refused=kIOReturnSuccess;
  if(isBig && !direct && !vol.dir) {
    // Before the store is attached (APFS probes the disk for its volume UUID long before the kernel asks for the Data volume, which is what
    // attaches the store) a disk this size is read from its seed alone, and writes wait. (UniXNU-piano publishes the disk only after
    // attachStore, which sets up the volatile disk whenever the store is not open read-write; this stays for a failed allocation.)
    if(buffer->getDirection()==kIODirectionIn) {
      struct SeedReadOps {
        IOMemoryDescriptor *buffer;
        uint64_t copy(uint64_t at,const void *p,uint64_t n){return buffer->writeBytes(at,p,n);}
      } rops={buffer};
      static const uint8_t zero[4096]={};
      uint64_t done=0,offset=block*512;
      while(done<bytes) {
        uint64_t at=offset+done,index=at/4096,skip=at&4095,n=4096-skip;
        if(n>bytes-done)n=bytes-done;
        const uint8_t *page=big.find(index);
        uint64_t got=rops.copy(done,(page?page:zero)+skip,n);
        done+=got;if(got!=n)break;
      }
      __atomic_fetch_add(&native_ram_block_reads,1,__ATOMIC_RELAXED);
      IOReturn early=done==bytes?kIOReturnSuccess:kIOReturnIOError;
      if(early)__atomic_fetch_add(&native_ram_block_failures,1,__ATOMIC_RELAXED);
      IOLockUnlock(gate);
      completion->action(completion->target,completion->parameter,early,done);
      return kIOReturnSuccess;
    }
    IOLockUnlock(gate);return kIOReturnNotReady;
  }
  if(isBig && !direct){
    // Volatile large disk (VolatileDisk.h): the pages written this boot in RAM, every other page from the seed; the store is not used.
    // A write first admits the pages it may take from the pool (VolatilePool.h): refused past the budget, or after kAdmitMs of waiting
    // for the refill thread (the gate released while it sleeps). volatileDiskIO then only takes pages that are there.
    struct VolatileOps {
      IOMemoryDescriptor *buffer;VolatilePool *pool;
      uint8_t *allocate(){return pool->take();}
      uint64_t read(uint64_t at,void *p,uint64_t n){return buffer->readBytes(at,p,n);}
      uint64_t copy(uint64_t at,const void *p,uint64_t n){return buffer->writeBytes(at,p,n);}
    } vops={buffer,&pool};
    bool write=buffer->getDirection()==kIODirectionOut;
    if(write){
      struct AdmitEnv {
        NativeRamBlock *self;uint64_t deadline,offset,length;
        uint64_t need(){return self->online?volatileNeed(self->vol,offset,length):0;}
        void wakeRefill(){IOLockWakeup(self->gate,&self->pool,false);}
        bool sleep(){return IOLockSleepDeadline(self->gate,&self->spareEvent,deadline,THREAD_UNINT)!=THREAD_TIMED_OUT;}
      } env={this,0,block*512,bytes};
      clock_interval_to_deadline(kAdmitMs,kMillisecondScale,&env.deadline);
      VolatileAdmit admit=volatileAdmit(pool,env);
      if(admit==VolatileAdmit::NoSpace)refused=kIOReturnNoSpace;
      else if(admit==VolatileAdmit::NoMemory)refused=kIOReturnNoMemory;
      if(!online){IOLockUnlock(gate);return kIOReturnNotReady;}   // it may have slept
    }
    actual=refused?0:volatileDiskIO(big,vol,write,block*512,bytes,vops,vops,noMemory);
    if(refused && pool.refusedNoSpace+pool.refusedNoMemory==1)IOLockWakeup(gate,&pool,false);   // the status line says it at once
    if(write)__atomic_fetch_add(&native_ram_block_writes,1,__ATOMIC_RELAXED);
    else __atomic_fetch_add(&native_ram_block_reads,1,__ATOMIC_RELAXED);
    native_ram_block_volatile_bytes=vol.resident;
    native_ram_block_resident_bytes=big.resident+pool.heldBytes();
  }else if(sparse.pages||isBig){
    struct Operations {
      IOMemoryDescriptor *buffer;
      uint8_t *allocate(){return static_cast<uint8_t *>(IOMallocZero(4096));}
      uint64_t read(uint64_t at,void *p,uint64_t n){return buffer->readBytes(at,p,n);}
      uint64_t copy(uint64_t at,const void *p,uint64_t n){return buffer->writeBytes(at,p,n);}
    } ops={buffer};
    bool write=buffer->getDirection()==kIODirectionOut;
    if(direct){
      struct BufferOps {
        IOMemoryDescriptor *buffer;
        uint64_t read(uint64_t at,void *p,uint64_t n){return buffer->readBytes(at,p,n);}
        uint64_t copy(uint64_t at,const void *p,uint64_t n){return buffer->writeBytes(at,p,n);}
      } bops={buffer};
      StoreOps store;bool storeError=false;
      if(write && directFailed)actual=0;
      else actual=isBig?directDiskIO(big,overlay,store,stage,kStagePages,write,block*512,bytes,bops,&storeError)
                       :directDiskIO(sparse,overlay,store,stage,kStagePages,write,block*512,bytes,bops,&storeError);
      if(storeError){
        if(write)directFailed=true;
        native_data_persist_state=5;__atomic_fetch_add(&native_data_persist_errors,1,__ATOMIC_RELAXED);
      }
      if(write && actual){__atomic_fetch_add(&native_data_persist_writes,(actual+4095)/4096,__ATOMIC_RELAXED);__atomic_fetch_add(&native_data_persist_syncs,1,__ATOMIC_RELAXED);native_data_persist_present=overlay.present;}
    } else {
    actual=ramSparseIO(sparse,write,block*512,bytes,ops,noMemory);
    }
    if(!direct && write && actual==bytes && overlay.active && !overlay.readOnly){
      StoreOps store;
      uint64_t firstPage=block*512/4096,lastPage=(block*512+bytes-1)/4096;
      for(uint64_t i=firstPage;i<=lastPage && overlay.active;){
        uint32_t n=0;
        while(n<kStagePages && i+n<=lastPage && sparse.pages[i+n]){memcpy(stage+(uint64_t)n*persist::kBlock,sparse.pages[i+n],persist::kBlock);++n;}
        if(!n || !overlay.writeRun(store,i,n,stage)){persistFailed();break;}
        __atomic_fetch_add(&native_data_persist_writes,n,__ATOMIC_RELAXED);
        __atomic_fetch_add(&native_data_persist_syncs,1,__ATOMIC_RELAXED);   // one run = one durable commit
        native_data_persist_present=overlay.present;
        i+=n;
      }
    }
    if(write)__atomic_fetch_add(&native_ram_block_writes,1,__ATOMIC_RELAXED);
    else __atomic_fetch_add(&native_ram_block_reads,1,__ATOMIC_RELAXED);
    native_ram_block_resident_bytes=isBig?big.resident:sparse.resident;
  }else if (buffer->getDirection() == kIODirectionIn) {
    actual = buffer->writeBytes(0, memory + block * 512, bytes);
    __atomic_fetch_add(&native_ram_block_reads, 1, __ATOMIC_RELAXED);
  } else {
    actual = buffer->readBytes(0, memory + block * 512, bytes);
    __atomic_fetch_add(&native_ram_block_writes, 1, __ATOMIC_RELAXED);
  }
  IOReturn status = refused ? refused : actual == bytes ? kIOReturnSuccess : noMemory ? kIOReturnNoMemory : kIOReturnIOError;
  if (status) __atomic_fetch_add(&native_ram_block_failures, 1, __ATOMIC_RELAXED);
  IOLockUnlock(gate);
  completion->action(completion->target, completion->parameter, status, actual);
  return kIOReturnSuccess;
}

void NativeRamBlock::stop(IOService *provider)
{
  if (powerJoined) { PMstop(); powerJoined = false; }
  if (gate) {
    IOLockLock(gate);
    online = false;
    if (power.on()) IOLockWakeup(gate, &power, false);   // held requests see !online and fail
    refillStop = true;                                   // the refill thread ends (and releases its reference)
    IOLockWakeup(gate, &pool, false);
    IOLockWakeup(gate, &spareEvent, false);              // waiting writers see !online and fail
    IOLockUnlock(gate);
  }
  IOBlockStorageDevice::stop(provider);
}

// UniXNU-piano: the volatile disk's refill thread (VolatilePool.h volatileRefill): 2 MiB chunks with no lock held, as long as the
// pool wants them (its reserve, a waiting write's need) and the budget allows; the status line at most every 10 s when it changed.
void NativeRamBlock::volatileRefillThread(void *argument, wait_result_t)
{
  NativeRamBlock *self = static_cast<NativeRamBlock *>(argument);
  struct Env {
    NativeRamBlock *self;
    bool stopping() { return self->refillStop; }
    void idle() {
      AbsoluteTime deadline;
      clock_interval_to_deadline(10, kSecondScale, &deadline);
      IOLockSleepDeadline(self->gate, &self->pool, deadline, THREAD_UNINT);
      self->volatileStatus(false);
    }
    void unlock() { IOLockUnlock(self->gate); }
    void lock() { IOLockLock(self->gate); }
    uint8_t *allocChunk() { return static_cast<uint8_t *>(IOMallocAligned(VolatilePool::kChunkBytes, 4096)); }
    void freeChunk(uint8_t *chunk) { IOFreeAligned(chunk, VolatilePool::kChunkBytes); }
    void wakeWriters() { IOLockWakeup(self->gate, &self->spareEvent, false); }
    void backoff() {
      AbsoluteTime deadline;
      clock_interval_to_deadline(50, kMillisecondScale, &deadline);
      IOLockSleepDeadline(self->gate, &self->pool, deadline, THREAD_UNINT);
    }
  } env = { self };
  IOLockLock(self->gate);
  volatileRefill(self->pool, env);
  IOLockUnlock(self->gate);
  self->release();
}

// "NativeRamBlock: volatile: N pages, M MiB resident ..." (called with the gate held, from the refill thread only: never from I/O).
// At most every 10 s, and only when the numbers moved; a refusal shows at once.
void NativeRamBlock::volatileStatus(bool always)
{
  const uint64_t refusals = pool.refusedNoSpace + pool.refusedNoMemory;
  const uint64_t pages = vol.resident / 4096;
  uint64_t now = mach_absolute_time(), since = 0;
  absolutetime_to_nanoseconds(now - statusAt, &since);
  const bool due = statusAt == 0 || since >= 10000000000ULL;                       // 10 s
  const bool refusedNow = refusals != refusalsShown && (statusAt == 0 || since >= 1000000000ULL);   // new refusals: 1 s
  if (!always && !refusedNow && !(due && pages != statusShown)) return;
  statusAt = now;
  statusShown = pages;
  refusalsShown = refusals;
  const uint64_t held = pool.heldBytes() >> 20, budget = pool.budgetPages / 256, peak = pool.peak / 256;
  const uint64_t noSpace = pool.refusedNoSpace, noMemory = pool.refusedNoMemory, waits = pool.waits, failures = pool.chunkFailures;
  const uint64_t unmapped = pool.unmapped;
  IOLockUnlock(gate);   // IOLog without the disk's lock
  IOLog("NativeRamBlock: volatile: %llu pages, %llu MiB resident (%llu MiB held in chunks, peak %llu MiB, budget %llu MiB); "
        "%llu pages unmapped; writes refused: %llu no space, %llu no memory; %llu waits, %llu chunk failures\n",
        pages, pages / 256, held, peak, budget, unmapped, noSpace, noMemory, waits, failures);
  IOLockLock(gate);
}

// UniXNU-piano: unmap (TRIM, advertised only for the volatile disk): the pages APFS freed go back to the pool. Never allocates.
IOReturn NativeRamBlock::doUnmap(IOBlockStorageDeviceExtent *extents, UInt32 extentsCount, IOStorageUnmapOptions)
{
  if (!gate) return kIOReturnNotReady;
  if (!extents && extentsCount) return kIOReturnBadArgument;
  IOLockLock(gate);
  if (!online || !isBig || direct || !vol.dir || !unmapOn) { IOLockUnlock(gate); return kIOReturnUnsupported; }
  struct Give { VolatilePool *pool; void operator()(uint8_t *page) { pool->give(page); } } give = { &pool };
  for (UInt32 k = 0; k < extentsCount; ++k) {
    const uint64_t first = extents[k].blockStart, count = extents[k].blockCount;
    if (first >= capacity || count > capacity - first) continue;   // outside the disk: nothing to give back
    pool.unmapped += volatileUnmap(vol, first * 512, count * 512, give);
  }
  native_ram_block_volatile_bytes = vol.resident;
  IOLockUnlock(gate);
  return kIOReturnSuccess;
}

// UniXNU-piano: native-ufs-suspend=1. With the store open read-write the disk joins the power tree below NativeUfs (its power parent),
// so a system sleep takes it to off before the UFS link goes into Hibern8 and back on after the link is up. Without the argument, or
// with Data volatile or read only (nothing of the disk is written to UFS), it takes no part, as before.
void NativeRamBlock::joinPower()
{
  if (!gate || powerJoined || !overlay.active || overlay.readOnly) return;
  IOService *parent = native_ufs_power_parent();
  if (!parent) return;
  PMinit();
  parent->addPowerChild(this);
  const IOReturn r = registerPowerDriver(this, pianopower::powerStates(), pianopower::kStateCount);
  if (r != kIOReturnSuccess) {
    IOLog("NativeRamBlock: power: registerPowerDriver failed (%#x): the Data disk takes no part in a system sleep\n", r);
    PMstop();
    return;
  }
  changePowerStateTo(pianopower::kOn);
  powerJoined = true;
  IOLog("NativeRamBlock: power: native-ufs-suspend=1: the Data disk is NativeUfs's power child (a sleep syncs its store and holds "
        "its requests before the link goes down, and releases them once the link is back)\n");
}

IOReturn NativeRamBlock::setPowerState(unsigned long state, IOService *)
{
  if (!powerJoined || !gate) return kIOPMAckImplied;
  struct Store {
    bool sync() { return native_ufs_store_sync(); }
    void veto(const char *who) { native_ufs_power_veto(who); }
  } store;
  IOLockLock(gate);   // waits for the request in progress: none is half done below
  if (state != pianopower::kOff) powerSeenOn = true;
  if (state == pianopower::kOff && !powerSeenOn) {
    IOLog("NativeRamBlock: power: an off before the first on (not a system sleep): ignored\n");
  } else if (state == pianopower::kOff) {
    const bool wasHeld = power.held;
    const uint64_t t0 = mach_absolute_time();
    const bool held = power.off(store, overlay.active && !overlay.readOnly);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    if (!wasHeld && held)
      IOLog("NativeRamBlock: suspend %u: store synced in %llu us (state %u, generation %llu, %llu pages present); new requests held\n",
            power.holds, ns / 1000, native_data_persist_state, overlay.generation, overlay.present);
    else if (!held)
      IOLog("NativeRamBlock: suspend: the store's cache sync FAILED (%llu us): the Data disk keeps running and NativeUfs leaves the "
            "link up for this sleep\n", ns / 1000);
  } else if (power.on()) {
    IOLockWakeup(gate, &power, false);
    IOLog("NativeRamBlock: resume %u: requests released (%llu waits across the sleep)\n", power.holds, powerWaits);
  }
  IOLockUnlock(gate);
  return kIOPMAckImplied;
}

void NativeRamBlock::free()
{
  if (overlayBits) { IOFree(overlayBits, persist::bitmapBlocks(sparse.bytes / 4096) * persist::kBlock); overlayBits = nullptr; }
  if (stage) { IOFree(stage, kStagePages * persist::kBlock); stage = nullptr; }
  // The volatile disk's pages and leaves all live in the pool's chunks (the refill thread, which holds a reference, is gone by now).
  if (pool.chunks) {
    struct Release { void operator()(uint8_t *chunk) { IOFreeAligned(chunk, VolatilePool::kChunkBytes); } } release;
    pool.release(release);
    IOFree(pool.chunks, pool.chunkCapacity * sizeof(uint8_t *));
    pool.chunks = nullptr;
  }
  if (vol.dir) {
    IOFree(vol.dir, VolatileDisk::entries(vol.bytes) * sizeof(uint8_t **));
    vol = {};
  }
  if (isBig) {
    for (uint64_t k = 0; k < big.count; ++k) IOFree(big.page[k], 4096);
    if (big.index) IOFree(big.index, (big.capacity ? big.capacity : 1) * sizeof(uint64_t));
    if (big.page) IOFree(big.page, (big.capacity ? big.capacity : 1) * sizeof(uint8_t *));
    big = {}; isBig = false;
  }
  if (gate) { IOLockFree(gate); gate = nullptr; }
  if(sparse.pages){
    for(uint64_t i=0;i<sparse.bytes/4096;++i)if(sparse.pages[i])IOFree(sparse.pages[i],4096);
    IOFree(sparse.pages,(sparse.bytes/4096)*sizeof(uint8_t *));sparse={};
  }
  // Dense boot allocation and packed input remain owned until reboot.
  IOBlockStorageDevice::free();
}

static int selectVolumeSeed(const char *uuid, dev_t *device, char *path, size_t size,
    uint64_t *seedBase,uint64_t *seedBytes,uint8_t seedDigest[32])
{
  if (!uuid || strlen(uuid) != 36 || !device || !path || size < 6) return 0;
  *device = NODEV;
  path[0] = 0;
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSString *wanted = OSString::withCString(uuid);
  if (!matching || !properties || !wanted) {
    OSSafeReleaseNULL(wanted); OSSafeReleaseNULL(properties); OSSafeReleaseNULL(matching);
    return 0;
  }
  bool ready = properties->setObject("UUID", wanted) && matching->setObject("IOPropertyMatch", properties);
  wanted->release(); properties->release();
  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  matching->release();
  if (!iterator) return 0;
  bool selected = false;
  unsigned matches = 0;
  dev_t candidate = NODEV;
  char candidatePath[64] = {};
  uint64_t base=0,bytes=0;uint8_t hash[32]={};
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    if (++matches != 1) { selected = false; break; }
    OSString *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
    OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    OSNumber *mjr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
    OSNumber *mnr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
    if (!identity || strcmp(identity->getCStringNoCopy(), uuid) || !name || !mjr || !mnr ||
        !name->getLength() || name->getLength() >= sizeof(candidatePath) - 6 || name->getLength() >= size - 6 ||
        mjr->unsigned64BitValue() > 255 || mnr->unsigned64BitValue() > 0xffffff) continue;
    unsigned depth = 0;
    for (IOService *provider = media; provider && depth++ < 64; provider = provider->getProvider()) {
      NativeRamBlock *ram = OSDynamicCast(NativeRamBlock, provider);
      if (!ram) continue;
      ram->attachStore();
      if (!ram->seedInfo(&base,&bytes,hash)) continue;
      candidate = static_cast<dev_t>(makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
      snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", name->getCStringNoCopy());
      selected = true;
      break;
    }
  }
  iterator->release();
  if (!selected || matches != 1) return 0;
  *device = candidate;
  strlcpy(path, candidatePath, size);
  if(seedBase)*seedBase=base;
  if(seedBytes)*seedBytes=bytes;
  if(seedDigest)memcpy(seedDigest,hash,32);
  return 1;
}

extern "C" int native_ram_block_volume_device(const char *uuid,dev_t *device,char *path,size_t size) {
  return selectVolumeSeed(uuid,device,path,size,nullptr,nullptr,nullptr);
}
extern "C" int native_ram_block_volume_seed(const char *uuid,dev_t *device,char *path,size_t size,
    uint64_t *base,uint64_t *bytes,uint8_t digest[32]) {
  if(!base || !bytes || !digest)return 0;
  *base=0;*bytes=0;memset(digest,0,32);
  return selectVolumeSeed(uuid,device,path,size,base,bytes,digest);
}
