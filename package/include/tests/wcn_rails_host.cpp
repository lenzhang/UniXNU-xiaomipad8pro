// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the shared WCN7860 rails helper (package/include/PianoWcnRails.h, docs/drivers/bluetooth.md §7.1), two simulated radios
// (Wi-Fi and Bluetooth, each its own PianoWcnRails with its own tag and hold mutex) over one pair of words and one model of the PMIC
// and apps DRV-2:
//   - the reference count: the first get votes the six rails (mode NPM, voltage, enable; DT order), the second only reads them back;
//     a put that is not the last changes nothing; the last put keeps the rails unless it asks to release, and a release withdraws only
//     the enables the helper added (a rail another master keeps enabled stays), never a PHY LDO;
//   - the voltage rules (DT minimum, the PMIC's own value when higher within the maximum, no voltage command above it), a rail that
//     cannot be read (refused unless blind), a refused transaction (the attempt's own enables withdrawn when nobody owns the rails,
//     recorded when somebody does), a rail that reads lower afterwards, a rail that dropped while up (voted again), the PPID page rule;
//   - the PMU lock: nesting, waiting and its reports, the bound (busy, nothing changed), a lost and an unbalanced give, an unbalanced
//     put, bad arguments;
//   - real threads: both radios get/put and raise their pins in loops: never two rail transactions at once, never both radios
//     inside their pin sections at once, every put balanced, the words free and ownerless at the end.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. wcn_rails_host.cpp -o /tmp/wcn_rails_host && /tmp/wcn_rails_host
#include "PianoWcnRails.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------- the PMIC and DRV-2 model
struct Regulator {
  bool present = true;
  uint8_t type = 0;
  bool othersEnable = false;   // another master's vote (or the boot state) keeps it on
  uint16_t othersMv = 0;
  bool appsEnable = false;     // apps DRV-2's vote
  uint16_t appsMv = 0;
  uint8_t mode = 0;
  bool unreadable = false;
  int lowerAfterVote = 0;      // mV the read-back shows below the set point once voted (a rail that sags)
  bool voted = false;
  bool enabled() const { return othersEnable || appsEnable; }
  uint16_t millivolts() const
  {
    const uint16_t mv = appsMv > othersMv ? appsMv : othersMv;
    return uint16_t(voted && lowerAfterVote ? mv - lowerAfterVote : mv);
  }
};

struct Tx {
  uint32_t tag;
  std::string resource;
  std::vector<PianoWcnVrmCmd> cmds;
};

struct Model {
  std::mutex m;
  std::map<uint16_t, Regulator> regs;
  std::map<std::string, uint16_t> byName;
  std::vector<Tx> txs;
  std::string failOn;            // a VRM transaction for this resource is refused
  std::atomic<int> inside{0};
  std::atomic<int> overlaps{0};
  bool slow = false;             // threads: each transaction takes a little while

  Model()
  {
    for (unsigned i = 0; i < PIANO_WCN_RAIL_COUNT; ++i) {
      const PianoWcnRail &t = piano_wcn_rail_table[i];
      const uint16_t ppid = uint16_t(t.sid << 8 | (t.smps ? piano_wcn_smps_page(t.smps) : piano_wcn_ldo_page(t.ldo)));
      Regulator r;
      r.type = t.smps ? 0x21 : 0x04;
      r.othersMv = t.min_mv;
      regs[ppid] = r;
      byName[t.cmd_db] = ppid;
    }
    // Piano at hand-over as the model has it: vddaon and vdddig on (other consumers), the PHY LDOs on, the rest off.
    reg("smpf5").othersEnable = true;
    reg("smpf5").othersMv = 880;
    reg("smpd4").othersEnable = true;
    reg("smpd4").othersMv = 880;
    reg("ldof1").othersEnable = true;
    reg("ldog3").othersEnable = true;
  }
  Regulator &reg(const char *name) { return regs[byName[name]]; }
  int vrm(uint32_t tag, const char *resource, const PianoWcnVrmCmd *cmds, unsigned count)
  {
    if (inside.fetch_add(1) != 0) overlaps++;
    if (slow) std::this_thread::sleep_for(std::chrono::microseconds(50));
    int rc = 0;
    {
      std::lock_guard<std::mutex> g(m);
      txs.push_back({tag, resource, std::vector<PianoWcnVrmCmd>(cmds, cmds + count)});
      auto it = byName.find(resource);
      if (it == byName.end() || failOn == resource || !count || count > 3) {
        rc = -5;
      } else {
        Regulator &r = regs[it->second];
        for (unsigned i = 0; i < count; ++i) {
          if (cmds[i].reg == PianoWcnVrmMode) r.mode = uint8_t(cmds[i].value);
          if (cmds[i].reg == PianoWcnVrmVoltage) r.appsMv = uint16_t(cmds[i].value);
          if (cmds[i].reg == PianoWcnVrmEnable) {
            r.appsEnable = cmds[i].value != 0;
            r.voted = r.voted || r.appsEnable;
          }
        }
      }
    }
    inside--;
    return rc;
  }
  int pmic(uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *mv)
  {
    std::lock_guard<std::mutex> g(m);
    auto it = regs.find(ppid);
    if (it == regs.end() || !it->second.present || it->second.unreadable) return -5;
    *status = it->second.enabled() ? 0x80 : 0;
    *enable = it->second.enabled() ? 0x80 : 0;
    *mode = it->second.mode;
    *mv = it->second.millivolts();
    return 0;
  }
  int pmicType(uint16_t ppid, uint8_t *type)
  {
    std::lock_guard<std::mutex> g(m);
    auto it = regs.find(ppid);
    if (it == regs.end() || !it->second.present) return -19;
    *type = it->second.type;
    return 0;
  }
  unsigned count(uint32_t tag) const
  {
    unsigned n = 0;
    for (const Tx &t : txs) n += t.tag == tag;
    return n;
  }
};

// ------------------------------------------------------------------------------------------------------------- a radio
struct Event {
  uint32_t event, a, b;
};
struct Radio {
  Model *model;
  std::mutex hold;
  uint32_t slept = 0;
  std::function<void(uint32_t)> onSleep;
  std::vector<Event> events;
  bool threaded = false;
  PianoWcnRailsOps ops{};
  PianoWcnRails rails{};
  Radio(Model *mo, volatile uint32_t *state, volatile uint32_t *lock, uint32_t tag, bool knowsPpids) : model(mo)
  {
    ops.context = this;
    ops.vrm = [](void *c, const char *res, const PianoWcnVrmCmd *cmds, unsigned n) {
      Radio *r = static_cast<Radio *>(c);
      return r->model->vrm(r->rails.tag, res, cmds, n);
    };
    ops.pmic = [](void *c, uint16_t ppid, uint8_t *s, uint8_t *e, uint8_t *m, uint16_t *mv) {
      return static_cast<Radio *>(c)->model->pmic(ppid, s, e, m, mv);
    };
    ops.pmic_type = [](void *c, uint16_t ppid, uint8_t *t) { return static_cast<Radio *>(c)->model->pmicType(ppid, t); };
    ops.sleep_ms = [](void *c, uint32_t ms) {
      Radio *r = static_cast<Radio *>(c);
      r->slept += ms;
      if (r->threaded) std::this_thread::sleep_for(std::chrono::microseconds(20 * ms));
      if (r->onSleep) r->onSleep(r->slept);
    };
    ops.mutex_lock = [](void *c) { static_cast<Radio *>(c)->hold.lock(); };
    ops.mutex_unlock = [](void *c) { static_cast<Radio *>(c)->hold.unlock(); };
    ops.report = [](void *c, uint32_t e, uint32_t a, uint32_t b) {
      Radio *r = static_cast<Radio *>(c);
      if (!r->threaded) r->events.push_back({e, a, b});
    };
    rails.state = state;
    rails.lock = lock;
    rails.tag = tag;
    rails.ops = &ops;
    if (knowsPpids) {   // PianoWifi's way: the PPIDs from its own survey
      for (unsigned i = 0; i < PIANO_WCN_RAIL_COUNT; ++i) {
        const PianoWcnRail &t = piano_wcn_rail_table[i];
        rails.ppid[i] = uint16_t(t.sid << 8 | (t.smps ? piano_wcn_smps_page(t.smps) : piano_wcn_ldo_page(t.ldo)));
      }
      ops.pmic_type = nullptr;
    }
  }
  bool saw(uint32_t e) const
  {
    for (const Event &x : events)
      if (x.event == e) return true;
    return false;
  }
};

static unsigned railIndex(const char *cmdDb)
{
  for (unsigned i = 0; i < PIANO_WCN_RAIL_COUNT; ++i)
    if (!strcmp(piano_wcn_rail_table[i].cmd_db, cmdDb)) return i;
  return 99;
}

// ------------------------------------------------------------------------------------------------------------- the tests
static void testReferenceCount()
{
  Model m;
  volatile uint32_t state = 0, lock = 0;
  Radio bt(&m, &state, &lock, PianoWcnTagBt, false), wifi(&m, &state, &lock, PianoWcnTagWifi, true);

  // Bluetooth first: the six rails voted in DT order, each mode NPM, a voltage, the enable.
  CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnOk);
  CHECK(m.count(PianoWcnTagBt) == 6 && m.txs.size() == 6);
  static const char *const kOrder[6] = {"ldof3", "ldof2", "smpf5", "smpd4", "smpi7", "smpg3"};
  for (unsigned i = 0; i < 6 && i < m.txs.size(); ++i) {
    const Tx &t = m.txs[i];
    CHECK(t.resource == kOrder[i] && t.cmds.size() == 3);
    CHECK(t.cmds[0].reg == PianoWcnVrmMode && t.cmds[0].value == PianoWcnVrmModeNpm);
    CHECK(t.cmds[1].reg == PianoWcnVrmVoltage && t.cmds[2].reg == PianoWcnVrmEnable && t.cmds[2].value == 1);
  }
  CHECK(m.txs[0].cmds[1].value == 1800 && m.txs[4].cmds[1].value == 1312 && m.txs[5].cmds[1].value == 1864);
  CHECK(m.txs[2].cmds[1].value == 880);   // vddaon read 880 mV (above its 876 minimum): kept
  const uint32_t added = (1u << 0 | 1u << 1 | 1u << 4 | 1u << 5) << PIANO_WCN_ADDED_SHIFT;   // the four that were off
  CHECK(state == (PIANO_WCN_BT | PIANO_WCN_UP | added) && lock == 0);
  CHECK(piano_wcn_rails_owners(&bt.rails) == (PIANO_WCN_BT | PIANO_WCN_UP));
  CHECK(bt.rails.voted == 6 && bt.rails.kept == 0 && bt.rails.votes == 1 && bt.saw(PianoWcnRailsVoted));
  for (const char *phy : {"ldof1", "ldog3"}) {   // the PHY LDOs: read, never voted
    for (const Tx &t : m.txs) CHECK(t.resource != phy);
    CHECK(bt.rails.rail[railIndex(phy)].read && bt.rails.rail[railIndex(phy)].read_after);
  }
  // Bluetooth again: idempotent, nothing sent.
  CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnOk && m.txs.size() == 6 && bt.rails.verifies == 1);

  // Wi-Fi second: a read-back, no transaction, both owners.
  CHECK(piano_wcn_rails_get(&wifi.rails, PIANO_WCN_WLAN) == PianoWcnOk);
  CHECK(m.txs.size() == 6 && wifi.rails.verifies == 1 && wifi.rails.votes == 0 && wifi.saw(PianoWcnRailsVerified));
  CHECK(state == (PIANO_WCN_BT | PIANO_WCN_WLAN | PIANO_WCN_UP | added));

  // Bluetooth leaves asking for a release: Wi-Fi still owns the rails, nothing is withdrawn.
  CHECK(piano_wcn_rails_put(&bt.rails, PIANO_WCN_BT | PIANO_WCN_RELEASE) == PIANO_WCN_WLAN);
  CHECK(m.txs.size() == 6 && bt.rails.withdrawn == 0 && state == (PIANO_WCN_WLAN | PIANO_WCN_UP | added));
  // Wi-Fi leaves without a release (its default): the rails stay voted and known up.
  CHECK(piano_wcn_rails_put(&wifi.rails, PIANO_WCN_WLAN) == 0);
  CHECK(m.txs.size() == 6 && state == (PIANO_WCN_UP | added));
  for (const char *n : kOrder) CHECK(m.reg(n).enabled());
  // Bluetooth comes back (a wake): the read-back alone.
  CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnOk && m.txs.size() == 6);
  // Its last put with a release: only the four enables it added are withdrawn; vddaon/vdddig (other consumers) stay on.
  CHECK(piano_wcn_rails_put(&bt.rails, PIANO_WCN_BT | PIANO_WCN_RELEASE) == 0);
  CHECK(m.txs.size() == 10 && bt.rails.withdrawn == (1u << 0 | 1u << 1 | 1u << 4 | 1u << 5) && state == 0);
  for (size_t i = 6; i < m.txs.size(); ++i)
    CHECK(m.txs[i].cmds.size() == 1 && m.txs[i].cmds[0].reg == PianoWcnVrmEnable && m.txs[i].cmds[0].value == 0);
  CHECK(!m.reg("ldof3").enabled() && !m.reg("ldof2").enabled() && !m.reg("smpi7").enabled() && !m.reg("smpg3").enabled());
  CHECK(m.reg("smpf5").enabled() && m.reg("smpd4").enabled() && m.reg("ldof1").enabled() && m.reg("ldog3").enabled());
  // A release with nothing added (everything was on already) withdraws nothing and keeps the rails known up.
  Model on;
  for (auto &[ppid, r] : on.regs) r.othersEnable = true;
  volatile uint32_t s2 = 0, l2 = 0;
  Radio a(&on, &s2, &l2, PianoWcnTagBt, false);
  CHECK(piano_wcn_rails_get(&a.rails, PIANO_WCN_BT) == PianoWcnOk && s2 == (PIANO_WCN_BT | PIANO_WCN_UP));
  CHECK(piano_wcn_rails_put(&a.rails, PIANO_WCN_BT | PIANO_WCN_RELEASE) == 0 && s2 == PIANO_WCN_UP && on.txs.size() == 6);
  // A put of a client that is not an owner: counted, the owners unchanged.
  CHECK(piano_wcn_rails_put(&a.rails, PIANO_WCN_WLAN) == 0 && a.rails.put_unbalanced == 1 && a.saw(PianoWcnRailsPutUnbalanced));
}

static void testVoltageRules()
{
  Model m;
  m.reg("smpg3").othersMv = 2100;    // above vddrfa1p8's 2000 mV maximum: no voltage command
  m.reg("smpi7").othersMv = 1330;    // within range above the minimum: sent again as it is
  m.reg("ldof3").othersMv = 1700;    // below the minimum: the minimum
  volatile uint32_t state = 0, lock = 0;
  Radio bt(&m, &state, &lock, PianoWcnTagBt, false);
  CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnOk);
  CHECK(bt.rails.voted == 5 && bt.rails.kept == 1);
  const unsigned g3 = railIndex("smpg3"), i7 = railIndex("smpi7"), f3 = railIndex("ldof3");
  CHECK(bt.rails.rail[g3].kept_voltage && bt.rails.rail[g3].voted == 0 && m.txs[5].cmds.size() == 2);
  CHECK(m.txs[5].cmds[0].reg == PianoWcnVrmMode && m.txs[5].cmds[1].reg == PianoWcnVrmEnable);
  CHECK(bt.rails.rail[i7].voted == 1330 && bt.rails.rail[f3].voted == 1800);
}

static void testFaults()
{
  {   // a voted rail that cannot be read: refused, nothing sent; blind votes it
    Model m;
    m.reg("smpd4").unreadable = true;
    volatile uint32_t state = 0, lock = 0;
    Radio bt(&m, &state, &lock, PianoWcnTagBt, false);
    CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnRailRead && bt.rails.step == -PianoWcnStepRead);
    CHECK(m.txs.empty() && state == 0 && lock == 0 && bt.saw(PianoWcnRailsFailed));
    bt.rails.blind = 1;
    CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnOk && m.txs.size() == 6 && (state & PIANO_WCN_BT));
  }
  {   // a missing PHY LDO is only read: no refusal
    Model m;
    m.reg("ldog3").unreadable = true;
    volatile uint32_t state = 0, lock = 0;
    Radio bt(&m, &state, &lock, PianoWcnTagBt, false);
    CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnOk);
  }
  {   // a refused transaction with nobody on the rails: the attempt's own enables are withdrawn again
    Model m;
    m.failOn = "smpi7";
    volatile uint32_t state = 0, lock = 0;
    Radio bt(&m, &state, &lock, PianoWcnTagBt, false);
    CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnRailVote && bt.rails.step == -PianoWcnStepVote);
    CHECK(state == 0 && lock == 0);
    CHECK(!m.reg("ldof3").enabled() && !m.reg("ldof2").enabled() && m.reg("smpf5").enabled() && m.reg("smpd4").enabled());
    CHECK(m.txs.size() == 5 + 2);   // four votes, the refused fifth, two withdrawals (ldof3, ldof2: smpf5/smpd4 were on)
  }
  {   // the read-back is lower than before: dropped
    Model m;
    m.reg("smpf5").lowerAfterVote = 30;
    volatile uint32_t state = 0, lock = 0;
    Radio bt(&m, &state, &lock, PianoWcnTagBt, false);
    CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnRailDropped && bt.rails.step == -PianoWcnStepCheck);
    CHECK(state == 0);
  }
  {   // a rail that dropped while up is voted again; a re-vote that fails with an owner present withdraws nothing
    Model m;
    volatile uint32_t state = 0, lock = 0;
    Radio bt(&m, &state, &lock, PianoWcnTagBt, false), wifi(&m, &state, &lock, PianoWcnTagWifi, true);
    CHECK(piano_wcn_rails_get(&wifi.rails, PIANO_WCN_WLAN) == PianoWcnOk && m.txs.size() == 6);
    m.reg("ldof2").appsEnable = false;   // something took apps' vote away
    CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnOk && bt.saw(PianoWcnRailsRevote) && m.txs.size() == 12);
    CHECK(state == (PIANO_WCN_WLAN | PIANO_WCN_BT | PIANO_WCN_UP | (0x33u << PIANO_WCN_ADDED_SHIFT)));
    m.reg("ldof2").appsEnable = false;
    m.failOn = "smpd4";
    CHECK(piano_wcn_rails_put(&bt.rails, PIANO_WCN_BT) == PIANO_WCN_WLAN);
    CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnRailVote);
    CHECK(state == (PIANO_WCN_WLAN | (0x33u << PIANO_WCN_ADDED_SHIFT)) && m.reg("ldof2").enabled());   // not up, nothing withdrawn
    m.failOn.clear();
    CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnOk && (state & PIANO_WCN_UP));
    // A release whose withdrawal is refused keeps that rail's bit for the next one.
    CHECK(piano_wcn_rails_put(&wifi.rails, PIANO_WCN_WLAN) == PIANO_WCN_BT);
    m.failOn = "ldof3";
    CHECK(piano_wcn_rails_put(&bt.rails, PIANO_WCN_BT | PIANO_WCN_RELEASE) == 0 && bt.rails.step == -PianoWcnStepRelease);
    CHECK(state == (0x01u << PIANO_WCN_ADDED_SHIFT) && m.reg("ldof3").enabled() && !m.reg("smpg3").enabled());
  }
  {   // the page rule: an LDO answering on the alternate page, found by its type
    Model m;
    const uint16_t primary = uint16_t(5 << 8 | piano_wcn_ldo_page(2)), alternate = uint16_t(5 << 8 | piano_wcn_ldo_page_alt(2));
    m.regs[alternate] = m.regs[primary];
    m.regs[primary].present = false;
    m.byName["ldof2"] = alternate;
    volatile uint32_t state = 0, lock = 0;
    Radio bt(&m, &state, &lock, PianoWcnTagBt, false);
    CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnOk && bt.rails.ppid[railIndex("ldof2")] == alternate);
    CHECK(bt.rails.ppid[railIndex("smpi7")] == uint16_t(8 << 8 | piano_wcn_smps_page(7)));
    Model w;   // a peripheral of the wrong kind at the primary page: not taken
    w.regs[uint16_t(6 << 8 | piano_wcn_smps_page(3))].type = 0x04;
    Radio c(&w, &state, &lock, PianoWcnTagWifi, false);
    state = 0;
    CHECK(piano_wcn_rails_get(&c.rails, PIANO_WCN_WLAN) == PianoWcnRailRead && c.rails.ppid[railIndex("smpg3")] == 0);
  }
  {   // bad arguments
    volatile uint32_t state = 0, lock = 0;
    Model m;
    Radio bt(&m, &state, &lock, PianoWcnTagBt, false);
    CHECK(piano_wcn_rails_get(&bt.rails, 0) == PianoWcnArgument && piano_wcn_rails_get(&bt.rails, 0x100) == PianoWcnArgument);
    CHECK(piano_wcn_rails_get(nullptr, PIANO_WCN_BT) == PianoWcnArgument && piano_wcn_pmu_lock(nullptr, 1) == PianoWcnArgument);
    bt.rails.tag = 0;
    CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnArgument && piano_wcn_rails_owners(nullptr) == 0);
  }
}

static void testLock()
{
  Model m;
  volatile uint32_t state = 0, lock = 0;
  Radio bt(&m, &state, &lock, PianoWcnTagBt, false);
  // Nesting: the enable-pin section around a get.
  CHECK(piano_wcn_pmu_lock(&bt.rails, 1) == PianoWcnOk && lock == PianoWcnTagBt && bt.rails.holds == 1);
  CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnOk && lock == PianoWcnTagBt && bt.rails.holds == 1);
  CHECK(piano_wcn_pmu_lock(&bt.rails, 0) == PianoWcnOk && lock == 0 && bt.rails.holds == 0);
  // Unbalanced give: counted, nothing changes.
  CHECK(piano_wcn_pmu_lock(&bt.rails, 0) == PianoWcnOk && bt.rails.unbalanced == 1 && bt.saw(PianoWcnLockUnbalanced));
  // Held by Wi-Fi: waited for, reported; it gives the lock back after 7 ms.
  lock = PianoWcnTagWifi;
  bt.events.clear();
  bt.slept = 0;
  bt.onSleep = [&](uint32_t slept) {
    if (slept == 7) lock = 0;
  };
  CHECK(piano_wcn_pmu_lock(&bt.rails, 1) == PianoWcnOk && lock == PianoWcnTagBt && bt.rails.contended == 1);
  CHECK(bt.events.size() == 2 && bt.events[0].event == PianoWcnLockWaiting && bt.events[0].a == PianoWcnTagWifi);
  CHECK(bt.events[1].event == PianoWcnLockGot && bt.events[1].b == 7 && bt.rails.max_wait_ms == 7);
  piano_wcn_pmu_lock(&bt.rails, 0);
  // Held past the bound: busy, the lock not taken, nothing changed (a get answers busy, a put still drops its bit).
  lock = PianoWcnTagWifi;
  bt.onSleep = nullptr;
  bt.rails.bound_ms = 12000;
  bt.events.clear();
  const uint32_t before = state;
  CHECK(piano_wcn_rails_get(&bt.rails, PIANO_WCN_BT) == PianoWcnBusy && lock == PianoWcnTagWifi && state == before);
  CHECK(bt.rails.busy == 1 && bt.saw(PianoWcnLockBusy) && bt.saw(PianoWcnLockStillWaiting) && bt.rails.holds == 0);
  CHECK(piano_wcn_rails_put(&bt.rails, PIANO_WCN_BT) == 0 && !(state & PIANO_WCN_BT) && (state & PIANO_WCN_UP));
  // A give that finds another tag (the word was cleared and taken meanwhile): counted as lost, left alone.
  lock = 0;
  CHECK(piano_wcn_pmu_lock(&bt.rails, 1) == PianoWcnOk);
  lock = PianoWcnTagWifi;
  CHECK(piano_wcn_pmu_lock(&bt.rails, 0) == PianoWcnOk && bt.rails.lost == 1 && lock == PianoWcnTagWifi);
}

static void testThreads()
{
  Model m;
  m.slow = true;
  volatile uint32_t state = 0, lock = 0;
  Radio bt(&m, &state, &lock, PianoWcnTagBt, false), wifi(&m, &state, &lock, PianoWcnTagWifi, true);
  bt.threaded = wifi.threaded = true;
  std::atomic<int> pinInside{0}, pinOverlaps{0}, getFailures{0};
  std::atomic<unsigned> maxOwners{0};
  auto run = [&](Radio &r, unsigned client, unsigned seed) {
    for (unsigned i = 0; i < 150; ++i) {
      // Raise the enable pin: the lock around the get and the pin, as both drivers do.
      if (piano_wcn_pmu_lock(&r.rails, 1)) {
        getFailures++;
        continue;
      }
      if (piano_wcn_rails_get(&r.rails, client)) getFailures++;
      if (pinInside.fetch_add(1) != 0) pinOverlaps++;
      unsigned owners = piano_wcn_rails_owners(&r.rails) & PIANO_WCN_CLIENTS;
      if (owners > maxOwners) maxOwners = owners;
      std::this_thread::sleep_for(std::chrono::microseconds(30));
      pinInside--;
      piano_wcn_pmu_lock(&r.rails, 0);
      std::this_thread::sleep_for(std::chrono::microseconds((seed * 7 + i * 13) % 40));
      piano_wcn_rails_put(&r.rails, client | ((i + seed) % 3 == 0 ? PIANO_WCN_RELEASE : 0));
    }
  };
  std::thread a([&] { run(bt, PIANO_WCN_BT, 1); }), b([&] { run(wifi, PIANO_WCN_WLAN, 2); });
  a.join();
  b.join();
  CHECK(m.overlaps == 0 && pinOverlaps == 0 && getFailures == 0);
  CHECK((state & PIANO_WCN_CLIENTS) == 0 && lock == 0 && bt.rails.holds == 0 && wifi.rails.holds == 0);
  CHECK(bt.rails.put_unbalanced == 0 && wifi.rails.put_unbalanced == 0 && bt.rails.lost == 0 && wifi.rails.lost == 0);
  CHECK(bt.rails.votes + wifi.rails.votes >= 1 && bt.rails.verifies + wifi.rails.verifies >= 1);
  CHECK(maxOwners != 0);
}

int main()
{
  testReferenceCount();
  testVoltageRules();
  testFaults();
  testLock();
  testThreads();
  if (failures) {
    std::printf("wcn_rails_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("wcn_rails_host: all tests passed\n");
  return 0;
}
