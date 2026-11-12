// SPDX-License-Identifier: BSD-2-Clause-Patent
// package/PianoKeyboard/BitI2c.h of this repository (UPSTREAM has its sha256) with the namespace, the include guard and this comment
// changed, the code unchanged: PianoKeyboard is not modified, so PianoCamera carries its own copy (as PianoBacklight and PianoAudio
// do), for the front sensor's bus (gpio4/gpio5) and the rear sensor's when piano-camera-rear-bus=1 (gpio115/gpio116).
// I2C master in software on two TLMM pins, open drain: a line is driven low or released to its pull-up, never driven high. Register
// writes and register reads (write the register, repeated START, read N bytes), clock stretching honoured. Generalised from
// package/PianoPower/Gauge.h's I2cBus (same repository, BSD-2-Clause-Patent; run on piano at ~80 kHz) with a configurable half-bit
// time. No QUP serial engine is used: its clocks, firmware and DFS tables are not ours to set up.
#ifndef PIANO_CAMERA_BIT_I2C_H
#define PIANO_CAMERA_BIT_I2C_H
#include <stdint.h>

namespace pianocamera {

struct LineIo {
  void *ctx;
  void (*sda)(void *ctx, bool release);
  void (*scl)(void *ctx, bool release);
  bool (*readSda)(void *ctx);
  bool (*readScl)(void *ctx);
  void (*delayUs)(void *ctx, unsigned us);
};

enum class I2c : uint8_t { ok, nackAddress, nackData, busBusy, stretchTimeout };
inline const char *i2cName(I2c r)
{
  switch (r) {
  case I2c::ok: return "ok";
  case I2c::nackAddress: return "no ack (address)";
  case I2c::nackData: return "no ack (data)";
  case I2c::busBusy: return "bus held low";
  case I2c::stretchTimeout: return "clock stretch timeout";
  }
  return "?";
}

class BitI2c {
 public:
  void configure(const LineIo &io, unsigned halfUs, unsigned stretchUs)
  {
    io_ = io;
    half_ = halfUs;
    stretch_ = stretchUs;
  }
  unsigned failedByte = 0;   // the byte index of the last NACK

  // Frees a bus a device holds (SDA low): up to nine clocks until SDA is released, then a STOP.
  bool recover()
  {
    io_.sda(io_.ctx, true);
    for (unsigned i = 0; i < 9 && !io_.readSda(io_.ctx); ++i) {
      io_.scl(io_.ctx, false);
      wait();
      if (!sclHigh()) return false;
      wait();
    }
    stop();
    return io_.readSda(io_.ctx) && io_.readScl(io_.ctx);
  }
  // START, address+W, bytes, STOP.
  I2c write(uint8_t address, const uint8_t *bytes, unsigned n) { return run(address, bytes, n, nullptr, 0); }
  // START, address+W, the register bytes, repeated START, address+R, n bytes (ACK all but the last), STOP.
  I2c writeRead(uint8_t address, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn) { return run(address, w, wn, r, rn); }

 private:
  I2c run(uint8_t address, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
  {
    if ((!io_.readSda(io_.ctx) || !io_.readScl(io_.ctx)) && !recover()) return I2c::busBusy;
    const I2c result = transfer(address, w, wn, r, rn);
    stop();
    io_.delayUs(io_.ctx, 2 * half_ + 4);   // bus free time
    return result;
  }
  I2c transfer(uint8_t address, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
  {
    I2c e = I2c::ok;
    failedByte = 0;
    if (wn || !rn) {
      if (!start()) return I2c::busBusy;
      if (!writeByte(uint8_t(address << 1), &e)) return e != I2c::ok ? e : I2c::nackAddress;
      for (unsigned i = 0; i < wn; ++i)
        if (!writeByte(w[i], &e)) {
          failedByte = i + 1;
          return e != I2c::ok ? e : I2c::nackData;
        }
    }
    if (!rn) return I2c::ok;
    if (!start()) return I2c::busBusy;   // (repeated) START
    if (!writeByte(uint8_t(address << 1 | 1), &e)) return e != I2c::ok ? e : I2c::nackAddress;
    for (unsigned i = 0; i < rn; ++i)
      if (!readByte(&r[i], i + 1 < rn, &e)) return e;
    return I2c::ok;
  }
  void wait() { io_.delayUs(io_.ctx, half_); }
  bool sclHigh()
  {
    io_.scl(io_.ctx, true);
    for (unsigned waited = 0; !io_.readScl(io_.ctx); waited += 5) {
      if (waited >= stretch_) return false;
      io_.delayUs(io_.ctx, 5);
    }
    return true;
  }
  // START or repeated START: SDA falls while SCL is high; leaves SCL low.
  bool start()
  {
    io_.sda(io_.ctx, true);
    wait();
    if (!sclHigh()) return false;
    wait();
    if (!io_.readSda(io_.ctx)) return false;   // someone else holds SDA
    io_.sda(io_.ctx, false);
    wait();
    io_.scl(io_.ctx, false);
    wait();
    return true;
  }
  void stop()
  {
    io_.sda(io_.ctx, false);
    wait();
    sclHigh();
    wait();
    io_.sda(io_.ctx, true);
    wait();
  }
  // One bit, SCL low on entry and exit; returns SDA as sampled while SCL was high.
  bool bit(bool release, I2c *e)
  {
    io_.sda(io_.ctx, release);
    wait();
    if (!sclHigh()) {
      *e = I2c::stretchTimeout;
      return false;
    }
    wait();
    const bool level = io_.readSda(io_.ctx);
    io_.scl(io_.ctx, false);
    return level;
  }
  bool writeByte(uint8_t byte, I2c *e)
  {
    for (int i = 7; i >= 0; --i) {
      bit((byte >> i) & 1, e);
      if (*e != I2c::ok) return false;
    }
    const bool nack = bit(true, e);   // the device pulls SDA low to acknowledge
    return *e == I2c::ok && !nack;
  }
  bool readByte(uint8_t *byte, bool ack, I2c *e)
  {
    uint8_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v = uint8_t(v << 1 | (bit(true, e) ? 1 : 0));
      if (*e != I2c::ok) return false;
    }
    bit(!ack, e);
    *byte = v;
    return *e == I2c::ok;
  }

  LineIo io_ = {};
  unsigned half_ = 5, stretch_ = 20000;
};

}  // namespace pianocamera
#endif
