// SPDX-License-Identifier: BSD-2-Clause-Patent
// The USB stream (SID 0x40) through its own stage-1 context bank of the apps SMMU with an identity map of DRAM, Normal write-back
// inner shareable: the DWC3's transactions then carry cacheable attributes and are coherent with the CPUs' caches (the DT marks
// usb@a600000 dma-coherent). Apple's USB controller drivers do no cache maintenance (Apple's fabric is coherent), so Apple's
// AppleUSBXHCI on our nub (level 6, docs/drivers/usb-host.md H6) needs this; a bypass bank (PianoDwc3's SmmuStream.h, which our
// own level 5 rings in a non-cacheable mapping are fine with) leaves the master non-coherent.
// After QcomDwc3's smmuIdentity() (unixnu-research/QcomDwc3 e4efe95, QcomDwc3.cpp, BSD-2-Clause-Patent; its SM8550 version on
// sheng): the same table (T0SZ 28, a 64 GiB input space walked from level 1, 1 GiB blocks from 2 GiB up, attribute 0 = 0xff
// write-back, inner shareable, AF, unprivileged read/write; nothing below 2 GiB, where piano has no DRAM: a stray access faults)
// and the same context bank words (CBA2R VA64, CBAR stage 1 with stage 2 bypass, TCR2 40-bit output, TCR walks cacheable inner
// shareable 4 KiB granule without TTBR1, MAIR0, SCTLR M/TRE/AFE/CFRE/ASIDPNE). Ours: the portable plan/apply split; an exact entry
// the firmware or PianoDwc3 left for the stream is redirected in place (QcomDwc3 refused it); the bank search skips every bank
// another stream uses and every bank with translation on, from one below the top (as PianoDwc3's planner); the TLB maintenance by
// the context's TLBIALL and the global TLBIALLNSNH with non-zero values (Qualcomm's trapped MMIO, PianoDwc3's note) and bounded
// waits; nothing panics. ARM SMMUv2 / MMU-500 programming model (register offsets: global space 0, GR1 at one page, context banks
// after the global pages). Portable: Read(offset) / Write(offset, value) on offsets from the SMMU base; Expired() bounds waits.
#pragma once
#include <stdint.h>

namespace pianousbhost {

struct IdentityPlan {
  unsigned groups = 0, banks = 0, pages = 0, slot = ~0u, bank = ~0u;
  bool reuse = false;          // the stream's own exact stream-match entry is redirected
  uint32_t smr = 0, s2cr = 0;  // what that entry held
  const char *error = nullptr;
};

// Context bank registers (offsets in the bank's page)
constexpr uint32_t kCbSctlr = 0x0, kCbTcr2 = 0x10, kCbTtbr0 = 0x20, kCbTtbr1 = 0x28, kCbTcr = 0x30, kCbContextIdr = 0x34,
                   kCbMair0 = 0x38, kCbMair1 = 0x3c, kCbFsr = 0x58, kCbTlbiAll = 0x618, kCbTlbSync = 0x7f0, kCbTlbStatus = 0x7f4;
constexpr uint32_t kSctlrIdentity = 1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12;   // M, TRE, AFE, CFRE, ASIDPNE
constexpr uint32_t kCbarStage1 = 1u << 16 | 0xfu << 12 | 3u << 8;                       // stage 1, stage 2 bypass; MemAttr, BPSHCFG
constexpr uint32_t kTcr2Identity = 2u | 7u << 15;                                       // 40-bit output, sign extension from bit 47
constexpr uint32_t kTcrIdentity = 28u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23;       // T0SZ 28, IRGN0/ORGN0 WB, SH0 inner, EPD1
constexpr unsigned kIdentityEntries = 64, kIdentityFirstGiB = 2;
constexpr uint64_t identityBlock(uint64_t gib) { return gib << 30 | 1ull << 10 | 3ull << 8 | 1ull << 6 | 1ull; }
// The level-1 table (one 4 KiB page): Put(index, descriptor).
template <class Put> void identityTable(Put put)
{
  for (unsigned i = 0; i < kIdentityEntries; ++i) put(i, i >= kIdentityFirstGiB ? identityBlock(i) : 0);
}

template <class Read> IdentityPlan planSmmuIdentity(uint16_t stream, uint64_t windowBytes, Read rd)
{
  IdentityPlan p;
  const uint32_t idr0 = rd(0x20), idr1 = rd(0x24), idr2 = rd(0x28);
  if (!(idr0 & (1u << 30)) || !(idr2 & (1u << 12))) {
    p.error = "no AArch64 stage 1 support";
    return p;
  }
  p.groups = idr0 & 0xffu;
  if (p.groups > 128) p.groups = 128;   // Qualcomm: groups above 128 do not behave architecturally
  p.banks = idr1 & 0xffu;
  p.pages = 1u << (((idr1 >> 28) & 7u) + 1);
  if ((idr1 >> 31) || !p.groups || p.banks < 2 || p.banks > 128 || p.pages > 128 || uint64_t(p.pages + p.banks) * 4096 > windowBytes) {
    p.error = "unsupported SMMU geometry";
    return p;
  }
  uint64_t used[2] = {0, 0};
  unsigned freeSlot = ~0u, matches = 0;
  for (unsigned i = 0; i < p.groups; ++i) {
    const uint32_t smr = rd(0x800 + 4 * i);
    if (!(smr & 0x80000000u)) {
      if (freeSlot == ~0u) freeSlot = i;
      continue;
    }
    const uint32_t s2cr = rd(0xc00 + 4 * i), mask = (smr >> 16) & 0x7fffu;
    if (((smr ^ stream) & 0x7fffu & ~mask) == 0) {
      ++matches;
      p.slot = i;
      p.smr = smr;
      p.s2cr = s2cr;
      continue;   // its bank (if any) is the stream's own: free to take over
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
  } else {
    if (freeSlot == ~0u) {
      p.error = "no free stream-match group";
      return p;
    }
    p.slot = freeSlot;
  }
  // The stream's own old bank may be the one: then it is free (not marked) but may have translation on only if it is ours already.
  const unsigned own = p.reuse && ((p.s2cr >> 16) & 3u) == 0 ? (p.s2cr & 255u) : ~0u;
  for (unsigned b = p.banks - 1; b-- > 0;) {   // banks - 2 down to 0
    if ((used[b / 64] >> (b % 64)) & 1u) continue;
    if ((rd((p.pages + b) * 4096 + kCbSctlr) & 1u) && b != own) continue;   // translation enabled by someone else
    p.bank = b;
    return p;
  }
  p.error = "no free context bank";
  return p;
}

// Returns nullptr on success, else what failed. tablePa: the level-1 table (identityTable()), cleaned to the point of coherency.
template <class Read, class Write, class Expired>
const char *applySmmuIdentity(const IdentityPlan &p, uint16_t stream, uint64_t tablePa, Read rd, Write wr, Expired expired)
{
  if (p.error) return p.error;
  if (!tablePa || (tablePa & 0xfffu)) return "the table is not page aligned";
  const uint32_t cb = (p.pages + p.bank) * 4096;
  // The new bank is programmed completely before the stream is switched to it (one S2CR store); until then a live entry keeps its
  // old context. Its old bank is left as it is (a bypass bank of PianoDwc3's or the firmware's).
  wr(cb + kCbSctlr, 0);   // translation off while it is programmed
  wr(0x1800 + 4 * p.bank, 1);
  wr(0x1000 + 4 * p.bank, kCbarStage1);
  wr(cb + kCbTcr2, kTcr2Identity);
  wr(cb + kCbTcr, kTcrIdentity);
  wr(cb + kCbTtbr0, uint32_t(tablePa));
  wr(cb + kCbTtbr0 + 4, uint32_t(tablePa >> 32));
  wr(cb + kCbTtbr1, 0);
  wr(cb + kCbTtbr1 + 4, 0);
  wr(cb + kCbContextIdr, 0);
  wr(cb + kCbMair0, 0xff);
  wr(cb + kCbMair1, 0);
  wr(cb + kCbTlbiAll, 0xffffffffu);
  wr(cb + kCbTlbSync, 0xffffffffu);
  while (rd(cb + kCbTlbStatus) & 1u)
    if (expired()) return "context TLB sync timeout";
  const uint32_t fsr = rd(cb + kCbFsr);
  if (fsr) wr(cb + kCbFsr, fsr);   // stale faults cleared (write-1-to-clear)
  wr(cb + kCbSctlr, kSctlrIdentity);
  if ((rd(cb + kCbSctlr) & kSctlrIdentity) != kSctlrIdentity || rd(0x1800 + 4 * p.bank) != 1 || rd(0x1000 + 4 * p.bank) != kCbarStage1 ||
      rd(cb + kCbTtbr0) != uint32_t(tablePa) || rd(cb + kCbTtbr0 + 4) != uint32_t(tablePa >> 32))
    return "context bank readback";
  wr(0xc00 + 4 * p.slot, p.bank);   // S2CR: translate through the bank (then, for a new group, the match)
  if ((rd(0xc00 + 4 * p.slot) & 0x300ffu) != p.bank) return "stream context readback";
  if (!p.reuse) wr(0x800 + 4 * p.slot, 0x80000000u | stream);
  if (rd(0x800 + 4 * p.slot) != (0x80000000u | stream)) return "stream match readback";
  wr(0x68, 0xffffffffu);   // TLBIALLNSNH
  wr(0x70, 0xffffffffu);   // sTLBGSYNC
  while (rd(0x74) & 1u)    // sTLBGSTATUS.GSACTIVE
    if (expired()) return "TLB sync timeout";
  return nullptr;
}

}  // namespace pianousbhost
