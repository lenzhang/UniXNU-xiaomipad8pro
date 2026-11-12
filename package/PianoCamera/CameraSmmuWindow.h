// SPDX-License-Identifier: BSD-2-Clause-Patent
// package/PianoWifi/WifiSmmuWindow.h of this repository (sha256 0e48005c9b635b92249d4611e6ec026aaf0227c2035ebb1584f043339aa642e5),
// with the namespace changed to pianocamera, WifiHw.h replaced by CameraHw.h (and the window's two constants it took from there,
// below), and these lines added: PianoWifi is not modified. PianoCamera level 3 maps its frame buffer with it for stream 0x1c00 when
// the boot chain left the stream no route (CameraSmmu.h decides). The original's comment follows; "level 4/5", "the chip" and "the
// firmware's regions" are PianoWifi's.
// PianoWifi level 5, the 32-bit question (docs/drivers/wifi.md section 10.11): a translating context bank for the endpoint's
// stream (0x1401) whose window is a 32-bit address space mapping level 4's DMA window in 2 MiB blocks, so that every address the
// chip is given fits 32 bits. Levels 3 and 4 give the stream a *bypass* bank, where a bus address is a physical address — and
// piano's XNU memory is all above 4 GiB, so a bypassing stream can never produce a 32-bit address. The chip's own driver gives it
// 36-bit addresses for the firmware's regions but 32-bit ones for the rings of the data path, which is why this exists before
// level 6 needs it, and why level 5 can measure it early (piano-wifi-iova=1).
//
// Reused by copying, with attribution (UPSTREAM has the sha256 of each; neither package is modified):
//   package/PianoDpu/DpuSmmu.h — the context bank register offsets and their fields, the VMSAv8-64 descriptor helpers (block,
//     table, leaf attributes for stage 1 and stage 2), the "non-cacheable walks, attribute 0 Normal non-cacheable" choice (our
//     window is reached write-combined, as level 4's is, so nothing keeps caches and the device coherent), the TLBIALL/TLBSYNC/
//     TLBSTATUS sequence with non-zero written values (Qualcomm's trapped SMMU MMIO) and bounded waits.
//   package/PianoUsbHost/SmmuIdentity.h — the plan/apply split for a stream that has no translating bank of its own: a free
//     stream-match group (or the stream's own exact entry, redirected in place), a free context bank found from one below the top,
//     the whole bank programmed before the one store that switches the stream to it.
// Ours: the 32-bit input space (T0SZ 32, the walk starts at level 1, 4 entries of 1 GiB), the two-page table, the window's 2 MiB
// blocks over one contiguous physical range, and the read-write leaf attributes the firmware's regions need.
//
// Portable: Read(offset from the SMMU base), Write(offset, value), Expired() bounds every wait; the owner cleans the table out of
// the caches before the SMMU may walk it. Host tests: tests/qmi_host.cpp (the plan's choices, the table's descriptors, the walk of
// every window address, the apply order and its read-backs).
#pragma once
#include <stdint.h>
#include "CameraHw.h"

namespace pianocamera {
constexpr uint32_t kIovaWindowBase = 0x20000000;   // 512 MiB: inside the first 1 GiB of a 32-bit input space (WifiHw.h's value)
constexpr uint32_t kIovaBlockBytes = 2u << 20;
namespace iova {

// ---- the apps SMMU's registers (ARM IHI 0062: global space, then a page per context bank) ----
constexpr uint32_t kIdr0 = 0x020, kIdr1 = 0x024, kIdr2 = 0x028;
constexpr uint32_t kIdr0S1ts = 1u << 30, kIdr2Ptfs4k = 1u << 12;
constexpr uint32_t kTlbiAllNsNh = 0x68, kTlbGSync = 0x70, kTlbGStatus = 0x74;
constexpr uint32_t smrOffset(unsigned n) { return 0x800 + 4 * n; }
constexpr uint32_t s2crOffset(unsigned n) { return 0xc00 + 4 * n; }
constexpr uint32_t cbarOffset(unsigned n) { return 0x1000 + 4 * n; }
constexpr uint32_t cba2rOffset(unsigned n) { return 0x1800 + 4 * n; }
constexpr uint32_t kCbSctlr = 0x000, kCbTcr2 = 0x010, kCbTtbr0Lo = 0x020, kCbTtbr0Hi = 0x024, kCbTtbr1Lo = 0x028, kCbTcr = 0x030,
                   kCbContextIdr = 0x034, kCbMair0 = 0x038, kCbMair1 = 0x03c, kCbFsr = 0x058, kCbTlbiAll = 0x618,
                   kCbTlbSync = 0x7f0, kCbTlbStatus = 0x7f4;
// SCTLR: M translation on, TRE, AFE, CFRE (a fault aborts the transaction rather than stalling it), ASIDPNE. piano's SMMU keeps
// ASIDPNE at 0 (wifi-l5ib, 2026-11-12: wrote 0x1027, read 0x27): the ASID namespace is then shared between banks, so the window's
// ASID is its own bank number (as Linux's arm-smmu gives each stage-1 bank), and only the other four bits must read back.
constexpr uint32_t kSctlrM = 1u << 0;
constexpr uint32_t kSctlrAsidpne = 1u << 12;
constexpr uint32_t kSctlrWindow = 1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | kSctlrAsidpne;
constexpr uint32_t kSctlrRequired = kSctlrWindow & ~kSctlrAsidpne;
constexpr uint32_t ttbr0Hi(uint64_t tablePa, unsigned bank) { return uint32_t(tablePa >> 32) | (bank & 0xffu) << 16; }   // ASID 55:48
constexpr uint32_t kCbarStage1 = 1u << 16 | 0xfu << 12 | 3u << 8;   // stage 1 with stage 2 bypass; MemAttr, BPSHCFG
constexpr uint32_t kFsrFaults = 0xc00001feu;
// TCR (VA64): T0SZ 5:0, IRGN0 9:8, ORGN0 11:10, SH0 13:12, TG0 15:14 (0 = 4 KiB), EPD1 23. Non-cacheable non-shareable walks.
constexpr uint32_t kT0sz = 32;
constexpr uint32_t kTcrWindow = kT0sz | 1u << 23;
constexpr uint32_t tcr2Window(unsigned pasize) { return uint32_t(pasize) | 7u << 15; }   // output size, sign extension from the bus
constexpr uint32_t kMair0Nc = 0x44;   // attribute 0: Normal, inner and outer non-cacheable
inline unsigned idr2Oas(uint32_t idr2) { return (idr2 >> 4) & 0xfu; }
constexpr unsigned kPaBits[6] = {32, 36, 40, 42, 44, 48};

// ---- the descriptors ----
constexpr uint64_t kOutMask = 0x0000fffffffff000ull;
constexpr uint64_t kDescTable = 3, kDescBlock = 1;
constexpr uint64_t kAf = 1ull << 10, kXn = 1ull << 54, kPxn = 1ull << 53;
// Stage 1, attribute index 0, read-write at any privilege level, accessed, never executable.
constexpr uint64_t kLeafRw = kAf | 1ull << 6 | kXn | kPxn;
constexpr uint64_t blockDesc(uint64_t pa) { return (pa & kOutMask) | kLeafRw | kDescBlock; }
constexpr uint64_t tableDesc(uint64_t pa) { return (pa & kOutMask) | kDescTable; }

// ---- the table: one level-1 page (4 entries of 1 GiB for a 32-bit input space) and one level-2 page (512 blocks of 2 MiB) ----
constexpr unsigned kTablePages = 2, kL1Entries = 4, kL2Entries = 512;
constexpr uint64_t kTableBytes = uint64_t(kTablePages) * 4096;

struct Window {
  uint64_t tablePa = 0;        // the level-1 page (the level-2 page follows it)
  uint64_t *l1 = nullptr, *l2 = nullptr;
  uint32_t base = kIovaWindowBase, bytes = 0, blocks = 0;
  uint64_t arenaPa = 0;
};

// The window's blocks over one contiguous physical range. False when the range or the window does not fit the rule: both 2 MiB
// aligned, the window inside the first gigabyte of the input space, and the whole range inside one level-2 page.
inline bool buildWindow(Window &w, uint64_t *l1, uint64_t *l2, uint64_t tablePa, uint64_t arenaPa, uint32_t arenaBytes,
                        uint32_t base)
{
  if (!l1 || !l2 || (tablePa & 0xfffu) || (arenaPa & (kIovaBlockBytes - 1)) || (base & (kIovaBlockBytes - 1)) || !arenaBytes)
    return false;
  const uint32_t blocks = arenaBytes / kIovaBlockBytes;
  if (!blocks || arenaBytes % kIovaBlockBytes) return false;
  const uint32_t first = base >> 21;
  if ((base >> 30) != 0 || first + blocks > kL2Entries) return false;
  for (unsigned i = 0; i < kL1Entries; ++i) l1[i] = 0;
  for (unsigned i = 0; i < kL2Entries; ++i) l2[i] = 0;
  l1[0] = tableDesc(tablePa + 4096);
  for (uint32_t i = 0; i < blocks; ++i) l2[first + i] = blockDesc(arenaPa + uint64_t(i) * kIovaBlockBytes);
  w.tablePa = tablePa;
  w.l1 = l1;
  w.l2 = l2;
  w.base = base;
  w.bytes = blocks * kIovaBlockBytes;
  w.blocks = blocks;
  w.arenaPa = arenaPa;
  return true;
}

// What the table translates an address of the window to (0 when it is not mapped): the self-check before the chip is given any
// address of it.
inline uint64_t translate(const Window &w, uint64_t iova)
{
  if (!w.l1 || !w.l2 || (iova >> 32)) return 0;
  const uint64_t l1 = w.l1[(iova >> 30) & 3];
  if ((l1 & 3) != kDescTable) return 0;
  const uint64_t l2 = w.l2[(iova >> 21) & 511];
  if ((l2 & 3) != kDescBlock) return 0;
  return (l2 & kOutMask & ~(uint64_t(kIovaBlockBytes) - 1)) | (iova & (kIovaBlockBytes - 1));
}

// The chip's address for an offset in the window (0 when it is outside what was mapped).
inline uint64_t busOf(const Window &w, uint64_t offset)
{
  return offset < w.bytes ? uint64_t(w.base) + offset : 0;
}

// ---- the plan: a stream-match group and a context bank for the stream ----
struct Plan {
  unsigned groups = 0, banks = 0, pages = 0, slot = ~0u, bank = ~0u, pasize = 0;
  bool reuse = false;          // the stream's own exact entry is redirected in place (level 3 may have left a bypass one)
  uint32_t smr = 0, s2cr = 0, idr0 = 0, idr2 = 0, cb = 0;
  const char *error = nullptr;
};

template <class Read>
inline Plan plan(uint16_t stream, uint64_t windowBytes, Read rd)
{
  Plan p;
  p.idr0 = rd(kIdr0);
  p.idr2 = rd(kIdr2);
  const uint32_t idr1 = rd(kIdr1);
  if (!(p.idr0 & kIdr0S1ts) || !(p.idr2 & kIdr2Ptfs4k)) {
    p.error = "no stage 1 translation with 4 KiB pages";
    return p;
  }
  p.groups = p.idr0 & 0xffu;
  if (p.groups > 128) p.groups = 128;   // groups above 128 do not behave architecturally on this SMMU
  p.banks = idr1 & 0xffu;
  p.pages = 1u << (((idr1 >> 28) & 7u) + 1);
  if ((idr1 >> 31) || !p.groups || p.banks < 2 || p.banks > 128 || p.pages > 128 ||
      uint64_t(p.pages + p.banks) * 4096 > windowBytes) {
    p.error = "unsupported SMMU geometry";
    return p;
  }
  const unsigned oas = idr2Oas(p.idr2);
  if (oas < 1) {
    p.error = "the SMMU's output size is 32 bits: XNU's memory lies above 4 GiB";
    return p;
  }
  p.pasize = oas > 5 ? 5 : oas;
  uint64_t used[2] = {0, 0};
  unsigned freeSlot = ~0u, matches = 0;
  for (unsigned i = 0; i < p.groups; ++i) {
    const uint32_t smr = rd(smrOffset(i));
    if (!(smr & 0x80000000u)) {
      if (freeSlot == ~0u) freeSlot = i;
      continue;
    }
    const uint32_t s2cr = rd(s2crOffset(i)), mask = (smr >> 16) & 0x7fffu;
    if (((smr ^ stream) & 0x7fffu & ~mask) == 0) {
      ++matches;
      p.slot = i;
      p.smr = smr;
      p.s2cr = s2cr;
      continue;
    }
    if (((s2cr >> 16) & 3u) == 0) used[(s2cr & 127u) / 64] |= 1ull << ((s2cr & 127u) % 64);
  }
  if (matches > 1) {
    p.error = "the stream matches several stream-match groups";
    return p;
  }
  if (matches == 1) {
    if ((p.smr >> 16) & 0x7fffu) {
      p.error = "the stream sits in a masked stream-match group";
      return p;
    }
    p.reuse = true;
  } else if (freeSlot == ~0u) {
    p.error = "no free stream-match group";
    return p;
  } else {
    p.slot = freeSlot;
  }
  // The stream's own bank (a bypass bank level 3 gave it) counts as free; any other bank with translation on belongs to someone.
  const unsigned own = p.reuse && ((p.s2cr >> 16) & 3u) == 0 ? (p.s2cr & 255u) : ~0u;
  for (unsigned b = p.banks - 1; b-- > 0;) {
    if ((used[b / 64] >> (b % 64)) & 1u) continue;
    if ((rd((p.pages + b) * 4096 + kCbSctlr) & kSctlrM) && b != own) continue;
    p.bank = b;
    p.cb = (p.pages + b) * 4096;
    return p;
  }
  p.error = "no free context bank";
  return p;
}

// The bank programmed completely, then the one store that points the stream at it (and, for a new group, the match that makes the
// entry live). Returns nullptr on success, else what failed; nothing is left half-programmed with the stream pointing at it.
template <class Read, class Write, class Expired>
inline const char *apply(const Plan &p, uint16_t stream, uint64_t tablePa, Read rd, Write wr, Expired expired)
{
  if (p.error) return p.error;
  if (!tablePa || (tablePa & 0xfffu)) return "the table is not page aligned";
  if ((tablePa + kTableBytes - 1) >> kPaBits[p.pasize]) return "the table lies outside the SMMU's output size";
  const uint32_t cb = p.cb;
  wr(cb + kCbSctlr, 0);                      // translation off while the bank is programmed
  wr(cba2rOffset(p.bank), 1);                // 64-bit descriptors
  wr(cbarOffset(p.bank), kCbarStage1);
  wr(cb + kCbTcr2, tcr2Window(p.pasize));
  wr(cb + kCbTcr, kTcrWindow);
  wr(cb + kCbTtbr0Lo, uint32_t(tablePa));
  wr(cb + kCbTtbr0Hi, ttbr0Hi(tablePa, p.bank));
  wr(cb + kCbTtbr1Lo, 0);
  wr(cb + kCbTtbr1Lo + 4, 0);
  wr(cb + kCbContextIdr, 0);
  wr(cb + kCbMair0, kMair0Nc);
  wr(cb + kCbMair1, 0);
  wr(cb + kCbTlbiAll, 0xffffffffu);
  wr(cb + kCbTlbSync, 0xffffffffu);
  while (rd(cb + kCbTlbStatus) & 1u)
    if (expired()) return "context TLB sync timeout";
  const uint32_t fsr = rd(cb + kCbFsr);
  if (fsr & kFsrFaults) wr(cb + kCbFsr, fsr & kFsrFaults);   // stale faults cleared (write one to clear)
  wr(cb + kCbSctlr, kSctlrWindow);
  if ((rd(cb + kCbSctlr) & kSctlrRequired) != kSctlrRequired || rd(cba2rOffset(p.bank)) != 1 ||
      rd(cbarOffset(p.bank)) != kCbarStage1 || rd(cb + kCbTtbr0Lo) != uint32_t(tablePa) ||
      rd(cb + kCbTtbr0Hi) != ttbr0Hi(tablePa, p.bank) || rd(cb + kCbTcr) != kTcrWindow)
    return "context bank readback";
  wr(s2crOffset(p.slot), p.bank);            // translate through the bank
  if ((rd(s2crOffset(p.slot)) & 0x300ffu) != p.bank) return "stream context readback";
  if (!p.reuse) wr(smrOffset(p.slot), 0x80000000u | stream);
  if (rd(smrOffset(p.slot)) != (0x80000000u | stream)) return "stream match readback";
  wr(kTlbiAllNsNh, 0xffffffffu);
  wr(kTlbGSync, 0xffffffffu);
  while (rd(kTlbGStatus) & 1u)
    if (expired()) return "TLB sync timeout";
  return nullptr;
}

// The bank's fault registers, for the log after a failure that may have been a translation fault.
struct Fault {
  uint32_t fsr = 0, farLo = 0, farHi = 0, fsynr0 = 0, gfsr = 0;
};
template <class Read>
inline Fault fault(const Plan &p, Read rd)
{
  Fault f;
  if (p.error) return f;
  f.fsr = rd(p.cb + kCbFsr);
  f.farLo = rd(p.cb + 0x060);
  f.farHi = rd(p.cb + 0x064);
  f.fsynr0 = rd(p.cb + 0x068);
  f.gfsr = rd(0x048);
  return f;
}

}  // namespace iova
}  // namespace pianocamera
