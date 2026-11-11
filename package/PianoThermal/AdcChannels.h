// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal B6: piano's eleven board thermistors on the PMIC ADC (docs/drivers/thermal.md section 12): which PMIC, which ADC
// channel, its conversion settings and its role. Portable: the kext and tests/adc_host.cpp include it; `adc_host --map` prints it
// for tooling/k27/check-adc-map.py, which compares it with the piano DT (the build does, when the DT and dtc are there).
//
// Source: the piano boot DT (out/inputs/piano-boot.dtb, Ubuntu's running DT), verified 2026-11-11:
//   spmi@c400000 "qcom,spmi-pmic-arb", qcom,ee = 0; pmic@0 (PMK8550, SID 0) / adc@9000 "qcom,spmi-adc5-gen3", reg <0x9000 0x9100>
//   (two SDAMs: the ADC is a PBS sequence driven through SDAM memory), interrupts on peripherals 0x90 and 0x91;
//   eleven channel@N: reg = bus << 13 | SID << 8 | ADC channel, a label, qcom,ratiometric, qcom,pre-scaling <1 1>,
//   qcom,decimation, qcom,hw-settle-time (us), and qcom,adc5-gen4 on the four of PMIH0108 (SID 7);
//   their consumers thermal-sensor-* ("generic-adc-thermal") and those sensors' thermal zones (no trips: monitoring only).
// The ADC channel numbers: 0x44..0x49 AMUX1..6_THM_100K_PU, 0x4a..0x4d AMUX1..4_GPIO_100K_PU, a 100 kOhm pull-up inside the PMIC
// and the thermistor to ground on an AMUX or a GPIO pin (Linux include/linux/iio/adc/qcom-adc5-gen3-common.h, read as the
// specification only). PM8550 GPIOs 2-4 and PMIH0108 GPIO 6 carry thermistors and must be high-impedance (the ADC node's
// pinctrl "therm-default-state": bias-high-impedance); PmicAdc.h reads, never writes, their state.
// Encodings (the same file and drivers/iio/adc/qcom-spmi-adc5-gen3.c, specification only): decimation index into {85, 340, 1360};
// hardware settle index into {15, 100, 200, 300, 400, 500, 600, 700, 1000, 2000, 4000, 8000, 16000, 32000, 64000, 128000} us;
// fast average 1 sample (index 0); calibration 1 = ratiometric. Gen3's ratiometric result is code = 2^14 * R / (R + Rpu); Gen4's is
// code = 0xffff * R / (R + Rpu) (Ubuntu's Gen4 support, K:5812ec75bb03, its only source: needs-boot; the first round's raw codes
// on the console check it, a 25 C thermistor reading about 0x8000).
#ifndef PIANO_THERMAL_ADC_CHANNELS_H
#define PIANO_THERMAL_ADC_CHANNELS_H
#include <stdint.h>

namespace pianothermal::adc {

enum class Gen : uint8_t { gen3, gen4 };
// What a channel is for. The first four are the charging-safety ones (package/include/PianoBoardTemps.h).
enum class Role : uint8_t { battery, charger, connector, connector2, board };

constexpr uint32_t kPullUpOhms = 100000;
constexpr uint32_t kFullScaleGen3 = 1u << 14, kFullScaleGen4 = 0xffff;
constexpr uint16_t kInvalidCodeGen3 = 0x8000;   // Gen3's "no data" marker (a valid mid-scale ratio on Gen4)

struct ChannelDesc {
  const char *name;        // short name: the console and the IORegistry
  const char *label;       // DT label
  const char *zone;        // DT thermal zone (minus "-thermal")
  uint16_t reg;            // DT reg: bus << 13 | SID << 8 | ADC channel
  Gen gen;
  uint16_t decimation;     // DT qcom,decimation
  uint16_t settleUs;       // DT qcom,hw-settle-time
  Role role;
  constexpr uint8_t bus() const { return uint8_t((reg >> 13) & 3); }
  constexpr uint8_t sid() const { return uint8_t((reg >> 8) & 0x1f); }
  constexpr uint8_t channel() const { return uint8_t(reg & 0xff); }
  constexpr uint32_t fullScale() const { return gen == Gen::gen4 ? kFullScaleGen4 : kFullScaleGen3; }
};

// The board line's order: the charging-safety channels first, then the skin, then the rest. Every channel: ratiometric, 1:1.
constexpr unsigned kChannelCount = 11;
constexpr ChannelDesc kChannels[kChannelCount] = {
    {"batt", "pmih010x_batt_therm", "battery", 0x744, Gen::gen4, 340, 15, Role::battery},
    {"charger", "pmih010x_charger1_therm", "charger1", 0x74d, Gen::gen4, 340, 15, Role::charger},
    {"conn", "pmih010x_conn_therm", "conn", 0x747, Gen::gen4, 340, 15, Role::connector},
    {"conn2", "pm8550_conn_therm2", "conn2", 0x14b, Gen::gen3, 1360, 200, Role::connector2},
    {"msm", "pm8550_cpu_therm", "skin-msm", 0x144, Gen::gen3, 1360, 200, Role::board},
    {"quiet", "pmih010x_quiet_therm", "skin-quiet", 0x74a, Gen::gen4, 340, 15, Role::board},
    {"xo", "pmk8550_xo_therm", "xo", 0x044, Gen::gen3, 1360, 200, Role::board},
    {"wifi", "pm8550_wifi_therm", "wifi", 0x146, Gen::gen3, 1360, 200, Role::board},
    {"display", "pm8550_display_therm", "display", 0x149, Gen::gen3, 1360, 200, Role::board},
    {"backlight", "pm8550_backlight_therm", "backlight", 0x14a, Gen::gen3, 1360, 200, Role::board},
    {"flash", "pm8550_flash_therm", "flash", 0x145, Gen::gen3, 1360, 200, Role::board},
};

constexpr uint16_t kDecimations[3] = {85, 340, 1360};
constexpr uint32_t kSettleUs[16] = {15, 100, 200, 300, 400, 500, 600, 700, 1000, 2000, 4000, 8000, 16000, 32000, 64000, 128000};

constexpr int decimationIndex(uint16_t d)
{
  for (int i = 0; i < 3; ++i)
    if (kDecimations[i] == d) return i;
  return -1;
}
constexpr int settleIndex(uint32_t us)
{
  for (int i = 0; i < 16; ++i)
    if (kSettleUs[i] == us) return i;
  return -1;
}
// The channel the board line, the IORegistry and the charging API call `name`, or -1.
constexpr int channelIndex(const char *name)
{
  for (unsigned i = 0; i < kChannelCount; ++i) {
    const char *a = kChannels[i].name, *b = name;
    while (*a && *a == *b) ++a, ++b;
    if (!*a && !*b) return int(i);
  }
  return -1;
}
constexpr int roleIndex(Role r)
{
  for (unsigned i = 0; i < kChannelCount; ++i)
    if (kChannels[i].role == r) return int(i);
  return -1;
}

constexpr bool mapValid()
{
  for (const ChannelDesc &c : kChannels) {
    if (decimationIndex(c.decimation) < 0 || settleIndex(c.settleUs) < 0 || c.channel() < 0x44 || c.channel() > 0x4d) return false;
    if ((c.sid() == 7) != (c.gen == Gen::gen4)) return false;   // PMIH0108's channels are its Gen4 ADC's
  }
  return roleIndex(Role::battery) >= 0 && roleIndex(Role::charger) >= 0 && roleIndex(Role::connector) >= 0 &&
         roleIndex(Role::connector2) >= 0;
}
static_assert(mapValid(), "a channel's settings are not encodable");

// The thermistor pins that must be high-impedance (PMIC GPIO n at peripheral 0x88 + n - 1; EN_CTL 0x46 bit 7 clear = high-Z).
struct ThermPin {
  const char *pmic;
  uint8_t sid, gpio;
};
constexpr unsigned kThermPinCount = 4;
constexpr ThermPin kThermPins[kThermPinCount] = {{"pm8550", 1, 2}, {"pm8550", 1, 3}, {"pm8550", 1, 4}, {"pmih0108", 7, 6}};

}  // namespace pianothermal::adc
#endif
