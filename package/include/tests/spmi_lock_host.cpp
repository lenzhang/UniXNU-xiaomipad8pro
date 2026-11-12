// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of package/include/PianoSpmi.h, the SPMI transaction lock every native kext shares (docs/spmi-lock.md):
//   - the protocol alone: take/give, nesting, the counts, the interrupt mask balanced, the bound's take-over, a lost hold;
//   - the primitives against one channel: the command word, the byte packing, the retries outside the hold, the drain after a
//     timeout inside it, a denied read final, readStable;
//   - concurrency: threads that each run one kext's own accessor (PianoWifi's, PianoPower's and PianoInput's SpmiObserver.h,
//     PianoThermal's PmicAdc.h Bus, PianoTorch's TorchCore.h write) at once against a model of the arbiter with one command engine
//     per EE: a command stored while another is in flight is dropped, and every observer channel shows the engine's last read data
//     (the two failures of the 2026-11-12/13 probes: an error status, and "ok" with another register's value). Without the lock
//     the model must see both kinds of corruption (the test proves it can); with it, none, every value right.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "PianoSpmi.h"
#include "../../PianoWifi/SpmiObserver.h"
#include "../../PianoPower/SpmiObserver.h"
#include "../../PianoInput/SpmiObserver.h"
#include "../../PianoThermal/PmicAdc.h"
#include "../../PianoTorch/TorchCore.h"

static int failures;
#define CHECK(c)                                                                \
  do {                                                                          \
    if (!(c)) {                                                                 \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                                               \
    }                                                                           \
  } while (0)

// ------------------------------------------------------------------------------------------------- a lock for host threads
static thread_local int tMaskDepth;   // > 0: "interrupts masked" on this thread
static std::atomic<uint64_t> gNextSelf{1};
static thread_local uint64_t tSelf;
static std::atomic<int> gMaskedSpins{0};   // spins while masked (must stay 0: a waiter spins with interrupts as the caller had them)
static std::atomic<uint64_t> gClockUs{0};

uintptr_t hMask(void *)
{
  const uintptr_t was = tMaskDepth == 0;   // 1: they were enabled
  ++tMaskDepth;
  return was;
}
void hUnmask(void *, uintptr_t was)
{
  --tMaskDepth;
  if (tMaskDepth < 0) std::abort();
  (void)was;
}
uint64_t hSelf(void *)
{
  if (!tSelf) tSelf = gNextSelf++;
  return tSelf;
}
static std::function<void()> gSpinHook;   // a test's view of the world between two spins
void hSpin(void *)
{
  if (tMaskDepth) ++gMaskedSpins;
  gClockUs += 1;
  if (gSpinHook) gSpinHook();
  std::this_thread::yield();
}
uint64_t hNow(void *) { return gClockUs.load(); }
const PianoSpmiLockOps kHostOps = {nullptr, hMask, hUnmask, hSelf, hSpin, hNow};

static PianoSpmiLock makeLock(PianoSpmiWords *w, uint32_t tag)
{
  PianoSpmiLock l{};
  l.words = w;
  l.tag = tag;
  l.ops = &kHostOps;
  return l;
}

// ------------------------------------------------------------------------------------------------- the protocol alone
static void testProtocol()
{
  PianoSpmiWords w{};
  PianoSpmiLock a = makeLock(&w, piano_spmi_tag_of("PianoWifi")), b = makeLock(&w, piano_spmi_tag_of("PianoThermal"));
  CHECK(a.tag == 0x57494649u && b.tag == 0x54484552u);   // "WIFI", "THER"
  CHECK(piano_spmi_tag_of("PianoDwc3") == 0x44574333u && piano_spmi_tag_of("NativeUfs") == 0x55465320u);   // "DWC3", "UFS "
  CHECK(piano_spmi_tag_of("") == 0x20202020u);

  // take / give: the word, the holder, the mask
  PianoSpmiToken t = piano_spmi_take(&a);
  CHECK(t.kind == PianoSpmiTokenOuter && w.owner == a.tag && w.holder == hSelf(nullptr) && w.depth == 1 && tMaskDepth == 1);
  // nested: only counts
  PianoSpmiToken n = piano_spmi_take(&b);
  CHECK(n.kind == PianoSpmiTokenNested && w.owner == a.tag && w.depth == 2 && b.nested == 1 && tMaskDepth == 2);
  piano_spmi_give(&b, n);
  CHECK(w.owner == a.tag && w.depth == 1 && tMaskDepth == 1);
  piano_spmi_give(&a, t);
  CHECK(w.owner == 0 && w.holder == 0 && w.depth == 0 && tMaskDepth == 0 && a.takes == 1 && w.takes == 1 && a.lost == 0);

  // no lock: nothing happens
  PianoSpmiToken none = piano_spmi_take(nullptr);
  CHECK(none.kind == PianoSpmiTokenNone && tMaskDepth == 0);
  piano_spmi_give(nullptr, none);

  // contention: another thread holds it for a while; we wait (unmasked), then get it and count the wait
  {
    std::atomic<bool> held{false}, release{false};
    std::thread other([&] {
      PianoSpmiToken x = piano_spmi_take(&b);
      held = true;
      while (!release) std::this_thread::yield();
      piano_spmi_give(&b, x);
    });
    while (!held) std::this_thread::yield();
    std::thread releaser([&] {
      for (int i = 0; i < 2000; ++i) std::this_thread::yield();
      release = true;
    });
    gMaskedSpins = 0;
    PianoSpmiToken y = piano_spmi_take(&a);
    CHECK(y.kind == PianoSpmiTokenOuter && w.owner == a.tag);
    CHECK(a.contended == 1 && a.last_other == b.tag && w.contended == 1 && w.last_waiter == a.tag && w.last_holder == b.tag);
    CHECK(gMaskedSpins == 0);
    piano_spmi_give(&a, y);
    other.join();
    releaser.join();
    CHECK(w.owner == 0 && a.taken_over == 0 && b.lost == 0);
  }

  // the bound: a holder that never gives it back (a word left set) is taken over, counted; its late give is "lost"
  {
    PianoSpmiWords v{};
    PianoSpmiLock c = makeLock(&v, 0x43414d45u), d = makeLock(&v, 0x544f5243u);
    c.bound_us = d.bound_us = 200;
    PianoSpmiToken stuck{};
    std::thread holder([&] { stuck = piano_spmi_take(&c); });
    holder.join();   // its thread is gone with the word still set
    CHECK(v.owner == c.tag);
    PianoSpmiToken e = piano_spmi_take(&d);
    CHECK(e.kind == PianoSpmiTokenOuter && v.owner == d.tag && d.taken_over == 1 && v.taken_over == 1);
    piano_spmi_give(&d, e);
    CHECK(v.owner == 0);
    tMaskDepth = 1;                  // the stuck token's give, on its own (masked) state
    piano_spmi_give(&c, stuck);      // not the holder any more: counted, nothing freed
    CHECK(c.lost == 1 && v.owner == 0 && tMaskDepth == 0);
  }

  // The bound counts the waiter's own spins while no new hold begins: holds that keep changing hands (other kexts' transactions)
  // never make it take over, however long it waits; nor does its own preemption (the clock jumping between two spins).
  {
    PianoSpmiWords v{};
    PianoSpmiLock e = makeLock(&v, 0x57494649u);
    e.bound_us = 100;
    v.owner = 0x504f5745u;   // "POWE" holds it
    v.holder = 12345;
    unsigned spins = 0;
    gSpinHook = [&] {
      ++spins;
      gClockUs += 1000000;                 // preempted for a second between two spins
      if (spins < 1000) v.takes = v.takes + 1;   // meanwhile others' transactions come and go (the word handed on each time)
    };
    PianoSpmiToken x = piano_spmi_take(&e);
    gSpinHook = nullptr;
    CHECK(x.kind == PianoSpmiTokenOuter && e.taken_over == 1 && spins >= 999 + 100 && spins <= 999 + 101);   // the last change at spin 999
    piano_spmi_give(&e, x);
    CHECK(v.owner == 0 && tMaskDepth == 0);
  }

  char line[512];
  piano_spmi_format(&a, line, sizeof line);
  CHECK(std::strstr(line, "SPMI lock: ours 2 tx (") && std::strstr(line, "), waited 1 (") && std::strstr(line, "last behind THER") &&
        std::strstr(line, "; all kexts ") && std::strlen(line) < 300);
  piano_spmi_format_part(&a, line, sizeof line, PianoSpmiLineAll);
  CHECK(std::strncmp(line, "SPMI lock: all kexts ", 21) == 0);
  piano_spmi_format_part(&a, line, sizeof line, PianoSpmiLineOurs);
  CHECK(std::strstr(line, "(err 0, timeout 0, retry 0, unstable 0)") && !std::strstr(line, "all kexts"));
}

// ------------------------------------------------------------------------------------------------- one channel
struct OneChannel {
  uint32_t cmd = 0, status = pianospmi::kDone, wdata0 = 0, wdata1 = 0, rdata0 = 0, rdata1 = 0;
  unsigned commands = 0, finishAfter = 0, polls = 0, failFirst = 0, denyAll = 0;
  PianoSpmiWords *words = nullptr;
  bool heldAtGap = false, heldAtEveryPoll = true, doneAtGive = true;
  uint32_t rd(uint32_t off)
  {
    if (words && !words->owner) heldAtEveryPoll = false;
    if (off == pianospmi::kStatus) {
      if (!(status & pianospmi::kDone) && ++polls >= finishAfter) status = pianospmi::kDone;
      return status;
    }
    return off == pianospmi::kRdata0 ? rdata0 : off == pianospmi::kRdata1 ? rdata1 : 0;
  }
  void wr(uint32_t off, uint32_t v)
  {
    if (off == pianospmi::kWdata0) wdata0 = v;
    if (off == pianospmi::kWdata1) wdata1 = v;
    if (off != pianospmi::kCmd) return;
    cmd = v;
    ++commands;
    polls = 0;
    if (denyAll) status = pianospmi::kDone | pianospmi::kDenied;
    else if (commands <= failFirst) status = pianospmi::kDone | pianospmi::kDropped;
    else status = finishAfter ? 0 : pianospmi::kDone;
  }
  void delay(unsigned us)
  {
    if (us == pianospmi::kRetryGapUs && words && words->owner) heldAtGap = true;
  }
};

static void testPrimitives()
{
  using namespace pianospmi;
  PianoSpmiWords w{};
  PianoSpmiLock l = makeLock(&w, 0x54455354u);
  OneChannel ch;
  ch.words = &w;
  auto rd = [&](uint32_t off) { return ch.rd(off); };
  auto wr = [&](uint32_t off, uint32_t v) { ch.wr(off, v); };
  auto dl = [&](unsigned us) { ch.delay(us); };

  // a read of 6 bytes: the command word, the bytes, the hold around the poll
  ch.rdata0 = 0x44332211;
  ch.rdata1 = 0x6655;
  uint8_t v[8] = {};
  uint32_t st = 0;
  CHECK(readOnce(rd, wr, dl, 0x40, v, 6, &st, &l) == Result::ok);
  CHECK(ch.cmd == (1u << 27 | 0x40u << 4 | 5u) && st == kDone && v[0] == 0x11 && v[3] == 0x44 && v[4] == 0x55 && v[5] == 0x66);
  CHECK(ch.heldAtEveryPoll && w.owner == 0 && l.takes == 1 && tMaskDepth == 0);
  // the range: refused before any store
  const unsigned before = ch.commands;
  CHECK(readOnce(rd, wr, dl, 0xfc, v, 8, &st, &l) == Result::error && readOnce(rd, wr, dl, 0, v, 0, &st, &l) == Result::error &&
        readOnce(rd, wr, dl, 0, v, 9, &st, &l) == Result::error && ch.commands == before);

  // two dropped commands, then ok: retried with the lock free between
  ch.failFirst = ch.commands + 2;
  unsigned retried = 0;
  CHECK(read(rd, wr, dl, 0x04, v, 2, &st, &l, &retried) == Result::ok && retried == 2 && !ch.heldAtGap && l.retries == 2 &&
        l.errors == 2);
  // always dropped: five tries in all, then the error
  ch.failFirst = ch.commands + 100;
  const unsigned c0 = ch.commands;
  CHECK(read(rd, wr, dl, 0x04, v, 2, &st, &l, &retried) == Result::error && retried == kRetries && ch.commands - c0 == 5 &&
        (st & kDropped));
  ch.failFirst = 0;
  // denied: final at once
  ch.denyAll = 1;
  const unsigned c1 = ch.commands;
  CHECK(read(rd, wr, dl, 0x04, v, 2, &st, &l, &retried) == Result::error && retried == 0 && ch.commands - c1 == 1 && (st & kDenied));
  ch.denyAll = 0;

  // a timeout: the hold stays until the channel is done (the drain), so the next holder finds the bus quiet
  ch.finishAfter = kPollUs + 300;
  CHECK(readOnce(rd, wr, dl, 0x08, v, 1, &st, &l) == Result::timeout && (ch.status & kDone) && ch.heldAtEveryPoll &&
        l.timeouts == 1);
  ch.finishAfter = 0;

  // a write of 7 bytes: data words, then the command
  const uint8_t bytes[7] = {1, 2, 3, 4, 5, 6, 7};
  CHECK(write(rd, wr, dl, 0x4f, bytes, 7, &st, &l) == Result::ok && ch.wdata0 == 0x04030201u && ch.wdata1 == 0x00070605u &&
        ch.cmd == (0u << 27 | 0x4fu << 4 | 6u));

  // readStable: two equal answers in a row; a value that keeps changing is an error
  unsigned flip = 0;
  auto rdFlip = [&](uint32_t off) { return off == kRdata0 ? uint32_t(flip++ & 1 ? 0x04 : 0x1c) : ch.rd(off); };
  CHECK(readStable(rdFlip, wr, dl, 0x04, v, 1, &st, &l) == Result::error && l.unstable >= 3);
  ch.rdata0 = 0x1c;
  CHECK(readStable(rd, wr, dl, 0x04, v, 1, &st, &l) == Result::ok && v[0] == 0x1c);
  flip = 0;
  unsigned n = 0;
  auto rdOnceWrong = [&](uint32_t off) { return off == kRdata0 ? uint32_t(n++ == 0 ? 0x00 : 0x04) : ch.rd(off); };
  CHECK(readStable(rdOnceWrong, wr, dl, 0x04, v, 1, &st, &l) == Result::ok && v[0] == 0x04);   // one wrong answer outvoted
  CHECK(w.owner == 0 && tMaskDepth == 0);
}

// ------------------------------------------------------------------------------------------------- the model of the arbiter
// APIDs 0..63; peripheral registers pmic[apid][reg] fixed (reads must return them), except the scratch APIDs the writers own.
// One command engine for EE 0's channels: a command takes `latency` status reads (of any channel) to finish; a command stored
// while another is in flight is dropped (done | dropped). Every observer channel's read data shows the engine's last result.
struct ArbModel {
  static constexpr unsigned kApids = 64, kAdcApid = 40, kTorchApid = 41;
  std::mutex m;
  uint8_t pmic[kApids][256];
  uint32_t obsStatus[kApids], chnStatus[kApids], chnData0[kApids], chnData1[kApids];
  int busy = -1;   // the channel in flight: apid, + 1000 for an owner channel
  unsigned left = 0, latency = 4;
  uint32_t busyCmd = 0, rdata0 = 0, rdata1 = 0;
  unsigned drops = 0, overtaken = 0, completed = 0;

  ArbModel()
  {
    for (unsigned a = 0; a < kApids; ++a) {
      for (unsigned r = 0; r < 256; ++r) pmic[a][r] = uint8_t(a * 7 + r * 13 + 5);
      obsStatus[a] = chnStatus[a] = pianospmi::kDone;
      chnData0[a] = chnData1[a] = 0;
    }
  }
  static uint8_t expect(unsigned a, unsigned r) { return uint8_t(a * 7 + r * 13 + 5); }
  void command(int ch, uint32_t v)
  {
    std::lock_guard<std::mutex> g(m);
    uint32_t &status = ch >= 1000 ? chnStatus[ch - 1000] : obsStatus[ch];
    if (busy >= 0) {
      status = pianospmi::kDone | pianospmi::kDropped;
      ++drops;
      return;
    }
    busy = ch;
    busyCmd = v;
    left = latency;
    status = 0;
  }
  uint32_t status(int ch)
  {
    std::lock_guard<std::mutex> g(m);
    if (busy >= 0 && --left == 0) finish();
    return ch >= 1000 ? chnStatus[ch - 1000] : obsStatus[ch];
  }
  void finish()   // with m held
  {
    const unsigned apid = unsigned(busy % 1000), reg = (busyCmd >> 4) & 0xff, bytes = (busyCmd & 7) + 1, op = busyCmd >> 27;
    if (op == pianospmi::kOpExtReadL) {
      uint32_t lo = 0, hi = 0;
      for (unsigned b = 0; b < bytes && reg + b < 256; ++b) (b < 4 ? lo : hi) |= uint32_t(pmic[apid][reg + b]) << (8 * (b & 3));
      rdata0 = lo;
      rdata1 = hi;
      obsStatus[apid] = pianospmi::kDone;
    } else {
      for (unsigned b = 0; b < bytes && reg + b < 256; ++b)
        pmic[apid][reg + b] = uint8_t((b < 4 ? chnData0[apid] : chnData1[apid]) >> (8 * (b & 3)));
      chnStatus[apid] = pianospmi::kDone;
    }
    ++completed;
    busy = -1;
  }
  uint32_t rdata(int which)
  {
    std::lock_guard<std::mutex> g(m);
    return which ? rdata1 : rdata0;
  }
  void wdata(unsigned apid, int which, uint32_t v)
  {
    std::lock_guard<std::mutex> g(m);
    (which ? chnData1 : chnData0)[apid] = v;
  }
};
static ArbModel *gArb;

// The windows as the kexts' Io see them: obsv at 0x20 * APID, a channel's command/status/data at +0x00/+0x08/+0x18/+0x1c; chnl is
// the owner channel page of one APID.
static uint32_t obsvRd(uint32_t off)
{
  const int apid = int(off / 0x20);
  const uint32_t r = off % 0x20;
  std::this_thread::yield();
  if (r == pianospmi::kStatus) return gArb->status(apid);
  if (r == pianospmi::kRdata0) return gArb->rdata(0);
  if (r == pianospmi::kRdata1) return gArb->rdata(1);
  return 0;
}
static void obsvWr(uint32_t off, uint32_t v)
{
  if (off % 0x20 == pianospmi::kCmd) gArb->command(int(off / 0x20), v);
}
static void hostDelay(unsigned us)
{
  (void)us;
  std::this_thread::yield();
}
static uint32_t coreRd(uint32_t off)
{
  if (off == 0) return 0x70010000u;
  if (off == 4) return ArbModel::kApids;
  if (off >= 0x2000 && off < 0x2000 + 4 * ArbModel::kApids) return uint32_t((off - 0x2000) / 4 + 0x100) << 8 | 1u;   // PPID 0x100+i
  return 0;
}

// Each kext's Io over the model.
namespace wifiio {
using namespace pianowifi;
uint32_t rd(void *, Window w, uint32_t off) { return w == Window::obsv ? obsvRd(off) : w == Window::core ? coreRd(off) : 0; }
void wr(void *, Window w, uint32_t off, uint32_t v)
{
  if (w == Window::obsv) obsvWr(off, v);
}
void delay(void *, unsigned us) { hostDelay(us); }
const Io io = {nullptr, rd, wr, delay};
}  // namespace wifiio
namespace powerio {
using namespace pianopower;
uint32_t rd(void *, Window w, uint32_t off) { return w == Window::obsv ? obsvRd(off) : w == Window::core ? coreRd(off) : 0; }
void wr(void *, Window w, uint32_t off, uint32_t v)
{
  if (w == Window::obsv) obsvWr(off, v);
}
void delay(void *, unsigned us) { hostDelay(us); }
const Io io = {nullptr, rd, wr, delay};
}  // namespace powerio
namespace inputio {
using namespace pianoinput;
uint32_t rd(void *, Window w, uint32_t off) { return w == Window::obsv ? obsvRd(off) : w == Window::core ? coreRd(off) : 0; }
void wr(void *, Window w, uint32_t off, uint32_t v)
{
  if (w == Window::obsv) obsvWr(off, v);
}
void delay(void *, unsigned us) { hostDelay(us); }
const Io io = {nullptr, rd, wr, delay};
}  // namespace inputio
namespace adcio {
using namespace pianothermal::adc;
uint32_t rd(void *, Win w, uint32_t off)
{
  if (w == Win::obsv) return obsvRd(off);
  if (w == Win::core) return coreRd(off);
  if (w == Win::chnl && off == pianospmi::kStatus) return gArb->status(1000 + int(ArbModel::kAdcApid));
  return 0;
}
void wr(void *, Win w, uint32_t off, uint32_t v)
{
  if (w == Win::obsv) obsvWr(off, v);
  if (w != Win::chnl) return;
  if (off == pianospmi::kWdata0 || off == pianospmi::kWdata1) gArb->wdata(ArbModel::kAdcApid, off == pianospmi::kWdata1, v);
  if (off == pianospmi::kCmd) gArb->command(1000 + int(ArbModel::kAdcApid), v);
}
void delay(void *, uint32_t us) { hostDelay(us); }
uint64_t now(void *) { return gClockUs.load(); }
const Io io = {nullptr, rd, wr, delay, now};
}  // namespace adcio
namespace torchio {
uint32_t rd(void *, uint32_t off) { return off == pianospmi::kStatus ? gArb->status(1000 + int(ArbModel::kTorchApid)) : 0; }
void wr(void *, uint32_t off, uint32_t v)
{
  if (off == pianospmi::kWdata0 || off == pianospmi::kWdata1) gArb->wdata(ArbModel::kTorchApid, off == pianospmi::kWdata1, v);
  if (off == pianospmi::kCmd) gArb->command(1000 + int(ArbModel::kTorchApid), v);
}
void delay(void *, unsigned us) { hostDelay(us); }
const pianotorch::ChnlIo io = {nullptr, rd, wr, delay};
}  // namespace torchio

struct Tally {
  std::atomic<unsigned> reads{0}, failed{0}, wrong{0}, writes{0}, writeFailed{0}, writeWrong{0};
};

// One reader thread: `kind` picks the kext's accessor; random APIDs of the fixed ones (0..39), registers and lengths.
static void reader(int kind, unsigned seed, unsigned count, Tally &t)
{
  std::mt19937 rng(seed);
  pianothermal::adc::Bus bus;
  bus.io = adcio::io;
  for (unsigned i = 0; i < count; ++i) {
    const int apid = int(rng() % 40);
    const unsigned bytes = kind == 2 ? 1 : 1 + rng() % 8;
    const uint8_t reg = uint8_t(rng() % (257 - bytes));
    uint8_t v[8] = {};
    bool ok = false;
    switch (kind) {
      case 0: ok = pianowifi::spmiRead(wifiio::io, apid, reg, v, bytes) == pianowifi::ReadResult::ok; break;
      case 1: ok = pianopower::spmiRead(powerio::io, apid, reg, v, bytes) == pianopower::ReadResult::ok; break;
      case 2: ok = pianoinput::spmiRead8(inputio::io, apid, reg, v) == pianoinput::ReadResult::ok; break;
      default: ok = bus.readApid(apid, reg, v, bytes) == pianothermal::adc::Err::none; break;
    }
    ++t.reads;
    if (!ok) {
      ++t.failed;
      continue;
    }
    for (unsigned b = 0; b < bytes; ++b)
      if (v[b] != ArbModel::expect(unsigned(apid), reg + b)) {
        ++t.wrong;
        break;
      }
  }
}

// One writer thread: PianoThermal's ADC writes (7 command bytes at 0x4f) or PianoTorch's one-byte writes, each read back through the
// observer channel (its own APID: nobody else writes there).
static void writer(int kind, unsigned seed, unsigned count, Tally &t)
{
  std::mt19937 rng(seed);
  pianothermal::adc::Bus bus;
  bus.io = adcio::io;
  bus.apid = int(ArbModel::kAdcApid);
  bus.writes = true;
  for (unsigned i = 0; i < count; ++i) {
    uint8_t value[7], back[7] = {};
    for (uint8_t &b : value) b = uint8_t(rng());
    bool ok, same = true;
    if (kind == 0) {
      ok = bus.write(pianothermal::adc::kRegSid, value, 7) == pianothermal::adc::Err::none &&
           bus.read(pianothermal::adc::kRegSid, back, 7) == pianothermal::adc::Err::none;
      for (unsigned b = 0; ok && b < 7; ++b) same = same && back[b] == value[b];
    } else {
      ok = pianotorch::spmiWriteByte(torchio::io, 0x46, value[0]) == pianotorch::WriteResult::ok &&
           pianopower::spmiRead(powerio::io, int(ArbModel::kTorchApid), 0x46, back, 1) == pianopower::ReadResult::ok;
      same = ok && back[0] == value[0];
    }
    ++t.writes;
    if (!ok) ++t.writeFailed;
    else if (!same) ++t.writeWrong;
  }
}

static void run(Tally &t, unsigned perThread)
{
  std::vector<std::thread> threads;
  for (int k = 0; k < 4; ++k) threads.emplace_back(reader, k, 100 + unsigned(k), perThread, std::ref(t));
  for (int k = 0; k < 2; ++k) threads.emplace_back(writer, k, 200 + unsigned(k), perThread / 2, std::ref(t));
  for (std::thread &th : threads) th.join();
}

static void testConcurrency()
{
  // Without the lock: the model must show both failures (else the test proves nothing).
  {
    ArbModel arb;
    gArb = &arb;
    piano_spmi_host_lock = nullptr;
    Tally t;
    unsigned rounds = 0;
    while ((t.failed == 0 || t.wrong == 0) && rounds++ < 5) run(t, 1500);
    std::printf("spmi_lock_host: without the lock: %u reads, %u failed, %u wrong; %u writes, %u failed, %u read back wrong; %u "
                "commands dropped\n",
                t.reads.load(), t.failed.load(), t.wrong.load(), t.writes.load(), t.writeFailed.load(), t.writeWrong.load(),
                arb.drops);
    CHECK(t.failed > 0 && t.wrong > 0 && arb.drops > 0);
  }
  // With it: every value right, nothing dropped, the waits counted, the mask balanced.
  {
    ArbModel arb;
    gArb = &arb;
    PianoSpmiWords w{};
    PianoSpmiLock l = makeLock(&w, piano_spmi_tag_of("PianoHost"));
    l.bound_us = 0xffffffffu;   // no take-over here (the host scheduler may park a holder for long; the bound is tested above)
    piano_spmi_host_lock = &l;
    gMaskedSpins = 0;
    Tally t;
    run(t, 1500);
    char line[512];
    piano_spmi_format(&l, line, sizeof line);
    std::printf("spmi_lock_host: with the lock: %u reads, %u failed, %u wrong; %u writes, %u failed, %u read back wrong; %u "
                "commands dropped\n  %s\n",
                t.reads.load(), t.failed.load(), t.wrong.load(), t.writes.load(), t.writeFailed.load(), t.writeWrong.load(),
                arb.drops, line);
    CHECK(t.failed == 0 && t.wrong == 0 && t.writeFailed == 0 && t.writeWrong == 0 && arb.drops == 0);
    CHECK(l.contended > 0 && w.owner == 0 && w.depth == 0 && l.errors == 0 && l.timeouts == 0 && gMaskedSpins == 0);
    CHECK(l.takes == w.takes && l.takes >= t.reads + 2 * t.writes);
    piano_spmi_host_lock = nullptr;
  }
}

int main()
{
  testProtocol();
  testPrimitives();
  testConcurrency();
  if (failures) {
    std::fprintf(stderr, "spmi_lock_host: %d check(s) failed\n", failures);
    return 1;
  }
  std::printf("spmi_lock_host: all checks passed\n");
  return 0;
}
