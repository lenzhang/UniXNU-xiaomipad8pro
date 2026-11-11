// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoGpuCeiling.h (docs/policy.md §5.3): one slot per caller, the lowest non-zero slot wins, clearing one leaves the
// others in force, an unknown client changes nothing, a repeat changes nothing, the holder on a tie is the lowest id.
#include "PianoGpuCeiling.h"
#include <cstdio>
#include <cstdlib>

using namespace pianogpu;

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

int main()
{
  CeilingSlots s;
  bool changed = true;
  CHECK(s.effective == 0 && s.holder() == kCeilingClients);
  // PianoThermal's passive trip (client 0, the old callers' null param2): 160 MHz.
  CHECK(s.set(kCeilingThermal, 160000, &changed) && changed && s.effective == 160000 && s.holder() == kCeilingThermal);
  // Power Saver (client 1) asks 342 MHz: the thermal 160 MHz stays in force.
  CHECK(s.set(kCeilingPolicy, 342000, &changed) && !changed && s.effective == 160000);
  // The trip clears: Power Saver's 342 MHz now.
  CHECK(s.set(kCeilingThermal, 0, &changed) && changed && s.effective == 342000 && s.holder() == kCeilingPolicy);
  // A repeat changes nothing.
  CHECK(s.set(kCeilingPolicy, 342000, &changed) && !changed && s.effective == 342000);
  // A lower thermal ceiling under Power Saver wins; Power Saver ending leaves it.
  CHECK(s.set(kCeilingThermal, 222000, &changed) && changed && s.effective == 222000);
  CHECK(s.set(kCeilingPolicy, 0, &changed) && !changed && s.effective == 222000 && s.holder() == kCeilingThermal);
  // An unknown client: refused, nothing changes.
  CHECK(!s.set(kCeilingClients, 100000, &changed) && !changed && s.effective == 222000);
  CHECK(!s.set(99, 0, nullptr) && s.effective == 222000);
  // A free slot (2) with the same value: the holder is the lowest id.
  CHECK(s.set(2, 222000, &changed) && !changed && s.holder() == kCeilingThermal);
  CHECK(s.set(kCeilingThermal, 0, &changed) && !changed && s.holder() == 2);
  // Everything cleared: no ceiling.
  CHECK(s.set(2, 0, &changed) && changed && s.effective == 0 && s.holder() == kCeilingClients);
  CHECK(kCeilingClients == 4 && ceilingClientName(kCeilingThermal) && ceilingClientName(kCeilingPolicy) && ceilingClientName(3));
  std::printf("PASS: GPU ceiling slots: %u slots, the lowest non-zero wins, clear and repeat, unknown clients refused, the holder "
              "(%u checks)\n", kCeilingClients, checks);
  return 0;
}
