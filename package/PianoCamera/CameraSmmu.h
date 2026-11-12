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

// The window's words back as found: the stream first made to fault (so nothing reaches memory in between), the group's match as found,
// its S2CR as found, the bank's SCTLR (translation off), its other words and its CBAR/CBA2R as found, the TLBs invalidated. nullptr,
// or what failed.
template <class Read, class Write, class Expired>
inline const char *releaseRoute(const RoutePlan &p, Read rd, Write wr, Expired expired)
{
  if (p.route != Route::window) return nullptr;
  const uint32_t cb = p.window.cb;
  wr(0xc00 + 4 * p.slot, 2u << 16);                       // S2CR: fault
  if (!p.window.reuse) wr(0x800 + 4 * p.slot, p.foundSmr);   // SMR as found (a free group: not valid)
  wr(0xc00 + 4 * p.slot, p.foundS2cr);
  wr(cb + iova::kCbSctlr, p.foundSctlr);
  const uint32_t fsr = rd(cb + iova::kCbFsr);
  if (fsr & iova::kFsrFaults) wr(cb + iova::kCbFsr, fsr & iova::kFsrFaults);   // faults of ours (logged by the caller) cleared
  for (unsigned i = 0; i < 9; ++i) wr(cb + kBankWords[i], p.foundBank[i]);
  wr(0x1000 + 4 * p.bank, p.foundCbar);
  wr(0x1800 + 4 * p.bank, p.foundCba2r);
  wr(iova::kTlbiAllNsNh, 0xffffffffu);
  wr(iova::kTlbGSync, 0xffffffffu);
  while (rd(iova::kTlbGStatus) & 1u)
    if (expired()) return "TLB sync timeout";
  if (rd(0x800 + 4 * p.slot) != p.foundSmr || rd(0xc00 + 4 * p.slot) != p.foundS2cr || (rd(cb + iova::kCbSctlr) & 1u) != (p.foundSctlr & 1u))
    return "release readback";
  return nullptr;
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
