// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoPower's portable cores: the charger PMIC discovery and port decode over a model of the SPMI arbiter
// (ChargerPmic.h, SpmiObserver.h), the software I2C bus and the bq27z561 reads against a model of the gauge (Gauge.h), and the MCA
// codec and charging policy against a model of the DSP's charger properties (PianoMca.c). Built and run by
// tooling/k27/build-native-piano-power.sh before the kext.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include "SpmiObserver.h"
#include "ChargerPmic.h"
#include "Gauge.h"
extern "C" {
#include "PianoMca.h"
}

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
  bool dropReads = false;

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
        } else if (prop == PIANO_MCA_CHARGE_ENABLE) {
          resp[24] = uint8_t(props.count(prop) ? props[prop] : 0);
        } else {
          put32(resp + 24, props.count(prop) ? props[prop] : 0);
        }
      } else {
        CHECK(op == PIANO_MCA_OP_WRITE);
        const uint32_t value = prop == PIANO_MCA_CHARGE_ENABLE ? out[20] : le32(out + 20);
        writes.push_back({prop, value});
        if (!retcodeFor.count(prop) || !retcodeFor[prop]) props[prop] = value;
        if (prop == PIANO_MCA_PD_FIXED && value == 9000 && props.count(0xf9000)) props[PIANO_MCA_BUS_VOLTAGE] = props[0xf9000];
      }
      piano_mca_receive(&m, resp, sizeof resp);
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
  CHECK(piano_mca_write_allowed(PIANO_MCA_PD_FIXED, 9000, 4) && !piano_mca_write_allowed(PIANO_MCA_PD_FIXED, 12000, 4));
  CHECK(!piano_mca_write_allowed(PIANO_MCA_STATUS, 1, 4) && !piano_mca_write_allowed(0x201a, 0, 4));
  CHECK(piano_mca_icl_for(PIANO_MCA_TYPE_SDP, 0) == 500 && piano_mca_icl_for(PIANO_MCA_TYPE_CDP, 0) == 900);
  CHECK(piano_mca_icl_for(PIANO_MCA_TYPE_DCP, 0) == 1500 && piano_mca_icl_for(PIANO_MCA_TYPE_PD, 3000) == 1500);
  CHECK(piano_mca_icl_for(PIANO_MCA_TYPE_PD, 0) == 900 && piano_mca_icl_for(PIANO_MCA_TYPE_PD, 1200) == 1200);
  CHECK(piano_mca_icl_for(PIANO_MCA_TYPE_FLOAT, 0) == 500 && piano_mca_icl_for(PIANO_MCA_TYPE_UNKNOWN, 0) == 500);
  CHECK(piano_mca_fcc_for(1, 30, 0) == 1000 && piano_mca_fcc_for(1, 30, 1) == 1500 && piano_mca_fcc_for(0, 30, 0) == 500);
  CHECK(piano_mca_fcc_for(1, 10, 0) == 500 && piano_mca_fcc_for(1, 38, 0) == 500 && piano_mca_fcc_for(1, 37, 0) == 1000);

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

int main()
{
  testCharger();
  testGauge();
  testMcaCodec();
  testMcaPolicy();
  testMcaHv();
  if (gFailures) {
    std::fprintf(stderr, "power_host: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("power_host: all passed\n");
  return 0;
}
