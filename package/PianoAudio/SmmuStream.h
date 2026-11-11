// SPDX-License-Identifier: BSD-2-Clause-Patent
// package/PianoDwc3/SmmuStream.h of this repository (UPSTREAM has its sha256), with the namespace changed to pianoaudio and
// this line added: PianoDwc3 is not modified, so level 6 carries its own copy for the DSP's PCM streams 0x1001/0x1081/0x1041/0x1061.
// The USB stream (SID 0x40) through a bypass context bank of the apps SMMU (ARM SMMUv2 / MMU-500 programming model): a bank
// with translation off (SCTLR.M = 0) and VA64 (CBA2R bit 0, so DMA above 4 GiB passes), the stream's S2CR pointing at it.
// Modelled on UniXNU-sm8750-common kits/Sm8750Platform/SmmuTakeover.h (BSD-2-Clause-Patent), which NativeUfs uses for SID
// 0x60 on piano: same bank shape, same global TLB sync with a non-zero value (Qualcomm's trapped MMIO). Differences: an exact
// entry the firmware left for the stream is redirected (or kept, when it already bypasses with VA64), the search starts one
// bank below the top (NativeUfs's first choice), and nothing here panics. Portable: Read(offset) / Write(offset, value) work
// on offsets from the SMMU base; a Write must not store from the zero register (the caller's asm guarantees it).
#pragma once
#include <stdint.h>

namespace pianoaudio {

struct SmmuPlan {
  unsigned groups = 0, banks = 0, pages = 0, slot = ~0u, bank = ~0u;
  bool reuse = false;      // the stream already has its own exact stream-match entry (slot)
  bool keep = false;       // that entry already bypasses (S2CR bypass, or a bank with M = 0 and VA64): nothing to write
  uint32_t smr = 0, s2cr = 0, sctlr = 0, cbar = 0, cba2r = 0;   // what the existing entry / its bank held
  const char *error = nullptr;
};

template <class Read>
SmmuPlan planSmmuStream(uint16_t stream, uint64_t windowBytes, Read rd)
{
  SmmuPlan p;
  const uint32_t idr0 = rd(0x20), idr1 = rd(0x24), idr2 = rd(0x28);
  if (!(idr0 & (1u << 30)) || !(idr2 & (1u << 12))) { p.error = "no AArch64 stage 1 support"; return p; }
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
    if (((s2cr >> 16) & 3u) == 0) used[(s2cr & 127u) / 64] |= 1ull << ((s2cr & 127u) % 64);
    if (((smr ^ stream) & 0x7fffu & ~mask) == 0) {
      ++matches;
      p.slot = i;
      p.smr = smr;
      p.s2cr = s2cr;
    }
  }
  if (matches > 1) { p.error = "the stream matches several stream-match groups"; return p; }
  if (matches == 1) {
    if ((p.smr >> 16) & 0x7fffu) { p.error = "the stream sits in a masked stream-match group"; return p; }
    p.reuse = true;
    const uint32_t type = (p.s2cr >> 16) & 3u;
    if (type == 1) { p.keep = true; return p; }
    if (type == 0) {
      const unsigned b = p.s2cr & 255u;
      if (b < p.banks) {
        p.sctlr = rd((p.pages + b) * 4096);
        p.cbar = rd(0x1000 + 4 * b);
        p.cba2r = rd(0x1800 + 4 * b);
        if (!(p.sctlr & 1u) && (p.cba2r & 1u)) { p.keep = true; p.bank = b; return p; }
      }
    }
  } else {
    if (freeSlot == ~0u) { p.error = "no free stream-match group"; return p; }
    p.slot = freeSlot;
  }
  for (unsigned b = p.banks - 1; b-- > 0;) {   // banks - 2 down to 0
    if ((used[b / 64] >> (b % 64)) & 1u) continue;
    if (rd((p.pages + b) * 4096) & 1u) continue;   // translation enabled by someone
    p.bank = b;
    return p;
  }
  p.error = "no free context bank";
  return p;
}

// Returns nullptr on success, else what failed. Expired() bounds the TLB sync wait.
template <class Read, class Write, class Expired>
const char *applySmmuStream(const SmmuPlan &p, uint16_t stream, Read rd, Write wr, Expired expired)
{
  if (p.error) return p.error;
  if (p.keep) return nullptr;
  const uint32_t cb = (p.pages + p.bank) * 4096;
  wr(cb, 0);                                // SCTLR: translation off
  wr(0x1800 + 4 * p.bank, 1);               // CBA2R: VA64
  wr(0x1000 + 4 * p.bank, 0x10000);         // CBAR: stage 1 context, stage 2 bypass
  if (rd(cb) & 1u || rd(0x1800 + 4 * p.bank) != 1 || rd(0x1000 + 4 * p.bank) != 0x10000) return "bypass bank readback";
  wr(0xc00 + 4 * p.slot, p.bank);           // S2CR: translate through the bypass bank
  if ((rd(0xc00 + 4 * p.slot) & 0x300ffu) != p.bank) return "stream context readback";
  if (!p.reuse) wr(0x800 + 4 * p.slot, 0x80000000u | stream);
  if (rd(0x800 + 4 * p.slot) != (0x80000000u | stream)) return "stream match readback";
  wr(0x68, 0xffffffffu);                    // TLBIALLNSNH (mappings and tables kept)
  wr(0x70, 0xffffffffu);                    // sTLBGSYNC
  while (rd(0x74) & 1u)                     // sTLBGSTATUS.GSACTIVE
    if (expired()) return "TLB sync timeout";
  return nullptr;
}

}  // namespace pianoaudio
