// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio level 1: what the survey reads and what the readings say. Portable (host tests in tests/audio_host.cpp): every hardware
// access goes through Load, a function of a physical address that returns the 32-bit word there. There is no store anywhere in this
// file, so a level 1 survey cannot write to the hardware.
//
// What is read: the main TLMM's configuration and level words of the fourteen audio pins (always readable: the TLMM is in the
// always-on domain, PianoPower, PianoDwc3 and PianoWifi read it the same way) and the apps SMMU's stream-match entries for the
// DSP's PCM streams. What is not: the amplifiers themselves (an I2C read needs the bus's pins and engine set up and the amplifiers
// out of reset, all writes: level 5), the I2C hub's engine (its clocks' state is not known here), and every LPASS block (macros,
// SoundWire masters, LPI pins: their clocks are the DSP's PRM resources and nothing the application processor can read says
// whether they run; a load from an unclocked block can stall the bus).
#pragma once
#include <stdint.h>
#include "AudioHw.h"

namespace pianoaudio {

// ---------------------------------------------------------------------------------------------------------------- decoders
// The TLMM pin decode of package/PianoWifi/WifiSurvey.h (same layout, re-expressed in this namespace).
struct PinDecode {
  unsigned pull, function, driveMa;
  bool outputEnable, in, out;
};
inline PinDecode decodePin(uint32_t cfg, uint32_t io)
{
  PinDecode d;
  d.pull = cfg & 3u;
  d.function = (cfg >> 2) & 0xfu;
  d.driveMa = (((cfg >> 6) & 7u) + 1) * 2;
  d.outputEnable = (cfg >> 9) & 1u;
  d.in = io & 1u;
  d.out = (io >> 1) & 1u;
  return d;
}
inline const char *pullName(unsigned pull) { return pull == 0 ? "none" : pull == 1 ? "down" : pull == 2 ? "keeper" : "up"; }
constexpr unsigned kPullUp = 3;

// An amplifier's reset line (active low): who holds it where.
enum class Reset : uint8_t {
  held,       // GPIO, output enabled, driven low: the amplifier is held in shutdown
  released,   // GPIO, output enabled, driven high: out of reset (it may answer on the bus)
  undriven,   // GPIO, output disabled: nobody drives it from the SoC (the level read says where the board pulls it)
  other,      // not the GPIO function: muxed to something else
};
inline Reset resetState(const PinDecode &d)
{
  if (d.function != 0) return Reset::other;
  if (!d.outputEnable) return Reset::undriven;
  return d.out ? Reset::released : Reset::held;
}
inline const char *resetName(Reset r)
{
  return r == Reset::held ? "held" : r == Reset::released ? "released" : r == Reset::undriven ? "undriven" : "other";
}

// --------------------------------------------------------------------------------------------------------- the survey record
struct PinState {
  uint32_t cfg = 0, io = 0;
};
struct Survey {
  PinState pins[kPinCount];
  unsigned loads = 0;
};

template <class Load>
void surveyPins(Survey &s, Load load)
{
  for (unsigned i = 0; i < kPinCount; ++i) {
    const uint64_t page = kTlmm + uint64_t(kPins[i].gpio) * kTlmmStride;
    s.pins[i].cfg = load(page);
    s.pins[i].io = load(page + 4);
    s.loads += 2;
  }
}

// ------------------------------------------------------------------------------------- what the record says, in a few words
struct Summary {
  Reset reset[kAmpCount];
  bool resetIn[kAmpCount], irqIn[kAmpCount], irqGpio[kAmpCount];
  unsigned busFunction[2];
  bool busMuxed = false;    // both pins on a function other than GPIO (the hub's i2chub0_se0, whose number is not in the DT)
  bool busPullUp = false;   // both with the internal pull-up, as hub-i2c0-data-clk-state sets them
  bool busIdle = false;     // both read high: an idle bus (a low SDA with SCL high is a held bus)
  unsigned tdmFunction[4];
  unsigned tdmMuxed = 0;    // how many of the four TDM pins are on a function other than GPIO
  unsigned held = 0, released = 0, undriven = 0, other = 0;
};
inline Summary summarize(const Survey &s)
{
  Summary m{};
  for (unsigned a = 0; a < kAmpCount; ++a) {
    const PinState &r = s.pins[kAmps[a].resetPin], &q = s.pins[kAmps[a].irqPin];
    const PinDecode rd = decodePin(r.cfg, r.io), qd = decodePin(q.cfg, q.io);
    m.reset[a] = resetState(rd);
    m.resetIn[a] = rd.in;
    m.irqIn[a] = qd.in;
    m.irqGpio[a] = qd.function == 0;
    m.held += m.reset[a] == Reset::held;
    m.released += m.reset[a] == Reset::released;
    m.undriven += m.reset[a] == Reset::undriven;
    m.other += m.reset[a] == Reset::other;
  }
  const PinDecode sda = decodePin(s.pins[kPinSda].cfg, s.pins[kPinSda].io), scl = decodePin(s.pins[kPinScl].cfg, s.pins[kPinScl].io);
  m.busFunction[0] = sda.function;
  m.busFunction[1] = scl.function;
  m.busMuxed = sda.function != 0 && scl.function != 0;
  m.busPullUp = sda.pull == kPullUp && scl.pull == kPullUp;
  m.busIdle = sda.in && scl.in;
  for (unsigned i = 0; i < 4; ++i) {
    const PinState &p = s.pins[kPinTdmFirst + i];
    m.tdmFunction[i] = decodePin(p.cfg, p.io).function;
    m.tdmMuxed += m.tdmFunction[i] != 0;
  }
  return m;
}

// What level 5 (the amplifiers' bus probe) has to do first, from the reset lines.
inline const char *resetVerdict(const Summary &m)
{
  if (m.held == kAmpCount)
    return "all four amplifiers are held in shutdown (reset driven low): level 5 must release them (reset high) before a bus probe";
  if (m.released == kAmpCount)
    return "all four reset lines are released (driven high): a level 5 bus probe can read DEVID without touching the reset lines";
  if (m.undriven == kAmpCount)
    return "no reset line is driven by the SoC: the board's pull decides (see each line's level); level 5 drives them";
  return "the reset lines disagree (see the amp lines): level 5 sets each one explicitly before a bus probe";
}

// --------------------------------------------------------------------------------------------------- apps SMMU, read only
// package/PianoWifi/WifiSurvey.h's surveySmmu (same repository), re-expressed for this driver's two streams: every stream-match
// group that matches a stream (ARM SMMUv2: SMR valid bit 31, mask 30:16, id 14:0; S2CR type 17:16 (0 translate through context
// bank 7:0, 1 bypass, 2 fault)). Only the global page is read (IDR0/IDR1, the SMRs, the S2CRs): unlike PianoWifi this survey does
// not read a matching context bank's SCTLR, because on SM8550 under the same hypervisor the audio streams' context banks are the
// hypervisor's (UniXNU-sheng porting.toml: programming one reset the board), so their pages are left alone.
struct SmmuMatch {
  unsigned group = 0;
  uint32_t smr = 0, s2cr = 0;
};
struct SmmuStreams {
  uint32_t idr0 = 0, idr1 = 0, groups = 0, banks = 0, pages = 0;
  const char *error = nullptr;
  SmmuMatch found[2][4];
  unsigned count[2] = {0, 0};
  unsigned more[2] = {0, 0};   // matches beyond the four recorded
};
template <class Read>
SmmuStreams surveySmmu(const uint16_t (&streams)[2], uint64_t windowBytes, Read rd)
{
  SmmuStreams r;
  r.idr0 = rd(0x20);
  r.idr1 = rd(0x24);
  r.groups = r.idr0 & 0xffu;
  if (r.groups > 128) r.groups = 128;   // Qualcomm: groups above 128 do not behave architecturally (PianoDwc3 SmmuStream.h)
  r.banks = r.idr1 & 0xffu;
  r.pages = 1u << (((r.idr1 >> 28) & 7u) + 1);
  if (!r.groups || !r.banks || r.banks > 128 || r.pages > 128 || uint64_t(r.pages + r.banks) * 4096 > windowBytes) {
    r.error = "unsupported SMMU geometry";
    return r;
  }
  for (unsigned i = 0; i < r.groups; ++i) {
    const uint32_t smr = rd(0x800 + 4 * i);
    if (!(smr & 0x80000000u)) continue;
    const uint32_t mask = (smr >> 16) & 0x7fffu;
    for (unsigned k = 0; k < 2; ++k) {
      if (((smr ^ streams[k]) & 0x7fffu & ~mask) != 0) continue;
      if (r.count[k] >= 4) {
        ++r.more[k];
        continue;
      }
      SmmuMatch &m = r.found[k][r.count[k]++];
      m.group = i;
      m.smr = smr;
      m.s2cr = rd(0xc00 + 4 * i);
    }
  }
  return r;
}
inline const char *s2crType(uint32_t s2cr)
{
  const unsigned t = (s2cr >> 16) & 3u;
  return t == 0 ? "translate" : t == 1 ? "bypass" : t == 2 ? "fault" : "reserved";
}

}  // namespace pianoaudio
