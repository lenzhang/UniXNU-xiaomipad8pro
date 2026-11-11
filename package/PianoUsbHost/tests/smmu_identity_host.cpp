// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of SmmuIdentity.h on an SMMU register model shaped like piano's apps SMMU after NativeUfs (IDR1 0x60000053: 83 banks,
// 128 pages; SID 0x60 on bank 82, as PianoDwc3's tests/smmu_stream_host.cpp): the bank and group chosen, the context bank words,
// the order (the bank complete and translating before the stream points at it, the match last for a new group), the redirect of an
// exact entry PianoDwc3's bypass left, the refusals, the TLB waits; and the identity table's descriptors.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. smmu_identity_host.cpp -o /tmp/smmu_identity_host && /tmp/smmu_identity_host
#include "SmmuIdentity.h"
#include <cstdio>
#include <map>
#include <vector>

using namespace pianousbhost;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Smmu {
  std::map<uint32_t, uint32_t> r;
  std::vector<std::pair<uint32_t, uint32_t>> log;
  Smmu()
  {
    r[0x20] = 1u << 30 | 64;
    r[0x24] = 0x60000053;
    r[0x28] = 1u << 12;
    r[0x800 + 4 * 1] = 0x80000060;   // NativeUfs: SID 0x60 on bank 82 (translation off, VA64)
    r[0xc00 + 4 * 1] = 82;
    r[0x1000 + 4 * 82] = 0x10000;
    r[0x1800 + 4 * 82] = 1;
  }
  static uint32_t cb(unsigned bank) { return (128 + bank) * 4096; }
  auto reader() { return [this](uint32_t o) { return r[o]; }; }
  auto writer() { return [this](uint32_t o, uint32_t v) { log.push_back({o, v}); r[o] = v; }; }
  int at(uint32_t o) const
  {
    for (size_t i = 0; i < log.size(); ++i)
      if (log[i].first == o) return int(i);
    return -1;
  }
};

int main()
{
  const auto never = [] { return false; };
  const uint64_t table = 0x0000000881234000ull;
  {  // nothing for SID 0x40: a free group, the bank below NativeUfs's
    Smmu s;
    const IdentityPlan p = planSmmuIdentity(0x40, 0x100000, s.reader());
    CHECK(!p.error && !p.reuse && p.slot == 0 && p.bank == 81 && p.pages == 128 && p.banks == 83);
    CHECK(applySmmuIdentity(p, 0x40, table, s.reader(), s.writer(), never) == nullptr);
    const uint32_t cb = Smmu::cb(81);
    CHECK(s.r[cb + kCbSctlr] == kSctlrIdentity && s.r[0x1800 + 4 * 81] == 1 && s.r[0x1000 + 4 * 81] == kCbarStage1);
    CHECK(s.r[cb + kCbTcr] == kTcrIdentity && s.r[cb + kCbTcr2] == kTcr2Identity && s.r[cb + kCbMair0] == 0xff);
    CHECK(s.r[cb + kCbTtbr0] == 0x81234000u && s.r[cb + kCbTtbr0 + 4] == 0x8);
    CHECK(s.r[0x800] == 0x80000040 && s.r[0xc00] == 81);
    CHECK(s.r[0x804] == 0x80000060 && s.r[0xc04] == 82);   // UFS untouched
    // Order: SCTLR off first, the bank translating before S2CR, S2CR before the match, the global sync last.
    CHECK(s.at(cb + kCbSctlr) == 0);
    int sctlrOn = -1;
    for (size_t i = 0; i < s.log.size(); ++i)
      if (s.log[i].first == cb + kCbSctlr && s.log[i].second == kSctlrIdentity) sctlrOn = int(i);
    CHECK(sctlrOn > 0 && s.at(0xc00) > sctlrOn && s.at(0x800) > s.at(0xc00) && s.at(0x70) > s.at(0x800));
    // No store from a zero register is the kernel's business; the TLB invalidates carry non-zero values.
    CHECK(s.r[cb + kCbTlbiAll] == 0xffffffffu && s.r[cb + kCbTlbSync] == 0xffffffffu && s.r[0x68] == 0xffffffffu);
  }
  {  // PianoDwc3's bypass entry for 0x40 on bank 81 (translation off, VA64): redirected in place; bank 81 is the stream's own: taken
    Smmu s;
    s.r[0x800 + 4 * 5] = 0x80000040;
    s.r[0xc00 + 4 * 5] = 81;
    s.r[0x1000 + 4 * 81] = 0x10000;
    s.r[0x1800 + 4 * 81] = 1;
    const IdentityPlan p = planSmmuIdentity(0x40, 0x100000, s.reader());
    CHECK(!p.error && p.reuse && p.slot == 5 && p.bank == 81);
    CHECK(applySmmuIdentity(p, 0x40, table, s.reader(), s.writer(), never) == nullptr);
    CHECK(s.r[0xc00 + 4 * 5] == 81 && s.r[0x800 + 4 * 5] == 0x80000040 && s.at(0x800 + 4 * 5) < 0);   // the match not rewritten
    CHECK(s.r[Smmu::cb(81) + kCbSctlr] == kSctlrIdentity);
  }
  {  // the stream's entry on a bank another stream shares: that bank is skipped
    Smmu s;
    s.r[0x800 + 4 * 5] = 0x80000040;
    s.r[0xc00 + 4 * 5] = 81;
    s.r[0x800 + 4 * 6] = 0x80000123;
    s.r[0xc00 + 4 * 6] = 81;
    const IdentityPlan p = planSmmuIdentity(0x40, 0x100000, s.reader());
    CHECK(!p.error && p.reuse && p.bank == 80);
  }
  {  // banks with translation on (someone else's) are skipped; none left: refused, nothing written
    Smmu s;
    for (unsigned b = 0; b < 82; ++b) s.r[Smmu::cb(b)] = 1;
    const IdentityPlan p = planSmmuIdentity(0x40, 0x100000, s.reader());
    CHECK(p.error != nullptr);
    CHECK(applySmmuIdentity(p, 0x40, table, s.reader(), s.writer(), never) == p.error && s.log.empty());
  }
  {  // a masked group covering the stream; two groups matching it: refused
    Smmu s;
    s.r[0x800 + 4 * 3] = 0x80000000u | 0xfu << 16 | 0x40;
    CHECK(planSmmuIdentity(0x40, 0x100000, s.reader()).error != nullptr);
    Smmu t;
    t.r[0x800 + 4 * 3] = 0x80000040;
    t.r[0x800 + 4 * 4] = 0x80000040;
    CHECK(planSmmuIdentity(0x40, 0x100000, t.reader()).error != nullptr);
  }
  {  // no stage 1, a bad geometry, an unaligned table
    Smmu s;
    s.r[0x20] = 64;
    CHECK(planSmmuIdentity(0x40, 0x100000, s.reader()).error != nullptr);
    Smmu g;
    g.r[0x24] = 0x60000080 | 0x7fu;   // 127 banks + 128 pages: beyond the window
    CHECK(planSmmuIdentity(0x40, 0x100000, g.reader()).error != nullptr);
    Smmu u;
    const IdentityPlan p = planSmmuIdentity(0x40, 0x100000, u.reader());
    CHECK(applySmmuIdentity(p, 0x40, table + 8, u.reader(), u.writer(), never) != nullptr && u.log.empty());
  }
  {  // TLB syncs that never complete
    Smmu s;
    s.r[Smmu::cb(81) + kCbTlbStatus] = 1;
    const IdentityPlan p = planSmmuIdentity(0x40, 0x100000, s.reader());
    unsigned polls = 0;
    CHECK(applySmmuIdentity(p, 0x40, table, s.reader(), s.writer(), [&] { return ++polls > 5; }) != nullptr);
    CHECK(s.r[0xc00] == 0);   // the stream was not switched to the unfinished bank
    Smmu g;
    g.r[0x74] = 1;
    unsigned more = 0;
    CHECK(applySmmuIdentity(planSmmuIdentity(0x40, 0x100000, g.reader()), 0x40, table, g.reader(), g.writer(),
                            [&] { return ++more > 5; }) != nullptr);
  }
  {  // the table: invalid below 2 GiB, 1 GiB identity blocks with the attributes above
    uint64_t t[kIdentityEntries] = {};
    identityTable([&](unsigned i, uint64_t d) { t[i] = d; });
    CHECK(t[0] == 0 && t[1] == 0);
    CHECK(t[2] == (0x80000000ull | 0x741) && t[63] == (63ull << 30 | 0x741));
    bool ok = true;
    for (unsigned i = 2; i < kIdentityEntries; ++i) ok &= (t[i] >> 30) == i && (t[i] & 3) == 1 && ((t[i] >> 2) & 7) == 0;
    CHECK(ok);
  }
  std::printf("smmu_identity_host: %s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
