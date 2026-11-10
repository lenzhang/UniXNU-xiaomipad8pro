// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of SmmuStream.h on an SMMU register model shaped like piano's apps SMMU after NativeUfs (IDR1 0x60000053:
// 83 banks, 128 pages; SID 0x60 on bank 82, as logs/native-runs/20261111-004859-loop recorded).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. smmu_stream_host.cpp -o /tmp/smmu_stream_host && /tmp/smmu_stream_host
#include "SmmuStream.h"
#include <cstdio>
#include <map>

using namespace pianousb;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Smmu {
  std::map<uint32_t, uint32_t> r;
  unsigned writes = 0;
  Smmu()
  {
    r[0x20] = 1u << 30 | 64;   // S1TS, 64 stream-match groups
    r[0x24] = 0x60000053;
    r[0x28] = 1u << 12;
    r[0x800 + 4 * 1] = 0x80000060;
    r[0xc00 + 4 * 1] = 82;
    r[0x1000 + 4 * 82] = 0x10000;
    r[0x1800 + 4 * 82] = 1;
  }
  uint32_t cb(unsigned bank) const { return (128 + bank) * 4096; }
  auto reader() { return [this](uint32_t o) { return r[o]; }; }
  auto writer() { return [this](uint32_t o, uint32_t v) { ++writes; r[o] = v; }; }
};

int main()
{
  const auto never = [] { return false; };
  {  // nothing for SID 0x40 yet: a free group, the bank below NativeUfs's
    Smmu s;
    SmmuPlan p = planSmmuStream(0x40, 0x100000, s.reader());
    CHECK(!p.error && !p.reuse && !p.keep && p.slot == 0 && p.bank == 81 && p.pages == 128 && p.banks == 83);
    CHECK(applySmmuStream(p, 0x40, s.reader(), s.writer(), never) == nullptr);
    CHECK(s.r[0x800] == 0x80000040 && s.r[0xc00] == 81 && s.r[0x1000 + 4 * 81] == 0x10000 && s.r[0x1800 + 4 * 81] == 1);
    CHECK(s.r[s.cb(81)] == 0 && s.r[0x68] == 0xffffffffu && s.r[0x70] == 0xffffffffu);
    CHECK(s.r[0x804] == 0x80000060 && s.r[0xc04] == 82);   // UFS untouched
  }
  {  // the firmware's exact entry on a bank with translation off and VA64: kept, nothing written
    Smmu s;
    s.r[0x800 + 4 * 5] = 0x80000040;
    s.r[0xc00 + 4 * 5] = 1;
    s.r[s.cb(1)] = 0x0a9f00e0;
    s.r[0x1800 + 4 * 1] = 1;
    SmmuPlan p = planSmmuStream(0x40, 0x100000, s.reader());
    CHECK(!p.error && p.reuse && p.keep && p.slot == 5 && p.bank == 1);
    CHECK(applySmmuStream(p, 0x40, s.reader(), s.writer(), never) == nullptr && s.writes == 0);
  }
  {  // the same without VA64: redirected in place to a bypass bank (its old bank is in use: skipped)
    Smmu s;
    s.r[0x800 + 4 * 5] = 0x80000040;
    s.r[0xc00 + 4 * 5] = 81;
    SmmuPlan p = planSmmuStream(0x40, 0x100000, s.reader());
    CHECK(!p.error && p.reuse && !p.keep && p.slot == 5 && p.bank == 80);
    CHECK(applySmmuStream(p, 0x40, s.reader(), s.writer(), never) == nullptr);
    CHECK(s.r[0x800 + 4 * 5] == 0x80000040 && s.r[0xc00 + 4 * 5] == 80 && s.r[0x1800 + 4 * 80] == 1);
  }
  {  // S2CR bypass type: kept
    Smmu s;
    s.r[0x800 + 4 * 2] = 0x80000040;
    s.r[0xc00 + 4 * 2] = 1u << 16;
    SmmuPlan p = planSmmuStream(0x40, 0x100000, s.reader());
    CHECK(!p.error && p.keep);
  }
  {  // a masked group covering the stream: refused
    Smmu s;
    s.r[0x800 + 4 * 3] = 0x80000000u | 0xfu << 16 | 0x40;
    s.r[0xc00 + 4 * 3] = 3;
    SmmuPlan p = planSmmuStream(0x40, 0x100000, s.reader());
    CHECK(p.error != nullptr);
    CHECK(applySmmuStream(p, 0x40, s.reader(), s.writer(), never) == p.error && s.writes == 0);
  }
  {  // banks with translation on are skipped; none left: refused
    Smmu s;
    for (unsigned b = 0; b < 82; ++b) s.r[s.cb(b)] = 1;
    SmmuPlan p = planSmmuStream(0x40, 0x100000, s.reader());
    CHECK(p.error != nullptr);
  }
  {  // TLB sync never completes
    Smmu s;
    s.r[0x74] = 1;
    SmmuPlan p = planSmmuStream(0x40, 0x100000, s.reader());
    unsigned polls = 0;
    CHECK(applySmmuStream(p, 0x40, s.reader(), s.writer(), [&] { return ++polls > 5; }) != nullptr);
  }
  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
  return failures ? 1 : 0;
}
