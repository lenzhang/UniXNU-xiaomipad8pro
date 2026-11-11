// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoDpu B1c (DpuRefresh.h) on the timed register model (dpu_model.h):
//   - the path read and the refusals (not the 120 Hz family, DSI/INTF disagreeing, programmable fetch on, a gate closed);
//   - 120 -> 60 -> 120 Hz: the exact stores in their order, all inside one frame and the line window, the latch, no glitch, the
//     measured refresh, the halves in lockstep; 100 round trips clean; the counters turned on when the boot left them off;
//   - the anomalies and their revert: the DSI shadow at the other place (the switch glitches, the way back cannot latch), a CTL that
//     never latches, a direct TOTAL write, a store the guard refuses;
//   - the policy: idle -> 60, bursts -> 120, a blinking cursor stays at 60, the dwell, disabled after a failure; and the policy
//     driving the model through a session of swaps.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_refresh_host.cpp -o /tmp/dpu_refresh_host && /tmp/dpu_refresh_host
#include "DpuRefresh.h"
#include "dpu_model.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace pianodpu;
using namespace pianodpu::refresh;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static LivePath path(model::Dpu &m)
{
  LivePath p = readPath(m);
  return p;
}

static void testPath()
{
  model::Dpu m;
  m.sleepMs(3);
  LivePath p = path(m);
  CHECK(p.ok && p.ctl == 0 && p.lms == 0x3 && p.dspps == 0x3);
  CHECK(p.link[0].htotal == 576 && p.link[0].vtotal == 2310 && p.link[1].vtotal == 2310 && p.link[0].dsiVtotal() == 2310);
  CHECK(refuse(p) == nullptr);
  CHECK(m.stores.empty());   // reading the path stores nothing
  {
    model::Dpu g;
    g.regs[kDispCc + kCcAhb] = 0x80000000u;   // MDSS AHB off: nothing of MDSS read
    const LivePath q = path(g);
    CHECK(!q.ok && std::strstr(q.why, "AHB"));
  }
  {
    model::Dpu g;
    g.regs[kDispCc + kCcByte1] = 0x80000000u;   // DSI1's byte clock off
    CHECK(!path(g).ok);
  }
  LivePath q = p;
  q.link[1].config |= kIntfConfigProgFetch;
  CHECK(refuse(q) && std::strstr(refuse(q), "programmable fetch"));
  q = p;
  q.link[0].vtotal = q.link[1].vtotal = 2400;
  CHECK(refuse(q) != nullptr);
  q = p;
  q.link[1].dsiTotal = 4619u << 16 | 575;
  CHECK(refuse(q) && std::strstr(refuse(q), "DSI TOTAL"));
  q = p;
  q.link[1].dsiTotal = 2309u << 16 | 574;
  CHECK(refuse(q) != nullptr);
  CHECK(expectedMilliHz(kVtotal60) == 60000 && expectedMilliHz(kVtotal120) == 120000);
  CHECK(rateOk(59980, 4620, 20) && rateOk(119980, 2310, 20) && !rateOk(119980, 4620, 20) && !rateOk(57000, 4620, 20));
}

static void testSwitch()
{
  model::Dpu m;
  m.sleepMs(5);
  LivePath p = path(m);
  const size_t before = m.stores.size();
  Switch s = switchTo(m, p, kVtotal60, Options());
  CHECK(s.ok && !s.reverted && s.refused == 0 && m.refused.empty());
  CHECK(s.latchUs >= 0 && s.latchUs < 9000);           // within the frame the stores were in
  CHECK(s.rate.milliHz > 59000 && s.rate.milliHz < 61000 && s.rate.lockstep);
  CHECK(s.period[0] == 4620 * 576 && s.period[1] == 4620 * 576);
  CHECK(m.intf[1].period == 4620 * 576 && m.intf[2].period == 4620 * 576);
  CHECK((m.dsi[0].total >> 16) == 4619 && (m.dsi[1].total >> 16) == 4619 && (m.dsi[0].total & 0xffff) == 575);
  CHECK(!m.dsi[0].db && !m.dsi[1].db);                 // DB mode off again after the latch
  CHECK(m.glitches == 0 && m.midFrameTotal == 0 && m.topStatus == 0);
  // The stores, in order: DSI0 DB/TOTAL/flush, DSI1 DB/TOTAL/flush, INTF1, INTF2 periods, CTL_INTF_FLUSH, CTL_FLUSH; then DB off x2.
  const uint64_t expect[] = {dsiReg(0, 0x1ec), dsiReg(0, kDsiTotal), dsiReg(0, 0x1e8), dsiReg(1, 0x1ec), dsiReg(1, kDsiTotal),
                             dsiReg(1, 0x1e8), intfReg(1, kIntfVsyncPeriod), intfReg(2, kIntfVsyncPeriod), ctlReg(0, kCtlIntfFlush),
                             ctlReg(0, kCtlFlush), dsiReg(0, 0x1ec), dsiReg(1, 0x1ec)};
  CHECK(m.stores.size() - before == sizeof expect / sizeof expect[0]);
  for (size_t k = 0; k < sizeof expect / sizeof expect[0] && before + k < m.stores.size(); ++k) CHECK(m.stores[before + k].pa == expect[k]);
  CHECK(m.stores[before + 1].v == (4619u << 16 | 575) && m.stores[before + 6].v == 4620 * 576 && m.stores[before + 8].v == 0x6 &&
        m.stores[before + 9].v == 0x80000000u && m.stores[before + 10].v == 0);
  for (size_t k = 0; k < 10; ++k) {   // the staging stores: one frame, inside the window
    CHECK(m.stores[before + k].frame == m.stores[before].frame);
    CHECK(m.stores[before + k].line >= kWindowLo && m.stores[before + k].line <= kWindowHi);
  }
  CHECK(m.stores[before + 10].frame > m.stores[before].frame);   // DB mode dropped after the frame start that latched
  CHECK(s.stores == 12);
  // and back
  p = path(m);
  CHECK(p.link[0].vtotal == 4620 && refuse(p) == nullptr);
  s = switchTo(m, p, kVtotal120, Options());
  CHECK(s.ok && s.rate.milliHz > 119000 && s.rate.milliHz < 121000 && m.glitches == 0);
  CHECK(m.intf[1].period == 2310 * 576 && (m.dsi[1].total >> 16) == 2309);
  // 100 round trips
  unsigned ok = 0;
  for (unsigned k = 0; k < 100; ++k) {
    m.sleepMs(7 * (k % 5));   // anywhere in the frame
    const LivePath a = path(m);
    ok += switchTo(m, a, kVtotal60, Options()).ok;
    const LivePath b = path(m);
    ok += switchTo(m, b, kVtotal120, Options()).ok;
  }
  CHECK(ok == 200 && m.glitches == 0 && m.midFrameTotal == 0 && m.refused.empty() && m.desync == 0);
  // nothing outside the guard's registers, never TLMM, never a pipe or an LM
  CHECK(!m.anyStoreIn(kTlmm, 0x200000) && !m.anyStoreIn(kMdp + 0x4000, 0x8000) && !m.anyStoreIn(kMdp + 0x24000, 0xc000) &&
        !m.anyStoreIn(kMdp + kLmBase, 0x8000));
  // the downstream layout on hardware that has it
  model::Dpu d;
  d.dbHw = model::DbHw::downstream;
  d.sleepMs(2);
  Options od;
  od.db = Db::downstream;
  CHECK(switchTo(d, path(d), kVtotal60, od).ok && switchTo(d, path(d), kVtotal120, od).ok && d.glitches == 0);
  CHECK(d.storesTo(dsiReg(0, 0x1e4)) == 2 && d.storesTo(dsiReg(0, 0x1ec)) == 0);
}

static void testCounters()
{
  model::Dpu m;
  for (unsigned i = 1; i <= 2; ++i) {
    m.regs[intfReg(i, kIntfCountEn)] = 0;
    m.intf[i].counting = false;
  }
  m.sleepMs(3);
  LivePath p = path(m);
  CHECK(p.ok && (p.link[0].countEn & 3u) == 0);
  CHECK(countersOn(m, p) && (p.link[0].countEn & 3u) == 3 && (p.link[1].countEn & 3u) == 3);
  CHECK(m.storesTo(intfReg(1, kIntfCountEn)) == 1 && m.storesTo(intfReg(2, kIntfCountEn)) == 1);
  m.sleepMs(10);
  CHECK(switchTo(m, path(m), kVtotal60, Options()).ok);
  CHECK(countersOn(m, p) && m.storesTo(intfReg(1, kIntfCountEn)) == 1);   // already on: nothing more
}

static void testAnomalies()
{
  {   // SunUEFI's words on hardware with the downstream layout: the TOTAL lands mid-frame (an underrun), and the way back
      // stays in a shadow nothing flushes: reverted, and the revert reports its own failure.
    model::Dpu m;
    m.dbHw = model::DbHw::downstream;
    m.sleepMs(4);
    const Switch s = switchTo(m, path(m), kVtotal60, Options());
    CHECK(!s.ok && s.reverted && std::strstr(s.why, "underran"));
    CHECK(!s.revertOk);
    CHECK(m.intf[1].period == 2310 * 576);   // the INTFs went back
  }
  {   // a CTL that never latches: no switch, the way back cannot latch either
    model::Dpu m;
    m.flushStuck = true;
    m.sleepMs(4);
    const Switch s = switchTo(m, path(m), kVtotal60, Options());
    CHECK(!s.ok && s.reverted && std::strstr(s.why, "CTL_FLUSH bit 31") && !s.revertOk);
    CHECK(!m.dsi[0].db && !m.dsi[1].db);     // DB mode dropped anyway
    CHECK(m.intf[1].period == 2310 * 576);   // nothing latched
  }
  {   // TOTAL written directly (piano-dpu-refresh-db=0): the controller's frame changes inside a frame
    model::Dpu m;
    m.sleepMs(4);
    Options o;
    o.db = Db::direct;
    const Switch s = switchTo(m, path(m), kVtotal60, o);
    CHECK(!s.ok && s.reverted && m.midFrameTotal > 0);
  }
  {   // refusals: no stores at all
    model::Dpu m;
    m.regs[intfReg(2, kIntfConfig)] = kIntfConfigProgFetch;
    m.sleepMs(2);
    const Switch s = switchTo(m, path(m), kVtotal60, Options());
    CHECK(!s.ok && !s.reverted && s.stores == 0 && m.stores.empty());
    model::Dpu n;
    n.sleepMs(2);
    CHECK(!switchTo(n, path(n), 3000, Options()).ok && n.stores.empty());
  }
  {   // an underrun left from before is cleared first and does not count
    model::Dpu m;
    m.topStatus = kUnderrunBits;
    m.sleepMs(4);
    const Switch s = switchTo(m, path(m), kVtotal60, Options());
    CHECK(s.ok && m.storesTo(kMdp + kTopIntrClear) == 1 && m.topStatus == 0);
  }
  CHECK(!storeAllowed(kMdp + kTopIntrClear, 1u << 27) && storeAllowed(kMdp + kTopIntrClear, kUnderrunBits));
  CHECK(!storeAllowed(intfReg(0, kIntfEn), 0) && !storeAllowed(intfReg(3, kIntfVsyncPeriod), 1));   // the DP INTFs
  CHECK(!storeAllowed(kTlmm + 98 * 0x1000 + 4, 0) && !storeAllowed(kTlmm + 117 * 0x1000 + 4, 0) && !storeAllowed(kTlmm + 118 * 0x1000, 0));
  CHECK(!storeAllowed(kDsiPhy[0], 0) && !storeAllowed(kVbif, 0) && !storeAllowed(kRegDma, 0) && !storeAllowed(kSmmu, 0));
  CHECK(!storeAllowed(kMdp + kPipes[0].base + kSrc0Addr, 0) && !storeAllowed(pccReg(0, 0x4), 0));   // a pipe; the PCC's constants
  CHECK(!storeAllowed(intfReg(1, kIntfEn), 2) && !storeAllowed(intfReg(1, kIntfHsyncCtl), 0) && !storeAllowed(dsiReg(0, kDsiActiveV), 0));
}

static void testPolicy()
{
  PolicyConfig c;
  {
    Policy p(c);
    p.applied(120, 0);
    CHECK(p.decide(500).hz == 120 && p.decide(500).nextMs == 1000);
    CHECK(p.decide(1000).hz == 60);
    p.applied(60, 1000);
    CHECK(p.decide(5000).hz == 60 && p.decide(5000).nextMs == 0);
    CHECK(!p.swap(5000));                    // one swap: no burst
    CHECK(p.swap(5100));                     // the second within 250 ms: a burst at 60 Hz -> wake
    CHECK(p.decide(5100).hz == 120);
    p.applied(120, 5110);
    CHECK(p.decide(5900).hz == 120 && p.decide(6100).hz == 60);   // 1 s after the burst
    CHECK(p.switches() == 2);
  }
  {   // a blinking cursor: a swap every 500 ms is never a burst: 60 Hz after the first second, and it stays there
    Policy p(c);
    p.applied(120, 0);
    unsigned wakes = 0, hz = 120;
    for (uint64_t t = 0; t < 20000; t += 500) {
      wakes += p.swap(t);
      const Policy::Decision d = p.decide(t);
      if (d.hz != hz) {
        hz = d.hz;
        p.applied(hz, t);
      }
    }
    CHECK(hz == 60 && wakes == 0 && p.switches() == 1);
  }
  {   // scrolling: a swap every 16 ms keeps 120; 1 s after it stops -> 60
    Policy p(c);
    p.applied(120, 0);
    for (uint64_t t = 0; t <= 3000; t += 16) p.swap(t);   // the last at 2992
    CHECK(p.decide(3000).hz == 120 && p.decide(3991).hz == 120 && p.decide(3992).hz == 60);
  }
  {   // the dwell: a burst right after the switch down waits for the dwell
    Policy p(c);
    p.applied(60, 1000);
    p.swap(1010);
    CHECK(p.swap(1050));
    CHECK(p.decide(1050).hz == 60 && p.decide(1050).nextMs == 1200 && p.decide(1200).hz == 120);
  }
  {   // disabled (a failed switch): no decision moves
    Policy p(c);
    p.applied(120, 0);
    p.disable();
    CHECK(!p.enabled() && p.decide(100000).hz == 120 && p.decide(100000).nextMs == 0);
    p.swap(1);
    CHECK(!p.swap(2));
  }
  {   // burstSwaps = 1: every swap is a burst
    PolicyConfig one = c;
    one.burstSwaps = 1;
    Policy p(one);
    p.applied(60, 0);
    CHECK(p.swap(500) && p.decide(500).hz == 120);
  }
}

// The policy driving the model: a session of swaps, the switches it asks for, each verified on the model.
static void testSession()
{
  model::Dpu m;
  m.sleepMs(3);
  Policy pol;
  pol.applied(120, 0);
  unsigned hz = 120, done = 0, bad = 0;
  uint64_t swapAt = 0;
  for (uint64_t t = 0; t < 30000; t += 5) {
    const uint64_t now = m.nowUs() / 1000;
    if (now < t) m.sleepMs(uint32_t(t - now));
    // 0-3 s: scrolling; 3-10 s: a cursor; 10-12 s: an animation; then idle with a cursor
    const bool scrolling = t < 3000 || (t >= 10000 && t < 12000);
    const uint64_t gap = scrolling ? 16 : 500;
    if (t >= swapAt) {
      pol.swap(t);
      swapAt = t + gap;
    }
    const Policy::Decision d = pol.decide(m.nowUs() / 1000);
    if (d.hz == hz) continue;
    const Switch s = switchTo(m, path(m), d.hz == 60 ? kVtotal60 : kVtotal120, Options());
    if (getenv("DPU_TRACE")) std::printf("t %llu: -> %u Hz %s\n", (unsigned long long)t, d.hz, s.ok ? "ok" : s.why);
    if (s.ok) {
      hz = d.hz;
      pol.applied(hz, m.nowUs() / 1000);
      ++done;
    } else {
      ++bad;
      pol.disable();
    }
  }
  CHECK(bad == 0 && done == 3 && hz == 60 && m.glitches == 0);   // down at 4 s, up at 10.02 s (the second swap), down at 13 s
}

int main()
{
  testPath();
  testSwitch();
  testCounters();
  testAnomalies();
  testPolicy();
  testSession();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("PianoDpu refresh host tests: all passed\n");
  return 0;
}
