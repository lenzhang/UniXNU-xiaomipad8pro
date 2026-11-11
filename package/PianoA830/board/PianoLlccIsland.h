// SPDX-License-Identifier: BSD-2-Clause-Patent
// The LLCC ISLAND1 slice (SCID 12), the ADSP's island cache, as CX collapse needs it (docs/drivers/gpu.md "The LLCC island",
// docs/power.md §3.6). Facts (docs/ubuntu-history/kernel.md §2.2, Linux drivers/soc/qcom/llcc-qcom.c read as the specification only):
// SM8750 has the LLCC v6 layout; slice n's TRP_ATTR0 (reserved ways) is at broadcast base 0x26800000 + 0x41000 + 64 * n, ATTR1 (bonus
// ways) at +8, ATTR2 (cache mode, probe target ways, fixed size bit 8, priority from bit 12) at +0x10, ATTR3 (the capacity per bank in
// 64-byte lines) at +0x14; COMMON_HW_INFO at +0x64000 (version, >= 6.0); COMMON_STATUS0 at channel 0 (0x24800000) + 0x6400c, the bank
// count in bits 31:28; TRP_STATUS of slice n at n * 0x1000 + 4 (bit 0 active). ISLAND1 = SCID 12, 7936 KiB, priority 7, fixed size,
// reserved ways 0x7fffffff, no bonus ways: so 0x26841300 must read 0x7fffffff and 0x26841308 0. With the LPASS votes released and the
// ways only as bonus ways, 16 of 16 suspends of Ubuntu on this tablet ended in a reset; with the reserved ways 6 of 6 resumed.
//
// Two paths (PianoA830's starter thread, any GPU boot):
//  - read(): read only, always: COMMON_HW_INFO, ATTR0..3 and TRP_STATUS of slice 12, all through the broadcast-OR view (what a read
//    there returns is the OR of the banks). Linux reads the same registers under Gunyah.
//  - program() (piano-llcc-island=1, piano-gpu=2 only): ATTR0 = 0x7fffffff and ATTR1 = 0 through the broadcast-OR view (a write there
//    reaches every bank), then ATTR2 and ATTR3 only where they read 0 (the facts' fixed size, priority 7, 7936 KiB over the banks), as
//    llcc-qcom programs the slice; the per-slice algorithm bits are never written. Refused, with nothing written, when the block does
//    not answer as LLCC v6 (all ones, a version below 6.0, no bank count: not accessible under Gunyah), or when the slice is active on
//    any bank (its user has it; it is not changed under it). Verified by reading back ATTR0 through both views (OR and AND: every bank
//    holds every reserved way) and ATTR1 through the OR view (no bank has a bonus way), and ATTR2/ATTR3 where written; a read-back that
//    does not match (writes the hypervisor ignores or masks) puts the values found back and reports it.
//  - ready(): the two words read again (OR view): reserved ways 0x7fffffff and no bonus way.
// Portable (no IOKit): tests/test_llcc_island.cpp runs it on a four-bank model. The kext maps one page per register group (PianoA830.cpp).
#pragma once
#include <stdint.h>

namespace pianollcc {

enum Block : uint8_t { kOr, kAnd, kChannel0, kBlocks };
constexpr uint64_t kBlockPhys[kBlocks] = {0x26800000ull, 0x26c00000ull, 0x24800000ull};
constexpr uint32_t kScid = 12;
constexpr uint32_t kAttr0 = 0x41000 + 64 * kScid, kAttr1 = kAttr0 + 8, kAttr2 = kAttr0 + 0x10, kAttr3 = kAttr0 + 0x14;
constexpr uint32_t kHwInfo = 0x64000, kStatus0 = 0x6400c, kTrpStatus = kScid * 0x1000 + 4;
constexpr uint32_t kReservedWays = 0x7fffffff, kBonusWays = 0;
constexpr uint32_t kVersion6 = 0x06000000, kActive = 1u << 0;
constexpr uint32_t kMaxCapKib = 7936, kPriority = 7;
constexpr uint32_t kAttr2Want = 1u << 8 | kPriority << 12;   // fixed size, priority 7 (cache mode and probe target ways 0)
static_assert(kAttr0 == 0x41300 && kAttr1 == 0x41308 && kTrpStatus == 0xc004, "the facts' offsets");
static_assert(kBlockPhys[kOr] + kAttr0 == 0x26841300ull && kBlockPhys[kOr] + kAttr1 == 0x26841308ull, "0x26841300 / 0x26841308");

// ATTR3: the slice's capacity per bank in 64-byte lines.
constexpr uint32_t attr3For(uint32_t banks) { return banks ? uint32_t((uint64_t(kMaxCapKib) * 1024 / banks) >> 6) : 0; }
static_assert(attr3For(4) == 31744, "7936 KiB over four banks");

// 32-bit accesses at a byte offset of one block; false = the access failed (host models; the kext's never fail).
struct Io {
  void *context;
  bool (*read)(void *context, Block block, uint32_t offset, uint32_t *value);
  bool (*write)(void *context, Block block, uint32_t offset, uint32_t value);
};

struct Reading {
  bool ok = false;          // every read answered
  uint32_t hwInfo = 0, attr[4] = {}, status = 0;
};

// The verdict on a reading.
enum class State : uint8_t { unknown, notAccessible, notReserved, reserved };
inline State judge(const Reading &r)
{
  if (!r.ok) return State::notAccessible;
  // A block that is not there (or not mapped for us) reads all ones; an LLCC older than v6 has another layout.
  if (r.hwInfo == 0xffffffffu || r.hwInfo < kVersion6 || (r.attr[0] == 0xffffffffu && r.attr[1] == 0xffffffffu)) return State::notAccessible;
  return r.attr[0] == kReservedWays && r.attr[1] == kBonusWays ? State::reserved : State::notReserved;
}
inline const char *stateName(State s)
{
  switch (s) {
  case State::notAccessible: return "not accessible";
  case State::notReserved: return "NOT reserved";
  case State::reserved: return "reserved";
  default: return "not read";
  }
}

inline Reading read(const Io &io)
{
  Reading r;
  r.ok = io.read(io.context, kOr, kHwInfo, &r.hwInfo) && io.read(io.context, kOr, kAttr0, &r.attr[0]) &&
         io.read(io.context, kOr, kAttr1, &r.attr[1]) && io.read(io.context, kOr, kAttr2, &r.attr[2]) &&
         io.read(io.context, kOr, kAttr3, &r.attr[3]) && io.read(io.context, kOr, kTrpStatus, &r.status);
  return r;
}

// The two words alone, again (any context: two loads).
inline bool ready(const Io &io)
{
  uint32_t reserved = 0, bonus = 0xffffffffu;
  return io.read(io.context, kOr, kAttr0, &reserved) && io.read(io.context, kOr, kAttr1, &bonus) && reserved == kReservedWays &&
         bonus == kBonusWays;
}

enum class Outcome : uint8_t {
  alreadyReserved,   // found as the facts say: nothing written
  programmed,        // written and read back on every view
  refusedAccess,     // the block does not answer as LLCC v6: nothing written
  refusedActive,     // slice 12 is active on a bank: nothing written
  refusedBanks,      // ATTR3 had to be written and the bank count is not 1..15: nothing written
  readbackFailed,    // a write did not read back: the values found were put back (restored says whether that read back)
};
inline const char *outcomeName(Outcome o)
{
  switch (o) {
  case Outcome::alreadyReserved: return "already reserved, nothing written";
  case Outcome::programmed: return "programmed and read back";
  case Outcome::refusedAccess: return "REFUSED: the LLCC does not answer as v6 here (not accessible): nothing written";
  case Outcome::refusedActive: return "REFUSED: slice 12 is active (its user has it): nothing written";
  case Outcome::refusedBanks: return "REFUSED: no bank count for ATTR3: nothing written";
  default: return "READ-BACK FAILED (the writes did not stick): the values found were put back";
  }
}

struct Program {
  Outcome outcome = Outcome::refusedAccess;
  Reading before, after;
  uint32_t banks = 0;
  uint32_t andAttr0 = 0;     // ATTR0 through the AND view after the writes
  uint32_t writes = 0;
  bool wroteAttr2 = false, wroteAttr3 = false;
  bool restored = false;     // readbackFailed: the values found read back after the restore
  uint32_t badOffset = 0, badValue = 0, badExpect = 0;   // the first read-back that did not match
};

inline Program program(const Io &io)
{
  Program p;
  p.before = read(io);
  if (judge(p.before) == State::notAccessible) return p;
  if (judge(p.before) == State::reserved) {
    uint32_t all = 0;
    if (io.read(io.context, kAnd, kAttr0, &all) && all == kReservedWays) {
      p.andAttr0 = all;
      p.after = p.before;
      p.outcome = Outcome::alreadyReserved;
      return p;
    }
    // Some bank lacks a reserved way the OR view shows: program it like the others.
  }
  if (p.before.status & kActive) {
    p.outcome = Outcome::refusedActive;
    return p;
  }
  const bool attr2 = p.before.attr[2] == 0, attr3 = p.before.attr[3] == 0;
  if (attr3) {
    uint32_t status0 = 0;
    if (!io.read(io.context, kChannel0, kStatus0, &status0)) return p;   // refusedAccess
    p.banks = status0 >> 28;
    if (!p.banks || p.banks > 15) {
      p.outcome = Outcome::refusedBanks;
      return p;
    }
  }
  struct Write {
    uint32_t offset, value;
    bool on;
  };
  const Write writes[] = {{kAttr0, kReservedWays, true}, {kAttr1, kBonusWays, true}, {kAttr2, kAttr2Want, attr2},
                          {kAttr3, attr3For(p.banks), attr3}};
  bool failed = false;
  for (const Write &w : writes)
    if (w.on) {
      ++p.writes;
      if (!io.write(io.context, kOr, w.offset, w.value)) failed = true;
    }
  p.wroteAttr2 = attr2;
  p.wroteAttr3 = attr3;
  p.after = read(io);
  uint32_t all = 0;
  const bool andOk = io.read(io.context, kAnd, kAttr0, &all);
  p.andAttr0 = all;
  auto bad = [&](uint32_t offset, uint32_t value, uint32_t expect) {
    if (!failed) {
      p.badOffset = offset;
      p.badValue = value;
      p.badExpect = expect;
    }
    failed = true;
  };
  if (!p.after.ok || !andOk) bad(kAttr0, 0xffffffffu, kReservedWays);
  if (p.after.attr[0] != kReservedWays) bad(kAttr0, p.after.attr[0], kReservedWays);
  if (all != kReservedWays) bad(kAttr0, all, kReservedWays);   // the AND view: a bank without every reserved way
  if (p.after.attr[1] != kBonusWays) bad(kAttr1, p.after.attr[1], kBonusWays);
  if (attr2 && p.after.attr[2] != kAttr2Want) bad(kAttr2, p.after.attr[2], kAttr2Want);
  if (attr3 && p.after.attr[3] != attr3For(p.banks)) bad(kAttr3, p.after.attr[3], attr3For(p.banks));
  if (!failed) {
    p.outcome = Outcome::programmed;
    return p;
  }
  // Put back what was found, in the reverse order, and see whether it stuck.
  p.outcome = Outcome::readbackFailed;
  if (attr3) io.write(io.context, kOr, kAttr3, p.before.attr[3]);
  if (attr2) io.write(io.context, kOr, kAttr2, p.before.attr[2]);
  io.write(io.context, kOr, kAttr1, p.before.attr[1]);
  io.write(io.context, kOr, kAttr0, p.before.attr[0]);
  const Reading back = read(io);
  p.restored = back.ok && back.attr[0] == p.before.attr[0] && back.attr[1] == p.before.attr[1] && back.attr[2] == p.before.attr[2] &&
               back.attr[3] == p.before.attr[3];
  p.after = back;
  return p;
}

}  // namespace pianollcc
