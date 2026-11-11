// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoDpu B1e (DpuPanel.h) on the timed register model (dpu_model.h):
//   - the DCS short packet and its FIFO words; one command on both links: the stores in order, low power, sent in the vertical
//     blanking (by the hardware's block or, without it, triggered in the front porch), every word restored as found; the
//     refusals (not video mode, the FIFO in use, no software trigger, a command that is not a sleep command, a busy DMA);
//   - display sleep: 0x28, >= 20 ms, 0x10, >= 100 ms, INTF2 stopped before INTF1, both stopped, the panel halves asleep; wake:
//     the INTF flush, INTF1 and INTF2 started together and in phase, counting in lockstep, 0x11, >= 120 ms, 0x29; 50 cycles;
//   - the failures: a DMA that never finishes (the link left alone, nothing after it), an engine that does not stop (the panel
//     brought back), an engine that does not start; DCS only; high speed;
//   - nothing written outside the guard's registers: never TLMM (the panel's reset and supply pins).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_panel_host.cpp -o /tmp/dpu_panel_host && /tmp/dpu_panel_host
#include "DpuPanel.h"
#include "dpu_model.h"
#include <cstdio>
#include <cstring>

using namespace pianodpu;
using namespace pianodpu::panel;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool clean(const model::Dpu &m)
{
  return m.refused.empty() && !m.anyStoreIn(kTlmm, 0x200000) && !m.anyStoreIn(kDsiPhy[0], 0x1000) && !m.anyStoreIn(kDsiPhy[1], 0x1000) &&
         !m.anyStoreIn(kMdp + 0x4000, 0x8000) && !m.anyStoreIn(kMdp + 0x24000, 0xc000) && !m.anyStoreIn(kMdp + kLmBase, 0x8000);
}

static void testPacket()
{
  const Packet p = dcsShort(kDcsDisplayOff);
  CHECK(p.words[0] == 0x80050028u && p.words[1] == 0 && p.count == 2 && p.bytes == 4);
  CHECK(dcsShort(kDcsSleepOut).words[0] == 0x80050011u);
  CHECK(dcsAllowed(0x28) && dcsAllowed(0x10) && dcsAllowed(0x11) && dcsAllowed(0x29) && !dcsAllowed(0x51) && !dcsAllowed(0x01));
}

static void testSend()
{
  model::Dpu m;
  m.sleepMs(3);
  uint32_t found[2][4];
  for (unsigned l = 0; l < 2; ++l) {
    found[l][0] = m.rd(dsiReg(l, kDsiCtrl));
    found[l][1] = m.rd(dsiReg(l, kDsiCmdDmaCtrl));
    found[l][2] = m.rd(dsiReg(l, kDsiDmaCtrl1));
    found[l][3] = m.rd(dsiReg(l, kDsiDmaLength));
  }
  const size_t before = m.stores.size();
  Sent s = sendDcs(m, kDcsDisplayOff, Options());
  CHECK(s.ok && s.done[0] && s.done[1] && !s.waitedBlanking && s.refused == 0);
  CHECK(m.dcs.size() == 2 && m.dcs[0].command == 0x28 && m.dcs[1].command == 0x28 && m.dcs[0].lowPower && m.dcs[1].lowPower);
  CHECK(m.commandsInFrame == 0 && !m.half[0].displayOn && !m.half[1].displayOn);
  for (unsigned l = 0; l < 2; ++l) {   // every word as found, the FIFO free, the done bit clear
    CHECK(m.rd(dsiReg(l, kDsiCtrl)) == found[l][0] && m.rd(dsiReg(l, kDsiCmdDmaCtrl)) == found[l][1]);
    CHECK(m.rd(dsiReg(l, kDsiDmaCtrl1)) == found[l][2] && m.rd(dsiReg(l, kDsiDmaLength)) == found[l][3]);
    CHECK(m.rd(dsiReg(l, kDsiTpgCtrl)) == 0 && (m.rd(dsiReg(l, kDsiIntrCtrl)) & 1u) == 0 && (m.rd(dsiReg(l, kDsiIntrCtrl)) & 2u));
  }
  // DSI0's preparation in order, DSI1's, then the two triggers back to back
  const uint64_t prep[] = {kDsiIntrCtrl, kDsiCtrl, kDsiTpgCtrl, kDsiTpgFifo, kDsiTpgFifo, kDsiCmdDmaCtrl, kDsiDmaCtrl1, kDsiDmaLength};
  for (unsigned l = 0; l < 2; ++l)
    for (unsigned k = 0; k < 8; ++k) CHECK(m.stores[before + 8 * l + k].pa == dsiReg(l, uint32_t(prep[k])));
  CHECK(m.stores[before + 1].v == (found[0][0] | 4u) && m.stores[before + 2].v == 0x30006u && m.stores[before + 3].v == 0x80050028u &&
        m.stores[before + 4].v == 0 && m.stores[before + 5].v == ((found[0][1] & ~0xc4000000u) | 0x14000000u) &&
        m.stores[before + 6].v == (found[0][2] & ~2u) && m.stores[before + 7].v == 4);
  CHECK(m.stores[before + 16].pa == dsiReg(0, kDsiTrigDma) && m.stores[before + 17].pa == dsiReg(1, kDsiTrigDma));
  CHECK(m.stores[before + 17].ns - m.stores[before + 16].ns < 1000);
  CHECK(m.storesTo(dsiReg(0, 0x1ec)) == 2 && m.firstStore(dsiReg(0, 0x1ec), 1) >= 0);   // PeripheralIO's FIFO reset pulse
  CHECK(clean(m));
  // high speed: the low-power bit clear
  Options hs;
  hs.lowPower = false;
  CHECK(sendDcs(m, kDcsDisplayOn, hs).ok && !m.dcs.back().lowPower && m.half[0].displayOn);
  // no hardware block within the frame: triggered in the front porch
  model::Dpu n;
  n.blockDma = false;
  n.sleepMs(3);
  s = sendDcs(n, kDcsDisplayOff, Options());
  CHECK(s.ok && s.waitedBlanking && n.commandsInFrame == 0 && n.dcs.size() == 2);
  CHECK(n.stores[n.firstStore(dsiReg(0, kDsiTrigDma), 1)].line >= kVActiveEnd);
}

static void testRefusals()
{
  {
    model::Dpu m;
    m.regs[dsiReg(1, kDsiCtrl)] = 0x1f5;   // DSI1 in command mode
    m.sleepMs(2);
    const Sent s = sendDcs(m, kDcsDisplayOff, Options());
    CHECK(!s.ok && std::strstr(s.why, "video mode") && m.stores.empty());
  }
  {
    model::Dpu m;
    m.regs[dsiReg(0, kDsiTpgCtrl)] = 0x30006;   // the FIFO in use
    CHECK(!sendDcs(m, kDcsDisplayOff, Options()).ok && m.stores.empty());
  }
  {
    model::Dpu m;
    CHECK(!sendDcs(m, 0x51, Options()).ok && m.stores.empty());   // brightness: not this slice's
  }
  {
    model::Dpu m;
    m.dsi[0].dmaPending = true;   // a DMA of someone else's that never ends
    m.dsi[0].dmaAt = UINT64_MAX;
    const Sent s = sendDcs(m, kDcsDisplayOff, Options());
    CHECK(!s.ok && std::strstr(s.why, "busy") && m.stores.empty());
  }
}

static void testSleepWake()
{
  model::Dpu m;
  m.sleepMs(3);
  const LivePath p = readPath(m);
  CHECK(p.ok);
  Result off = panelOff(m, p, Options());
  CHECK(off.ok && off.enginesStopped && off.step == 4);
  const model::Dcs *d28 = m.firstDcs(0x28, 0), *d10 = m.firstDcs(0x10, 0), *e28 = m.firstDcs(0x28, 1), *e10 = m.firstDcs(0x10, 1);
  CHECK(d28 && d10 && e28 && e10);
  if (d28 && d10) CHECK(d10->ns - d28->ns >= 20000000ull);
  const int stop2 = m.firstStore(intfReg(2, kIntfEn), 0), stop1 = m.firstStore(intfReg(1, kIntfEn), 0);
  CHECK(stop2 >= 0 && stop1 > stop2);                                  // the slave first
  if (d10 && stop2 >= 0) CHECK(m.stores[size_t(stop2)].ns - d10->ns >= 100000000ull);   // 100 ms after sleep-in
  CHECK(!m.intf[1].on && !m.intf[2].on && m.half[0].sleeping && m.half[1].sleeping && !m.half[0].displayOn);
  const uint32_t f1 = m.intf[1].frames, f2 = m.intf[2].frames;
  m.sleepMs(1000);   // asleep: no frames, no fetch
  CHECK(m.intf[1].frames == f1 && m.intf[2].frames == f2 && m.rd(intfReg(1, kIntfStatus)) == 0);
  const size_t before = m.stores.size();
  Result on = panelOn(m, p, Options(), true);
  CHECK(on.ok && on.engines.ok && on.engines.rate.lockstep && on.engines.latchUs >= 0);
  CHECK(m.stores[before].pa == ctlReg(0, kCtlIntfFlush) && m.stores[before + 1].pa == ctlReg(0, kCtlFlush));
  CHECK(m.stores[before + 2].pa == intfReg(1, kIntfEn) && m.stores[before + 3].pa == intfReg(2, kIntfEn));
  CHECK(m.stores[before + 3].ns - m.stores[before + 2].ns < 1000 && m.desync == 0);
  CHECK(m.intf[1].on && m.intf[2].on && m.intf[1].frames > f1 && m.intf[1].frames - f1 == m.intf[2].frames - f2);
  const model::Dcs *d11 = m.firstDcs(0x11, 0), *d29 = nullptr;
  for (const model::Dcs &x : m.dcs)
    if (x.command == 0x29 && x.link == 1 && d11 && x.ns > d11->ns) d29 = &x;
  CHECK(d11 && d29);
  if (d11 && d29) CHECK(d29->ns - d11->ns >= 120000000ull);
  CHECK(!m.half[0].sleeping && !m.half[1].sleeping && m.half[0].displayOn && m.half[1].displayOn);
  CHECK(m.glitches == 0 && clean(m));
  // 50 cycles
  unsigned ok = 0;
  for (unsigned k = 0; k < 50; ++k) {
    m.sleepMs(3 * k % 17);
    ok += panelOff(m, p, Options()).ok;
    m.sleepMs(500);
    ok += panelOn(m, p, Options(), true).ok;
  }
  CHECK(ok == 100 && m.desync == 0 && m.glitches == 0 && m.half[0].displayOn && clean(m));
}

static void testFailures()
{
  {   // display-off never finishes on DSI1: stop there, DSI1's words left to its engine, nothing more
    model::Dpu m;
    m.dmaHang[1] = true;
    m.sleepMs(3);
    const LivePath p = readPath(m);
    const Result r = panelOff(m, p, Options());
    CHECK(!r.ok && r.step == 1 && std::strstr(r.why, "DSI1"));
    CHECK(m.rd(dsiReg(1, kDsiTpgCtrl)) == 0x30006u && m.rd(dsiReg(0, kDsiTpgCtrl)) == 0);
    CHECK(m.storesTo(intfReg(1, kIntfEn)) == 0 && m.storesTo(intfReg(2, kIntfEn)) == 0 && m.intf[1].on);
  }
  {   // sleep-in never finishes on DSI0: no engine store, no undo on a link the engine still owns
    model::Dpu m;
    m.dmaBudget[0] = 1;
    m.sleepMs(3);
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.step == 2 && !r.restored && m.storesTo(intfReg(2, kIntfEn)) == 0);
  }
  {   // INTF2 does not stop: brought back (EN = 1 again), sleep-out, display-on
    model::Dpu m;
    m.engineStuck[1] = true;
    m.sleepMs(3);
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.step == 3 && r.restored && std::strstr(r.why, "INTF2"));
    CHECK(m.half[0].displayOn && !m.half[0].sleeping && m.rd(intfReg(2, kIntfEn)) == 1 && m.intf[1].on && m.intf[2].on);
    CHECK(m.firstStore(intfReg(1, kIntfEn), 0) < 0);   // the master was never stopped
  }
  {   // INTF1 does not stop: INTF2 (stopped) started again at INTF1's frame start, in phase; the panel back
    model::Dpu m;
    m.engineStuck[0] = true;
    m.sleepMs(3);
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.step == 4 && r.restored && m.desync == 0 && m.intf[1].on && m.intf[2].on && m.half[1].displayOn);
  }
  {   // the engines do not start at the wake: reported, no DCS sent into a stopped link
    model::Dpu m;
    m.sleepMs(3);
    const LivePath p = readPath(m);
    CHECK(panelOff(m, p, Options()).ok);
    m.engineDead[0] = m.engineDead[1] = true;
    const size_t dcs = m.dcs.size();
    const Result r = panelOn(m, p, Options(), true);
    CHECK(!r.ok && r.step == 1 && std::strstr(r.why, "running") && m.dcs.size() == dcs);
  }
  {   // DCS only: the engines keep running
    model::Dpu m;
    m.sleepMs(3);
    const LivePath p = readPath(m);
    Options o;
    o.engines = false;
    const Result off = panelOff(m, p, o);
    CHECK(off.ok && !off.enginesStopped && m.intf[1].on && m.half[0].sleeping && m.storesTo(intfReg(1, kIntfEn)) == 0);
    const Result on = panelOn(m, p, o, false);
    CHECK(on.ok && m.half[0].displayOn && !m.half[1].sleeping && m.storesTo(intfReg(2, kIntfEn)) == 0);
  }
}

int main()
{
  testPacket();
  testSend();
  testRefusals();
  testSleepWake();
  testFailures();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("PianoDpu panel host tests: all passed\n");
  return 0;
}
