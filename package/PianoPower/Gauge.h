// SPDX-License-Identifier: BSD-2-Clause-Patent
// The battery as its two fuel gauges report it. piano has two cell packs in parallel, each with a TI bq27z561 at I2C address 0x55 on a
// bus of the application processor (live DT): pack A on i2c@a94000 (qup1_se5, pins gpio52/gpio53, shared with the SC8541 charge pump at
// 0x6a), pack B on i2c@88c000 (qup2_se3, pins gpio12/gpio13). Ubuntu reads them directly (bq27xxx); they are not behind the DSP.
//
// Reading them needs no QUP serial engine (its clocks, firmware and DFS tables are not ours to set up): the two pins of each bus are
// taken as plain GPIOs and the bus is driven in software, open drain (a line is either driven low or released to the board's pull-up;
// never driven high). Only reads: each transaction writes the one-byte command pointer and reads two bytes back, which changes nothing
// in the gauge. Pack values follow Ubuntu's piano_battery (Project-SunUEFI docs/devel/linux/kernel.md): currents and charges summed,
// voltage averaged, state of charge weighted by full charge capacity.
//
// bq27z561 standard commands (TI datasheet, two bytes little endian, Smart Battery style): 0x06 Temperature (0.1 K), 0x08 Voltage (mV),
// 0x0a BatteryStatus, 0x0c Current (mA, signed, + charging), 0x10 RemainingCapacity (mAh), 0x12 FullChargeCapacity (mAh), 0x14
// AverageCurrent (mA), 0x2a CycleCount, 0x2c RelativeStateOfCharge (%), 0x3c DesignCapacity (mAh). BatteryStatus bit 6 DSG (discharging), bit 5 FC (fully
// charged), bit 4 FD (fully discharged), bit 7 INIT.
#ifndef PIANO_POWER_GAUGE_H
#define PIANO_POWER_GAUGE_H
#include <stdint.h>

namespace pianopower {

// ---------------------------------------------------------------------------------------------------------- the bus in software
// Line control: release (true) lets the pull-up take the line high, false drives it low.
struct I2cIo {
  void *ctx;
  void (*sda)(void *ctx, bool release);
  void (*scl)(void *ctx, bool release);
  bool (*readSda)(void *ctx);
  bool (*readScl)(void *ctx);
  void (*delayUs)(void *ctx, unsigned us);
};

constexpr unsigned kHalfBitUs = 5;        // about 80 kHz with the register accesses
constexpr unsigned kStretchUs = 50000;    // a gauge may hold SCL low while it prepares data
constexpr unsigned kBusFreeUs = 100;      // between transactions (the bq27 family wants >= 66 us)

enum class I2cResult : uint8_t { ok, nackAddress, nackData, busBusy, stretchTimeout };

inline const char *i2cName(I2cResult r)
{
  switch (r) {
  case I2cResult::ok: return "ok";
  case I2cResult::nackAddress: return "no ack (address)";
  case I2cResult::nackData: return "no ack (command)";
  case I2cResult::busBusy: return "bus held low";
  case I2cResult::stretchTimeout: return "clock stretch timeout";
  }
  return "?";
}

class I2cBus {
 public:
  explicit I2cBus(const I2cIo &io) : io_(io) {}

  // Frees a bus a device holds (SDA low): clocks until SDA is released, at most 9 times, then a stop.
  bool recover()
  {
    io_.sda(io_.ctx, true);
    for (unsigned i = 0; i < 9 && !io_.readSda(io_.ctx); ++i) {
      io_.scl(io_.ctx, false);
      io_.delayUs(io_.ctx, kHalfBitUs);
      if (!sclHigh()) return false;
      io_.delayUs(io_.ctx, kHalfBitUs);
    }
    stop();
    return io_.readSda(io_.ctx) && io_.readScl(io_.ctx);
  }

  // A register read: START, address+W, command, repeated START, address+R, two bytes (ACK, NACK), STOP.
  I2cResult readWord(uint8_t address, uint8_t command, uint16_t *value)
  {
    if ((!io_.readSda(io_.ctx) || !io_.readScl(io_.ctx)) && !recover()) return I2cResult::busBusy;
    const I2cResult r = transfer(address, command, value);
    stop();
    io_.delayUs(io_.ctx, kBusFreeUs);
    return r;
  }

 private:
  const I2cIo io_;

  I2cResult transfer(uint8_t address, uint8_t command, uint16_t *value)
  {
    I2cResult r = I2cResult::ok;
    uint8_t lo = 0, hi = 0;
    if (!start()) return I2cResult::busBusy;
    if (!writeByte(uint8_t(address << 1), &r)) return r != I2cResult::ok ? r : I2cResult::nackAddress;
    if (!writeByte(command, &r)) return r != I2cResult::ok ? r : I2cResult::nackData;
    if (!start()) return I2cResult::busBusy;   // repeated START
    if (!writeByte(uint8_t(address << 1 | 1), &r)) return r != I2cResult::ok ? r : I2cResult::nackAddress;
    if (!readByte(&lo, true, &r) || !readByte(&hi, false, &r)) return r;
    *value = uint16_t(lo | hi << 8);
    return I2cResult::ok;
  }

  bool sclHigh()
  {
    io_.scl(io_.ctx, true);
    for (unsigned waited = 0; !io_.readScl(io_.ctx); waited += 10) {
      if (waited >= kStretchUs) return false;
      io_.delayUs(io_.ctx, 10);
    }
    return true;
  }
  // START (or repeated START): SDA falls while SCL is high. Expects SCL low or high on entry; leaves SCL low.
  bool start()
  {
    io_.sda(io_.ctx, true);
    io_.delayUs(io_.ctx, kHalfBitUs);
    if (!sclHigh()) return false;
    io_.delayUs(io_.ctx, kHalfBitUs);
    if (!io_.readSda(io_.ctx)) return false;   // someone else holds SDA
    io_.sda(io_.ctx, false);
    io_.delayUs(io_.ctx, kHalfBitUs);
    io_.scl(io_.ctx, false);
    io_.delayUs(io_.ctx, kHalfBitUs);
    return true;
  }
  void stop()
  {
    io_.sda(io_.ctx, false);
    io_.delayUs(io_.ctx, kHalfBitUs);
    sclHigh();
    io_.delayUs(io_.ctx, kHalfBitUs);
    io_.sda(io_.ctx, true);
    io_.delayUs(io_.ctx, kHalfBitUs);
  }
  // One bit with SCL low on entry and exit; returns the SDA level sampled while SCL was high.
  bool bit(bool release, I2cResult *r)
  {
    io_.sda(io_.ctx, release);
    io_.delayUs(io_.ctx, kHalfBitUs);
    if (!sclHigh()) {
      *r = I2cResult::stretchTimeout;
      return false;
    }
    io_.delayUs(io_.ctx, kHalfBitUs);
    const bool level = io_.readSda(io_.ctx);
    io_.scl(io_.ctx, false);
    return level;
  }
  bool writeByte(uint8_t byte, I2cResult *r)
  {
    for (int i = 7; i >= 0; --i) {
      bit((byte >> i) & 1, r);
      if (*r != I2cResult::ok) return false;
    }
    const bool nack = bit(true, r);   // the device pulls SDA low to acknowledge
    return *r == I2cResult::ok && !nack;
  }
  bool readByte(uint8_t *byte, bool ack, I2cResult *r)
  {
    uint8_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v = uint8_t(v << 1 | (bit(true, r) ? 1 : 0));
      if (*r != I2cResult::ok) return false;
    }
    bit(!ack, r);
    *byte = v;
    return *r == I2cResult::ok;
  }
};

// ------------------------------------------------------------------------------------------------------------------ bq27z561
constexpr uint8_t kGaugeAddress = 0x55;
enum GaugeCommand : uint8_t {
  kGaugeTemperature = 0x06, kGaugeVoltage = 0x08, kGaugeStatus = 0x0a, kGaugeCurrent = 0x0c, kGaugeRemaining = 0x10,
  kGaugeFull = 0x12, kGaugeAverageCurrent = 0x14, kGaugeCycles = 0x2a, kGaugeSoc = 0x2c, kGaugeDesign = 0x3c,
};
constexpr uint16_t kGaugeDsg = 1u << 6, kGaugeFc = 1u << 5, kGaugeFd = 1u << 4, kGaugeInit = 1u << 7;

struct GaugeReading {
  bool ok = false;
  I2cResult error = I2cResult::ok;
  uint8_t failedCommand = 0;
  int voltageMv = 0, currentMa = 0, averageMa = 0, socPct = 0, remainingMah = 0, fullMah = 0, cycles = 0;
  int designMah = 0;      // 0 when the gauge did not answer it (read on its own: it never invalidates the reading)
  int tempDeciC = 0;      // tenths of a degree Celsius
  uint16_t status = 0;
};

inline int16_t asSigned(uint16_t v) { return int16_t(v); }

inline GaugeReading readGauge(I2cBus &bus)
{
  static const uint8_t kOrder[] = {kGaugeVoltage, kGaugeCurrent, kGaugeAverageCurrent, kGaugeSoc, kGaugeRemaining, kGaugeFull,
                                   kGaugeTemperature, kGaugeStatus, kGaugeCycles};
  GaugeReading g;
  uint16_t v[sizeof kOrder] = {};
  for (unsigned i = 0; i < sizeof kOrder; ++i) {
    g.error = bus.readWord(kGaugeAddress, kOrder[i], &v[i]);
    if (g.error != I2cResult::ok) {
      g.failedCommand = kOrder[i];
      return g;
    }
  }
  g.voltageMv = v[0];
  g.currentMa = asSigned(v[1]);
  g.averageMa = asSigned(v[2]);
  g.socPct = v[3];
  g.remainingMah = v[4];
  g.fullMah = v[5];
  g.tempDeciC = int(v[6]) - 2732;
  g.status = v[7];
  g.cycles = v[8];
  uint16_t design = 0;
  if (bus.readWord(kGaugeAddress, kGaugeDesign, &design) == I2cResult::ok && design && design != 0xffff) g.designMah = design;
  // A reading no battery can have means the transaction went wrong somewhere (a stuck line reads as 0xffff).
  g.ok = g.voltageMv >= 2500 && g.voltageMv <= 5000 && g.socPct <= 100 && g.tempDeciC > -400 && g.tempDeciC < 900;
  if (!g.ok) g.failedCommand = 0xff;
  return g;
}

struct PackReading {
  bool ok = false;     // at least one pack read
  unsigned packs = 0;  // how many
  int voltageMv = 0, currentMa = 0, socPct = 0, remainingMah = 0, fullMah = 0, tempDeciC = 0;
  bool charging = false, discharging = false, full = false;
};

inline PackReading combinePacks(const GaugeReading *g, unsigned n)
{
  PackReading p;
  long voltage = 0, temperature = 0;
  for (unsigned i = 0; i < n; ++i) {
    if (!g[i].ok) continue;
    p.packs++;
    voltage += g[i].voltageMv;
    if (p.packs == 1 || g[i].tempDeciC > temperature) temperature = g[i].tempDeciC;  // the warmer pack
    p.currentMa += g[i].currentMa;
    p.remainingMah += g[i].remainingMah;
    p.fullMah += g[i].fullMah;
    p.full = (p.packs == 1 || p.full) && (g[i].status & kGaugeFc);
  }
  if (!p.packs) return p;
  p.ok = true;
  p.voltageMv = int(voltage / long(p.packs));
  p.tempDeciC = int(temperature);
  if (p.fullMah > 0) {
    p.socPct = int((long(p.remainingMah) * 100 + p.fullMah / 2) / p.fullMah);
  } else {
    long soc = 0;
    for (unsigned i = 0; i < n; ++i) if (g[i].ok) soc += g[i].socPct;
    p.socPct = int(soc / long(p.packs));
  }
  if (p.socPct > 100) p.socPct = 100;
  p.charging = p.currentMa > 20;
  p.discharging = p.currentMa < -20;
  return p;
}

}  // namespace pianopower
#endif
