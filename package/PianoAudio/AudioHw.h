// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano (Xiaomi Pad 8 Pro, SM8750) facts for the audio path, as PianoAudio's level 1 survey reads them, and the blocks it does not
// read (yet). Sources (facts only, no code, comment or table copied from any of them):
//   DT   = out/inputs/piano-boot.dtb, the running Ubuntu DT (dtc -I dtb -O dts): geniqup@9c0000 / i2c@980000 and its four
//          audio-codec@34..37 (foursemi,fs19xx), pinctrl@f100000 (hub-i2c0-data-clk-state, sec-tdm-default-state,
//          speaker-amp-irq-*-state), the sound node (xiaomi,piano-sndcard), remoteproc@6800000 glink-edge / gpr (q6apm, q6prm),
//          the LPASS macros codec@6aa0000 / 6ac0000 / 6ae0000 / 6b00000 / 7660000, soundwire@6ab0000 / 6ad0000 / 6b10000 / 7630000,
//          pinctrl@7760000 (LPASS LPI), iommu@15000000
//   UBU  = xiaomipad-8pro-mainline device/audio (README.md, Xiaomi-Pad-8-Pro.m4, card.json; BSD-3-Clause / MIT files) and
//          tools/speaker-playback-check.py (MIT): the TDM frame, the slot order, the amplifiers' DEVID and STATUS register
//   TLMM = the TLMM layout of package/PianoWifi/WifiHw.h (same SoC block, used by PianoPower, PianoDwc3 and PianoWifi)
// docs/drivers/audio.md section 2 has the same facts with their state (verified / inferred / needs-boot).
#pragma once
#include <stdint.h>

namespace pianoaudio {

// ---- TLMM (DT pinctrl@f100000, 0x102000 bytes). Pin n: configuration at n * 0x1000 (pull 1:0 (0 none, 1 down, 2 keeper, 3 up),
// function 5:2 (0 = GPIO), drive 8:6 ((v + 1) * 2 mA), output enable 9), input (bit 0) and output (bit 1) at +4 ----
constexpr uint64_t kTlmm = 0x0f100000;
constexpr uint32_t kTlmmStride = 0x1000;

// The audio pins of the main TLMM (the DMIC and SoundWire pins are LPASS LPI pins, below, not read at level 1).
//   bus:   hub-i2c0-data-clk-state: gpio64, gpio65, function "i2chub0_se0", 2 mA, pull-up (the I2C hub's SE0 = i2c@980000, 400 kHz;
//          the four amplifiers are its only devices). Which of the two is SDA is the QUP lane order (first SDA): inferred.
//   reset: the amplifiers' reset-gpios, active low (flag 1): 0x34 gpio128, 0x35 gpio129, 0x36 gpio107, 0x37 gpio108. No pin state
//          in the DT (the driver drives them): low = the amplifier held in shutdown.
//   irq:   interrupts-extended <tlmm N 8> (level low): 0x34 gpio66, 0x35 gpio84, 0x36 gpio81, 0x37 gpio67; speaker-amp-irq-*-state:
//          function "gpio", 2 mA, no bias.
//   tdm:   sec-tdm-default-state (the sound node's pinctrl-0): gpio121 "i2s1_sck", gpio122 "i2s1_data0", gpio123 "i2s1_ws",
//          gpio124 "i2s1_data1", 8 mA, no bias: the LPAIF secondary interface (TDM) on main TLMM pins.
enum class Role : uint8_t { bus, reset, irq, tdm };
struct Pin { const char *name; unsigned gpio; Role role; };
constexpr Pin kPins[] = {
  {"hub0 SDA", 64, Role::bus},       {"hub0 SCL", 65, Role::bus},
  {"amp 0x35 TL reset", 129, Role::reset}, {"amp 0x37 TR reset", 108, Role::reset},
  {"amp 0x34 BL reset", 128, Role::reset}, {"amp 0x36 BR reset", 107, Role::reset},
  {"amp 0x35 TL irq", 84, Role::irq},  {"amp 0x37 TR irq", 67, Role::irq},
  {"amp 0x34 BL irq", 66, Role::irq},  {"amp 0x36 BR irq", 81, Role::irq},
  {"TDM sck", 121, Role::tdm},       {"TDM data0", 122, Role::tdm}, {"TDM ws", 123, Role::tdm}, {"TDM data1", 124, Role::tdm},
};
constexpr unsigned kPinCount = sizeof kPins / sizeof kPins[0];
enum : unsigned { kPinSda = 0, kPinScl = 1, kPinResetFirst = 2, kPinIrqFirst = 6, kPinTdmFirst = 10 };

// ---- the four FourSemi FS19xx smart amplifiers (DT audio-codec@34..37 on i2c@980000; sound node speaker-dai-link codec order) ----
// In TDM slot order: the speaker-dai-link lists TL (0x35), TR (0x37), BL (0x34), BR (0x36); UBU: slot 0 top left, 1 top right,
// 2 bottom left, 3 bottom right in landscape with the front camera on top (the bottom pair "to be confirmed by ear" there).
// Each takes its slot from the vendor preset foursemi/piano/fs19xx.fsm (odm_a, read at run time by a later level, never committed).
struct Amp { const char *position; uint8_t address; uint8_t slot; uint8_t resetPin; uint8_t irqPin; };   // pins: kPins indexes
constexpr Amp kAmps[] = {
  {"Top Left", 0x35, 0, kPinResetFirst + 0, kPinIrqFirst + 0},    {"Top Right", 0x37, 1, kPinResetFirst + 1, kPinIrqFirst + 1},
  {"Bottom Left", 0x34, 2, kPinResetFirst + 2, kPinIrqFirst + 2}, {"Bottom Right", 0x36, 3, kPinResetFirst + 3, kPinIrqFirst + 3},
};
constexpr unsigned kAmpCount = sizeof kAmps / sizeof kAmps[0];
// UBU (for level 5, not read at level 1): register 0x03 DEVID reads 0x2900 on all four; register 0x00 STATUS bits 0 BOVDS, 1 PLLS,
// 2 OTPDS, 3 OVDS, 4 UVDS, 5 OCDS, 6 CLKS, 8 OTWDS, 9 BOPS, 10 BOLDS, 11 INDS, 14 VDDPS (polarity not documented); 16-bit registers.
constexpr uint8_t kAmpRegStatus = 0x00, kAmpRegDevId = 0x03;
constexpr uint16_t kAmpDevId = 0x2900;

// ---- the TDM frame the amplifiers expect (UBU, the stock ACDB's PARAM_ID_TDM_INTF_CFG for LPAIF interface 1) ----
constexpr unsigned kTdmRate = 48000, kTdmSlots = 4, kTdmSlotBits = 32, kTdmBitClock = kTdmRate * kTdmSlots * kTdmSlotBits;   // 6.144 MHz

// ---- apps SMMU (DT iommu@15000000, qcom,sm8750-smmu-500). The q6apm "dais" node (the DSP's DMA into the PCM buffers the
// application processor shares): iommus <&apps_smmu 0x1001 0x80>, <&apps_smmu 0x1041 0x20> ----
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000;
constexpr uint16_t kStreamApm = 0x1001, kStreamApm2 = 0x1041;

// ---- not read at level 1: the LPASS blocks. Their clocks and power are the DSP's (q6prm clocks: macro and dcodec votes 0x66 and
// 0x67, the macros' mclk 0x39 / 0x40 / 0x42 / 0x44, the TDM bit clock 0x25 of dai@28 = SECONDARY_TDM_RX_0) and no register the
// application processor can read tells whether they run, so a load could reach an unclocked block (a stalled bus). Listed for the
// log's "not read" line and the design page; status = the DT's (okay / disabled). ----
struct Block { const char *name; uint64_t base; uint32_t bytes; bool enabled; };
constexpr Block kLpassBlocks[] = {
  {"WSA2 macro", 0x06aa0000, 0x1000, false}, {"WSA2 SoundWire", 0x06ab0000, 0x10000, false},
  {"RX macro", 0x06ac0000, 0x1000, false},   {"RX SoundWire", 0x06ad0000, 0x10000, false},
  {"TX macro", 0x06ae0000, 0x1000, true},    {"WSA macro", 0x06b00000, 0x1000, false},
  {"WSA SoundWire", 0x06b10000, 0x10000, false}, {"TX SoundWire", 0x07630000, 0x10000, false},
  {"VA macro", 0x07660000, 0x2000, true},    {"LPI TLMM", 0x07760000, 0x20000, true},
};
constexpr unsigned kLpassBlockCount = sizeof kLpassBlocks / sizeof kLpassBlocks[0];

// ---- the DSP side (DT remoteproc@6800000 glink-edge "lpass", qcom,remote-pid 2; PianoPower brings the DSP up) ----
// gpr: GLINK channel "adsp_apps", qcom,domain 2 (the ADSP), qcom,intents 0x200 x 0x14 (20 receive buffers of 512 bytes); services
// 1 q6apm (with q6apm-lpass-dais / q6apm-dais), 2 q6prm (q6prm-lpass-clocks); protection domain "avs/audio" in "msm/adsp/audio_pd".
constexpr const char *kGprChannel = "adsp_apps";
constexpr uint32_t kGprDomainAdsp = 2, kGprServiceApm = 1, kGprServicePrm = 2;

}  // namespace pianoaudio
