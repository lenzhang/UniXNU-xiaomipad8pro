// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of Reg64.h (the only behaviour PianoXhciHost adds to Apple's generic xHCI driver, docs/drivers/usb-host.md §5.4)
// against a model register file: a 64-bit write is two 32-bit stores with the **high half first**, each followed by a barrier; a
// 64-bit read is two 32-bit loads, low half first; the halves are not swapped, not truncated and not widened; a write of an
// address above 4 GiB (where the order decides whether the controller latches the right ring address) still stores the high half
// first; the register file is touched only at the two words of the register.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. reg64_host.cpp -o /tmp/reg64_host && /tmp/reg64_host
#include "Reg64.h"
#include <cstdio>
#include <map>
#include <vector>

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// A 32-bit register file that records every access in order: ('r'|'w'|'b', offset, value).
struct Access {
  char kind;
  uint64_t offset;
  uint32_t value;
  bool operator==(const Access &o) const { return kind == o.kind && offset == o.offset && value == o.value; }
};
struct Regs {
  std::map<uint64_t, uint32_t> words;
  std::vector<Access> trace;
  uint32_t load(uint64_t offset)
  {
    const uint32_t v = words[offset];
    trace.push_back({'r', offset, v});
    return v;
  }
  void store(uint64_t offset, uint32_t value)
  {
    words[offset] = value;
    trace.push_back({'w', offset, value});
  }
  void barrier() { trace.push_back({'b', 0, 0}); }
};

int main()
{
  {   // the write order: high half, barrier, low half, barrier - and nothing else
    Regs r;
    pianoxhci::write64(r, 0x50, 0x0000000888776655ull);
    const std::vector<Access> want = {{'w', 0x54, 0x00000008u}, {'b', 0, 0}, {'w', 0x50, 0x88776655u}, {'b', 0, 0}};
    CHECK(r.trace == want);
    CHECK(r.words[0x54] == 0x00000008u && r.words[0x50] == 0x88776655u);
    CHECK(r.words.size() == 2);
  }
  {   // above 4 GiB: the high half is the one that must land first (a ring address the controller would otherwise latch half)
    Regs r;
    pianoxhci::write64(r, 0x18, 0x00000008c0001000ull);
    CHECK(r.trace.size() == 4);
    CHECK((r.trace[0] == Access{'w', 0x1c, 0x00000008u} && r.trace[1].kind == 'b'));
    CHECK((r.trace[2] == Access{'w', 0x18, 0xc0001000u} && r.trace[3].kind == 'b'));
    CHECK(pianoxhci::read64(r, 0x18) == 0x00000008c0001000ull);
  }
  {   // zero, all ones, and only-high-half values survive the round trip
    for (uint64_t v : {uint64_t(0), ~uint64_t(0), uint64_t(0xffffffff00000000ull), uint64_t(0x00000000ffffffffull),
                       uint64_t(0x0000000100000000ull), uint64_t(0xdeadbeefcafe0000ull)}) {
      Regs r;
      pianoxhci::write64(r, 0x30, v);
      CHECK(pianoxhci::read64(r, 0x30) == v);
      CHECK(r.words[0x34] == uint32_t(v >> 32) && r.words[0x30] == uint32_t(v));
    }
  }
  {   // the read order: the low half first, then the high half; the halves are not swapped
    Regs r;
    r.words[0x40] = 0x11112222u;
    r.words[0x44] = 0x33334444u;
    CHECK(pianoxhci::read64(r, 0x40) == 0x3333444411112222ull);
    CHECK(r.trace.size() == 2);
    CHECK((r.trace[0] == Access{'r', 0x40, 0x11112222u} && r.trace[1] == Access{'r', 0x44, 0x33334444u}));
  }
  {   // two registers side by side: a write to one does not touch the other's halves
    Regs r;
    pianoxhci::write64(r, 0x00, 0x0123456789abcdefull);
    pianoxhci::write64(r, 0x08, 0xfedcba9876543210ull);
    CHECK(pianoxhci::read64(r, 0x00) == 0x0123456789abcdefull);
    CHECK(pianoxhci::read64(r, 0x08) == 0xfedcba9876543210ull);
    CHECK(r.words.size() == 4);
  }
  std::printf("%s: reg64_host\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
