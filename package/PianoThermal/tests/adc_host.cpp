// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoThermal's B6 parts (docs/drivers/thermal.md section 12):
//   - Ntc.h: the Steinhart-Hart coefficients derived again from the part's three parameters, the fixed-point ln, the integer curve
//     against the same formula in double, sign, monotony, the defining points, the code -> resistance step;
//   - AdcChannels.h: the map's shape and encodings;
//   - PmicAdc.h: the conversion state machine and the round against a register model of the SPMI arbiter v7 (map, owner table,
//     observer and read/write channels) with the ADC's SDAM 0 and the PBS sequencer behind it (handshake, trigger, end of
//     conversion, status clears); every write audited (only the ADC APID's read/write channel, only the allowlist, never core or
//     cnfg); the error paths (arbiter, APID, owner, denied/failed/timed-out transactions, handshake, stale and stuck status, faults,
//     dropped and wedged conversions, read-back mismatch, no-data/open/short/implausible codes) and their time bounds;
//   - BoardTemps.h: package/include/PianoBoardTemps.h's answer (roles, validity, age, disabled, stopped, bad arguments).
//   adc_host                run the tests
//   adc_host --map          print the channel map for tooling/k27/check-adc-map.py
//   adc_host --ntc          print the curve's derivation
//   adc_host --curve R ...  print the curve's milli-degrees C at each resistance R (ohms), for tooling/k27/check-ntc-curve.py
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. -I../../include adc_host.cpp -o /tmp/adc_host && /tmp/adc_host
#include "AdcChannels.h"
#include "BoardTemps.h"
#include "Ntc.h"
#include "PmicAdc.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace pianothermal;
using namespace pianothermal::adc;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------------- the curve in double
static constexpr double kK0 = 273.15;
static double rFromB(double celsius, double b) { return ntc::kR25Ohms * std::exp(b * (1 / (celsius + kK0) - 1 / (25 + kK0))); }

// Steinhart-Hart through (25 C, R25), (50 C, from B25/50), (85 C, from B25/85): Cramer's rule.
static std::array<double, 3> deriveCoefficients()
{
  const double t[3] = {25, 50, 85}, r[3] = {double(ntc::kR25Ohms), rFromB(50, ntc::kB2550), rFromB(85, ntc::kB2585)};
  double m[3][3], y[3];
  for (int i = 0; i < 3; ++i) {
    const double l = std::log(r[i]);
    m[i][0] = 1;
    m[i][1] = l;
    m[i][2] = l * l * l;
    y[i] = 1 / (t[i] + kK0);
  }
  auto det = [](double a[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  };
  const double d = det(m);
  std::array<double, 3> c{};
  for (int j = 0; j < 3; ++j) {
    double mj[3][3];
    std::memcpy(mj, m, sizeof mj);
    for (int i = 0; i < 3; ++i) mj[i][j] = y[i];
    c[size_t(j)] = det(mj) / d;
  }
  return c;
}
static double shCelsius(double ohms, const std::array<double, 3> &c)
{
  const double l = std::log(ohms);
  return 1 / (c[0] + c[1] * l + c[2] * l * l * l) - kK0;
}
static double shOhms(double celsius, const std::array<double, 3> &c)   // the inverse, by bisection on ln R
{
  double lo = std::log(10.0), hi = std::log(1e9);
  for (int i = 0; i < 200; ++i) {
    const double mid = (lo + hi) / 2;
    if (shCelsius(std::exp(mid), c) > celsius) lo = mid;
    else hi = mid;
  }
  return std::exp((lo + hi) / 2);
}

static void testNtc()
{
  const std::array<double, 3> c = deriveCoefficients();
  const double scale = std::ldexp(1.0, 50);
  CHECK(std::fabs(c[0] * scale - double(ntc::kA)) <= 2);
  CHECK(std::fabs(c[1] * scale - double(ntc::kB)) <= 2);
  CHECK(std::fabs(c[2] * scale - double(ntc::kC)) <= 2);
  CHECK(std::llround(std::log(2.0) * 4294967296.0) == ntc::kLn2Q32);
  // ln in Q32 against the library's, 1 .. 2^40
  double worstLn = 0;
  for (uint64_t x = 1; x < (1ull << 40); x = x * 3 / 2 + 1) {
    const double e = std::fabs(double(ntc::lnQ32(x)) / 4294967296.0 - std::log(double(x)));
    if (e > worstLn) worstLn = e;
  }
  CHECK(worstLn < 1e-8);
  CHECK(ntc::lnQ32(1) == 0);
  // the integer curve against the formula in double, 1 kOhm .. 10 MOhm; strictly falling (an NTC)
  int32_t prev = INT32_MAX;
  double worst = 0;
  for (double r = 1000; r <= 1e7; r *= 1.01) {
    const uint64_t ohms = uint64_t(std::llround(r));
    const int32_t m = ntc::milliC(ohms);
    const double e = std::fabs(m / 1000.0 - shCelsius(double(ohms), c));
    if (e > worst) worst = e;
    CHECK(m < prev);
    prev = m;
  }
  CHECK(worst <= 0.001);
  // the defining points and the part's range
  CHECK(std::abs(ntc::milliC(100000) - 25000) <= 1);
  CHECK(std::abs(ntc::milliC(uint64_t(std::llround(rFromB(50, ntc::kB2550)))) - 50000) <= 10);
  CHECK(std::abs(ntc::milliC(uint64_t(std::llround(rFromB(85, ntc::kB2585)))) - 85000) <= 10);
  CHECK(std::abs(ntc::milliC(uint64_t(std::llround(shOhms(-40, c)))) + 40000) <= 2);
  CHECK(std::abs(ntc::milliC(uint64_t(std::llround(shOhms(125, c)))) - 125000) <= 30);   // 1 ohm is ~0.02 C at 125 C
  // sign: the 0 C resistance is ~355.6 kOhm
  CHECK(ntc::milliC(360000) < 0 && ntc::milliC(350000) > 0);
  CHECK(ntc::milliC(4500000) < -40000 && ntc::milliC(2000) > 125000);
  CHECK(ntc::milliC(0) == ntc::milliC(1));   // clamped, no division by zero
  // a code as a resistance: R = 100 k * code / (full scale - code)
  CHECK(ntc::ohmsOf(8192, kFullScaleGen3, kPullUpOhms) == 100000);
  CHECK(ntc::ohmsOf(4096, kFullScaleGen3, kPullUpOhms) == 33333);
  CHECK(ntc::ohmsOf(32768, kFullScaleGen4, kPullUpOhms) == 100003);
  CHECK(ntc::ohmsOf(1, kFullScaleGen3, kPullUpOhms) == 6);
  CHECK(ntc::ohmsOf(16383, kFullScaleGen3, kPullUpOhms) == 1638300000);
}

// ------------------------------------------------------------------------------------------------------------- the map
static void testMap()
{
  std::set<std::string> names, labels;
  std::set<uint16_t> regs;
  unsigned gen4 = 0;
  for (const ChannelDesc &c : kChannels) {
    CHECK(names.insert(c.name).second && labels.insert(c.label).second && regs.insert(c.reg).second);
    CHECK(c.bus() == 0 && (c.sid() == 0 || c.sid() == 1 || c.sid() == 7));
    CHECK(c.gen == Gen::gen4 ? decimationIndex(c.decimation) == 1 && settleIndex(c.settleUs) == 0
                             : decimationIndex(c.decimation) == 2 && settleIndex(c.settleUs) == 2);
    gen4 += c.gen == Gen::gen4;
  }
  CHECK(gen4 == 4 && kChannelCount == 11);
  CHECK(channelIndex("batt") == 0 && channelIndex("flash") == 10 && channelIndex("bat") < 0 && channelIndex("batt2") < 0);
  CHECK(roleIndex(Role::battery) == channelIndex("batt") && roleIndex(Role::charger) == channelIndex("charger") &&
        roleIndex(Role::connector) == channelIndex("conn") && roleIndex(Role::connector2) == channelIndex("conn2"));
  CHECK(kChannels[channelIndex("xo")].sid() == 0 && kChannels[channelIndex("xo")].channel() == 0x44);
  CHECK(gpioPpid(1, 2) == 0x189 && gpioPpid(7, 6) == 0x78d);
  CHECK(kAdcPpid == 0x090);
  // the allowlist: exactly EOC_CLR, CONV_ERR_CLR, SID..DELAY_CTL and CONV_REQ
  std::set<unsigned> allowed;
  for (unsigned r = 0; r < 0x100; ++r)
    if (writable(r, 1)) allowed.insert(r);
  CHECK((allowed == std::set<unsigned>{0x4b, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0xe5}));
  CHECK(writable(kRegSid, 7) && !writable(kRegSid, 8) && !writable(0x4a, 2) && !writable(0xe5, 2) && !writable(0x4b, 0));
  CHECK(!writable(0xff, 2) && !writable(0x56, 1) && !writable(0x5c, 2) && !writable(0xe6, 1));
}

// ------------------------------------------------------------------------------------------------------------- the model
struct Write {
  int apid;
  unsigned reg;
  std::vector<uint8_t> bytes;
};

struct Model {
  // the arbiter
  uint32_t version = 0x70020000;
  std::vector<uint32_t> map, owner;
  std::map<uint16_t, std::array<uint8_t, 256>> pages;   // PPID -> registers
  int adcApid = -1;
  std::map<uint32_t, uint32_t> obsvReg;                 // observer channel registers, by offset
  uint32_t wdata0 = 0, wdata1 = 0, chnlStatus = 0;
  // the clock
  uint64_t now = 0;
  // the PBS: one trigger at a time
  bool pending = false;
  uint64_t due = 0;
  uint32_t convUs = 2400, clearUs = 150;
  // the thermistors, milli-C by (sid, channel); codes forced by (sid, channel)
  std::map<uint16_t, int32_t> temperature;
  std::map<uint16_t, uint16_t> forcedCode;
  // faults
  std::set<uint16_t> faultOn, dropOn, wedgeOn;   // by sid << 8 | channel
  bool hsStuck = false, eocClearBroken = false, faultClearBroken = false, denyWrites = false, readsTimeOut = false;
  int dropWriteReg = -1;
  // audit
  std::vector<Write> writes;
  unsigned coreOrCnfgWrites = 0, badChannelWrites = 0, triggersWhileBusy = 0, badConfigs = 0, reads = 0;
  std::array<double, 3> c = deriveCoefficients();

  Model()
  {
    map.assign(300, 0);
    owner.assign(300, 0);
    auto add = [&](unsigned apid, uint16_t ppid, uint32_t ee) {
      map[apid] = uint32_t(ppid) << 8 | 1u;   // bit 0: noise in the low byte, as the real map's other fields would be
      owner[apid] = ee;
      pages[ppid].fill(0);
    };
    add(5, 0x013, 0);     // PON
    add(9, 0x071, 0);     // SDAM 0x7100
    add(40, 0x090, 0);    // the ADC's SDAM 0
    add(41, 0x091, 0);    // its SDAM 1
    add(80, 0x189, 0);    // PM8550 GPIO 2, 3, 4
    add(81, 0x18a, 0);
    add(82, 0x18b, 0);
    add(150, 0x78d, 3);   // PMIH0108 GPIO 6 and 12 (another EE's: reads still work)
    add(152, 0x793, 3);
    add(151, 0x7a0, 3);
    adcApid = 40;
    auto &sdam = pages[0x090];
    sdam[kRegType] = 0x2e;
    sdam[kRegSubtype] = 0x02;
    sdam[kRegSdamSize] = 0x04;
    sdam[kRegHs] = kHsReady;
    sdam[kRegDigParam] = 0xc3;   // bits the conversion must keep
    for (auto &p : pages)
      if (p.first >> 8 && (p.first & 0xff) >= 0x88 && (p.first & 0xff) < 0xa0) p.second[kGpioRegType] = kGpioType;
    const int32_t t[kChannelCount] = {31200, 33000, 30100, 30400, 35200, 31000, 36000, 34100, 33300, 33800, 32900};
    for (unsigned i = 0; i < kChannelCount; ++i) temperature[key(kChannels[i])] = t[i];
  }
  static uint16_t key(const ChannelDesc &d) { return uint16_t(d.sid() << 8 | d.channel()); }
  std::array<uint8_t, 256> &sdam() { return pages[0x090]; }

  uint16_t codeFor(uint16_t k, uint32_t fullScale)
  {
    if (forcedCode.count(k)) return forcedCode[k];
    const double r = shOhms(temperature.count(k) ? temperature[k] / 1000.0 : 25.0, c);
    return uint16_t(std::lround(fullScale * r / (r + kPullUpOhms)));
  }
  void tick()
  {
    if (!pending || now < due) return;
    auto &s = sdam();
    const uint8_t perph = s[kRegPerphCh];
    if (perph & kChanConvReq) {
      const uint8_t sid = s[kRegSid] & 0x1f, bus = (s[kRegSid] >> 5) & 3, ch = s[kRegChSel];
      const uint16_t k = uint16_t(sid << 8 | ch);
      const ChannelDesc *d = nullptr;
      for (const ChannelDesc &x : kChannels)
        if (key(x) == k) d = &x;
      // the PBS's view of the command: the DT's settings, encoded
      if (!d || bus || s[kRegTimerSel] != kTimeImmediate || ((s[kRegDigParam] >> 4) & 3) != kCalRatiometric ||
          ((s[kRegDigParam] >> 2) & 3) != unsigned(decimationIndex(d->decimation)) || (s[kRegDigParam] & 0xc3) != 0xc3 ||
          s[kRegFastAvg] != kFastAvgEnable || (s[kRegDelayCtl] & 0xf) != unsigned(settleIndex(d->settleUs)) || (perph & 7))
        ++badConfigs;
      if (wedgeOn.count(k)) return;               // never finishes: HS stays busy, CONV_REQ stays 1
      if (faultOn.count(k)) {
        s[kRegStatus1] |= kStatus1Fault;
      } else if (!dropOn.count(k)) {
        const uint16_t code = codeFor(k, d ? d->fullScale() : kFullScaleGen3);
        s[kRegData0] = uint8_t(code);
        s[kRegData0 + 1] = uint8_t(code >> 8);
        s[kRegEocSts] |= kEocChan0;
        s[kRegStatus1] |= kStatus1Eoc;
      }
    } else {                                      // a status clear
      if ((s[kRegEocClr] & 1) && !eocClearBroken) {
        s[kRegEocSts] &= uint8_t(~kEocChan0);
        s[kRegStatus1] &= uint8_t(~kStatus1Eoc);
      }
      if ((s[kRegConvErrClr] & 1) && !faultClearBroken) s[kRegStatus1] &= uint8_t(~kStatus1Fault);
      s[kRegEocClr] = 0;
      s[kRegConvErrClr] = 0;
    }
    s[kRegConvReq] = 0;
    s[kRegHs] = kHsReady;
    pending = false;
  }
  void sdamWrite(unsigned reg, uint8_t v)
  {
    if (int(reg) == dropWriteReg) return;
    auto &s = sdam();
    if (reg == kRegConvReq && (v & 1)) {
      if (pending || s[kRegConvReq]) ++triggersWhileBusy;
      s[reg] = v;
      pending = true;
      due = now + ((s[kRegPerphCh] & kChanConvReq) ? convUs : clearUs);
      s[kRegHs] = kHsBusy;
      return;
    }
    s[reg] = v;
  }
  uint8_t pageRead(uint16_t ppid, unsigned reg)
  {
    if (ppid == 0x090) {
      tick();
      if (hsStuck && reg == kRegHs) return kHsBusy;
    }
    return pages.count(ppid) ? pages[ppid][reg] : 0;
  }

  uint32_t rd(Win w, uint32_t off)
  {
    tick();
    switch (w) {
      case Win::core:
        if (off == 0) return version;
        if (off == 4) return uint32_t(map.size());
        if (off >= kApidMap && (off - kApidMap) / 4 < map.size()) return map[(off - kApidMap) / 4];
        return 0;
      case Win::cnfg: return off / 4 < owner.size() ? owner[off / 4] : 0;
      case Win::obsv: return obsvReg.count(off) ? obsvReg[off] : 0;
      case Win::chnl: return off == kChStatus ? chnlStatus : 0;
    }
    return 0;
  }
  void wr(Win w, uint32_t off, uint32_t v)
  {
    tick();
    if (w == Win::core || w == Win::cnfg) {
      ++coreOrCnfgWrites;
      return;
    }
    if (w == Win::obsv) {
      const uint32_t apid = off / kObsvStride, ch = apid * kObsvStride;
      if (off != ch + kChCmd || apid >= map.size()) {
        ++badChannelWrites;
        return;
      }
      const uint32_t op = v >> 27, reg = (v >> 4) & 0xff, n = (v & 7) + 1;
      if (op != kOpExtReadL || readsTimeOut) {
        obsvReg[ch + kChStatus] = readsTimeOut ? 0 : (kChDone | kChFailure);
        return;
      }
      const uint16_t ppid = uint16_t((map[apid] >> 8) & 0xfff);
      uint32_t lo = 0, hi = 0;
      for (uint32_t b = 0; b < n; ++b) (b < 4 ? lo : hi) |= uint32_t(pageRead(ppid, (reg + b) & 0xff)) << (8 * (b & 3));
      obsvReg[ch + kChRdata0] = lo;
      obsvReg[ch + kChRdata1] = hi;
      obsvReg[ch + kChStatus] = kChDone;
      ++reads;
      return;
    }
    // chnl: the ADC APID's read/write channel page
    if (off == kChWdata0) wdata0 = v;
    else if (off == kChWdata1) wdata1 = v;
    else if (off == kChCmd) {
      const uint32_t op = v >> 27, reg = (v >> 4) & 0xff, n = (v & 7) + 1;
      if (op != kOpExtWriteL) {
        ++badChannelWrites;
        chnlStatus = kChDone | kChFailure;
        return;
      }
      if (denyWrites || owner[size_t(adcApid)] != 0) {
        chnlStatus = kChDone | kChDenied;
        return;
      }
      Write rec{adcApid, reg, {}};
      for (uint32_t b = 0; b < n; ++b) {
        const uint8_t byte = uint8_t((b < 4 ? wdata0 : wdata1) >> (8 * (b & 3)));
        rec.bytes.push_back(byte);
        sdamWrite(reg + b, byte);
      }
      writes.push_back(rec);
      chnlStatus = kChDone;
    } else {
      ++badChannelWrites;
    }
  }
  Io io()
  {
    Io i;
    i.ctx = this;
    i.rd = [](void *m, Win w, uint32_t o) { return static_cast<Model *>(m)->rd(w, o); };
    i.wr = [](void *m, Win w, uint32_t o, uint32_t v) { static_cast<Model *>(m)->wr(w, o, v); };
    i.delayUs = [](void *m, uint32_t us) { static_cast<Model *>(m)->now += us; };
    i.nowUs = [](void *m) { return static_cast<Model *>(m)->now; };
    return i;
  }
  // every write: the ADC APID, the allowlist
  bool auditClean() const
  {
    if (coreOrCnfgWrites || badChannelWrites) return false;
    for (const Write &w : writes)
      if (w.apid != adcApid || !writable(w.reg, unsigned(w.bytes.size()))) return false;
    return true;
  }
};

// find + the kext's order: map the channel only after find() said yes, then allow writes.
static Err attach(Board &b, Model &m)
{
  const Err e = b.adc.find(m.io());
  if (e == Err::none) b.adc.enableWrites();
  return e;
}

static void testRound()
{
  Model m;
  Board b;
  CHECK(attach(b, m) == Err::none);
  CHECK(b.adc.where.apid == 40 && b.adc.where.owner == 0 && b.adc.arbiter.v7 && b.adc.arbiter.count == 300);
  Survey s;
  CHECK(b.adc.survey(s) == Err::none && s.type == 0x2e && s.hs == kHsReady && !s.eoc && !s.convReq && s.command[3] == 0xc3);
  CHECK(m.writes.empty());   // find and the survey are reads
  b.round();
  CHECK(b.eventCount == 0 && b.enabledCount() == kChannelCount && b.adc.stopped == Err::none);
  for (unsigned i = 0; i < kChannelCount; ++i) {
    const ChannelState &st = b.ch[i];
    CHECK(st.valid && st.ok == 1);
    CHECK(std::abs(st.last.milliC - m.temperature[Model::key(kChannels[i])]) <= 20);   // a code is 0.005 C (Gen3) / 0.0013 C (Gen4)
    CHECK(st.last.us > m.convUs && st.last.us < m.convUs + 2000);
  }
  CHECK(m.badConfigs == 0 && m.triggersWhileBusy == 0 && m.auditClean());
  // per conversion: the 7 command bytes, the trigger, the status clear's three bytes
  CHECK(m.writes.size() == 5 * kChannelCount);
  const Write &cmd = m.writes[0];
  const ChannelDesc &batt = kChannels[0];
  CHECK(cmd.reg == kRegSid && cmd.bytes.size() == 7);
  CHECK((cmd.bytes == std::vector<uint8_t>{0x07, 0x80, 0x01, uint8_t(0xc3 | 0x10 | 1 << 2), 0x80, 0x44, 0x00}));
  CHECK(m.writes[1].reg == kRegConvReq && m.writes[1].bytes == std::vector<uint8_t>{1});
  CHECK(m.writes[2].reg == kRegEocClr && m.writes[2].bytes == std::vector<uint8_t>{1});
  CHECK(m.writes[3].reg == kRegPerphCh && m.writes[3].bytes == std::vector<uint8_t>{0});
  CHECK(m.writes[4].reg == kRegConvReq && m.writes[4].bytes == std::vector<uint8_t>{1});
  const Write &cmd3 = m.writes[5 * 3];   // conn2: PM8550 (SID 1), Gen3, decimation 1360, settle 200 us
  CHECK((cmd3.bytes == std::vector<uint8_t>{0x01, 0x80, 0x01, uint8_t(0xc3 | 0x10 | 2 << 2), 0x80, 0x4b, 0x02}));
  CHECK(batt.gen == Gen::gen4);
  // a second round: the same, nothing left pending in between
  b.round();
  CHECK(b.enabledCount() == kChannelCount && b.ch[0].ok == 2 && m.triggersWhileBusy == 0 && m.auditClean());
  CHECK(b.adc.conversions == 2 * kChannelCount && b.adc.failures == 0 && b.adc.maxUs < 5000);
  // temperatures move: the next round follows
  m.temperature[Model::key(kChannels[0])] = 45500;
  m.temperature[Model::key(kChannels[3])] = -5000;
  b.round();
  CHECK(std::abs(b.ch[0].last.milliC - 45500) <= 20 && std::abs(b.ch[3].last.milliC + 5000) <= 20 && b.ch[3].last.milliC < 0);
  // the pins: three high-Z, one driven
  m.pages[0x18a][kGpioRegEn] = kGpioMasterEn;
  const PinState p2 = b.adc.readPin(kThermPins[0]), p3 = b.adc.readPin(kThermPins[1]), p6 = b.adc.readPin(kThermPins[3]),
                 p12 = b.adc.readPin(kThermPins[4]);
  CHECK(p2.err == Err::none && p2.apid == 80 && p2.type == kGpioType && p2.highZ());
  CHECK(p3.err == Err::none && !p3.highZ());
  CHECK(p6.err == Err::none && p6.apid == 150 && p6.highZ());   // another EE's GPIO: observer reads work
  CHECK(p12.err == Err::none && p12.apid == 152 && p12.highZ() && !kThermPins[4].inDt && gpioPpid(7, 12) == 0x793);
  m.map[152] = 0;
  CHECK(b.adc.readPin(kThermPins[4]).err == Err::noApid);
  CHECK(m.auditClean());
}

static void testArbiterErrors()
{
  {   // not a v7 arbiter
    Model m;
    m.version = 0x50020000;
    Board b;
    CHECK(attach(b, m) == Err::notArbiterV7 && b.adc.stopped == Err::notArbiterV7 && !b.adc.bus.writes);
    b.round();
    CHECK(b.enabledCount() == 0 && b.ch[0].err == Err::notArbiterV7 && m.writes.empty() && m.reads == 0);
  }
  {   // PPID 0x090 not mapped
    Model m;
    m.map[40] = 0;
    Board b;
    CHECK(attach(b, m) == Err::noApid);
    b.round();
    CHECK(b.enabledCount() == 0 && m.writes.empty());
  }
  {   // another EE owns the SDAM: no write is even attempted
    Model m;
    m.owner[40] = 2;
    Board b;
    CHECK(attach(b, m) == Err::notOwned && !b.adc.bus.writes);
    b.round();
    CHECK(b.enabledCount() == 0 && b.ch[5].err == Err::notOwned && m.writes.empty() && m.reads == 0);
  }
  {   // two entries for the PPID: EE 0's wins
    Model m;
    m.map[12] = 0x090u << 8;
    m.owner[12] = 3;
    Board b;
    CHECK(attach(b, m) == Err::none && b.adc.where.apid == 40 && b.adc.where.entries == 2);
  }
  {   // writes denied by the arbiter after all: stopped at the first write
    Model m;
    m.denyWrites = true;
    Board b;
    CHECK(attach(b, m) == Err::none);
    b.round();
    CHECK(b.adc.stopped == Err::spmiDenied && b.enabledCount() == 0 && b.eventCount == 1 && b.events[0].channel == 0);
    CHECK(m.writes.empty());
  }
  {   // reads never complete: bounded (1000 x 1 us), stopped
    Model m;
    Board b;
    CHECK(attach(b, m) == Err::none);
    m.readsTimeOut = true;
    const uint64_t t0 = m.now;
    b.round();
    CHECK(b.adc.stopped == Err::spmiTimeout && b.enabledCount() == 0 && m.now - t0 <= 1100 && m.writes.empty());
  }
  {   // a write without enableWrites (the channel not mapped): refused before any store
    Model m;
    Board b;
    CHECK(b.adc.find(m.io()) == Err::none);
    const uint8_t one = 1;
    CHECK(b.adc.bus.write(kRegConvReq, &one, 1) == Err::refused && m.writes.empty());
    b.adc.enableWrites();
    CHECK(b.adc.bus.write(0x56, &one, 1) == Err::refused && b.adc.bus.write(0x40, &one, 1) == Err::refused);   // CH_EN, memory
    CHECK(b.adc.bus.write(kRegData0, &one, 1) == Err::refused && m.writes.empty() && b.adc.bus.writeCount == 0);
  }
}

static void testAdcErrors()
{
  const uint16_t kConn2 = Model::key(kChannels[3]), kBatt = Model::key(kChannels[0]);
  {   // the handshake never ready: 20 ms, then stopped, nothing written
    Model m;
    m.hsStuck = true;
    Board b;
    CHECK(attach(b, m) == Err::none);
    const uint64_t t0 = m.now;
    b.round();
    CHECK(b.adc.stopped == Err::handshake && b.enabledCount() == 0 && m.writes.empty());
    CHECK(m.now - t0 >= 20000 && m.now - t0 <= 22000 && b.adc.lastHs == kHsBusy);
  }
  {   // a stale end of conversion: cleared first, then the conversion runs
    Model m;
    m.sdam()[kRegEocSts] = kEocChan0;
    m.sdam()[kRegData0] = 0x34;
    Board b;
    CHECK(attach(b, m) == Err::none);
    b.round();
    CHECK(b.adc.stopped == Err::none && b.enabledCount() == kChannelCount && b.adc.staleClears == 1);
    CHECK(std::abs(b.ch[0].last.milliC - 31200) <= 20 && m.triggersWhileBusy == 0 && m.auditClean());
  }
  {   // ... that does not clear: stopped
    Model m;
    m.sdam()[kRegEocSts] = kEocChan0;
    m.eocClearBroken = true;
    Board b;
    CHECK(attach(b, m) == Err::none);
    b.round();
    CHECK(b.adc.stopped == Err::eocStuck && b.enabledCount() == 0 && m.auditClean());
  }
  {   // a stale fault: cleared; one that does not clear: stopped
    Model m;
    m.sdam()[kRegStatus1] = kStatus1Fault;
    Board b;
    CHECK(attach(b, m) == Err::none);
    b.round();
    CHECK(b.adc.stopped == Err::none && b.adc.staleClears == 1 && b.enabledCount() == kChannelCount);
    Model m2;
    m2.sdam()[kRegStatus1] = kStatus1Fault;
    m2.faultClearBroken = true;
    Board b2;
    CHECK(attach(b2, m2) == Err::none);
    b2.round();
    CHECK(b2.adc.stopped == Err::faultStuck && b2.enabledCount() == 0);
  }
  {   // a conversion fault on one channel: that channel off (its fault cleared), the others go on, and stay on next round
    Model m;
    m.faultOn.insert(kConn2);
    Board b;
    CHECK(attach(b, m) == Err::none);
    b.round();
    CHECK(b.adc.stopped == Err::none && b.eventCount == 1 && b.events[0].channel == 3 && b.events[0].err == Err::convFault);
    CHECK(!b.ch[3].enabled && !b.ch[3].valid && b.enabledCount() == kChannelCount - 1 && !(m.sdam()[kRegStatus1] & kStatus1Fault));
    bool clearedFault = false;
    for (const Write &w : m.writes) clearedFault |= w.reg == kRegConvErrClr;
    CHECK(clearedFault && m.auditClean());
    m.faultOn.clear();
    b.round();
    CHECK(b.eventCount == 0 && !b.ch[3].enabled && b.ch[4].ok == 2);   // disabled is disabled for the boot
  }
  {   // a conversion the PBS drops (no end of conversion, idle again): 600 ms, that channel off, the rest fine
    Model m;
    m.dropOn.insert(kBatt);
    Board b;
    CHECK(attach(b, m) == Err::none);
    const uint64_t t0 = m.now;
    b.round();
    CHECK(b.events[0].channel == 0 && b.events[0].err == Err::convTimeout && b.eventCount == 1 && b.adc.stopped == Err::none);
    CHECK(b.enabledCount() == kChannelCount - 1 && b.ch[1].valid);
    CHECK(m.now - t0 >= 600000 && m.now - t0 <= 600000 + 10 * 6000 + 5000);   // the timeout, then ten normal conversions
  }
  {   // a wedged PBS: the channel times out, the next one finds no handshake, the ADC stops
    Model m;
    m.wedgeOn.insert(kBatt);
    Board b;
    CHECK(attach(b, m) == Err::none);
    const uint64_t t0 = m.now;
    b.round();
    CHECK(b.eventCount == 2 && b.events[0].err == Err::convTimeout && b.events[1].channel == 1 && b.events[1].err == Err::handshake);
    CHECK(b.adc.stopped == Err::handshake && b.enabledCount() == 0 && m.now - t0 <= 600000 + 25000);
    CHECK(m.triggersWhileBusy == 0);   // no trigger ever written while one was pending
  }
  {   // the command bytes do not stick: stopped before any trigger
    Model m;
    m.dropWriteReg = kRegChSel;
    Board b;
    CHECK(attach(b, m) == Err::none);
    b.round();
    CHECK(b.adc.stopped == Err::configMismatch && b.enabledCount() == 0);
    for (const Write &w : m.writes) CHECK(w.reg != kRegConvReq);
  }
}

static void testCodes()
{
  const uint16_t kConn2 = Model::key(kChannels[3]), kBatt = Model::key(kChannels[0]), kCharger = Model::key(kChannels[1]),
                 kXo = Model::key(kChannels[6]), kMsm = Model::key(kChannels[4]), kQuiet = Model::key(kChannels[5]),
                 kWifi = Model::key(kChannels[7]);
  Model m;
  m.forcedCode[kBatt] = 0x8000;    // Gen4: mid-scale, 25 C
  m.forcedCode[kConn2] = 0x8000;   // Gen3: no data
  m.forcedCode[kCharger] = 0xffff; // Gen4 full scale: open
  m.forcedCode[kXo] = 0x4000;      // Gen3 full scale: open
  m.forcedCode[kMsm] = 0;          // short
  m.forcedCode[kQuiet] = 900;      // Gen4: ~1.4 kOhm, ~143 C: implausible
  m.forcedCode[kWifi] = 0xfff0;    // Gen3 negative: short
  Board b;
  CHECK(attach(b, m) == Err::none);
  b.round();
  CHECK(b.adc.stopped == Err::none);
  CHECK(b.ch[0].valid && std::abs(b.ch[0].last.milliC - 25000) <= 5 && b.ch[0].last.code == 0x8000);
  CHECK(!b.ch[3].enabled && b.ch[3].err == Err::invalidData && b.ch[3].last.code == 0x8000);
  CHECK(b.ch[1].err == Err::open && b.ch[6].err == Err::open && b.ch[4].err == Err::shorted && b.ch[7].err == Err::shorted);
  CHECK(b.ch[5].err == Err::implausible && b.ch[5].last.milliC > 125000);
  CHECK(b.eventCount == 6 && b.enabledCount() == kChannelCount - 6 && b.ch[2].valid && b.ch[8].valid);
  CHECK(m.auditClean() && m.triggersWhileBusy == 0);
  // scale() alone: sign and range at the edges
  Reading r;
  r.code = uint16_t(std::lround(kFullScaleGen3 * 400000.0 / 500000.0));   // 400 kOhm: below 0 C
  CHECK(scale(kChannels[3], r) == Err::none && r.milliC < 0 && r.milliC > -5000);
  r.code = 8192;
  CHECK(scale(kChannels[3], r) == Err::none && r.ohms == 100000 && r.milliC == 25000);
  r.code = 16200;                                                          // ~8.8 MOhm: below -40 C
  CHECK(scale(kChannels[3], r) == Err::implausible);
  for (const char *e : {errName(Err::none), errName(Err::handshake), errName(Err::badChannel)}) CHECK(e && *e);
  CHECK(isGlobal(Err::refused) && !isGlobal(Err::convTimeout) && !isGlobal(Err::none) && !isGlobal(Err::implausible));
}

// ------------------------------------------------------------------------------------------------------------- the API
static void testBoardTemps()
{
  Model m;
  Board b;
  CHECK(attach(b, m) == Err::none);
  piano_board_temps t{};
  t.version = 0;
  CHECK(fillBoardTemps(snapshotOf(b, 30000), m.now, &t) == -1 && fillBoardTemps(snapshotOf(b, 30000), m.now, nullptr) == -1);
  t.version = PIANO_BOARD_TEMPS_VERSION;
  CHECK(fillBoardTemps(Snapshot(), 0, &t) == 0 && t.state == PIANO_BOARD_TEMPS_OFF && !t.valid && !t.disabled);
  CHECK(fillBoardTemps(snapshotOf(b, 30000), m.now, &t) == 0 && t.state == PIANO_BOARD_TEMPS_STARTING && !t.valid);
  m.faultOn.insert(Model::key(kChannels[3]));   // conn2
  m.temperature[Model::key(kChannels[1])] = -12000;
  b.round();
  const Snapshot s = snapshotOf(b, 30000);
  CHECK(fillBoardTemps(s, m.now, &t) == 3 && t.state == PIANO_BOARD_TEMPS_RUNNING && t.rounds == 1 && t.period_ms == 30000);
  CHECK(t.valid == 0x7 && t.disabled == (1u << PIANO_BOARD_TEMP_CONNECTOR2) && t.milli_c[PIANO_BOARD_TEMP_CONNECTOR2] == 0);
  CHECK(std::abs(t.milli_c[PIANO_BOARD_TEMP_BATTERY] - 31200) <= 20 && std::abs(t.milli_c[PIANO_BOARD_TEMP_CHARGER] + 12000) <= 20);
  CHECK(std::abs(t.milli_c[PIANO_BOARD_TEMP_CONNECTOR] - 30100) <= 20 && t.age_ms[PIANO_BOARD_TEMP_BATTERY] < 100);
  // three periods old still counts, a millisecond more does not
  const uint64_t last = s.atUs[channelIndex("conn")];
  CHECK(fillBoardTemps(s, last + 90000ull * 1000, &t) >= 1 && (t.valid & (1u << PIANO_BOARD_TEMP_CONNECTOR)));
  CHECK(fillBoardTemps(s, last + 90001ull * 1000, &t) == 0 && !t.valid && t.milli_c[PIANO_BOARD_TEMP_BATTERY] == 0);
  // stopped: nothing counts, every channel off
  m.hsStuck = true;
  b.round();
  CHECK(fillBoardTemps(snapshotOf(b, 30000), m.now, &t) == 0 && t.state == PIANO_BOARD_TEMPS_STOPPED && t.disabled == 0xf);
}

// ------------------------------------------------------------------------------------------------------------- printers
static void printMap()
{
  std::printf("ee 0\nsdam %#x %#x\n", kSdamBase, kSdam1Base);
  for (const ChannelDesc &c : kChannels)
    std::printf("channel %#x label %s zone %s gen %u decimation %u settle %u ratiometric 1 prescale 1/1\n", c.reg, c.label, c.zone,
                c.gen == Gen::gen4 ? 4u : 3u, c.decimation, c.settleUs);
  for (const ThermPin &p : kThermPins)
    if (p.inDt) std::printf("pin sid %u gpio %u\n", p.sid, p.gpio);   // the firmware's pin is not in the DT
}

static void printNtc()
{
  const std::array<double, 3> c = deriveCoefficients();
  std::printf("part: R25 %u ohm, B25/50 %u K, B25/85 %u K\n", ntc::kR25Ohms, ntc::kB2550, ntc::kB2585);
  std::printf("points: 25 C %.1f ohm, 50 C %.1f ohm, 85 C %.1f ohm\n", double(ntc::kR25Ohms), rFromB(50, ntc::kB2550), rFromB(85, ntc::kB2585));
  std::printf("Steinhart-Hart: a %.9e b %.9e c %.9e\n", c[0], c[1], c[2]);
  std::printf("Q50: a %lld b %lld c %lld (Ntc.h: %lld %lld %lld)\n", (long long)std::llround(std::ldexp(c[0], 50)),
              (long long)std::llround(std::ldexp(c[1], 50)), (long long)std::llround(std::ldexp(c[2], 50)), (long long)ntc::kA,
              (long long)ntc::kB, (long long)ntc::kC);
  for (int t = -40; t <= 125; t += 15) std::printf("  %4d C  %10.0f ohm\n", t, shOhms(t, c));
}

int main(int argc, char **argv)
{
  if (argc > 1 && !strcmp(argv[1], "--map")) {
    printMap();
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "--ntc")) {
    printNtc();
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "--curve")) {
    for (int i = 2; i < argc; ++i) std::printf("%s %d\n", argv[i], ntc::milliC(std::strtoull(argv[i], nullptr, 10)));
    return 0;
  }
  testNtc();
  testMap();
  testRound();
  testArbiterErrors();
  testAdcErrors();
  testCodes();
  testBoardTemps();
  if (failures) {
    std::printf("adc_host: %d FAILED\n", failures);
    return 1;
  }
  std::printf("adc_host: all passed\n");
  return 0;
}
