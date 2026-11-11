// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoPower's portable cores: the charger PMIC discovery and port decode over a model of the SPMI arbiter
// (ChargerPmic.h, SpmiObserver.h), the software I2C bus and the bq27z561 reads against a model of the gauge (Gauge.h), and the MCA
// codec and charging policy against a model of the DSP's charger properties (PianoMca.c), and ChargePolicy wired to the codec
// (ChargeLink.h, piano-power-policy=1) against the same model. Built and run by tooling/k27/build-native-piano-power.sh before the kext.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include "SpmiObserver.h"
#include "ChargerPmic.h"
#include "Gauge.h"
#include "RtcReadout.h"
#include <algorithm>
#include <ctime>
extern "C" {
#include "PianoMca.h"
#include "Servreg.h"
}
#include "ChargeLink.h"

using namespace pianopower;

static int gFailures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++gFailures;                                                               \
    }                                                                            \
  } while (0)

// ------------------------------------------------------------------------------------------------------------- SPMI model
struct Arb {
  struct Entry {uint16_t ppid; uint32_t owner; uint8_t regs[256];};
  std::vector<Entry> apids;
  uint32_t lastCmd[1024] = {};
  unsigned writesOutsideCommands = 0;
  int add(uint8_t sid, uint16_t address, uint8_t type, uint8_t subtype, uint32_t owner)
  {
    Entry e{};
    e.ppid = uint16_t(sid << 8 | address >> 8);
    e.owner = owner;
    e.regs[4] = type;
    e.regs[5] = subtype;
    apids.push_back(e);
    return int(apids.size() - 1);
  }
  static uint32_t rd(void *ctx, Window w, uint32_t off)
  {
    Arb *a = static_cast<Arb *>(ctx);
    if (w == Window::core) {
      if (off == 0) return 0x70020000;
      if (off == 4) return uint32_t(a->apids.size());
      if (off >= kSpmiApidMap && (off - kSpmiApidMap) / 4 < a->apids.size()) return uint32_t(a->apids[(off - kSpmiApidMap) / 4].ppid) << 8;
      return 0;
    }
    if (w == Window::cnfg) return off / 4 < a->apids.size() ? a->apids[off / 4].owner : 0;
    const uint32_t apid = off / kObsvStride, reg = off % kObsvStride;
    if (apid >= a->apids.size()) return 0;
    if (reg == kObsvStatus) return kStatusDone;
    const uint32_t cmd = a->lastCmd[apid], start = (cmd >> 4) & 0xff, count = (cmd & 7) + 1;
    uint32_t v = 0;
    const uint32_t first = reg == kObsvData0 ? 0 : 4;
    for (uint32_t b = 0; b < 4 && first + b < count; ++b) v |= uint32_t(a->apids[apid].regs[(start + first + b) & 0xff]) << (8 * b);
    return v;
  }
  static void wr(void *ctx, Window w, uint32_t off, uint32_t value)
  {
    Arb *a = static_cast<Arb *>(ctx);
    if (w == Window::obsv && off % kObsvStride == kObsvCmd) a->lastCmd[off / kObsvStride] = value;
    else a->writesOutsideCommands++;
  }
  static void delay(void *, unsigned) {}
  Io io() { return Io{this, rd, wr, delay}; }
};

static void testCharger()
{
  Arb a;
  a.add(0, 0x0100, 0x51, 0x10, 0);               // another PMIC's REVID
  const int revid = a.add(7, 0x0100, 0x51, 0x47, 0);
  a.add(7, 0x0800, 0x0a, 0x01, 0);               // something else on SID 7
  a.add(7, 0x2600, 0x02, 0x7f, 2);               // the charger, owned by EE 2
  a.add(7, 0x2700, 0x03, 0x01, 2);               // DCDC
  a.add(7, 0x2900, 0x04, 0x01, 2);               // USBIN
  const int typec = a.add(7, 0x2c00, 0x05, 0x01, 2);
  a.add(7, 0x2c00, 0x05, 0x01, 0);               // the same PPID mapped for EE 0 as well: preferred
  a.add(7, 0x3000, 0x02, 0xa8, 2);               // a higher charger-type block: not the charger
  (void)revid;
  const Io io = a.io();
  const Arbiter arb = spmiArbiter(io);
  CHECK(arb.v7 && arb.count == a.apids.size());
  const ChargerLayout l = discoverCharger(io, arb);
  CHECK(l.found);
  CHECK(l.model == 0x47 && l.subtype == 0x7f);
  CHECK(l.charger.address == 0x2600 && l.charger.owner == 2);
  CHECK(l.dcdc.address == 0x2700 && l.usbin.address == 0x2900 && l.typec.address == 0x2c00);
  CHECK(l.typec.apid == typec + 1 && l.typec.owner == 0);
  CHECK(l.peripherals == 8 && l.ownedBy[2] == 5 && l.ownedBy[0] == 3);

  // Port decode: a source with Rp (bit 2) on CC2; then a USB device (Rd seen, no Rp); then nothing.
  uint8_t *t = a.apids[size_t(l.typec.apid)].regs;
  t[0x06] = 0x04; t[0x08] = 0x00; t[0x0b] = 0x03;
  PortState p = readPort(io, l);
  CHECK(p.ok && p.attached && p.sourceSeen && !p.sinkSeen && p.cc == 2 && rpMilliamps(p.rpBits) == 1500);
  // The Rp bit order (Linux's register facts): bit 1 3.0 A (the Mac's port read 0x02), bit 2 1.5 A, bit 3 the default, bit 0 alone none.
  CHECK(rpMilliamps(0x02) == 3000 && rpMilliamps(0x04) == 1500 && rpMilliamps(0x08) == 500 && rpMilliamps(0x00) == 0);
  CHECK(rpMilliamps(0x01) == 0);
  t[0x06] = 0x02;
  p = readPort(io, l);
  CHECK(p.sourceSeen && rpMilliamps(p.rpBits) == 3000);
  t[0x06] = 0x00; t[0x08] = 0x08; t[0x0b] = 0x01;
  p = readPort(io, l);
  CHECK(p.attached && !p.sourceSeen && p.sinkSeen && p.cc == 1);
  t[0x0b] = 0x00;
  p = readPort(io, l);
  CHECK(!p.attached && !p.sourceSeen && !p.sinkSeen);
  CHECK(a.writesOutsideCommands == 0);   // read only: the observer channel's command is the only store

  // SUBTYPE 0xba moves the blocks by 0x300.
  Arb b;
  b.add(7, 0x0100, 0x51, 0x56, 0);
  b.add(7, 0x2600, 0x02, 0xba, 2);
  b.add(7, 0x2f00, 0x05, 0x01, 2);
  const Io io2 = b.io();
  const ChargerLayout l2 = discoverCharger(io2, spmiArbiter(io2));
  CHECK(l2.found && l2.typec.address == 0x2f00 && l2.usbin.address == 0x2c00 && l2.dcdc.apid < 0);
  // Not a PMIH010x.
  Arb c;
  c.add(7, 0x0100, 0x51, 0x33, 0);
  const Io io3 = c.io();
  CHECK(!discoverCharger(io3, spmiArbiter(io3)).found);
}

// --------------------------------------------------------------------------------------------------------- I2C gauge model
struct Gauge {
  uint8_t address = kGaugeAddress;
  bool present = true, stuckLow = false;
  std::map<uint8_t, uint16_t> regs;
  bool mSda = true, mScl = true, sSda = true, lastSda = true, lastScl = true;
  enum State { idle, address_, command, ignore, transmit } state = idle;
  int bits = 0;
  uint8_t shift = 0, reg = 0, txByte = 0;
  int txBits = 0, txIndex = 0;
  bool ackPhase = false, ackIsMine = false, nacked = false, reading = false;
  unsigned starts = 0, stops = 0, reads = 0;

  bool sda() const { return mSda && sSda && !stuckLow; }
  bool scl() const { return mScl; }
  void driveBit(bool one) { sSda = one; }
  void startTx()
  {
    const uint16_t v = regs.count(reg) ? regs[reg] : 0;
    txByte = uint8_t(txIndex == 0 ? v : v >> 8);
    txIndex++;
    txBits = 1;
    driveBit((txByte >> 7) & 1);
  }
  void rising(bool d)
  {
    if (ackPhase) {
      if (!ackIsMine) nacked = d;
      return;
    }
    if (state == address_ || state == command || state == ignore) {
      shift = uint8_t(shift << 1 | (d ? 1 : 0));
      bits++;
    }
  }
  void falling()
  {
    if (ackPhase) {
      ackPhase = false;
      if (ackIsMine) {
        sSda = true;
        if (state == transmit) startTx();
      } else if (nacked) {
        sSda = true;
        state = idle;
      } else {
        startTx();
      }
      return;
    }
    if (state == transmit) {
      if (txBits < 8) {
        driveBit((txByte >> (7 - txBits)) & 1);
        txBits++;
      } else {
        sSda = true;
        ackPhase = true;
        ackIsMine = false;
        nacked = false;
      }
      return;
    }
    if ((state == address_ || state == command || state == ignore) && bits == 8) {
      bits = 0;
      if (state == address_) {
        if (present && (shift >> 1) == address) {
          reading = shift & 1;
          state = reading ? transmit : command;
          if (reading) {txIndex = 0; reads++;}
          sSda = false;
          ackPhase = true;
          ackIsMine = true;
        } else {
          state = idle;
        }
      } else if (state == command) {
        reg = shift;
        state = ignore;
        sSda = false;
        ackPhase = true;
        ackIsMine = true;
      } else {
        sSda = false;
        ackPhase = true;
        ackIsMine = true;
      }
      shift = 0;
    }
  }
  void update()
  {
    const bool d = sda(), c = scl();
    if (c && lastScl && lastSda != d) {
      if (!d) {starts++; state = address_; bits = 0; shift = 0; ackPhase = false; sSda = true;}
      else {stops++; state = idle; sSda = true;}
    }
    if (!lastScl && c) rising(d);
    if (lastScl && !c) falling();
    lastSda = sda();
    lastScl = c;
  }
  static void setSda(void *ctx, bool release) { Gauge *g = static_cast<Gauge *>(ctx); g->mSda = release; g->update(); }
  static void setScl(void *ctx, bool release) { Gauge *g = static_cast<Gauge *>(ctx); g->mScl = release; g->update(); }
  static bool getSda(void *ctx) { return static_cast<Gauge *>(ctx)->sda(); }
  static bool getScl(void *ctx) { return static_cast<Gauge *>(ctx)->scl(); }
  static void delay(void *, unsigned) {}
  I2cIo io() { return I2cIo{this, setSda, setScl, getSda, getScl, delay}; }
};

static void fillGauge(Gauge &g, int mv, int ma, int soc, int rem, int full, int deciC, uint16_t status)
{
  g.regs[kGaugeVoltage] = uint16_t(mv);
  g.regs[kGaugeCurrent] = uint16_t(int16_t(ma));
  g.regs[kGaugeAverageCurrent] = uint16_t(int16_t(ma));
  g.regs[kGaugeSoc] = uint16_t(soc);
  g.regs[kGaugeRemaining] = uint16_t(rem);
  g.regs[kGaugeFull] = uint16_t(full);
  g.regs[kGaugeTemperature] = uint16_t(deciC + 2732);
  g.regs[kGaugeStatus] = status;
  g.regs[kGaugeCycles] = 172;
}

static void testGauge()
{
  Gauge a;
  fillGauge(a, 3912, -321, 47, 2300, 4900, 305, kGaugeDsg | kGaugeInit);
  I2cBus busA(a.io());
  uint16_t v = 0;
  CHECK(busA.readWord(kGaugeAddress, kGaugeVoltage, &v) == I2cResult::ok && v == 3912);
  CHECK(a.starts == 2 && a.stops == 1 && a.reg == kGaugeVoltage);
  const GaugeReading ga = readGauge(busA);
  CHECK(ga.ok && ga.voltageMv == 3912 && ga.currentMa == -321 && ga.socPct == 47 && ga.remainingMah == 2300 && ga.fullMah == 4900);
  CHECK(ga.tempDeciC == 305 && ga.status == (kGaugeDsg | kGaugeInit) && ga.cycles == 172);
  CHECK(a.mSda && a.mScl && a.sda() && a.scl());   // the bus is released after each transaction

  Gauge b;
  fillGauge(b, 3916, -300, 49, 2450, 5000, 320, kGaugeDsg);
  I2cBus busB(b.io());
  const GaugeReading gb = readGauge(busB);
  const GaugeReading both[2] = {ga, gb};
  const PackReading p = combinePacks(both, 2);
  CHECK(p.ok && p.packs == 2 && p.voltageMv == 3914 && p.currentMa == -621 && p.remainingMah == 4750 && p.fullMah == 9900);
  CHECK(p.socPct == 48 && p.tempDeciC == 320 && p.discharging && !p.charging && !p.full);

  // One pack missing: the other alone.
  GaugeReading none;
  const GaugeReading one[2] = {none, gb};
  const PackReading q = combinePacks(one, 2);
  CHECK(q.ok && q.packs == 1 && q.voltageMv == 3916 && q.socPct == 49);

  // No device at the address.
  Gauge absent;
  absent.present = false;
  I2cBus busC(absent.io());
  CHECK(busC.readWord(kGaugeAddress, kGaugeVoltage, &v) == I2cResult::nackAddress);
  const GaugeReading gc = readGauge(busC);
  CHECK(!gc.ok && gc.error == I2cResult::nackAddress && gc.failedCommand == kGaugeVoltage);

  // A line held low that clocking does not free.
  Gauge stuck;
  stuck.stuckLow = true;
  I2cBus busD(stuck.io());
  CHECK(busD.readWord(kGaugeAddress, kGaugeVoltage, &v) == I2cResult::busBusy);

  // An implausible reading (all ones) is not ok.
  Gauge ones;
  for (uint8_t r : {kGaugeVoltage, kGaugeCurrent, kGaugeAverageCurrent, kGaugeSoc, kGaugeRemaining, kGaugeFull, kGaugeTemperature,
                    kGaugeStatus, kGaugeCycles})
    ones.regs[r] = 0xffff;
  I2cBus busE(ones.io());
  CHECK(!readGauge(busE).ok);
}

// ------------------------------------------------------------------------------------------------------------ MCA model
static uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }

struct Dsp {
  std::map<uint32_t, uint32_t> props;
  std::vector<uint32_t> pdo;   // flat {min, max, ma} triples
  std::map<uint32_t, uint32_t> retcodeFor;   // the firmware's answer to writes of a property
  struct Write {uint32_t property, value;};
  std::vector<Write> writes;
  unsigned reads = 0, panEnables = 0, panAcks = 0;
  std::map<uint32_t, unsigned> readsOf;
  uint32_t otg[4] = {};   // the last OTG configuration payload written
  bool dropReads = false;
  void (*hook)(void *, PianoMca &, int) = nullptr;   // after each answer is received (the owner's view of it)
  void *hookCtx = nullptr;

  // Answers every request the codec has until it has none; returns how many.
  unsigned run(PianoMca &m)
  {
    unsigned n = 0;
    uint8_t out[PIANO_MCA_REQUEST_BYTES], resp[PIANO_MCA_RESPONSE_BYTES];
    for (unsigned guard = 0; guard < 200; ++guard) {
      const uint32_t len = piano_mca_next(&m, out);
      if (!len) break;
      CHECK(len == PIANO_MCA_REQUEST_BYTES);
      CHECK(le32(out) == PIANO_MCA_OWNER && le32(out + 4) == PIANO_MCA_TYPE_REQUEST);
      const uint32_t op = le32(out + 8), prop = le32(out + 12), seq = le32(out + 16);
      std::memset(resp, 0, sizeof resp);
      put32(resp, PIANO_MCA_OWNER);
      put32(resp + 4, PIANO_MCA_TYPE_REQUEST);
      put32(resp + 8, op);
      put32(resp + 12, prop);
      put32(resp + 16, op == PIANO_MCA_OP_WRITE && retcodeFor.count(prop) ? retcodeFor[prop] : 0);
      put32(resp + 20, seq);
      if (op == PIANO_MCA_OP_READ) {
        reads++;
        readsOf[prop]++;
        if (dropReads) {piano_mca_timeout(&m); ++n; continue;}
        if (prop == PIANO_MCA_PDOS) {
          for (size_t i = 0; i < pdo.size(); ++i) put32(resp + 24 + 4 * i, pdo[i]);
        } else if (prop == PIANO_MCA_CHARGE_ENABLE || prop == PIANO_MCA_INPUT_SUSPEND || prop == PIANO_MCA_AICL_ENABLE) {
          resp[24] = uint8_t(props.count(prop) ? props[prop] : 0);
        } else {
          put32(resp + 24, props.count(prop) ? props[prop] : 0);
        }
      } else {
        CHECK(op == PIANO_MCA_OP_WRITE);
        const bool oneByte = prop == PIANO_MCA_CHARGE_ENABLE || prop == PIANO_MCA_INPUT_SUSPEND || prop == PIANO_MCA_CP_STATE;
        const uint32_t value = oneByte ? out[20] : le32(out + 20);
        if (prop == PIANO_MCA_OTG_CFG)
          for (unsigned w = 0; w < 4; ++w) otg[w] = le32(out + 20 + 4 * w);
        writes.push_back({prop, value});
        if (!retcodeFor.count(prop) || !retcodeFor[prop]) props[prop] = value;
        if (prop == PIANO_MCA_PD_FIXED && value == 9000 && props.count(0xf9000)) props[PIANO_MCA_BUS_VOLTAGE] = props[0xf9000];
        if (prop == PIANO_MCA_PD_FIXED && value == 5000 && props.count(0xf5000)) props[PIANO_MCA_BUS_VOLTAGE] = props[0xf5000];
      }
      const int r = piano_mca_receive(&m, resp, sizeof resp);
      if (hook) hook(hookCtx, m, r);
      ++n;
    }
    return n;
  }
  void pan(PianoMca &m)
  {
    uint8_t out[PIANO_MCA_PAN_REQUEST_BYTES], resp[16];
    for (unsigned guard = 0; guard < 10; ++guard) {
      const uint32_t len = piano_mca_pan_next(&m, out);
      if (!len) break;
      CHECK(len == PIANO_MCA_PAN_REQUEST_BYTES && le32(out) == PIANO_MCA_OWNER_PAN && le32(out + 8) == PIANO_MCA_PAN_WRITE);
      if (le32(out + 12) == PIANO_MCA_PAN_ENABLE) panEnables++;
      if (le32(out + 12) == PIANO_MCA_PAN_ACK) panAcks++;
      std::memset(resp, 0, sizeof resp);
      put32(resp, PIANO_MCA_OWNER_PAN);
      put32(resp + 4, PIANO_MCA_TYPE_REQUEST);
      put32(resp + 8, PIANO_MCA_PAN_WRITE);
      CHECK(piano_mca_receive(&m, resp, sizeof resp) == McaPanDone);
    }
  }
  uint32_t last(uint32_t property) const
  {
    for (size_t i = writes.size(); i--;) if (writes[i].property == property) return writes[i].value;
    return 0;
  }
  unsigned count(uint32_t property) const
  {
    unsigned n = 0;
    for (const Write &w : writes) n += w.property == property;
    return n;
  }
};

static void cycle(PianoMca &m, Dsp &d) {piano_mca_refresh(&m); d.run(m);}

static void testMcaCodec()
{
  PianoMca m;
  piano_mca_init(&m, 0, 0);
  piano_mca_link(&m, 1);
  uint8_t out[PIANO_MCA_REQUEST_BYTES];
  const uint32_t n = piano_mca_next(&m, out);
  CHECK(n == PIANO_MCA_REQUEST_BYTES && le32(out + 8) == PIANO_MCA_OP_READ && le32(out + 12) == PIANO_MCA_BUS_VOLTAGE && le32(out + 16) == 1);
  CHECK(piano_mca_next(&m, out) == 0);   // one in flight
  // A stale answer (wrong seq) is ignored; the right one advances.
  uint8_t resp[PIANO_MCA_RESPONSE_BYTES] = {};
  put32(resp, PIANO_MCA_OWNER); put32(resp + 4, 1); put32(resp + 8, 1); put32(resp + 12, PIANO_MCA_BUS_VOLTAGE); put32(resp + 20, 7);
  put32(resp + 24, 5000000);
  CHECK(piano_mca_receive(&m, resp, sizeof resp) == McaIgnored);
  put32(resp + 20, 1);
  CHECK(piano_mca_receive(&m, resp, sizeof resp) == McaAnswer);
  CHECK(piano_mca_has(&m, McaBusVoltage) && m.value[McaBusVoltage] == 5000000);
  // A notification starts another cycle once this one ends.
  uint8_t notify[16] = {};
  put32(notify, PIANO_MCA_OWNER); put32(notify + 4, PIANO_MCA_TYPE_NOTIFY);
  CHECK(piano_mca_receive(&m, notify, sizeof notify) == McaNotified && m.refresh_wanted);
  // A timeout skips the field.
  CHECK(piano_mca_next(&m, out) && le32(out + 12) == PIANO_MCA_BUS_CURRENT);
  piano_mca_timeout(&m);
  CHECK(m.timeouts == 1 && !piano_mca_has(&m, McaBusCurrent));
  // Policy 0 never writes, even attached with the charger disabled.
  Dsp d;
  d.props[PIANO_MCA_BUS_VOLTAGE] = 5000000;
  d.props[PIANO_MCA_REAL_TYPE] = PIANO_MCA_TYPE_DCP;
  d.props[PIANO_MCA_ICL] = 100;
  d.props[PIANO_MCA_TBAT] = 30;
  d.props[PIANO_MCA_CHARGE_ENABLE] = 0;
  d.run(m);
  cycle(m, d);
  CHECK(d.writes.empty());
  // PAN: enable once, then an acknowledgement per notified port.
  d.pan(m);
  CHECK(d.panEnables == 1 && d.panAcks == 0);
  uint8_t pan[32] = {};
  put32(pan, PIANO_MCA_OWNER_PAN); put32(pan + 4, 1); put32(pan + 8, PIANO_MCA_PAN_NOTIFY); pan[12] = 1;
  CHECK(piano_mca_receive(&m, pan, sizeof pan) == McaPanNotified);
  pan[12] = 7;
  CHECK(piano_mca_receive(&m, pan, sizeof pan) == McaIgnored);   // no such port
  d.pan(m);
  CHECK(d.panAcks == 1);
  // The read-only input-path diagnostic (§5c): the one-byte properties decode from byte 0, the four-byte ones from the word.
  Dsp diag;
  diag.props[PIANO_MCA_INPUT_SUSPEND] = 1;
  diag.props[PIANO_MCA_AICL_ENABLE] = 1;
  diag.props[PIANO_MCA_AICL_STATUS] = 0x30;
  diag.props[PIANO_MCA_STATUS] = 7;
  diag.props[PIANO_MCA_BUS_CURRENT] = 0;
  PianoMca p;
  piano_mca_init(&p, 0, 0);
  piano_mca_link(&p, 1);
  diag.run(p);
  CHECK(piano_mca_has(&p, McaInputSuspend) && p.value[McaInputSuspend] == 1);
  CHECK(piano_mca_has(&p, McaAiclEnable) && p.value[McaAiclEnable] == 1);
  CHECK(piano_mca_has(&p, McaAiclStatus) && p.value[McaAiclStatus] == 0x30);
  CHECK(piano_mca_has(&p, McaStatus) && p.value[McaStatus] == 7);
  CHECK(diag.readsOf[PIANO_MCA_INPUT_SUSPEND] && diag.readsOf[PIANO_MCA_AICL_ENABLE] && diag.readsOf[PIANO_MCA_AICL_STATUS]);
  CHECK(diag.writes.empty());   // the diagnostic never writes
  CHECK(!std::strcmp(piano_mca_status_name(1), "charging") && !std::strcmp(piano_mca_status_name(2), "discharging"));
  CHECK(!std::strcmp(piano_mca_status_name(3), "not charging") && !std::strcmp(piano_mca_status_name(4), "full"));
  CHECK(!std::strcmp(piano_mca_status_name(7), "unknown") && !std::strcmp(piano_mca_status_name(0), "unknown"));
  // A one-byte property whose answer carries only one byte is still taken (the model sends the full 280 bytes).
  CHECK(p.errors == 0);

  // Short or foreign messages are ignored.
  CHECK(piano_mca_receive(&m, resp, 8) == McaIgnored);
  put32(resp, 0x1234);
  CHECK(piano_mca_receive(&m, resp, sizeof resp) == McaIgnored);
}

static void testMcaPolicy()
{
  // The allowlist.
  CHECK(piano_mca_write_allowed(PIANO_MCA_ICL, 1500, 4) && !piano_mca_write_allowed(PIANO_MCA_ICL, 2500, 4));
  CHECK(!piano_mca_write_allowed(PIANO_MCA_ICL, 50, 4) && !piano_mca_write_allowed(PIANO_MCA_FCC, 3000, 4));
  CHECK(piano_mca_write_allowed(PIANO_MCA_CHARGE_ENABLE, 1, 1) && !piano_mca_write_allowed(PIANO_MCA_CHARGE_ENABLE, 0, 1));
  CHECK(piano_mca_write_allowed(PIANO_MCA_INPUT_SUSPEND, 0, 1) && !piano_mca_write_allowed(PIANO_MCA_INPUT_SUSPEND, 1, 1));
  CHECK(!piano_mca_write_allowed(PIANO_MCA_INPUT_SUSPEND, 0, 4));
  CHECK(piano_mca_write_allowed(PIANO_MCA_PD_FIXED, 9000, 4) && !piano_mca_write_allowed(PIANO_MCA_PD_FIXED, 12000, 4));
  CHECK(!piano_mca_write_allowed(PIANO_MCA_STATUS, 1, 4) && !piano_mca_write_allowed(0x201a, 0, 4));
  CHECK(piano_mca_icl_for(PIANO_MCA_TYPE_SDP, 0) == 500 && piano_mca_icl_for(PIANO_MCA_TYPE_CDP, 0) == 900);
  CHECK(piano_mca_icl_for(PIANO_MCA_TYPE_DCP, 0) == 1500 && piano_mca_icl_for(PIANO_MCA_TYPE_PD, 3000) == 1500);
  CHECK(piano_mca_icl_for(PIANO_MCA_TYPE_PD, 0) == 900 && piano_mca_icl_for(PIANO_MCA_TYPE_PD, 1200) == 1200);
  CHECK(piano_mca_icl_for(PIANO_MCA_TYPE_FLOAT, 0) == 500 && piano_mca_icl_for(PIANO_MCA_TYPE_UNKNOWN, 0) == 500);
  CHECK(piano_mca_fcc_for(1, 30, 0) == 1000 && piano_mca_fcc_for(1, 30, 1) == 1500 && piano_mca_fcc_for(0, 30, 0) == 500);
  CHECK(piano_mca_fcc_for(1, 10, 0) == 500 && piano_mca_fcc_for(1, 38, 0) == 500 && piano_mca_fcc_for(1, 37, 0) == 1000);

  // The suspended input: written to 0 only with piano-power-unsuspend, at most three times, and never to 1.
  {
    Dsp e;
    e.props[PIANO_MCA_BUS_VOLTAGE] = 5100000;
    e.props[PIANO_MCA_REAL_TYPE] = PIANO_MCA_TYPE_CDP;
    e.props[PIANO_MCA_INPUT_SUSPEND] = 1;
    e.props[PIANO_MCA_CHARGE_ENABLE] = 1;
    e.props[PIANO_MCA_TBAT] = 28;
    PianoMca off;
    piano_mca_init(&off, 1, 0);
    piano_mca_link(&off, 1);
    e.run(off);
    CHECK(e.count(PIANO_MCA_INPUT_SUSPEND) == 0);          // not allowed by default
    CHECK(e.last(PIANO_MCA_ICL) == 900);                   // CDP keeps the Ubuntu policy's 900 mA
    Dsp f = e;
    f.writes.clear();
    PianoMca on;
    piano_mca_init(&on, 1, 0);
    piano_mca_allow_unsuspend(&on, 1);
    piano_mca_link(&on, 1);
    f.run(on);
    CHECK(f.count(PIANO_MCA_INPUT_SUSPEND) == 1 && f.last(PIANO_MCA_INPUT_SUSPEND) == 0 && f.props[PIANO_MCA_INPUT_SUSPEND] == 0);
    cycle(on, f);
    CHECK(f.count(PIANO_MCA_INPUT_SUSPEND) == 1);          // it reads 0 now: nothing more
    // A firmware that refuses it: three tries, then no more.
    Dsp g = e;
    g.writes.clear();
    g.retcodeFor[PIANO_MCA_INPUT_SUSPEND] = 5;
    PianoMca refused;
    piano_mca_init(&refused, 1, 0);
    piano_mca_allow_unsuspend(&refused, 1);
    piano_mca_link(&refused, 1);
    for (int i = 0; i < 6; ++i) {piano_mca_refresh(&refused); g.run(refused);}
    CHECK(g.count(PIANO_MCA_INPUT_SUSPEND) == 3);
    // Without the policy the switch is ignored.
    PianoMca ro;
    piano_mca_init(&ro, 0, 0);
    piano_mca_allow_unsuspend(&ro, 1);
    CHECK(!ro.unsuspend_allowed);
  }

  // A DCP is attached, the charger disabled: input limit, charge current and enable, then nothing more.
  PianoMca m;
  piano_mca_init(&m, 1, 0);
  piano_mca_link(&m, 1);
  Dsp d;
  d.props[PIANO_MCA_BUS_VOLTAGE] = 5100000;
  d.props[PIANO_MCA_REAL_TYPE] = PIANO_MCA_TYPE_DCP;
  d.props[PIANO_MCA_ICL] = 100;
  d.props[PIANO_MCA_TBAT] = 30;
  d.props[PIANO_MCA_CHARGE_ENABLE] = 0;
  d.run(m);
  CHECK(piano_mca_take_events(&m) & McaEvAttach);
  CHECK(d.last(PIANO_MCA_ICL) == 1500 && d.last(PIANO_MCA_FCC) == 1000 && d.last(PIANO_MCA_CHARGE_ENABLE) == 1);
  CHECK(m.icl_set == 1500 && m.fcc_set == 1000 && m.writes_ok == 3);
  CHECK(!d.readsOf.count(PIANO_MCA_PDOS) && !d.readsOf.count(PIANO_MCA_FCC) && d.reads >= 9);
  const size_t settled = d.writes.size();
  cycle(m, d);
  cycle(m, d);
  CHECK(d.writes.size() == settled);   // read back as set: nothing more
  // Warm: the charge current drops to the floor, the input limit stays.
  d.props[PIANO_MCA_TBAT] = 40;
  cycle(m, d);
  CHECK(d.last(PIANO_MCA_FCC) == 500 && d.count(PIANO_MCA_ICL) == 1);
  // No firmware temperature (out of range): the gauges' is used.
  d.props[PIANO_MCA_TBAT] = 0xffffff9c;   // -100 C
  piano_mca_battery_temperature(&m, 1, 25);
  cycle(m, d);
  CHECK(d.last(PIANO_MCA_FCC) == 1000);
  // The firmware refuses the input limit: three tries per attach, then no more.
  d.props[PIANO_MCA_ICL] = 100;
  d.retcodeFor[PIANO_MCA_ICL] = 5;
  for (int i = 0; i < 6; ++i) cycle(m, d);
  CHECK(d.count(PIANO_MCA_ICL) == 1 + 3);
  CHECK(m.writes_failed == 3);
  d.retcodeFor.erase(PIANO_MCA_ICL);
  // Detach: back to the firmware's own values, once.
  d.props[PIANO_MCA_BUS_VOLTAGE] = 300000;
  cycle(m, d);
  CHECK(piano_mca_take_events(&m) & McaEvDetach);
  CHECK(d.last(PIANO_MCA_ICL) == PIANO_MCA_ICL_FLOOR && d.last(PIANO_MCA_FCC) == PIANO_MCA_FCC_FLOOR);
  const size_t detached = d.writes.size();
  cycle(m, d);
  CHECK(d.writes.size() == detached);
  // An SDP: 500 mA.
  d.props[PIANO_MCA_BUS_VOLTAGE] = 5000000;
  d.props[PIANO_MCA_REAL_TYPE] = PIANO_MCA_TYPE_SDP;
  d.props[PIANO_MCA_CHARGE_ENABLE] = 1;
  cycle(m, d);
  CHECK(d.last(PIANO_MCA_ICL) == 500);
  // A restarted firmware (link down and up): everything again.
  piano_mca_link(&m, 0);
  piano_mca_link(&m, 1);
  d.props[PIANO_MCA_ICL] = 100;
  const unsigned before = d.count(PIANO_MCA_ICL);
  d.run(m);
  CHECK(d.count(PIANO_MCA_ICL) == before + 1 && d.last(PIANO_MCA_ICL) == 500);
}

static void testBoardConfig()
{
  CHECK(piano_mca_write_allowed(PIANO_MCA_CP_STATE, 1, 1) && !piano_mca_write_allowed(PIANO_MCA_CP_STATE, 0, 1));
  CHECK(piano_mca_write_allowed(PIANO_MCA_CID_CFG, 0x708, 4) && !piano_mca_write_allowed(PIANO_MCA_CID_CFG, 0x709, 4));
  CHECK(piano_mca_write_allowed(PIANO_MCA_OTG_CFG, 0, 16) && !piano_mca_write_allowed(PIANO_MCA_OTG_CFG, 0, 4));
  Dsp d;
  d.props[PIANO_MCA_BUS_VOLTAGE] = 5100000;
  d.props[PIANO_MCA_REAL_TYPE] = PIANO_MCA_TYPE_DCP;
  d.props[PIANO_MCA_ICL] = 100;
  d.props[PIANO_MCA_TBAT] = 28;
  d.props[PIANO_MCA_CHARGE_ENABLE] = 1;
  PianoMca m;
  piano_mca_init(&m, 1, 0);
  piano_mca_allow_board(&m, 1);
  piano_mca_link(&m, 1);
  d.run(m);
  // The board configuration first, then the policy, all in this one run (the read-back cycle after the board writes).
  CHECK(d.count(PIANO_MCA_CP_STATE) == 1 && d.last(PIANO_MCA_CP_STATE) == 1);
  CHECK(d.count(PIANO_MCA_CID_CFG) == 1 && d.last(PIANO_MCA_CID_CFG) == 0x708);
  CHECK(d.count(PIANO_MCA_OTG_CFG) == 1 && d.otg[0] == 0 && d.otg[1] == 1 && d.otg[2] == 0xffffffffu && d.otg[3] == 0xffffffffu);
  size_t firstPolicy = d.writes.size(), lastBoard = 0;
  for (size_t i = 0; i < d.writes.size(); ++i) {
    const uint32_t q = d.writes[i].property;
    if (q == PIANO_MCA_CP_STATE || q == PIANO_MCA_OTG_CFG || q == PIANO_MCA_CID_CFG) lastBoard = i;
    else if (firstPolicy == d.writes.size()) firstPolicy = i;
  }
  CHECK(lastBoard < firstPolicy && d.last(PIANO_MCA_ICL) == 1500);
  CHECK(m.board_done == 7 && (piano_mca_take_events(&m) & McaEvBoardDone));
  cycle(m, d);
  CHECK(d.count(PIANO_MCA_CP_STATE) == 1);   // once per generation
  // A restarted firmware: sent again.
  piano_mca_link(&m, 0);
  piano_mca_link(&m, 1);
  d.run(m);
  CHECK(d.count(PIANO_MCA_CP_STATE) == 2 && d.count(PIANO_MCA_OTG_CFG) == 2 && d.count(PIANO_MCA_CID_CFG) == 2);
  // Refused: five cycles of tries, then the policy goes on.
  Dsp r = d;
  r.writes.clear();
  r.retcodeFor[PIANO_MCA_CID_CFG] = 5;
  r.props[PIANO_MCA_ICL] = 100;   // the model kept the earlier run's 1500
  PianoMca n;
  piano_mca_init(&n, 1, 0);
  piano_mca_allow_board(&n, 1);
  piano_mca_link(&n, 1);
  for (int i = 0; i < 8; ++i) {piano_mca_refresh(&n); r.run(n);}
  CHECK(r.count(PIANO_MCA_CID_CFG) == 5 && r.count(PIANO_MCA_CP_STATE) == 1 && r.count(PIANO_MCA_OTG_CFG) == 1);
  CHECK((piano_mca_take_events(&n) & McaEvBoardGaveUp) && r.count(PIANO_MCA_ICL) >= 1);
  // Off by default, and never without the policy.
  Dsp o = d;
  o.writes.clear();
  PianoMca off;
  piano_mca_init(&off, 1, 0);
  piano_mca_link(&off, 1);
  o.run(off);
  CHECK(o.count(PIANO_MCA_CP_STATE) == 0 && o.count(PIANO_MCA_OTG_CFG) == 0);
  PianoMca ro;
  piano_mca_init(&ro, 0, 0);
  piano_mca_allow_board(&ro, 1);
  CHECK(!ro.board_allowed);
  // The diag reads include the port state, the float readback and the board readbacks, and the verify flag always.
  CHECK(d.readsOf[PIANO_MCA_OTG_BOOST] && d.readsOf[PIANO_MCA_OTG_GATE] && d.readsOf[PIANO_MCA_TYPEC_MODE] && d.readsOf[PIANO_MCA_SNK_SRC]);
  CHECK(d.readsOf[PIANO_MCA_USB_TYPE] && d.readsOf[PIANO_MCA_TERM_VOLT] && d.readsOf[PIANO_MCA_VERIFY] && d.readsOf[PIANO_MCA_CID_CFG]);
}

static void testServreg()
{
  uint8_t out[96];
  const uint32_t n = servreg_register_listener(out, sizeof out, 7, SERVREG_CHARGER_PD);
  QcomQmiMessage m;
  CHECK(n == QCOM_QMI_HEADER + 4 + 3 + 19 && !qcom_qmi_parse(out, n, &m));
  CHECK(m.type == QCOM_QMI_REQUEST && m.transaction == 7 && m.message == SERVREG_REGISTER_LISTENER && m.length == n - QCOM_QMI_HEADER);
  uint32_t size = 0;
  const uint8_t *e = qcom_qmi_tlv(&m, 0x01, &size);
  CHECK(e && size == 1 && e[0] == 1);
  const uint8_t *path = qcom_qmi_tlv(&m, 0x02, &size);
  CHECK(path && size == 19 && !std::memcmp(path, "msm/adsp/charger_pd", 19));
  // The response: success with the state up.
  const uint8_t resp[] = {2, 7, 0, 0x20, 0, 14, 0, 0x02, 4, 0, 0, 0, 0, 0, 0x10, 4, 0, 0xff, 0xff, 0xff, 0x1f};
  CHECK(!qcom_qmi_parse(resp, sizeof resp, &m));
  uint16_t result = 1, error = 1;
  CHECK(!servreg_result(&m, &result, &error) && result == 0 && servreg_listener_state(&m) == SERVREG_STATE_UP);
  const uint8_t refused[] = {2, 7, 0, 0x20, 0, 7, 0, 0x02, 4, 0, 1, 0, 0x10, 0};
  CHECK(!qcom_qmi_parse(refused, sizeof refused, &m) && servreg_result(&m, &result, &error) && result == 1 && error == 0x10);
  // An indication for our path, down, transaction 0x1234; and its acknowledgement.
  uint8_t ind[64] = {4, 3, 0, 0x22, 0};
  uint32_t at = QCOM_QMI_HEADER;
  const uint8_t down[4] = {0xff, 0xff, 0xff, 0x0f}, txn[2] = {0x34, 0x12};
  at = servreg_tlv(ind, at, sizeof ind, 0x01, down, 4);
  at = servreg_tlv(ind, at, sizeof ind, 0x02, "msm/adsp/charger_pd", 19);
  at = servreg_tlv(ind, at, sizeof ind, 0x03, txn, 2);
  ind[5] = (uint8_t)(at - QCOM_QMI_HEADER); ind[6] = 0;
  CHECK(!qcom_qmi_parse(ind, at, &m));
  uint32_t state = 0;
  uint16_t indTxn = 0;
  CHECK(!servreg_state_updated(&m, SERVREG_CHARGER_PD, &state, &indTxn) && state == SERVREG_STATE_DOWN && indTxn == 0x1234);
  CHECK(servreg_state_updated(&m, "msm/adsp/audio_pd", &state, &indTxn));   // another domain's: not ours
  const uint32_t a = servreg_set_ack(out, sizeof out, 8, SERVREG_CHARGER_PD, indTxn);
  CHECK(a && !qcom_qmi_parse(out, a, &m) && m.message == SERVREG_SET_ACK && m.transaction == 8);
  const uint8_t *t = qcom_qmi_tlv(&m, 0x02, &size);
  CHECK(t && size == 2 && t[0] == 0x34 && t[1] == 0x12);
  CHECK(!std::strcmp(servreg_state_name(SERVREG_STATE_UP), "up") && !std::strcmp(servreg_state_name(0), "unknown"));
  CHECK(servreg_register_listener(out, 10, 1, SERVREG_CHARGER_PD) == 0);   // too small: nothing built
}

// The port owner's enable answered or not, notifications with an SVID in the opcode's high half, and the read-only UCSI probe.
static void testPortOwners()
{
  PianoMca m;
  piano_mca_init(&m, 0, 0);
  piano_mca_link(&m, 1);
  uint8_t out[PIANO_MCA_PAN_REQUEST_BYTES];
  CHECK(piano_mca_pan_next(&m, out) == PIANO_MCA_PAN_REQUEST_BYTES && le32(out + 12) == PIANO_MCA_PAN_ENABLE && le32(out + 16) == 0);
  CHECK(le32(out + 20) == 0 && m.pan_enabled == 0);
  piano_mca_pan_timeout(&m);
  CHECK(m.pan_enabled == 2 && (piano_mca_take_events(&m) & McaEvPanSilent) && !piano_mca_pan_next(&m, out));
  piano_mca_link(&m, 0);
  piano_mca_link(&m, 1);
  CHECK(m.pan_enabled == 0 && piano_mca_pan_next(&m, out));
  uint8_t reply[16] = {};
  put32(reply, PIANO_MCA_OWNER_PAN); put32(reply + 4, PIANO_MCA_TYPE_REQUEST); put32(reply + 8, PIANO_MCA_PAN_WRITE);
  CHECK(piano_mca_receive(&m, reply, sizeof reply) == McaPanDone && m.pan_enabled == 1);
  CHECK(piano_mca_take_events(&m) & McaEvPanEnabled);
  uint8_t note[32] = {};
  put32(note, PIANO_MCA_OWNER_PAN); put32(note + 4, PIANO_MCA_TYPE_NOTIFY); put32(note + 8, 0xff010000u | PIANO_MCA_PAN_NOTIFY);
  CHECK(piano_mca_receive(&m, note, sizeof note) == McaPanNotified);   // the DP SVID in bits 31:16
  CHECK(piano_mca_pan_next(&m, out) && le32(out + 12) == PIANO_MCA_PAN_ACK && le32(out + 16) == 0);
  CHECK(piano_mca_receive(&m, reply, sizeof reply) == McaPanDone && !(piano_mca_take_events(&m) & McaEvPanEnabled));

  // UCSI: nothing without the allowance; then one read per link.
  uint8_t u[PIANO_MCA_UCSI_REQUEST_BYTES];
  CHECK(!piano_mca_ucsi_next(&m, u));
  piano_mca_allow_ucsi(&m, 1);
  piano_mca_link(&m, 0);
  piano_mca_link(&m, 1);
  CHECK(piano_mca_ucsi_next(&m, u) == PIANO_MCA_UCSI_REQUEST_BYTES && !piano_mca_ucsi_next(&m, u));
  CHECK(le32(u) == PIANO_MCA_OWNER_UCSI && le32(u + 4) == PIANO_MCA_TYPE_REQUEST && le32(u + 8) == PIANO_MCA_UCSI_READ);
  uint8_t r[12 + 48 + 4] = {};
  put32(r, PIANO_MCA_OWNER_UCSI); put32(r + 4, PIANO_MCA_TYPE_REQUEST); put32(r + 8, PIANO_MCA_UCSI_READ);
  r[12] = 0x00; r[13] = 0x01;            // VERSION 1.0
  put32(r + 16, 0x20000000u);            // CCI: busy
  put32(r + 12 + 48, 0);
  CHECK(piano_mca_receive(&m, r, 20) == McaIgnored);   // too short to hold a buffer and a return code
  CHECK(piano_mca_receive(&m, r, sizeof r) == McaUcsiDone);
  CHECK(m.ucsi == 3 && m.ucsi_version == 0x100 && m.ucsi_cci == 0x20000000u && m.ucsi_bytes == 48 && m.ucsi_ret == 0);
  CHECK(piano_mca_receive(&m, r, sizeof r) == McaIgnored);   // nothing asked any more
  uint8_t un[24] = {};
  put32(un, PIANO_MCA_OWNER_UCSI); put32(un + 4, PIANO_MCA_TYPE_NOTIFY); put32(un + 8, PIANO_MCA_UCSI_NOTIFY);
  CHECK(piano_mca_receive(&m, un, sizeof un) == McaUcsiNotified && m.ucsi_notifies == 1);
  piano_mca_link(&m, 0);
  piano_mca_link(&m, 1);
  CHECK(piano_mca_ucsi_next(&m, u));
  piano_mca_ucsi_timeout(&m);
  CHECK(m.ucsi == 4 && (piano_mca_take_events(&m) & McaEvUcsiSilent) && !piano_mca_ucsi_next(&m, u));

  // The firmware's port view is read every cycle.
  Dsp d;
  d.props[PIANO_MCA_BUS_VOLTAGE] = 5000000;
  PianoMca c;
  piano_mca_init(&c, 0, 0);
  piano_mca_link(&c, 1);
  d.run(c);
  CHECK(d.readsOf[PIANO_MCA_PD_STATE] == 1 && d.readsOf[PIANO_MCA_DATA_ROLE] == 1 && d.readsOf[PIANO_MCA_OTG_PLUGIN] == 1);
  CHECK(d.readsOf[PIANO_MCA_CC_TOGGLE] == 1 && d.writes.empty());
}

static void testMcaHv()
{
  // A PD source with 5 V/3 A and 9 V/3 A: 900 mA... then the 5 V PDO's limit, 9 V after three cycles, then 1.6 A.
  PianoMca m;
  piano_mca_init(&m, 1, 1);
  piano_mca_link(&m, 1);
  Dsp d;
  d.props[PIANO_MCA_BUS_VOLTAGE] = 5000000;
  d.props[PIANO_MCA_REAL_TYPE] = PIANO_MCA_TYPE_PD;
  d.props[PIANO_MCA_ICL] = 100;
  d.props[PIANO_MCA_TBAT] = 28;
  d.props[PIANO_MCA_CHARGE_ENABLE] = 1;
  d.props[PIANO_MCA_VERIFY] = 0;
  d.pdo = {5000, 5000, 3000, 9000, 9000, 3000};
  d.props[0xf9000] = 8900000;   // the model's bus after a 9 V request
  d.run(m);   // the attach cycle and the read-back after its writes
  CHECK(d.readsOf[PIANO_MCA_PDOS] >= 1 && m.pdos == 2 && piano_mca_pdo_fixed(&m, 9000) == 3000 && piano_mca_pdo_fixed(&m, 5000) == 3000);
  CHECK(d.last(PIANO_MCA_ICL) == 1500 && d.count(PIANO_MCA_PD_FIXED) == 0 && m.cycles == 2);
  cycle(m, d);   // third cycle since attach: the request, then the read-back that sees 9 V
  CHECK(d.count(PIANO_MCA_PD_FIXED) == 1 && d.last(PIANO_MCA_PD_FIXED) == 9000);
  CHECK(m.hv == McaHvOn && d.last(PIANO_MCA_ICL) == 1600 && d.last(PIANO_MCA_FCC) == 1500);
  // The source drops back to 5 V on its own: 9 V blocked, the 5 V limit again.
  d.props[PIANO_MCA_BUS_VOLTAGE] = 5000000;
  cycle(m, d);
  CHECK(m.hv == McaHvBlocked && d.last(PIANO_MCA_ICL) == 1500);
  // A source that never reaches 9 V: asked once, then back to 5 V.
  PianoMca n;
  piano_mca_init(&n, 1, 1);
  piano_mca_link(&n, 1);
  Dsp e = d;
  e.writes.clear();
  e.props.erase(0xf9000);
  e.props[PIANO_MCA_BUS_VOLTAGE] = 5000000;
  for (int i = 0; i < 8; ++i) {piano_mca_refresh(&n); e.run(n);}
  CHECK(e.count(PIANO_MCA_PD_FIXED) == 2 && e.last(PIANO_MCA_PD_FIXED) == 5000 && n.hv == McaHvBlocked);
  // Without piano-power-hv the request is never made.
  PianoMca o;
  piano_mca_init(&o, 1, 0);
  piano_mca_link(&o, 1);
  Dsp f = d;
  f.writes.clear();
  for (int i = 0; i < 6; ++i) {piano_mca_refresh(&o); f.run(o);}
  CHECK(f.count(PIANO_MCA_PD_FIXED) == 0);
  // An authentication in progress holds the request back.
  PianoMca q;
  piano_mca_init(&q, 1, 1);
  piano_mca_link(&q, 1);
  Dsp g = d;
  g.writes.clear();
  g.props[PIANO_MCA_VERIFY] = 1;
  for (int i = 0; i < 6; ++i) {piano_mca_refresh(&q); g.run(q);}
  CHECK(g.count(PIANO_MCA_PD_FIXED) == 0);
}

// ------------------------------------------------------------------------------------- ChargePolicy wired to the codec (ChargeLink.h)
// The owner's side as PianoPower runs it: the codec linked after charger_pd, the DSP model answering, the link stepped after each
// pass, a resync answered by a relink. The defaults are the 2026-11-11 evening run's Mac port: CDP at 5.23 V, Rp 3.0 A, 97 %, 29 C, the firmware's
// 100 mA requested limit.
struct LinkRig {
  Dsp d;
  PianoMca m{};
  ChargeLink link;
  LinkOptions opts;
  GaugeReading packs[2];
  PortState port;
  piano_board_temps board{};
  bool havePort = true, haveBoard = false, pdUp = true, packsFresh = true, sleep = false, shutdown = false;
  uint32_t wakes = 0;
  uint64_t now = 0;
  unsigned relinks = 0, handoverReady = 0, refused = 0, waitingPd = 0, waitingBoard = 0;
  uint32_t events = 0;              // every decision's events since the last take()
  std::vector<LinkReadback> readbacks;

  explicit LinkRig(LinkOptions o, bool boardCfg = true) : opts(o)
  {
    piano_mca_init(&m, o.writes ? 1 : 0, o.hv != 0);
    piano_mca_allow_board(&m, boardCfg);
    link.configure(m, o);
    piano_mca_link(&m, 1);
    // as PianoPower's receive loop: each write answer to the policy, and a decision after each finished cycle
    d.hook = [](void *ctx, PianoMca &mm, int r) {
      LinkRig *rig = static_cast<LinkRig *>(ctx);
      if (r == McaWritten) rig->link.answered(mm);
      if (r == McaCycleDone) rig->stepOnce();
    };
    d.hookCtx = this;
    d.props[PIANO_MCA_BUS_VOLTAGE] = 5231000;
    d.props[PIANO_MCA_BUS_CURRENT] = 397000;
    d.props[PIANO_MCA_REAL_TYPE] = PIANO_MCA_TYPE_CDP;
    d.props[PIANO_MCA_ICL] = 100;
    d.props[PIANO_MCA_TBAT] = 29;
    d.props[PIANO_MCA_CHARGE_ENABLE] = 1;
    d.props[PIANO_MCA_TERM_VOLT] = 5200;
    d.props[PIANO_MCA_VERIFY] = 0;
    for (GaugeReading &g : packs) {
      g.ok = true;
      g.voltageMv = 4450;
      g.currentMa = -800;
      g.socPct = 97;
      g.tempDeciC = 290;
    }
    port.ok = port.attached = port.sourceSeen = true;
    port.cc = 1;
    port.rpBits = 0x2;   // 3.0 A, as the Mac's port
    board.version = PIANO_BOARD_TEMPS_VERSION;
    board.state = PIANO_BOARD_TEMPS_RUNNING;
    temps(30, 33, 31, 30);
  }
  void temps(int battery, int charger, int conn, int conn2)
  {
    const int c[4] = {battery, charger, conn, conn2};
    board.valid = 0;
    for (unsigned i = 0; i < 4; ++i) {
      board.milli_c[i] = c[i] * 1000;
      if (c[i] > -100) board.valid |= 1u << i;   // -100: that channel does not read
    }
  }
  void soc(int pct) { packs[0].socPct = pct; packs[1].socPct = pct - 1; }
  LinkView view() const
  {
    LinkView v;
    v.nowMs = now;
    v.chargerPdUp = pdUp;
    v.packs = packs;
    v.packsFresh = packsFresh;
    v.port = havePort ? &port : nullptr;
    v.board = haveBoard ? &board : nullptr;
    v.wakes = wakes;
    v.willSleep = sleep;
    v.shutdown = shutdown;
    return v;
  }
  LinkOutcome stepOnce()
  {
    LinkOutcome o;
    link.step(m, view(), o);
    events |= o.events;
    relinks += o.resync;
    handoverReady += o.handoverReady;
    refused += o.refused;
    waitingPd += o.waitingPd;
    waitingBoard += o.waitingBoard;
    for (unsigned i = 0; i < o.readbacks; ++i) readbacks.push_back(o.readback[i]);
    if (o.resync) {piano_mca_link(&m, 0); piano_mca_link(&m, 1);}
    return o;
  }
  // Until nothing moves: the model answers everything, the link decides and queues, a resync relinks.
  void settle()
  {
    for (int guard = 0; guard < 40; ++guard) {
      const unsigned n = d.run(m);
      const LinkOutcome o = stepOnce();
      if (!n && !o.queued && !o.resync && !o.decided) return;
    }
    CHECK(!"the link never settled");
  }
  void period(uint64_t dt = 5000) { now += dt; piano_mca_refresh(&m); settle(); }
  uint32_t take() { const uint32_t e = events; events = 0; return e; }
  int index(uint32_t property, unsigned from = 0) const
  {
    for (size_t i = from; i < d.writes.size(); ++i) if (d.writes[i].property == property) return int(i);
    return -1;
  }
  // Every write the model saw is one the codec's allowlist admits, and within the stage A ceilings.
  bool allAllowed() const
  {
    for (const Dsp::Write &w : d.writes) {
      const uint8_t bytes = w.property == PIANO_MCA_CHARGE_ENABLE || w.property == PIANO_MCA_CP_STATE ? 1
                            : w.property == PIANO_MCA_OTG_CFG ? 16 : 4;
      if (!piano_mca_write_allowed_for(&m, w.property, w.value, bytes)) return false;
      if (w.property == PIANO_MCA_ICL && (w.value < 100 || w.value > 1600)) return false;
      if (w.property == PIANO_MCA_FCC && (w.value < 100 || w.value > 1500)) return false;
      if (w.property == PIANO_MCA_CHARGE_ENABLE && w.value == 0 && !link.cfg.limit) return false;
      if (w.property == PIANO_MCA_INPUT_SUSPEND || w.property == PIANO_MCA_TERM_VOLT) return false;
    }
    return true;
  }
};

static void testLinkCdp()
{
  // That Mac port, the default options: the board configuration, then the CDP row (900 / 900) whatever the Rp says.
  LinkRig r{LinkOptions()};
  r.period();
  const int cp = r.index(PIANO_MCA_CP_STATE), cid = r.index(PIANO_MCA_CID_CFG), fcc = r.index(PIANO_MCA_FCC), icl = r.index(PIANO_MCA_ICL);
  CHECK(cp >= 0 && cid >= 0 && r.index(PIANO_MCA_OTG_CFG) >= 0 && fcc > cid && icl > fcc);   // board first, then FCC, then ICL
  CHECK(r.d.last(PIANO_MCA_ICL) == 900 && r.d.last(PIANO_MCA_FCC) == 900 && r.d.count(PIANO_MCA_ICL) == 1);
  CHECK(r.link.plan.cls == PIANO_CHG_C_CDP && r.link.plan.icl_why == PIANO_CHG_ICL_ROW && r.link.plan.rp_ma == 0);
  CHECK(!r.d.count(PIANO_MCA_CHARGE_ENABLE) && !r.d.count(PIANO_MCA_PD_FIXED) && !r.d.count(PIANO_MCA_INPUT_SUSPEND));
  CHECK(r.take() & PIANO_CHG_EV_ATTACH);
  CHECK(r.m.icl_set == 900 && r.m.fcc_set == 900);   // what the 3-minute warning and the mca line read
  // the input limit read back on the next cycle, as written
  bool readBack = false;
  for (const LinkReadback &b : r.readbacks) readBack |= b.property == PIANO_MCA_ICL && b.readable && b.agrees && b.read == 900;
  CHECK(readBack);
  const size_t settled = r.d.writes.size();
  for (int i = 0; i < 4; ++i) r.period();
  CHECK(r.d.writes.size() == settled);                // nothing more while it reads back as written
  r.d.pan(r.m);                                       // the port enable answered
  r.period();
  CHECK(piano_mca_configured(&r.m));                  // the quiet flag can be released
  // the firmware loses the limit: one disagreeing cycle is tolerated, the second writes it again
  r.d.props[PIANO_MCA_ICL] = 100;
  r.period();
  CHECK(r.d.count(PIANO_MCA_ICL) == 1);
  r.period();
  CHECK(r.d.count(PIANO_MCA_ICL) == 2 && r.d.last(PIANO_MCA_ICL) == 900);
  CHECK(r.allAllowed() && r.refused == 0 && r.waitingPd == 0);
  // piano-power-write=0: the decisions for the log, nothing sent, the codec refuses everything
  LinkOptions ro;
  ro.writes = false;
  LinkRig w(ro);
  for (int i = 0; i < 3; ++i) w.period();
  CHECK(w.d.writes.empty() && w.link.plan.icl_ma == 900 && w.link.decisions >= 3);
  CHECK(piano_mca_queue(&w.m, PIANO_MCA_ICL, 900, 4) == -1);
  // without the external policy the queue is closed (the built-in policy keeps its own path)
  PianoMca plain;
  piano_mca_init(&plain, 1, 0);
  CHECK(piano_mca_queue(&plain, PIANO_MCA_ICL, 900, 4) == -1);
}

static void testLinkRp()
{
  LinkOptions o;
  o.rp = true;
  // Rp 3.0 A with the connectors reading 31 / 30 C: 1600 mA (the ceiling), the charge current still the CDP row
  {
    LinkRig r(o);
    r.haveBoard = true;
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 1600 && r.d.last(PIANO_MCA_FCC) == 900);
    CHECK(r.link.plan.icl_why == PIANO_CHG_ICL_RP && r.link.plan.rp_ma == 3000);
    r.port.rpBits = 0x4;   // 1.5 A
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 1500);
    r.port.rpBits = 0x8;   // the default: nothing above the row
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 900 && r.link.plan.icl_why == PIANO_CHG_ICL_ROW);
    r.port.rpBits = 0x2;
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 1600);
    r.take();
    // the bus sags under it: the trust ends for the boot
    r.d.props[PIANO_MCA_BUS_VOLTAGE] = 4400000;
    r.period();
    CHECK((r.take() & PIANO_CHG_EV_RP_DISTRUST) && r.d.last(PIANO_MCA_ICL) == 900);
    r.d.props[PIANO_MCA_BUS_VOLTAGE] = 5200000;
    r.period();
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 900 && r.link.st.rp_distrust);
    CHECK(r.allAllowed());
  }
  // no PianoThermal: unknown is never cool, the row and one note
  {
    LinkRig r(o);
    r.period();
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 900 && (r.take() & PIANO_CHG_EV_RP_WITHHELD));
    r.period();
    CHECK(!(r.take() & PIANO_CHG_EV_RP_WITHHELD));   // once per attach
  }
  // a warm connector (52 C): the row; one connector channel lost, the other cool: the Rp limit
  {
    LinkRig r(o);
    r.haveBoard = true;
    r.temps(30, 33, 52, 30);
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 900);
    r.temps(30, 33, 31, -100);
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 1600);
    // no Type-C reading: the row
    r.havePort = false;
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 900);
  }
  // a PD source: its contract governs, the Rp is not used
  {
    LinkRig r(o);
    r.haveBoard = true;
    r.d.props[PIANO_MCA_REAL_TYPE] = PIANO_MCA_TYPE_PD;
    r.d.pdo = {5000, 5000, 1200, 9000, 9000, 2000};
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 1200 && r.link.plan.icl_why == PIANO_CHG_ICL_ROW);
  }
  // the source goes twice within 30 s of an Rp limit taking (a port's over-current cut): distrusted
  {
    LinkRig r(o);
    r.haveBoard = true;
    for (int k = 0; k < 2; ++k) {
      r.d.props[PIANO_MCA_BUS_VOLTAGE] = 5200000;
      r.period();
      CHECK(r.d.last(PIANO_MCA_ICL) == 1600);
      r.d.props[PIANO_MCA_BUS_VOLTAGE] = 200000;
      r.period();
    }
    CHECK((r.take() & PIANO_CHG_EV_RP_DISTRUST) && r.link.st.rp_distrust);
    r.d.props[PIANO_MCA_BUS_VOLTAGE] = 5200000;
    r.period();
    CHECK(r.d.last(PIANO_MCA_ICL) == 900);
  }
}

static void testLinkPd()
{
  // 5 V/3 A + 9 V/2 A, piano-power-hv=1: the 5 V offer (capped 1500) first, 9 V 15 s after the attach, then 1600 mA
  LinkOptions o;
  o.hv = 1;
  LinkRig r(o);
  r.d.props[PIANO_MCA_REAL_TYPE] = PIANO_MCA_TYPE_PD;
  r.d.pdo = {5000, 5000, 3000, 9000, 9000, 2000};
  r.d.props[0xf9000] = 8900000;
  r.d.props[0xf5000] = 5100000;
  r.period();
  CHECK(r.d.last(PIANO_MCA_ICL) == 1500 && r.d.last(PIANO_MCA_FCC) == 1500 && !r.d.count(PIANO_MCA_PD_FIXED));
  for (int i = 0; i < 4; ++i) r.period();
  CHECK(r.d.count(PIANO_MCA_PD_FIXED) == 1 && r.d.last(PIANO_MCA_PD_FIXED) == 9000);
  r.period();
  CHECK(r.link.plan.hv == PIANO_CHG_HV_ON && r.d.last(PIANO_MCA_ICL) == 1600 && r.link.plan.icl_why == PIANO_CHG_ICL_9V);
  bool asked = false;
  for (const LinkReadback &b : r.readbacks) asked |= b.property == PIANO_MCA_PD_FIXED && b.agrees && b.read == 8900;
  CHECK(asked && r.allAllowed());
  // without piano-power-hv the codec refuses a 9 V request even if one were planned
  LinkRig f{LinkOptions()};
  CHECK(piano_mca_queue(&f.m, PIANO_MCA_PD_FIXED, 9000, 4) == -1 && piano_mca_queue(&f.m, PIANO_MCA_PD_FIXED, 5000, 4) == 0);
  CHECK(piano_mca_queue(&f.m, PIANO_MCA_PPS, (450u << 16) | 60u, 4) == -1);   // the APDO request only with hv 2
}

static void testLinkDetachWakeGeneration()
{
  LinkRig r{LinkOptions()};
  r.period();
  r.period();
  // detach: the firmware's own values back where written, once; a new attach sends the row again
  r.d.props[PIANO_MCA_BUS_VOLTAGE] = 300000;
  r.period();
  CHECK((r.take() & PIANO_CHG_EV_DETACH) && r.d.last(PIANO_MCA_ICL) == 100 && r.d.last(PIANO_MCA_FCC) == 500);
  const size_t gone = r.d.writes.size();
  r.period();
  CHECK(r.d.writes.size() == gone);
  r.d.props[PIANO_MCA_BUS_VOLTAGE] = 5200000;
  r.period();
  CHECK(r.d.last(PIANO_MCA_ICL) == 900 && r.d.last(PIANO_MCA_FCC) == 900 && r.d.count(PIANO_MCA_ICL) == 3);
  // a wake: the codec relinked (board configuration again), then everything sent again although the firmware kept it
  r.take();
  r.wakes++;
  r.period();
  r.period();
  CHECK(r.relinks == 1 && (r.take() & PIANO_CHG_EV_WAKE));
  CHECK(r.d.count(PIANO_MCA_CP_STATE) == 2 && r.d.count(PIANO_MCA_CID_CFG) == 2);
  CHECK(r.d.count(PIANO_MCA_ICL) == 4 && r.d.count(PIANO_MCA_FCC) == 4 && r.index(PIANO_MCA_ICL, size_t(r.index(PIANO_MCA_CP_STATE, 1))) > 0);
  // a new charger generation (charger_pd up again: PianoPower links the codec afresh): the same without a resync
  const unsigned relinks = r.relinks;
  piano_mca_link(&r.m, 0);
  piano_mca_link(&r.m, 1);
  r.d.props[PIANO_MCA_ICL] = 100;   // the firmware is on its defaults
  r.settle();
  r.period();
  CHECK((r.take() & PIANO_CHG_EV_GENERATION) && r.relinks == relinks);
  CHECK(r.d.count(PIANO_MCA_CP_STATE) == 3 && r.d.count(PIANO_MCA_ICL) == 5 && r.d.last(PIANO_MCA_ICL) == 900);
  CHECK(r.allAllowed());
}

static void testLinkGates()
{
  // charger_pd not up (the codec linked on the 60 s fallback): the board configuration is the codec's, no policy write
  LinkRig r{LinkOptions()};
  r.pdUp = false;
  for (int i = 0; i < 3; ++i) r.period();
  CHECK(r.d.count(PIANO_MCA_CP_STATE) == 1 && !r.d.count(PIANO_MCA_ICL) && !r.d.count(PIANO_MCA_FCC) && r.waitingPd == 1);
  r.pdUp = true;
  r.period();
  CHECK(r.d.last(PIANO_MCA_ICL) == 900);
  // the board configuration refused: five cycles of it, the policy only after it gave up
  LinkRig b{LinkOptions()};
  b.d.retcodeFor[PIANO_MCA_CID_CFG] = 5;
  for (int i = 0; i < 8; ++i) b.period();
  const int lastCid = [&] { int k = -1; for (size_t i = 0; i < b.d.writes.size(); ++i) if (b.d.writes[i].property == PIANO_MCA_CID_CFG) k = int(i); return k; }();
  CHECK(b.d.count(PIANO_MCA_CID_CFG) == 5 && b.index(PIANO_MCA_ICL) > lastCid && b.waitingBoard == 1);
  // a write that gets no answer: the policy hears -60 (a refusal) and plans it again on the next decision
  LinkRig t{LinkOptions()};
  t.period();
  t.d.props[PIANO_MCA_ICL] = 100;
  t.period();                        // the first cycle that reads 100
  auto hook = t.d.hook;
  t.d.hook = nullptr;                // the owner's loop by hand from here
  t.now += 5000;
  piano_mca_refresh(&t.m);
  t.d.run(t.m);                      // the second
  const LinkOutcome o = t.stepOnce();   // plans the input limit again
  CHECK(o.queued == 1);
  uint8_t out[PIANO_MCA_REQUEST_BYTES];
  CHECK(piano_mca_next(&t.m, out) && le32(out + 8) == PIANO_MCA_OP_WRITE && le32(out + 12) == PIANO_MCA_ICL);
  t.link.timeout(t.m);               // no answer in 1 s
  CHECK(t.m.last_result == -60 && t.link.st.tries[PIANO_CHG_S_ICL] == 1 && !(t.link.st.pending & (1u << PIANO_CHG_S_ICL)));
  t.d.hook = hook;
  t.settle();
  t.period();
  CHECK(t.d.last(PIANO_MCA_ICL) == 900);
}

static void testLinkHealth()
{
  LinkOptions o;
  o.health = true;
  // 85 %: the stop (0x2003 = 0) before the limits, no hold; it reads back 0
  {
    LinkRig r(o);
    r.soc(85);
    r.period();
    const int stop = r.index(PIANO_MCA_CHARGE_ENABLE);
    CHECK(stop >= 0 && r.d.writes[size_t(stop)].value == 0 && stop < r.index(PIANO_MCA_FCC) && stop < r.index(PIANO_MCA_ICL));
    CHECK(!r.link.hold && r.link.plan.why_off == PIANO_CHG_OFF_LIMIT && (r.take() & PIANO_CHG_EV_LIMIT_STOP));
    bool stopped = false;
    for (const LinkReadback &b : r.readbacks) stopped |= b.property == PIANO_MCA_CHARGE_ENABLE && b.agrees && b.read == 0;
    CHECK(stopped);
    r.soc(78);
    r.period();
    CHECK(r.d.count(PIANO_MCA_CHARGE_ENABLE) == 1);   // between the thresholds nothing changes
    r.soc(75);
    r.period();
    CHECK(r.d.last(PIANO_MCA_CHARGE_ENABLE) == 1 && (r.take() & PIANO_CHG_EV_LIMIT_RESUME) && r.link.hold);
    // the packs not read lately: no hold (the charge is not known)
    r.packsFresh = false;
    r.period();
    CHECK(!r.link.hold);
    CHECK(r.allAllowed());
  }
  // battery health off: never a 0, and the codec would refuse one
  {
    LinkRig r{LinkOptions()};
    r.soc(95);
    for (int i = 0; i < 3; ++i) r.period();
    CHECK(!r.d.count(PIANO_MCA_CHARGE_ENABLE) && !r.link.hold);
    CHECK(piano_mca_queue(&r.m, PIANO_MCA_CHARGE_ENABLE, 0, 1) == -1);
    LinkRig h(o);
    CHECK(piano_mca_queue(&h.m, PIANO_MCA_CHARGE_ENABLE, 0, 1) == 0);
  }
  // thresholds out of range: refused, the mode stays off
  {
    LinkOptions bad = o;
    bad.stopPct = 30;
    LinkRig r(bad);
    CHECK(r.link.healthRefused && !r.link.cfg.limit && !r.m.stop_allowed);
  }
}

static void testLinkSleepShutdown()
{
  LinkOptions o;
  o.health = true;
  LinkRig r(o);
  r.soc(60);
  r.period();
  r.period();
  CHECK(r.link.hold && !r.d.count(PIANO_MCA_CHARGE_ENABLE));
  // the system sleeps: the protected charge stopped for the sleep, the CDP row kept, ready once the firmware took it
  r.sleep = true;
  r.settle();
  CHECK(r.d.last(PIANO_MCA_CHARGE_ENABLE) == 0 && !r.link.hold && r.handoverReady >= 1 && (r.take() & PIANO_CHG_EV_SLEEP));
  CHECK(r.d.last(PIANO_MCA_ICL) == 900);
  // awake: relinked, the stop lifted, everything sent again, the hold back
  r.sleep = false;
  r.wakes++;
  r.handoverReady = 0;
  r.period();
  r.period();
  CHECK(r.relinks == 1 && r.d.last(PIANO_MCA_CHARGE_ENABLE) == 1 && r.link.hold);
  // nothing attached: the handover is ready at once
  LinkRig n(o);
  n.d.props[PIANO_MCA_BUS_VOLTAGE] = 0;
  n.period();
  n.sleep = true;
  n.settle();
  CHECK(n.handoverReady >= 1 && !n.d.count(PIANO_MCA_CHARGE_ENABLE));
  // charger_pd not up: nothing can be sent, ready at once
  LinkRig p(o);
  p.pdUp = false;
  p.sleep = true;
  p.settle();
  CHECK(p.handoverReady >= 1);
  // shutdown with our stop in force: lifted
  LinkRig s(o);
  s.soc(90);
  s.period();
  CHECK(s.d.last(PIANO_MCA_CHARGE_ENABLE) == 0);
  s.shutdown = true;
  s.settle();
  CHECK(s.d.last(PIANO_MCA_CHARGE_ENABLE) == 1 && s.handoverReady >= 1 && !s.link.hold);
  CHECK(r.allAllowed() && s.allAllowed());
}

static void testLinkThermal()
{
  LinkOptions o;
  o.rp = true;
  LinkRig r(o);
  r.haveBoard = true;
  r.period();
  CHECK(r.d.last(PIANO_MCA_ICL) == 1600);
  // a connector at 61 C: 100 mA until both read below 50; one of them lost meanwhile keeps the hold (unknown, never cool)
  r.temps(30, 33, 61, 40);
  r.period();
  CHECK((r.take() & PIANO_CHG_EV_CONN_HOT) && r.d.last(PIANO_MCA_ICL) == 100 && r.link.plan.icl_why == PIANO_CHG_ICL_CONN);
  r.temps(30, 33, 45, -100);
  r.period();
  CHECK(r.d.last(PIANO_MCA_ICL) == 100);
  r.temps(30, 33, 45, 44);
  r.period();
  CHECK((r.take() & PIANO_CHG_EV_CONN_COOL) && r.d.last(PIANO_MCA_ICL) == 1600);
  // the charger at 86 C: 500 mA at most until below 75
  r.temps(30, 86, 40, 40);
  r.period();
  CHECK((r.take() & PIANO_CHG_EV_CHGR_HOT) && r.d.last(PIANO_MCA_ICL) == 500);
  r.temps(30, 76, 40, 40);
  r.period();
  CHECK(r.d.last(PIANO_MCA_ICL) == 500);
  r.temps(30, 74, 40, 40);
  r.period();
  CHECK((r.take() & PIANO_CHG_EV_CHGR_COOL) && r.d.last(PIANO_MCA_ICL) == 1600);
  // the battery thermistor at 45 C with the packs at 29: the charge current derated at once (900 - 7 x 300 -> the 500 floor), noted
  r.temps(45, 33, 31, 30);
  r.period();
  CHECK((r.take() & PIANO_CHG_EV_NTC_APART) && r.d.last(PIANO_MCA_FCC) == 500);
  // PianoThermal gone quiet (no valid bits): the firmware's and the gauges' temperatures still count, back up one step per 30 s
  r.board.valid = 0;
  for (int i = 0; i < 14; ++i) r.period();
  CHECK(r.d.last(PIANO_MCA_FCC) == 900);
  CHECK(r.d.last(PIANO_MCA_ICL) == 900);   // and no connector reading: no Rp limit
  CHECK(r.allAllowed());
}

// A random walk of the owner's world: every write the model sees must pass the codec's allowlist and the ceilings, a stop only with
// battery health, and nothing before charger_pd and the board configuration.
static void testLinkRandom()
{
  uint64_t x = 0x9e3779b97f4a7c15ull;
  auto rnd = [&](unsigned n) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return unsigned(x % n); };
  for (int run = 0; run < 12; ++run) {
    LinkOptions o;
    o.hv = rnd(3);
    o.rp = rnd(2);
    o.health = rnd(2);
    o.writes = rnd(6) != 0;
    LinkRig r(o);
    r.haveBoard = rnd(2);
    r.d.props[0xf9000] = 8900000;
    r.d.props[0xf5000] = 5100000;
    for (int i = 0; i < 300; ++i) {
      switch (rnd(24)) {
      case 0: r.d.props[PIANO_MCA_BUS_VOLTAGE] = rnd(2) ? 5200000 : 100000; break;
      case 1: r.d.props[PIANO_MCA_REAL_TYPE] = rnd(16); break;
      case 2: r.d.pdo = rnd(2) ? std::vector<uint32_t>{5000, 5000, 3000, 9000, 9000, 2000} : std::vector<uint32_t>{5000, 5000, 1500}; break;
      case 3: r.port.rpBits = uint8_t(1u << rnd(4)); break;
      case 4: r.temps(int(rnd(70)), int(rnd(100)), int(rnd(80)) - 10, rnd(4) ? int(rnd(80)) : -100); break;
      case 5: r.wakes++; break;
      case 6: piano_mca_link(&r.m, 0); piano_mca_link(&r.m, 1); break;
      case 7: r.sleep = !r.sleep; break;
      case 8: r.pdUp = rnd(5) != 0; break;
      case 9: r.soc(int(rnd(60)) + 40); break;
      case 10: r.d.props[PIANO_MCA_BUS_VOLTAGE] = 4000000 + rnd(600000); break;
      case 11: r.havePort = !r.havePort; break;
      case 12: r.packsFresh = !r.packsFresh; break;
      case 13: r.d.props[PIANO_MCA_ICL] = rnd(2000); break;
      case 14: if (rnd(2)) r.d.retcodeFor[PIANO_MCA_ICL] = 5; else r.d.retcodeFor.erase(PIANO_MCA_ICL); break;
      default: break;
      }
      r.period(rnd(4) ? 5000 : rnd(60000));
      if (!o.writes) CHECK(r.d.writes.empty());
      if (r.sleep) CHECK(!r.link.hold);
    }
    CHECK(r.allAllowed());
  }
}

// ------------------------------------------------------------------------------------------------------------- PMIC RTC
// The count as RDATA shows it during a read: `ticks` lists the counter value at each register read (the four bytes of one
// read come from the value current when that byte is read, so a carry between them tears the value).
struct TickingRtc {
  std::vector<uint32_t> perByte;   // the counter's value when byte k of all reads so far is read
  unsigned bytesRead = 0, reads = 0;
  bool fail = false;
  static int rd(void *ctx, uint8_t reg, uint8_t *value, unsigned n)
  {
    TickingRtc *t = static_cast<TickingRtc *>(ctx);
    t->reads++;
    if (t->fail || reg != PIANO_RTC_RDATA || n == 0 || n > 4) return -1;
    for (unsigned b = 0; b < n; ++b) {
      const uint32_t v = t->perByte[std::min<size_t>(t->bytesRead++, t->perByte.size() - 1)];
      value[b] = uint8_t(v >> (8 * b));
    }
    return 0;
  }
};

static void testRtc()
{
  // Dates against the host's gmtime_r: the edges, the leap days, 2100 (no leap day), the last second of 2^32, and a sweep.
  auto same = [](uint32_t utc) {
    piano_rtc_date d;
    piano_rtc_civil(utc, &d);
    const time_t t = time_t(utc);
    struct tm tm;
    gmtime_r(&t, &tm);
    return d.year == uint32_t(tm.tm_year + 1900) && d.month == uint32_t(tm.tm_mon + 1) && d.day == uint32_t(tm.tm_mday) &&
           d.hour == uint32_t(tm.tm_hour) && d.minute == uint32_t(tm.tm_min) && d.second == uint32_t(tm.tm_sec);
  };
  const uint32_t edges[] = {0u, 59u, 60u, 86399u, 86400u, 68255999u, 68256000u, 951782399u, 951782400u, 951868800u, 1767225599u,
                            1767225600u, 1794355200u, 4102444799u, 4102444800u, 4107542399u, 4107542400u, 0xfffffffeu, 0xffffffffu};
  for (uint32_t e : edges) CHECK(same(e));
  unsigned mismatches = 0;
  for (uint64_t u = 0; u <= 0xffffffffull; u += 65521) mismatches += !same(uint32_t(u));
  CHECK(mismatches == 0);
  piano_rtc_date d;
  piano_rtc_civil(951782400u, &d);
  CHECK(d.year == 2000 && d.month == 2 && d.day == 29 && d.hour == 0);
  piano_rtc_civil(0xffffffffu, &d);
  CHECK(d.year == 2106 && d.month == 2 && d.day == 7 && d.hour == 6 && d.minute == 28 && d.second == 15);

  // UTC = count + offset modulo 2^32; the plausible window.
  CHECK(piano_rtc_utc(1000u, 1794354000u) == 1794355000u);
  CHECK(piano_rtc_utc(0xffffff00u, 0x200u) == 0x100u);                 // an offset "below zero" (count ahead of UTC)
  CHECK(piano_rtc_utc(1794355000u, uint32_t(-1000)) == 1794354000u);
  CHECK(!piano_rtc_plausible(0) && !piano_rtc_plausible(1767225599u) && piano_rtc_plausible(1767225600u));
  CHECK(piano_rtc_plausible(4102444799u) && !piano_rtc_plausible(4102444800u));
  const uint8_t le[4] = {0x78, 0x56, 0x34, 0x12};
  CHECK(piano_rtc_le32(le) == 0x12345678u);

  // The read: no carry, two register reads; a carry between the bytes (0x1ff -> 0x200 after byte 0), the low byte read again is
  // smaller, the third read gives the settled count; a failing channel.
  TickingRtc still;
  still.perByte = {0x01020304u};
  uint32_t count = 0;
  CHECK(piano_rtc_read_count(TickingRtc::rd, &still, &count) == 0 && count == 0x01020304u && still.reads == 2);
  TickingRtc torn;
  torn.perByte = {0x1ffu, 0x200u};   // byte 0 of the first read sees 0x1ff, everything after sees 0x200
  CHECK(piano_rtc_read_count(TickingRtc::rd, &torn, &count) == 0 && count == 0x200u && torn.reads == 3);
  TickingRtc fullCarry;
  fullCarry.perByte = {0x00ffffffu, 0x01000000u};
  CHECK(piano_rtc_read_count(TickingRtc::rd, &fullCarry, &count) == 0 && count == 0x01000000u && fullCarry.reads == 3);
  TickingRtc broken;
  broken.perByte = {1u};
  broken.fail = true;
  CHECK(piano_rtc_read_count(TickingRtc::rd, &broken, &count) != 0);

  // The readout over the arbiter model: RTC_RW at SID 0 0x6100 (an APID owned elsewhere first, EE 0's wins), the SDAM at 0x7100.
  Arb a;
  a.add(7, 0x2200, 0x02, 0xba, 0);
  a.add(0, 0x6100, 0x0b, 0x07, 3);
  const int rtc = a.add(0, 0x6100, 0x0b, 0x07, 0);
  a.add(0, 0x6200, 0x0b, 0x08, 0);
  const int sdam = a.add(0, 0x7100, 0x2e, 0x00, 0);
  const uint32_t raw = 85432100u, cell = 1794355200u - raw;
  a.apids[rtc].regs[PIANO_RTC_EN_CTL] = 0x80;
  for (unsigned b = 0; b < 4; ++b) {
    a.apids[rtc].regs[PIANO_RTC_RDATA + b] = uint8_t(raw >> (8 * b));
    a.apids[sdam].regs[PIANO_RTC_SDAM_OFFSET + b] = uint8_t(cell >> (8 * b));
  }
  const Io io = a.io();
  const Arbiter arb = spmiArbiter(io);
  RtcReading r = readRtc(io, arb, false, 0);
  CHECK(r.ok && r.rtc.apid == rtc && r.rtc.owner == 0 && r.sdam.apid == sdam && r.type == 0x0b && r.subtype == 0x07);
  CHECK(r.enabled && r.count == raw && r.sdamOk && r.sdamOffset == cell);
  CHECK(r.source == RtcOffsetSource::sdam && r.offset == cell && r.utc == 1794355200u && r.dated);
  // The argument wins over the cell; an offset that gives no date is reported as such; the counter stopped.
  r = readRtc(io, arb, true, 1794355200u - raw + 7, &r);
  CHECK(r.source == RtcOffsetSource::arg && r.utc == 1794355207u && r.dated);
  for (unsigned b = 0; b < 4; ++b) a.apids[sdam].regs[PIANO_RTC_SDAM_OFFSET + b] = 0;
  r = readRtc(io, arb, false, 0, &r);
  CHECK(r.ok && r.source == RtcOffsetSource::sdam && r.offset == 0 && r.utc == raw && !r.dated);
  a.apids[rtc].regs[PIANO_RTC_EN_CTL] = 0x00;
  r = readRtc(io, arb, false, 0, &r);
  CHECK(r.ok && !r.enabled);
  CHECK(a.writesOutsideCommands == 0);   // nothing but observer commands
  // No RTC in the map.
  Arb none;
  none.add(7, 0x2200, 0x02, 0xba, 0);
  const Io io2 = none.io();
  r = readRtc(io2, spmiArbiter(io2), false, 0);
  CHECK(!r.ok && r.rtc.apid < 0);
}

int main()
{
  testRtc();
  testCharger();
  testGauge();
  testMcaCodec();
  testMcaPolicy();
  testMcaHv();
  testBoardConfig();
  testServreg();
  testPortOwners();
  testLinkCdp();
  testLinkRp();
  testLinkPd();
  testLinkDetachWakeGeneration();
  testLinkGates();
  testLinkHealth();
  testLinkSleepShutdown();
  testLinkThermal();
  testLinkRandom();
  if (gFailures) {
    std::fprintf(stderr, "power_host: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("power_host: all passed\n");
  return 0;
}
