// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoTorch's portable part (FlashHw.h, TorchCore.h, TorchSpmi.h) against a model of the SPMI arbiter v7 (APID map,
// owner table, EE 0's observer channels, the owner write channels) with the PM8550 flash module and two other peripherals:
//   - level 1: only observer reads of the flash APID, never a write, never another peripheral, never past its 256 bytes;
//   - the refusals (no APID, another owner, not a flash module, the three-channel layout, channels 3/4 in use, a partial survey);
//   - the on sequence: only the ten allowlisted registers of PPID 0x1ee, masked read-modify-write (other bits kept), each read
//     back; 50 mA -> ITARGET 4 on both channels, the clamp 9, 5 mA steps, software strobe, timers off; the hard 100 mA cap;
//   - off; a read-back mismatch and a denied write (both: off attempted, the state off, a cool-down);
//   - the guard: the time limit, the boot test, the module's thermal/current/voltage/short bits, the SoC and battery limits,
//     one unreadable poll tolerated and two not, the enables found off.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. -I../../PianoPower torch_host.cpp -o /tmp/torch_host && /tmp/torch_host
#include "TorchSpmi.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

using namespace pianotorch;
using pianopower::Window;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// --------------------------------------------------------------------------------------------------------- the arbiter model
struct Arbiter {
  struct Apid {
    uint16_t ppid;
    uint32_t owner;
  };
  std::vector<Apid> apids;
  std::map<uint32_t, uint8_t> pmic;               // ppid << 8 | reg
  std::map<uint32_t, bool> ignoresWrites;          // a register that keeps its value (a read-back mismatch)
  std::map<int, uint32_t> obsvStatus, obsvD0, obsvD1;
  std::map<int, uint32_t> chnlStatus, chnlData;
  std::vector<std::tuple<uint16_t, uint8_t, unsigned>> reads;   // ppid, reg, bytes
  std::vector<std::tuple<uint16_t, uint8_t, uint8_t>> writes;   // ppid, reg, value
  unsigned failReads = 0;                                       // the next N observer reads fail (status error)

  uint8_t &r(uint16_t ppid, uint8_t reg) { return pmic[uint32_t(ppid) << 8 | reg]; }
  uint8_t flash(uint8_t reg) { return r(kPpid, reg); }

  uint32_t rd(Window w, uint32_t off)
  {
    if (w == Window::core) {
      if (off == 0) return 0x70020000;
      if (off == 4) return uint32_t(apids.size());
      if (off >= 0x2000 && off < 0x2000 + 4 * apids.size()) return uint32_t(apids[(off - 0x2000) / 4].ppid) << 8;
      return 0;
    }
    if (w == Window::cnfg) return off / 4 < apids.size() ? apids[off / 4].owner : 0;
    const int apid = int(off / 0x20);
    switch (off % 0x20) {
      case 0x08: return obsvStatus[apid];
      case 0x18: return obsvD0[apid];
      case 0x1c: return obsvD1[apid];
    }
    return 0;
  }
  void wr(Window w, uint32_t off, uint32_t value)
  {
    CHECK(w == Window::obsv);
    if (w != Window::obsv || off % 0x20 != 0) return;
    const int apid = int(off / 0x20);
    CHECK((value >> 27) == 1);   // an extended read: the observer channel never writes
    const uint8_t reg = uint8_t(value >> 4);
    const unsigned bytes = (value & 7) + 1;
    const uint16_t ppid = apids[apid].ppid;
    reads.emplace_back(ppid, reg, bytes);
    CHECK(unsigned(reg) + bytes <= 0x100);
    if (failReads) {
      --failReads;
      obsvStatus[apid] = 0x3;
      return;
    }
    uint32_t lo = 0, hi = 0;
    for (unsigned b = 0; b < bytes; ++b) (b < 4 ? lo : hi) |= uint32_t(r(ppid, uint8_t(reg + b))) << (8 * (b & 3));
    obsvD0[apid] = lo;
    obsvD1[apid] = hi;
    obsvStatus[apid] = 1;
  }
  // the owner channel of one APID (offsets into its 0x20 bytes)
  uint32_t chRd(int apid, uint32_t off) { return off == kChnlStatus ? chnlStatus[apid] : 0; }
  void chWr(int apid, uint32_t off, uint32_t value)
  {
    if (off == kChnlWdata0) {
      chnlData[apid] = value;
      return;
    }
    CHECK(off == kChnlCmd);
    CHECK((value >> 27) == kOpExtWriteL && (value & 7) == 0);
    const uint8_t reg = uint8_t(value >> 4);
    const uint16_t ppid = apids[apid].ppid;
    if (apids[apid].owner != 0) {
      chnlStatus[apid] = 0x5;   // done + denied
      return;
    }
    writes.emplace_back(ppid, reg, uint8_t(chnlData[apid]));
    if (!ignoresWrites[uint32_t(ppid) << 8 | reg]) r(ppid, reg) = uint8_t(chnlData[apid]);
    chnlStatus[apid] = 1;
  }
};

static Arbiter *gArb;
static uint32_t ioRd(void *, Window w, uint32_t off) { return gArb->rd(w, off); }
static void ioWr(void *, Window w, uint32_t off, uint32_t v) { gArb->wr(w, off, v); }
static void ioDelay(void *, unsigned) {}
static int gChApid;
static uint32_t chRd(void *, uint32_t off) { return gArb->chRd(gChApid, off); }
static void chWr(void *, uint32_t off, uint32_t v) { gArb->chWr(gChApid, off, v); }
static const pianopower::Io kIo = {nullptr, ioRd, ioWr, ioDelay};
static const ChnlIo kChnl = {nullptr, chRd, chWr, ioDelay};

// Piano's PMICs as far as the test needs: the PON (SID 0), a PM8550 GPIO, the flash module (four channels, off, reset values 0).
static void piano(Arbiter &a, uint32_t flashOwner = 0)
{
  a = Arbiter();
  a.apids = {{0x013, 0}, {0x18d, 0}, {kPpid, flashOwner}};
  a.r(kPpid, kRegRevision) = 0x02;
  a.r(kPpid, kRegType) = kTypeFlash;
  a.r(kPpid, kRegSubtype) = kSubtype4ch;
  a.r(kPpid, kRegThermThrsh1) = 0x03;
  a.r(kPpid, kRegThermThrsh2) = 0x01;
  a.r(0x013, 0x10) = 0x80;
  gArb = &a;
}

struct Port {
  SpmiPort port;
  Bus bus;
  explicit Port(bool writes)
  {
    const pianopower::Arbiter arb = pianopower::spmiArbiter(kIo);
    const pianopower::Apid ap = pianopower::spmiFindApid(kIo, arb, kPpid);
    port.io = &kIo;
    port.apid = ap.apid;
    gChApid = ap.apid;
    port.chnl = writes ? &kChnl : nullptr;
    bus = portBus(port);
  }
};

static bool inAllowlist(uint8_t reg)
{
  for (const Writable &w : kWritable)
    if (w.reg == reg) return true;
  return false;
}

// Every read is of the flash APID, at most 8 bytes, inside its 256 bytes; every write is to an allowlisted register of it.
static bool onlyFlash(const Arbiter &a)
{
  for (const auto &[ppid, reg, bytes] : a.reads)
    if (ppid != kPpid || unsigned(reg) + bytes > 0x100 || bytes > 8) return false;
  for (const auto &[ppid, reg, value] : a.writes)
    if (ppid != kPpid || !inAllowlist(reg)) return false;
  return true;
}

static void testConstants()
{
  CHECK(kPpid == 0x1ee);
  CHECK(itargetForMa(50) == 4 && itargetForMa(100) == 9 && itargetForMa(10) == 0 && itargetForMa(5) == 0);
  CHECK(itargetForMa(45) == 3 && maForItarget(3) == 40);   // rounds down, never up
  CHECK(clampMa(500, 100) == 100 && clampMa(1, 100) == 10 && clampMa(80, 500) == 80 && clampMa(200, 500) == 100);
  CHECK(clampForMa(100) == 9 && clampForMa(60) == 5);
  CHECK(!writable(kRegThermThrsh1, 0x07) && !writable(kRegThermThrsh2, 0x07) && !writable(0xd0, 0xff) && !writable(0x40, 0xff));
  CHECK(!writable(kRegChanEn, 0x0c) && !writable(kRegIres, 0x04) && !writable(kRegModuleEn, 0x7f) && !writable(kRegItarget0 + 2, 0x7f));
  CHECK(writable(kRegChanEn, 0x03) && writable(kRegModuleEn, 0x80) && writable(kRegTorchClamp, 0x7f) && !writable(kRegChanEn, 0));
  const Faults f = decodeFaults(0x04, 0x20, 0x00, kPianoChannelMask);
  CHECK(f.shortCircuit && f.thermal && !f.current && !f.voltage && !f.timeout);
  CHECK(!decodeFaults(0x10, 0, 0x10, kPianoChannelMask).any());   // channel 2's bits are not piano's
  CHECK(std::strcmp(strobeName(0x01), "software, level, active high") == 0);
}

static void testSurvey()
{
  Arbiter a;
  piano(a);
  Port p(false);
  CHECK(p.port.apid == 2);
  const Survey s = survey(p.bus);
  CHECK(s.identity && s.flash && s.fourChannel && s.complete && s.failures == 0);
  CHECK(s.revision == 0x02 && !s.lit() && s.reg[kRegThermThrsh1] == 0x03);
  CHECK(a.writes.empty() && onlyFlash(a));
  CHECK(writeRefusal(s, p.port.apid, 0) == nullptr);
  CHECK(writeRefusal(s, -1, 0) != nullptr && writeRefusal(s, 2, 2) != nullptr);
  // level 1 has no write path at all: put() refuses before any bus access
  Controller c(p.bus);
  StepResult r;
  CHECK(!c.put(r, kRegChanEn, 0x03, 0x03) && r.refused == 1 && a.writes.empty());

  // not a flash module: only the identity read
  piano(a);
  a.r(kPpid, kRegType) = 0x51;
  Port q(false);
  const Survey t = survey(q.bus);
  CHECK(t.identity && !t.flash && t.reads == 1 && writeRefusal(t, 2, 0) != nullptr);
  // three channels: read and logged, never written
  piano(a);
  a.r(kPpid, kRegSubtype) = kSubtype3chPm8150;
  Port q3(false);
  const Survey u = survey(q3.bus);
  CHECK(u.flash && !u.fourChannel && writeRefusal(u, 2, 0) != nullptr);
  // someone else's channel on
  piano(a);
  a.r(kPpid, kRegChanEn) = 0x04;
  Port q4(false);
  CHECK(writeRefusal(survey(q4.bus), 2, 0) != nullptr);
  // a read that fails: incomplete, refused
  piano(a);
  Port q5(false);
  a.failReads = 0;
  Survey v = survey(q5.bus);
  CHECK(v.complete);
  a.reads.clear();
  a.failReads = 0;
  // fail the second range read
  struct Once {
    static bool read(void *ctx, uint8_t reg, uint8_t *value, unsigned bytes)
    {
      if (reg == 0x10) return false;
      return portRead(ctx, reg, value, bytes);
    }
  };
  Bus flaky = q5.bus;
  flaky.read = Once::read;
  v = survey(flaky);
  CHECK(!v.complete && v.failures == 1 && writeRefusal(v, 2, 0) != nullptr);
}

static void testOnOff()
{
  Arbiter a;
  piano(a);
  a.r(kPpid, kRegIres) = 0x04;        // channel 3's bit: kept
  a.r(kPpid, kRegModuleEn) = 0x01;    // a low bit: kept
  a.r(kPpid, kRegItarget0) = 0x80;    // bit 7: kept
  Port p(true);
  GuardConfig g;
  Torch t(p.bus, g, 100);
  Temperatures temps;
  char line[120];
  t.describe(0, line, sizeof line);
  CHECK(std::strcmp(line, "off") == 0);
  Torch::Outcome o = t.request(1000, 50, 0, temps);
  CHECK(o.lit && o.steps.ok && o.steps.writes == 12 && o.steps.readbacks == 12 && t.ma() == 50 && t.itarget() == 4);
  CHECK(a.flash(kRegModuleEn) == 0x81 && a.flash(kRegChanEn) == 0x03 && a.flash(kRegIres) == 0x07);
  CHECK(a.flash(kRegItarget0) == 0x84 && a.flash(kRegItarget0 + 1) == 0x04 && a.flash(kRegTorchClamp) == 9);
  CHECK(a.flash(kRegStrobe0) == 0x01 && a.flash(kRegStrobe0 + 1) == 0x01 && a.flash(kRegTimer0) == 0 && a.flash(kRegTimer0 + 1) == 0);
  CHECK(onlyFlash(a));
  CHECK(t.deadlineMs() == 1000 + 300 * 1000);
  t.describe(1000, line, sizeof line);
  CHECK(std::strcmp(line, "on 50 mA (2 x 25 mA, itarget 4), off in 300 s") == 0);

  a.writes.clear();
  o = t.request(2000, 0, 0, temps);
  CHECK(!o.lit && o.steps.ok && o.steps.writes == 4 && !t.lit() && t.lastReason() == Reason::user);
  CHECK(a.flash(kRegModuleEn) == 0x01 && a.flash(kRegChanEn) == 0 && a.flash(kRegItarget0) == 0x80 && a.flash(kRegItarget0 + 1) == 0);
  t.describe(2000, line, sizeof line);
  CHECK(std::strcmp(line, "off (asked)") == 0);

  // clamped to the limit and to the hard cap; the seconds to maxS
  o = t.request(3000, 900, 99999, temps);
  CHECK(o.lit && t.ma() == 100 && t.itarget() == 9 && t.deadlineMs() == 3000 + 1800 * 1000);
  Torch low(p.bus, g, 60);
  o = low.request(4000, 100, 10, temps);
  CHECK(o.lit && low.ma() == 60 && a.flash(kRegItarget0 + 1) == 5 && a.flash(kRegTorchClamp) == 5);
  Torch silly(p.bus, g, 5000);
  CHECK(silly.maxMa() == 100);
  Controller c(p.bus);
  const StepResult capped = c.on(77, 77);
  CHECK(capped.ok && a.flash(kRegItarget0 + 1) == 9 && a.flash(kRegTorchClamp) == 9);
  c.off();
}

static void testFailures()
{
  Arbiter a;
  piano(a);
  a.ignoresWrites[uint32_t(kPpid) << 8 | (kRegItarget0 + 1)] = true;
  a.r(kPpid, kRegItarget0 + 1) = 0x11;
  Port p(true);
  GuardConfig g;
  Torch t(p.bus, g, 100);
  Temperatures temps;
  Torch::Outcome o = t.request(1000, 50, 0, temps);
  CHECK(!o.lit && !t.lit() && o.reason == Reason::writeFailed && o.steps.why && o.steps.failedReg == kRegItarget0 + 1);
  CHECK(o.steps.failedWant == 4 && o.steps.failedGot == 0x11);
  CHECK(a.flash(kRegModuleEn) == 0 && a.flash(kRegChanEn) == 0);   // never enabled, and off ran
  o = t.request(1000 + 59000, 50, 0, temps);
  CHECK(!o.lit && o.reason == Reason::writeFailed && o.steps.writes == 0);   // the cool-down: no write at all
  a.ignoresWrites.clear();
  o = t.request(1000 + 60000, 50, 0, temps);
  CHECK(o.lit);
  t.request(70000, 0, 0, temps);

  // the arbiter denies (another owner): the first write fails, nothing changes
  piano(a, 3);
  Port q(true);
  Torch u(q.bus, g, 100);
  o = u.request(1000, 50, 0, temps);
  CHECK(!o.lit && o.reason == Reason::writeFailed && a.writes.empty() && q.port.writeFailures >= 1 && (q.port.lastWriteStatus & 0xe));

  // a fault the module reports right after on: off again
  piano(a);
  a.r(kPpid, kRegStatus2) = kSts2Otst1;
  Port r(true);
  Torch w(r.bus, g, 100);
  o = w.request(1000, 50, 0, temps);
  CHECK(!o.lit && o.reason == Reason::pmicThermal && a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);
}

static void testGuard()
{
  Arbiter a;
  piano(a);
  Port p(true);
  GuardConfig g;
  Torch t(p.bus, g, 100);
  Temperatures temps;
  // the time limit
  CHECK(t.request(0, 50, 5, temps).lit);
  CHECK(t.poll(4000, temps).lit);
  Torch::Outcome o = t.poll(5000, temps);
  CHECK(!o.lit && o.reason == Reason::timeout && a.flash(kRegChanEn) == 0);
  // the boot test names itself
  CHECK(t.request(6000, 50, 3, temps).lit);
  t.setTestMode(true);
  o = t.poll(9000, temps);
  CHECK(!o.lit && o.reason == Reason::test);
  // the module's own bits, while on
  CHECK(t.request(10000, 50, 0, temps).lit);
  a.r(kPpid, kRegStatus2) = kSts2BobIlimS1;
  o = t.poll(11000, temps);
  CHECK(!o.lit && o.reason == Reason::pmicCurrent);
  a.r(kPpid, kRegStatus2) = 0;
  CHECK(!t.request(12000, 50, 0, temps).lit);   // cooling down
  CHECK(t.request(71000, 50, 0, temps).lit);
  a.r(kPpid, kRegStatus1) = 0x01;   // channel 0 short
  o = t.poll(72000, temps);
  CHECK(!o.lit && o.reason == Reason::shortCircuit);
  a.r(kPpid, kRegStatus1) = 0;
  // temperatures: refused when hot, off when it gets hot
  temps.socValid = true;
  temps.socDeci = 900;
  CHECK(t.request(200000, 50, 0, temps).reason == Reason::socHot && !t.lit());
  temps.socDeci = 500;
  CHECK(t.request(200000, 50, 0, temps).lit);
  temps.batteryValid = true;
  temps.batteryCenti = 4600;
  o = t.poll(201000, temps);
  CHECK(!o.lit && o.reason == Reason::batteryHot);
  temps = Temperatures();
  // one unreadable poll is tolerated, two are not
  CHECK(t.request(300000, 50, 0, temps).lit);
  a.failReads = 1;
  CHECK(t.poll(301000, temps).lit);
  CHECK(t.poll(302000, temps).lit);
  a.failReads = 1;
  CHECK(t.poll(303000, temps).lit);
  a.failReads = 3;
  o = t.poll(304000, temps);
  CHECK(!o.lit && o.reason == Reason::statusUnreadable);
  a.failReads = 0;
  // someone (or the module) turned the channels off
  CHECK(t.request(400000, 50, 0, temps).lit);
  a.r(kPpid, kRegChanEn) = 0x01;
  o = t.poll(401000, temps);
  CHECK(!o.lit && o.reason == Reason::lostState);
  // sleep and shutdown: off with their reason
  CHECK(t.request(500000, 50, 0, temps).lit);
  o = t.turnOff(500500, Reason::sleep);
  CHECK(!o.lit && t.lastReason() == Reason::sleep && a.flash(kRegModuleEn) == 0);
  CHECK(onlyFlash(a));
}

int main()
{
  testConstants();
  testSurvey();
  testOnOff();
  testFailures();
  testGuard();
  if (failures) {
    std::printf("torch_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("torch_host: all passed\n");
  return 0;
}
