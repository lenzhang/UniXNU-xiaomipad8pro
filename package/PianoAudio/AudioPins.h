// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio levels 4-6: the pin values the kext writes (and gives back), and the LPASS registers level 4 reads. Portable, no access
// here (tests/audio_host.cpp checks them); the kext (AudioBoard.cpp) stores what these return.
//   TLMM CTL (Linux pinctrl-sm8750.c as a specification: mux 5:2, pull 1:0, drive 8:6, output enable 9, eGPIO present 11 / enable 12;
//   PianoKeyboard's and PianoBacklight's run on piano), IO (bit 0 the pad, bit 1 the output value).
//   the TDM pins (level 6): gpio121-124 to their first function (i2s1_sck, i2s1_data0, i2s1_ws, i2s1_data1 in the SoC's pin table),
//     8 mA, no bias: the DT's sec-tdm-default-state, which Ubuntu's sound card applies; given back as found afterwards.
//   the reset lines (level 5): GPIO, output enabled, the value written first (no glitch); an eGPIO-capable pin (gpio107/108, the
//     right-hand pair) is claimed for this TLMM as Linux claims it when it muxes the pin (the eGPIO enable bit set).
//   the bus (level 5): bit-banged on gpio64/65 as PianoBacklight drives its hub bus: GPIO with the pull-up, a low is the output
//     enabled with the value 0, a high is released to the pull-up; given back as found after each burst.
// Why bit-banged and not the hub's GENI engine: the engine needs its GCC clocks (the hub's "se", "core" and the wrapper's "s-ahb")
// and two interconnect votes ("qup-core", "qup-config": RPMh bandwidth votes) that no native driver sets up; PianoAudio touches
// neither GCC nor RPMh. Software I2C needs only the two TLMM pins, and its ~80 kHz moves the 4348-byte preset in well under 2 s.
#pragma once
#include <stdint.h>
#include "AudioHw.h"

namespace pianoaudio {

constexpr uint32_t kCtlPull = 3u, kCtlFunction = 0xfu << 2, kCtlDrive = 7u << 6, kCtlOe = 1u << 9;
constexpr uint32_t kCtlEgpioPresent = 1u << 11, kCtlEgpioEnable = 1u << 12;
constexpr uint32_t kIoOut = 1u << 1;
constexpr unsigned kFunctionI2s1 = 1;   // gpio121-124's first function
constexpr unsigned kFunctionHub0 = 1;   // gpio64/65's first function (i2chub0_se0)
constexpr unsigned kTdmDriveCode = 3;   // (8 mA / 2) - 1

// A pin routed to the other (LPI) TLMM: eGPIO present, enable clear. Not driven for the bus or the TDM.
inline bool foreign(uint32_t ctl) { return (ctl & kCtlEgpioPresent) && !(ctl & kCtlEgpioEnable); }

inline uint32_t tdmCtl(uint32_t found)
{
  return (found & ~(kCtlPull | kCtlFunction | kCtlDrive | kCtlOe)) | kFunctionI2s1 << 2 | kTdmDriveCode << 6;
}

inline uint32_t resetCtl(uint32_t found)
{
  uint32_t v = (found & ~(kCtlFunction | kCtlOe)) | kCtlOe;
  if (found & kCtlEgpioPresent) v |= kCtlEgpioEnable;
  return v;
}
inline uint32_t resetIo(bool release) { return release ? kIoOut : 0; }

constexpr uint32_t kCtlPullUp = 3u;
inline uint32_t i2cCtl(uint32_t found, bool low) { return (found & ~(kCtlFunction | kCtlOe | kCtlPull)) | kCtlPullUp | (low ? kCtlOe : 0); }

// ---- the LPASS registers level 4 loads while the macro and digital codec votes and the VA mclk hold (sheng: register access needs
// the two votes). The VA macro's identity (Linux lpass-va-macro.c as a specification: CORE_ID_0 the major revision in 7:0, CORE_ID_1
// bit 3 "has a VA macro", CORE_ID_2 the minor revision 7:4 and step 3:0), and the configuration and level of the LPI pins the DMICs
// use (the LPI TLMM: pin n at n * 0x1000, CFG +0 with the main TLMM's field layout, VALUE +4; DT pins 6/7, 8/9, 12/13, 17/18).
struct LpassRead {
  const char *name;
  uint64_t pa;
};
constexpr uint64_t kVaMacro = 0x07660000, kLpiTlmm = 0x07760000;
constexpr LpassRead kVaIds[] = {
  {"VA CORE_ID_0", kVaMacro + 0xc0}, {"VA CORE_ID_1", kVaMacro + 0xc4}, {"VA CORE_ID_2", kVaMacro + 0xc8}, {"VA CORE_ID_3", kVaMacro + 0xcc},
};
constexpr unsigned kDmicPins[] = {6, 7, 8, 9, 12, 13, 17, 18};
constexpr unsigned kDmicPinCount = sizeof kDmicPins / sizeof kDmicPins[0];
inline uint64_t lpiPin(unsigned pin) { return kLpiTlmm + uint64_t(pin) * 0x1000; }

struct VaIdentity {
  unsigned major, minor, step;
  bool hasVa;
};
inline VaIdentity vaIdentity(uint32_t id0, uint32_t id1, uint32_t id2)
{
  return {id0 & 0xffu, (id2 >> 4) & 0xfu, id2 & 0xfu, ((id1 >> 3) & 1u) != 0};
}

}  // namespace pianoaudio
