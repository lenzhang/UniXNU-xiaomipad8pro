// SPDX-License-Identifier: BSD-2-Clause-Patent
// The facts PianoTorch uses: piano's rear torch is the PM8550 flash LED module, its two factory-wired channels driven together
// (docs/drivers/torch.md section 2). Portable: no I/O, no store; host-tested by tests/torch_host.cpp.
//
// Sources, each fact read, not copied:
//   - the piano boot DT (out/inputs/piano-boot.dtb): spmi@c400000 (core 0xc400000, chnls 0xc500000 0x400000, obsrvr 0xc440000,
//     cnfg 0xc42d000, qcom,ee = 0, bus 0), pmic@1 = PM8550 (SID 1), led-controller@ee00 "qcom,pm8550-flash-led" with status
//     "disabled" (the boot DT leaves it to the OS);
//   - the Ubuntu piano kernel (piano/dev 7f8d6bedde47, GPL-2.0, specification only): sm8750-xiaomi-piano.dts (v1.1.1, K:82a40d864b46)
//     enables that node with one LED, led-sources <1 2> (the factory's qcom,id 0 and 1 = channels 1 and 2, i.e. channel indices 0
//     and 1, "the flash and the torch both drive the two channels together"), led-max-microamp 100000 (the torch, both channels),
//     flash-max-microamp 400000, flash-max-timeout-us 300000; drivers/leds/flash/leds-qcom-flash.c: the module's identity (TYPE
//     0x04 = 0x18, SUBTYPE 0x05: 0x07 four channels, 0x04/0x03 three), the four-channel register layout, the torch current step
//     (5 mA with the channel's IRESOLUTION bit set, 12.5 mA clear), ITARGET = per-channel current / step - 1, the torch clamp
//     (per-channel maximum in torch steps - 1: 9 for piano's 100 mA over two channels), the software strobe word (SW select,
//     level trigger, active high = 0x01), the safety timer off for a torch, the status bits;
//   - drivers/spmi/spmi-pmic-arb.c (same tree, specification only): v7 writes go through the owner EE's channel at chnls +
//     0x1000 * APID (command +0x00, status +0x08, write data +0x10), opcode 0 = extended register write long, and Linux refuses
//     a write to an APID whose owner (cnfg + 4 * APID, bits 2:0) is not its own EE.
// Not known from any source (needs-boot, logged by level 1): the reset values, the owner EE of the flash APID (Ubuntu's writes
// succeed, so it is inferred to be EE 0), the second STATUS1 bit of each channel, what the module does with a clamp below ITARGET.
#ifndef PIANO_TORCH_FLASH_HW_H
#define PIANO_TORCH_FLASH_HW_H
#include <stdint.h>

namespace pianotorch {

// ---------------------------------------------------------------------------------------------------- where the module is
constexpr uint32_t kSid = 1;              // PM8550 (pmic@1)
constexpr uint32_t kBase = 0xee00;        // led-controller@ee00
constexpr uint16_t kPpid = uint16_t(kSid << 8 | kBase >> 8);   // 0x1ee: the APID map's key
constexpr uint64_t kSpmiChnls = 0x0c500000;                    // the write channels, 0x1000 per APID
constexpr uint32_t kChnlStride = 0x1000, kChnlCmd = 0x00, kChnlStatus = 0x08, kChnlWdata0 = 0x10;
constexpr uint32_t kOpExtWriteL = 0;      // command = opcode << 27 | register << 4 | (bytes - 1)
constexpr uint32_t kStatusDone = 1u << 0, kStatusErrors = 0xeu;   // bits 1-3: failure, denied, dropped

// ---------------------------------------------------------------------------------------------- identity (all variants)
constexpr uint8_t kRegRevision = 0x00, kRegType = 0x04, kRegSubtype = 0x05;
constexpr uint8_t kTypeFlash = 0x18;
constexpr uint8_t kSubtype4ch = 0x07, kSubtype3chPm8150 = 0x04, kSubtype3chPmi8998 = 0x03;

// ----------------------------------------------------------------------------------- the four-channel layout (PM8550)
constexpr uint8_t kRegStatus1 = 0x06;     // per channel two bits from 2 * channel; the first is a short circuit
constexpr uint8_t kRegStatus2 = 0x07;     // module faults, bits below
constexpr uint8_t kRegStatus3 = 0x09;     // per channel: bit 2 * channel = the safety timer expired
constexpr uint8_t kRegTimer0 = 0x3e;      // + channel: bit 7 enable, bits 6:0 time in 10 ms steps
constexpr uint8_t kRegItarget0 = 0x42;    // + channel: bits 6:0, current = (ITARGET + 1) * step
constexpr uint8_t kRegModuleEn = 0x46;    // bit 7
constexpr uint8_t kRegIres = 0x49;        // bit n set: channel n in 5 mA steps (torch), clear: 12.5 mA (flash)
constexpr uint8_t kRegStrobe0 = 0x4a;     // + channel: bit 2 hardware strobe, bit 1 edge trigger, bit 0 active high
constexpr uint8_t kRegChanEn = 0x4e;      // bit n: channel n on
constexpr uint8_t kRegThermThrsh2 = 0x78, kRegThermThrsh1 = 0x7a;   // bits 2:0 each
constexpr uint8_t kRegTorchClamp = 0xed;  // bits 6:0

constexpr unsigned kChannels4ch = 4;
constexpr uint8_t kModuleEnBit = 0x80, kItargetMask = 0x7f, kStrobeMask = 0x7f, kClampMask = 0x7f, kThrshMask = 0x07;
constexpr uint8_t kStrobeHw = 0x04, kStrobeEdge = 0x02, kStrobeActiveHigh = 0x01;
constexpr uint8_t kStrobeSoftwareLevelHigh = kStrobeActiveHigh;     // software strobe, level, active high
constexpr uint8_t kTimerEnable = 0x80, kTimerValueMask = 0x7f;
constexpr unsigned kTimerStepMs = 10;

// STATUS2 of the four-channel module.
constexpr uint8_t kSts2VphLow = 0x01, kSts2BclIbat = 0x02, kSts2BobIlimS1 = 0x04, kSts2BobIlimS2 = 0x08, kSts2Otst2 = 0x10,
                  kSts2Otst1 = 0x20, kSts2BobThermOverload = 0x40;
constexpr uint8_t kSts2Thermal = kSts2Otst1 | kSts2Otst2 | kSts2BobThermOverload;
constexpr uint8_t kSts2Current = kSts2BclIbat | kSts2BobIlimS1 | kSts2BobIlimS2;
constexpr uint8_t kSts2Voltage = kSts2VphLow;

// --------------------------------------------------------------------------------------------------------- piano's wiring
constexpr uint8_t kPianoChannelMask = 0x03;          // led-sources <1 2>: channel indices 0 and 1
constexpr unsigned kPianoChannels = 2;
constexpr unsigned kTorchStepUa = 5000;              // with the IRESOLUTION bit set
constexpr unsigned kUbuntuTorchMaxMa = 100;          // led-max-microamp 100000 for the lamp (both channels)
constexpr unsigned kTorchMinMa = kPianoChannels * kTorchStepUa / 1000;   // 10: one step on each channel

// The two other channels' bits of a register that holds one bit per channel.
constexpr uint8_t kOtherChannelMask = 0x0f & uint8_t(~kPianoChannelMask);

// ITARGET for a total lamp current split over piano's two channels (Linux's rounding: the per-channel current + 1 uA, divided
// by the step, minus one; at least 0). 50 mA -> 4 (25 mA a channel), 100 mA -> 9 (50 mA).
constexpr uint8_t itargetForMa(unsigned totalMa)
{
  const unsigned perUa = totalMa * 1000 / kPianoChannels + 1;
  return perUa > kTorchStepUa ? uint8_t(perUa / kTorchStepUa - 1) : uint8_t(0);
}
// The lamp current an ITARGET gives with both channels on (torch steps).
constexpr unsigned maForItarget(uint8_t itarget) { return (unsigned(itarget & kItargetMask) + 1) * kTorchStepUa / 1000 * kPianoChannels; }
// Linux's clamp for a lamp limit: the per-channel limit in torch steps, minus one (9 for 100 mA).
constexpr uint8_t clampForMa(unsigned totalMa)
{
  const unsigned steps = totalMa * 1000 / kPianoChannels / kTorchStepUa;
  return steps ? uint8_t(steps - 1) : uint8_t(0);
}
static_assert(kPpid == 0x1ee, "PM8550 flash PPID");
static_assert(itargetForMa(50) == 4 && maForItarget(4) == 50, "50 mA = 2 x 25 mA");
static_assert(itargetForMa(100) == 9 && maForItarget(9) == 100, "Ubuntu's 100 mA = 2 x 50 mA");
static_assert(itargetForMa(10) == 0 && maForItarget(0) == 10, "one step a channel");
static_assert(clampForMa(kUbuntuTorchMaxMa) == 9, "Linux's clamp for piano");

// --------------------------------------------------------------------------------------------------------------- decoding
inline const char *subtypeName(uint8_t subtype)
{
  return subtype == kSubtype4ch ? "four-channel" : subtype == kSubtype3chPm8150 ? "three-channel (PM8150 layout)"
       : subtype == kSubtype3chPmi8998 ? "three-channel (PMI8998 layout)" : "unknown";
}

// One channel's torch/flash setting as read.
struct ChannelView {
  uint8_t itarget = 0, strobe = 0, timer = 0;
  bool torchSteps = false;   // IRESOLUTION bit: 5 mA steps
  bool on = false;           // CHAN_EN bit (the module must be enabled as well to drive it)
  unsigned ua() const { return (unsigned(itarget & kItargetMask) + 1) * (torchSteps ? 5000u : 12500u); }
};

inline const char *strobeName(uint8_t strobe)
{
  switch (strobe & (kStrobeHw | kStrobeEdge | kStrobeActiveHigh)) {
    case 0: return "software, level, active low";
    case kStrobeActiveHigh: return "software, level, active high";
    case kStrobeEdge: return "software, edge, active low";
    case kStrobeEdge | kStrobeActiveHigh: return "software, edge, active high";
    case kStrobeHw: return "hardware, level, active low";
    case kStrobeHw | kStrobeActiveHigh: return "hardware, level, active high";
    case kStrobeHw | kStrobeEdge: return "hardware, edge, active low";
    default: return "hardware, edge, active high";
  }
}

// What a set of STATUS bytes means, in words (a static string per class; the raw bytes are logged next to it).
struct Faults {
  bool shortCircuit = false, timeout = false, thermal = false, current = false, voltage = false;
  bool any() const { return shortCircuit || timeout || thermal || current || voltage; }
};
inline Faults decodeFaults(uint8_t status1, uint8_t status2, uint8_t status3, uint8_t channelMask)
{
  Faults f;
  for (unsigned ch = 0; ch < kChannels4ch; ++ch) {
    if (!(channelMask & (1u << ch))) continue;
    f.shortCircuit = f.shortCircuit || (status1 & (1u << (2 * ch)));
    f.timeout = f.timeout || (status3 & (1u << (2 * ch)));
  }
  f.thermal = status2 & kSts2Thermal;
  f.current = status2 & kSts2Current;
  f.voltage = status2 & kSts2Voltage;
  return f;
}

}  // namespace pianotorch
#endif
