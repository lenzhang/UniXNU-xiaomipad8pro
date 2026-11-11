// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of UsbSleep.h: what a system sleep's off and on do to PianoDwc3's gadget (keep the console for a configured host with
// piano-sleep-usb=1, else a soft disconnect; nothing before the takeover; the interrupt masked), the clock check at the wake (an
// unclocked glue is never touched again), and the QSCRATCH guard (no write while asleep or unclocked: recorded, the last value
// per register, replayed at the wake; K:7dffd78b342d's SoC reset).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. usb_sleep_host.cpp -o /tmp/usb_sleep_host && /tmp/usb_sleep_host
#include "UsbSleep.h"
#include "PianoUsbHw.h"
#include <cstdio>

using namespace pianousb;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void testOffOn()
{
  {   // the default with the Mac configured: kept, polled until the freeze, the interrupt masked; on: touched again, no reconnect
    UsbSleep s;
    const SleepOn first = s.on(true);   // the power tree's first on
    CHECK(!first.touch && s.state() == SleepState::awake && s.wakes() == 0);
    const SleepOff off = s.off(true, true, true);
    CHECK(!off.halt && off.maskIrq && off.keepPolling && s.state() == SleepState::kept && s.coreUsable() && !s.takeoverMayRun());
    const SleepOff again = s.off(true, true, true);
    CHECK(!again.halt && !again.maskIrq && !again.keepPolling && s.sleeps() == 1);
    const SleepOn on = s.on(true);
    CHECK(on.touch && !on.reconnect && !on.replay && s.state() == SleepState::awake && s.wakes() == 1);
  }
  {   // no host configured: a soft disconnect, the thread off the core; on: reconnect
    UsbSleep s;
    const SleepOff off = s.off(true, false, false);
    CHECK(off.halt && !off.maskIrq && !off.keepPolling && s.state() == SleepState::quiesced && !s.coreUsable());
    const SleepOn on = s.on(true);
    CHECK(on.touch && on.reconnect && s.coreUsable());
  }
  {   // piano-sleep-usb=0: a soft disconnect even with the Mac configured
    UsbSleep s(false);
    const SleepOff off = s.off(true, true, true);
    CHECK(off.halt && off.maskIrq && !off.keepPolling && s.state() == SleepState::quiesced);
    CHECK(s.on(true).reconnect);
  }
  {   // asleep before the takeover finished: nothing to do; the takeover waits for the wake, then runs
    UsbSleep s;
    const SleepOff off = s.off(false, false, false);
    CHECK(!off.halt && !off.maskIrq && s.state() == SleepState::early && !s.takeoverMayRun());
    const SleepOn on = s.on(false);   // the takeover checks the clocks itself
    CHECK(on.touch && !on.reconnect && s.takeoverMayRun());
  }
  {   // the glue unclocked at the wake: never touched again, whatever comes later
    UsbSleep s;
    s.off(true, false, false);
    const SleepOn on = s.on(false);
    CHECK(!on.touch && !on.reconnect && s.state() == SleepState::stuck && !s.coreUsable());
    const SleepOff off2 = s.off(true, false, false);
    CHECK(!off2.halt && s.state() == SleepState::stuck);
    CHECK(!s.on(true).touch && s.state() == SleepState::stuck);
    UsbSleep k;
    k.off(true, true, true);   // kept, then unclocked at the wake: stuck as well
    CHECK(!k.on(false).touch && k.state() == SleepState::stuck);
  }
  {   // many cycles alternate cleanly
    UsbSleep s;
    for (unsigned i = 0; i < 10; ++i) {
      const bool configured = i & 1;
      const SleepOff off = s.off(true, configured, true);
      CHECK(off.halt == !configured && off.keepPolling == configured);
      const SleepOn on = s.on(true);
      CHECK(on.touch && on.reconnect == !configured);
    }
    CHECK(s.sleeps() == 10 && s.wakes() == 10);
  }
}

static void testQscratch()
{
  using namespace pianohw;
  UsbSleep s;
  CHECK(s.qscratchWrite(kQsHsPhyCtrl, 0x1, true));            // awake and clocked: now
  CHECK(!s.qscratchWrite(kQsHsPhyCtrl, 0x2, false));          // awake, unclocked: recorded
  CHECK(s.recorded() == 1 && s.recordedValue(0) == 0x2);
  s.replayed();
  s.off(true, true, false);
  CHECK(!s.qscratchWrite(kQsSsPhyCtrl, 0x10, true));          // asleep (kept): recorded even though clocked
  CHECK(!s.qscratchWrite(kQsHsPhyCtrl, 0x20, true));
  CHECK(!s.qscratchWrite(kQsSsPhyCtrl, 0x11, true));          // the last value per register, in the order first asked
  CHECK(s.recorded() == 2 && s.recordedOffset(0) == kQsSsPhyCtrl && s.recordedValue(0) == 0x11 && s.recordedOffset(1) == kQsHsPhyCtrl &&
        s.recordedValue(1) == 0x20);
  const SleepOn on = s.on(true);
  CHECK(on.touch && on.replay);
  s.replayed();
  CHECK(s.recorded() == 0 && s.qscratchWrite(kQsGeneralCfg, 0x3, true));
  // quiesced, then unclocked at the wake: recorded, never replayed
  s.off(true, false, false);
  CHECK(!s.qscratchWrite(kQsHsPhyCtrl, 0x5, true));
  const SleepOn stuck = s.on(false);
  CHECK(!stuck.replay && !stuck.touch && !s.qscratchWrite(kQsHsPhyCtrl, 0x6, true) && s.recorded() == 1);
  // more registers than kept: counted as lost
  UsbSleep m;
  m.off(true, false, false);
  for (uint32_t r = 0; r < UsbSleep::kRecorded + 2; ++r) m.qscratchWrite(0x100 + 4 * r, r, true);
  CHECK(m.recorded() == UsbSleep::kRecorded && m.lost() == 2 && m.deferred() == UsbSleep::kRecorded + 2);
}

int main()
{
  testOffOn();
  testQscratch();
  std::printf("usb_sleep_host: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
  return failures ? 1 : 0;
}
