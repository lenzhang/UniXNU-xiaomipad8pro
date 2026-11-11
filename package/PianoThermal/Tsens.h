// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: piano's four TSENS v2 controllers, their sensor map with the stock trip points, the register decode and one poll of
// every sensor. Portable (no IOKit): the kext and tests/thermal_host.cpp include it. Nothing here writes a register.
//
// Sources (docs/drivers/thermal.md section 2):
//   - the piano boot DT (out/inputs/piano-boot.dtb): thermal-sensor@c228000/c229000/c22a000/c22b000, compatible "qcom,sm8750-tsens",
//     "qcom,tsens-v2", reg <TM 0x1000> <SROT 0x1000>, #qcom,sensors 15/7/16/9; the thermal-zones' thermal-sensors = <&tsensN s>
//     and their trips (temperature/hysteresis in m°C, type passive/hot/critical): the table below, verified 2026-11-11;
//   - the register layout: Linux drivers/thermal/qcom/tsens-v2.c read as the specification (GPL: no code taken): TM_Sn_STATUS
//     0xa0 + 4n with LAST_TEMP [11:0] signed deci-degrees C and VALID bit 21 (MIN/LOWER/UPPER/CRITICAL/MAX status bits 16..20),
//     TM_Sn_UPPER_LOWER_THRESHOLD 0x20 + 4n (lower [11:0], upper [23:12]), TM_Sn_CRITICAL_THRESHOLD 0x60 + 4n [11:0], TM_TRDY
//     0xe4 bit 0; SROT HW_VER 0x0 (major [31:28], minor [27:16], step [15:0]) and CTRL 0x4 (TSENS_EN bit 0, SENSOR_EN [18:3],
//     result format "temperature" bit 21). The status word layout is hardware-verified on this SoC by Adreno830's Sm8750Tsens (13T)
//     and by the REDMI K90 XNU port (annibale_cpufreq.c): see UPSTREAM.
#ifndef PIANO_THERMAL_TSENS_H
#define PIANO_THERMAL_TSENS_H
#include <stddef.h>
#include <stdint.h>

namespace pianothermal {

// ------------------------------------------------------------------------------------------------------------- registers
constexpr uint32_t kTmIntEn = 0x04;          // raw only (the enable bits' meaning is not needed read-only)
constexpr uint32_t kTmUpLowIntStatus = 0x08;
constexpr uint32_t kTmCriticalIntStatus = 0x14;
constexpr uint32_t kTmUpLowThreshold = 0x20;  // + 4n
constexpr uint32_t kTmCriticalThreshold = 0x60;
constexpr uint32_t kTmStatus = 0xa0;
constexpr uint32_t kTmTrdy = 0xe4;
constexpr uint32_t kTmBytes = 0x1000;          // DT reg length of the TM range (one page)
constexpr uint32_t kSrotVersion = 0x00, kSrotCtrl = 0x04, kSrotBytes = 0x1000;

constexpr uint32_t kStatusValid = 1u << 21;
constexpr uint32_t kStatusMin = 1u << 16, kStatusLower = 1u << 17, kStatusUpper = 1u << 18, kStatusCritical = 1u << 19,
                   kStatusMax = 1u << 20;
constexpr uint32_t kCtrlEnable = 1u << 0, kCtrlSensorShift = 3, kCtrlSensorBits = 0xffff, kCtrlResultTemp = 1u << 21;

constexpr unsigned kControllers = 4, kMaxSensors = 16;
constexpr unsigned kReadTries = 3;              // reads of one sensor while VALID is clear (VALID drops for a few AHB cycles)
constexpr unsigned kStalePolls = 3;             // a sensor without VALID stands by its last reading for this many polls
constexpr int32_t kPlausibleMinDeci = -400;     // -40.0 C .. 150.0 C: anything else is counted and ignored (the hardware
constexpr int32_t kPlausibleMaxDeci = 1500;     // resets the SoC long before 150 C; 12 bits reach 204.7 C)

inline int32_t signExtend12(uint32_t v)
{
  v &= 0xfff;
  return (v & 0x800) ? int32_t(v) - 0x1000 : int32_t(v);
}

// One TM_Sn_STATUS word: the reading in deci-degrees C (sign-extended even when not VALID) and whether VALID is set.
inline bool decodeStatus(uint32_t status, int32_t &deci)
{
  deci = signExtend12(status);
  return (status & kStatusValid) != 0;
}

struct Thresholds {
  int32_t lowerDeci, upperDeci, criticalDeci;
};
inline Thresholds decodeThresholds(uint32_t upperLower, uint32_t critical)
{
  return {signExtend12(upperLower), signExtend12(upperLower >> 12), signExtend12(critical)};
}

struct Version {
  uint32_t major, minor, step;
};
inline Version decodeVersion(uint32_t v) { return {v >> 28, (v >> 16) & 0xfff, v & 0xffff}; }

struct Ctrl {
  bool enabled;
  uint32_t sensorMask;   // bit n: sensor n enabled
  bool temperatureFormat;
};
inline Ctrl decodeCtrl(uint32_t v)
{
  return {(v & kCtrlEnable) != 0, (v >> kCtrlSensorShift) & kCtrlSensorBits, (v & kCtrlResultTemp) != 0};
}

// The threshold interrupts (TSV2): TM_UPPER_LOWER_INT_STATUS/CLEAR/MASK 0x08/0x0c/0x10 with sensor n's lower interrupt at bit n and
// its upper one at bit 16 + n; TM_CRITICAL_INT_STATUS/CLEAR/MASK 0x14/0x18/0x1c (bit n; 30 cycle-counter monitor, 31 watchdog
// bark); a mask bit set = masked; an interrupt is cleared by writing its clear bit 1 then 0; TM_INT_EN [2:0] = 7 enables them all
// on v2 (Linux tsens_enable_irq). Used by the suspend hooks (below) and the uplow interrupt (TsensIrq.h, piano-thermal-irq=1;
// docs/drivers/thermal.md section 13); levels 1-2 alone never write them.
constexpr uint32_t kTmUpLowIntClear = 0x0c, kTmUpLowIntMask = 0x10, kTmCriticalIntClear = 0x18, kTmCriticalIntMask = 0x1c;
constexpr uint32_t kTmIntEnAll = 7;

inline uint32_t lowerIntBits(unsigned sensors) { return sensors >= 16 ? 0xffffu : (1u << sensors) - 1u; }
inline uint32_t upperIntBits(unsigned sensors) { return lowerIntBits(sensors) << 16; }
inline uint32_t encodeThreshold(int32_t deci) { return uint32_t(deci) & 0xfffu; }
inline uint32_t encodeUpperLower(int32_t lowerDeci, int32_t upperDeci)
{
  return encodeThreshold(lowerDeci) | encodeThreshold(upperDeci) << 12;
}

// System suspend (Linux 08e750858038 on piano: a lower-threshold interrupt crossed while the device cools in suspend cannot be
// serviced, refires, and being wake-armed aborts or ends every sleep): the controller's up/low mask with every sensor's lower
// interrupt masked and, while nothing services the uplow interrupt natively (today), every upper one too; the critical interrupts
// are not touched (the hardware's own protection). clear: the bits to pulse 1 then 0 in TM_UPPER_LOWER_INT_CLEAR (any latched
// status of a masked interrupt goes with it).
struct SuspendMask {
  uint32_t mask, clear;
};
inline SuspendMask suspendMask(uint32_t current, unsigned sensors, bool maskUpper)
{
  const uint32_t bits = lowerIntBits(sensors) | (maskUpper ? upperIntBits(sensors) : 0u);
  return {current | bits, bits};
}

inline unsigned popcount(uint32_t v)
{
  unsigned n = 0;
  for (; v; v &= v - 1) ++n;
  return n;
}

// Decimal text of a deci-degree value ("-0.5", "48.2"): fixed format for the log, no floating point in the kernel.
inline void deciText(int32_t deci, char *out, size_t size)
{
  if (!size) return;
  const bool neg = deci < 0;
  const uint32_t a = uint32_t(neg ? -deci : deci);
  char tmp[16];
  unsigned n = 0;
  tmp[n++] = char('0' + a % 10);
  tmp[n++] = '.';
  uint32_t whole = a / 10;
  do {
    tmp[n++] = char('0' + whole % 10);
    whole /= 10;
  } while (whole && n < sizeof tmp - 1);
  if (neg) tmp[n++] = '-';
  size_t o = 0;
  while (n && o + 1 < size) out[o++] = tmp[--n];
  out[o] = 0;
}

// ------------------------------------------------------------------------------------------------------------- the map
// Stock trips of each sensor's thermal zone (deci-degrees C; 0 = the zone has no such trip). hot/critical are 120/125 C on every
// zone that has them; gpuss* have no hot trip; cpu-0-0-1, -1-0, -1-1, -3-0, -3-1, -5-0, -5-1 pass at 118 C (hysteresis 8 C), the
// other cpu-* and gpuss* at 105 C (5 C); ddr's hot hysteresis is 2 C.
struct Sensor {
  const char *name;
  int16_t passiveDeci, passiveHystDeci, hotDeci, criticalDeci;
};

enum class Group : uint8_t { cpu0, cpu1, gpu, nsp };

struct Controller {
  const char *name;      // the log's name of the controller's hottest sensor
  uint64_t tm, srot;     // physical addresses (DT reg)
  uint8_t count;         // #qcom,sensors
  uint8_t cpuFirst, cpuLast;   // the zones with a CPU cooling map (cpufreq domain = controller index 0/1); 0/0 for none
  const Sensor *sensors;
};

#define PT_AOSS(n) {"aoss" #n, 0, 0, 1200, 1250}
#define PT_CPU105(n) {n, 1050, 50, 1200, 1250}
#define PT_CPU118(n) {n, 1180, 80, 1200, 1250}
#define PT_HOT(n) {n, 0, 0, 1200, 1250}
#define PT_GPU(n) {"gpuss" #n, 1050, 50, 0, 1250}

inline constexpr Sensor kTsens0[15] = {
    PT_AOSS(0),
    PT_CPU105("cpu-0-0-0"), PT_CPU118("cpu-0-0-1"), PT_CPU118("cpu-0-1-0"), PT_CPU118("cpu-0-1-1"),
    PT_CPU105("cpu-0-2-0"), PT_CPU105("cpu-0-2-1"), PT_CPU118("cpu-0-3-0"), PT_CPU118("cpu-0-3-1"),
    PT_CPU105("cpu-0-4-0"), PT_CPU105("cpu-0-4-1"), PT_CPU118("cpu-0-5-0"), PT_CPU118("cpu-0-5-1"),
    PT_HOT("cpuss-0-0"), PT_HOT("cpuss-0-1"),
};
inline constexpr Sensor kTsens1[7] = {
    PT_AOSS(1),
    PT_CPU105("cpu-1-0-0"), PT_CPU105("cpu-1-0-1"), PT_CPU105("cpu-1-1-0"), PT_CPU105("cpu-1-1-1"),
    PT_HOT("cpuss-1-0"), PT_HOT("cpuss-1-1"),
};
inline constexpr Sensor kTsens2[16] = {
    PT_AOSS(2),
    PT_GPU(0), PT_GPU(1), PT_GPU(2), PT_GPU(3), PT_GPU(4), PT_GPU(5), PT_GPU(6), PT_GPU(7),
    PT_HOT("modem0"), PT_HOT("modem1"), PT_HOT("modem2"), PT_HOT("modem3"),
    PT_HOT("camera0"), PT_HOT("camera1"), PT_HOT("video"),
};
inline constexpr Sensor kTsens3[9] = {
    PT_AOSS(3),
    PT_HOT("nsphvx0"), PT_HOT("nsphvx1"), PT_HOT("nsphvx2"),
    PT_HOT("nsphmx0"), PT_HOT("nsphmx1"), PT_HOT("nsphmx2"), PT_HOT("nsphmx3"),
    {"ddr", 0, 0, 1200, 1250},
};
#undef PT_AOSS
#undef PT_CPU105
#undef PT_CPU118
#undef PT_HOT
#undef PT_GPU

inline constexpr Controller kMap[kControllers] = {
    {"cpu0", 0x0c228000, 0x0c222000, 15, 1, 12, kTsens0},   // cluster 0 (cpu0-5): cpu-0-* (1..12, cooling cpu0), cpuss-0-*
    {"cpu1", 0x0c229000, 0x0c223000, 7, 1, 4, kTsens1},     // cluster 1 (cpu6-7): cpu-1-* (1..4, cooling cpu6), cpuss-1-*
    {"gpu", 0x0c22a000, 0x0c224000, 16, 0, 0, kTsens2},     // gpuss0-7 (1..8), modem, camera, video
    {"nsp", 0x0c22b000, 0x0c225000, 9, 0, 0, kTsens3},      // the NPU (nsphvx/nsphmx) and ddr
};

// The policy's passive threshold of a sensor: its zone's stock passive trip; zones without one (aoss, cpuss, modem, camera, video,
// nsp, ddr: only a hot trip at 120 C, which notifies and cools nothing) take the SoC's lowest stock passive trip, 105.0 C (5 C
// hysteresis), so that passive always comes before the critical threshold (125 C - margin).
constexpr int32_t kDefaultPassiveDeci = 1050, kDefaultPassiveHystDeci = 50;
inline int32_t passiveOf(const Sensor &s) { return s.passiveDeci ? s.passiveDeci : kDefaultPassiveDeci; }
inline int32_t passiveHystOf(const Sensor &s) { return s.passiveDeci ? s.passiveHystDeci : kDefaultPassiveHystDeci; }

// ------------------------------------------------------------------------------------------------------------- one poll
// read(context, controller, offset): a 32-bit load at a byte offset of controller's TM range (0xffffffff when it is not mapped).
struct TsensIo {
  void *context;
  uint32_t (*read)(void *context, unsigned controller, uint32_t offset);
  bool (*mapped)(void *context, unsigned controller);
};

class Monitor {
 public:
  // Every sensor of every mapped controller: up to kReadTries loads while VALID is clear. A VALID reading outside the plausible
  // window is counted and treated as no reading; a sensor without a fresh reading keeps its last one for kStalePolls polls.
  void poll(const TsensIo &io)
  {
    ++polls_;
    for (unsigned c = 0; c < kControllers; ++c) {
      fresh_[c] = 0;
      if (!io.mapped(io.context, c)) {
        usable_[c] = 0;
        continue;
      }
      for (unsigned s = 0; s < kMap[c].count; ++s) {
        int32_t deci = 0;
        bool valid = false;
        uint32_t raw = 0;
        for (unsigned t = 0; t < kReadTries && !valid; ++t) {
          raw = io.read(io.context, c, kTmStatus + 4 * s);
          ++reads_;
          valid = decodeStatus(raw, deci);
          if (!valid) ++invalidReads_;
        }
        raw_[c][s] = raw;
        if (valid && (deci < kPlausibleMinDeci || deci > kPlausibleMaxDeci)) {
          ++implausible_;
          valid = false;
        }
        const uint16_t bit = uint16_t(1u << s);
        if (valid) {
          deci_[c][s] = int16_t(deci);
          age_[c][s] = 0;
          fresh_[c] |= bit;
          usable_[c] |= bit;
          seen_[c] |= bit;
        } else if ((usable_[c] & bit) && ++age_[c][s] <= kStalePolls) {
          ++staleUses_;
        } else {
          usable_[c] &= uint16_t(~bit);
        }
      }
    }
  }

  // The hottest usable sensor of controller c among sensors [first, last] (all when last == 0); false when none is usable.
  bool hottest(unsigned c, int32_t &deci, unsigned &sensor, unsigned first = 0, unsigned last = 0) const
  {
    if (c >= kControllers) return false;
    if (!last) last = kMap[c].count - 1;
    bool any = false;
    for (unsigned s = first; s <= last && s < kMap[c].count; ++s) {
      if (!(usable_[c] & (1u << s))) continue;
      if (!any || deci_[c][s] > deci) {
        deci = deci_[c][s];
        sensor = s;
        any = true;
      }
    }
    return any;
  }
  // The hottest usable sensor of the whole SoC.
  bool hottestAll(int32_t &deci, unsigned &controller, unsigned &sensor) const
  {
    bool any = false;
    for (unsigned c = 0; c < kControllers; ++c) {
      int32_t d;
      unsigned s;
      if (hottest(c, d, s) && (!any || d > deci)) {
        deci = d;
        controller = c;
        sensor = s;
        any = true;
      }
    }
    return any;
  }
  bool anyUsable() const
  {
    for (unsigned c = 0; c < kControllers; ++c)
      if (usable_[c]) return true;
    return false;
  }
  bool usable(unsigned c, unsigned s) const { return c < kControllers && s < kMaxSensors && (usable_[c] & (1u << s)); }
  int32_t deci(unsigned c, unsigned s) const { return deci_[c][s]; }
  uint32_t raw(unsigned c, unsigned s) const { return raw_[c][s]; }
  uint16_t freshMask(unsigned c) const { return fresh_[c]; }
  uint16_t usableMask(unsigned c) const { return usable_[c]; }
  uint16_t seenMask(unsigned c) const { return seen_[c]; }
  uint64_t polls() const { return polls_; }
  uint64_t reads() const { return reads_; }
  uint64_t invalidReads() const { return invalidReads_; }
  uint64_t implausible() const { return implausible_; }
  uint64_t staleUses() const { return staleUses_; }

 private:
  int16_t deci_[kControllers][kMaxSensors] = {};
  uint8_t age_[kControllers][kMaxSensors] = {};
  uint32_t raw_[kControllers][kMaxSensors] = {};
  uint16_t fresh_[kControllers] = {}, usable_[kControllers] = {}, seen_[kControllers] = {};
  uint64_t polls_ = 0, reads_ = 0, invalidReads_ = 0, implausible_ = 0, staleUses_ = 0;
};

}  // namespace pianothermal

#endif
