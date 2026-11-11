// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoThermal's portable parts:
//   - Tsens.h: the TSENS v2 status/threshold/version/ctrl decode (sign, units, VALID), the deci-degree text, the sensor map's shape,
//     the poll against a register model (VALID retries, implausible readings, stale stand-ins, unmapped controllers, hottest);
//   - Policy.h: the protection state machine (thresholds from the stock trips, hysteresis, the critical debounce, unknown);
//   - Scmi.h: the message header, the shared-memory transport and the perf protocol against a model of the CPUCP (v4 with level
//     indexing and v3 with scaled values, paged DESCRIBE_LEVELS, LEVEL_GET/SET, busy/timeout/error/wrong-answer channels);
//   - Governor.h: fixed and governor modes, hispeed, down hysteresis, battery/power caps, floor, the thermal limit; the load window
//     without PianoThermal's own work and without the clock measurement's window (P9, 2026-11-12).
//   - Cluster.h: a cluster against its stock trips and against the 90 C ceiling (capped at each zone's stock trip), the near-trip
//     test, the poll tiers (the fast-clock rule fast only when warm); Tsens.h's interrupt encoders and the suspend mask.
//   thermal_host         run the tests
//   thermal_host --map   print the sensor map for tooling/k27/check-thermal-map.py
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. thermal_host.cpp -o /tmp/thermal_host && /tmp/thermal_host
#include "Cluster.h"
#include "Governor.h"
#include "Policy.h"
#include "Scmi.h"
#include "Tsens.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace pianothermal;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------------- decode
static uint32_t statusWord(int32_t deci, bool valid) { return (uint32_t(deci) & 0xfff) | (valid ? kStatusValid : 0); }

static void testDecode()
{
  int32_t d = 0;
  CHECK(decodeStatus(kStatusValid | 0x1e2, d) && d == 482);           // 48.2 C
  CHECK(decodeStatus(kStatusValid | 0xffb, d) && d == -5);            // -0.5 C: 12-bit two's complement
  CHECK(decodeStatus(kStatusValid | 0x800, d) && d == -2048);         // the most negative code
  CHECK(decodeStatus(kStatusValid | 0x7ff, d) && d == 2047);
  CHECK(!decodeStatus(0x1e2, d) && d == 482);                         // VALID clear: decoded, but not valid
  CHECK(decodeStatus(kStatusValid | kStatusUpper | kStatusMax | 0x3e8, d) && d == 1000);   // status bits do not leak into the value
  const Thresholds t = decodeThresholds(uint32_t(1200) << 12 | (uint32_t(-50) & 0xfff), 1250);
  CHECK(t.upperDeci == 1200 && t.lowerDeci == -50 && t.criticalDeci == 1250);
  const Version v = decodeVersion(0x20050001);
  CHECK(v.major == 2 && v.minor == 5 && v.step == 1);
  const Ctrl c = decodeCtrl(kCtrlEnable | (0x7fffu << kCtrlSensorShift) | kCtrlResultTemp);
  CHECK(c.enabled && popcount(c.sensorMask) == 15 && c.temperatureFormat);
  CHECK(!decodeCtrl(0).enabled && !decodeCtrl(0).temperatureFormat);
  char s[16];
  deciText(482, s, sizeof s); CHECK(!strcmp(s, "48.2"));
  deciText(-5, s, sizeof s); CHECK(!strcmp(s, "-0.5"));
  deciText(0, s, sizeof s); CHECK(!strcmp(s, "0.0"));
  deciText(1250, s, sizeof s); CHECK(!strcmp(s, "125.0"));
  deciText(-2048, s, sizeof s); CHECK(!strcmp(s, "-204.8"));
  deciText(1250, s, 4); CHECK(!strcmp(s, "125"));                      // truncated, still terminated
}

static void testMap()
{
  const unsigned dtCount[kControllers] = {15, 7, 16, 9};   // #qcom,sensors of the four DT nodes
  std::set<std::string> names;
  for (unsigned c = 0; c < kControllers; ++c) {
    CHECK(kMap[c].count == dtCount[c]);
    CHECK(kMap[c].count <= kMaxSensors);
    CHECK((kMap[c].tm & 0xfff) == 0 && (kMap[c].srot & 0xfff) == 0);
    for (unsigned s = 0; s < kMap[c].count; ++s) {
      const Sensor &sn = kMap[c].sensors[s];
      CHECK(names.insert(sn.name).second);
      CHECK(sn.criticalDeci == 1250);
      CHECK(passiveOf(sn) >= 1050 && passiveOf(sn) <= 1180);
      CHECK(passiveOf(sn) < sn.criticalDeci - 50);
    }
  }
  CHECK(!strcmp(kMap[0].sensors[1].name, "cpu-0-0-0") && kMap[0].sensors[1].passiveDeci == 1050);
  CHECK(!strcmp(kMap[0].sensors[2].name, "cpu-0-0-1") && kMap[0].sensors[2].passiveDeci == 1180 && kMap[0].sensors[2].passiveHystDeci == 80);
  CHECK(!strcmp(kMap[1].sensors[4].name, "cpu-1-1-1") && kMap[1].sensors[4].passiveDeci == 1050);
  CHECK(!strcmp(kMap[2].sensors[1].name, "gpuss0") && kMap[2].sensors[8].passiveDeci == 1050 && !kMap[2].sensors[8].hotDeci);
  CHECK(!strcmp(kMap[3].sensors[8].name, "ddr") && passiveOf(kMap[3].sensors[8]) == 1050 && kMap[3].sensors[8].hotDeci == 1200);
  CHECK(kMap[0].cpuFirst == 1 && kMap[0].cpuLast == 12 && kMap[1].cpuFirst == 1 && kMap[1].cpuLast == 4);
  CHECK(!strcmp(kMap[0].sensors[12].name, "cpu-0-5-1") && !strcmp(kMap[1].sensors[4].name, "cpu-1-1-1"));
  CHECK(kMap[2].tm == 0x0c22a000 && kMap[2].srot == 0x0c224000);
}

// ------------------------------------------------------------------------------------------------------------- poll
struct TsensModel {
  bool mapped[kControllers] = {true, true, true, true};
  uint32_t status[kControllers][kMaxSensors] = {};
  unsigned clearFirst[kControllers][kMaxSensors] = {};   // reads that return VALID clear before the word
  unsigned reads = 0;
  std::vector<uint32_t> offsets;
  static uint32_t read(void *ctx, unsigned c, uint32_t off)
  {
    TsensModel &m = *static_cast<TsensModel *>(ctx);
    ++m.reads;
    m.offsets.push_back(off);
    const unsigned s = (off - kTmStatus) / 4;
    if (m.clearFirst[c][s]) {
      --m.clearFirst[c][s];
      return m.status[c][s] & ~kStatusValid;
    }
    return m.status[c][s];
  }
  static bool isMapped(void *ctx, unsigned c) { return static_cast<TsensModel *>(ctx)->mapped[c]; }
  TsensIo io() { return {this, read, isMapped}; }
  void all(int32_t deci)
  {
    for (unsigned c = 0; c < kControllers; ++c)
      for (unsigned s = 0; s < kMap[c].count; ++s) status[c][s] = statusWord(deci, true);
  }
};

static void testPoll()
{
  TsensModel model;
  model.all(400);
  model.status[1][3] = statusWord(523, true);
  model.clearFirst[1][3] = 2;                               // VALID only on the third read
  model.status[2][5] = statusWord(2047, true);              // 204.7 C: implausible
  model.status[3][2] = statusWord(410, false);              // never VALID
  const TsensIo io = model.io();
  Monitor m;
  m.poll(io);
  for (uint32_t off : model.offsets) CHECK(off >= kTmStatus && off < kTmStatus + 4 * kMaxSensors && !(off & 3));
  CHECK(m.implausible() == 1);
  CHECK(m.usable(1, 3) && m.deci(1, 3) == 523);
  CHECK(!m.usable(2, 5) && !m.usable(3, 2));
  CHECK(m.invalidReads() == 2 + kReadTries);                // two retries of [1][3], every try of [3][2]
  int32_t d = 0;
  unsigned c = 0, s = 0;
  CHECK(m.hottestAll(d, c, s) && d == 523 && c == 1 && s == 3);
  CHECK(m.hottest(1, d, s, 1, 4) && d == 523 && s == 3);
  CHECK(m.hottest(0, d, s) && d == 400);
  // stale: [1][3] loses VALID for good: it stands in for kStalePolls polls, then it is out
  model.status[1][3] &= ~kStatusValid;
  for (unsigned i = 0; i < kStalePolls; ++i) {
    m.poll(io);
    CHECK(m.usable(1, 3) && m.deci(1, 3) == 523 && !(m.freshMask(1) & (1u << 3)));
  }
  m.poll(io);
  CHECK(!m.usable(1, 3));
  CHECK(m.staleUses() == kStalePolls);
  // an unmapped controller: no load at all, nothing usable
  model.mapped[0] = false;
  model.offsets.clear();
  const unsigned before = model.reads;
  m.poll(io);
  CHECK(!m.usableMask(0) && m.usableMask(2));
  CHECK(model.reads - before == (7 - 1 + kReadTries) + 16 + (9 - 1 + kReadTries));   // [1][3] and [3][2] retried
  // nothing mapped: nothing usable
  for (bool &b : model.mapped) b = false;
  m.poll(io);
  CHECK(!m.anyUsable());
}

// ------------------------------------------------------------------------------------------------------------- policy
struct Scene {
  TsensModel model;
  Monitor m;
  Policy p;
  uint64_t now = 100000, stepMs = 1000;
  Scene() { model.all(450); }
  PolicyEvent step(unsigned c, unsigned s, int32_t deci)
  {
    model.all(450);
    model.status[c][s] = statusWord(deci, true);
    m.poll(model.io());
    now += stepMs;
    return p.update(m, now);
  }
};

static void testPolicy()
{
  Scene x;
  PolicyEvent e = x.step(0, 1, 500);
  CHECK(e.after == State::normal && !e.changed());
  e = x.step(1, 2, 850);                                   // warm at 85.0
  CHECK(e.after == State::warm && e.changed() && e.controller == 1 && e.sensor == 2 && e.deci == 850);
  CHECK(x.step(1, 2, 801).after == State::warm);          // held down to 80.0 (5 C hysteresis)
  CHECK(x.step(1, 2, 800).after == State::warm);
  CHECK(x.step(1, 2, 799).after == State::normal);
  CHECK(x.step(0, 2, 1060).after == State::warm);         // cpu-0-0-1 passes at 118 (capped to 110 by the critical 115)
  CHECK(x.step(0, 1, 1050).after == State::passive);      // cpu-0-0-0 passes at 105
  CHECK(x.step(0, 1, 1000).after == State::passive);      // held to 100.0
  CHECK(x.step(0, 1, 999).after == State::warm);
  CHECK(x.step(0, 2, 1099).after == State::warm);
  CHECK(x.step(0, 2, 1100).after == State::passive);
  CHECK(x.step(0, 2, 1020).after == State::passive);      // 110 - 8 = 102: held
  CHECK(x.step(0, 2, 1019).after == State::warm);
  CHECK(x.step(3, 8, 1049).after == State::warm);         // ddr has no passive trip: the SoC's lowest (105) counts
  CHECK(x.step(3, 8, 1050).after == State::passive);
  CHECK(x.step(3, 8, 999).after == State::warm);
  // critical: 125 - 10 = 115, for 1 s and two updates in a row
  e = x.step(2, 3, 1150);
  CHECK(e.after == State::passive && e.criticalPending);
  e = x.step(2, 3, 1149);                                  // the streak breaks
  CHECK(e.after == State::passive && !e.criticalPending);
  e = x.step(2, 3, 1150);
  CHECK(e.after == State::passive && e.criticalPending);
  e = x.step(2, 3, 1160);
  CHECK(e.after == State::critical && e.controller == 2 && e.sensor == 3 && e.deci == 1160 && !e.criticalPending);
  CHECK(x.step(2, 3, 1100).after == State::critical);     // held to 110.0
  CHECK(x.step(2, 3, 1099).after == State::passive);      // gpuss2 passes at 105: still passive
  CHECK(x.step(2, 3, 999).after == State::warm);
  CHECK(x.step(2, 3, 600).after == State::normal);
  CHECK(x.p.peak() == State::critical);
  // straight to critical from normal after two updates 1 s apart (no stop in between)
  Scene y;
  CHECK(y.step(1, 1, 1300).after == State::passive);
  CHECK(y.step(1, 1, 1300).after == State::critical);
  // fast updates (20 ms): the critical threshold must hold for 1 s, not for two updates
  Scene f;
  f.stepMs = 20;
  for (unsigned i = 0; i < 50; ++i) {
    e = f.step(1, 1, 1200);
    CHECK(e.after == State::passive && e.criticalPending);
  }
  CHECK(f.step(1, 1, 1200).after == State::critical);      // 50 x 20 ms after the first
  // unknown: no usable sensor for 5 s (and two updates); the state is kept meanwhile
  Scene z;
  CHECK(z.step(0, 5, 900).after == State::warm);
  for (bool &b : z.model.mapped) b = false;
  for (unsigned i = 0; i < 5; ++i) {
    z.m.poll(z.model.io());
    z.now += 1000;
    e = z.p.update(z.m, z.now);
    CHECK(!e.unknownAfter && e.after == State::warm);
  }
  z.m.poll(z.model.io());
  z.now += 1000;
  e = z.p.update(z.m, z.now);
  CHECK(e.unknownAfter && e.changed() && e.after == State::warm);
  for (bool &b : z.model.mapped) b = true;
  e = z.step(0, 5, 500);
  CHECK(!e.unknownAfter && e.unknownBefore && e.after == State::normal);
  // a custom configuration: warm at 70, margin 20 (critical at 105 for every sensor)
  PolicyConfig pc;
  pc.warmDeci = 700;
  pc.criticalMarginDeci = 200;
  Scene w;
  w.p = Policy(pc);
  CHECK(w.step(0, 13, 700).after == State::warm);
  CHECK(w.step(0, 13, 999).after == State::warm);
  CHECK(w.step(0, 13, 1000).after == State::passive);      // cpuss-0-0: passive 105 capped to 5 C under the critical 105
  CHECK(w.step(0, 13, 1050).after == State::passive);      // critical (125 - 20) pending
  CHECK(w.step(0, 13, 1050).after == State::critical);
}

// ------------------------------------------------------------------------------------------------------------- SCMI
struct Cpucp {
  uint32_t mem[scmi::kShmemBytes / 4] = {};
  uint64_t now = 0;
  uint32_t version = 0x40000;
  bool indexing = true, neverFree = false, neverAnswer = false, errorBit = false, wrongHeader = false;
  unsigned page = 8, rings = 0;
  uint32_t current[2] = {3, 15};
  std::vector<uint32_t> khz[2];
  std::vector<uint32_t> headers;
  std::map<uint32_t, uint32_t> writes;   // offset -> count
  int32_t setStatus = 0;
  Cpucp()
  {
    khz[0] = {384000, 556800, 748800, 960000, 1132800, 1324800, 1516800, 1708800, 1900800, 2112000, 2323200, 2534400, 2745600,
              2956800, 3168000, 3532800};
    khz[1] = {1017600, 1228800, 1440000, 1651200, 1862400, 2073600, 2284800, 2496000, 2707200, 2918400, 3129600, 3340800, 3552000,
              3763200, 4089600, 4320000};
    mem[scmi::kChanStatus / 4] = scmi::kStatusFree;
  }
  static uint32_t rd(void *c, uint32_t off) { return static_cast<Cpucp *>(c)->mem[off / 4]; }
  static void wr(void *c, uint32_t off, uint32_t v)
  {
    Cpucp &m = *static_cast<Cpucp *>(c);
    m.mem[off / 4] = v;
    ++m.writes[off];
  }
  static uint64_t nowUs(void *c) { return static_cast<Cpucp *>(c)->now; }
  static void pause(void *c, uint32_t us) { static_cast<Cpucp *>(c)->now += us; }
  static void ring(void *c) { static_cast<Cpucp *>(c)->serve(); }
  scmi::Io io() { return {this, rd, wr, ring, nowUs, pause}; }
  void reply(int32_t status, std::initializer_list<uint32_t> values)
  {
    mem[scmi::kPayload / 4] = uint32_t(status);
    unsigned i = 1;
    for (uint32_t v : values) mem[scmi::kPayload / 4 + i++] = v;
    mem[scmi::kLength / 4] = 4 + 4 * i;
  }
  void serve()
  {
    ++rings;
    now += 5;
    if (neverAnswer) return;
    const uint32_t h = mem[scmi::kHeader / 4];
    headers.push_back(h);
    const scmi::Header hd = scmi::decodeHeader(h);
    const uint32_t *in = &mem[scmi::kPayload / 4];
    const uint32_t a0 = in[0], a1 = in[1];
    if (hd.protocol != scmi::kProtocolPerf || hd.type != 0 || mem[scmi::kLength / 4] < 4) {
      reply(-10, {});
    } else if (hd.message == scmi::kVersion) {
      reply(0, {version});
    } else if (hd.message == scmi::kAttributes) {
      reply(0, {2, 0, 0, 0});
    } else if (hd.message == scmi::kDomainAttributes) {
      if (a0 > 1) reply(-4, {});
      else {
        const uint32_t attr = scmi::kDomSetLevel | (indexing ? scmi::kDomLevelIndexing : 0);
        const char *n = a0 ? "NCC1" : "NCC0";
        uint32_t name[4] = {};
        memcpy(name, n, 4);
        // v3: the level values are in units of 1/10 MHz here, so khz = value x sustainedKhz / sustainedLevel = value x 100
        reply(0, {attr, 1000, a0 ? 4089600u : 3532800u, version >= 0x40000 ? (a0 ? 15u : 15u) : (a0 ? 40896u : 35328u), name[0],
                  name[1], name[2], name[3]});
      }
    } else if (hd.message == scmi::kDescribeLevels) {
      if (a0 > 1) {
        reply(-4, {});
      } else {
        const std::vector<uint32_t> &k = khz[a0];
        const unsigned n = a1 >= k.size() ? 0 : std::min<unsigned>(page, unsigned(k.size() - a1));
        mem[scmi::kPayload / 4] = 0;
        mem[scmi::kPayload / 4 + 1] = n | uint32_t(k.size() - a1 - n) << 16;
        unsigned w = 2;
        for (unsigned i = 0; i < n; ++i) {
          const unsigned idx = a1 + i;
          if (version >= 0x40000) {
            // reverse order inside a page: the parser must sort
            const unsigned j = a1 + (n - 1 - i);
            mem[scmi::kPayload / 4 + w++] = 100 + j;   // perf value (opaque in indexing mode)
            mem[scmi::kPayload / 4 + w++] = 0;
            mem[scmi::kPayload / 4 + w++] = 30;        // latency
            mem[scmi::kPayload / 4 + w++] = k[j];
            mem[scmi::kPayload / 4 + w++] = j;
          } else {
            mem[scmi::kPayload / 4 + w++] = k[idx] / 100;
            mem[scmi::kPayload / 4 + w++] = 0;
            mem[scmi::kPayload / 4 + w++] = 30;
          }
        }
        mem[scmi::kLength / 4] = 4 + 4 * w;
      }
    } else if (hd.message == scmi::kLevelGet) {
      if (a0 > 1) reply(-4, {});
      else reply(0, {keyOf(a0, current[a0])});
    } else if (hd.message == scmi::kLevelSet) {
      if (setStatus) {
        reply(setStatus, {});
      } else {
        int found = -1;
        for (unsigned i = 0; i < khz[a0 & 1].size(); ++i)
          if (keyOf(a0 & 1, i) == a1) found = int(i);
        if (a0 > 1 || found < 0) reply(-5, {});
        else {
          current[a0] = unsigned(found);
          reply(0, {});
        }
      }
    } else {
      reply(-1, {});
    }
    if (wrongHeader) mem[scmi::kHeader / 4] ^= 1u << 18;
    mem[scmi::kChanStatus / 4] = neverFree ? 0 : scmi::kStatusFree | (errorBit ? scmi::kStatusError : 0);
  }
  uint32_t keyOf(unsigned d, unsigned i) const
  {
    if (version >= 0x40000 && indexing) return i;
    if (version >= 0x40000) return 100 + i;
    return khz[d][i] / 100;
  }
};

static void testScmi()
{
  CHECK(scmi::header(scmi::kLevelSet, scmi::kProtocolPerf, 0x3ff) == (0x7u | 0x13u << 10 | 0x3ffu << 18));
  const scmi::Header h = scmi::decodeHeader(scmi::header(0x8, 0x13, 0x405));   // the token wraps at 10 bits
  CHECK(h.message == 8 && h.protocol == 0x13 && h.type == 0 && h.token == 5);
  CHECK(scmi::kDoorbell == 0x104 && scmi::kPayloadWords == 121);

  for (int v = 0; v < 3; ++v) {
    Cpucp cp;
    cp.version = v == 2 ? 0x30000 : 0x40000;
    cp.indexing = v == 0;
    scmi::Channel ch;
    ch.io = cp.io();
    scmi::Protocol p;
    CHECK(!scmi::queryProtocol(ch, p) && p.version == cp.version && p.domains == 2);
    for (unsigned d = 0; d < 2; ++d) {
      scmi::Domain dom;
      CHECK(!scmi::describeDomain(ch, p.version, d, dom));
      CHECK(dom.count == 16 && dom.canSet && dom.indexing == (v == 0) && dom.rateLimitUs == 1000);
      CHECK(!strcmp(dom.name, d ? "NCC1" : "NCC0"));
      for (unsigned i = 0; i < dom.count; ++i) {
        CHECK(dom.levels[i].khz == cp.khz[d][i]);
        CHECK(dom.levels[i].key == cp.keyOf(d, i));
        CHECK(dom.levels[i].latencyUs == 30);
        if (i) CHECK(dom.levels[i].khz > dom.levels[i - 1].khz);
      }
      // sustained: 3532.8 / 4089.6 MHz -> the fastest level at or under it
      CHECK(dom.levels[dom.sustained].khz == (d ? 4089600u : 3532800u));
      int idx = -1;
      CHECK(!scmi::levelGet(ch, dom, d, idx) && idx == int(cp.current[d]));
      CHECK(!scmi::levelSet(ch, dom, d, 5) && cp.current[d] == 5);
      CHECK(!scmi::levelGet(ch, dom, d, idx) && idx == 5);
      CHECK(scmi::levelSet(ch, dom, d, 16) == -2);           // out of the table: refused before any message
      CHECK(scmi::fastestAtMost(dom, 2000000) == (d ? 4u : 8u));
      CHECK(scmi::slowestAtLeast(dom, 2000000) == (d ? 5u : 9u));
      CHECK(scmi::fastestAtMost(dom, 1) == 0 && scmi::slowestAtLeast(dom, 9000000) == 15);
    }
    // the tokens count up, every message is protocol 0x13 and a command
    for (size_t i = 0; i < cp.headers.size(); ++i) {
      const scmi::Header hd = scmi::decodeHeader(cp.headers[i]);
      CHECK(hd.protocol == 0x13 && hd.type == 0 && hd.token == (i & 0x3ff));
    }
    CHECK(cp.rings == cp.headers.size() && ch.messages == cp.rings && !ch.failures && !ch.lastError);
    CHECK(cp.writes[scmi::kChanFlags] == ch.messages);     // polled: flags written (0) on every message
  }
  // an SCMI error status is returned as is and is not a transport failure
  {
    Cpucp cp;
    scmi::Channel ch;
    ch.io = cp.io();
    scmi::Domain dom;
    CHECK(scmi::describeDomain(ch, 0x40000, 2, dom) == -4 && !ch.lastError);
    CHECK(!scmi::describeDomain(ch, 0x40000, 1, dom));
    cp.setStatus = -3;
    CHECK(scmi::levelSet(ch, dom, 1, 2) == -3 && !ch.lastError && ch.failures == 2);
  }
  // transport failures, bounded in (virtual) time and sticky
  struct Case {
    bool neverFree, neverAnswer, errorBit, wrongHeader;
    int32_t expect;
  } cases[] = {
      {false, true, false, false, scmi::kErrTimeout},
      {false, false, true, false, scmi::kErrChannel},
      {false, false, false, true, scmi::kErrWrongAnswer},
  };
  for (const Case &k : cases) {
    Cpucp cp;
    cp.neverAnswer = k.neverAnswer;
    cp.errorBit = k.errorBit;
    cp.wrongHeader = k.wrongHeader;
    scmi::Channel ch;
    ch.io = cp.io();
    uint32_t out[2];
    unsigned n = 2;
    CHECK(ch.call(scmi::kVersion, nullptr, 0, out, &n) == k.expect && n == 0 && ch.lastError == k.expect);
    CHECK(cp.now <= ch.freeWaitUs + ch.replyWaitUs + 100);
  }
  {
    Cpucp cp;
    cp.mem[scmi::kChanStatus / 4] = 0;   // someone else's message in flight: never free
    scmi::Channel ch;
    ch.io = cp.io();
    uint32_t out[1];
    unsigned n = 1;
    CHECK(ch.call(scmi::kVersion, nullptr, 0, out, &n) == scmi::kErrBusy && cp.rings == 0 && cp.writes.empty());
    CHECK(cp.now >= ch.freeWaitUs && cp.now <= ch.freeWaitUs + 20);
  }
  {
    scmi::Channel ch;   // no io
    unsigned n = 0;
    CHECK(ch.call(scmi::kVersion, nullptr, 0, nullptr, &n) == scmi::kErrNoChannel);
  }
  // a malformed DESCRIBE_LEVELS (more entries claimed than words) is not trusted
  {
    scmi::Domain dom{};
    uint32_t out[4] = {5, 1, 2, 3};
    uint32_t remaining = 0;
    CHECK(scmi::parseLevels(dom, 0x40000, out, 4, remaining) == -1 && !dom.count);
  }
}

// ------------------------------------------------------------------------------------------------------------- governor
static const uint32_t kNcc1[16] = {1017600, 1228800, 1440000, 1651200, 1862400, 2073600, 2284800, 2496000,
                                   2707200, 2918400, 3129600, 3340800, 3552000, 3763200, 4089600, 4320000};

static TickInput tick(uint32_t busy, PowerSource p = PowerSource::battery) { return {busy, p}; }
static ThermalInput cool() { return {true, -300, -250, false}; }        // 75 C on a 105 C trip
static ThermalInput hot() { return {true, 0, 50, false}; }             // at the trip
static ThermalInput between() { return {true, -20, 30, false}; }       // 103 C: under the trip, over the release point
static ThermalInput cut() { return {true, 150, 200, true}; }

// P9 (2026-11-12): the load window leaves PianoThermal's own time out, and the window with the clock measurement makes no decision.
static void testLoadWindow()
{
  CHECK(busyPermilleExcluding(100, 25, 0) == 750 && busyPermilleExcluding(100, 25, 25) == 500);
  CHECK(busyPermilleExcluding(100, 25, 75) == 0 && busyPermilleExcluding(100, 25, 200) == 0);   // own time >= used: 0, no wrap
  CHECK(busyPermilleExcluding(100, 100, 5) == 0 && busyPermilleExcluding(0, 0, 0) == 0);
  const uint64_t ms = 1000000;
  CoreLoad c;
  CHECK(c.read(0, 0, 0, false) == -1);                               // the first reading only starts the window
  // a core idle but for the governor's own 10 ms polls (100 us each over 100 ms: 1 ms): 0 busy, not 1 %
  CHECK(c.read(100 * ms, 99 * ms, 1 * ms, false) == 0);
  // the same core with a spinner for half the window, plus the polls: 50 %, the polls not counted
  CHECK(c.read(200 * ms, 99 * ms + 49 * ms, 2 * ms, false) == 500);
  // the clock measurement ran on every core (2 ms of adds here, outside the own-time count): the window is dropped
  CHECK(c.read(300 * ms, 147 * ms + 98 * ms, 2 * ms, true) == -1);
  CHECK(c.read(400 * ms, 245 * ms + 100 * ms, 2 * ms, false) == 0);   // the next window is a normal one again
  // a failed reading: the next reading starts a window instead of spanning the gap
  c.fail();
  CHECK(c.read(500 * ms, 345 * ms + 10 * ms, 2 * ms, false) == -1 && c.read(600 * ms, 355 * ms + 100 * ms, 2 * ms, false) == 0);
  // the 10 ms scheduler-tick quantisation of the idle time (PianoThermal.cpp idleNs) shows as up to 10 % on an idle core; no wrap when
  // a later reading's idle exceeds the wall time
  CHECK(c.read(700 * ms, 455 * ms + 90 * ms, 2 * ms, false) == 100 && c.read(800 * ms, 545 * ms + 110 * ms, 2 * ms, false) == 0);
  // the governor: a tick without a busy figure is no decision (enforce only); the old code made it a 0 % tick, a step down
  GovernorConfig gv;
  gv.externalKhz = capKhz(4089);
  Governor g;
  g.init(CpuMode::govern, gv, kNcc1, 16, 14, 14);
  for (int i = 0; i < 2; ++i) CHECK(g.enforce(PowerSource::external) == 14);
  CHECK(g.tick({1000, PowerSource::external}) == 14 && g.level() == 14);
}

static void testGovernor()
{
  CHECK(busyPermille(100, 25) == 750 && busyPermille(100, 100) == 0 && busyPermille(100, 150) == 0 && busyPermille(0, 0) == 0);
  CHECK(capKhz(1785) == 1785999 && capKhz(0) == 0);
  GovernorConfig gc;
  gc.capKhz = capKhz(1958);
  Governor g;
  // fixed: the fastest level <= 1958 MHz (1862.4 here), whatever the load or the power source
  g.init(CpuMode::fixed, gc, kNcc1, 16, 14, 15);
  CHECK(g.tick(tick(0)) == 4 && g.tick(tick(1000, PowerSource::external)) == 4 && g.enforce(PowerSource::battery) == 4);
  // the thermal limit: at the trip one level down per stepMs (20 ms), under the release point up one per riseMs (1 s)
  uint64_t t = 1000;
  CHECK(g.thermal(hot(), t) && g.limit() == 14);
  CHECK(!g.thermal(hot(), t + 5) && g.limit() == 14);           // 5 ms later: not yet (one step per 10 ms)
  t += 20;
  for (int i = 0; i < 9; ++i, t += 20) g.thermal(hot(), t);
  CHECK(g.limit() == 5 && g.enforce(PowerSource::battery) == 4);
  g.thermal(hot(), t);
  t += 20;
  g.thermal(hot(), t);
  CHECK(g.limit() == 3 && g.enforce(PowerSource::battery) == 3);
  CHECK(!g.thermal(between(), t += 20) && g.limit() == 3);     // in between: held
  CHECK(!g.thermal(cool(), t += 20) && g.limit() == 3);        // the cool spell starts
  CHECK(!g.thermal(cool(), t += 500) && g.limit() == 3);
  CHECK(g.thermal(cool(), t += 500) && g.limit() == 4 && g.enforce(PowerSource::battery) == 4);
  CHECK(!g.thermal(between(), t += 20));                        // the spell breaks: the next rise is 1 s after the next cool
  CHECK(!g.thermal(cool(), t += 20) && !g.thermal(cool(), t += 990) && g.thermal(cool(), t += 10) && g.limit() == 5);
  CHECK(g.thermal(cut(), t += 1) && g.limit() == 0 && g.enforce(PowerSource::battery) == 0);
  ThermalInput none{false, 0, 0, false};
  g.thermal(cool(), t += 2000);
  g.thermal(cool(), t += 2000);
  CHECK(g.limit() == 1 && g.thermal(none, t += 1) && g.limit() == 0);   // no reading: the slowest at once
  // report: never moves
  g.init(CpuMode::report, gc, kNcc1, 16, 14, 15);
  g.thermal(cut(), 5000);
  CHECK(g.tick(tick(1000)) == 15 && g.enforce(PowerSource::battery) == 15);

  // the governor with Ubuntu's caps: Power Saver on battery (1958.4 on cluster 1), Balanced on power (4089.6, the sustained level)
  GovernorConfig gv;
  gv.batteryKhz = capKhz(1958);
  gv.externalKhz = capKhz(4089);
  gv.hispeedKhz = 1800000;
  g.init(CpuMode::govern, gv, kNcc1, 16, 14, 15);
  CHECK(g.ceiling(PowerSource::battery) == 4 && g.ceiling(PowerSource::unknown) == 4 && g.ceiling(PowerSource::external) == 14);
  CHECK(g.tick(tick(0)) == 4);            // the boot level (4320, boost) is above the battery cap: down to it at once
  CHECK(g.tick(tick(0)) == 4);
  CHECK(g.tick(tick(0)) == 0);            // idle: the slowest after downTicks (3) ticks that wanted it
  CHECK(g.tick(tick(950)) == 4);          // saturated at 1017.6: hispeed (the slowest >= 1800 = 1862.4), also the battery cap
  CHECK(g.tick(tick(950, PowerSource::external)) == 6);    // 1862.4 x 0.95 / 0.8 = 2211.6 -> 2284.8
  CHECK(g.tick(tick(1000, PowerSource::external)) == 9);   // 2284.8 x 1.25 = 2856 -> 2918.4
  CHECK(g.tick(tick(1000, PowerSource::external)) == 13);  // 3648 -> 3763.2
  CHECK(g.tick(tick(1000, PowerSource::external)) == 14);  // 4704 -> capped at the sustained 4089.6 (no boost)
  // down hysteresis: two low ticks and one at the current need: no drop
  g.tick(tick(100, PowerSource::external));
  g.tick(tick(100, PowerSource::external));
  CHECK(g.level() == 14);
  CHECK(g.tick(tick(800, PowerSource::external)) == 14);   // 80 % busy wants exactly the current level
  // three low ticks: down to the fastest of what they wanted
  g.tick(tick(300, PowerSource::external));                // 4089.6 x 0.3 / 0.8 = 1533.6 -> 1651.2
  g.tick(tick(600, PowerSource::external));                // 3067.2 -> 3129.6 (10)
  CHECK(g.level() == 14);
  CHECK(g.tick(tick(100, PowerSource::external)) == 10);
  // the power source back to battery while above its cap: at once, by tick or by enforce
  CHECK(g.enforce(PowerSource::battery) == 4);
  g.tick(tick(1000, PowerSource::external));
  CHECK(g.level() > 4 && g.tick(tick(1000, PowerSource::battery)) == 4);
  // the floor
  GovernorConfig gf = gv;
  gf.floorKhz = 1400000;
  g.init(CpuMode::govern, gf, kNcc1, 16, 14, 0);
  CHECK(g.tick(tick(0)) == 2);
  for (int i = 0; i < 5; ++i) g.tick(tick(0));
  CHECK(g.level() == 2);
  // thermal on top of the governor: at the trip the limit steps down every 20 ms even under full load; cut at once; up 1 s a level
  g.init(CpuMode::govern, gv, kNcc1, 16, 14, 0);
  for (int i = 0; i < 5; ++i) g.tick(tick(1000, PowerSource::external));
  CHECK(g.level() == 14);
  t = 50000;
  for (int i = 0; i < 10; ++i, t += 20) g.thermal(hot(), t);
  CHECK(g.limit() == 5 && g.enforce(PowerSource::external) == 5 && g.tick(tick(1000, PowerSource::external)) == 5);
  CHECK(g.thermal(cut(), t) && g.enforce(PowerSource::external) == 0);
  g.thermal(cool(), t += 20);
  g.thermal(cool(), t += 1000);
  CHECK(g.limit() == 1 && g.tick(tick(1000, PowerSource::external)) == 1);
  // a refused level: the bookkeeping follows the caller
  g.setLevel(0);
  CHECK(g.level() == 0);
  // no cap at all: the sustained level is the top (Linux: boost off)
  GovernorConfig none2;
  g.init(CpuMode::govern, none2, kNcc1, 16, 14, 0);
  for (int i = 0; i < 20; ++i) g.tick(tick(1000));
  CHECK(g.level() == 14);
}

// ------------------------------------------------------------------------------------------------------------- profile, sleep
// PianoPolicy's profile (docs/policy.md 5.2): what the governor is handed as its power source, and so its ceiling; the fixed level
// ignores it. The sleep plan: what kOff and kOn do, once each.
static void testProfileAndSleep()
{
  CHECK(profileValid(0) && profileValid(1) && profileValid(2) && !profileValid(3) && !profileValid(~uintptr_t(0)));
  const PowerSource sources[3] = {PowerSource::unknown, PowerSource::battery, PowerSource::external};
  for (PowerSource p : sources) {
    CHECK(profilePower(p, Profile::bySource) == p);
    CHECK(profilePower(p, Profile::saver) == PowerSource::battery);
    CHECK(profilePower(p, Profile::balanced) == PowerSource::external);
  }
  GovernorConfig gv;
  gv.batteryKhz = capKhz(1958);
  gv.externalKhz = capKhz(4089);
  gv.hispeedKhz = 1800000;
  Governor g;
  g.init(CpuMode::govern, gv, kNcc1, 16, 14, 0);
  // on external power: Balanced by the source; Power Saver forces the battery cap at once (enforce), Balanced lifts it again
  for (int i = 0; i < 5; ++i) g.tick(tick(1000, profilePower(PowerSource::external, Profile::bySource)));
  CHECK(g.level() == 14);
  CHECK(g.enforce(profilePower(PowerSource::external, Profile::saver)) == 4);
  CHECK(g.ceiling(profilePower(PowerSource::external, Profile::saver)) == 4);
  for (int i = 0; i < 5; ++i) g.tick(tick(1000, profilePower(PowerSource::external, Profile::saver)));
  CHECK(g.level() == 4);
  // on battery with Balanced: the external caps
  for (int i = 0; i < 5; ++i) g.tick(tick(1000, profilePower(PowerSource::battery, Profile::balanced)));
  CHECK(g.level() == 14 && g.ceiling(profilePower(PowerSource::unknown, Profile::balanced)) == 14);
  // back to the source on battery: the battery cap at once
  CHECK(g.enforce(profilePower(PowerSource::battery, Profile::bySource)) == 4);
  // the fixed level: no power caps, the profile changes nothing
  GovernorConfig gc;
  gc.capKhz = capKhz(1958);
  g.init(CpuMode::fixed, gc, kNcc1, 16, 14, 15);
  for (Profile pr : {Profile::bySource, Profile::saver, Profile::balanced})
    CHECK(g.enforce(profilePower(PowerSource::external, pr)) == 4 && g.tick(tick(1000, profilePower(PowerSource::battery, pr))) == 4);
  CHECK(!strcmp(profileName(Profile::saver), "Power Saver") && !strcmp(profileName(Profile::balanced), "Balanced"));

  {   // the default: park on off, unpark on on, no TSENS write either way
    SleepPlan s;
    const SleepSteps first = s.on();   // the power tree's first kOn
    CHECK(!first.park && !first.mask && !first.finish && !first.unpark && !s.asleep());
    const SleepSteps off = s.off();
    CHECK(off.park && !off.mask && !off.finish && !off.unpark && s.asleep());
    const SleepSteps again = s.off();
    CHECK(!again.park && !again.mask);
    const SleepSteps on = s.on();
    CHECK(on.unpark && !on.finish && !on.park && !s.asleep() && s.wakes() == 1);
    CHECK(!s.on().unpark && s.wakes() == 1);
  }
  {   // piano-thermal-sleep-mask=1: masked at off, put back at on, every cycle
    SleepPlan s(true);
    for (int cycle = 0; cycle < 3; ++cycle) {
      const SleepSteps off = s.off();
      CHECK(off.park && off.mask);
      s.maskResult(4);
      const SleepSteps on = s.on();
      CHECK(on.finish && on.unpark);
    }
    CHECK(s.wakes() == 3);
    // another caller prepared first (-1): not ours to finish
    CHECK(s.off().mask);
    s.maskResult(-1);
    const SleepSteps on = s.on();
    CHECK(!on.finish && on.unpark);
    // a mask that found no controller (0) is still finished (finish restores only what prepare saved)
    CHECK(s.off().mask);
    s.maskResult(0);
    CHECK(s.on().finish);
  }
}

// ------------------------------------------------------------------------------------------------------------- cluster
static void testCluster()
{
  TsensModel model;
  model.all(450);
  Monitor m;
  ClusterTrips trips;
  m.poll(model.io());
  ThermalInput in = clusterThermal(m, 0, trips, false);
  CHECK(in.valid && in.hotExcessDeci == 450 - 1050 && in.releaseExcessDeci == 450 - 1000 && !in.cut);
  CHECK(!nearTrip(m, trips));
  // cpu-0-0-1 (118 C, hysteresis 8) at 112: under its trip, over its release point (110)
  model.status[0][2] = statusWord(1120, true);
  m.poll(model.io());
  in = clusterThermal(m, 0, trips, false);
  CHECK(in.hotExcessDeci == -60 && in.releaseExcessDeci == 20 && !in.cut);
  CHECK(nearTrip(m, trips));
  // cpu-0-0-0 (105 C) at 105: at its trip; cpuss-0-0 (sensor 13) does not count for the cluster
  model.all(450);
  model.status[0][1] = statusWord(1050, true);
  model.status[0][13] = statusWord(1140, true);
  m.poll(model.io());
  in = clusterThermal(m, 0, trips, false);
  CHECK(in.hotExcessDeci == 0 && !in.cut);
  CHECK(clusterThermal(m, 1, trips, false).hotExcessDeci == 450 - 1050);
  // the critical threshold (125 - 10) on a cluster sensor, or PianoThermal's critical state
  model.status[1][4] = statusWord(1150, true);
  m.poll(model.io());
  CHECK(clusterThermal(m, 1, trips, false).cut && !clusterThermal(m, 0, trips, false).cut);
  CHECK(clusterThermal(m, 0, trips, true).cut);
  // overrides (absolute): hot 85, release 80, cut 95
  ClusterTrips o;
  o.hotDeci = 850;
  o.releaseDeci = 800;
  o.cutDeci = 950;
  model.all(860);
  m.poll(model.io());
  in = clusterThermal(m, 0, o, false);
  CHECK(in.hotExcessDeci == 10 && in.releaseExcessDeci == 60 && !in.cut);
  // no reading on the cluster's sensors
  model.mapped[1] = false;
  m.poll(model.io());
  CHECK(!clusterThermal(m, 1, trips, false).valid);
  // near: a GPU sensor 10 C under its 105 C trip
  model.all(450);
  model.mapped[1] = true;
  model.status[2][3] = statusWord(949, true);
  m.poll(model.io());
  CHECK(!nearTrip(m, trips));
  model.status[2][3] = statusWord(950, true);
  m.poll(model.io());
  CHECK(nearTrip(m, trips));

  // The 90 C ceiling (PianoThermal.cpp's default piano-cpufreq-hot since 2026-11-12, P9): one cpu-* sensor at 90 C trips the
  // cluster; the release point 85 C; the cut still 115 C. The stock trips are only the ceiling's upper bound.
  ClusterTrips ceiling;
  ceiling.hotDeci = kCeilingDeci;
  model.all(450);
  model.status[0][5] = statusWord(899, true);
  m.poll(model.io());
  in = clusterThermal(m, 0, ceiling, false);
  CHECK(in.hotExcessDeci == -1 && in.releaseExcessDeci == 899 - 850 && !in.cut);
  CHECK(nearTrip(m, ceiling) && !nearTrip(m, trips));                 // 80 C and up is near the ceiling, not near 105 C
  model.status[0][5] = statusWord(900, true);
  m.poll(model.io());
  CHECK(clusterThermal(m, 0, ceiling, false).hotExcessDeci == 0);
  model.status[0][5] = statusWord(849, true);
  m.poll(model.io());
  CHECK(clusterThermal(m, 0, ceiling, false).releaseExcessDeci < 0);   // under the release point: the limit may rise again
  model.status[0][5] = statusWord(799, true);
  m.poll(model.io());
  CHECK(!nearTrip(m, ceiling));
  // a ceiling above a zone's stock trip is capped at that trip: 110 C asked, the 105 C zones trip at 105 (release 100), the 118 C
  // zone cpu-0-0-1 at 110 (release 105)
  ClusterTrips high;
  high.hotDeci = 1100;
  model.all(450);
  model.status[0][1] = statusWord(1050, true);                        // cpu-0-0-0, stock 105 C
  m.poll(model.io());
  in = clusterThermal(m, 0, high, false);
  CHECK(in.hotExcessDeci == 0 && in.releaseExcessDeci == 50);
  CHECK(sensorHot(kMap[0].sensors[1], high) == 1050 && sensorRelease(kMap[0].sensors[1], high) == 1000);
  CHECK(sensorHot(kMap[0].sensors[2], high) == 1100 && sensorRelease(kMap[0].sensors[2], high) == 1050);
  // the stock trips (piano-cpufreq-hot=0) and a ceiling with its own release point
  CHECK(sensorHot(kMap[0].sensors[2], trips) == 1180 && sensorRelease(kMap[0].sensors[2], trips) == 1100);
  ClusterTrips own;
  own.hotDeci = 900;
  own.releaseDeci = 870;
  CHECK(sensorHot(kMap[1].sensors[1], own) == 900 && sensorRelease(kMap[1].sensors[1], own) == 870);
  own.releaseDeci = 950;                                              // a release point at or above the trip: the zone's hysteresis
  CHECK(sensorRelease(kMap[1].sensors[1], own) == 850);
  // the governor on the ceiling: a cluster at its top level with a sensor at 91 C steps down one level per 10 ms
  GovernorConfig gv;
  gv.externalKhz = capKhz(3532);
  gv.stepMs = 10;
  Governor g;
  g.init(CpuMode::govern, gv, kNcc1, 16, 14, 14);
  model.all(450);
  model.status[1][2] = statusWord(910, true);
  m.poll(model.io());
  uint64_t t = 10000;
  for (int i = 0; i < 5; ++i, t += 10) g.thermal(clusterThermal(m, 1, ceiling, false), t);
  CHECK(g.limit() == 10 && g.enforce(PowerSource::external) == 10);
  // with the stock trips the same reading changes nothing (the old behaviour)
  Governor old;
  old.init(CpuMode::govern, gv, kNcc1, 16, 14, 14);
  for (int i = 0; i < 5; ++i, t += 10) old.thermal(clusterThermal(m, 1, trips, false), t);
  CHECK(old.limit() == 15);
}

// ------------------------------------------------------------------------------------------------------------- interrupts, polling
static void testInterruptsAndPolling()
{
  CHECK(lowerIntBits(15) == 0x7fff && lowerIntBits(16) == 0xffff && lowerIntBits(7) == 0x7f && upperIntBits(9) == 0x1ff0000);
  const uint32_t ul = encodeUpperLower(700, 850);
  const Thresholds th = decodeThresholds(ul, 0);
  CHECK(th.lowerDeci == 700 && th.upperDeci == 850 && (ul >> 24) == 0);
  CHECK(decodeThresholds(encodeUpperLower(-50, 1200), 0).lowerDeci == -50);
  // suspend: every lower interrupt masked (and the upper ones while nothing services them), other bits kept, all pulsed clear
  SuspendMask sm = suspendMask(0, 15, false);
  CHECK(sm.mask == 0x7fff && sm.clear == 0x7fff);
  sm = suspendMask(0x00030000, 7, true);
  CHECK(sm.mask == (0x7fu | 0x7f0000u) && sm.clear == (0x7fu | 0x7f0000u));
  sm = suspendMask(0x80000001u, 9, false);
  CHECK(sm.mask == (0x80000000u | 0x1ffu) && sm.clear == 0x1ff);
  // poll tiers
  TsensModel model;
  model.all(450);
  Monitor m;
  ClusterTrips trips;
  m.poll(model.io());
  CHECK(pollTier(m, trips, false, false, 700) == Poll::base);
  CHECK(pollTier(m, trips, true, false, 700) == Poll::fast);    // a cluster thermally limited
  // a cluster above the fast-poll clock: every 100 ms while every sensor is under the fast-poll temperature (P9: before 2026-11-12
  // this was the 10 ms tier at any temperature), 10 ms from it up
  CHECK(pollTier(m, trips, false, true, 700) == Poll::clock && pollTier(m, trips, false, true, 700, 700) == Poll::clock);
  model.status[2][6] = statusWord(700, true);
  m.poll(model.io());
  CHECK(pollTier(m, trips, false, false, 700) == Poll::warm);
  CHECK(pollTier(m, trips, false, true, 700) == Poll::fast && pollTier(m, trips, false, true, 700, 750) == Poll::clock);
  model.status[1][1] = statusWord(950, true);                   // 10 C under cpu-1-0-0's 105 C trip
  m.poll(model.io());
  CHECK(pollTier(m, trips, false, false, 700) == Poll::fast);
  // the 90 C ceiling: fast from 80 C on a cpu-* sensor
  ClusterTrips ceiling;
  ceiling.hotDeci = kCeilingDeci;
  model.all(450);
  model.status[0][3] = statusWord(800, true);
  m.poll(model.io());
  CHECK(pollTier(m, ceiling, false, false, 900) == Poll::fast && pollTier(m, trips, false, false, 900) == Poll::base);
  // no reading at all with a fast cluster: fast (blind is not cool)
  model.all(450);
  for (unsigned c = 0; c < kControllers; ++c) model.mapped[c] = false;
  m.poll(model.io());
  CHECK(pollTier(m, trips, false, true, 700) == Poll::fast);
  CHECK(pollPeriodMs(Poll::base, 1000, 250, 10) == 1000 && pollPeriodMs(Poll::warm, 1000, 250, 10) == 250);
  CHECK(pollPeriodMs(Poll::warm, 100, 250, 10) == 100 && pollPeriodMs(Poll::fast, 100, 250, 10) == 10);
  CHECK(pollPeriodMs(Poll::fast, 5, 250, 10) == 5);
  CHECK(pollPeriodMs(Poll::clock, 100, 250, 10) == 100 && pollPeriodMs(Poll::clock, 1000, 250, 10) == 100);   // the governor's tick
  CHECK(pollPeriodMs(Poll::clock, 1000, 250, 10, 50) == 50 && pollPeriodMs(Poll::clock, 20, 250, 10) == 20);
  CHECK(!strcmp(pollName(Poll::clock), "clock") && !strcmp(pollName(Poll::fast), "fast") && kPollTiers == 4);
}

static void printMap()
{
  for (unsigned c = 0; c < kControllers; ++c)
    for (unsigned s = 0; s < kMap[c].count; ++s) {
      const Sensor &sn = kMap[c].sensors[s];
      std::printf("%llx %u %u %s %d %d %d %d\n", (unsigned long long)kMap[c].tm, kMap[c].count, s, sn.name, sn.passiveDeci,
                  sn.passiveHystDeci, sn.hotDeci, sn.criticalDeci);
    }
}

int main(int argc, char **argv)
{
  if (argc > 1 && !strcmp(argv[1], "--map")) {
    printMap();
    return 0;
  }
  testDecode();
  testMap();
  testPoll();
  testPolicy();
  testScmi();
  testLoadWindow();
  testGovernor();
  testProfileAndSleep();
  testCluster();
  testInterruptsAndPolling();
  if (failures) {
    std::printf("thermal_host: %d FAILED\n", failures);
    return 1;
  }
  std::printf("thermal_host: all passed\n");
  return 0;
}
