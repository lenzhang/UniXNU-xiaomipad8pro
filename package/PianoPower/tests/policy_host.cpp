// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of ChargePolicy.c, the charging policy (docs/drivers/charging-policy.md): the tables, the pieces, and the decisions
// against a model of the charger firmware (it takes or refuses writes and reads them back as the real one does: the input limit
// as requested, the float voltage as 2 mV - 3600, the charge current not at all) and of the PD source (9 V when asked, if it
// offers it). Every planned write is checked against the policy's allowlist and the owner's ceilings. Build and run:
//   cc -std=c11 -Wall -Wextra -Werror -c package/PianoPower/ChargePolicy.c -o /tmp/ChargePolicy.o
//   c++ -std=c++20 -Wall -Wextra -Werror -I package/PianoPower package/PianoPower/tests/policy_host.cpp /tmp/ChargePolicy.o \
//     -o /tmp/policy_host && /tmp/policy_host
// (tooling/k27/build-native-piano-power.sh runs it before the kext; the wiring into PianoPower is tested in power_host.cpp.)
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "ChargePolicy.h"
#include "PianoMca.h"

static int gFailures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++gFailures;                                                               \
    }                                                                            \
  } while (0)

// The ids are PianoMca.h's (the owner's codec); the policy repeats them to stand alone.
static_assert(PIANO_CHG_ENABLE == PIANO_MCA_CHARGE_ENABLE && PIANO_CHG_FCC == PIANO_MCA_FCC && PIANO_CHG_ICL == PIANO_MCA_ICL);
static_assert(PIANO_CHG_FV == PIANO_MCA_TERM_VOLT && PIANO_CHG_PD_FIXED == PIANO_MCA_PD_FIXED);
static_assert(int(PIANO_CHG_T_SDP) == int(PIANO_MCA_TYPE_SDP) && int(PIANO_CHG_T_CDP) == int(PIANO_MCA_TYPE_CDP) && int(PIANO_CHG_T_DCP) == int(PIANO_MCA_TYPE_DCP));
static_assert(int(PIANO_CHG_T_FLOAT) == int(PIANO_MCA_TYPE_FLOAT) && int(PIANO_CHG_T_HVDCP3P5) == int(PIANO_MCA_TYPE_HVDCP_3P5));
static_assert(int(PIANO_CHG_T_TYPEC) == int(PIANO_MCA_TYPE_C) && int(PIANO_CHG_T_PD) == int(PIANO_MCA_TYPE_PD) && int(PIANO_CHG_T_PPS) == int(PIANO_MCA_TYPE_PD_PPS));
static_assert(int(PIANO_CHG_T_PD_VERIFY) == int(PIANO_MCA_TYPE_PD_VERIFY) && int(PIANO_CHG_T_OCP) == int(PIANO_MCA_TYPE_DCP_B));  // 15: one id, two names

// ------------------------------------------------------------------------------------------------------- the firmware and source
struct Sim {
  PianoChargeConfig cfg;
  PianoChargeState st;
  PianoChargePlan plan;
  PianoChargeInput in;
  // the firmware
  uint32_t icl = 100, fcc = 0, fv = 4400;
  int enable = 1;
  bool refuseIcl = false, ignoreFv = false, ackWrites = true, readsOk = true, bothGauges = true;
  // the source
  bool source = true, follows9v = true, typecSource = true, pdosOk = true;
  int type = PIANO_CHG_T_DCP;
  std::vector<std::vector<uint32_t>> pdos;
  int busMv = 5000;
  int verify = 0;
  // the system and the battery (loadMa > 0): the bus current is min(input limit, load) and the bus busMv less ohms x that current;
  // above kneeMa the source's voltage gives way by kneeOhms more
  int loadMa = 0, kneeMa = 0;
  double ohms = 0, kneeOhms = 0;
  // the firmware's own input limit (0x2029): aiclCap > 0 holds it there below the request (the Mac's port: 1750-1800 under 2000)
  int aiclCap = 0;
  bool aiclRead = false;
  // the charger (charger > 0, docs §17): stuck keeps the battery at 0 mA whatever the limits (every native boot of 2026-11-12 after
  // about 40 s); a 0x2003 write of 0 followed by one of 1 clears it when toggleClears; otherwise the battery takes min(charger, fcc)
  // and the system loadBase, so the input asked is loadBase plus what the battery takes
  int charger = 0, loadBase = 0;
  bool stuck = false, toggleClears = true, sawZero = false;
  unsigned enableZeros = 0;
  // the battery
  int soc = 50, tbatC = 30, tWarmDc = 300, tCoolDc = 290, ibat = 1000, vbat = 3900;
  // the Type-C current and PianoThermal's board thermistors (0.1 degC; boardValid: PIANO_CHG_IN_NTC..CONN2 bits)
  uint32_t rpMa = 3000, boardValid = 0;
  int ntcDc = 300, chgrDc = 330, conn1Dc = 310, conn2Dc = 300;
  // time and counters
  uint64_t now = 0;
  uint32_t generation = 1, wakes = 0;
  uint8_t sleep = PIANO_CHG_AWAKE;
  std::vector<PianoChargeWrite> all;   // every write made
  unsigned plans = 0;

  Sim() { piano_charge_config_default(&cfg); piano_charge_init(&st); std::memset(&plan, 0, sizeof plan); }
  void temp(int c) { tbatC = c; tWarmDc = c * 10; tCoolDc = c * 10 - 10; }
  void fill()
  {
    std::memset(&in, 0, sizeof in);
    in.now_ms = now;
    in.generation = generation;
    in.wakes = wakes;
    in.sleep = sleep;
    if (!readsOk) return;
    in.valid = PIANO_CHG_IN_BUS | PIANO_CHG_IN_TYPE | PIANO_CHG_IN_ICL | PIANO_CHG_IN_ENABLE | PIANO_CHG_IN_FV | PIANO_CHG_IN_VERIFY |
               PIANO_CHG_IN_TBAT | PIANO_CHG_IN_TYPEC;
    in.bus_mv = source ? busMv : 0;
    if (charger > 0) {
      // the battery's share: nothing while stuck or disabled, else what the charge current and the input left after the system allow
      const int lim = aiclCap && aiclCap < int(icl) ? aiclCap : int(icl);
      int take = stuck || !enable || !source ? 0 : std::min(charger, int(fcc));
      if (loadBase + take > lim) take = std::max(lim - loadBase, 0);
      ibat = take;
      loadMa = loadBase + take;
    }
    const int held = aiclCap && aiclCap < int(icl) ? aiclCap : int(icl);
    if (source && loadMa > 0) {
      const int ma = held < loadMa ? held : loadMa;
      in.valid |= PIANO_CHG_IN_IBUS;
      in.ibus_ma = ma;
      in.bus_mv = busMv - int(ohms * ma) - (kneeMa && ma > kneeMa ? int(kneeOhms * (ma - kneeMa)) : 0);
    }
    if (source && (aiclCap || aiclRead)) {
      in.valid |= PIANO_CHG_IN_AICL;
      in.aicl_ma = held;
    }
    in.type = source ? type : 0;
    in.icl_ma = int32_t(icl);
    in.enable = enable;
    in.fv_raw = 2 * int32_t(fv) - 3600;
    in.verify = verify;
    in.tbat_c = tbatC;
    in.typec_source = source && typecSource;
    in.rp_ma = source && typecSource ? rpMa : 0;
    in.valid |= boardValid;
    in.t_ntc_dc = ntcDc;
    in.t_chgr_dc = chgrDc;
    in.t_conn1_dc = conn1Dc;
    in.t_conn2_dc = conn2Dc;
    if (source && pdosOk && (type == PIANO_CHG_T_PD || type == PIANO_CHG_T_PD_VERIFY || type == PIANO_CHG_T_PPS)) {
      in.valid |= PIANO_CHG_IN_PDOS;
      in.pdos = uint8_t(pdos.size());
      for (size_t i = 0; i < pdos.size() && i < 7; ++i)
        for (int k = 0; k < 3; ++k) in.pdo[i][k] = pdos[i][size_t(k)];
    }
    if (bothGauges) {
      in.valid |= PIANO_CHG_IN_GAUGES;
      in.soc_max_pct = soc;
      in.soc_min_pct = soc > 0 ? soc - 1 : 0;
      in.vbat_max_mv = vbat;
      in.t_warm_dc = tWarmDc;
      in.t_cool_dc = tCoolDc;
      in.ibat_ma = ibat;
    }
  }
  bool offers9v() const
  {
    for (auto &p : pdos)
      if ((p[0] == 9000 && p[1] == 9000) || (p[0] < p[1] && p[0] <= 9000 && p[1] >= 9000)) return true;
    return false;
  }
  void apply(const PianoChargeWrite &w)
  {
    int32_t ret = 0;
    switch (w.property) {
    case PIANO_CHG_ICL: if (refuseIcl) ret = 1; else icl = w.value; break;
    case PIANO_CHG_FCC: fcc = w.value; break;
    case PIANO_CHG_FV: if (!ignoreFv) fv = w.value; break;
    case PIANO_CHG_ENABLE:
      enable = int(w.value);
      if (!w.value) {sawZero = true; enableZeros++;}
      else if (sawZero && toggleClears) stuck = false;
      if (w.value) sawZero = false;
      break;
    case PIANO_CHG_PD_FIXED: busMv = w.value == 9000 && follows9v && offers9v() ? 9000 : 5000; break;
    case PIANO_CHG_PPS: busMv = follows9v && offers9v() ? int((w.value >> 16) * 20) : 5000; break;
    default: ret = 1;
    }
    piano_charge_ack(&st, w.property, w.value, ret);
  }
  // One decision: dt ms later; the writes checked, applied and acknowledged (unless ackWrites is off).
  void step(uint64_t dt = 5000)
  {
    now += dt;
    fill();
    piano_charge_decide(&cfg, &in, &st, &plan);
    check();
    if (ackWrites)
      for (unsigned i = 0; i < plan.writes; ++i) apply(plan.write[i]);
  }
  void check()
  {
    plans++;
    CHECK(plan.writes <= PIANO_CHG_MAX_WRITES);
    if (!cfg.writes) CHECK(plan.writes == 0);
    unsigned slots = 0;
    for (unsigned i = 0; i < plan.writes; ++i) {
      const PianoChargeWrite &w = plan.write[i];
      all.push_back(w);
      CHECK(piano_charge_write_allowed(&cfg, w.property, w.value, w.bytes));
      CHECK(!(slots & (1u << w.slot)));   // one write per slot and plan
      slots |= 1u << w.slot;
      if (w.property == PIANO_CHG_ICL) CHECK(w.value <= piano_charge_icl_max(&cfg) && w.value <= PIANO_CHG_RP_ABS_MAX);
      if (w.property == PIANO_CHG_ICL && w.value > (cfg.icl_cap_ma ? cfg.icl_cap_ma : PIANO_CHG_ICL_CAP))
        CHECK(cfg.rp && cfg.ramp && plan.icl_why == PIANO_CHG_ICL_RP);   // above the cap only a stepped Rp limit
      if (w.property == PIANO_CHG_FCC) CHECK(w.value <= (cfg.fcc_cap_ma ? cfg.fcc_cap_ma : PIANO_CHG_FCC_CAP));
      // a stop only from battery health, or as the charging restart's first write (its start, in the plan that starts it)
      if (w.property == PIANO_CHG_ENABLE && w.value == 0) CHECK(cfg.limit || (cfg.kick && (plan.kick_events & PIANO_CHG_KICK_EV_START)));
      if (plan.kick_events & PIANO_CHG_KICK_EV_START) CHECK(cfg.kick && cfg.writes);
    }
    if (plan.hold_awake) CHECK(cfg.limit && cfg.writes);
  }
  bool wrote(uint32_t property, uint32_t value) const
  {
    for (unsigned i = 0; i < plan.writes; ++i)
      if (plan.write[i].property == property && plan.write[i].value == value) return true;
    return false;
  }
  bool wrote(uint32_t property) const
  {
    for (unsigned i = 0; i < plan.writes; ++i)
      if (plan.write[i].property == property) return true;
    return false;
  }
  int index(uint32_t property) const
  {
    for (unsigned i = 0; i < plan.writes; ++i)
      if (plan.write[i].property == property) return int(i);
    return -1;
  }
  void pd(std::vector<std::vector<uint32_t>> p, int t = PIANO_CHG_T_PD) { pdos = std::move(p); type = t; }
};

static const std::vector<std::vector<uint32_t>> kPd5and9 = {{5000, 5000, 3000}, {9000, 9000, 2000}};

// ------------------------------------------------------------------------------------------------------------------ the tables
static void testTables()
{
  CHECK(piano_charge_class(0) == PIANO_CHG_C_REFUSED);
  CHECK(piano_charge_class(1) == PIANO_CHG_C_SDP && piano_charge_class(2) == PIANO_CHG_C_CDP);
  for (int t : {3, 5, 6, 7, 8}) CHECK(piano_charge_class(t) == PIANO_CHG_C_DCP);
  CHECK(piano_charge_class(4) == PIANO_CHG_C_FLOAT && piano_charge_class(15) == PIANO_CHG_C_FLOAT);
  CHECK(piano_charge_class(9) == PIANO_CHG_C_TYPEC && piano_charge_class(10) == PIANO_CHG_C_PD);
  CHECK(piano_charge_class(11) == PIANO_CHG_C_PD_PPS && piano_charge_class(12) == PIANO_CHG_C_PD_PPS);
  for (int t : {13, 14, 16, 17, 19, 20, -1}) CHECK(piano_charge_class(t) == PIANO_CHG_C_REFUSED);

  // input limit and charge current at 5 V
  CHECK(piano_charge_icl_5v(PIANO_CHG_C_SDP, 0) == 500 && piano_charge_fcc_full(PIANO_CHG_C_SDP, 0) == 500);
  CHECK(piano_charge_icl_5v(PIANO_CHG_C_CDP, 0) == 900 && piano_charge_fcc_full(PIANO_CHG_C_CDP, 0) == 900);
  CHECK(piano_charge_icl_5v(PIANO_CHG_C_DCP, 0) == 1500 && piano_charge_fcc_full(PIANO_CHG_C_DCP, 0) == 2000);
  CHECK(piano_charge_icl_5v(PIANO_CHG_C_FLOAT, 0) == 500 && piano_charge_fcc_full(PIANO_CHG_C_FLOAT, 0) == 1000);
  CHECK(piano_charge_icl_5v(PIANO_CHG_C_TYPEC, 0) == 500 && piano_charge_fcc_full(PIANO_CHG_C_TYPEC, 0) == 500);
  CHECK(piano_charge_icl_5v(PIANO_CHG_C_PD, 0) == 500);          // no 5 V offer known
  CHECK(piano_charge_icl_5v(PIANO_CHG_C_PD, 3000) == 1500);
  CHECK(piano_charge_icl_5v(PIANO_CHG_C_PD_PPS, 900) == 900);
  CHECK(piano_charge_icl_5v(PIANO_CHG_C_PD, 50) == 100);
  CHECK(piano_charge_fcc_full(PIANO_CHG_C_PD, 0) == 2000 && piano_charge_fcc_full(PIANO_CHG_C_PD_PPS, 0) == 2000);
  CHECK(piano_charge_icl_5v(PIANO_CHG_C_REFUSED, 3000) == 0 && piano_charge_fcc_full(PIANO_CHG_C_REFUSED, 0) == 0);
  // at 9 V
  CHECK(piano_charge_icl_9v(PIANO_CHG_C_PD, 2000) == 1600 && piano_charge_icl_9v(PIANO_CHG_C_PD, 1000) == 1000);
  CHECK(piano_charge_icl_9v(PIANO_CHG_C_PD_PPS, 3000) == 2500 && piano_charge_icl_9v(PIANO_CHG_C_DCP, 3000) == 0);
  CHECK(piano_charge_fcc_full(PIANO_CHG_C_PD, 1) == 3000 && piano_charge_fcc_full(PIANO_CHG_C_PD_PPS, 1) == 3500);

  // temperature: 500 below 15, full to 38, -300 per degree above, 500 at least
  CHECK(piano_charge_fcc_thermal(2000, 0, 30) == 500);
  CHECK(piano_charge_fcc_thermal(2000, 1, 14) == 500 && piano_charge_fcc_thermal(2000, 1, 15) == 2000);
  CHECK(piano_charge_fcc_thermal(2000, 1, 38) == 2000 && piano_charge_fcc_thermal(2000, 1, 39) == 1700);
  CHECK(piano_charge_fcc_thermal(2000, 1, 42) == 800 && piano_charge_fcc_thermal(2000, 1, 43) == 500);
  CHECK(piano_charge_fcc_thermal(2000, 1, 60) == 500 && piano_charge_fcc_thermal(3500, 1, 45) == 1400);
  CHECK(piano_charge_fcc_thermal(500, 1, 50) == 500 && piano_charge_fcc_thermal(0, 1, 30) == 0);
  CHECK(piano_charge_fcc_thermal(2000, 1, -5) == 500);

  // PDOs
  const uint32_t a[][3] = {{5000, 5000, 3000}, {9000, 9000, 2220}, {3300, 11000, 3250}};
  int pps = 0;
  CHECK(piano_charge_pdo_5v_ma(a, 3) == 3000);
  CHECK(piano_charge_pdo_9v_ma(a, 3, 1, &pps) == 2220 && !pps);       // the fixed PDO wins
  const uint32_t b[][3] = {{5000, 5000, 3000}, {3300, 11000, 3250}};
  CHECK(piano_charge_pdo_5v_ma(b, 2) == 3000);
  CHECK(piano_charge_pdo_9v_ma(b, 2, 0, &pps) == 0);                   // hv=1: fixed only
  CHECK(piano_charge_pdo_9v_ma(b, 2, 1, &pps) == 3000 && pps);         // capped at 3 A
  const uint32_t c[][3] = {{3300, 5900, 2000}, {5000, 5000, 3000}};
  CHECK(piano_charge_pdo_5v_ma(c, 2) == 2000);                         // the lowest covering 5 V
  const uint32_t d[][3] = {{5000, 5000, 3000}, {9000, 9000, 50}, {3300, 11000, 400}};
  CHECK(piano_charge_pdo_9v_ma(d, 3, 1, &pps) == 0);                   // too little at 9 V either way
  CHECK(piano_charge_pdo_5v_ma(d, 0) == 0);
  CHECK(piano_charge_pps_word(9000, 3000) == (450u << 16 | 60u));

  // float voltage: 4480, 4060 from 48 degC until below 46, hot without a temperature
  uint8_t hot = 0;
  CHECK(piano_charge_fv_target(&hot, 1, 30) == 4480 && !hot);
  CHECK(piano_charge_fv_target(&hot, 1, 48) == 4060 && hot);
  CHECK(piano_charge_fv_target(&hot, 1, 46) == 4060);
  CHECK(piano_charge_fv_target(&hot, 1, 45) == 4480);
  CHECK(piano_charge_fv_target(&hot, 0, 30) == 4060);
  CHECK(piano_charge_fv_raw(4400) == 5200 && piano_charge_fv_raw(4480) == 5360 && piano_charge_fv_raw(4060) == 4520);

  // battery health hysteresis
  CHECK(piano_charge_soc_hold(0, 79, 80, 75) == 0 && piano_charge_soc_hold(0, 80, 80, 75) == 1);
  CHECK(piano_charge_soc_hold(1, 76, 80, 75) == 1 && piano_charge_soc_hold(1, 75, 80, 75) == 0);
  CHECK(piano_charge_soc_hold(0, 77, 80, 75) == 0 && piano_charge_soc_hold(1, 100, 100, 95) == 0);

  // configuration
  PianoChargeConfig cfg;
  piano_charge_config_default(&cfg);
  CHECK(cfg.writes && !cfg.hv && !cfg.full_charge && !cfg.limit && cfg.stop_pct == 80 && cfg.resume_pct == 75);
  CHECK(cfg.icl_cap_ma == 1600 && cfg.fcc_cap_ma == 1500 && !cfg.sleep_charge);
  CHECK(piano_charge_config_limit(&cfg, 1, 80, 75) == 0 && cfg.limit);
  CHECK(piano_charge_config_limit(&cfg, 1, 90, 0) == 0 && cfg.stop_pct == 90 && cfg.resume_pct == 85);
  CHECK(piano_charge_config_limit(&cfg, 1, 80, 80) == -1 && cfg.stop_pct == 90);
  CHECK(piano_charge_config_limit(&cfg, 1, 49, 40) == -1 && piano_charge_config_limit(&cfg, 1, 101, 75) == -1);
  CHECK(piano_charge_config_limit(&cfg, 1, 60, 39) == -1);
  CHECK(piano_charge_config_limit(&cfg, 0, 0, 0) == 0 && !cfg.limit);

  // the allowlist
  piano_charge_config_default(&cfg);
  CHECK(piano_charge_write_allowed(&cfg, PIANO_CHG_ENABLE, 0, 1) && piano_charge_write_allowed(&cfg, PIANO_CHG_ENABLE, 1, 1));
  CHECK(!piano_charge_write_allowed(&cfg, PIANO_CHG_ENABLE, 2, 1) && !piano_charge_write_allowed(&cfg, PIANO_CHG_ENABLE, 1, 4));
  CHECK(piano_charge_write_allowed(&cfg, PIANO_CHG_ICL, 100, 4) && piano_charge_write_allowed(&cfg, PIANO_CHG_ICL, 1600, 4));
  CHECK(!piano_charge_write_allowed(&cfg, PIANO_CHG_ICL, 1601, 4) && !piano_charge_write_allowed(&cfg, PIANO_CHG_ICL, 99, 4));
  CHECK(piano_charge_write_allowed(&cfg, PIANO_CHG_FCC, 1500, 4) && !piano_charge_write_allowed(&cfg, PIANO_CHG_FCC, 1501, 4));
  CHECK(piano_charge_write_allowed(&cfg, PIANO_CHG_FV, 4400, 4) && !piano_charge_write_allowed(&cfg, PIANO_CHG_FV, 4480, 4));
  CHECK(piano_charge_write_allowed(&cfg, PIANO_CHG_PD_FIXED, 5000, 4) && !piano_charge_write_allowed(&cfg, PIANO_CHG_PD_FIXED, 9000, 4));
  CHECK(!piano_charge_write_allowed(&cfg, PIANO_CHG_PPS, piano_charge_pps_word(9000, 3000), 4));
  CHECK(!piano_charge_write_allowed(&cfg, 0x2019, 0, 1) && !piano_charge_write_allowed(&cfg, 0x201a, 0, 16));
  cfg.full_charge = 1; cfg.hv = 2; cfg.icl_cap_ma = 2500; cfg.fcc_cap_ma = 3500;
  CHECK(piano_charge_write_allowed(&cfg, PIANO_CHG_FV, 4480, 4) && piano_charge_write_allowed(&cfg, PIANO_CHG_FV, 4060, 4));
  CHECK(!piano_charge_write_allowed(&cfg, PIANO_CHG_FV, 4540, 4) && !piano_charge_write_allowed(&cfg, PIANO_CHG_FV, 4500, 4));
  CHECK(piano_charge_write_allowed(&cfg, PIANO_CHG_PD_FIXED, 9000, 4) && !piano_charge_write_allowed(&cfg, PIANO_CHG_PD_FIXED, 12000, 4));
  CHECK(piano_charge_write_allowed(&cfg, PIANO_CHG_PPS, piano_charge_pps_word(9000, 3000), 4));
  CHECK(!piano_charge_write_allowed(&cfg, PIANO_CHG_PPS, piano_charge_pps_word(9000, 3050), 4));
  CHECK(!piano_charge_write_allowed(&cfg, PIANO_CHG_PPS, piano_charge_pps_word(8000, 3000), 4));
  CHECK(!piano_charge_write_allowed(&cfg, PIANO_CHG_PPS, piano_charge_pps_word(9000, 450), 4));
  CHECK(piano_charge_write_allowed(&cfg, PIANO_CHG_ICL, 2500, 4) && piano_charge_write_allowed(&cfg, PIANO_CHG_FCC, 3500, 4));
  cfg.icl_cap_ma = 9000; cfg.fcc_cap_ma = 9000;   // the absolute ceilings hold whatever the configuration says
  CHECK(!piano_charge_write_allowed(&cfg, PIANO_CHG_ICL, 2501, 4) && !piano_charge_write_allowed(&cfg, PIANO_CHG_FCC, 3501, 4));
}

// ------------------------------------------------------------------------------------------------------------ the 5 V decisions
static void testFiveVolt()
{
  // A DCP at 30 degC: the input limit (it goes up) then the charge current (the stage A ceilings), once.
  {
    Sim s;
    s.step();
    CHECK(s.plan.events & PIANO_CHG_EV_ATTACH);
    CHECK(s.plan.cls == PIANO_CHG_C_DCP && s.plan.icl_ma == 1500 && s.plan.fcc_ma == 1500);
    CHECK(s.plan.writes == 2 && s.plan.write[0].property == PIANO_CHG_ICL && s.plan.write[0].value == 1500);
    CHECK(s.plan.write[1].property == PIANO_CHG_FCC && s.plan.write[1].value == 1500);
    CHECK(!s.plan.hold_awake);
    s.step();
    CHECK(s.plan.writes == 0);
    // the firmware loses the limit: one disagreeing read-back is tolerated (it may predate the write), the second is not
    s.icl = 100;
    s.step();
    CHECK(s.plan.writes == 0);
    s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, 1500));
    s.step();
    CHECK(s.plan.writes == 0);
    // detach: the firmware's own values back, only where written
    s.source = false;
    s.step();
    CHECK(s.plan.events & PIANO_CHG_EV_DETACH);
    CHECK(s.wrote(PIANO_CHG_ICL, 100) && s.wrote(PIANO_CHG_FCC, 500) && !s.wrote(PIANO_CHG_ENABLE));
    s.step();
    CHECK(s.plan.writes == 0);
    s.step(60000);
    CHECK(s.plan.writes == 0);
  }
  // SDP, CDP, Type-C, float, OCP (15)
  for (auto [type, icl, fcc] : {std::tuple{1, 500u, 500u}, {2, 900u, 900u}, {9, 500u, 500u}, {4, 500u, 1000u}, {15, 500u, 1000u}}) {
    Sim s;
    s.type = type;
    s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, icl) && s.wrote(PIANO_CHG_FCC, fcc));
  }
  // QC is held at 5 V with the DCP values
  {
    Sim s;
    s.type = PIANO_CHG_T_HVDCP3;
    s.cfg.icl_cap_ma = 2500; s.cfg.fcc_cap_ma = 3500;
    s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, 1500) && s.wrote(PIANO_CHG_FCC, 2000));
  }
  // unknown and reserved sources: nothing
  for (int type : {0, 13, 14, 16}) {
    Sim s;
    s.type = type;
    for (int i = 0; i < 5; ++i) { s.step(); CHECK(s.plan.writes == 0); }
    CHECK(s.plan.cls == PIANO_CHG_C_REFUSED);
  }
  // a PD source at 5 V: at most its 5 V offer, 500 mA until its PDOs are read
  {
    Sim s;
    s.pd({{5000, 5000, 1200}, {9000, 9000, 2000}});
    s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, 1200));
    Sim t;
    t.pd({});
    t.step();
    CHECK(t.wrote(PIANO_CHG_ICL, 500));
  }
  // no bus reading: no decision, no write
  {
    Sim s;
    s.step();
    s.readsOk = false;
    s.step();
    CHECK(s.plan.writes == 0 && !(s.plan.events & PIANO_CHG_EV_DETACH));
    s.readsOk = true;
    s.step();
    CHECK(s.plan.writes == 0);
  }
  // writes off: the decisions for the log, nothing planned
  {
    Sim s;
    s.cfg.writes = 0;
    s.cfg.limit = 1;
    for (int i = 0; i < 10; ++i) { s.step(); CHECK(s.plan.writes == 0 && !s.plan.hold_awake); }
    CHECK(s.plan.icl_ma == 1500);
    Sim t;
    t.cfg.writes = 0;
    t.cfg.hv = 1;
    t.pd(kPd5and9);
    for (int i = 0; i < 10; ++i) {
      t.step();
      CHECK(t.plan.writes == 0 && !(t.plan.events & (PIANO_CHG_EV_HV_ASKED | PIANO_CHG_EV_HV_BLOCKED)) && t.plan.hv == PIANO_CHG_HV_OFF);
    }
  }
  // the type changes while attached: new limits
  {
    Sim s;
    s.type = PIANO_CHG_T_SDP;
    s.step();
    s.step();
    s.type = PIANO_CHG_T_DCP;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_TYPE) && s.wrote(PIANO_CHG_ICL, 1500) && s.wrote(PIANO_CHG_FCC, 1500));
  }
  // three refusals: given up until the next attach
  {
    Sim s;
    s.refuseIcl = true;
    s.step(); CHECK(s.wrote(PIANO_CHG_ICL));
    s.step(); CHECK(s.wrote(PIANO_CHG_ICL));
    s.step(); CHECK(s.wrote(PIANO_CHG_ICL));
    s.step(); CHECK(!s.wrote(PIANO_CHG_ICL) && (s.plan.events & PIANO_CHG_EV_GAVE_UP));
    s.step(); CHECK(s.plan.writes == 0);
    s.refuseIcl = false;
    s.source = false; s.step();
    s.source = true; s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, 1500));
  }
  // a write with no answer is not sent twice; the owner gets it again after 10 s
  {
    Sim s;
    s.ackWrites = false;
    s.step(); CHECK(s.wrote(PIANO_CHG_ICL));
    s.step(); CHECK(s.plan.writes == 0);
    s.step(6000); CHECK(s.wrote(PIANO_CHG_ICL) && s.wrote(PIANO_CHG_FCC));
  }
  // a new charger generation: the firmware is on its defaults, everything is sent again
  {
    Sim s;
    s.step(); s.step();
    CHECK(s.plan.writes == 0);
    s.generation++;
    s.icl = 100;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_GENERATION) && s.wrote(PIANO_CHG_ICL, 1500) && s.wrote(PIANO_CHG_FCC, 1500));
  }
}

// ------------------------------------------------------------------------------------------------------- temperature and float
static void testThermal()
{
  Sim s;
  s.cfg.icl_cap_ma = 2500; s.cfg.fcc_cap_ma = 3500;
  s.temp(40);
  s.step();
  CHECK(s.wrote(PIANO_CHG_FCC, 1400));   // 2000 - 2 x 300
  s.temp(44);
  s.step();
  CHECK(s.wrote(PIANO_CHG_FCC, 500));    // down at once
  s.temp(30);
  s.step(10000);
  CHECK(!s.wrote(PIANO_CHG_FCC));        // up only 30 s after the last change
  s.step(20000);
  CHECK(s.wrote(PIANO_CHG_FCC, 800));    // one 300 mA step
  s.step(29000);
  CHECK(!s.wrote(PIANO_CHG_FCC));
  s.step(1000);
  CHECK(s.wrote(PIANO_CHG_FCC, 1100));
  for (int i = 0; i < 10; ++i) s.step(30000);
  CHECK(s.fcc == 2000);
  s.temp(39);                            // a degree to spare going up: 39 gives 1700 at once going down
  s.step();
  CHECK(s.wrote(PIANO_CHG_FCC, 1700));
  s.temp(10);
  s.step();
  CHECK(s.wrote(PIANO_CHG_FCC, 500));
  // without any temperature: 500
  Sim t;
  t.cfg.icl_cap_ma = 2500; t.cfg.fcc_cap_ma = 3500;
  t.bothGauges = false;
  t.tbatC = 200;   // implausible: ignored
  t.step();
  CHECK(t.wrote(PIANO_CHG_FCC, 500));
  // the warmer reading counts: the firmware 30, a pack 41
  Sim u;
  u.cfg.icl_cap_ma = 2500; u.cfg.fcc_cap_ma = 3500;
  u.tbatC = 30; u.tWarmDc = 415; u.tCoolDc = 300;
  u.step();
  CHECK(u.wrote(PIANO_CHG_FCC, 1100));

  // the float voltage (opt-in), read back from 0x2007
  Sim f;
  f.cfg.full_charge = 1;
  f.step();
  CHECK(f.wrote(PIANO_CHG_FV, 4480) && f.index(PIANO_CHG_FV) == int(f.plan.writes) - 1);   // the last write
  f.step();
  CHECK(!f.wrote(PIANO_CHG_FV) && f.fv == 4480);
  f.temp(48); f.step(); CHECK(f.wrote(PIANO_CHG_FV, 4060));
  f.temp(47); f.step(); CHECK(!f.wrote(PIANO_CHG_FV));
  f.temp(45); f.step(); CHECK(f.wrote(PIANO_CHG_FV, 4480));
  f.cfg.full_charge = 0; f.step(); CHECK(f.wrote(PIANO_CHG_FV, 4400));
  f.step(); CHECK(!f.wrote(PIANO_CHG_FV));
  f.step(); CHECK(f.plan.writes == 0 && f.plan.fv_mv == 0);
  // after a wake the firmware still has it: not sent again
  Sim g;
  g.cfg.full_charge = 1;
  g.fv = 4480;
  g.step();
  CHECK(!g.wrote(PIANO_CHG_FV));
  // a firmware that takes it but reads it back otherwise: written again at most three times, then left alone
  Sim h;
  h.cfg.full_charge = 1;
  h.ignoreFv = true;
  unsigned fvWrites = 0;
  bool readback = false;
  for (int i = 0; i < 30; ++i) {
    h.step();
    fvWrites += h.wrote(PIANO_CHG_FV);
    readback |= (h.plan.events & PIANO_CHG_EV_READBACK) != 0;
  }
  CHECK(fvWrites == 3 && readback);
}

// --------------------------------------------------------------------------------------------------------------------- 9 V
static void testNineVolt()
{
  // a fixed 9 V PDO, 15 s after the attach, then the 9 V row
  {
    Sim s;
    s.cfg.hv = 1;
    s.pd(kPd5and9);
    s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, 1500) && !s.wrote(PIANO_CHG_PD_FIXED));
    s.step(); s.step();
    CHECK(s.plan.writes == 0);
    s.step();   // 20 s
    CHECK(s.wrote(PIANO_CHG_PD_FIXED, 9000) && (s.plan.events & PIANO_CHG_EV_HV_ASKED));
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_HV_ON) && s.wrote(PIANO_CHG_ICL, 1600) && s.plan.bus_mv == 9000);
    CHECK(!s.wrote(PIANO_CHG_FCC));   // 3000 at 9 V, but the stage A ceiling is 1500 already
    s.step();
    CHECK(s.plan.writes == 0);
    // a failed PDO read is no change of offer
    s.pdosOk = false;
    s.step();
    CHECK(s.plan.writes == 0 && s.plan.hv == PIANO_CHG_HV_ON);
    s.pdosOk = true;
    // the source goes back to 5 V by itself: asked again after 10 s, three times per attach
    s.busMv = 5000;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_HV_RETRY) && s.wrote(PIANO_CHG_ICL, 1500));
    s.step();
    CHECK(!s.wrote(PIANO_CHG_PD_FIXED));
    s.step(6000);
    CHECK(s.wrote(PIANO_CHG_PD_FIXED, 9000));
  }
  // the stage B ceilings: 9 V from a plain PD source is 1600 / 3000
  {
    Sim s;
    s.cfg.hv = 1; s.cfg.icl_cap_ma = 2500; s.cfg.fcc_cap_ma = 3500;
    s.pd(kPd5and9);
    for (int i = 0; i < 6; ++i) s.step();
    CHECK(s.icl == 1600 && s.fcc == 3000 && s.busMv == 9000);
  }
  // a source that does not reach 9 V: back to 5 V after 15 s and 3 decisions, then left at 5 V until detach
  {
    Sim s;
    s.cfg.hv = 1;
    s.follows9v = false;
    s.pd(kPd5and9);
    for (int i = 0; i < 4; ++i) s.step();
    CHECK(s.wrote(PIANO_CHG_PD_FIXED, 9000));
    s.step(); s.step();
    CHECK(!s.wrote(PIANO_CHG_PD_FIXED));
    s.step();
    CHECK(s.wrote(PIANO_CHG_PD_FIXED, 5000) && (s.plan.events & PIANO_CHG_EV_HV_BLOCKED));
    for (int i = 0; i < 20; ++i) { s.step(); CHECK(!s.wrote(PIANO_CHG_PD_FIXED)); }
    s.source = false; s.step();
    s.source = true; s.follows9v = true;
    for (int i = 0; i < 4; ++i) s.step();
    CHECK(s.wrote(PIANO_CHG_PD_FIXED, 9000));
  }
  // a detach while 9 V is asked (a hard reset): not asked again for 60 s
  {
    Sim s;
    s.cfg.hv = 1;
    s.pd(kPd5and9);
    s.ackWrites = false;
    for (int i = 0; i < 4; ++i) s.step();
    CHECK(s.wrote(PIANO_CHG_PD_FIXED, 9000));
    s.ackWrites = true;
    s.source = false; s.step();
    s.source = true;
    bool asked = false;
    for (int i = 0; i < 10; ++i) { s.step(); asked |= s.wrote(PIANO_CHG_PD_FIXED, 9000); }
    CHECK(!asked);
    s.step(15000);
    CHECK(s.wrote(PIANO_CHG_PD_FIXED, 9000));
  }
  // never while an authentication runs (#20)
  {
    Sim s;
    s.cfg.hv = 1;
    s.pd(kPd5and9);
    s.verify = 1;
    for (int i = 0; i < 10; ++i) { s.step(); CHECK(!s.wrote(PIANO_CHG_PD_FIXED)); }
    s.verify = 0;
    s.step();
    CHECK(s.wrote(PIANO_CHG_PD_FIXED, 9000));
  }
  // a PPS APDO covering 9 V (hv=2 only): the PPS request at its current (3 A at most), the PD_PPS row
  {
    Sim s;
    s.cfg.hv = 1;
    s.pd({{5000, 5000, 3000}, {3300, 11000, 3250}}, PIANO_CHG_T_PPS);
    for (int i = 0; i < 6; ++i) { s.step(); CHECK(!s.wrote(PIANO_CHG_PPS) && !s.wrote(PIANO_CHG_PD_FIXED, 9000)); }
    Sim t;
    t.cfg.hv = 2; t.cfg.icl_cap_ma = 2500; t.cfg.fcc_cap_ma = 3500;
    t.pd({{5000, 5000, 3000}, {3300, 11000, 3250}}, PIANO_CHG_T_PPS);
    for (int i = 0; i < 4; ++i) t.step();
    CHECK(t.wrote(PIANO_CHG_PPS, 450u << 16 | 60u));
    t.step();
    CHECK(t.wrote(PIANO_CHG_ICL, 2500) && t.wrote(PIANO_CHG_FCC, 3500));
    // thermal derating applies at 9 V too
    t.temp(41);
    t.step();
    CHECK(t.wrote(PIANO_CHG_FCC, 2600));
  }
  // hv off at 9 V without a request of ours (a wake without a handover): taken as on when allowed
  {
    Sim s;
    s.cfg.hv = 1;
    s.pd(kPd5and9);
    s.busMv = 9000;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_HV_ON) && s.wrote(PIANO_CHG_ICL, 1600));
  }
}

// -------------------------------------------------------------------------------------------------------- Preserve Battery Health
static void testLimit()
{
  Sim s;
  s.cfg.limit = 1;
  s.cfg.hv = 1;
  s.pd(kPd5and9);
  s.soc = 70;
  for (int i = 0; i < 6; ++i) s.step();
  CHECK(s.busMv == 9000 && s.plan.hold_awake && s.plan.charge);
  // at the limit: the stop first, the source back at 5 V, the 5 V limits, no hold
  s.soc = 80;
  s.step();
  CHECK((s.plan.events & PIANO_CHG_EV_LIMIT_STOP) && s.wrote(PIANO_CHG_ENABLE, 0) && s.index(PIANO_CHG_ENABLE) == 0);
  CHECK(s.wrote(PIANO_CHG_PD_FIXED, 5000) && s.wrote(PIANO_CHG_ICL, 1500));
  CHECK(!s.plan.hold_awake && !s.plan.charge && s.plan.why_off == PIANO_CHG_OFF_LIMIT);
  s.step();
  CHECK(s.plan.writes == 0 && s.enable == 0 && s.busMv == 5000);
  // between the thresholds nothing changes; no 9 V while stopped
  for (int soc : {79, 77, 76}) {
    s.soc = soc;
    s.step();
    CHECK(s.plan.writes == 0 && !s.plan.hold_awake);
  }
  // the stop's read-back: 0x2003 reads 0, the packs must not take current a minute on
  s.ibat = 400;
  bool leak = false;
  for (int i = 0; i < 14; ++i) { s.step(); leak |= (s.plan.events & PIANO_CHG_EV_STOP_LEAK) != 0; }
  CHECK(leak);
  s.ibat = -300;
  // the firmware sets the bit again by itself: written again after two read-backs
  s.enable = 1;
  s.step(); CHECK(!s.wrote(PIANO_CHG_ENABLE));
  s.step(); CHECK(s.wrote(PIANO_CHG_ENABLE, 0));
  // resume at 75
  s.soc = 75;
  s.step();
  CHECK((s.plan.events & PIANO_CHG_EV_LIMIT_RESUME) && s.wrote(PIANO_CHG_ENABLE, 1) && s.plan.hold_awake);
  CHECK(s.wrote(PIANO_CHG_PD_FIXED, 9000) && s.index(PIANO_CHG_PD_FIXED) < s.index(PIANO_CHG_ENABLE));   // 9 V again, then the resume
  s.step();
  CHECK(s.wrote(PIANO_CHG_ICL, 1600) && s.plan.hv == PIANO_CHG_HV_ON);
  // a firmware 0 the policy never wrote is reported, not overridden
  {
    Sim t;
    t.enable = 0;
    t.step();
    CHECK((t.plan.events & PIANO_CHG_EV_FW_DISABLED) && !t.wrote(PIANO_CHG_ENABLE));
    t.step();
    CHECK(!(t.plan.events & PIANO_CHG_EV_FW_DISABLED));
  }
  // limit off: never a 0 write, never a hold
  {
    Sim t;
    t.soc = 95;
    for (int i = 0; i < 5; ++i) { t.step(); CHECK(!t.wrote(PIANO_CHG_ENABLE) && !t.plan.hold_awake); }
  }
  // no charge reading: the decision stands for 5 min, then charging goes on
  {
    Sim t;
    t.cfg.limit = 1;
    t.soc = 85;
    t.step();
    CHECK(t.wrote(PIANO_CHG_ENABLE, 0));
    t.bothGauges = false;
    t.step(60000);   // the first decision without a reading: the 5 min start here
    CHECK(!t.wrote(PIANO_CHG_ENABLE) && !t.plan.hold_awake);
    t.step(299000);
    CHECK(!t.wrote(PIANO_CHG_ENABLE));
    t.step(1000);
    CHECK((t.plan.events & PIANO_CHG_EV_SOC_LOST) && t.wrote(PIANO_CHG_ENABLE, 1) && !t.plan.hold_awake);
  }
  // the mode's hot and cold stops (55 / below 53, below 0 / 2)
  {
    Sim t;
    t.cfg.limit = 1;
    t.soc = 50;
    t.step();
    t.temp(55); t.step(); CHECK((t.plan.events & PIANO_CHG_EV_HOT) && t.wrote(PIANO_CHG_ENABLE, 0) && t.plan.why_off == PIANO_CHG_OFF_HOT);
    t.temp(54); t.step(); CHECK(!t.wrote(PIANO_CHG_ENABLE) && !t.plan.charge);
    t.temp(52); t.step(); CHECK(t.wrote(PIANO_CHG_ENABLE, 1));
    t.tbatC = 10; t.tWarmDc = 100; t.tCoolDc = -5;
    t.step(); CHECK((t.plan.events & PIANO_CHG_EV_COLD) && t.wrote(PIANO_CHG_ENABLE, 0));
    t.tCoolDc = 15; t.step(); CHECK(!t.wrote(PIANO_CHG_ENABLE));
    t.tCoolDc = 20; t.step(); CHECK(t.wrote(PIANO_CHG_ENABLE, 1));
  }
  // a stop of ours is lifted on detach (the next attach decides again) and at shutdown
  {
    Sim t;
    t.cfg.limit = 1;
    t.soc = 90;
    t.step();
    CHECK(t.wrote(PIANO_CHG_ENABLE, 0));
    t.source = false;
    t.step();
    CHECK(t.wrote(PIANO_CHG_ENABLE, 1));
    t.source = true;
    t.step();
    CHECK(t.wrote(PIANO_CHG_ENABLE, 0));
    t.fill();
    piano_charge_shutdown(&t.cfg, &t.in, &t.st, &t.plan);
    CHECK(t.wrote(PIANO_CHG_ENABLE, 1) && !t.plan.sleep_ready);
    for (unsigned i = 0; i < t.plan.writes; ++i) t.apply(t.plan.write[i]);
    piano_charge_shutdown(&t.cfg, &t.in, &t.st, &t.plan);
    CHECK(t.plan.writes == 0 && t.plan.sleep_ready);
  }
}

// --------------------------------------------------------------------------------------------------- sleep, wake, #18, shutdown
static void testSleep()
{
  // protected charging at 9 V below the limit, then a demand sleep: stopped for the sleep, 5 V limits, 5 V
  {
    Sim s;
    s.cfg.limit = 1; s.cfg.hv = 1;
    s.pd(kPd5and9);
    s.soc = 60;
    for (int i = 0; i < 6; ++i) s.step();
    CHECK(s.busMv == 9000 && s.icl == 1600 && s.plan.hold_awake);
    s.sleep = PIANO_CHG_WILL_SLEEP;
    s.step(100);
    CHECK((s.plan.events & PIANO_CHG_EV_SLEEP) && !s.plan.sleep_ready && !s.plan.hold_awake);
    CHECK(s.wrote(PIANO_CHG_ENABLE, 0) && s.wrote(PIANO_CHG_ICL, 1500) && s.wrote(PIANO_CHG_PD_FIXED, 5000));
    CHECK(s.plan.why_off == PIANO_CHG_OFF_SLEEP);
    s.step(100);
    CHECK(s.plan.writes == 0 && s.plan.sleep_ready);
    // the wake: everything again, charging resumed, 9 V 15 s later
    s.sleep = PIANO_CHG_AWAKE;
    s.wakes++;
    s.step(100);
    CHECK(s.plan.resync && (s.plan.events & PIANO_CHG_EV_WAKE) && (s.plan.events & PIANO_CHG_EV_ATTACH));
    CHECK(s.wrote(PIANO_CHG_ENABLE, 1) && s.wrote(PIANO_CHG_ICL, 1500) && s.wrote(PIANO_CHG_FCC, 1500) && s.plan.hold_awake);
    bool asked = false;
    for (int i = 0; i < 4; ++i) { s.step(); asked |= s.wrote(PIANO_CHG_PD_FIXED, 9000); }
    CHECK(asked);
  }
  // sleep_charge=1: charging goes on through the sleep (the limit is not watched)
  {
    Sim s;
    s.cfg.limit = 1; s.cfg.sleep_charge = 1;
    s.soc = 60;
    s.step();
    s.sleep = PIANO_CHG_WILL_SLEEP;
    s.step(100);
    CHECK(!s.wrote(PIANO_CHG_ENABLE) && s.plan.sleep_ready);
  }
  // at the limit already: stays stopped, nothing more to send
  {
    Sim s;
    s.cfg.limit = 1;
    s.soc = 85;
    s.step();
    s.step();
    s.sleep = PIANO_CHG_WILL_SLEEP;
    s.step(100);
    CHECK(s.plan.writes == 0 && s.plan.sleep_ready && s.plan.why_off == PIANO_CHG_OFF_LIMIT);
  }
  // limit off at 5 V: the limits already are the 5 V ones: nothing to send
  {
    Sim s;
    s.step();
    s.sleep = PIANO_CHG_WILL_SLEEP;
    s.step(100);
    CHECK(s.plan.writes == 0 && s.plan.sleep_ready);
  }
  // nothing attached: ready at once (a tablet that charged once must still sleep on battery)
  {
    Sim s;
    s.step();
    s.source = false;
    s.step();
    s.step();
    s.sleep = PIANO_CHG_WILL_SLEEP;
    s.step(100);
    CHECK(s.plan.writes == 0 && s.plan.sleep_ready);
  }
  // a refused handover write is not waited for after its tries
  {
    Sim s;
    s.cfg.hv = 1;
    s.pd(kPd5and9);
    for (int i = 0; i < 6; ++i) s.step();
    s.refuseIcl = true;
    s.sleep = PIANO_CHG_WILL_SLEEP;
    bool ready = false;
    for (int i = 0; i < 6 && !ready; ++i) { s.step(100); ready = s.plan.sleep_ready; }
    CHECK(ready && s.icl == 1600 && s.busMv == 5000);
  }
  // #18: after a wake the firmware sees no source while the PMIC does: re-sync twice, then report
  {
    Sim s;
    s.step();
    s.wakes++;
    s.busMv = 0;   // the firmware's view; the PMIC still sees the Rp (typecSource)
    s.step();
    CHECK(s.plan.resync);
    unsigned resyncs = 0;
    bool wedged = false;
    for (int i = 0; i < 40; ++i) { s.step(); resyncs += s.plan.resync; wedged |= (s.plan.events & PIANO_CHG_EV_WEDGED) != 0; }
    CHECK(resyncs == 2 && wedged);
    // the firmware recovers
    s.busMv = 5000;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_ATTACH) && s.wrote(PIANO_CHG_ICL, 1500));
  }
  // a real detach (the PMIC sees nothing either): no re-sync
  {
    Sim s;
    s.step();
    s.source = false;
    for (int i = 0; i < 20; ++i) { s.step(); CHECK(!s.plan.resync); }
  }
  // shutdown at 9 V: back to 5 V, limits kept
  {
    Sim s;
    s.cfg.hv = 1;
    s.pd(kPd5and9);
    for (int i = 0; i < 6; ++i) s.step();
    s.fill();
    piano_charge_shutdown(&s.cfg, &s.in, &s.st, &s.plan);
    CHECK(s.wrote(PIANO_CHG_PD_FIXED, 5000) && !s.wrote(PIANO_CHG_ICL) && !s.wrote(PIANO_CHG_ENABLE));
  }
}

// ---------------------------------------------------------------------------------------------- the direct-charging curve (spec)
static PianoChargeCpInput cpIn(int vbat, int tcool, int twarm, int soc)
{
  PianoChargeCpInput c{};
  c.t_valid = c.soc_valid = c.verified = 1;
  c.vbat_max_mv = vbat; c.vbat_min_mv = vbat - 10;
  c.t_cool_c = tcool; c.t_warm_c = twarm;
  c.soc_max_pct = soc;
  c.cell_ma_max = 3000;
  c.apdo_ma = 6000; c.vbus_mv = uint32_t(2 * vbat + 300);
  return c;
}

static void testCp()
{
  PianoChargeCpState s{};
  PianoChargeCpInput c = cpIn(3800, 25, 26, 50);
  CHECK(piano_charge_cp_ibat_ma(&c, &s) == 8000 && s.profile == PIANO_CHG_CP_NORMAL);   // normal table, first step
  c.running = 1;
  c.vbat_max_mv = 4200;
  CHECK(piano_charge_cp_ibat_ma(&c, &s) == 7230);
  c.vbat_max_mv = 4100;
  CHECK(piano_charge_cp_ibat_ma(&c, &s) == 7230);            // the stage only moves on
  c.vbat_max_mv = 4387;
  CHECK(piano_charge_cp_ibat_ma(&c, &s) == 0);               // the firmware's float less 13 mV: the buck finishes
  // FFC (the float option on, verified, 18-47 degC, below 95 %)
  PianoChargeCpState f{};
  PianoChargeCpInput d = cpIn(3800, 25, 26, 50);
  d.full_charge = 1;
  CHECK(piano_charge_cp_ibat_ma(&d, &f) == 12000 && f.profile == PIANO_CHG_CP_FFC);   // 12.4 A, the stage's 12 A
  d.running = 1;
  d.vbat_max_mv = 4200; CHECK(piano_charge_cp_ibat_ma(&d, &f) == 10650);
  d.vbat_max_mv = 4320; CHECK(piano_charge_cp_ibat_ma(&d, &f) == 9040);
  d.vbat_max_mv = 4480; CHECK(piano_charge_cp_ibat_ma(&d, &f) == 7230);
  d.vbat_max_mv = 4540; CHECK(piano_charge_cp_ibat_ma(&d, &f) == 0);
  d.vbat_max_mv = 4200; d.soc_max_pct = 95;
  CHECK(piano_charge_cp_ibat_ma(&d, &f) == 7230 && f.profile == PIANO_CHG_CP_NORMAL);   // 95 %: the normal table, past 4150
  // with the float option on, the normal table runs to 4473
  PianoChargeCpState n{};
  PianoChargeCpInput e = cpIn(4400, 25, 26, 96);
  e.full_charge = 1; e.running = 1;
  CHECK(piano_charge_cp_ibat_ma(&e, &n) == 7230);
  e.vbat_max_mv = 4473; CHECK(piano_charge_cp_ibat_ma(&e, &n) == 0);
  // temperature
  PianoChargeCpState w{};
  PianoChargeCpInput t = cpIn(3800, 30, 37, 50);
  CHECK(piano_charge_cp_ibat_ma(&t, &w) == 8000);
  t.t_warm_c = 38; CHECK(piano_charge_cp_ibat_ma(&t, &w) == 0);   // no start from 38
  t.running = 1; t.t_warm_c = 40; t.now_ms = 1000;
  CHECK(piano_charge_cp_ibat_ma(&t, &w) == 4500);
  t.t_warm_c = 38; t.now_ms = 21000; CHECK(piano_charge_cp_ibat_ma(&t, &w) == 4500);   // held 30 s
  t.now_ms = 31000; CHECK(piano_charge_cp_ibat_ma(&t, &w) == 5000);
  t.now_ms = 40000; CHECK(piano_charge_cp_ibat_ma(&t, &w) == 5000);
  t.now_ms = 46000; CHECK(piano_charge_cp_ibat_ma(&t, &w) == 5500);
  t.t_warm_c = 39; t.now_ms = 80000; CHECK(piano_charge_cp_ibat_ma(&t, &w) == 5500);   // not 2 degC cooler: no step
  t.t_warm_c = 44; CHECK(piano_charge_cp_ibat_ma(&t, &w) == 4500);
  t.t_warm_c = 45; CHECK(piano_charge_cp_ibat_ma(&t, &w) == 0);
  PianoChargeCpState k{};
  PianoChargeCpInput u = cpIn(3800, 17, 20, 50);
  CHECK(piano_charge_cp_ibat_ma(&u, &k) == 4500);
  u.t_cool_c = 14; CHECK(piano_charge_cp_ibat_ma(&u, &k) == 0);
  // the source and the window
  PianoChargeCpState q{};
  PianoChargeCpInput v = cpIn(3800, 25, 26, 50);
  v.apdo_ma = 3000; CHECK(piano_charge_cp_ibat_ma(&v, &q) == 6000);
  v.apdo_ma = 6000; v.vbus_mv = 20000; CHECK(piano_charge_cp_ibat_ma(&v, &q) == 6700);   // 67 W
  v.vbus_mv = 7900; v.verified = 0; CHECK(piano_charge_cp_ibat_ma(&v, &q) == 0);
  v.verified = 1; v.vbat_min_mv = 3400; CHECK(piano_charge_cp_ibat_ma(&v, &q) == 0);
  v.vbat_min_mv = 3790; v.cell_ma_max = -7000; CHECK(piano_charge_cp_ibat_ma(&v, &q) == 0);
  v.cell_ma_max = 3000; v.t_valid = 0; CHECK(piano_charge_cp_ibat_ma(&v, &q) == 0);
}

// ------------------------------------------------------------------------------------------- the Type-C current, the board thermistors
static void testRpAndBoard()
{
  // the row function: no PD contract, 1.5 / 3.0 A, within the ceiling, never below the row
  CHECK(piano_charge_icl_rp(PIANO_CHG_C_CDP, 900, 3000, 1600) == 1600 && piano_charge_icl_rp(PIANO_CHG_C_CDP, 900, 1500, 1600) == 1500);
  CHECK(piano_charge_icl_rp(PIANO_CHG_C_CDP, 900, 500, 1600) == 900 && piano_charge_icl_rp(PIANO_CHG_C_CDP, 900, 0, 1600) == 900);
  CHECK(piano_charge_icl_rp(PIANO_CHG_C_SDP, 500, 3000, 1600) == 1600 && piano_charge_icl_rp(PIANO_CHG_C_TYPEC, 500, 1500, 1600) == 1500);
  CHECK(piano_charge_icl_rp(PIANO_CHG_C_DCP, 1500, 1500, 1600) == 1500 && piano_charge_icl_rp(PIANO_CHG_C_DCP, 1500, 3000, 1600) == 1600);
  CHECK(piano_charge_icl_rp(PIANO_CHG_C_FLOAT, 500, 3000, 2500) == 2500);
  CHECK(piano_charge_icl_rp(PIANO_CHG_C_PD, 500, 3000, 1600) == 500 && piano_charge_icl_rp(PIANO_CHG_C_PD_PPS, 1200, 3000, 1600) == 1200);
  CHECK(piano_charge_icl_rp(PIANO_CHG_C_REFUSED, 0, 3000, 1600) == 0);
  const uint32_t conns = PIANO_CHG_IN_CONN1 | PIANO_CHG_IN_CONN2;
  // a CDP with Rp 3.0 A: the row without the option, 1600 with it and a connector reading, the row without one
  {
    Sim s;
    s.type = PIANO_CHG_T_CDP;
    s.boardValid = conns;
    s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, 900) && s.plan.icl_why == PIANO_CHG_ICL_ROW && s.plan.rp_ma == 0);
    Sim t;
    t.type = PIANO_CHG_T_CDP;
    t.cfg.rp = 1;
    t.boardValid = conns;
    t.step();
    CHECK(t.wrote(PIANO_CHG_ICL, 1600) && t.wrote(PIANO_CHG_FCC, 900) && t.plan.icl_why == PIANO_CHG_ICL_RP && t.plan.rp_ma == 3000);
    // asleep: the row (nothing watches the bus)
    t.step();
    t.sleep = PIANO_CHG_WILL_SLEEP;
    t.step(100);
    CHECK(t.wrote(PIANO_CHG_ICL, 900));
    Sim u;
    u.type = PIANO_CHG_T_CDP;
    u.cfg.rp = 1;
    u.step();
    CHECK(u.wrote(PIANO_CHG_ICL, 900) && (u.plan.events & PIANO_CHG_EV_RP_WITHHELD));
    u.step();
    CHECK(!(u.plan.events & PIANO_CHG_EV_RP_WITHHELD));
  }
  // the sag ends the trust at once, for good (a new generation or a wake does not bring it back)
  {
    Sim s;
    s.type = PIANO_CHG_T_CDP;
    s.cfg.rp = 1;
    s.boardValid = conns;
    s.step();
    s.step();
    CHECK(s.plan.writes == 0 && s.icl == 1600);
    s.busMv = 4400;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_RP_DISTRUST) && s.wrote(PIANO_CHG_ICL, 900));
    s.busMv = 5100;
    s.generation++;
    s.wakes++;
    s.step();
    s.step();
    CHECK(s.icl == 900 && s.st.rp_distrust);
  }
  // a connector at 60 degC: 100 mA; held while one reading is missing; released below 50 on both
  {
    Sim s;
    s.boardValid = conns;
    s.step();
    CHECK(s.icl == 1500);
    s.conn2Dc = 605;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_CONN_HOT) && s.wrote(PIANO_CHG_ICL, 100) && s.plan.icl_why == PIANO_CHG_ICL_CONN);
    s.conn2Dc = 400;
    s.boardValid = PIANO_CHG_IN_CONN1;
    s.step();
    CHECK(s.plan.writes == 0 && s.icl == 100);
    s.boardValid = conns;
    s.conn1Dc = 495;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_CONN_COOL) && s.wrote(PIANO_CHG_ICL, 1500));
    // the charger at 85: 500 at most; 75 still held; 74.9 released
    s.boardValid |= PIANO_CHG_IN_CHGR;
    s.chgrDc = 850;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_CHGR_HOT) && s.wrote(PIANO_CHG_ICL, 500));
    s.chgrDc = 750;
    s.step();
    CHECK(s.plan.writes == 0);
    s.chgrDc = 749;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_CHGR_COOL) && s.wrote(PIANO_CHG_ICL, 1500));
    // a 9 V PD source is capped too
    Sim t;
    t.cfg.hv = 1;
    t.pd(kPd5and9);
    t.boardValid = conns;
    for (int i = 0; i < 6; ++i) t.step();
    CHECK(t.icl == 1600);
    t.conn1Dc = 700;
    t.step();
    CHECK(t.wrote(PIANO_CHG_ICL, 100));
  }
  // the battery thermistor: the warmer reading derates, a gap over 10 degC is noted once per episode
  {
    Sim s;
    s.cfg.icl_cap_ma = 2500; s.cfg.fcc_cap_ma = 3500;
    s.boardValid = PIANO_CHG_IN_NTC;
    s.ntcDc = 405;   // 40.5 degC against packs at 30
    s.step();
    CHECK(s.wrote(PIANO_CHG_FCC, 1400) && !(s.plan.events & PIANO_CHG_EV_NTC_APART));
    s.ntcDc = 455;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_NTC_APART) && s.wrote(PIANO_CHG_FCC, 500));
    s.step();
    CHECK(!(s.plan.events & PIANO_CHG_EV_NTC_APART));
    // an implausible thermistor reading is not a temperature
    Sim t;
    t.boardValid = PIANO_CHG_IN_NTC;
    t.ntcDc = 1200;
    t.step();
    CHECK(t.wrote(PIANO_CHG_FCC, 1500));
  }
}

// ------------------------------------------------------------------------------------------------------ the stepped Rp limit (§15)
static void testRamp()
{
  // the steps
  CHECK(piano_charge_ramp_next(900, 3000) == 1500 && piano_charge_ramp_next(1500, 3000) == 2000 && piano_charge_ramp_next(2000, 3000) == 2500);
  CHECK(piano_charge_ramp_next(2500, 3000) == 3000 && piano_charge_ramp_next(2500, 2800) == 2800 && piano_charge_ramp_next(900, 1200) == 1200);
  CHECK(piano_charge_ramp_next(1600, 3000) == 2000 && piano_charge_ramp_next(3000, 3000) == 3000 && piano_charge_ramp_next(500, 3000) == 1500);
  CHECK(piano_charge_ramp_back(2500, 900) == 2000 && piano_charge_ramp_back(1500, 900) == 900 && piano_charge_ramp_back(2000, 1500) == 1500);
  CHECK(piano_charge_ramp_back(2800, 900) == 2500 && piano_charge_ramp_back(1600, 900) == 1500 && piano_charge_ramp_back(900, 900) == 900);
  CHECK(piano_charge_ramp_back(3000, 500) == 2500);
  // the allowlist's ceiling: 1600, or with rp and ramp the larger rp_cap (at most 3000)
  PianoChargeConfig c;
  piano_charge_config_default(&c);
  CHECK(!c.ramp && !c.rp_cap_ma && piano_charge_icl_max(&c) == 1600);
  c.rp_cap_ma = 3000;
  CHECK(piano_charge_icl_max(&c) == 1600);          // neither rp nor ramp
  c.rp = 1;
  CHECK(piano_charge_icl_max(&c) == 1600 && !piano_charge_write_allowed(&c, PIANO_CHG_ICL, 1700, 4));   // rp at once: the cap
  c.ramp = 1;
  CHECK(piano_charge_icl_max(&c) == 3000 && piano_charge_write_allowed(&c, PIANO_CHG_ICL, 3000, 4));
  c.rp_cap_ma = 9000;
  CHECK(piano_charge_icl_max(&c) == 3000 && !piano_charge_write_allowed(&c, PIANO_CHG_ICL, 3001, 4));
  c.rp_cap_ma = 0;
  CHECK(piano_charge_icl_max(&c) == 1600);
  const uint32_t conns = PIANO_CHG_IN_CONN1 | PIANO_CHG_IN_CONN2;
  auto mac = [&](Sim &s) {   // the 2026-11-12 Mac port: CDP, Rp 3.0 A, 5.23 V open, 0.21 Ohm, the system taking more than it gets
    s.type = PIANO_CHG_T_CDP;
    s.cfg.rp = 1;
    s.cfg.ramp = 1;
    s.cfg.rp_cap_ma = 3000;
    s.boardValid = conns;
    s.busMv = 5230;
    s.ohms = 0.21;
    s.loadMa = 3500;
  };
  // the climb: the row first (the input before the charge current), then a step per 15 s (10 s held and two decisions after the
  // decision that saw it taken), the bus 4.6 V at 3 A; each wait reason once a step
  {
    Sim s;
    mac(s);
    s.step();
    CHECK(s.plan.writes == 2 && s.plan.write[0].property == PIANO_CHG_ICL && s.plan.write[0].value == 900);
    CHECK(s.plan.write[1].property == PIANO_CHG_FCC && s.plan.write[1].value == 1500);   // the DCP row's 2000 within the 1500 cap
    CHECK(s.plan.icl_why == PIANO_CHG_ICL_RP && s.plan.ramp_ma == 900 && s.plan.ramp_target_ma == 3000);
    std::vector<std::pair<uint64_t, uint32_t>> steps;
    unsigned waits = 0;
    for (int i = 0; i < 40; ++i) {
      s.step();
      if (s.plan.events & PIANO_CHG_EV_RAMP_STEP) {
        CHECK(s.wrote(PIANO_CHG_ICL, s.plan.ramp_ma) && s.plan.ramp_from_ma < s.plan.ramp_ma);
        steps.push_back({s.now, s.plan.ramp_ma});
      }
      waits += (s.plan.events & PIANO_CHG_EV_RAMP_WAIT) != 0;
    }
    CHECK(steps.size() == 4 && steps[0].second == 1500 && steps[3].second == 3000);
    for (size_t i = 1; i < steps.size(); ++i) CHECK(steps[i].first == steps[i - 1].first + 15000);
    CHECK(s.plan.ramp_wait == PIANO_CHG_RAMP_TOP && s.icl == 3000 && waits == 1 && !s.st.ramp_ceiling_ma);
    // a detach long after the last step costs nothing; the next attach climbs again from the row
    s.source = false;
    s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, 100) && !s.st.ramp_ceiling_ma && !s.st.ramp_ma);
    s.source = true;
    s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, 900));
  }
  // a source that gives way above 2.2 A: a sag under 2500 -> 2000, this boot's ceiling (kept across a generation and a wake)
  {
    Sim s;
    mac(s);
    s.kneeMa = 2200;
    s.kneeOhms = 1.0;
    for (int i = 0; i < 40; ++i) s.step();
    CHECK(s.st.ramp_ceiling_ma == 2000 && s.st.ramp_back_why == PIANO_CHG_BACK_SAG && s.st.ramp_back_from_ma == 2500 && s.icl == 2000);
    s.generation++;
    s.wakes++;
    for (int i = 0; i < 40; ++i) s.step();
    CHECK(s.icl == 2000 && s.plan.ramp_wait == PIANO_CHG_RAMP_TOP && s.plan.ramp_ceiling_ma == 2000);
  }
  // a quick detach after a step: one step back for the boot; a slow one: nothing
  {
    Sim s;
    mac(s);
    for (int i = 0; i < 4; ++i) s.step();
    CHECK(s.icl == 1500 && (s.plan.events & PIANO_CHG_EV_RAMP_STEP));
    s.source = false;
    s.step();
    CHECK((s.plan.events & PIANO_CHG_EV_RAMP_BACK) && s.st.ramp_ceiling_ma == 900 && s.st.ramp_back_why == PIANO_CHG_BACK_DETACH);
    s.source = true;
    for (int i = 0; i < 20; ++i) s.step();
    CHECK(s.icl == 900 && s.plan.ramp_wait == PIANO_CHG_RAMP_TOP);
  }
  // the input not used up: no step; no bus current reading: none either; a low bus: none
  {
    Sim s;
    mac(s);
    s.loadMa = 780;   // within an eighth (788 mA at 900) counts as used up; 780 does not
    for (int i = 0; i < 20; ++i) s.step();
    CHECK(s.icl == 900 && s.plan.ramp_wait == PIANO_CHG_RAMP_NOT_USED);
    Sim t;
    mac(t);
    t.loadMa = 0;   // no IBUS bit
    for (int i = 0; i < 20; ++i) t.step();
    CHECK(t.icl == 900 && t.plan.ramp_wait == PIANO_CHG_RAMP_NO_CURRENT);
    Sim u;
    mac(u);
    u.busMv = 4800;   // 4.61 V under 900 mA
    for (int i = 0; i < 20; ++i) u.step();
    CHECK(u.icl == 900 && u.plan.ramp_wait == PIANO_CHG_RAMP_BUS_LOW);
  }
  // no connector reading: 1500 at most (noted once); with one again the steps go on; Rp 1.5 A: 1500 is the target
  {
    Sim s;
    mac(s);
    s.boardValid = 0;
    for (int i = 0; i < 20; ++i) s.step();
    CHECK(s.icl == 1500 && s.plan.ramp_wait == PIANO_CHG_RAMP_TOP && s.plan.ramp_target_ma == 1500);
    s.boardValid = conns;
    for (int i = 0; i < 40; ++i) s.step();
    CHECK(s.icl == 3000);
    Sim t;
    mac(t);
    t.rpMa = 1500;
    for (int i = 0; i < 20; ++i) t.step();
    CHECK(t.icl == 1500 && t.plan.ramp_target_ma == 1500);
  }
  // refused three times: back, the ceiling
  {
    Sim s;
    mac(s);
    for (int i = 0; i < 20 && s.icl != 1500; ++i) s.step();
    CHECK(s.icl == 1500);
    s.refuseIcl = true;
    for (int i = 0; i < 10; ++i) s.step();
    CHECK(s.st.ramp_ceiling_ma == 1500 && s.st.ramp_back_why == PIANO_CHG_BACK_REFUSED && s.icl == 1500);
    CHECK(s.plan.ramp_wait == PIANO_CHG_RAMP_TOP);
    s.refuseIcl = false;
    for (int i = 0; i < 20; ++i) s.step();
    CHECK(s.icl == 1500);
  }
  // writes off: the steps for the log only, nothing sent; ramp off: the Rp limit at once within the cap (the old behaviour)
  {
    Sim s;
    mac(s);
    s.cfg.writes = 0;
    for (int i = 0; i < 20; ++i) {s.step(); CHECK(s.plan.writes == 0);}
    Sim t;
    mac(t);
    t.cfg.ramp = 0;
    t.step();
    CHECK(t.wrote(PIANO_CHG_ICL, 1600) && t.plan.ramp_ma == 0);
  }
  // a board hold mid-climb: 100 mA; afterwards the row first, never straight back up
  {
    Sim s;
    mac(s);
    for (int i = 0; i < 10; ++i) s.step();
    CHECK(s.icl >= 2000);
    s.conn1Dc = 610;
    s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, 100) && !s.st.ramp_ma);
    s.conn1Dc = 400;
    s.step();
    CHECK(s.wrote(PIANO_CHG_ICL, 900));
  }
}

// ---------------------------------------------------------------------------------------------------------------- random walk
static void testRandom()
{
  uint64_t x = 0x2545f4914f6cdd1dull;
  auto rnd = [&](unsigned n) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return unsigned(x % n); };
  for (int run = 0; run < 40; ++run) {
    Sim s;
    s.cfg.hv = uint8_t(rnd(3));
    s.cfg.full_charge = uint8_t(rnd(2));
    s.cfg.limit = uint8_t(rnd(2));
    s.cfg.sleep_charge = uint8_t(rnd(2));
    s.cfg.writes = rnd(8) != 0;
    s.cfg.rp = uint8_t(rnd(2));
    s.cfg.ramp = uint8_t(rnd(2));
    s.cfg.rp_cap_ma = uint16_t(rnd(2) ? 3000 : rnd(4000));
    if (rnd(2)) { s.cfg.icl_cap_ma = 2500; s.cfg.fcc_cap_ma = 3500; }
    if (rnd(2)) { s.loadMa = 300 + int(rnd(3500)); s.ohms = 0.05 * rnd(8); s.kneeMa = rnd(2) ? 0 : 1000 + int(rnd(2000)); s.kneeOhms = 1.0; }
    for (int i = 0; i < 1500; ++i) {
      switch (rnd(40)) {
      case 0: s.source = !s.source; break;
      case 1: s.type = int(rnd(17)); break;
      case 2: s.pd(rnd(2) ? kPd5and9 : std::vector<std::vector<uint32_t>>{{5000, 5000, 3000}, {3300, 11000, 3250}},
                   PIANO_CHG_T_PD + int(rnd(3))); break;
      case 3: s.temp(int(rnd(70)) - 10); break;
      case 4: s.refuseIcl = !s.refuseIcl; break;
      case 5: s.ackWrites = !s.ackWrites; break;
      case 6: s.generation++; break;
      case 7: s.sleep = PIANO_CHG_WILL_SLEEP; break;
      case 8: if (s.sleep == PIANO_CHG_WILL_SLEEP) { s.sleep = PIANO_CHG_AWAKE; s.wakes++; } break;
      case 9: s.readsOk = !s.readsOk; break;
      case 10: s.bothGauges = !s.bothGauges; break;
      case 11: s.follows9v = !s.follows9v; break;
      case 12: s.verify = int(rnd(2)); break;
      case 13: s.busMv = int(rnd(12000)); break;
      case 14: s.typecSource = !s.typecSource; break;
      case 15: s.ignoreFv = !s.ignoreFv; break;
      case 16: { s.fill(); piano_charge_shutdown(&s.cfg, &s.in, &s.st, &s.plan); s.check();
                 for (unsigned k = 0; k < s.plan.writes; ++k) s.apply(s.plan.write[k]); } break;
      case 17: s.rpMa = rnd(2) ? 3000 : rnd(2) ? 1500 : 500; break;
      case 18: s.boardValid = (rnd(16) << 10) & (PIANO_CHG_IN_NTC | PIANO_CHG_IN_CHGR | PIANO_CHG_IN_CONN1 | PIANO_CHG_IN_CONN2); break;
      case 19: s.conn1Dc = int(rnd(800)); s.conn2Dc = int(rnd(800)); s.chgrDc = int(rnd(1000)); s.ntcDc = int(rnd(700)); break;
      case 20: s.ibat = rnd(2) ? 0 : int(rnd(2000)) - 1000; break;
      case 21: s.vbat = 3500 + int(rnd(1000)); break;
      case 22: s.aiclCap = rnd(2) ? 0 : 500 + int(rnd(2500)); break;
      case 23: s.cfg.kick = uint8_t(rnd(4) != 0); break;
      default: break;
      }
      s.soc += int(rnd(3)) - 1;
      if (s.soc < 0) s.soc = 0;
      if (s.soc > 100) s.soc = 100;
      s.step(rnd(4) ? 5000 : rnd(40000));
      if (s.sleep == PIANO_CHG_WILL_SLEEP) CHECK(!s.plan.hold_awake);
      if (!s.source && s.readsOk) CHECK(!s.plan.hold_awake);
      // the board holds always win; an Rp limit only with the option, a connector reading below 50 degC and no PD contract
      if (s.st.conn_hot && s.plan.cls != PIANO_CHG_C_NONE && s.plan.icl_ma) CHECK(s.plan.icl_ma <= 100);
      if (s.plan.icl_why == PIANO_CHG_ICL_RP) {
        CHECK(s.cfg.rp && !s.st.rp_distrust && s.plan.cls != PIANO_CHG_C_PD && s.plan.cls != PIANO_CHG_C_PD_PPS);
        CHECK(s.plan.icl_ma <= piano_charge_icl_max(&s.cfg));
        // a connector reading for anything from the Rp, except a stepped limit up to 1.5 A
        if (!(s.cfg.ramp && s.plan.icl_ma <= 1500)) CHECK(s.in.valid & (PIANO_CHG_IN_CONN1 | PIANO_CHG_IN_CONN2));
        if (!s.cfg.ramp) CHECK(s.plan.icl_ma <= (s.cfg.icl_cap_ma ? s.cfg.icl_cap_ma : 1600u));
        if (s.cfg.ramp && s.st.ramp_ceiling_ma) CHECK(s.plan.icl_ma <= std::max(s.st.ramp_ceiling_ma, 900u) || s.plan.icl_ma <= 1500);
      }
      if (s.st.ramp_ceiling_ma) CHECK(s.cfg.rp && s.cfg.ramp);
      // the restart: never more than three per attach; never while a sleep is handed over
      CHECK(s.st.kick_count <= 3);
      if (s.sleep == PIANO_CHG_WILL_SLEEP) CHECK(!(s.plan.kick_events & PIANO_CHG_KICK_EV_START));
    }
  }
}

// ---------------------------------------------------------------------------------------- the balance line and the used-up limit
static void testBalance()
{
  // chg-ramp at 2026-11-12 09:10 (93 %): 4911 mV and 1481 mA in under 2000 asked, 0x2029 1750, both packs at 0 mA at 4362 mV
  PianoChargeBalanceIn i{};
  i.input_valid = 1; i.bus_mv = 4911; i.ibus_ma = 1481; i.limit_ma = 1750; i.battery_valid = 1; i.vbat_mv = 4362; i.ibat_ma = 0;
  PianoChargeBalance b;
  piano_charge_balance(&i, &b);
  CHECK(b.in_mw == 7273 && b.bat_mw == 0 && b.sys_mw == 6543 && b.spare_ma == 269 && b.verdict == PIANO_CHG_BAL_SPARE);
  CHECK(std::strstr(piano_charge_balance_name(b.verdict), "NOT CHARGING"));
  i.ibat_ma = 620;   // charging: the system is the input less the battery
  piano_charge_balance(&i, &b);
  CHECK(b.verdict == PIANO_CHG_BAL_CHARGING && b.bat_mw == 2704 && b.sys_mw == 6543 - 2704);
  i.ibat_ma = -200;   // draining (the battery's power is negative: the system gets it on top of the input)
  piano_charge_balance(&i, &b);
  CHECK(b.verdict == PIANO_CHG_BAL_DRAINING && b.bat_mw == -872 && b.sys_mw == 6543 + 872);
  i.ibat_ma = 10; i.ibus_ma = 1700;   // within 50 mA of 0, the input at its limit: used up by the system
  piano_charge_balance(&i, &b);
  CHECK(b.verdict == PIANO_CHG_BAL_USED_UP && b.spare_ma == 50);
  i.limit_ma = 0;   // no limit known: not "to spare"
  piano_charge_balance(&i, &b);
  CHECK(b.verdict == PIANO_CHG_BAL_USED_UP && b.spare_ma == 0);
  i.bus_mv = 300; i.ibus_ma = 0; i.ibat_ma = -1500;   // no input
  piano_charge_balance(&i, &b);
  CHECK(b.verdict == PIANO_CHG_BAL_ON_BATTERY && b.in_mw == 0 && b.sys_mw == 6543);
  i.input_valid = 0; i.bus_mv = 5000;   // no bus reading (level 1) counts as no input for the sums
  piano_charge_balance(&i, &b);
  CHECK(b.verdict == PIANO_CHG_BAL_ON_BATTERY && b.in_mw == 0);
  i.battery_valid = 0;
  piano_charge_balance(&i, &b);
  CHECK(b.verdict == PIANO_CHG_BAL_UNKNOWN && b.sys_mw == 0);
  CHECK(std::strcmp(piano_charge_balance_name(99), "?") == 0);
  // the limit the steps use up: the step, or the firmware's own once settled below it
  CHECK(piano_charge_used_limit(2000, 1750, 1) == 1750 && piano_charge_used_limit(2000, 1750, 0) == 2000);
  CHECK(piano_charge_used_limit(2000, 2000, 1) == 2000 && piano_charge_used_limit(2000, 0, 1) == 2000);
  CHECK(piano_charge_used_limit(2000, 50, 1) == 2000 && piano_charge_used_limit(1500, 2500, 1) == 1500);
}

// The steps against the firmware's own limit: the Mac's port held 0x2029 at 1750-1800 under 2000 asked, the bus at 1.33-1.71 A.
static void testRampAicl()
{
  const uint32_t conns = PIANO_CHG_IN_CONN1 | PIANO_CHG_IN_CONN2;
  auto mac = [&](Sim &s) {
    s.type = PIANO_CHG_T_CDP;
    s.cfg.rp = 1;
    s.cfg.ramp = 1;
    s.cfg.rp_cap_ma = 3000;
    s.boardValid = conns;
    s.busMv = 5230;
    s.ohms = 0.21;
  };
  // the system and the battery take 2600 mA; the firmware holds 1750: at 2000 asked the bus reads 1750, which uses up its own limit
  // once it has stayed so 15 s: the steps go on (the request rises; the firmware's own limit is its own)
  {
    Sim s;
    mac(s);
    s.loadMa = 2600;
    s.aiclCap = 1750;
    for (int i = 0; i < 40; ++i) s.step();
    CHECK(s.icl == 3000 && s.plan.ramp_wait == PIANO_CHG_RAMP_TOP);
    // without it read the old rule holds at 2000 (1750 + 250 < 2000)
    Sim t;
    mac(t);
    t.loadMa = 2600;
    t.aiclCap = 1750;
    t.aiclRead = false;
    Sim u = t;
    u.aiclCap = 0;
    u.loadMa = 1700;   // the bus current of chg-ramp (1.33-1.71 A), no 0x2029: not used up (1700 + 250 < 2000)
    for (int i = 0; i < 40; ++i) u.step();
    CHECK(u.icl == 2000 && u.plan.ramp_wait == PIANO_CHG_RAMP_NOT_USED && u.plan.ramp_used_ma == 2000);
  }
  // the firmware still walking its limit up (50 mA a decision): never settled, so the step is what is used up
  {
    Sim s;
    mac(s);
    s.loadMa = 1450;
    s.aiclRead = true;
    for (int i = 0; i < 10; ++i) s.step();
    CHECK(s.icl == 2000);
    s.aiclCap = 1100;
    int steps = 0;
    for (int i = 0; i < 12; ++i) {
      s.step();
      steps += (s.plan.events & PIANO_CHG_EV_RAMP_STEP) != 0;
      if (s.aiclCap < 1950) s.aiclCap += 50;
    }
    CHECK(!steps && s.icl == 2000 && s.plan.ramp_used_ma == 2000);
  }
}

// The charging restart (docs §17) against the 2026-11-12 state: the Mac's port, CDP, Rp 3.0 A, the system at 1450 mA, both packs at
// 0 mA at 93 % and 4362 mV, 34 C, the charger not charging until its enable is toggled.
static void testKick()
{
  const uint32_t conns = PIANO_CHG_IN_CONN1 | PIANO_CHG_IN_CONN2;
  auto mac = [&](Sim &s) {
    s.type = PIANO_CHG_T_CDP;
    s.cfg.rp = 1;
    s.cfg.ramp = 1;
    s.cfg.rp_cap_ma = 3000;
    s.boardValid = conns;
    s.busMv = 5230;
    s.ohms = 0.21;
    s.soc = 93;
    s.vbat = 4362;
    s.temp(34);
    s.charger = 1200;   // what the battery would take at most (its own acceptance); the charge current caps it
    s.loadBase = 1450;
    s.stuck = true;
  };
  CHECK([] { PianoChargeConfig c; piano_charge_config_default(&c); return c.kick == 1; }());
  // the restart: 0x2003 = 0 alone, then 1, the charge current again, and the verdict; the steps then go on with the battery's load
  {
    Sim s;
    mac(s);
    uint64_t start = 0, resumed = 0, charging = 0;
    unsigned starts = 0, fccAfter = 0;
    for (int i = 0; i < 80; ++i) {
      s.step();
      const uint32_t k = s.plan.kick_events;
      if (k & PIANO_CHG_KICK_EV_START) {
        starts++;
        start = s.now;
        CHECK(s.plan.writes == 1 && s.wrote(PIANO_CHG_ENABLE, 0) && s.plan.kick_count == 1 && s.plan.kick_ibat_ma == 0);
        CHECK(s.plan.kick_spare_ma >= 150 && s.st.disabled);
      }
      if (start && !resumed && s.now == start + 5000) CHECK(s.wrote(PIANO_CHG_ENABLE, 1));   // the next decision lifts it
      if (k & PIANO_CHG_KICK_EV_RESUMED) resumed = s.now;
      if (resumed && s.now > resumed && s.wrote(PIANO_CHG_FCC)) fccAfter++;
      if (k & PIANO_CHG_KICK_EV_CHARGING) {charging = s.now; CHECK(s.plan.kick_ibat_ma >= 50);}
      CHECK(!(k & (PIANO_CHG_KICK_EV_NONE | PIANO_CHG_KICK_EV_ABANDONED)));
    }
    CHECK(starts == 1 && resumed > start && charging >= resumed + 15000 && fccAfter == 1);
    CHECK(s.enable == 1 && !s.stuck && s.enableZeros == 1 && s.fcc == 1500 && s.ibat > 0);
    // the input before the restart: the steps waited at 2000 (not used up); after it the battery's share uses it up: 2500, 3000
    CHECK(s.icl == 3000 && s.plan.ramp_wait == PIANO_CHG_RAMP_TOP);
  }
  // the restart does not help: three, two minutes apart, then none more until the next attach; 0x2003 ends at 1 each time
  {
    Sim s;
    mac(s);
    s.toggleClears = false;
    std::vector<uint64_t> starts;
    unsigned none = 0, gaveUp = 0;
    for (int i = 0; i < 200; ++i) {
      s.step();
      if (s.plan.kick_events & PIANO_CHG_KICK_EV_START) starts.push_back(s.now);
      none += (s.plan.kick_events & PIANO_CHG_KICK_EV_NONE) != 0;
      gaveUp += (s.plan.kick_events & PIANO_CHG_KICK_EV_GAVE_UP) != 0;
    }
    CHECK(starts.size() == 3 && none == 3 && gaveUp == 1 && s.enable == 1 && !s.st.disabled && s.enableZeros == 3);
    for (size_t i = 1; i < starts.size(); ++i) CHECK(starts[i] >= starts[i - 1] + 120000);
    // a new attach: the budget again
    s.source = false;
    s.step();
    s.source = true;
    for (int i = 0; i < 20; ++i) s.step();   // 100 s: the row, the steps, 20 s at 0 mA, the restart
    CHECK(s.st.kick_count == 1 && s.enableZeros == 4);
  }
  // when not: near full, at the float voltage, the input used up, warm, cold, the option or the writes off, the firmware's own 0,
  // battery health holding, no gauge reading; and the packs reading 0 only once (a single reading is no evidence)
  auto none = [&](void (*setup)(Sim &)) {
    Sim s;
    mac(s);
    setup(s);
    for (int i = 0; i < 60; ++i) {s.step(); if (s.plan.kick_events & PIANO_CHG_KICK_EV_START) return false;}
    return s.enableZeros == 0 || s.cfg.limit;
  };
  CHECK(none([](Sim &s) {s.soc = 96;}));
  CHECK(none([](Sim &s) {s.vbat = 4375;}));                 // within 30 mV of 4400
  CHECK(none([](Sim &s) {s.loadBase = 1900; s.cfg.rp_cap_ma = 2000;}));   // 1900 of 2000: 100 mA to spare
  CHECK(none([](Sim &s) {s.temp(46);}));
  CHECK(none([](Sim &s) {s.temp(14);}));
  CHECK(none([](Sim &s) {s.cfg.kick = 0;}));
  CHECK(none([](Sim &s) {s.cfg.writes = 0;}));
  CHECK(none([](Sim &s) {s.enable = 0;}));                  // the firmware's own disable: never overridden
  CHECK(none([](Sim &s) {s.bothGauges = false;}));
  CHECK(none([](Sim &s) {s.cfg.limit = 1; s.soc = 85;}));   // battery health holds: its own stop, not a restart
  {
    Sim s;
    mac(s);
    for (int i = 0; i < 6; ++i) s.step();
    s.charger = 0;   // the packs move: -40 mA, then 0 again: the 20 s start over
    s.stuck = false;
    s.ibat = -40;
    s.loadMa = 1450;
    s.step();
    s.charger = 1200;
    s.stuck = true;
    unsigned before = 0;
    for (int i = 0; i < 4; ++i) {s.step(); before += (s.plan.kick_events & PIANO_CHG_KICK_EV_START) != 0;}
    CHECK(!before);   // 0 mA again at +5..+20 s: not yet (20 s from the first reading back at 0)
    s.step();
    s.step();
    CHECK(s.st.kick_count == 1);
  }
  // cut short: a detach after the 0 was taken plans 1 (and the firmware's own limits back); a shutdown too; a wake and a new
  // generation still end at 1
  {
    Sim s;
    mac(s);
    for (int i = 0; i < 40 && !(s.plan.kick_events & PIANO_CHG_KICK_EV_START); ++i) s.step();
    CHECK(s.enable == 0 && s.st.disabled);
    s.source = false;
    s.step();
    CHECK(s.wrote(PIANO_CHG_ENABLE, 1) && s.enable == 1 && s.st.kick_stage == PIANO_CHG_KICK_IDLE && !s.st.kick_count);
    Sim t;
    mac(t);
    for (int i = 0; i < 40 && !(t.plan.kick_events & PIANO_CHG_KICK_EV_START); ++i) t.step();
    t.fill();
    piano_charge_shutdown(&t.cfg, &t.in, &t.st, &t.plan);
    t.check();
    CHECK(t.wrote(PIANO_CHG_ENABLE, 1));
    for (int g = 0; g < 2; ++g) {
      Sim u;
      mac(u);
      for (int i = 0; i < 40 && !(u.plan.kick_events & PIANO_CHG_KICK_EV_START); ++i) u.step();
      if (g) u.generation++; else u.wakes++;
      for (int i = 0; i < 6; ++i) u.step();
      CHECK(u.enable == 1 && !u.st.disabled);
    }
  }
  // the firmware refuses the 0 (and later answers nothing): the enable's rules end at 1; the restart judges or gives up
  {
    Sim s;
    mac(s);
    s.toggleClears = false;
    for (int i = 0; i < 40 && !(s.plan.kick_events & PIANO_CHG_KICK_EV_START); ++i) s.step();
    // the 0 was planned and applied by the model; answer it as refused instead
    s.enable = 1;
    piano_charge_ack(&s.st, PIANO_CHG_ENABLE, 0, 1);
    for (int i = 0; i < 20; ++i) s.step();
    CHECK(s.enable == 1 && !s.st.disabled && s.st.kick_stage == PIANO_CHG_KICK_IDLE);
  }
  // asleep: never; the handover (with health off) plans no 0 either; a sleep between the 0 and the 1 lifts it for the sleep, and
  // a decision within the 5 s the 0 is held plans no 1
  {
    Sim s;
    mac(s);
    s.sleep = PIANO_CHG_WILL_SLEEP;
    for (int i = 0; i < 20; ++i) {s.step(); CHECK(!s.wrote(PIANO_CHG_ENABLE, 0));}
    Sim t;
    mac(t);
    for (int i = 0; i < 40 && !(t.plan.kick_events & PIANO_CHG_KICK_EV_START); ++i) t.step();
    t.step(2000);
    CHECK(!t.wrote(PIANO_CHG_ENABLE) && t.enable == 0 && t.st.kick_stage == PIANO_CHG_KICK_STOPPING);
    t.sleep = PIANO_CHG_WILL_SLEEP;
    t.step(100);
    CHECK(t.wrote(PIANO_CHG_ENABLE, 1) && t.enable == 1 && t.st.kick_stage == PIANO_CHG_KICK_IDLE);
    t.step(100);
    CHECK(t.plan.sleep_ready && !t.st.disabled);
  }
}

int main()
{
  testTables();
  testFiveVolt();
  testThermal();
  testNineVolt();
  testLimit();
  testSleep();
  testCp();
  testRpAndBoard();
  testRamp();
  testBalance();
  testRampAicl();
  testKick();
  testRandom();
  if (gFailures) {
    std::fprintf(stderr, "policy_host: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("policy_host: all charging-policy tests passed\n");
  return 0;
}
