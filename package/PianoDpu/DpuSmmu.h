// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu B1b, the SMMU part (docs/drivers/dpu.md section 4.2): the DPU stream's own context bank of the apps SMMU, reprogrammed in
// place with a page table of ours. The route (the stream-match group of 0x800 and its S2CR) is never written: moving the live stream to
// another bank reset the SoC on Linux. Portable (host tests in tests/dpu_flip_host.cpp): every register access goes through an Io
// object (rd(offset from the SMMU base), wr(offset, value), delayUs(us)); every table write is followed by io.clean(pointer, bytes)
// before the SMMU may walk it (the walker is not coherent with the CPU's caches: walks are non-cacheable, TCR IRGN0/ORGN0 = 0).
//
// The table (4 KiB granule, 32-bit input, the walk starts at level 1):
//   L1[0] -> L2 for 0-1 GiB; L1[1] invalid; L1[2], L1[3] = 1 GiB identity blocks for 0x80000000-0xffffffff, read-write (what the
//   bank passed through untranslated before: the boot framebuffer at 0xfc800000 keeps scanning through the switch, and the rest of
//   DDR below 4 GiB stays reachable as before); L2[128 + k] -> L3 table k for the surface windows: kSlots slots of 32 MiB at IOVA
//   0x10000000 + 32 MiB * n (one 3200x2136 surface is 6675 pages of 4 KiB), mapped read-only (the DPU only reads a surface), each
//   followed by a guard of 4 pages on a zeroed scratch page (a fetch burst past the last row reads zeros instead of faulting).
// Format: chosen at run time from what the bank is (CBAR type 1: stage 1, stage 2 bypass, AArch64 descriptors with CBA2R VA64 = 1,
// MAIR0 attribute 0 = Normal non-cacheable; CBAR type 0: stage 2 only, MemAttr Normal non-cacheable). Nested banks (type 3) and
// stage 1 with stage 2 fault (type 2) are refused, as are AArch32 banks (VA64 = 0).
//
// Facts: ARM IHI 0062 (SMMU architecture v2: the global and context bank registers, SCTLR/TCR/TCR2/MAIR/FSR fields, TLBIALL/TLBSYNC/
// TLBSTATUS) and the VMSAv8-64 descriptor format (ARM ARM D8); what the same hypervisor family showed on SM8550 (ShengUfs, read for its
// facts: bank 3 SCTLR 0xe0 = translation off, TCR2 SEP 0 faulted every address with bit 31 set so SEP must be 7, no ATOS, a fault must
// terminate (CFCFG 0) rather than stall the display, TLBIALL + TLBSYNC + TLBSTATUS work on the bank page); the apps SMMU's stores come
// from a general register, never wzr (Qualcomm's trapped SMMU MMIO: PianoDwc3's SmmuStream.h, the kernel glue's wr).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "DpuSurvey.h"

namespace pianodpu {
namespace smmu {

// ------------------------------------------------------------------------------------------------------------- registers
// Global space 0 (page 0), global space 1 (page 1), and a context bank's page at (pages + n) * 4 KiB (IDR1: NUMPAGENDXB).
constexpr uint32_t kScr0 = 0x000, kIdr0 = 0x020, kIdr1 = 0x024, kIdr2 = 0x028;
constexpr uint32_t kIdr0S1ts = 1u << 30, kIdr0S2ts = 1u << 29;   // stage 1 / stage 2 translation supported
constexpr uint32_t kIdr2Ptfs4k = 1u << 12;                       // AArch64 4 KiB granule supported
inline unsigned idr2Oas(uint32_t idr2) { return (idr2 >> 4) & 0xfu; }   // 0 32, 1 36, 2 40, 3 42, 4 44, 5 48 bits
constexpr unsigned kPaBits[6] = {32, 36, 40, 42, 44, 48};
constexpr uint32_t smrOffset(unsigned n) { return 0x800 + 4 * n; }
constexpr uint32_t s2crOffset(unsigned n) { return 0xc00 + 4 * n; }
constexpr uint32_t cbarOffset(unsigned n) { return 0x1000 + 4 * n; }
constexpr uint32_t cba2rOffset(unsigned n) { return 0x1800 + 4 * n; }
// Context bank registers (offsets in the bank's page).
constexpr uint32_t kSctlr = 0x000, kResume = 0x008, kTcr2 = 0x010, kTtbr0Lo = 0x020, kTtbr0Hi = 0x024, kTcr = 0x030, kMair0 = 0x038,
                   kMair1 = 0x03c, kFsr = 0x058, kFarLo = 0x060, kFarHi = 0x064, kFsynr0 = 0x068, kTlbiAll = 0x618,
                   kTlbSync = 0x7f0, kTlbStatus = 0x7f4;
// SCTLR: M translation, CFRE fault reporting (abort), CFIE fault interrupt, CFCFG 1 stall / 0 terminate.
constexpr uint32_t kSctlrM = 1u << 0, kSctlrCfre = 1u << 5, kSctlrCfie = 1u << 6, kSctlrCfcfg = 1u << 7;
// FSR: TF 1, AFF 2, PF 3, EF 4, TLBMCF 5, TLBLKF 6, ASF 7, UUT 8, SS 30 (a stalled transaction), MULTI 31; write one to clear.
constexpr uint32_t kFsrFaults = 0xc00001feu, kFsrSs = 1u << 30;
// TCR (VA64): T0SZ 5:0, IRGN0 9:8, ORGN0 11:10, SH0 13:12, TG0 15:14 (0 = 4 KiB), EPD1 23; for stage 2 also SL0 7:6, PS 18:16.
// TCR2 (stage 1): PASize 2:0, SEP 17:15 (7: the sign extension comes from the upstream bus).
constexpr uint32_t kT0sz = 32;                                   // 32-bit IOVA: the walk starts at level 1
constexpr uint32_t kTcrS1 = kT0sz | 1u << 23;                    // non-cacheable non-shareable walks, 4 KiB, no TTBR1
constexpr uint32_t tcrS2(unsigned ps) { return kT0sz | 1u << 6 /* SL0: level 1 */ | uint32_t(ps) << 16; }
constexpr uint32_t tcr2S1(unsigned pasize) { return uint32_t(pasize) | 7u << 15; }
constexpr uint32_t kMair0Nc = 0x44;                              // attribute 0: Normal, inner and outer non-cacheable

// ------------------------------------------------------------------------------------------------------- the descriptors
enum class Stage : uint8_t { s1, s2 };
constexpr uint64_t kOutMask = 0x0000fffffffff000ull;
constexpr uint64_t kDescValid = 1, kDescTable = 3, kDescBlock = 1, kDescPage = 3;
constexpr uint64_t kAf = 1ull << 10, kXn = 1ull << 54, kPxn = 1ull << 53;
// Stage 1: AttrIndx 0 (MAIR0 attribute 0), AP[2:1] 01 read-write at any privilege / 11 read-only at any privilege, AF, never
// executable. Stage 2: MemAttr 0101 (Normal outer and inner non-cacheable), S2AP 11 read-write / 01 read-only, AF, XN.
constexpr uint64_t leafAttrs(Stage stage, bool writable)
{
  return stage == Stage::s1 ? (kAf | (writable ? 1ull << 6 : 3ull << 6) | kXn | kPxn)
                            : (kAf | 5ull << 2 | (writable ? 3ull << 6 : 1ull << 6) | kXn);
}
constexpr uint64_t blockDesc(Stage stage, uint64_t pa, bool writable) { return (pa & kOutMask) | leafAttrs(stage, writable) | kDescBlock; }
constexpr uint64_t pageDesc(Stage stage, uint64_t pa, bool writable) { return (pa & kOutMask) | leafAttrs(stage, writable) | kDescPage; }
constexpr uint64_t tableDesc(uint64_t pa) { return (pa & kOutMask) | kDescTable; }
constexpr bool descWritable(Stage stage, uint64_t d) { return stage == Stage::s1 ? ((d >> 7) & 1u) == 0 : ((d >> 7) & 1u) == 1; }

// ---------------------------------------------------------------------------------------------------------- the layout
constexpr uint32_t kWindowBase = 0x10000000u, kSlotBytes = 32u << 20;
constexpr unsigned kSlots = 4;
constexpr unsigned kSlotPages = kSlotBytes / 4096;               // 8192 PTEs a slot
constexpr unsigned kL3PerSlot = kSlotBytes >> 21;                // 16 level-3 tables (2 MiB each) a slot
constexpr unsigned kGuardPages = 4;                              // one 16 KiB scratch page after every surface
constexpr unsigned kL1Page = 0, kL2Page = 1, kL3FirstPage = 2;
constexpr unsigned kTablePages = kL3FirstPage + kSlots * kL3PerSlot;   // 66 pages, 264 KiB, physically contiguous
constexpr uint64_t kTableBytes = uint64_t(kTablePages) * 4096;
constexpr uint64_t kIdentityBase = 0x80000000ull, kIdentityEnd = 0x100000000ull;
constexpr uint32_t slotIova(unsigned slot) { return kWindowBase + kSlotBytes * slot; }
static_assert(slotIova(kSlots) <= 0x80000000u, "the windows stay below 2 GiB, out of the identity range");
static_assert((kWindowBase >> 30) == 0 && ((slotIova(kSlots) - 1) >> 30) == 0, "every window under L1[0]");

struct Tables {
  uint64_t *cpu = nullptr;   // kTablePages * 512 descriptors as the CPU writes them (cacheable; cleaned before the SMMU walks)
  uint64_t pa = 0;           // physical address of page 0 (4 KiB aligned, the pages contiguous)
  Stage stage = Stage::s1;
  uint64_t *page(unsigned i) const { return cpu + 512 * size_t(i); }
  uint64_t pagePa(unsigned i) const { return pa + 4096ull * i; }
  uint64_t *slotL3(unsigned slot) const { return page(kL3FirstPage + kL3PerSlot * slot); }
};

// Everything zeroed, then the identity blocks and the window's tables; the whole buffer cleaned.
template <class Clean>
void buildTables(Tables &t, Clean clean)
{
  for (size_t i = 0; i < size_t(kTablePages) * 512; ++i) t.cpu[i] = 0;
  uint64_t *l1 = t.page(kL1Page), *l2 = t.page(kL2Page);
  l1[0] = tableDesc(t.pagePa(kL2Page));
  l1[2] = blockDesc(t.stage, kIdentityBase, true);
  l1[3] = blockDesc(t.stage, kIdentityBase + 0x40000000ull, true);
  for (unsigned k = 0; k < kSlots * kL3PerSlot; ++k) l2[(kWindowBase >> 21) + k] = tableDesc(t.pagePa(kL3FirstPage + k));
  clean(t.cpu, size_t(kTableBytes));
}

// A slot's pages: count 4 KiB physical pages (each 4 KiB aligned, inside XNU's RAM), then the guard pages, read-only; the rest of
// the slot invalid. False (the slot left invalid) when they do not fit or a page is unfit.
template <class Clean>
bool mapSlot(Tables &t, unsigned slot, const uint64_t *pages, unsigned count, uint64_t guardPa, Clean clean, const char **why)
{
  if (slot >= kSlots || !count || count + kGuardPages > kSlotPages) {
    *why = "does not fit a 32 MiB window";
    return false;
  }
  for (unsigned i = 0; i < count; ++i)
    if ((pages[i] & 0xfffu) || pages[i] < kXnuDramBase || pages[i] >= kXnuDramEnd) {
      *why = "a page outside XNU's RAM or not 4 KiB aligned";
      return false;
    }
  if ((guardPa & 0x3fffu) || guardPa < kXnuDramBase || guardPa >= kXnuDramEnd) {
    *why = "the guard page";
    return false;
  }
  uint64_t *l3 = t.slotL3(slot);
  for (unsigned i = 0; i < kSlotPages; ++i)
    l3[i] = i < count ? pageDesc(t.stage, pages[i], false)
                      : i < count + kGuardPages ? pageDesc(t.stage, guardPa + 4096ull * (i - count), false) : 0;
  clean(l3, size_t(kSlotPages) * 8);
  return true;
}

template <class Clean>
void unmapSlot(Tables &t, unsigned slot, Clean clean)
{
  if (slot >= kSlots) return;
  uint64_t *l3 = t.slotL3(slot);
  for (unsigned i = 0; i < kSlotPages; ++i) l3[i] = 0;
  clean(l3, size_t(kSlotPages) * 8);
}

// The translation our own tables give an IOVA (the survey's walk over the CPU's view): the self-check before a flip.
inline Walk lookup(const Tables &t, uint32_t iova)
{
  SmmuBank b{};
  b.tcr = kTcrS1;
  b.ttbr0lo = uint32_t(t.pa);
  b.ttbr0hi = uint32_t(t.pa >> 32) & 0xffffu;
  return walk(b, iova, [&](uint64_t pa) -> uint64_t {
    if (pa < t.pa || pa >= t.pa + kTableBytes) return 0;
    return t.cpu[(pa - t.pa) / 8];
  });
}

// ------------------------------------------------------------------------------------------------------------- the plan
struct BankRegs {
  uint32_t sctlr = 0, tcr2 = 0, ttbr0lo = 0, ttbr0hi = 0, tcr = 0, mair0 = 0, mair1 = 0;
};
template <class Io>
BankRegs readBank(Io &io, uint32_t cb)
{
  BankRegs r;
  r.sctlr = io.rd(cb + kSctlr);
  r.tcr2 = io.rd(cb + kTcr2);
  r.ttbr0lo = io.rd(cb + kTtbr0Lo);
  r.ttbr0hi = io.rd(cb + kTtbr0Hi);
  r.tcr = io.rd(cb + kTcr);
  r.mair0 = io.rd(cb + kMair0);
  r.mair1 = io.rd(cb + kMair1);
  return r;
}

struct BankPlan {
  bool ok = false;
  const char *why = nullptr;
  Route route = Route::unread;
  Stage stage = Stage::s1;
  unsigned group = 0, bank = 0, pages = 0;
  uint32_t cb = 0;            // the bank page's offset from the SMMU base
  uint32_t cbar = 0, cba2r = 0, idr0 = 0, idr2 = 0;
  unsigned pasize = 0;        // TCR2 PASize (stage 1) / TCR PS (stage 2) encoding
};
// From the survey (level 2: the bank's registers read) and IDR0/IDR2: what the bank is and whether it can be ours. A bank with
// translation on (the boot loader's tables) is taken only when the caller walked those tables and found the splash identity-mapped.
inline BankPlan planBank(const SmmuSurvey &s, uint32_t idr0, uint32_t idr2, bool identityVerified)
{
  BankPlan p;
  p.route = route(s);
  p.idr0 = idr0;
  p.idr2 = idr2;
  const SmmuEntry *e = nullptr;
  for (unsigned i = 0; i < s.count && !e; ++i)
    if (smrCovers(s.found[i].smr, kStreamDpu)) e = &s.found[i];
  if (p.route != Route::bankOff && p.route != Route::bankOn) {
    p.why = p.route == Route::bankUnread ? "the bank's registers were not read (piano-dpu-smmu=2 reads them)"
            : p.route == Route::other  ? "the DPU's bank is neither stage 1 nor stage 2 alone"
                                       : "no translating bank of the DPU's own to reprogram (its route would have to move: reset risk)";
    if (p.route == Route::other && e && ((e->bank.cbar >> 16) & 3u) == 0) p.why = nullptr;   // stage 2 only: ours too
    if (p.why) return p;
  }
  p.group = e->group;
  p.bank = e->s2cr & 0xffu;
  p.pages = s.pages;
  p.cb = (s.pages + p.bank) * 4096;
  p.cbar = e->bank.cbar;
  p.cba2r = e->bank.cba2r;
  const unsigned type = (p.cbar >> 16) & 3u;
  p.stage = type == 0 ? Stage::s2 : Stage::s1;
  if (!e->bank.read) {
    p.why = "the bank's registers were not read";
    return p;
  }
  if (type == 0 && (e->bank.sctlr & kSctlrM)) {
    p.why = "a stage 2 bank with translation on: not the boot loader's expected state";
    return p;
  }
  if (!(p.cba2r & 1u)) {
    p.why = "the bank uses AArch32 tables (CBA2R VA64 0): not reprogrammed";
    return p;
  }
  if (!(idr0 & (p.stage == Stage::s1 ? kIdr0S1ts : kIdr0S2ts)) || !(idr2 & kIdr2Ptfs4k)) {
    p.why = "the SMMU does not report this stage with 4 KiB AArch64 tables (IDR0/IDR2)";
    return p;
  }
  const unsigned oas = idr2Oas(idr2);
  if (oas < 1) {
    p.why = "the SMMU's output size is 32 bits: XNU's RAM (and the tables) lie above 4 GiB";
    return p;
  }
  p.pasize = oas > 5 ? 5 : oas;
  if ((kXnuDramEnd - 1) >> kPaBits[p.pasize]) {
    p.why = "the output size does not cover XNU's RAM";
    return p;
  }
  if ((e->bank.sctlr & kSctlrM) && !identityVerified) {
    p.why = "translation is on with the boot loader's tables: walk them first (piano-dpu-smmu=3) and find the splash identity-mapped";
    return p;
  }
  p.ok = true;
  return p;
}

// What the bank is set to: our table, translation on, faults reported and terminated (never a stalled display fetch).
inline BankRegs wantRegs(const BankPlan &p, const BankRegs &found, uint64_t tablePa)
{
  BankRegs w = found;
  w.ttbr0lo = uint32_t(tablePa);
  w.ttbr0hi = uint32_t(tablePa >> 32) | (p.stage == Stage::s1 ? found.ttbr0hi & 0xffff0000u : 0);   // stage 1 keeps its ASID
  if (p.stage == Stage::s1) {
    w.tcr = kTcrS1;
    w.tcr2 = tcr2S1(p.pasize);
    w.mair0 = kMair0Nc;   // attributes 1-3 (unused by our descriptors) and MAIR1 as found: not written
  } else {
    w.tcr = tcrS2(p.pasize);
  }
  w.sctlr = (found.sctlr & ~kSctlrCfcfg) | kSctlrM | kSctlrCfre | kSctlrCfie;
  return w;
}

// ---------------------------------------------------------------------------------------------------- TLB and the bank
// TLBIALL then TLBSYNC on the bank's page, TLBSTATUS.SACTIVE polled (bounded); written values are ignored by the SMMU and never 0
// (the trap path). True when the sync completed.
template <class Io>
bool tlbSync(Io &io, uint32_t cb)
{
  io.wr(cb + kTlbiAll, 0xffffffffu);
  io.wr(cb + kTlbSync, 0xffffffffu);
  for (unsigned i = 0; i < 100000; ++i) {
    if (!(io.rd(cb + kTlbStatus) & 1u)) return true;
    io.delayUs(1);
  }
  return false;
}

struct EnableResult {
  bool ok = false;
  const char *why = nullptr;
  uint32_t fsr = 0, farLo = 0, farHi = 0, fsynr0 = 0, watchedUs = 0;
  bool restored = false;     // after a failure: the bank's registers are back as found
};

// Clear a fault: a stalled transaction terminated (RESUME.TnR), the fault bits written back to clear them.
template <class Io>
void clearFault(Io &io, uint32_t cb)
{
  const uint32_t fsr = io.rd(cb + kFsr);
  if (fsr & kFsrSs) io.wr(cb + kResume, 1);
  if (fsr & kFsrFaults) io.wr(cb + kFsr, fsr & kFsrFaults);
}

// Back as found: translation off first (the identity table and the boot loader's agree on the splash), the registers, the fault
// cleared, the TLB, then the found SCTLR (its M as found). True when the readback says so.
template <class Io>
bool restoreBank(const BankPlan &p, const BankRegs &found, Io &io)
{
  const uint32_t cb = p.cb;
  io.wr(cb + kSctlr, found.sctlr & ~kSctlrM);
  clearFault(io, cb);
  io.wr(cb + kTtbr0Lo, found.ttbr0lo);
  io.wr(cb + kTtbr0Hi, found.ttbr0hi);
  if (p.stage == Stage::s1) {
    io.wr(cb + kTcr2, found.tcr2);
    io.wr(cb + kMair0, found.mair0);
  }
  io.wr(cb + kTcr, found.tcr);
  const bool synced = tlbSync(io, cb);
  if (found.sctlr & kSctlrM) io.wr(cb + kSctlr, found.sctlr);
  return synced && io.rd(cb + kTtbr0Lo) == found.ttbr0lo && io.rd(cb + kTcr) == found.tcr &&
         (io.rd(cb + kSctlr) & kSctlrM) == (found.sctlr & kSctlrM);
}

// In place: (translation off if the boot loader's was on), our TTBR0/TCR/TCR2/MAIR, readback, fault cleared, TLB synced, then
// SCTLR.M; the display's own fetches (the splash through the identity blocks) watched for watchUs in 500 us steps. Any failure puts
// the bank back as found. The stream's group, S2CR, CBAR and CBA2R are not written.
template <class Io>
EnableResult enableBank(const BankPlan &p, const BankRegs &found, uint64_t tablePa, uint32_t watchUs, Io &io)
{
  EnableResult r;
  const uint32_t cb = p.cb;
  const BankRegs want = wantRegs(p, found, tablePa);
  auto fail = [&](const char *why) {
    r.why = why;
    r.restored = restoreBank(p, found, io);
    return r;
  };
  if (found.sctlr & kSctlrM) {
    io.wr(cb + kSctlr, found.sctlr & ~kSctlrM);
    if (!tlbSync(io, cb)) return fail("TLB sync timeout (translation off)");
  }
  io.wr(cb + kTtbr0Lo, want.ttbr0lo);
  io.wr(cb + kTtbr0Hi, want.ttbr0hi);
  if (p.stage == Stage::s1) {
    io.wr(cb + kTcr2, want.tcr2);
    io.wr(cb + kMair0, want.mair0);
  }
  io.wr(cb + kTcr, want.tcr);
  if (io.rd(cb + kTtbr0Lo) != want.ttbr0lo || io.rd(cb + kTtbr0Hi) != want.ttbr0hi || io.rd(cb + kTcr) != want.tcr)
    return fail("the bank ignores our writes (TTBR0/TCR readback)");
  clearFault(io, cb);
  if (!tlbSync(io, cb)) return fail("TLB sync timeout");
  io.wr(cb + kSctlr, want.sctlr);
  if (!(io.rd(cb + kSctlr) & kSctlrM)) return fail("SCTLR.M does not read back set");
  for (; r.watchedUs < watchUs; r.watchedUs += 500) {
    r.fsr = io.rd(cb + kFsr);
    if (r.fsr & kFsrFaults) break;
    io.delayUs(500);
  }
  if (r.fsr & kFsrFaults) {
    r.farLo = io.rd(cb + kFarLo);
    r.farHi = io.rd(cb + kFarHi);
    r.fsynr0 = io.rd(cb + kFsynr0);
    return fail("a fault while the display fetched through the new table");
  }
  r.ok = true;
  return r;
}

// ------------------------------------------------------------------------------------------------- the IOVA windows
// kSlots windows, least recently used first, never the one on screen or the one waiting to latch. key/key2: the surface and its
// memory (the caller keeps both alive while the slot holds them).
struct Slot {
  const void *key = nullptr, *key2 = nullptr;
  void *cookie = nullptr;    // the caller's (its wired memory, its mappings)
  unsigned pages = 0;
  uint64_t used = 0;
};
struct Slots {
  Slot slot[kSlots];
  uint64_t clock = 0;
  int shown = -1, pending = -1;
  int find(const void *key, const void *key2)
  {
    for (unsigned i = 0; i < kSlots; ++i)
      if (slot[i].key && slot[i].key == key && slot[i].key2 == key2) {
        slot[i].used = ++clock;
        return int(i);
      }
    return -1;
  }
  // A free slot first, else the least recently used one that is neither shown nor pending; -1 none.
  int victim() const
  {
    int pick = -1;
    for (unsigned i = 0; i < kSlots; ++i) {
      if (int(i) == shown || int(i) == pending) continue;
      if (!slot[i].key) return int(i);
      if (pick < 0 || slot[i].used < slot[pick].used) pick = int(i);
    }
    return pick;
  }
  void take(unsigned i, const void *key, const void *key2, void *cookie, unsigned pages)
  {
    slot[i] = Slot{key, key2, cookie, pages, ++clock};
  }
  void clear(unsigned i) { slot[i] = Slot{}; }
  unsigned used() const
  {
    unsigned n = 0;
    for (const Slot &s : slot) n += s.key != nullptr;
    return n;
  }
};

}  // namespace smmu
}  // namespace pianodpu
