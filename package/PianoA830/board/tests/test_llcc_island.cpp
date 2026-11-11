// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoLlccIsland.h on a four-bank LLCC model: a read through the broadcast-OR view returns the OR of the banks, through
// the AND view their AND, a write through the OR view reaches every bank; COMMON_HW_INFO, COMMON_STATUS0 (the bank count) and slice
// 12's TRP_STATUS. Cases: the firmware's bonus-only table (ATTR0/ATTR1 written, ATTR2/ATTR3 kept), an unconfigured slice (all four
// written, ATTR3 from the bank count), already reserved (nothing written), one bank short of a reserved way (seen through the AND
// view: programmed), the slice active (refused, nothing written), a block that reads all ones or is older than v6 (refused), writes
// the hypervisor ignores or masks (read-back failure, the values found put back), no bank count when ATTR3 is needed (refused), a
// failed read, and ready() before and after.
#include "PianoLlccIsland.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace pianollcc;

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

struct Model {
  static constexpr unsigned kBanks = 4;
  uint32_t attr[kBanks][4] = {};
  uint32_t status[kBanks] = {};
  uint32_t hwInfo = 0x06010000;
  uint32_t status0 = 4u << 28;
  bool allOnes = false;        // the block is not there for us
  bool ignoreWrites = false;   // the hypervisor drops the stores
  uint32_t writeMask = 0xffffffffu;   // ... or masks them
  bool failReads = false;
  unsigned writes = 0, reads = 0;
  void bankFill(unsigned slot, uint32_t value) {for (unsigned b = 0; b < kBanks; ++b) attr[b][slot] = value;}
};
static Model m;

static int slotOf(uint32_t offset)
{
  switch (offset) {
  case kAttr0: return 0;
  case kAttr1: return 1;
  case kAttr2: return 2;
  case kAttr3: return 3;
  default: return -1;
  }
}

static bool rd(void *, Block block, uint32_t offset, uint32_t *value)
{
  ++m.reads;
  if (m.failReads) return false;
  if (m.allOnes) {*value = 0xffffffffu;return true;}
  if (block == kChannel0) {
    if (offset != kStatus0) return false;
    *value = m.status0;
    return true;
  }
  if (offset == kHwInfo) {*value = m.hwInfo;return true;}
  uint32_t acc = block == kAnd ? 0xffffffffu : 0;
  const int slot = slotOf(offset);
  for (unsigned b = 0; b < Model::kBanks; ++b) {
    uint32_t v;
    if (slot >= 0) v = m.attr[b][slot];
    else if (offset == kTrpStatus) v = m.status[b];
    else return false;
    acc = block == kAnd ? acc & v : acc | v;
  }
  *value = acc;
  return true;
}

static bool wr(void *, Block block, uint32_t offset, uint32_t value)
{
  ++m.writes;
  if (block != kOr) return false;   // the island code only writes through the OR view
  const int slot = slotOf(offset);
  if (slot < 0) return false;
  if (!m.ignoreWrites)
    for (unsigned b = 0; b < Model::kBanks; ++b) m.attr[b][slot] = value & m.writeMask;
  return true;
}

static const Io io = {nullptr, rd, wr};

static void reset() {m = Model();}

int main()
{
  // The read-only line's reading and verdicts.
  reset();
  m.bankFill(1, 0x7fffffffu);m.bankFill(2, kAttr2Want);m.bankFill(3, 31744);
  Reading r = read(io);
  CHECK(r.ok && r.hwInfo == 0x06010000 && r.attr[0] == 0 && r.attr[1] == 0x7fffffffu && r.attr[2] == kAttr2Want && r.attr[3] == 31744);
  CHECK(judge(r) == State::notReserved && m.writes == 0 && !ready(io));
  CHECK(!std::strcmp(stateName(judge(r)), "NOT reserved"));

  // The firmware's bonus-only table: ATTR0 and ATTR1 written, ATTR2/ATTR3 kept; every view reads back; ready.
  Program p = program(io);
  CHECK(p.outcome == Outcome::programmed && p.writes == 2 && !p.wroteAttr2 && !p.wroteAttr3 && p.banks == 0);
  CHECK(p.after.attr[0] == kReservedWays && p.after.attr[1] == 0 && p.andAttr0 == kReservedWays && judge(p.after) == State::reserved);
  for (unsigned b = 0; b < Model::kBanks; ++b) CHECK(m.attr[b][0] == kReservedWays && m.attr[b][1] == 0 && m.attr[b][2] == kAttr2Want);
  CHECK(ready(io));

  // Again: already reserved, nothing written.
  const unsigned before = m.writes;
  p = program(io);
  CHECK(p.outcome == Outcome::alreadyReserved && m.writes == before && p.writes == 0);

  // An unconfigured slice: all four written, ATTR3 = 7936 KiB over the four banks.
  reset();
  p = program(io);
  CHECK(p.outcome == Outcome::programmed && p.writes == 4 && p.wroteAttr2 && p.wroteAttr3 && p.banks == 4);
  CHECK(m.attr[2][2] == 0x7100 && m.attr[3][3] == 31744 && attr3For(2) == 63488);

  // One bank short of a reserved way: the OR view looks reserved, the AND view does not: programmed like the others.
  reset();
  m.bankFill(0, kReservedWays);m.attr[2][0] = 0x3fffffff;m.bankFill(2, kAttr2Want);m.bankFill(3, 31744);
  CHECK(judge(read(io)) == State::reserved);
  p = program(io);
  CHECK(p.outcome == Outcome::programmed && p.writes == 2 && m.attr[2][0] == kReservedWays && p.andAttr0 == kReservedWays);

  // The slice active on a bank: refused, nothing written.
  reset();
  m.bankFill(1, 0xffffffffu);m.status[3] = kActive;
  p = program(io);
  CHECK(p.outcome == Outcome::refusedActive && m.writes == 0 && m.attr[0][1] == 0xffffffffu);
  CHECK(!std::strncmp(outcomeName(p.outcome), "REFUSED", 7));

  // A block that reads all ones, or an LLCC older than v6: not accessible, nothing written.
  reset();
  m.allOnes = true;
  CHECK(judge(read(io)) == State::notAccessible && !ready(io));
  p = program(io);
  CHECK(p.outcome == Outcome::refusedAccess && m.writes == 0);
  reset();
  m.hwInfo = 0x05000000;
  CHECK(judge(read(io)) == State::notAccessible);
  p = program(io);
  CHECK(p.outcome == Outcome::refusedAccess && m.writes == 0);

  // Writes the hypervisor ignores: the read-back fails, the values found are written back (and read back), not ready.
  reset();
  m.bankFill(1, 0xffffffffu);m.bankFill(2, kAttr2Want);m.bankFill(3, 31744);
  m.ignoreWrites = true;
  p = program(io);
  CHECK(p.outcome == Outcome::readbackFailed && p.restored && p.badOffset == kAttr0 && p.badValue == 0 && p.badExpect == kReservedWays);
  CHECK(m.attr[0][1] == 0xffffffffu && !ready(io));
  // ... or masks them: the values found are written back, and the report says that did not read back either.
  reset();
  m.bankFill(1, 0xffffffffu);m.bankFill(2, kAttr2Want);m.bankFill(3, 31744);
  m.writeMask = 0x0fffffffu;
  p = program(io);
  CHECK(p.outcome == Outcome::readbackFailed && !p.restored && p.badOffset == kAttr0 && p.badValue == 0x0fffffffu);
  CHECK(m.attr[1][0] == 0 && m.attr[1][1] == 0x0fffffffu && judge(p.after) == State::notReserved && !ready(io));

  // ATTR3 needed and no bank count: refused before any write.
  reset();
  m.status0 = 0;
  p = program(io);
  CHECK(p.outcome == Outcome::refusedBanks && m.writes == 0);

  // A failed read: not accessible.
  reset();
  m.failReads = true;
  CHECK(!read(io).ok && judge(read(io)) == State::notAccessible && !ready(io));
  CHECK(program(io).outcome == Outcome::refusedAccess && m.writes == 0);

  std::printf("PASS: LLCC ISLAND1 (SCID 12) on a four-bank model: the read-only reading and its verdicts, the bonus-only and the "
              "unconfigured slice programmed (ATTR2/ATTR3 only where 0), already reserved untouched, a bank short seen through the AND "
              "view, refusals (active, not v6, all ones, no bank count, a failed read) with nothing written, ignored and masked writes "
              "put back, ready() (%u checks)\n", checks);
  return 0;
}
