// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoBacklight's portable parts: BitI2c.h against a bit-level model of a KTZ8866 (address 0x11, register pointer with
// auto-increment, repeated-START reads, NACK for another address, clock stretching); Ktz8866.h's steps, levels and log limit; the
// Controller's register sequences on two register models (survey writes nothing; boot level; display off/on; steps and the floor;
// the lid; failures, retries and giving up; a missing IC; only 0x04/0x05/0x08 ever written, BL_EN bit 7 never, bits 5:0 kept); the
// whole stack (Controller over BitI2c over two bit-level ICs); PianoBacklightClient.h's KeyWatch.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. backlight_host.cpp -o /tmp/backlight_host && /tmp/backlight_host
#include "BitI2c.h"
#include "Ktz8866.h"
#include "PianoBacklightClient.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace pianobacklight;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// The datasheet's reset values (Table 5), with the ID's reserved bits 0b001.
static void resetRegs(uint8_t *r)
{
  std::memset(r, 0, kRegCount);
  r[kRegId] = 0x21; r[kRegCfg1] = 0xfa; r[kRegCfg2] = 0x8d; r[kRegBrtLsb] = 0x07; r[kRegBrtMsb] = 0xff; r[kRegEn] = 0x3f;
  r[kRegLcd1] = 0x18; r[kRegLcd2] = 0x11; r[kRegBoost] = 0x28; r[kRegOutp] = 0x1e; r[kRegOutn] = 0x1c; r[kRegOpt1] = 0x80;
  r[kRegOpt2] = 0x77; r[kRegRamp] = 0x44; r[kRegIfs] = 0xf8;
}

// ------------------------------------------------------------------------------------------------------------- bit-level IC
struct Chip {
  uint8_t address = kAddress;
  uint8_t reg[kRegCount] = {};
  uint8_t pointer = 0;
  bool sdaLow = false, sclLow = false, mSda = true, mScl = true, lastSda = true, lastScl = true;
  enum { idle, addr, ackAddr, wr, ackWr, rd, rdAck, ignore } st = idle;
  int bits = 0, stretch = 0, holding = 0;
  uint8_t shift = 0, byte = 0;
  bool reading = false, masterAck = false, firstByte = false;
  std::vector<std::pair<uint8_t, uint8_t>> writes;   // (register, value)
  unsigned starts = 0, stops = 0;
  bool sda() const { return mSda && !sdaLow; }
  bool scl() const { return mScl && !sclLow; }
  void load() { byte = pointer < kRegCount ? reg[pointer] : 0; ++pointer; sdaLow = !(byte & 0x80); }
  void rising()
  {
    if (st == addr || st == wr) { shift = uint8_t(shift << 1 | (sda() ? 1 : 0)); ++bits; }
    else if (st == rdAck) masterAck = !sda();
  }
  void falling()
  {
    switch (st) {
    case addr:
      if (bits == 8) {
        if ((shift >> 1) == address) { st = ackAddr; reading = shift & 1; sdaLow = true; }
        else st = ignore;
      }
      break;
    case ackAddr:
      sdaLow = false;
      if (stretch) { sclLow = true; holding = stretch; }
      if (reading) { st = rd; bits = 0; load(); }
      else { st = wr; bits = 0; shift = 0; firstByte = true; }
      break;
    case wr:
      if (bits == 8) {
        if (firstByte) pointer = shift;
        else {
          writes.push_back({pointer, shift});
          if (pointer < kRegCount) reg[pointer] = shift;
          ++pointer;
        }
        firstByte = false;
        st = ackWr;
        sdaLow = true;
      }
      break;
    case ackWr: sdaLow = false; st = wr; bits = 0; shift = 0; break;
    case rd:
      if (++bits < 8) sdaLow = !((byte >> (7 - bits)) & 1);
      else { sdaLow = false; st = rdAck; }
      break;
    case rdAck:
      if (masterAck) { st = rd; bits = 0; load(); }
      else { st = ignore; sdaLow = false; }
      break;
    default: break;
    }
  }
  void update()
  {
    const bool s = sda(), c = scl();
    if (c && lastScl && lastSda && !s) { st = addr; bits = 0; shift = 0; ++starts; }   // START or repeated START
    else if (c && lastScl && !lastSda && s) { st = idle; ++stops; }                    // STOP
    else if (c && !lastScl) rising();
    else if (!c && lastScl) falling();
    lastSda = sda();
    lastScl = scl();
  }
};
static Chip gChip[2];
static unsigned gDelays;
static void cSda(void *ctx, bool release) { Chip *c = static_cast<Chip *>(ctx); c->mSda = release; c->update(); }
static void cScl(void *ctx, bool release) { Chip *c = static_cast<Chip *>(ctx); c->mScl = release; c->update(); }
static bool cReadSda(void *ctx) { return static_cast<Chip *>(ctx)->sda(); }
static bool cReadScl(void *ctx) { return static_cast<Chip *>(ctx)->scl(); }
static void cDelay(void *ctx, unsigned)
{
  ++gDelays;
  Chip *c = static_cast<Chip *>(ctx);
  if (c->holding && !--c->holding) { c->sclLow = false; c->update(); }
}

static void testI2c()
{
  Chip &c = gChip[0];
  resetRegs(c.reg);
  BitI2c bus;
  bus.configure({&c, cSda, cScl, cReadSda, cReadScl, cDelay}, 5, 2000);
  uint8_t v = 0;
  const uint8_t id = kRegId;
  CHECK(bus.writeRead(kAddress, &id, 1, &v, 1) == I2c::ok && v == 0x21);
  CHECK(c.writes.empty());
  const uint8_t w[2] = {kRegBrtMsb, 0x73};
  CHECK(bus.write(kAddress, w, 2) == I2c::ok);
  CHECK(c.writes.size() == 1 && c.writes[0].first == kRegBrtMsb && c.writes[0].second == 0x73 && c.reg[kRegBrtMsb] == 0x73);
  // Auto-increment: two bytes read from 0x04.
  uint8_t two[2] = {};
  const uint8_t lsb = kRegBrtLsb;
  CHECK(bus.writeRead(kAddress, &lsb, 1, two, 2) == I2c::ok && two[0] == 0x07 && two[1] == 0x73);
  // Clock stretching after the address; nobody at 0x12; a stuck clock.
  c.stretch = 30;
  CHECK(bus.writeRead(kAddress, &id, 1, &v, 1) == I2c::ok && v == 0x21);
  c.stretch = 0;
  CHECK(bus.write(0x12, w, 2) == I2c::nackAddress);
  CHECK(c.writes.size() == 1);
  CHECK(c.stops >= 5);
}

// ------------------------------------------------------------------------------------------------------------- steps, levels, log limit
static void testSteps()
{
  Steps s;
  s.build(128, false);
  const uint16_t expected[] = {128, 161, 203, 256, 322, 406, 512, 645, 812, 1024, 1290, 1625, 2047};
  CHECK(s.count() == sizeof expected / sizeof expected[0]);
  for (unsigned i = 0; i < s.count() && i < 13; ++i) CHECK(s.at(i) == expected[i]);
  for (unsigned i = 1; i < s.count(); ++i) CHECK(s.at(i) > s.at(i - 1));
  CHECK(s.up(128) == 161 && s.up(150) == 161 && s.up(1500) == 1625 && s.up(1625) == 2047 && s.up(2047) == 2047);
  CHECK(s.down(2047) == 1625 && s.down(1500) == 1290 && s.down(161) == 128 && s.down(128) == 128 && s.down(140) == 128);
  CHECK(s.down(100) == 100);   // under the floor (found so): "down" never raises it
  CHECK(s.up(100) == 128);
  s.build(64, false);
  CHECK(s.at(0) == 64 && s.at(1) == 81 && s.at(s.count() - 1) == 2047);
  s.build(128, true);
  CHECK(s.count() == 17 && s.at(0) == 128 && s.at(16) == 2047 && s.at(8) == 128 + (1919 * 8 + 8) / 16);
  CHECK(codeForPercent(45, 128) == 921 && codeForPercent(0, 128) == 128 && codeForPercent(100, 128) == 2047 && codeForPercent(250, 128) == 2047);
  CHECK(codeForPercent(3, 128) == 128);
  CHECK(percentOf(2047) == 100 && percentOf(921) == 45 && percentOf(0) == 0);
  CHECK(rampUs(0) == 512 && rampUs(1) == 1000 && rampUs(4) == 8000 && rampUs(15) == 16384000);
  CHECK(idIsKtz8866(0x21) && idIsKtz8866(0x20) && !idIsKtz8866(0xff) && !idIsKtz8866(0x00) && !idIsKtz8866(0x61));
  CHECK(enableValue(0x3f, true) == 0x7f && enableValue(0x7f, false) == 0x3f && enableValue(0xff, true) == 0x7f && enableValue(0x15, false) == 0x15);
  CHECK(writable(0x04) && writable(0x05) && writable(0x08) && !writable(0x09) && !writable(0x0f) && !writable(0x15) && !writable(0x02));

  LogLimit l(3, 1000);
  uint32_t d = 0;
  CHECK(l.allow(0, &d) && d == 0);
  CHECK(l.allow(10, &d) && l.allow(20, &d));
  CHECK(!l.allow(30, &d) && !l.allow(40, &d));
  CHECK(l.allow(1000, &d) && d == 2);
  CHECK(l.allow(1001, &d) && d == 0);
}

// ------------------------------------------------------------------------------------------------------------- register-level model
struct Model final : Ics {
  uint8_t reg[kIcs][kRegCount];
  bool present[kIcs] = {true, true};
  bool deaf[kIcs] = {false, false};   // writes acknowledged, not taken
  int failWrites[kIcs] = {0, 0};   // the next N writes of the IC are not acknowledged (-1: all)
  struct Access { unsigned ic; bool write; uint8_t reg, value; };
  std::vector<Access> trace;
  std::vector<std::string> lines;
  bool inBurst = false;
  unsigned bursts = 0;
  uint64_t now = 0;
  Model() { resetRegs(reg[0]); resetRegs(reg[1]); }
  bool read(unsigned ic, uint8_t r, uint8_t *v) override
  {
    inBurst = true;
    if (!present[ic]) return false;
    *v = reg[ic][r];
    if (r == kRegFlag) reg[ic][r] = 0;   // cleared by the read
    trace.push_back({ic, false, r, *v});
    return true;
  }
  bool write(unsigned ic, uint8_t r, uint8_t v) override
  {
    inBurst = true;
    if (!present[ic]) return false;
    if (failWrites[ic]) {
      if (failWrites[ic] > 0) --failWrites[ic];
      return false;
    }
    trace.push_back({ic, true, r, v});
    if (deaf[ic]) return true;
    if (r == kRegEn && (v & kEnSoftReset)) resetRegs(reg[ic]);
    else reg[ic][r] = v;
    return true;
  }
  void done() override { if (inBurst) ++bursts; inBurst = false; }
  void log(const char *line) override { lines.push_back(line); if (std::getenv("BACKLIGHT_VERBOSE")) std::printf("  | %s\n", line); }
  uint64_t nowMs() override { return now += 5; }
  std::vector<Access> writes() const { std::vector<Access> w; for (const Access &a : trace) if (a.write) w.push_back(a); return w; }
  uint16_t code(unsigned ic) const { return codeOf(reg[ic][kRegBrtLsb], reg[ic][kRegBrtMsb]); }
  bool lit(unsigned ic) const { return reg[ic][kRegEn] & kEnBacklight; }
  void setFound(unsigned ic, uint16_t code, bool lit, uint8_t sinks = 0x3f)
  {
    reg[ic][kRegBrtLsb] = uint8_t(code & 7);
    reg[ic][kRegBrtMsb] = uint8_t(code >> 3);
    reg[ic][kRegEn] = uint8_t(sinks | (lit ? kEnBacklight : 0));
  }
  bool logged(const char *needle) const
  {
    for (const std::string &l : lines) if (l.find(needle) != std::string::npos) return true;
    return false;
  }
};

// Every write ever made: one of the three registers, BL_EN without bit 7, an MSB at or above the floor's.
static void checkWhitelist(const Model &m, uint16_t floor)
{
  for (const Model::Access &a : m.trace) {
    if (!a.write) continue;
    CHECK(writable(a.reg));
    if (a.reg == kRegEn) CHECK(!(a.value & kEnSoftReset) && (a.value & kEnSinks) == 0x3f);
    if (a.reg == kRegBrtMsb) CHECK(a.value >= (floor >> 3));
  }
}

static Config config(uint32_t percent = 45, bool force = false, bool lid = false)
{
  Config c;
  c.floor = 128;
  c.bootPercent = percent;
  c.bootForce = force;
  c.lidWatch = lid;
  return c;
}

static void testSurvey()
{
  Model m;
  m.setFound(0, 1500, true, 0x3f);
  m.setFound(1, 1500, true, 0x3f);
  m.reg[0][kRegFlag] = 0x02;
  Controller c(m);
  c.configure(config());
  CHECK(c.survey());
  CHECK(m.writes().empty());
  CHECK(m.bursts == 1 && !m.inBurst);
  CHECK(c.ic(0).foundCode == 1500 && c.ic(0).foundLit && c.ic(1).foundCode == 1500);
  CHECK(m.reg[0][kRegFlag] == 0x02);   // FLAG not read by default
  for (const Model::Access &a : m.trace) CHECK(a.reg != kRegFlag);
  CHECK(m.logged("survey primary: ID 0x21 (KTZ8866, device field 4)"));
  CHECK(m.logged("OVP 40.0 V") && m.logged("linear mapping") && m.logged("code 1500 (73 %"));
  CHECK(m.logged("full scale 30.0 mA per sink") && m.logged("ramps 0x14 0x44 (on 8000 us, off 8000 us)"));

  Model f;
  f.reg[1][kRegFlag] = 0x82;
  Controller cf(f);
  Config cfgFlags = config();
  cfgFlags.readFlags = true;
  cf.configure(cfgFlags);
  CHECK(cf.survey());
  CHECK(f.reg[1][kRegFlag] == 0 && f.logged("FLAG 0x82 LED short BL OVP (now cleared)"));

  // A missing secondary: survey false, start refused, nothing ever written whatever happens.
  Model g;
  g.present[1] = false;
  Controller cg(g);
  cg.configure(config());
  CHECK(!cg.survey());
  CHECK(!cg.start());
  CHECK(g.logged("control: OFF, the secondary did not answer"));
  cg.display(true, 1, 1);
  cg.display(false, 0, 1);
  cg.step(3, "test");
  cg.percent(80, "test");
  cg.retry();
  CHECK(g.writes().empty());
  // Something that is not a KTZ8866 at 0x11.
  Model h;
  h.reg[0][kRegId] = 0xff;
  Controller ch(h);
  ch.configure(config());
  CHECK(!ch.survey() && !ch.start() && h.writes().empty());
}

static void testBootLevel()
{
  // Found high (1500 = 73 %): lowered to 45 % (921), LSB then MSB, primary then secondary; BL_EN already set: not written.
  Model m;
  m.setFound(0, 1500, true);
  m.setFound(1, 1500, true);
  Controller c(m);
  c.configure(config());
  c.survey();
  CHECK(c.start());
  auto w = m.writes();
  CHECK(w.size() == 4);
  if (w.size() == 4) {
    CHECK(w[0].ic == 0 && w[0].reg == kRegBrtLsb && w[0].value == (921 & 7));
    CHECK(w[1].ic == 0 && w[1].reg == kRegBrtMsb && w[1].value == (921 >> 3));
    CHECK(w[2].ic == 1 && w[2].reg == kRegBrtLsb && w[3].ic == 1 && w[3].reg == kRegBrtMsb);
  }
  CHECK(m.code(0) == 921 && m.code(1) == 921 && m.lit(0) && m.lit(1));
  CHECK(m.logged("boot level: found code 1500 (73 %), set 921 (45 %"));
  CHECK(m.logged("write primary: BL_BRT_LSB 0x04 = 0x01, BL_BRT_MSB 0x05 = 0x73: code 921 (45 %), was 1500; read back ok"));
  CHECK(!m.inBurst && !c.pending());
  checkWhitelist(m, 128);

  // Found moderate (600): kept, nothing written.
  Model k;
  k.setFound(0, 600, true);
  k.setFound(1, 600, true);
  Controller ck(k);
  ck.configure(config());
  ck.survey();
  CHECK(ck.start() && k.writes().empty() && ck.code() == 600);
  CHECK(k.logged("not above 921 (45 %): kept"));

  // piano-backlight-level=30 given: set even though lower... and higher.
  Model g;
  g.setFound(0, 300, true);
  g.setFound(1, 300, true);
  Controller cg(g);
  cg.configure(config(30, true));
  cg.survey();
  CHECK(cg.start() && g.code(0) == codeForPercent(30, 128) && g.code(1) == codeForPercent(30, 128));

  // piano-backlight-level=0: as found, even when high.
  Model a;
  a.setFound(0, 2047, true);
  a.setFound(1, 2047, true);
  Controller ca(a);
  ca.configure(config(0));
  ca.survey();
  CHECK(ca.start() && a.writes().empty() && a.logged("boot level: as found, code 2047"));

  // Different codes on the two ICs (both under the cap): both get the primary's.
  Model d;
  d.setFound(0, 700, true);
  d.setFound(1, 650, true);
  Controller cd(d);
  cd.configure(config());
  cd.survey();
  CHECK(cd.start() && d.code(1) == 700 && d.writes().size() == 2 && d.logged("the ICs hold different codes (700, 650)"));

  // Found dark (BL_EN clear): turned on, code first, then BL_EN (bits 5:0 kept).
  Model o;
  o.setFound(0, 800, false);
  o.setFound(1, 800, false);
  Controller co(o);
  co.configure(config());
  co.survey();
  CHECK(co.start() && o.lit(0) && o.lit(1) && o.reg[0][kRegEn] == 0x7f);
  auto wo = o.writes();
  CHECK(wo.size() == 2 && wo[0].reg == kRegEn && wo[0].value == 0x7f && wo[1].reg == kRegEn);
  CHECK(o.logged("found the backlight OFF"));
  checkWhitelist(o, 128);

  // Found code 0 with BL_EN set (the IC's own off): raised to the boot level.
  Model z;
  z.setFound(0, 0, true);
  z.setFound(1, 0, true);
  Controller cz(z);
  cz.configure(config());
  cz.survey();
  CHECK(cz.start() && z.code(0) == 921 && z.code(1) == 921);
}

static void testDisplayAndSteps()
{
  Model m;
  m.setFound(0, 812, true);
  m.setFound(1, 812, true);
  Controller c(m);
  c.configure(config());
  c.survey();
  CHECK(c.start() && m.writes().empty());
  // Off before the first on (boot): ignored.
  c.display(false, 0, 0);
  CHECK(m.writes().empty() && m.lit(0) && m.logged("before the first on: ignored"));
  c.display(true, 1, 1);
  CHECK(m.writes().empty());
  // Display sleep: BL_EN cleared on both (sinks kept), the code untouched.
  const size_t before = m.trace.size();
  c.display(false, 1, 0);
  auto w = m.writes();
  CHECK(w.size() == 2 && w[0].ic == 0 && w[0].reg == kRegEn && w[0].value == 0x3f && w[1].ic == 1 && w[1].value == 0x3f);
  CHECK(!m.lit(0) && !m.lit(1) && m.code(0) == 812 && m.code(1) == 812);
  CHECK(m.trace.size() - before == 6);   // per IC: read BL_EN, write it, read it back
  CHECK(m.logged("write primary: BL_EN 0x08 = 0x3f (was 0x7f): backlight OFF, sinks 0x3f kept; read back ok"));
  // A second off changes nothing.
  c.display(false, 1, 0);
  CHECK(m.writes().size() == 2);
  // A step while dark: remembered, not written.
  c.step(1, "test keys");
  CHECK(m.writes().size() == 2 && c.code() == 1024 && m.logged("(applied when the backlight is on again)"));
  // Wake: the new code first, then BL_EN, on each IC.
  c.display(true, 1, 1);
  w = m.writes();
  CHECK(w.size() == 8);
  if (w.size() == 8) {
    CHECK(w[2].reg == kRegBrtLsb && w[3].reg == kRegBrtMsb && w[4].reg == kRegEn && w[4].value == 0x7f && w[4].ic == 0);
    CHECK(w[5].ic == 1 && w[5].reg == kRegBrtLsb && w[7].ic == 1 && w[7].reg == kRegEn);
  }
  CHECK(m.lit(0) && m.lit(1) && m.code(0) == 1024 && m.code(1) == 1024);
  // Steps: up to full scale and no further, down to the floor and no further.
  for (int i = 0; i < 10; ++i) c.step(1, "test keys");
  CHECK(m.code(0) == 2047 && m.code(1) == 2047 && m.logged("(full scale)"));
  c.step(-20, "test keys");
  CHECK(m.code(0) == 128 && m.code(1) == 128);
  m.now += 20000;   // a new log window (the steps above used this one up)
  c.step(-1, "test keys");
  CHECK(m.code(0) == 128 && m.logged("(the floor)") && m.logged("write/step lines not logged)"));
  c.step(2, "test keys");
  CHECK(m.code(0) == 203);
  c.percent(100, "test");
  CHECK(m.code(0) == 2047 && m.code(1) == 2047);
  c.percent(1, "test");
  CHECK(m.code(0) == 128);
  CHECK(!m.inBurst);
  checkWhitelist(m, 128);
  // The log limit: many steps in a short time leave a count, not a flood.
  Model q;
  q.setFound(0, 812, true);
  q.setFound(1, 812, true);
  Controller cq(q);
  cq.configure(config());
  cq.survey();
  cq.start();
  const size_t lines = q.lines.size();
  for (int i = 0; i < 40; ++i) cq.step(i % 2 ? 1 : -1, "flood");
  CHECK(q.lines.size() - lines <= 24);
}

static void testLid()
{
  // Not watched: nothing written on a closed lid.
  Model m;
  m.setFound(0, 812, true);
  m.setFound(1, 812, true);
  Controller c(m);
  c.configure(config());
  c.survey();
  c.start();
  c.lid(0);
  CHECK(m.writes().empty() && m.logged("lid closed (not acted on: piano-backlight-lid=0)"));
  // Watched: closed -> off, open -> on; with the display off the open lid does not light it.
  Model n;
  n.setFound(0, 812, true);
  n.setFound(1, 812, true);
  Controller d(n);
  d.configure(config(45, false, true));
  d.survey();
  d.start();
  d.lid(1);
  CHECK(n.writes().empty());
  d.lid(0);
  CHECK(!n.lit(0) && !n.lit(1));
  d.lid(1);
  CHECK(n.lit(0) && n.lit(1));
  d.display(true, 1, 1);
  d.display(false, 1, 0);
  CHECK(!n.lit(0));
  d.lid(0);
  d.lid(1);
  CHECK(!n.lit(0) && !n.lit(1));
  d.display(true, 1, 1);
  CHECK(n.lit(0) && n.lit(1));
  d.lid(-1);   // unknown counts as open
  CHECK(n.lit(0));
  checkWhitelist(n, 128);
}

// A system sleep (setPowerState off / on, docs/power.md 4): dark whatever the display says, then back to what the display and the
// lid want; the code kept or the newest one; a failed off retried; nothing but BL_EN and the code ever written.
static void testSleep()
{
  Model m;
  m.setFound(0, 812, true);
  m.setFound(1, 812, true);
  Controller c(m);
  c.configure(config(45, false, true));
  c.survey();
  c.start();
  c.display(true, 1, 1);
  CHECK(m.lit(0) && m.lit(1));
  // off: BL_EN cleared on both, the code kept
  c.sleep(true);
  CHECK(!m.lit(0) && !m.lit(1) && m.code(0) == 812 && m.code(1) == 812 && c.asleep() && !c.pending());
  CHECK(m.logged("system sleep: the backlight off on both ICs (BL_EN cleared, the code kept; HWEN untouched)"));
  const size_t writes = m.writes().size();
  c.sleep(true);   // a second off: nothing
  CHECK(m.writes().size() == writes);
  // while asleep: the display path, a step and the lid light nothing (the step's code is kept for the wake)
  c.display(true, 1, 1);
  c.lid(1);
  c.step(1, "test keys");
  CHECK(!m.lit(0) && !m.lit(1) && m.writes().size() == writes && c.code() == 1024);
  // on: the newest code first, then BL_EN, on both
  c.sleep(false);
  CHECK(m.lit(0) && m.lit(1) && m.code(0) == 1024 && m.code(1) == 1024 && !c.asleep());
  CHECK(m.logged("system wake: the backlight as before the sleep"));
  // the display went off during the sleep (IOMFB's system power state): the wake leaves it dark until the display's on
  c.sleep(true);
  c.display(false, 0, 0);
  c.sleep(false);
  CHECK(!m.lit(0) && !m.lit(1) && m.logged("system wake: the backlight stays off (the display or the lid wants it so)"));
  c.display(true, 1, 1);
  CHECK(m.lit(0) && m.lit(1));
  // the lid closed during the sleep (piano-backlight-lid=1): dark after the wake too
  c.sleep(true);
  c.lid(0);
  c.sleep(false);
  CHECK(!m.lit(0) && !m.lit(1));
  c.lid(1);
  CHECK(m.lit(0) && m.lit(1));
  // an IC that refuses the off write: pending, a retry takes it (the kext retries twice before acknowledging)
  m.failWrites[1] = 1;
  c.sleep(true);
  CHECK(!m.lit(0) && m.lit(1) && c.pending());
  c.retry();
  CHECK(!m.lit(1) && !c.pending());
  c.sleep(false);
  CHECK(m.lit(0) && m.lit(1));
  // the wake before the first display on (a sleep during boot): the boot screen's rule holds, lit as found
  Model b;
  b.setFound(0, 812, true);
  b.setFound(1, 812, true);
  Controller cb(b);
  cb.configure(config());
  cb.survey();
  cb.start();
  cb.sleep(true);
  CHECK(!b.lit(0) && !b.lit(1));
  cb.display(false, 0, 0);   // before the first on: ignored
  cb.sleep(false);
  CHECK(b.lit(0) && b.lit(1));
  checkWhitelist(m, 128);
  checkWhitelist(b, 128);
  // not controlling (a missing IC): a sleep writes nothing
  Model x;
  x.present[1] = false;
  Controller cx(x);
  cx.configure(config());
  cx.survey();
  cx.start();
  cx.sleep(true);
  cx.sleep(false);
  CHECK(x.writes().empty());
}

static void testFailures()
{
  // The secondary refuses one write: the primary goes dark, the secondary stays lit until a retry.
  Model m;
  m.setFound(0, 812, true);
  m.setFound(1, 812, true);
  Controller c(m);
  c.configure(config());
  c.survey();
  c.start();
  c.display(true, 1, 1);
  m.failWrites[1] = 1;
  c.display(false, 1, 0);
  CHECK(!m.lit(0) && m.lit(1) && c.pending());
  CHECK(m.logged("write secondary: BL_EN 0x08 = 0x3f (was 0x7f): backlight OFF, sinks 0x3f kept; WRITE FAILED"));
  c.retry();
  CHECK(!m.lit(1) && !c.pending());
  // A bus that stays dead: control stops after the error limit; no write afterwards.
  m.failWrites[1] = -1;
  c.display(true, 1, 1);
  for (int i = 0; i < 20; ++i) c.retry();
  CHECK(!c.controlling() && m.logged("control: OFF after 8 failed attempts in a row"));
  const size_t writes = m.writes().size();
  m.failWrites[1] = 0;
  c.display(false, 1, 0);
  c.step(1, "test");
  c.retry();
  CHECK(m.writes().size() == writes);
  // A read-back that differs (the IC acknowledged but did not take the code) counts as a failure and is retried.
  Model r;
  r.setFound(0, 812, true);
  r.setFound(1, 812, true);
  Controller cr(r);
  cr.configure(config(30, true));
  cr.survey();
  r.deaf[1] = true;
  cr.start();
  CHECK(r.code(0) == codeForPercent(30, 128) && r.code(1) == 812 && cr.pending() && r.logged("READ BACK DIFFERS"));
  r.deaf[1] = false;
  cr.retry();
  CHECK(r.code(1) == codeForPercent(30, 128) && !cr.pending() && cr.controlling());
}

// ------------------------------------------------------------------------------------------------------------- the whole stack
struct Wire final : Ics {
  BitI2c bus[2];
  bool muxed[2] = {false, false};
  unsigned restores = 0;
  std::vector<std::string> lines;
  uint64_t now = 0;
  Wire()
  {
    for (unsigned i = 0; i < 2; ++i) bus[i].configure({&gChip[i], cSda, cScl, cReadSda, cReadScl, cDelay}, 5, 2000);
  }
  bool read(unsigned ic, uint8_t reg, uint8_t *v) override
  {
    muxed[ic] = true;
    return bus[ic].writeRead(kAddress, &reg, 1, v, 1) == I2c::ok;
  }
  bool write(unsigned ic, uint8_t reg, uint8_t v) override
  {
    muxed[ic] = true;
    const uint8_t b[2] = {reg, v};
    return bus[ic].write(kAddress, b, 2) == I2c::ok;
  }
  void done() override
  {
    for (bool &m : muxed) if (m) { m = false; ++restores; }
  }
  void log(const char *line) override { lines.push_back(line); }
  uint64_t nowMs() override { return now += 3; }
};

static void testStack()
{
  for (Chip &c : gChip) {
    c = Chip();
    resetRegs(c.reg);
    c.reg[kRegBrtLsb] = 1500 & 7;
    c.reg[kRegBrtMsb] = 1500 >> 3;
    c.reg[kRegEn] = 0x7f;
  }
  gChip[1].stretch = 3;   // the secondary stretches the clock after its address
  Wire w;
  Controller c(w);
  c.configure(config());
  CHECK(c.survey());
  CHECK(gChip[0].writes.empty() && gChip[1].writes.empty() && w.restores == 2);
  CHECK(c.start());
  CHECK(codeOf(gChip[0].reg[kRegBrtLsb], gChip[0].reg[kRegBrtMsb]) == 921 && codeOf(gChip[1].reg[kRegBrtLsb], gChip[1].reg[kRegBrtMsb]) == 921);
  c.display(true, 1, 1);
  c.display(false, 1, 0);
  CHECK(gChip[0].reg[kRegEn] == 0x3f && gChip[1].reg[kRegEn] == 0x3f);
  c.display(true, 1, 1);
  CHECK(gChip[0].reg[kRegEn] == 0x7f && gChip[1].reg[kRegEn] == 0x7f);
  c.step(-1, "stack");
  CHECK(codeOf(gChip[1].reg[kRegBrtLsb], gChip[1].reg[kRegBrtMsb]) == 812);
  for (const Chip &chip : gChip)
    for (auto [reg, value] : chip.writes) {
      CHECK(writable(reg));
      if (reg == kRegEn) CHECK(!(value & kEnSoftReset));
    }
  CHECK(gChip[0].lastSda && gChip[0].lastScl && gChip[1].lastSda && gChip[1].lastScl);   // both buses idle
  CHECK(!w.muxed[0] && !w.muxed[1]);
}

// ------------------------------------------------------------------------------------------------------------- the producers' side
static void testKeys()
{
  KeyWatch k;
  CHECK(k.consumer(kUsageBrightnessUp) == 1);
  CHECK(k.consumer(kUsageBrightnessUp) == 0);   // the same report again: no second step
  CHECK(k.consumer(0) == 0);
  CHECK(k.consumer(kUsageBrightnessDown) == -1);
  CHECK(k.consumer(0xe9) == 0 && k.consumer(0) == 0);
  CHECK(k.consumer(kUsageBrightnessUp) == 1 && k.consumer(kUsageBrightnessDown) == -1);
  uint8_t r[9] = {5, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(k.keyboard(r) == 0);
  r[3] = kUsageKey2;
  CHECK(k.keyboard(r) == 0);   // 2 without Right Ctrl
  r[1] = kModRightCtrl;
  CHECK(k.keyboard(r) == 1);   // the combo became true (Ctrl after the key counts too)
  CHECK(k.keyboard(r) == 0);
  r[3] = 0;
  CHECK(k.keyboard(r) == 0);
  r[4] = kUsageKey2;
  CHECK(k.keyboard(r) == 1);
  CHECK(k.keyboard(r) == 0);
  r[5] = kUsageKey1;
  CHECK(k.keyboard(r) == -1);
  r[4] = r[5] = 0;
  CHECK(k.keyboard(r) == 0);
  r[1] = 0x01;   // left Ctrl is not the combo
  r[3] = kUsageKey1;
  CHECK(k.keyboard(r) == 0);
}

// piano-backlight-auto=1 (Config::autoAllowed): ambient targets through PianoAuto's AmbientBacklight.h. Without it nothing changes.
static void testAmbient()
{
  using pianoauto::Ambient;
  {
    Model m;
    m.setFound(0, 812, true);
    m.setFound(1, 812, true);
    Controller c(m);
    c.configure(config());
    c.survey();
    CHECK(c.start());
    CHECK(!c.ambient(500, 400, "test light") && !c.automatic(true, "test") && m.writes().empty() && !c.ambientState().automatic());
    c.step(1, "test keys");
    CHECK(m.code(0) == 1024 && !m.logged("automatic"));
  }
  Model m;
  m.setFound(0, 812, true);
  m.setFound(1, 812, true);
  Config cf = config();
  cf.autoAllowed = true;
  Controller c(m);
  c.configure(cf);
  c.survey();
  CHECK(c.start());
  c.display(true, 1, 1);
  // The first target: automatic mode, the curve's code on both ICs, its writes not logged one by one.
  CHECK(c.ambient(370, 300, "test light") && c.ambientState().automatic());
  CHECK(m.code(0) == 757 && m.code(1) == 757 && m.logged("automatic brightness: on (test light): 300 lux, target 370 permille"));
  CHECK(!m.logged("code 757 (37 %)"));
  // The keys in automatic mode shift the curve by the ladder's step; the offset stays as the room changes.
  c.step(1, "test keys");
  CHECK(c.ambientState().offset() == 1 && m.code(0) == 954 && m.logged("in automatic mode: offset 0 -> 1, code 757 -> 954"));
  CHECK(c.ambient(500, 400, "test light") && m.code(0) == 1290 && m.code(1) == 1290);   // 1024 one step up: the ladder's 1290
  // Dark (display asleep): a target is kept, nothing written; the wake lights the newest.
  c.display(false, 1, 0);
  const size_t before = m.writes().size();
  CHECK(c.ambient(240, 100, "test light") && m.writes().size() == before && !m.lit(0));
  c.display(true, 1, 1);
  CHECK(m.lit(0) && m.code(0) == Ambient::shift(Ambient::codeForPermille(240, 128), 1, 128, false));
  // One summary line per 10 s while targets come (retry(), every second in the kext, flushes it).
  m.now += 11000;
  c.retry();
  CHECK(m.logged("ambient: 3 targets in "));
  // An explicit level: manual, targets only recorded; the property re-enters with the last target and the offset.
  c.percent(50, "setProperties");
  CHECK(!c.ambientState().automatic() && m.code(0) == codeForPercent(50, 128) &&
        m.logged("automatic brightness: off, a level was set by setProperties"));
  CHECK(c.ambient(800, 3000, "test light") && m.code(0) == codeForPercent(50, 128));
  CHECK(c.automatic(true, "setProperties") && c.ambientState().automatic() &&
        m.code(0) == Ambient::shift(Ambient::codeForPermille(800, 128), 1, 128, false));
  // -1: the sensor leaves, the code stays; the keys step the ladder again.
  const uint16_t kept = m.code(0);
  CHECK(c.ambient(-1, 0, "test light") && !c.ambientState().automatic() && m.code(0) == kept &&
        m.logged("automatic brightness: off (test light left it)"));
  Steps ladder;
  ladder.build(128, false);
  c.step(1, "test keys");
  CHECK(m.code(0) == ladder.up(kept));
  CHECK(!m.inBurst);
  checkWhitelist(m, 128);
}

int main()
{
  testI2c();
  testSteps();
  testSurvey();
  testBootLevel();
  testDisplayAndSteps();
  testLid();
  testSleep();
  testFailures();
  testStack();
  testKeys();
  testAmbient();
  if (failures) {
    std::printf("backlight_host: %d FAILED\n", failures);
    return 1;
  }
  std::printf("backlight_host: all passed (%u I2C waits)\n", gDelays);
  return 0;
}
