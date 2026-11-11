// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of TsensIrq.h, the TSENS upper/lower threshold interrupts (piano-thermal-irq=1), against a model of the four
// controllers (thresholds compared with each sensor's temperature, TM_Sn_STATUS's UPPER/LOWER bits, the up/low mask and clear, an
// optional status latch, TM_INT_EN) and of a level-triggered line that calls the handler while it is up, unmasked, attached and
// delivered (and not while the thread holds the mask lock):
//   - the encoders and the handler's decision (service): what is masked and cleared, strays;
//   - the start: the quiet masks, the TM_INT_EN fix-up, the proof on every controller, the arming (windows, states, masks);
//   - the tier transitions on interrupts: rest (relaxed base, warm and fast-clock rules stood down), the fast hold, today's tiers
//     while a sensor is high, back to rest on the lower interrupt, no interrupt inside the hysteresis;
//   - the fallbacks: a proof that never interrupts, misses (suspect, grace, three in a row; a delivery in between resets the run),
//     a line stuck up (the storm guard), a stuck status (the thread's rate guard); each logged once, everything back as found;
//   - suspend: lower masked with the upper armed, no miss for a cool-down while suspended, the masks back at finish;
//   - the synthetic test (piano-thermal-irq-test): pass, miss, no crossing;
//   - a record that arrives between the step's take and its mask write stays masked; the writes stay inside the allowlist.
//   uplow_host         run the tests
//   uplow_host --map   print the controllers' TM addresses and SPIs for tooling/k27/check-tsens-irq.py
#include "TsensIrq.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pianothermal;
using namespace pianothermal::uplow;

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------------- the model
struct Model {
  struct Ctl {
    int32_t temp[kMaxSensors] = {};
    bool valid[kMaxSensors] = {};
    bool stuckUpper[kMaxSensors] = {};
    uint32_t word[kMaxSensors] = {};
    uint32_t mask = 0, clear = 0, intEn = 7, latch = 0;
    bool mapped = true, attached = false, gicOff = false, deliver = true, maskIgnored = false, attachFails = false;
    uint32_t events = 0;
    uint64_t handled = 0, strays = 0;
    pianoirq::StormGuard guard = pianoirq::stormGuard(100, 2000);
  } ctl[kControllers];
  // TM_UPPER_LOWER_INT_STATUS: not modelled (reads 0: the line follows the live status), a latch of the unmasked interrupts, or a
  // raw latch of every violated threshold, masked or not (the hardware's choice is not documented; the handler copes with all three)
  enum class Latch { none, interrupt, raw } latchMode = Latch::interrupt;
  bool locked = false, runaway = false, badWrite = false;
  uint64_t nowMs = 1000;
  unsigned writes = 0;
  std::vector<std::string> notes;

  Model()
  {
    for (unsigned c = 0; c < kControllers; ++c)
      for (unsigned s = 0; s < kMap[c].count; ++s) {
        ctl[c].temp[s] = 400;
        ctl[c].valid[s] = true;
        ctl[c].word[s] = 0x5a000000u | encodeUpperLower(-400, 1200);   // found: bits above the fields, a wide window
      }
  }
  void all(int32_t deci)
  {
    for (unsigned c = 0; c < kControllers; ++c)
      for (unsigned s = 0; s < kMap[c].count; ++s) ctl[c].temp[s] = deci;
    settle();
  }
  void set(unsigned c, unsigned s, int32_t deci)
  {
    ctl[c].temp[s] = deci;
    settle();
  }
  uint32_t statusOf(unsigned c, unsigned s) const
  {
    const Ctl &k = ctl[c];
    const Thresholds th = decodeThresholds(k.word[s], 0);
    uint32_t v = uint32_t(k.temp[s]) & 0xfff;
    if (k.valid[s]) v |= kStatusValid;
    if (k.temp[s] >= th.upperDeci || k.stuckUpper[s]) v |= kStatusUpper;
    if (k.temp[s] <= th.lowerDeci) v |= kStatusLower;
    return v;
  }
  uint32_t violated(unsigned c) const
  {
    uint32_t v = 0;
    for (unsigned s = 0; s < kMap[c].count; ++s) v |= violatedBits(statusOf(c, s), s);
    return v;
  }
  bool asserted(unsigned c) const
  {
    const Ctl &k = ctl[c];
    if ((k.intEn & 7) != 7) return false;
    if (k.maskIgnored) return true;
    return (((latchMode != Latch::none ? k.latch : 0u) | violated(c)) & ~k.mask & ourBits(kMap[c].count)) != 0;
  }
  void convert(unsigned c)
  {
    if (latchMode == Latch::interrupt) ctl[c].latch |= violated(c) & ~ctl[c].clear & ~ctl[c].mask;
    else if (latchMode == Latch::raw) ctl[c].latch |= violated(c) & ~ctl[c].clear;
  }
  uint32_t read(unsigned c, uint32_t off)
  {
    const Ctl &k = ctl[c];
    if (!k.mapped) return 0xffffffffu;
    if (off == kTmIntEn) return k.intEn;
    if (off == kTmUpLowIntStatus) return latchMode != Latch::none ? k.latch : 0;
    if (off == kTmUpLowIntClear) return k.clear;
    if (off == kTmUpLowIntMask) return k.mask;
    if (off >= kTmUpLowThreshold && off < kTmUpLowThreshold + 4 * kMaxSensors) return k.word[(off - kTmUpLowThreshold) / 4];
    if (off >= kTmStatus && off < kTmStatus + 4 * kMaxSensors) return statusOf(c, (off - kTmStatus) / 4);
    return 0;
  }
  void write(unsigned c, uint32_t off, uint32_t v)
  {
    Ctl &k = ctl[c];
    ++writes;
    if (off == kTmIntEn) k.intEn = v;
    else if (off == kTmUpLowIntClear) {
      k.clear = v;
      k.latch &= ~v;
    } else if (off == kTmUpLowIntMask) k.mask = v;
    else if (off >= kTmUpLowThreshold && off < kTmUpLowThreshold + 4 * kMaxSensors && (off & 3) == 0) k.word[(off - kTmUpLowThreshold) / 4] = v;
    else badWrite = true;   // the critical thresholds/interrupts, SROT, anything else: never
    convert(c);
    if (!locked) pump();
  }
  void settle()
  {
    for (unsigned c = 0; c < kControllers; ++c) convert(c);
    if (!locked) pump();
  }
  // The level line: the handler runs while a controller's line is up, attached, enabled at the GIC and delivered.
  void pump()
  {
    for (unsigned guard = 0; guard < 20000; ++guard) {
      bool ran = false;
      for (unsigned c = 0; c < kControllers; ++c) {
        Ctl &k = ctl[c];
        if (!k.attached || k.gicOff || !k.deliver || !asserted(c)) continue;
        handler(c);
        ran = true;
      }
      if (!ran) return;
    }
    runaway = true;
  }
  void handler(unsigned c);
  void advance(uint64_t ms)
  {
    nowMs += ms;
    settle();
  }
};

static Model *gModel;
static Model::Latch gLatch = Model::Latch::interrupt;
static uint32_t regRead(void *, unsigned c, uint32_t off) { return gModel->read(c, off); }
static void regWrite(void *, unsigned c, uint32_t off, uint32_t v) { gModel->write(c, off, v); }
static const Regs kRegs = {nullptr, regRead, regWrite};

// The kernel handler's shape (PianoThermalIrq.cpp): service under the lock, record and wake, or count a stray (storm guard).
void Model::handler(unsigned c)
{
  Ctl &k = ctl[c];
  const Serviced sv = service(kRegs, c, kMap[c].count);
  if (sv.bits) {
    k.events |= sv.bits;
    ++k.handled;
    return;
  }
  ++k.strays;
  if (k.guard.stray(nowMs)) k.gicOff = true;
}

static bool envMapped(void *, unsigned c) { return gModel->ctl[c].mapped; }
static uintptr_t envLock(void *)
{
  gModel->locked = true;
  return 0;
}
static void envUnlock(void *, uintptr_t)
{
  gModel->locked = false;
  gModel->pump();
}
static uint32_t envTake(void *, unsigned c)
{
  const uint32_t e = gModel->ctl[c].events;
  gModel->ctl[c].events = 0;
  return e;
}
static bool envWait(void *, unsigned c, uint32_t bits, uint32_t ms)
{
  for (uint32_t t = 0; t <= ms; ++t) {
    gModel->pump();
    if (gModel->ctl[c].events & bits) {
      gModel->ctl[c].events = 0;
      return true;
    }
    gModel->nowMs += 1;
  }
  return false;
}
static bool envAttach(void *, unsigned c)
{
  if (gModel->ctl[c].attachFails) return false;
  gModel->ctl[c].attached = true;
  gModel->pump();
  return true;
}
static bool envLineLive(void *, unsigned c) { return !gModel->ctl[c].gicOff; }
static void envDisable(void *, unsigned c) { gModel->ctl[c].gicOff = true; }
static uint64_t envCount(void *, unsigned c) { return gModel->ctl[c].handled; }
static uint64_t envNow(void *) { return gModel->nowMs; }
static void envNote(void *, const char *fmt, ...)
{
  char line[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  gModel->notes.push_back(line);
  if (std::getenv("UPLOW_VERBOSE")) std::printf("  %s\n", line);
}
static const Env kEnv = {nullptr, kRegs, envMapped, envLock, envUnlock, envTake, envWait, envAttach, envLineLive, envDisable, envCount,
                         envNow, envNote};

static uint32_t ioRead(void *, unsigned c, uint32_t off) { return gModel->read(c, off); }
static bool ioMapped(void *, unsigned c) { return gModel->ctl[c].mapped; }
static const TsensIo kIo = {nullptr, ioRead, ioMapped};

static unsigned notesWith(const char *what)
{
  unsigned n = 0;
  for (const std::string &s : gModel->notes)
    if (s.find(what) != std::string::npos) ++n;
  return n;
}

// One scene: the model, a Monitor and the machine, polled like PianoThermal.cpp's loop.
struct Scene {
  Model model;
  Monitor m;
  Machine mc;
  Config cfg;
  Scene()
  {
    gModel = &model;
    model.latchMode = gLatch;
  }
  bool start()
  {
    mc.configure(cfg);
    m.poll(kIo);
    return mc.start(kEnv, m, model.nowMs);
  }
  void poll(uint64_t afterMs = 0)
  {
    if (afterMs) model.advance(afterMs);
    m.poll(kIo);
    mc.step(kEnv, m, model.nowMs);
  }
  uint64_t handled() const
  {
    uint64_t n = 0;
    for (const Model::Ctl &k : model.ctl) n += k.handled;
    return n;
  }
};

// ------------------------------------------------------------------------------------------------------------- tests
static void testEncoders()
{
  CHECK(thresholdWord(0xab000000u | 0x123456u, 700, 850) == (0xab000000u | (850u << 12) | 700u));
  const Thresholds wide = decodeThresholds(thresholdWord(0, -500, 1300), 0);
  CHECK(wide.lowerDeci == kTripMinDeci && wide.upperDeci == kTripMaxDeci);
  CHECK(decodeThresholds(thresholdWord(0, -50, 470), 0).lowerDeci == -50);
  CHECK(ourBits(15) == 0x7fff7fffu && ourBits(16) == 0xffffffffu && ourBits(7) == 0x007f007fu);
  CHECK(violatedBits(kStatusUpper | kStatusValid | 0x352, 3) == upperBit(3));
  CHECK(violatedBits(kStatusLower | kStatusMin, 2) == lowerBit(2));
  CHECK(violatedBits(kStatusCritical | kStatusMax | kStatusValid, 5) == 0);   // critical/max are not the up/low interrupt's
  CHECK(kSpiUplow[0] == 0x303 && kSpiUplow[3] == 0x306 && kSpiCritical[0] == 0x1e4 && kSpiCritical[3] == 0x1e7);
  // service(): an unmasked violated upper is masked and its clear pulsed; a masked one is left; a latched masked one is cleared
  Model model;
  gModel = &model;
  Model::Ctl &k = model.ctl[0];
  k.word[4] = thresholdWord(0, 700, 850);
  k.temp[4] = 900;
  k.mask = ourBits(15) & ~upperBit(4);
  model.settle();
  Serviced sv = service(kRegs, 0, 15);
  CHECK(sv.bits == upperBit(4) && k.mask == ourBits(15) && k.clear == 0 && !(k.latch & upperBit(4)));
  sv = service(kRegs, 0, 15);
  CHECK(sv.bits == 0);                                          // masked now: a stray
  k.latch = lowerBit(2);                                        // a latched status of a masked interrupt
  sv = service(kRegs, 0, 15);
  CHECK(sv.bits == 0 && sv.cleared == lowerBit(2) && !k.latch);
  CHECK(!model.badWrite);
}

static void testStart()
{
  Scene sc;
  sc.model.ctl[0].intEn = 0;                       // the boot stages left the enables off on tsens0
  sc.model.ctl[1].temp[2] = 900;                   // hot at the start
  sc.model.ctl[3].valid[5] = false;                // never VALID: not armed
  for (unsigned i = 0; i < kReadTries; ++i) {}     // (the Monitor retries VALID kReadTries times)
  CHECK(sc.start());
  CHECK(sc.mc.live());
  CHECK(!sc.model.runaway && !sc.model.badWrite);
  for (unsigned c = 0; c < kControllers; ++c) {
    CHECK(sc.model.ctl[c].attached && !sc.model.ctl[c].gicOff);
    CHECK(sc.model.ctl[c].handled == 2);           // the proof: one upper, one lower interrupt
    CHECK(sc.model.ctl[c].intEn == 7);
  }
  CHECK(sc.model.ctl[0].word[1] == (0x5a000000u | encodeUpperLower(700, 850)));   // bits above the fields kept
  CHECK(sc.model.ctl[3].word[5] == (0x5a000000u | encodeUpperLower(-400, 1200)));  // not armed: as found
  CHECK(sc.mc.armed(3) == (0x1ff & ~(1u << 5)) && sc.mc.armed(0) == 0x7fff && sc.mc.armed(2) == 0xffff);
  // masks: low sensors' upper unmasked, lower masked; the hot one the reverse; the unarmed one both masked
  CHECK((sc.model.ctl[0].mask & ourBits(15)) == 0x00007fffu);
  CHECK(sc.mc.high(1) == (1u << 2));
  CHECK((sc.model.ctl[1].mask & ourBits(7)) == ((0x7fu & ~(1u << 2)) | (upperBit(2))));
  CHECK((sc.model.ctl[3].mask & ourBits(9)) == (0x1ffu | upperBit(5)));
  CHECK(notesWith("proof on s") == 4 && notesWith("uplow: live: 46 sensors armed on 4 controllers") == 1);
  CHECK(notesWith("int-en set to 7") == 1);
  CHECK(!sc.mc.rest());                            // tsens1 s2 is above the upper threshold
  for (unsigned c = 0; c < kControllers; ++c) CHECK(!sc.model.asserted(c));
  // a second start is refused
  CHECK(!sc.mc.start(kEnv, sc.m, sc.model.nowMs) && sc.mc.live());
}

static void testTiers()
{
  Scene sc;
  CHECK(sc.start());
  sc.poll(1000);
  CHECK(sc.mc.rest());
  const uint64_t t0 = sc.model.nowMs;
  CHECK(sc.mc.tier(Poll::warm, Poll::base, t0) == Poll::base);   // the warm tier stands down at rest
  CHECK(sc.mc.tier(Poll::fast, Poll::base, t0) == Poll::base);   // and so does the fast-clock rule (the caller's atRest tier)
  CHECK(sc.mc.tier(Poll::fast, Poll::fast, t0) == Poll::fast);   // a thermally limited cluster stays fast
  CHECK(sc.mc.baseMs(1000) == 5000 && sc.mc.baseMs(8000) == 8000);
  // 71..84 C: no interrupt (under the upper threshold), still at rest
  const uint64_t before = sc.handled();
  sc.model.set(0, 7, 840);
  sc.poll(5000);
  CHECK(sc.handled() == before && sc.mc.rest());
  // 86 C: the upper interrupt, the fast hold, then today's tiers while it is high
  sc.model.set(0, 7, 860);
  CHECK(sc.handled() == before + 1);
  sc.poll(1);
  CHECK(sc.mc.high(0) == (1u << 7) && sc.mc.holding(sc.model.nowMs) && !sc.mc.rest());
  CHECK(sc.mc.tier(Poll::warm, Poll::base, sc.model.nowMs) == Poll::fast);
  CHECK(sc.mc.baseMs(1000) == 1000);
  CHECK((sc.model.ctl[0].mask & upperBit(7)) && !(sc.model.ctl[0].mask & lowerBit(7)));
  CHECK(notesWith("tsens0 s7 cpu-0-3-0 upper interrupt at 86.0 C") == 1);
  sc.poll(1001);
  CHECK(!sc.mc.holding(sc.model.nowMs) && sc.mc.tier(Poll::warm, Poll::base, sc.model.nowMs) == Poll::warm);
  // cooling inside the hysteresis: no interrupt; under 70 C: the lower one, at rest again
  sc.model.set(0, 7, 750);
  sc.poll(250);
  CHECK(sc.handled() == before + 1 && sc.mc.high(0));
  sc.model.set(0, 7, 698);
  CHECK(sc.handled() == before + 2);
  sc.poll(250);
  CHECK(!sc.mc.high(0) && sc.mc.rest() && sc.mc.baseMs(1000) == 5000);
  CHECK(notesWith("lower interrupt at 69.8 C") == 1 && notesWith("at rest again") == 1);
  CHECK(sc.mc.upperEvents() == 1 && sc.mc.lowerEvents() == 1 && sc.mc.misses() == 0 && sc.mc.spurious() == 0);   // the proof's are its own
  // and up again: the same path
  sc.model.set(2, 3, 870);
  sc.poll(1);
  CHECK(sc.mc.high(2) == (1u << 3) && sc.mc.holding(sc.model.nowMs));
  // a sensor that was not armed (no reading at the start) keeps today's polling from 70 C
  Scene u;
  u.model.ctl[2].valid[9] = false;
  CHECK(u.start());
  u.model.ctl[2].valid[9] = true;
  u.model.set(2, 9, 720);
  u.poll(1000);
  CHECK(!u.mc.rest() && u.mc.tier(Poll::warm, Poll::base, u.model.nowMs) == Poll::warm);
  // the governor's tick is never relaxed
  Scene g;
  g.cfg.governing = true;
  CHECK(g.start());
  g.poll(100);
  CHECK(g.mc.rest() && g.mc.baseMs(100) == 100);
  CHECK(notesWith("at rest the base stays the governor's tick") == 1);
  CHECK(!sc.model.runaway && !sc.model.badWrite && !u.model.badWrite);
}

static void testMisses()
{
  Scene sc;
  CHECK(sc.start());
  sc.poll(1000);
  sc.model.ctl[2].deliver = false;          // the hypervisor stops delivering SPI 773
  sc.model.set(2, 3, 860);                  // 1 C past: not yet suspect
  sc.poll(5000);
  CHECK(sc.mc.rest());
  sc.model.set(2, 3, 880);                  // 3 C past: suspect, fast polls
  sc.poll(5000);
  CHECK(!sc.mc.rest() && sc.mc.holding(sc.model.nowMs) && sc.mc.misses() == 0);
  sc.poll(10);
  CHECK(sc.mc.misses() == 0);
  sc.poll(50);                              // the grace is over: a miss, the poll makes it high
  CHECK(sc.mc.misses() == 1 && sc.mc.high(2) == (1u << 3) && notesWith("uplow: MISSED: tsens2 s3 gpuss2 at 88.0 C >= 85.0 C") == 1);
  // a delivered interrupt in between resets the run
  sc.model.set(2, 4, 880);
  sc.poll(10);
  sc.poll(60);
  CHECK(sc.mc.misses() == 2);
  sc.model.ctl[2].deliver = true;
  sc.model.set(0, 1, 870);                  // delivered on SPI 771
  sc.poll(1);
  CHECK(sc.mc.live());
  sc.model.ctl[2].deliver = false;
  sc.model.set(2, 5, 880);
  sc.poll(10);
  sc.poll(60);
  sc.model.set(2, 6, 880);
  sc.poll(10);
  sc.poll(60);
  CHECK(sc.mc.live() && sc.mc.misses() == 4);
  sc.model.set(2, 7, 880);                  // the third in a row: off
  sc.poll(10);
  sc.poll(60);
  CHECK(sc.mc.phase() == Phase::failed);
  CHECK(notesWith("uplow: OFF (3 missed interrupts in a row)") == 1);
  for (unsigned c = 0; c < kControllers; ++c) {
    const Model::Ctl &k = sc.model.ctl[c];
    CHECK((k.mask & ourBits(kMap[c].count)) == ourBits(kMap[c].count) && k.gicOff);
    for (unsigned s = 0; s < kMap[c].count; ++s) CHECK(k.word[s] == (0x5a000000u | encodeUpperLower(-400, 1200)));
  }
  // today's polling from now on, and nothing more is logged
  const size_t notes = sc.model.notes.size();
  sc.model.set(2, 8, 900);
  sc.poll(1000);
  sc.poll(1000);
  CHECK(sc.model.notes.size() == notes);
  CHECK(sc.mc.tier(Poll::warm, Poll::base, sc.model.nowMs) == Poll::warm && sc.mc.baseMs(1000) == 1000);
  CHECK(sc.mc.suspendPrepare(kEnv) == -1 && !sc.mc.suspendFinish(kEnv));
  CHECK(!sc.model.runaway && !sc.model.badWrite);
}

static void testProofFails()
{
  Scene sc;
  sc.model.ctl[0].intEn = 0;
  sc.model.ctl[1].deliver = false;          // tsens1's line never arrives
  CHECK(!sc.start());
  CHECK(sc.mc.phase() == Phase::failed);
  CHECK(notesWith("uplow: OFF (proof: tsens1 s") == 1 && notesWith("at the start") == 1);
  CHECK(notesWith("no interrupt in 250 ms") == 1);
  // tsens0 was already armed: back as found, its line disabled; tsens2/3 were never touched
  CHECK(sc.model.ctl[0].intEn == 0 && sc.model.ctl[0].gicOff && sc.model.ctl[1].gicOff);
  CHECK(sc.model.ctl[0].word[3] == (0x5a000000u | encodeUpperLower(-400, 1200)));
  CHECK(!sc.model.ctl[2].attached && sc.model.ctl[2].mask == 0 && sc.model.ctl[3].mask == 0);
  CHECK(sc.mc.tier(Poll::warm, Poll::base, sc.model.nowMs) == Poll::warm && sc.mc.baseMs(1000) == 1000);
  // an attach refused (no ArmCpu slot)
  Scene a;
  a.model.ctl[2].attachFails = true;
  CHECK(!a.start() && notesWith("tsens2: SPI 773 not attached") == 1);
  CHECK(!a.model.ctl[0].gicOff == false);   // tsens0 and 1 had been attached: disabled again
  // nothing mapped
  Scene n;
  for (Model::Ctl &k : n.model.ctl) k.mapped = false;
  CHECK(!n.start() && notesWith("no controller could be armed") == 1 && n.model.writes == 0);
}

static void testStorms()
{
  // a line that stays up whatever is masked: the handler's storm guard disables it, the thread falls back
  Scene sc;
  CHECK(sc.start());
  sc.poll(1000);
  sc.model.ctl[3].maskIgnored = true;
  sc.model.settle();
  CHECK(!sc.model.runaway && sc.model.ctl[3].gicOff && sc.model.ctl[3].strays > 2000);
  sc.poll(10);
  CHECK(sc.mc.phase() == Phase::failed && notesWith("uplow: OFF (storm: SPI 774 stayed up with nothing to mask") == 1);
  // stuck up already during the proof
  Scene p;
  p.model.ctl[1].maskIgnored = true;
  CHECK(!p.start() && notesWith("uplow: OFF (storm: SPI 772 stayed up at its attach") == 1 && !p.model.runaway);
  // a status stuck at "upper" on a cool sensor: spurious interrupts, re-armed each time, until the rate guard trips
  Scene st;
  CHECK(st.start());
  st.poll(1000);
  st.model.ctl[0].stuckUpper[1] = true;
  st.model.settle();
  for (unsigned i = 0; i < 60 && st.mc.live(); ++i) st.poll(100);
  CHECK(st.mc.phase() == Phase::failed && st.mc.spurious() >= 40);
  CHECK(notesWith("uplow: OFF (stormy: more than 40 threshold interrupts in 10 s)") == 1);
  CHECK(notesWith("spurious (re-armed)") == st.mc.config().logEvents);   // logged one by one up to the limit
  CHECK(!st.model.runaway && !st.model.badWrite);
}

static void testSuspend()
{
  Scene sc;
  sc.model.ctl[1].temp[2] = 900;
  CHECK(sc.start());
  sc.poll(1000);
  CHECK(sc.mc.suspendPrepare(kEnv) == 4 && sc.mc.suspendPrepare(kEnv) == -1);
  for (unsigned c = 0; c < kControllers; ++c) CHECK((sc.model.ctl[c].mask & lowerIntBits(kMap[c].count)) == lowerIntBits(kMap[c].count));
  CHECK(!(sc.model.ctl[0].mask & upperBit(4)));   // upper kept armed
  CHECK(notesWith("suspend prepare: lower threshold interrupts masked on 4 controllers, the upper ones kept armed") == 1);
  // cooling while suspended: no interrupt (masked), the poll moves it without a miss
  const uint64_t before = sc.handled();
  sc.model.set(1, 2, 600);
  CHECK(sc.handled() == before);
  sc.poll(1000);
  CHECK(!sc.mc.high(1) && sc.mc.misses() == 0);
  // overheating while suspended: the upper interrupt still comes; the lower one stays masked
  sc.model.set(0, 3, 900);
  CHECK(sc.handled() == before + 1);
  sc.poll(1);
  CHECK(sc.mc.high(0) == (1u << 3) && (sc.model.ctl[0].mask & lowerBit(3)));
  CHECK(sc.mc.suspendFinish(kEnv) && !sc.mc.suspendFinish(kEnv));
  CHECK(!(sc.model.ctl[0].mask & lowerBit(3)) && (sc.model.ctl[0].mask & upperBit(3)));
  CHECK(!(sc.model.ctl[1].mask & upperBit(2)) && (sc.model.ctl[1].mask & lowerBit(2)));
  CHECK(notesWith("suspend finish: the uplow interrupt masks are back") == 1);
  // a fallback while suspended: everything stays masked, finish says so
  CHECK(sc.mc.suspendPrepare(kEnv) == 4);
  sc.model.ctl[2].maskIgnored = true;
  sc.model.settle();
  sc.poll(10);
  CHECK(sc.mc.phase() == Phase::failed && sc.mc.suspendFinish(kEnv));
  CHECK(notesWith("suspend finish: the uplow path fell back meanwhile") == 1);
  for (unsigned c = 0; c < kControllers; ++c) CHECK((sc.model.ctl[c].mask & ourBits(kMap[c].count)) == ourBits(kMap[c].count));
  // not live: the caller's own masking runs
  Scene off;
  CHECK(off.mc.suspendPrepare(kEnv) == -1 && !off.mc.suspendFinish(kEnv));
  // a suspend that began while the path was still starting: the caller masks with its own path (PianoThermal.cpp's suspendMask,
  // upper too), the arming in between keeps every lower interrupt masked, and finish writes this path's masks and tells the caller
  // not to restore its (pre-arming) ones
  Scene ex;
  ex.model.ctl[1].temp[2] = 900;
  ex.mc.configure(ex.cfg);
  CHECK(ex.mc.suspendPrepare(kEnv) == -1);
  for (unsigned c = 0; c < kControllers; ++c) {
    const SuspendMask sm = suspendMask(ex.model.ctl[c].mask, kMap[c].count, true);
    ex.model.write(c, kTmUpLowIntMask, sm.mask);
    ex.model.write(c, kTmUpLowIntClear, sm.clear);
    ex.model.write(c, kTmUpLowIntClear, 0);
  }
  ex.m.poll(kIo);
  CHECK(ex.mc.start(kEnv, ex.m, ex.model.nowMs));
  CHECK(ex.mc.high(1) == (1u << 2) && (ex.model.ctl[1].mask & lowerBit(2)));   // high, its lower kept masked across the suspend
  const uint64_t handledBefore = ex.handled();
  CHECK(ex.mc.suspendFinish(kEnv));                                            // so the caller skips its restore
  CHECK(!(ex.model.ctl[1].mask & lowerBit(2)) && (ex.model.ctl[1].mask & upperBit(2)));
  CHECK((ex.model.ctl[0].mask & ourBits(15)) == 0x00007fffu && ex.mc.live());
  ex.poll(1000);
  CHECK(ex.handled() == handledBefore && ex.mc.live() && ex.mc.events() == 0);   // no burst of lower interrupts
  CHECK(notesWith("went live during this suspend") == 1);
  // the same while the start fails: the caller restores its own
  Scene exf;
  exf.model.ctl[0].deliver = false;
  exf.mc.configure(exf.cfg);
  CHECK(exf.mc.suspendPrepare(kEnv) == -1);
  exf.m.poll(kIo);
  CHECK(!exf.mc.start(kEnv, exf.m, exf.model.nowMs) && !exf.mc.suspendFinish(kEnv));
  CHECK(!ex.model.runaway && !ex.model.badWrite && !exf.model.runaway && !exf.model.badWrite);
}

static void testSyntheticTest()
{
  Scene sc;
  sc.cfg.testDeci = 20;                     // 2.0 C above the reading
  sc.cfg.testMs = 120000;
  sc.model.ctl[0].temp[7] = 450;
  sc.model.ctl[1].temp[3] = 430;
  sc.model.ctl[2].temp[6] = 440;
  sc.model.ctl[3].temp[2] = 420;
  CHECK(sc.start());
  for (unsigned c = 0; c < kControllers; ++c) CHECK(sc.mc.testOn(c));
  CHECK(sc.mc.testSensor(0) == 7 && sc.mc.testThreshold(0) == 470 && sc.mc.testSensor(2) == 6);
  CHECK(sc.model.ctl[0].word[7] == (0x5a000000u | encodeUpperLower(-400, 470)));
  CHECK(notesWith("uplow: test: tsens0 s7 cpu-0-3-0 at 45.0 C: upper threshold 47.0 C (piano-thermal-irq-test=20)") == 1);
  sc.poll(1000);
  CHECK(!sc.mc.rest() && sc.mc.baseMs(1000) == 1000);   // no relaxed base during the test
  // tsens0: a crossing, the interrupt: PASS, the window back, not high, no hold
  sc.model.set(0, 7, 475);
  sc.poll(1);
  CHECK(!sc.mc.testOn(0) && !sc.mc.high(0) && !sc.mc.holding(sc.model.nowMs) && sc.mc.testsPassed() == 1);
  CHECK(sc.model.ctl[0].word[7] == (0x5a000000u | encodeUpperLower(700, 850)) && !(sc.model.ctl[0].mask & upperBit(7)));
  CHECK(notesWith("uplow: test: tsens0 s7 cpu-0-3-0: upper interrupt at 47.5 C after 1.0 s (threshold 47.0 C, from 45.0 C") == 1);
  // tsens1: crossing but its line is not delivered: a miss
  sc.model.ctl[1].deliver = false;
  sc.model.set(1, 3, 480);
  sc.poll(1000);
  sc.poll(60);
  CHECK(!sc.mc.testOn(1) && sc.mc.testsMissed() == 1 && sc.mc.misses() == 1);
  CHECK(notesWith("uplow: test: MISSED: tsens1 s3") == 1 && notesWith("tsens1 s3 cpu-1-1-0: FAILED") == 1);
  sc.model.ctl[1].deliver = true;
  // tsens3: a crossing
  sc.model.set(3, 2, 445);
  sc.poll(1);
  CHECK(sc.mc.testsPassed() == 2);
  // tsens2: never warms: no crossing after the test's time
  sc.poll(60000);
  sc.poll(60000);
  CHECK(!sc.mc.testRunning() && sc.mc.testsQuiet() == 1);
  CHECK(notesWith("uplow: test: tsens2 s6 gpuss5: no crossing in 120 s (highest 44.0 C, threshold 46.0 C)") == 1);
  CHECK(notesWith("uplow: test: done: 2 passed, 1 missed, 1 without a crossing") == 1);
  CHECK(sc.model.ctl[2].word[6] == (0x5a000000u | encodeUpperLower(700, 850)));
  sc.poll(1000);
  CHECK(sc.mc.live() && sc.mc.rest() && sc.mc.baseMs(1000) == 5000);
  CHECK(!sc.model.runaway && !sc.model.badWrite);
}

static void testInbox()
{
  // An interrupt that lands after the step took the records but before it writes the masks stays masked (no second interrupt),
  // and is processed by the next step.
  Scene sc;
  CHECK(sc.start());
  sc.poll(1000);
  sc.model.locked = true;                    // the temperature changes while the "thread" holds the lock: the line waits
  sc.model.ctl[0].temp[5] = 880;
  sc.model.settle();
  const uint64_t before = sc.handled();
  sc.model.locked = false;
  // step(): its first unlock lets the handler run (after the take), so the record is new to apply()
  sc.m.poll(kIo);
  sc.mc.step(kEnv, sc.m, sc.model.nowMs);
  CHECK(sc.handled() == before + 1);
  CHECK(!sc.mc.high(0) && (sc.model.ctl[0].mask & upperBit(5)));   // kept masked, not yet processed
  sc.poll(1);
  CHECK(sc.mc.high(0) == (1u << 5) && sc.handled() == before + 1);
  CHECK(!sc.model.runaway && !sc.model.badWrite);
}

static void printMap()
{
  for (unsigned c = 0; c < kControllers; ++c)
    std::printf("%llx %u %u\n", (unsigned long long)kMap[c].tm, kSpiUplow[c], kSpiCritical[c]);
}

int main(int argc, char **argv)
{
  if (argc > 1 && !strcmp(argv[1], "--map")) {
    printMap();
    return 0;
  }
  testEncoders();
  // every scene under each of the three latch behaviours the hardware may have
  for (Model::Latch latch : {Model::Latch::interrupt, Model::Latch::raw, Model::Latch::none}) {
    gLatch = latch;
    const int before = failures;
    testStart();
    testTiers();
    testMisses();
    testProofFails();
    testStorms();
    testSuspend();
    testSyntheticTest();
    testInbox();
    if (failures != before) std::printf("  (with the latch model %d)\n", int(latch));
  }
  if (failures) {
    std::printf("uplow_host: %d FAILED\n", failures);
    return 1;
  }
  std::printf("uplow_host: all passed\n");
  return 0;
}
