// SPDX-License-Identifier: BSD-2-Clause-Patent
// ChargePolicy (ChargePolicy.h) wired to PianoMca's codec: what PianoPower does at level 2 with piano-power-policy=1
// (docs/drivers/charging-policy.md §8). Portable, header only, no I/O: the owner hands in what it read besides the codec (the
// charger_pd state, the two packs, the Type-C block, PianoThermal's board temperatures, the wake count) and acts on the outcome
// (relink the codec, set the charging-hold assertion, log). Host-tested with the DSP model of tests/power_host.cpp.
//
//   - Gates: nothing is decided until msm/adsp/charger_pd reports up and the codec's board configuration of this link is through
//     (taken, or given up after five cycles), so the stock board configuration always precedes the first policy write.
//   - Cadence: one decision per finished codec cycle (5 s, and the cycles a notification or a write's read-back start); while a sleep
//     or a shutdown is handed over, also one per write answer, until the plan says ready.
//   - Writes: in the plan's order through piano_mca_queue, whose allowlist (piano_mca_write_allowed_for) is the codec's, not the
//     policy's: a write it refuses is answered to the policy as refused (three refusals: given up until the next attach). Every
//     answer, a timeout's included, goes back through piano_charge_ack(), and so does a late answer to a write the owner's timer had
//     given up on (McaLateWritten: the owner calls answered() for it too). The codec's input limit bound follows the policy's
//     (piano_mca_allow_icl(piano_charge_icl_max)): above 1600 mA only for the stepped Type-C current (charging-policy.md §15).
//   - Read-back: the input limit, the enable bit and the bus voltage request are compared with the first cycle read after they were
//     taken (the policy itself writes a limit again after two disagreeing read-backs); the charge current has none (0x2005 reads 0).
//   - Resync (a wake, Ubuntu #18): the plan's writes are not queued; the owner relinks the codec (board configuration, port enable,
//     UCSI read, everything read again) and the new generation sends everything again.
#ifndef PIANO_POWER_CHARGE_LINK_H
#define PIANO_POWER_CHARGE_LINK_H
#include <stdint.h>
#include "Gauge.h"
#include "ChargerPmic.h"
#include "PianoBoardTemps.h"
#include "ChargePolicy.h"
#include "PianoMca.h"

namespace pianopower {

struct LinkOptions {
  bool writes = true;              // piano-power-write
  unsigned hv = 0;                 // piano-power-hv: 0, 1 fixed 9 V PDO, 2 also a PPS APDO
  bool rp = false;                 // piano-power-rp: the Type-C Rp current as the 5 V limit of a source without PD
  bool ramp = true;                // piano-power-ramp: that limit in steps from the row (ChargePolicy cfg.ramp, docs §15)
  unsigned rpMax = 0;              // piano-power-rp-max: the stepped limit's ceiling, mA (0: the 1600 mA cap; at most 3000)
  bool health = false;             // piano-power-health: stop at stopPct, resume at resumePct
  unsigned stopPct = 80, resumePct = 75;
};

// What the owner knows besides the codec, for one decision.
struct LinkView {
  uint64_t nowMs = 0;
  bool chargerPdUp = false;                   // msm/adsp/charger_pd reports up (PianoQrtr.c's servreg listener)
  uint64_t chargerPdUpMs = 0;                 // when it was seen coming up (nowMs's clock; each up again moves it)
  const GaugeReading *packs = nullptr;        // the two packs' last reading, or none
  bool packsFresh = false;                    // that reading is recent enough to decide with
  const PortState *port = nullptr;            // the charger PMIC's Type-C block, or not readable
  const piano_board_temps *board = nullptr;   // PianoThermal's last round, or PianoThermal not there
  uint32_t wakes = 0;                         // system wakes so far
  bool willSleep = false, shutdown = false;   // a handover is asked for
};

struct LinkReadback {
  uint32_t property = 0, written = 0;
  int32_t read = 0;
  bool readable = false, agrees = false;
};

// What one step did.
struct LinkOutcome {
  bool decided = false;
  bool resync = false;            // relink the codec now
  bool waitingPd = false;         // not deciding: charger_pd is not up (once per wait)
  bool waitingBoard = false;      // not deciding: the board configuration is not through (once per link)
  bool planChanged = false;       // the class, a limit, the voltage, charging or the hold differ from the last one reported
  bool handoverReady = false;     // willSleep / shutdown: nothing is left to hand over
  uint32_t events = 0;            // PIANO_CHG_EV_*
  unsigned queued = 0, refused = 0, full = 0;
  uint32_t refusedProperty = 0, refusedValue = 0;
  LinkReadback readback[3];       // the input limit, the enable bit, the bus voltage request
  unsigned readbacks = 0;
};

inline int32_t milliToDeci(int32_t mc) { return mc >= 0 ? mc / 100 : -((-mc + 99) / 100); }

// The contract's "the gauges' I2C idle" (docs/power.md 4): from the sleep handover's acknowledgement to the wake no report runs (the
// gauges' bit-banged I2C, the Type-C block and the RTC over SPMI), so the freeze cuts none of them in the middle; the report that
// fell due meanwhile runs right after the wake. The codec's DSP traffic is not paused (the DSP stays as it is). update() once per
// pass of the owner's loop: +1 when the reports stop, -1 when they run again, 0 otherwise.
struct SleepReports {
  bool quiet = false;
  int update(bool policyOn, bool handover, bool acknowledged)
  {
    const bool q = policyOn && handover && acknowledged;
    if (q == quiet) return 0;
    quiet = q;
    return q ? 1 : -1;
  }
};

class ChargeLink {
 public:
  PianoChargeConfig cfg{};
  PianoChargeState st{};
  PianoChargePlan plan{};
  PianoChargeInput in{};
  uint32_t decisions = 0, queuedTotal = 0, refusedTotal = 0, answers = 0, resyncs = 0;
  bool hold = false;              // the charging-hold assertion's level
  bool healthRefused = false;     // the thresholds were refused: battery health stays off

  // After piano_mca_init (and the allow_* calls): the policy's configuration, and the codec told to leave its built-in policy out.
  void configure(PianoMca &m, const LinkOptions &o)
  {
    piano_charge_config_default(&cfg);
    cfg.writes = o.writes && m.policy ? 1 : 0;
    cfg.hv = uint8_t(o.hv > 2 ? 2 : o.hv);
    cfg.rp = o.rp ? 1 : 0;
    cfg.ramp = o.rp && o.ramp ? 1 : 0;
    cfg.rp_cap_ma = uint16_t(cfg.ramp && o.rpMax ? (o.rpMax > PIANO_CHG_RP_ABS_MAX ? PIANO_CHG_RP_ABS_MAX : o.rpMax) : 0);
    healthRefused = o.health && piano_charge_config_limit(&cfg, 1, o.stopPct, o.resumePct) != 0;
    piano_charge_init(&st);
    piano_mca_use_external_policy(&m, cfg.limit, cfg.hv >= 2);
    piano_mca_allow_icl(&m, piano_charge_icl_max(&cfg));   // the codec's bound follows the policy's (1600 unless stepped higher)
  }

  // The charger thermistor's reading was taken while charger_pd was up (at or after it came up this time).
  static bool chargerReadingCounts(const LinkView &v)
  {
    if (!v.chargerPdUp || !v.board) return false;
    const uint64_t age = v.board->age_ms[PIANO_BOARD_TEMP_CHARGER];
    return age <= v.nowMs && v.nowMs - age >= v.chargerPdUpMs;
  }

  static void fill(PianoChargeInput &x, const PianoMca &m, const LinkView &v)
  {
    x = PianoChargeInput();
    x.now_ms = v.nowMs;
    x.generation = m.links;
    x.wakes = v.wakes;
    x.sleep = v.willSleep ? PIANO_CHG_WILL_SLEEP : PIANO_CHG_AWAKE;
    auto has = [&](int f) { return piano_mca_has(&m, f) != 0; };
    if (has(McaBusVoltage)) {x.valid |= PIANO_CHG_IN_BUS; x.bus_mv = m.value[McaBusVoltage] / 1000;}
    if (has(McaBusCurrent)) {x.valid |= PIANO_CHG_IN_IBUS; x.ibus_ma = m.value[McaBusCurrent] / 1000;}
    if (has(McaType)) {x.valid |= PIANO_CHG_IN_TYPE; x.type = m.value[McaType];}
    if (has(McaIcl)) {x.valid |= PIANO_CHG_IN_ICL; x.icl_ma = m.value[McaIcl];}
    if (has(McaChargeEnable)) {x.valid |= PIANO_CHG_IN_ENABLE; x.enable = m.value[McaChargeEnable];}
    if (has(McaTermVolt)) {x.valid |= PIANO_CHG_IN_FV; x.fv_raw = m.value[McaTermVolt];}
    if (has(McaVerify)) {x.valid |= PIANO_CHG_IN_VERIFY; x.verify = m.value[McaVerify];}
    if (has(McaTbat)) {x.valid |= PIANO_CHG_IN_TBAT; x.tbat_c = m.value[McaTbat];}
    if (has(McaPdos)) {
      x.valid |= PIANO_CHG_IN_PDOS;
      x.pdos = m.pdos < 7 ? m.pdos : 7;
      for (unsigned i = 0; i < x.pdos; ++i)
        for (unsigned k = 0; k < 3; ++k) x.pdo[i][k] = m.pdo[i][k];
    }
    // the gauges only when both packs read (one charger feeds both)
    if (v.packs && v.packsFresh && v.packs[0].ok && v.packs[1].ok) {
      const GaugeReading &a = v.packs[0], &b = v.packs[1];
      x.valid |= PIANO_CHG_IN_GAUGES;
      x.soc_max_pct = a.socPct > b.socPct ? a.socPct : b.socPct;
      x.soc_min_pct = a.socPct > b.socPct ? b.socPct : a.socPct;
      x.vbat_max_mv = a.voltageMv > b.voltageMv ? a.voltageMv : b.voltageMv;
      x.t_warm_dc = a.tempDeciC > b.tempDeciC ? a.tempDeciC : b.tempDeciC;
      x.t_cool_dc = a.tempDeciC > b.tempDeciC ? b.tempDeciC : a.tempDeciC;
      x.ibat_ma = a.currentMa + b.currentMa;
    }
    if (v.port && v.port->ok) {
      x.valid |= PIANO_CHG_IN_TYPEC;
      x.typec_source = v.port->sourceSeen ? 1 : 0;
      x.rp_ma = v.port->sourceSeen ? rpMilliamps(v.port->rpBits) : 0;
    }
    // PianoThermal's: each temperature only with its valid bit (missing = unknown, never cool). The charger thermistor only from a
    // reading taken after charger_pd came up: its pin is the DSP firmware's, and before that firmware runs the channel reads the
    // pin's load (59.5-60.8 C at an idle board, 2026-11-12 queue5 P12; PianoThermal also holds it out while the pin is not
    // high-impedance, AdcChannels.h kPinGates).
    if (v.board && v.board->version == PIANO_BOARD_TEMPS_VERSION) {
      static const struct {unsigned index; uint32_t bit;} kMap[] = {
          {PIANO_BOARD_TEMP_BATTERY, PIANO_CHG_IN_NTC}, {PIANO_BOARD_TEMP_CHARGER, PIANO_CHG_IN_CHGR},
          {PIANO_BOARD_TEMP_CONNECTOR, PIANO_CHG_IN_CONN1}, {PIANO_BOARD_TEMP_CONNECTOR2, PIANO_CHG_IN_CONN2}};
      int32_t *const dst[] = {&x.t_ntc_dc, &x.t_chgr_dc, &x.t_conn1_dc, &x.t_conn2_dc};
      for (unsigned i = 0; i < 4; ++i) {
        if (!(v.board->valid & (1u << kMap[i].index))) continue;
        if (kMap[i].index == PIANO_BOARD_TEMP_CHARGER && !chargerReadingCounts(v)) continue;
        x.valid |= kMap[i].bit;
        *dst[i] = milliToDeci(v.board->milli_c[kMap[i].index]);
      }
    }
  }

  // A decision is due: a codec cycle ended since the last one, or (handing over) the first time and after each write answer.
  bool due(const PianoMca &m, const LinkView &v) const
  {
    if (!m.external) return false;
    if (m.cycles != lastCycles_) return true;
    const bool handover = v.willSleep || v.shutdown;
    return handover && (!lastHandover_ || answers != answersAtDecision_);
  }

  void step(PianoMca &m, const LinkView &v, LinkOutcome &o)
  {
    o = LinkOutcome();
    if (!m.external) return;
    const bool handover = v.willSleep || v.shutdown;
    if (!v.chargerPdUp) {
      if (!pdNoted_) {pdNoted_ = true; o.waitingPd = true;}
      if (handover) o.handoverReady = true;   // nothing can be sent: nothing to wait for
      hold = false;
      return;
    }
    pdNoted_ = false;
    if (m.links != boardLinks_) {boardLinks_ = m.links; boardNoted_ = false;}
    if (!piano_mca_board_settled(&m)) {
      if (!boardNoted_) {boardNoted_ = true; o.waitingBoard = true;}
      if (handover) o.handoverReady = true;
      return;
    }
    if (!due(m, v)) {
      o.handoverReady = handover && lastHandover_ && plan.sleep_ready && !m.queued && !m.sent_write;
      return;
    }
    const bool newCycle = m.cycles != lastCycles_;
    lastCycles_ = m.cycles;
    lastHandover_ = handover;
    answersAtDecision_ = answers;
    fill(in, m, v);
    if (v.shutdown) piano_charge_shutdown(&cfg, &in, &st, &plan);
    else piano_charge_decide(&cfg, &in, &st, &plan);
    decisions++;
    o.decided = true;
    o.events = plan.events;
    if (newCycle) readBack(m, o);
    if (plan.resync && !v.shutdown) {
      // the writes go to the new generation instead (the policy forgets this one when the generation moves)
      o.resync = true;
      resyncs++;
      for (auto &r : rb_) r = Pending();
      hold = false;
      return;
    }
    for (unsigned i = 0; i < plan.writes; ++i) {
      const PianoChargeWrite &w = plan.write[i];
      const int r = piano_mca_queue(&m, w.property, w.value, w.bytes);
      if (!r) {o.queued++; queuedTotal++; continue;}
      if (r == -2) {o.full++; continue;}   // left pending: the policy plans it again after 10 s
      piano_charge_ack(&st, w.property, w.value, -1);
      o.refused++;
      refusedTotal++;
      o.refusedProperty = w.property;
      o.refusedValue = w.value;
    }
    hold = plan.hold_awake && !handover;
    o.handoverReady = handover && plan.sleep_ready;
    if (plan.cls != shown_.cls || plan.icl_ma != shown_.icl || plan.fcc_ma != shown_.fcc || plan.hv != shown_.hv ||
        plan.charge != shown_.charge || plan.icl_why != shown_.why || (hold ? 1 : 0) != shown_.hold) {
      shown_ = {plan.cls, plan.hv, plan.charge, plan.icl_why, uint8_t(hold ? 1 : 0), plan.icl_ma, plan.fcc_ma};
      o.planChanged = true;
    }
  }

  // The codec answered a write (McaWritten): the policy learns the result; a taken limit, enable or voltage is read back next cycle.
  void answered(const PianoMca &m)
  {
    if (!m.external) return;
    answers++;
    piano_charge_ack(&st, m.last_property, m.last_value, m.last_result);
    if (m.last_result) return;
    const int slot = m.last_property == PIANO_MCA_ICL ? 0 : m.last_property == PIANO_MCA_CHARGE_ENABLE ? 1
                     : m.last_property == PIANO_MCA_PD_FIXED || m.last_property == PIANO_MCA_PPS ? 2 : -1;
    if (slot >= 0) rb_[slot] = Pending{m.last_property, m.last_value, m.cycles, true};
  }

  // The owner's timer gave up on the request in flight: a write's goes to the policy as no answer (-60, a refusal).
  void timeout(PianoMca &m)
  {
    const bool write = m.in_flight && m.sent_write;
    piano_mca_timeout(&m);
    if (write) answered(m);
  }

 private:
  struct Pending {uint32_t property = 0, value = 0, cycle = 0; bool armed = false;};
  struct Shown {uint8_t cls = 0xff, hv = 0xff, charge = 0xff, why = 0xff, hold = 0xff; uint32_t icl = 0, fcc = 0;};
  uint32_t lastCycles_ = 0, answersAtDecision_ = 0, boardLinks_ = 0;
  bool lastHandover_ = false, pdNoted_ = false, boardNoted_ = false;
  Pending rb_[3];
  Shown shown_;

  void readBack(const PianoMca &m, LinkOutcome &o)
  {
    for (Pending &p : rb_) {
      if (!p.armed || m.cycles <= p.cycle || o.readbacks >= 3) continue;
      LinkReadback &r = o.readback[o.readbacks++];
      r.property = p.property;
      r.written = p.value;
      if (p.property == PIANO_MCA_ICL) {
        r.readable = piano_mca_has(&m, McaIcl) != 0;
        r.read = m.value[McaIcl];
        r.agrees = r.readable && r.read <= int32_t(p.value) + 50 && r.read + 50 >= int32_t(p.value);
      } else if (p.property == PIANO_MCA_CHARGE_ENABLE) {
        r.readable = piano_mca_has(&m, McaChargeEnable) != 0;
        r.read = m.value[McaChargeEnable];
        r.agrees = r.readable && r.read == int32_t(p.value);
      } else {
        // the bus voltage request: 9 V counts in 8.0-9.6 V, 5 V below 6 V (the PPS word asks for 9 V)
        r.readable = piano_mca_has(&m, McaBusVoltage) != 0;
        r.read = m.value[McaBusVoltage] / 1000;
        const bool nine = p.property == PIANO_MCA_PPS || p.value == 9000;
        r.agrees = r.readable && (nine ? r.read >= 8000 && r.read <= 9600 : r.read < 6000);
      }
      p.armed = false;
    }
  }
};

}  // namespace pianopower
#endif
