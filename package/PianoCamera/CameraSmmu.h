// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera level 3: the route of the camera's stream 0x1c00 (every CAMSS write master; DT camss iommus) through the apps SMMU, as
// level 1's survey finds it, and the frame buffer's bus address on that route:
//   bypass     one stream-match group covers 0x1c00 and its S2CR bypasses (type 1), or translates through a bank with translation
//              off (SCTLR.M = 0) and 64-bit addressing (CBA2R.VA64): the boot chain left it so; the write master gets physical
//              addresses (40-bit reach: address >> 8 in 32 bits); nothing is written to the SMMU.
//   window     no group covers it, or its own exact group faults (type 2) or translates through a bank with translation off but
//              32-bit addressing: a translating bank of ours (CameraSmmuWindow.h, PianoWifi's IOVA window) maps the frame buffer
//              (one contiguous, 2 MiB-aligned range) at 0x20000000; anything the write master puts outside it faults instead of
//              reaching memory. Released afterwards: every SMMU word it changed back as found.
//   refused    several groups, a masked group that does not bypass, a bank with translation on (someone's tables), an SMMU without
//              stage-1 4 KiB tables, no free group or bank: level 3 stops before any stream (nothing written).
// Portable over Read(offset from the SMMU base) / Write(offset, value) / Expired(); the window's apply and this release are the only
// writers; the owner routes every write through level 3's allowlist (the group, the bank, the global TLB invalidate and sync).
//
// Which SMMU: the boot DT's camss node names apps_smmu (iommu@15000000, phandle 0x28 in out/inputs/piano-boot.dtb) with stream
// 0x1c00 mask 0; the only other SMMU is the GPU's (iommu@3da0000). kSmmu is that instance. Its global and bank registers are the TCU's
// (not behind the camera NoC); the camera's TBU is (inferred), so the owner does the window's TLB maintenance only while MM1, GCC's
// camera HF AXI and the camera NoC's clocks are on (the apply after step 304, the release first at step 340).
//
// What the release may write back (c3rp/c3r, 2026-11-12): piano's apps SMMU is reached through the hypervisor (Qualcomm's trapped SMMU
// MMIO; IDR0 0x4c017e7f: stage 1 only, no stage 2). Both runs aborted ("Unhandled implementation specific error", XNU's platform-
// error panic) right at the release's store of the free group's S2CR as found, 0x0300006e: TYPE translate through bank 110 of the 83
// IDR1 reports, a value the architecture leaves unpredictable and the hypervisor refuses (inferred: the store is the last line logged
// before the abort; every earlier store of the window, 21 of them, was accepted). So a found word goes back only when it is a valid
// configuration of this SMMU (s2crRestorable, cbarRestorable); otherwise the word keeps the value of ours this SMMU already accepted
// (S2CR: fault; CBAR/CBA2R: our stage-1 words), which leaves the group and the bank as free as found for every plan in this repository
// (a free group is never matched: its SMR is invalid; a bank with SCTLR.M clear and no stream pointing at it is free).
#pragma once
#include <stdint.h>
#include "CameraHw.h"
#include "CameraSmmuWindow.h"

namespace pianocamera {

enum class Route : uint8_t { refused, bypass, bypassBank, window };
inline const char *routeName(Route r)
{
  switch (r) {
  case Route::refused: return "refused";
  case Route::bypass: return "bypass (S2CR)";
  case Route::bypassBank: return "bypass (a bank with translation off)";
  case Route::window: return "our translating window";
  }
  return "?";
}

struct RoutePlan {
  Route route = Route::refused;
  const char *why = nullptr;     // refused: why; else a description
  unsigned matches = 0, slot = ~0u, bank = ~0u;
  uint32_t smr = 0, s2cr = 0, sctlr = 0, cba2r = 0, scr0 = 0;
  iova::Plan window;             // route == window
  // What the window's apply changes, as found (restored by release):
  uint32_t foundSmr = 0, foundS2cr = 0, foundSctlr = 0, foundCbar = 0, foundCba2r = 0;
  uint32_t foundBank[9] = {};    // the bank's other words apply() writes (kBankWords), as found
};
// The bank words apply() writes besides SCTLR (restored by release, so the bank is left exactly as found).
constexpr uint32_t kBankWords[9] = {iova::kCbTcr2, iova::kCbTcr, iova::kCbTtbr0Lo, iova::kCbTtbr0Hi, iova::kCbTtbr1Lo,
                                    iova::kCbTtbr1Lo + 4, iova::kCbContextIdr, iova::kCbMair0, iova::kCbMair1};

// Loads only.
template <class Read>
inline RoutePlan planRoute(uint64_t smmuBytes, Read rd)
{
  RoutePlan p;
  p.scr0 = rd(0x0);
  const uint32_t idr0 = rd(0x20), idr1 = rd(0x24);
  unsigned groups = idr0 & 0xffu;
  if (groups > 128) groups = 128;
  const unsigned banks = idr1 & 0xffu, pages = 1u << (((idr1 >> 28) & 7u) + 1);
  if ((idr1 >> 31) || !groups || banks < 2 || banks > 128 || uint64_t(pages + banks) * 4096 > smmuBytes) {
    p.why = "unsupported SMMU geometry";
    return p;
  }
  for (unsigned i = 0; i < groups; ++i) {
    const uint32_t smr = rd(0x800 + 4 * i);
    if (!(smr & 0x80000000u)) continue;
    const uint32_t mask = (smr >> 16) & 0x7fffu;
    if (((smr ^ kCamStream) & 0x7fffu & ~mask) != 0) continue;
    ++p.matches;
    p.slot = i;
    p.smr = smr;
    p.s2cr = rd(0xc00 + 4 * i);
  }
  if (p.matches > 1) {
    p.why = "stream 0x1c00 matches several stream-match groups";
    return p;
  }
  if (p.matches == 1) {
    const uint32_t type = (p.s2cr >> 16) & 3u;
    const bool masked = ((p.smr >> 16) & 0x7fffu) != 0;
    if (type == 1) {
      p.route = Route::bypass;
      p.why = "the boot chain's group bypasses";
      return p;
    }
    if (type == 0) {
      p.bank = p.s2cr & 0xffu;
      if (p.bank >= banks) {
        p.why = "the group names a bank the SMMU does not have";
        return p;
      }
      p.sctlr = rd(uint64_t(pages + p.bank) * 4096);
      p.cba2r = rd(0x1800 + 4 * p.bank);
      if (p.sctlr & 1u) {
        p.why = "the group translates through a bank with translation on (someone's tables)";
        return p;
      }
      if (p.cba2r & 1u) {
        p.route = Route::bypassBank;
        p.why = "the boot chain's group uses a bank with translation off and 64-bit addressing";
        return p;
      }
    }
    if (masked) {
      p.why = "a masked stream-match group covers 0x1c00 and does not bypass";
      return p;
    }
    // Its own exact group, faulting or on a 32-bit bypass bank: redirected to our window (iova::plan's reuse), restored after.
  }
  p.window = iova::plan(kCamStream, smmuBytes, rd);
  if (p.window.error) {
    p.why = p.window.error;
    return p;
  }
  p.route = Route::window;
  p.slot = p.window.slot;
  p.bank = p.window.bank;
  p.foundSmr = rd(0x800 + 4 * p.slot);
  p.foundS2cr = rd(0xc00 + 4 * p.slot);
  p.foundSctlr = rd(p.window.cb);
  p.foundCbar = rd(0x1000 + 4 * p.bank);
  p.foundCba2r = rd(0x1800 + 4 * p.bank);
  for (unsigned i = 0; i < 9; ++i) p.foundBank[i] = rd(p.window.cb + kBankWords[i]);
  p.why = p.window.reuse ? "the stream's own group redirected to our bank" : "a free group and bank of ours";
  return p;
}

// IDR0: S1TS bit 30, S2TS bit 29 (ARM IHI 0062). S2CR TYPE 17:16 (0 translate through CBNDX 7:0, 1 bypass, 2 fault, 3 reserved);
// CBAR TYPE 17:16 (0 stage 2, 1 stage 1 with stage 2 bypass, 2 stage 1 with stage 2 fault, 3 nested).
constexpr uint32_t kIdr0S2ts = 1u << 29;
constexpr uint32_t kS2crFault = 2u << 16;
// A found S2CR is a configuration of this SMMU: bypass or fault, or translation through a bank it has (c3r's 0x0300006e is not).
inline bool s2crRestorable(uint32_t s2cr, unsigned banks)
{
  const uint32_t type = (s2cr >> 16) & 3u;
  return type == 1 || type == 2 || (type == 0 && (s2cr & 0xffu) < banks);
}
// A found CBAR is a context type this SMMU implements: stage 1 (stage 2 bypassed) with stage-1 support; any stage-2 kind only with
// stage-2 support (piano's IDR0 has none: a free bank's CBAR, which reads 0 = "stage 2", stays ours).
inline bool cbarRestorable(uint32_t cbar, uint32_t idr0)
{
  const uint32_t type = (cbar >> 16) & 3u;
  return type == 1 ? (idr0 & iova::kIdr0S1ts) != 0 : (idr0 & kIdr0S2ts) != 0;
}

struct Release {
  const char *error = nullptr;   // nullptr: done
  bool s2crKept = false;         // the found S2CR was not a valid configuration: the group's S2CR stays at fault
  bool cbarKept = false;         // the found CBAR was not: the bank's CBAR and CBA2R stay at our stage-1 words
  uint32_t s2cr = 0, cbar = 0, cba2r = 0;   // what the group and bank hold after the release
};

// The window's words back (see the header): the stream first made to fault (so nothing reaches memory in between), the group's match
// as found (a free group: invalid), its S2CR as found when restorable (else fault), the bank's SCTLR as found (translation off), its
// faults of ours cleared (the caller logged them), its other words as found, CBAR/CBA2R as found when restorable, then the TLBs
// invalidated and synchronised (the owner calls this with the camera NoC still voted and clocked, before the table pages are freed).
template <class Read, class Write, class Expired>
inline Release releaseRoute(const RoutePlan &p, Read rd, Write wr, Expired expired)
{
  Release out;
  if (p.route != Route::window) return out;
  const uint32_t cb = p.window.cb;
  out.s2crKept = !s2crRestorable(p.foundS2cr, p.window.banks);
  out.cbarKept = !cbarRestorable(p.foundCbar, p.window.idr0);
  out.s2cr = out.s2crKept ? kS2crFault : p.foundS2cr;
  out.cbar = out.cbarKept ? iova::kCbarStage1 : p.foundCbar;
  out.cba2r = out.cbarKept ? 1u : p.foundCba2r;
  wr(0xc00 + 4 * p.slot, kS2crFault);                        // S2CR: fault
  if (!p.window.reuse) wr(0x800 + 4 * p.slot, p.foundSmr);   // SMR as found (a free group: not valid)
  if (!out.s2crKept && p.foundS2cr != kS2crFault) wr(0xc00 + 4 * p.slot, p.foundS2cr);
  wr(cb + iova::kCbSctlr, p.foundSctlr & ~iova::kSctlrM);
  const uint32_t fsr = rd(cb + iova::kCbFsr);
  if (fsr & iova::kFsrFaults) wr(cb + iova::kCbFsr, fsr & iova::kFsrFaults);   // faults of ours (logged by the caller) cleared
  for (unsigned i = 0; i < 9; ++i) wr(cb + kBankWords[i], p.foundBank[i]);
  if (!out.cbarKept) {
    wr(0x1000 + 4 * p.bank, p.foundCbar);
    wr(0x1800 + 4 * p.bank, p.foundCba2r);
  }
  wr(iova::kTlbiAllNsNh, 0xffffffffu);
  wr(iova::kTlbGSync, 0xffffffffu);
  while (rd(iova::kTlbGStatus) & 1u)
    if (expired()) {
      out.error = "TLB sync timeout";
      return out;
    }
  if (rd(0x800 + 4 * p.slot) != p.foundSmr || rd(0xc00 + 4 * p.slot) != out.s2cr || (rd(cb + iova::kCbSctlr) & iova::kSctlrM))
    out.error = "release readback";
  return out;
}

// Whether level 3 may write an SMMU word: the group and bank the plan chose, the global TLB invalidate and its sync.
inline bool smmuWritable(const RoutePlan &p, uint32_t off)
{
  if (p.route != Route::window) return false;
  if (off == 0x800 + 4 * p.slot || off == 0xc00 + 4 * p.slot || off == 0x1000 + 4 * p.bank || off == 0x1800 + 4 * p.bank) return true;
  if (off == iova::kTlbiAllNsNh || off == iova::kTlbGSync) return true;
  return off >= p.window.cb && off < p.window.cb + 0x800 && !(off & 3);
}

}  // namespace pianocamera
