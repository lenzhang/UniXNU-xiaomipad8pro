// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu B1b in the kernel (piano-dpu=2; docs/drivers/dpu.md sections 4.2, 6 and 8): DpuFlip.h's Hw (the MDSS/DPU block mapped
// once; the apps SMMU's page 0 and the DPU bank's page, stores only to that bank; the MDSS SPI 83 through ArmCpu with PianoIrq.h's
// storm guard; the table memory; the surfaces' wired pages and their cacheable aliases; level 3's test pattern), the boot arguments,
// the ladder on the survey's thread, the watch while a surface is on the panel, and the "PianoDpuFlip" platform function.
//   piano-dpu-flip=N      1 (default): the boot framebuffer's addresses flushed again, 3 times; 2: + the boot framebuffer 64 rows
//                         lower for 2 s and back; 3: + the DPU's SMMU bank in place (identity + windows), a test pattern through
//                         window 0 for 2 s, the fault status after a minute, the bank back as found; 4: the bank in place and the
//                         flips served to NativeGopDisplay (native-gop-display-flip=1)
//   piano-dpu-irq=0       CTL_FLUSH polled every ~1 ms instead of the vsync interrupt (default 1)
//   piano-dpu-clean=N     0 never clean a surface's rows from the CPU caches, 1 (default) through a cacheable alias when one can be
//                         made, 2 refuse a surface without one
//   piano-dpu-flush=MASK  the CTL_FLUSH bits of a flip (default 0: the pipes' own bits)
//   piano-dpu-underrun=0  an underrun does not revert (default 1)
//   piano-dpu-latch=MS    the latch timeout (default 50, 10..500)
//   piano-dpu-watch=S     the fault/underrun check while a surface is on the panel (default 10, 0 off; nothing wakes while the boot
//                         framebuffer is)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "DpuFlip.h"
#include "DpuFlipKernel.h"
#include "../common/PianoIrq.h"

extern "C" task_t kernel_task;
extern "C" volatile int32_t piano_dpu_stage;

namespace pianodpu {
namespace {

using pianoirq::dsb;
using pianoirq::rd32;
using pianoirq::wr32;   // a store from a general register, never wzr (the trapped SMMU)

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[640];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoDpu: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_dpu_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

// The CPU caches over [p, p + n) cleaned to the point of coherency by VA (DC CVAC: what the DPU and the SMMU's walker read is in
// memory; a read-permission operation, so a read-only alias takes it), then DSB.
void cleanRange(const void *p, size_t n)
{
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t line = uintptr_t(4) << ((ctr >> 16) & 15), end = reinterpret_cast<uintptr_t>(p) + n;
  for (uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1); a < end; a += line) __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
  dsb();
}

IOMemoryMap *mapIo(uint64_t pa, uint64_t bytes)
{
  IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  return map;
}

uint64_t nowUs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}

// A window's owner: the surface and its memory retained, the memory prepared (wired), the read-only cacheable alias for the clean.
struct Cookie {
  OSObject *surface;
  IOMemoryDescriptor *memory;
  IOMemoryMap *alias;
  bool prepared;
};

// The 4 KiB pages of md (at most max); false with why when a segment is not 4 KiB aligned or there are too many.
bool listPages(IOMemoryDescriptor *md, uint64_t *pages, unsigned max, unsigned *count, const char **why)
{
  const uint64_t length = md->getLength();
  unsigned n = 0;
  for (uint64_t off = 0; off < length;) {
    IOByteCount seg = 0;
    const addr64_t pa = md->getPhysicalSegment(off, &seg, kIOMemoryMapperNone);
    if (!pa || !seg) {
      *why = "no physical segment";
      return false;
    }
    if ((pa & 0xfffu) || (off + seg < length && (seg & 0xfffu))) {
      *why = "a segment not 4 KiB aligned";
      return false;
    }
    for (uint64_t a = 0; a < seg && off + a < length; a += 4096) {
      if (n == max) {
        *why = "more pages than a window";
        return false;
      }
      pages[n++] = pa + a;
    }
    off += seg;
  }
  *count = n;
  if (!n) *why = "empty";
  return n != 0;
}

// ------------------------------------------------------------------------------------------------------------- the Hw
class KernelHw {
 public:
  // The MDSS block (0xae00000) and the DPU behind it, one mapping: kMdss .. kMdp + kMdpBytes.
  uintptr_t mdss = 0;
  // The apps SMMU: page 0 (IDR0/IDR2) and the DPU bank's page (the only page ever stored to).
  uintptr_t smmu0 = 0, bank = 0;
  uint32_t bankOff = 0;
  uint64_t stores = 0, smmuStores = 0, refusedStores = 0;
  uint32_t cleanPolicy = 1;   // piano-dpu-clean
  // The vsync interrupt.
  pianoirq::SpiLine line{};
  pianoirq::Waiter waiter{};
  pianoirq::MissGuard miss = pianoirq::missGuard(3);
  volatile uint32_t sticky = 0;
  uint32_t seen = 0;
  bool attached = false;

  uint32_t rd(uint64_t pa)
  {
    if (!mdss || pa < kMdss || pa + 4 > kMdp + kMdpBytes) return 0xffffffffu;
    return rd32(mdss + uintptr_t(pa - kMdss));
  }
  void wr(uint64_t pa, uint32_t v)
  {
    if (!mdss || pa < kMdp || pa + 4 > kMdp + kMdpBytes) {   // MDSS top is read only here; nothing outside the DPU
      ++refusedStores;
      return;
    }
    wr32(mdss + uintptr_t(pa - kMdss), v);
    ++stores;
  }
  void barrier() { dsb(); }
  uint32_t smmuRd(uint32_t off)
  {
    if (smmu0 && off < 0x1000) return rd32(smmu0 + off);
    if (bank && off >= bankOff && off < bankOff + 0x1000) return rd32(bank + (off - bankOff));
    return 0xffffffffu;
  }
  void smmuWr(uint32_t off, uint32_t v)
  {
    if (!bank || off < bankOff || off >= bankOff + 0x1000) {   // the route (SMR, S2CR, CBAR, CBA2R) and other banks: never
      ++refusedStores;
      return;
    }
    wr32(bank + (off - bankOff), v);
    dsb();
    ++smmuStores;
  }
  uint64_t nowUs() { return pianodpu::nowUs(); }
  void delayUs(uint32_t us) { IODelay(us); }
  void sleepMs(uint32_t ms) { IOSleep(ms); }

  static void handler(void *context)
  {
    KernelHw *h = static_cast<KernelHw *>(context);
    const uint32_t bits = flip::serviceIrq([h](uint64_t pa) { return h->rd(pa); },
                                           [h](uint64_t pa, uint32_t v) { wr32(h->mdss + uintptr_t(pa - kMdss), v); });
    dsb();
    if (bits) {
      __atomic_fetch_or(&h->sticky, bits, __ATOMIC_RELEASE);
      h->waiter.kick();
      return;
    }
    if (h->line.stray()) h->waiter.kick();   // a storm: the line is off at the GIC now, the waits poll
  }
  void attachIrq()
  {
    const uint32_t id = pianoirq::intid(flip::kSpiMdss);
    const IOReturn r = line.attach(id, false, &KernelHw::handler, this);
    const uint32_t rb = line.readback;
    attached = r == kIOReturnSuccess;
    note("vsync: SPI %u (INTID %u, level) %s (%#x): readback %#x (group 1 %u, enabled %u, edge %u, priority %#x), router %#llx; %s",
         flip::kSpiMdss, id, attached ? "attached" : "NOT attached", r, rb, rb & 1, (rb >> 1) & 1, (rb >> 2) & 1, (rb >> 8) & 0xff,
         (unsigned long long)line.router, attached ? "INTF1's vsync wakes a latch wait, enabled only while a flush is pending"
                                                   : "CTL_FLUSH polled instead");
  }
  bool irqLive() const { return attached && line.live() && !miss.dead; }
  void irqMiss()
  {
    if (miss.dead) return;
    if (miss.miss())
      note("vsync: %u waits of 10 ms in a row with no interrupt (SPI 83 count %llu, %llu strays): polled from now on", miss.run,
           (unsigned long long)line.count(), (unsigned long long)line.strays);
  }
  void irqDelivered() { miss.delivered(); }
  uint64_t irqCount() const { return attached ? line.count() : 0; }
  uint32_t takeIrqBits() { return __atomic_exchange_n(&sticky, 0u, __ATOMIC_ACQ_REL); }
  void vsyncArm() { seen = waiter.now(); }
  bool waitVsync(uint32_t us)
  {
    if (!irqLive()) {
      IOSleep(1);
      return false;
    }
    const uint32_t ms = (us + 999) / 1000;
    const bool woke = waiter.wait(seen, ms ? ms : 1);
    seen = waiter.now();
    return woke;
  }

  void clean(const void *p, size_t n) { cleanRange(p, n); }

  // The tables: one physically contiguous, cacheable buffer (cleaned after every write), kept for the boot.
  IOBufferMemoryDescriptor *tableMemory = nullptr, *guardMemory = nullptr;
  bool contiguous(IOBufferMemoryDescriptor *&bmd, uint64_t bytes, uint64_t *pa)
  {
    if (!bmd)
      bmd = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous) |
                                                                          IOOptionBits(kIOMemoryMapperNone), bytes, 16384);
    if (!bmd) return false;
    IOByteCount length = 0;
    *pa = bmd->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    return *pa && !(*pa & 0x3fffu) && length >= bytes && bmd->getBytesNoCopy();
  }
  bool allocTables(uint64_t **cpu, uint64_t *pa)
  {
    if (!contiguous(tableMemory, smmu::kTableBytes, pa)) return false;
    *cpu = static_cast<uint64_t *>(tableMemory->getBytesNoCopy());
    return true;
  }
  bool allocGuard(uint64_t *pa)
  {
    if (!contiguous(guardMemory, 16384, pa)) return false;
    bzero(guardMemory->getBytesNoCopy(), 16384);
    cleanRange(guardMemory->getBytesNoCopy(), 16384);
    return true;
  }

  uint64_t surfaceLength(const PianoDpuFlipRequest &r)
  {
    IOMemoryDescriptor *md = OSDynamicCast(IOMemoryDescriptor, static_cast<OSObject *>(r.memory));
    return md ? md->getLength() : 0;
  }
  bool surfaceMap(const PianoDpuFlipRequest &r, uint64_t *pages, unsigned max, unsigned *count, void **cookie, const char **why)
  {
    IOMemoryDescriptor *md = OSDynamicCast(IOMemoryDescriptor, static_cast<OSObject *>(r.memory));
    OSObject *surface = static_cast<OSObject *>(r.surface);
    if (!md || !surface) {
      *why = "not an IOMemoryDescriptor";
      return false;
    }
    if (md->prepare(kIODirectionOut) != kIOReturnSuccess) {
      *why = "prepare failed";
      return false;
    }
    if (!listPages(md, pages, max, count, why)) {
      md->complete(kIODirectionOut);
      return false;
    }
    Cookie *c = static_cast<Cookie *>(IOMallocZero(sizeof(Cookie)));
    if (!c) {
      md->complete(kIODirectionOut);
      *why = "no memory";
      return false;
    }
    c->surface = surface;
    c->memory = md;
    c->prepared = true;
    if (cleanPolicy) {
      c->alias = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly | kIOMapCopybackCache);
      if (!c->alias) {
        static bool told;
        if (!told) note("flip: a cacheable alias of a %llux%llu surface was refused (piano-dpu-clean=%u)", (unsigned long long)r.width,
                        (unsigned long long)r.height, cleanPolicy);
        told = true;
      }
    }
    surface->retain();
    md->retain();
    *cookie = c;
    return true;
  }
  bool surfaceClean(void *cookie, uint64_t offset, uint64_t bytes)
  {
    Cookie *c = static_cast<Cookie *>(cookie);
    if (!c->alias || offset + bytes > c->alias->getLength()) return false;
    cleanRange(reinterpret_cast<const void *>(c->alias->getVirtualAddress() + offset), size_t(bytes));
    return true;
  }
  void surfaceRelease(void *cookie)
  {
    Cookie *c = static_cast<Cookie *>(cookie);
    if (!c) return;
    if (c->alias) c->alias->release();
    if (c->prepared) c->memory->complete(kIODirectionOut);
    c->memory->release();
    c->surface->release();
    IOFree(c, sizeof(Cookie));
  }

  // Level 3: 3200x2136 'BGRA' at row 12800, eight vertical bars (white yellow cyan green magenta red blue black, NativeGopDisplay's
  // native-gop-display-bars), a 16-pixel white border, a 256x256 black-and-white checker of 32-pixel squares at the top left inside it.
  bool testBuffer(uint64_t *pages, unsigned max, unsigned *count, void **cookie)
  {
    static const uint32_t bars[8] = {0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00, 0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000};
    const uint64_t bytes = uint64_t(kPanelHeight) * kSplashStride;
    IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, (bytes + 16383) & ~16383ull, 16384);
    if (!b) return false;
    uint32_t *px = static_cast<uint32_t *>(b->getBytesNoCopy());
    for (uint32_t y = 0; y < kPanelHeight; ++y)
      for (uint32_t x = 0; x < kPanelWidth; ++x) {
        uint32_t v = bars[x * 8 / kPanelWidth];
        if (x < 16 || y < 16 || x >= kPanelWidth - 16 || y >= kPanelHeight - 16) v = 0xffffffff;
        else if (x < 16 + 256 && y < 16 + 256) v = (((x - 16) / 32 + (y - 16) / 32) & 1) ? 0xff000000 : 0xffffffff;
        px[size_t(y) * kPanelWidth + x] = v;
      }
    cleanRange(px, size_t(bytes));
    const char *why = nullptr;
    if (b->prepare(kIODirectionOut) != kIOReturnSuccess) {
      b->release();
      return false;
    }
    Cookie *c = static_cast<Cookie *>(IOMallocZero(sizeof(Cookie)));
    if (!c || !listPages(b, pages, max, count, &why)) {
      if (why) note("flip 3: the test buffer: %s", why);
      if (c) IOFree(c, sizeof(Cookie));
      b->complete(kIODirectionOut);
      b->release();
      return false;
    }
    b->retain();   // as surface and as memory: two references, as surfaceRelease drops them
    *c = Cookie{b, b, nullptr, true};
    *cookie = c;
    return true;
  }

  void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char text[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    IOLog("PianoDpu: %s\n", text);
  }
};

constinit KernelHw gHw;
constinit flip::Flipper<KernelHw> gFlipper(gHw);
IOLock *volatile gLock;
const OSSymbol *volatile gSymbol;
volatile bool gServing;            // step 4 ready: requests go to the engine (else refused without the lock)
pianoirq::Waiter gWatch;           // the watch thread's wake-up: a surface came on the panel
IOService *gOwner;

// The boot loader's table memory for the walk of a bank it translates (piano-dpu-smmu=3): pages mapped on demand, read only.
struct TablePage {
  uint64_t pa;
  IOMemoryMap *map;
};
TablePage gTablePages[16];
unsigned gTablePageCount;
uint64_t tableLoad64(uint64_t pa)
{
  const uint64_t base = pa & ~0xfffull;
  uintptr_t va = 0;
  for (unsigned i = 0; i < gTablePageCount && !va; ++i)
    if (gTablePages[i].pa == base) va = uintptr_t(gTablePages[i].map->getVirtualAddress());
  if (!va && gTablePageCount < 16) {
    IOMemoryMap *map = mapIo(base, 0x1000);
    if (map) {
      gTablePages[gTablePageCount++] = {base, map};
      va = uintptr_t(map->getVirtualAddress());
    }
  }
  if (!va) return 0;
  return uint64_t(rd32(va + uintptr_t(pa & 0xfff))) | uint64_t(rd32(va + uintptr_t(pa & 0xfff) + 4)) << 32;
}

void publish(const char *what)
{
  if (!gOwner) return;
  OSString *s = OSString::withCString(what);
  if (s) {
    gOwner->setProperty("piano-dpu-b1b", s);
    s->release();
  }
}

}  // namespace

bool flipFunction(const OSSymbol *name, void *request, IOReturn *result)
{
  const OSSymbol *ours = __atomic_load_n(&gSymbol, __ATOMIC_ACQUIRE);
  if (!name || !ours || !name->isEqualTo(ours)) return false;
  PianoDpuFlipRequest *r = static_cast<PianoDpuFlipRequest *>(request);
  *result = kIOReturnSuccess;
  if (!r || r->size < sizeof(PianoDpuFlipRequest) || r->version != kPianoDpuFlipVersion) {
    if (r && r->size >= 4 * sizeof(uint32_t)) r->result = kPianoDpuFlipVersionMismatch;
    *result = kIOReturnBadArgument;
    return true;
  }
  if (!__atomic_load_n(&gServing, __ATOMIC_ACQUIRE)) {   // not serving: nothing on the panel but the boot framebuffer
    r->result = r->surface ? kPianoDpuFlipOff : kPianoDpuFlipShown;
    r->latchUs = r->workUs = 0;
    return true;
  }
  IOLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
  IOLockLock(lock);
  const bool before = gFlipper.watching();
  gFlipper.request(*r);
  const bool after = gFlipper.watching();
  if (gFlipper.state != flip::State::ready) __atomic_store_n(&gServing, false, __ATOMIC_RELEASE);
  IOLockUnlock(lock);
  if (after && !before) gWatch.kick();
  return true;
}

void flipRun(IOService *owner, const Survey &s, const SmmuSurvey &m, uint32_t smmuLevel)
{
  using pianoirq::bootArg;
  gOwner = owner;
  flip::Config &cfg = gFlipper.cfg;
  cfg.level = bootArg("piano-dpu-flip", 1);
  if (cfg.level < 1) cfg.level = 1;
  if (cfg.level > 4) cfg.level = 4;
  cfg.irq = bootArg("piano-dpu-irq", 1) != 0;
  cfg.clean = bootArg("piano-dpu-clean", 1);
  if (cfg.clean > 2) cfg.clean = 2;
  cfg.flush = bootArg("piano-dpu-flush", 0);
  cfg.underrun = bootArg("piano-dpu-underrun", 1) != 0;
  cfg.latchMs = bootArg("piano-dpu-latch", 50);
  if (cfg.latchMs < 10) cfg.latchMs = 10;
  if (cfg.latchMs > 500) cfg.latchMs = 500;
  cfg.watchS = bootArg("piano-dpu-watch", 10);
  if (cfg.watchS > 3600) cfg.watchS = 3600;
  gHw.cleanPolicy = cfg.clean;
  stage(6, "B1b: zero-copy flips (docs/drivers/dpu.md section 8)");
  note("B1b: piano-dpu-flip=%u irq=%u clean=%u flush=%#x underrun=%u latch=%u ms watch=%u s smmu=%u", cfg.level, cfg.irq, cfg.clean, cfg.flush,
       cfg.underrun, cfg.latchMs, cfg.watchS, smmuLevel);
  IOMemoryMap *mdss = mapIo(kMdss, kMdp + kMdpBytes - kMdss);
  if (!mdss) {
    stage(-6, "B1b: the MDSS/DPU block does not map");
    publish("failed: no mapping");
    return;
  }
  gHw.mdss = uintptr_t(mdss->getVirtualAddress());
  // The SMMU pages: page 0, and the bank the survey found for stream 0x800 (its page is the only one ever stored to).
  bool identity = false;
  const SmmuEntry *e = nullptr;
  for (unsigned i = 0; i < m.count && !e; ++i)
    if (smrCovers(m.found[i].smr, kStreamDpu)) e = &m.found[i];
  if (cfg.level >= 3 && e && !m.error && ((e->s2cr >> 16) & 3u) == 0 && (e->s2cr & 0xffu) < m.banks) {
    IOMemoryMap *p0 = mapIo(kSmmu, 0x1000);
    gHw.bankOff = (m.pages + (e->s2cr & 0xffu)) * 4096;
    IOMemoryMap *pb = gHw.bankOff + 0x1000 <= kSmmuBytes ? mapIo(kSmmu + gHw.bankOff, 0x1000) : nullptr;
    if (!p0 || !pb) {
      stage(-6, "B1b: the SMMU's page 0 or the DPU bank's page does not map");
      publish("failed: SMMU pages");
      return;
    }
    gHw.smmu0 = uintptr_t(p0->getVirtualAddress());
    gHw.bank = uintptr_t(pb->getVirtualAddress());
    if (route(m) == Route::bankOn) {
      if (smmuLevel >= 3) {
        unsigned walks = 0;
        uint64_t bad = 0;
        identity = flip::splashIdentity(e->bank, tableLoad64, &walks, &bad);
        if (identity) note("B1b: the boot loader's tables map the boot framebuffer to itself (%u walks)", walks);
        else note("B1b: the boot loader's tables do NOT map the boot framebuffer to itself (first at iova %#llx, %u walks)", (unsigned long long)bad, walks);
      } else {
        note("B1b: the DPU's bank translates (the boot loader's tables): piano-dpu-smmu=3 walks them before it is taken over");
      }
    }
  }
  IOLock *lock = IOLockAlloc();
  const OSSymbol *symbol = OSSymbol::withCStringNoCopy(kPianoDpuFlipFunction);
  if (!lock || !symbol) {
    stage(-6, "B1b: no lock");
    publish("failed: no lock");
    return;
  }
  __atomic_store_n(&gLock, lock, __ATOMIC_RELEASE);
  __atomic_store_n(&gSymbol, symbol, __ATOMIC_RELEASE);
  gFlipper.pages = static_cast<uint64_t *>(IOMalloc(smmu::kSlotPages * sizeof(uint64_t)));   // kept for the boot
  stage(7, cfg.level == 1 ? "B1b step 1: the boot framebuffer's addresses flushed again"
           : cfg.level == 2 ? "B1b step 2: the boot framebuffer shifted by 64 rows and back"
           : cfg.level == 3 ? "B1b step 3: the DPU's SMMU bank in place and a test pattern"
                            : "B1b step 4: the DPU's SMMU bank in place and flips for NativeGopDisplay");
  IOLockLock(lock);
  const bool ok = gFlipper.ladder(s, m, identity);
  const bool ready = gFlipper.state == flip::State::ready;
  IOLockUnlock(lock);
  note("B1b: %llu DPU stores, %llu SMMU stores (all to bank page %#x), %llu refused stores", (unsigned long long)gHw.stores,
       (unsigned long long)gHw.smmuStores, gHw.bankOff, (unsigned long long)gHw.refusedStores);
  char what[160];
  snprintf(what, sizeof what, "step %u %s%s%s", cfg.level, ok ? (ready ? "serving" : "passed") : "failed", gFlipper.failure ? ": " : "",
           gFlipper.failure ? gFlipper.failure : "");
  publish(what);
  if (!ok) {
    stage(-7, "B1b: a step failed (the lines above say which); the boot framebuffer stays");
    return;
  }
  stage(8, ready ? "B1b: serving" : "B1b: done");
  if (!ready) return;
  __atomic_store_n(&gServing, true, __ATOMIC_RELEASE);
  // The watch: every piano-dpu-watch s while a surface is on the panel, else asleep until one is (an idle desktop on the boot
  // framebuffer wakes nothing here).
  for (;;) {
    const uint32_t seen = gWatch.now();   // before the look: a surface shown after it wakes the wait below
    IOLockLock(lock);
    gFlipper.watch();
    const bool watching = gFlipper.watching();
    const bool failed = gFlipper.state == flip::State::failed;
    if (failed) __atomic_store_n(&gServing, false, __ATOMIC_RELEASE);
    IOLockUnlock(lock);
    if (failed) {
      IOLockLock(lock);
      gFlipper.summary(true);
      IOLockUnlock(lock);
      snprintf(what, sizeof what, "off for the boot: %s", gFlipper.failure ? gFlipper.failure : "?");
      publish(what);
      stage(-8, "B1b: off for the boot (copies); the revert lines above say why");
      return;
    }
    gWatch.wait(seen, watching ? cfg.watchS * 1000 : 3600u * 1000);
  }
}

}  // namespace pianodpu
