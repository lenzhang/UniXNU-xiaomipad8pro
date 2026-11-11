// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoAudio's level 1 survey (AudioSurvey.h) against register models:
//   - what is loaded: exactly the fourteen audio pins' two TLMM words each, nothing in an LPASS block, the I2C hub's engine, the
//     DSP's registers or anywhere else; the SMMU scan reads only the global page;
//   - the facts tables: slots 0..3 in order, every amplifier's pins of the right role, the TDM bit clock;
//   - the decoders: the TLMM decode, the reset line classes, the bus and TDM mux words, the verdicts;
//   - the SMMU stream scan with masks, the overflow count and a bad geometry.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. audio_host.cpp -o /tmp/audio_host && /tmp/audio_host
#include "AudioSurvey.h"
#include "AudioPins.h"
#include "AudioAmps.h"
#include "SmmuStream.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace pianoaudio;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ---------------------------------------------------------------------------------------------------------- the MMIO model
struct Soc {
  std::map<uint64_t, uint32_t> regs;
  std::vector<uint64_t> loads;
  uint32_t load(uint64_t pa)
  {
    loads.push_back(pa);
    auto it = regs.find(pa);
    return it == regs.end() ? 0 : it->second;
  }
  unsigned in(uint64_t base, uint64_t bytes) const
  {
    unsigned n = 0;
    for (uint64_t pa : loads) n += pa >= base && pa < base + bytes;
    return n;
  }
  // TLMM configuration word: pull 1:0, function 5:2, drive 8:6, output enable 9; io: in bit 0, out bit 1
  void pin(unsigned gpio, unsigned function, unsigned pull, bool oe, bool out, bool in, unsigned drive = 0)
  {
    regs[kTlmm + uint64_t(gpio) * kTlmmStride] = pull | function << 2 | drive << 6 | unsigned(oe) << 9;
    regs[kTlmm + uint64_t(gpio) * kTlmmStride + 4] = unsigned(in) | unsigned(out) << 1;
  }
};

static Survey run(Soc &soc)
{
  Survey s;
  surveyPins(s, [&](uint64_t pa) { return soc.load(pa); });
  return s;
}

static void testTables()
{
  CHECK(kPinCount == 14);
  CHECK(kAmpCount == 4);
  std::set<unsigned> gpios, addresses;
  for (unsigned i = 0; i < kPinCount; ++i) gpios.insert(kPins[i].gpio);
  CHECK(gpios.size() == kPinCount);   // no pin twice
  for (unsigned a = 0; a < kAmpCount; ++a) {
    CHECK(kAmps[a].slot == a);       // the table is in slot order
    CHECK(kPins[kAmps[a].resetPin].role == Role::reset);
    CHECK(kPins[kAmps[a].irqPin].role == Role::irq);
    addresses.insert(kAmps[a].address);
  }
  CHECK(addresses == (std::set<unsigned>{0x34, 0x35, 0x36, 0x37}));
  // the DT's pairs: 0x34 reset gpio128 irq gpio66, 0x35 129/84, 0x36 107/81, 0x37 108/67
  const unsigned want[4][3] = {{0x35, 129, 84}, {0x37, 108, 67}, {0x34, 128, 66}, {0x36, 107, 81}};
  for (unsigned a = 0; a < kAmpCount; ++a) {
    CHECK(kAmps[a].address == want[a][0]);
    CHECK(kPins[kAmps[a].resetPin].gpio == want[a][1]);
    CHECK(kPins[kAmps[a].irqPin].gpio == want[a][2]);
  }
  CHECK(kPins[kPinSda].gpio == 64 && kPins[kPinScl].gpio == 65);
  for (unsigned i = 0; i < 4; ++i) CHECK(kPins[kPinTdmFirst + i].role == Role::tdm && kPins[kPinTdmFirst + i].gpio == 121 + i);
  CHECK(kTdmBitClock == 6144000);
}

static void testLoads()
{
  Soc soc;
  const Survey s = run(soc);
  CHECK(soc.loads.size() == 2 * kPinCount);
  CHECK(s.loads == 2 * kPinCount);
  std::set<uint64_t> allowed;
  for (unsigned i = 0; i < kPinCount; ++i) {
    allowed.insert(kTlmm + uint64_t(kPins[i].gpio) * kTlmmStride);
    allowed.insert(kTlmm + uint64_t(kPins[i].gpio) * kTlmmStride + 4);
  }
  for (uint64_t pa : soc.loads) CHECK(allowed.count(pa) == 1);
  for (const Block &b : kLpassBlocks) CHECK(soc.in(b.base, b.bytes) == 0);
  CHECK(soc.in(0x980000, 0x4000) == 0);    // the I2C hub's SE0 engine
  CHECK(soc.in(0x9c0000, 0x2000) == 0);    // the hub wrapper
  CHECK(soc.in(0x6800000, 0x10000) == 0);  // the DSP
}

static void testDecode()
{
  const PinDecode d = decodePin(0x00000201 | 3u << 6, 0x2);   // GPIO, pull-down, 8 mA, output enabled, driven high
  CHECK(d.function == 0 && d.pull == 1 && d.driveMa == 8 && d.outputEnable && d.out && !d.in);
  CHECK(resetState(d) == Reset::released);
  CHECK(resetState(decodePin(0x201, 0x0)) == Reset::held);
  CHECK(resetState(decodePin(0x001, 0x1)) == Reset::undriven);
  CHECK(resetState(decodePin(0x004, 0x0)) == Reset::other);
  CHECK(!std::strcmp(pullName(3), "up") && !std::strcmp(resetName(Reset::held), "held"));
}

static void testSummary()
{
  {   // the likely hand-over: amplifiers held in reset, the bus on the hub's function with pull-ups and idle, TDM pins left on GPIO
    Soc soc;
    for (const Amp &a : kAmps) {
      soc.pin(kPins[a.resetPin].gpio, 0, 1, true, false, false);
      soc.pin(kPins[a.irqPin].gpio, 0, 0, false, false, true);
    }
    soc.pin(64, 1, 3, false, false, true);
    soc.pin(65, 1, 3, false, false, true);
    const Summary m = summarize(run(soc));
    CHECK(m.held == 4 && m.released == 0);
    CHECK(m.busMuxed && m.busPullUp && m.busIdle);
    CHECK(m.tdmMuxed == 0);
    for (unsigned a = 0; a < kAmpCount; ++a) CHECK(m.irqIn[a] && m.irqGpio[a] && m.reset[a] == Reset::held);
    CHECK(std::strstr(resetVerdict(m), "held in shutdown") != nullptr);
  }
  {   // Linux's running state: resets released, TDM muxed, SDA held low
    Soc soc;
    for (const Amp &a : kAmps) soc.pin(kPins[a.resetPin].gpio, 0, 0, true, true, true);
    soc.pin(64, 1, 3, false, false, false);
    soc.pin(65, 1, 3, false, false, true);
    for (unsigned g = 121; g <= 124; ++g) soc.pin(g, 2, 0, false, false, false, 3);
    const Summary m = summarize(run(soc));
    CHECK(m.released == 4);
    CHECK(m.busMuxed && !m.busIdle);
    CHECK(m.tdmMuxed == 4 && m.tdmFunction[0] == 2);
    CHECK(std::strstr(resetVerdict(m), "released") != nullptr);
  }
  {   // nothing configured: every word reads 0 (GPIO, input, no pull): undriven resets, the bus not muxed
    Soc soc;
    const Summary m = summarize(run(soc));
    CHECK(m.undriven == 4 && !m.busMuxed && !m.busPullUp && !m.busIdle);
    CHECK(std::strstr(resetVerdict(m), "no reset line is driven") != nullptr);
  }
  {   // mixed resets
    Soc soc;
    soc.pin(kPins[kAmps[0].resetPin].gpio, 0, 0, true, true, true);
    const Summary m = summarize(run(soc));
    CHECK(m.released == 1 && m.undriven == 3);
    CHECK(std::strstr(resetVerdict(m), "disagree") != nullptr);
  }
}

static void testSmmu()
{
  std::map<uint32_t, uint32_t> r;
  r[0x20] = 0x40;                           // 64 stream-match groups
  r[0x24] = 0x60000053;                     // 83 banks, 128 pages (IDR1 NUMPAGENDXB 6)
  r[0x800 + 4 * 3] = 0x80000000u | 0x0080u << 16 | 0x1001;   // the DT's <0x1001 0x80>
  r[0xc00 + 4 * 3] = 1u << 16;                                // bypass
  r[0x800 + 4 * 9] = 0x80000000u | 0x1081;                    // 0x1081 exactly: not 0x1001 (no mask)
  r[0x800 + 4 * 12] = 0x80000000u | 0x007fu << 16 | 0x1000;  // 0x1000..0x107f: both streams
  r[0xc00 + 4 * 12] = 0x00000005;                             // translate, bank 5
  r[0x800 + 4 * 20] = 0x00001001;                             // not valid
  std::vector<uint32_t> reads;
  const uint16_t streams[2] = {kStreamApm, kStreamApm2};
  const SmmuStreams s = surveySmmu(streams, kSmmuBytes, [&](uint32_t off) { reads.push_back(off); return r[off]; });
  CHECK(!s.error);
  CHECK(s.groups == 64 && s.banks == 83 && s.pages == 128);
  CHECK(s.count[0] == 2 && s.found[0][0].group == 3 && s.found[0][1].group == 12);
  CHECK(!std::strcmp(s2crType(s.found[0][0].s2cr), "bypass") && !std::strcmp(s2crType(s.found[0][1].s2cr), "translate"));
  CHECK(s.count[1] == 1 && s.found[1][0].group == 12);
  for (uint32_t off : reads) CHECK(off < 0x1000);   // the global page only: no context bank is read
  {   // more than four matches: four recorded, the rest counted
    std::map<uint32_t, uint32_t> m = r;
    for (unsigned g = 30; g < 36; ++g) m[0x800 + 4 * g] = 0x80000000u | 0x1001;
    const SmmuStreams t = surveySmmu(streams, kSmmuBytes, [&](uint32_t off) { return m[off]; });
    CHECK(t.count[0] == 4 && t.more[0] == 4);
  }
  std::map<uint32_t, uint32_t> bad;
  bad[0x20] = 0x40;
  bad[0x24] = 0x0;   // no banks
  CHECK(surveySmmu(streams, kSmmuBytes, [&](uint32_t off) { return bad[off]; }).error != nullptr);
}

// ---------------------------------------------------------------------------------------- levels 4-6: the pins and the SMMU
static void testPins()
{
  // the TDM pins from GPIO with a pull-down and 2 mA to i2s1 (function 1), 8 mA, no bias; nothing else changed
  const uint32_t gpio = 0x1u | 0u << 2 | 0u << 6;
  CHECK(tdmCtl(gpio) == (1u << 2 | 3u << 6));
  CHECK(decodePin(tdmCtl(gpio), 0).function == 1 && decodePin(tdmCtl(gpio), 0).driveMa == 8 && decodePin(tdmCtl(gpio), 0).pull == 0);
  CHECK(tdmCtl(0x3ffu) == (1u << 2 | 3u << 6));   // output enable, pull and drive all replaced
  // a reset line: GPIO, driven, the value first; an eGPIO pin claimed for this TLMM, a plain one left without the bit
  CHECK(resetCtl(0x0u) == kCtlOe && resetCtl(5u << 2 | 1u) == (kCtlOe | 1u));
  CHECK(resetCtl(kCtlEgpioPresent) == (kCtlEgpioPresent | kCtlEgpioEnable | kCtlOe));
  CHECK(resetIo(true) == 2 && resetIo(false) == 0);
  CHECK(resetState(decodePin(resetCtl(0), resetIo(false))) == Reset::held && resetState(decodePin(resetCtl(0), resetIo(true))) == Reset::released);
  // the bus: GPIO with the pull-up, low = output enabled
  CHECK(i2cCtl(kFunctionHub0 << 2 | 2u << 6, false) == (kCtlPullUp | 2u << 6) && i2cCtl(kFunctionHub0 << 2, true) == (kCtlPullUp | kCtlOe));
  CHECK(foreign(kCtlEgpioPresent) && !foreign(kCtlEgpioPresent | kCtlEgpioEnable) && !foreign(0));
  // the amplifier table the C side uses is AudioHw.h's, slot by slot
  const uint8_t addresses[4] = AUDIO_AMP_ADDRESSES;
  for (unsigned a = 0; a < kAmpCount; ++a) CHECK(addresses[a] == kAmps[a].address);
  // what level 4 reads lies in the VA macro's block and the LPI TLMM's (both DT okay), and nowhere else
  for (const LpassRead &r : kVaIds) CHECK(r.pa >= 0x07660000 && r.pa < 0x07662000 && r.pa % 4 == 0);
  for (unsigned i = 0; i < kDmicPinCount; ++i) CHECK(lpiPin(kDmicPins[i]) + 4 < 0x07760000 + 0x20000);
  const VaIdentity id = vaIdentity(0x02, 0x0f, 0x30);
  CHECK(id.major == 2 && id.minor == 3 && id.step == 0 && id.hasVa && !vaIdentity(1, 0x7, 0).hasVa);
}

// The SMMU model of PianoDwc3's SmmuStream.h tests, for the DSP's four PCM stream ids.
struct SmmuModel {
  std::map<uint32_t, uint32_t> r;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  SmmuModel()
  {
    r[0x20] = 1u << 30 | 0x40;    // AArch64 stage 1, 64 groups
    r[0x24] = 0x60000053;         // 83 banks, 128 pages
    r[0x28] = 1u << 12;
  }
  uint32_t rd(uint32_t off) { return r.count(off) ? r[off] : 0; }
  void wr(uint32_t off, uint32_t v) { writes.push_back({off, v}); r[off] = v; }
};
static const uint16_t kAudioStreams[4] = {0x1001, 0x1081, 0x1041, 0x1061};   // 0x1001 mask 0x80, 0x1041 mask 0x20 as exact ids

static void testSmmuBypass()
{
  {   // nothing left by the firmware: each stream a free group and a bypass bank of its own, from the top down
    SmmuModel m;
    unsigned banks[4];
    for (unsigned i = 0; i < 4; ++i) {
      const SmmuPlan p = planSmmuStream(kAudioStreams[i], kSmmuBytes, [&](uint32_t o) { return m.rd(o); });
      CHECK(!p.error && !p.reuse && !p.keep && p.slot == i);
      CHECK(!applySmmuStream(p, kAudioStreams[i], [&](uint32_t o) { return m.rd(o); }, [&](uint32_t o, uint32_t v) { m.wr(o, v); },
                             [] { return false; }));
      CHECK(m.rd(0x800 + 4 * i) == (0x80000000u | kAudioStreams[i]) && m.rd(0xc00 + 4 * i) == p.bank);
      CHECK(m.rd((128 + p.bank) * 4096) == 0 && m.rd(0x1800 + 4 * p.bank) == 1 && m.rd(0x1000 + 4 * p.bank) == 0x10000);
      banks[i] = p.bank;
    }
    CHECK(banks[0] == 81 && banks[1] == 80 && banks[2] == 79 && banks[3] == 78);
  }
  {   // 0x1001 already bypassing in an exact entry: kept, not one write
    SmmuModel m;
    m.r[0x800 + 4 * 5] = 0x80000000u | 0x1001;
    m.r[0xc00 + 4 * 5] = 1u << 16;
    const SmmuPlan p = planSmmuStream(0x1001, kSmmuBytes, [&](uint32_t o) { return m.rd(o); });
    CHECK(!p.error && p.reuse && p.keep);
    CHECK(!applySmmuStream(p, 0x1001, [&](uint32_t o) { return m.rd(o); }, [&](uint32_t o, uint32_t v) { m.wr(o, v); }, [] { return false; }));
    CHECK(m.writes.empty());
  }
  {   // the DT's masked group <0x1001 0x80> left by someone: refused, nothing written (level 6 then fails without the DSP's buffer)
    SmmuModel m;
    m.r[0x800 + 4 * 2] = 0x80000000u | 0x0080u << 16 | 0x1001;
    m.r[0xc00 + 4 * 2] = 1u << 16;
    const SmmuPlan p = planSmmuStream(0x1001, kSmmuBytes, [&](uint32_t o) { return m.rd(o); });
    CHECK(p.error && !std::strcmp(p.error, "the stream sits in a masked stream-match group"));
    CHECK(applySmmuStream(p, 0x1001, [&](uint32_t o) { return m.rd(o); }, [&](uint32_t o, uint32_t v) { m.wr(o, v); }, [] { return false; }));
    CHECK(m.writes.empty());
  }
}

int main()
{
  testTables();
  testLoads();
  testDecode();
  testSummary();
  testSmmu();
  testPins();
  testSmmuBypass();
  if (failures) {
    std::printf("audio_host: %d failures\n", failures);
    return 1;
  }
  std::printf("audio_host: all passed\n");
  return 0;
}
