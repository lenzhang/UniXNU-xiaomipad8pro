// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal B6: conversions of piano's board thermistors on the PMIC ADC5 gen3, through the SPMI PMIC arbiter v7 as EE 0, and a
// round over the eleven channels with its error handling. Portable (no IOKit): the kext (PianoThermalAdc.cpp) and
// tests/adc_host.cpp (a register model of the arbiter, the SDAM and the PBS sequencer) include it. docs/drivers/thermal.md
// section 12.
//
// The arbiter (v7, DT spmi@c400000, qcom,ee = 0; the same facts as package/PianoPower/SpmiObserver.h, k27 0057 and PianoDwc3's
// setRestartReason, the last two of which write SDAM 0x7100 this way on the tablet):
//   core  0x0C400000: version +0 (v7: 0x7xxxxxxx), APID count +4 [10:0], APID map +0x2000 + 4 * APID (PPID = SID << 8 | peripheral
//         in [19:8]);
//   cnfg  0x0C42D000: the owner table, 4 bytes per APID, owner EE in [2:0];
//   obsv  0x0C440000 + 0x20 * APID: EE 0's observer channel (reads of any peripheral);
//   chnls 0x0C500000 + 0x1000 * APID: the APID's read/write channel, which writes only for the APID's owner EE.
//   A channel: command +0x00 = opcode << 27 | register << 4 | (bytes - 1), status +0x08 (bit 0 done, 1 failure, 2 denied, 3 dropped),
//   write data +0x10 / +0x14, read data +0x18 / +0x1c, 1..8 bytes little-endian; opcode 0 extended write (16-bit address), 1
//   extended read; Linux polls the status 1000 x 1 us (drivers/spmi/spmi-pmic-arb.c, the specification only).
// The ADC (drivers/iio/adc/qcom-spmi-adc5-gen3.c, include/linux/iio/adc/qcom-adc5-gen3-common.h, drivers/nvmem/qcom-spmi-sdam.c,
// the specification only): the ADC is driven by the PMIC's PBS sequencer through SDAM memory. SDAM 0 of the ADC (PMK8550 SID 0
// 0x9000, PPID 0x090) holds the command bytes, and its channel 0 is the immediate-conversion channel. Offsets in the SDAM:
//   0x45 HS (handshake: 0x01 ready, bit 7 busy), 0x46 STATUS1 (bit 7 conversion fault, bit 0 EOC), 0x4a EOC_STS (bit 0: channel 0),
//   0x4b EOC_CLR, 0x4e CONV_ERR_CLR (bit 0), 0x4f SID (bus [6:5], SID [4:0]), 0x50 PERPH_CH (bit 7 = a conversion request, clear =
//   a status clear; [2:0] the SDAM channel), 0x51 TIMER_SEL (1 = immediate), 0x52 DIG_PARAM (calibration [5:4], decimation [3:2],
//   the other bits kept as found), 0x53 FAST_AVG (bit 7 enable, [2:0] samples), 0x54 the ADC channel, 0x55 DELAY_CTL (hardware
//   settle [3:0]), 0x5c/0x5d channel 0's result (little-endian), 0xe5 CONV_REQ (the SDAM's PBS trigger: 1 starts, the PBS clears).
//   One conversion: HS == ready and CONV_REQ == 0 (Linux polls 150 x 100 us); 0x4f..0x55 read, set, written back in one 7-byte
//   write; CONV_REQ = 1; the end of conversion (Linux takes the SDAM interrupt and waits up to 501 ms: the PBS may retry twice;
//   here EOC_STS bit 0 is polled); the result; the "status clear" EOC_CLR = 1, PERPH_CH = 0, CONV_REQ = 1. A fault is cleared the
//   same way through CONV_ERR_CLR.
//   Added here (not in Linux): a stale EOC or fault found before a conversion is cleared first (an interrupt-less poll would take a
//   stale EOC for its own), and the 7 command bytes are read back before the trigger.
//
// What is written: only SDAM 0's command and status-clear bytes (0x4b, 0x4e, 0x4f..0x55, 0xe5), only through the read/write channel
// of the APID of PPID 0x090, and only when the owner table gives that APID to EE 0 (checked before the channel is even mapped).
// writable() is the allowlist; Bus::write refuses anything else before a store. Nothing else of any PMIC is written: the
// survey, the thermistor pins' check and every other access are observer reads. Every wait is bounded (Timing). An error disables
// the channel it happened on; an error of the shared machinery (the arbiter, the handshake, the SDAM) stops the ADC for the boot.
#ifndef PIANO_THERMAL_PMIC_ADC_H
#define PIANO_THERMAL_PMIC_ADC_H
#include <stdint.h>
#include "AdcChannels.h"
#include "Ntc.h"

namespace pianothermal::adc {

// ------------------------------------------------------------------------------------------------------------- the arbiter
constexpr uint64_t kSpmiCore = 0x0c400000, kSpmiCnfg = 0x0c42d000, kSpmiObsv = 0x0c440000, kSpmiChnls = 0x0c500000;
constexpr uint32_t kCoreBytes = 0x3000, kCnfgBytes = 0x1000, kObsvBytes = 0x8000, kChnlBytes = 0x1000;
constexpr uint32_t kMaxApids = 1024, kApidMap = 0x2000, kObsvStride = 0x20, kChnlStride = 0x1000;
constexpr uint32_t kChCmd = 0x00, kChStatus = 0x08, kChWdata0 = 0x10, kChWdata1 = 0x14, kChRdata0 = 0x18, kChRdata1 = 0x1c;
constexpr uint32_t kOpExtWriteL = 0, kOpExtReadL = 1;
constexpr uint32_t kChDone = 1u << 0, kChFailure = 1u << 1, kChDenied = 1u << 2, kChDropped = 1u << 3;
constexpr unsigned kChPolls = 1000;   // x 1 us, Linux's PMIC_ARB_TIMEOUT_US

// ------------------------------------------------------------------------------------------------------------- the ADC SDAM
constexpr uint16_t kSdamBase = 0x9000, kSdam1Base = 0x9100;
constexpr uint16_t kAdcPpid = uint16_t(0 << 8 | kSdamBase >> 8);   // PMK8550 (SID 0), peripheral 0x90
constexpr uint8_t kRegType = 0x04, kRegSubtype = 0x05, kRegSdamSize = 0x44, kRegHs = 0x45, kRegStatus1 = 0x46, kRegEocSts = 0x4a,
                  kRegEocClr = 0x4b, kRegConvErrClr = 0x4e, kRegSid = 0x4f, kRegPerphCh = 0x50, kRegTimerSel = 0x51,
                  kRegDigParam = 0x52, kRegFastAvg = 0x53, kRegChSel = 0x54, kRegDelayCtl = 0x55, kRegData0 = 0x5c,
                  kRegConvReq = 0xe5;
constexpr uint8_t kHsReady = 0x01, kHsBusy = 0x80, kStatus1Fault = 0x80, kStatus1Eoc = 0x01, kEocChan0 = 0x01, kClearReq = 0x01,
                  kChanConvReq = 0x80, kTimeImmediate = 0x01, kFastAvgEnable = 0x80, kConvReqStart = 0x01;
constexpr uint8_t kDigCalShift = 4, kDigCalMask = 0x30, kDigDecShift = 2, kDigDecMask = 0x0c, kCalRatiometric = 1;
constexpr uint8_t kSidBusShift = 5, kSettleMask = 0x0f;
constexpr unsigned kConfigBytes = 7;   // 0x4f SID .. 0x55 DELAY_CTL
constexpr unsigned kStatusBytes = 5;   // 0x46 STATUS1 .. 0x4a EOC_STS

constexpr bool writableByte(unsigned r)
{
  return r == kRegEocClr || r == kRegConvErrClr || (r >= kRegSid && r <= kRegDelayCtl) || r == kRegConvReq;
}
// The write allowlist: every byte of [reg, reg + bytes) one of the conversion registers.
constexpr bool writable(unsigned reg, unsigned bytes)
{
  if (!bytes || bytes > 8 || reg + bytes > 0x100) return false;
  for (unsigned i = 0; i < bytes; ++i)
    if (!writableByte(reg + i)) return false;
  return true;
}

// The thermistor pins (read only): PMIC GPIO n is peripheral 0x88 + n - 1; TYPE 0x04 = 0x10; MODE_CTL 0x40; DIG_PULL_CTL 0x42 (shown
// only); EN_CTL 0x46, bit 7 the master enable (clear = high-impedance, the DT's bias-high-impedance; Linux pinctrl-spmi-gpio.c, the
// specification only).
constexpr uint8_t kGpioPeriphBase = 0x88, kGpioRegType = 0x04, kGpioRegMode = 0x40, kGpioRegPull = 0x42, kGpioRegEn = 0x46,
                  kGpioType = 0x10, kGpioMasterEn = 0x80;
constexpr uint16_t gpioPpid(uint8_t sid, uint8_t gpio) { return uint16_t(uint16_t(sid) << 8 | uint16_t(kGpioPeriphBase + gpio - 1)); }

// The plausible range of a board reading: the part's rated range. Outside: the channel is disabled (a broken or absent sensor).
constexpr int32_t kMinMilliC = -40000, kMaxMilliC = 125000;

// ------------------------------------------------------------------------------------------------------------- access
// The windows: core, cnfg and obsv as above (obsv: EE 0's 1024 observer channels), chnl = the read/write channel page of the ADC
// SDAM's APID only (mapped by the caller after find()). rd/wr take a window and a byte offset into it; wr is only ever used on obsv
// (a read command) and chnl. delayUs waits at least that long (it may sleep); nowUs is a monotonic clock.
enum class Win : uint8_t { core, cnfg, obsv, chnl };
struct Io {
  void *ctx;
  uint32_t (*rd)(void *ctx, Win w, uint32_t offset);
  void (*wr)(void *ctx, Win w, uint32_t offset, uint32_t value);
  void (*delayUs)(void *ctx, uint32_t us);
  uint64_t (*nowUs)(void *ctx);
};

enum class Err : uint8_t {
  none,
  // the shared machinery: the ADC stops for the boot
  notArbiterV7,     // the arbiter is not a v7 one (or its windows read nonsense)
  noApid,           // PPID 0x090 is not in the APID map
  notOwned,         // its APID is another EE's: this processor may not write it
  spmiFailed,       // an arbiter transaction ended with failure or dropped
  spmiDenied,       // ... denied
  spmiTimeout,      // ... never done (1000 us)
  handshake,        // HS never ready with CONV_REQ clear (20 ms)
  eocStuck,         // a stale EOC that a status clear did not clear
  faultStuck,       // a stale conversion fault that a status clear did not clear
  configMismatch,   // the command bytes read back differ from those written
  refused,          // a write outside the allowlist or without the channel (a bug: never sent)
  // one channel: that channel is disabled
  convTimeout,      // no end of conversion (600 ms)
  convFault,        // the PBS flagged a conversion fault
  invalidData,      // Gen3's no-data code 0x8000
  open,             // a code at full scale: no thermistor (open input)
  shorted,          // a code of 0 (or a negative one): input at ground
  implausible,      // a temperature outside -40..125 C
  badChannel,       // a channel description that cannot be encoded (a bug)
};
constexpr bool isGlobal(Err e) { return e != Err::none && e < Err::convTimeout; }
inline const char *errName(Err e)
{
  switch (e) {
    case Err::none: return "ok";
    case Err::notArbiterV7: return "not an SPMI arbiter v7";
    case Err::noApid: return "SDAM 0x9000 (PPID 0x090) not in the APID map";
    case Err::notOwned: return "SDAM 0x9000's APID belongs to another EE";
    case Err::spmiFailed: return "SPMI transaction failed";
    case Err::spmiDenied: return "SPMI transaction denied";
    case Err::spmiTimeout: return "SPMI transaction timed out";
    case Err::handshake: return "PBS handshake never ready (20 ms)";
    case Err::eocStuck: return "a stale end-of-conversion does not clear";
    case Err::faultStuck: return "a stale conversion fault does not clear";
    case Err::configMismatch: return "command bytes read back differ";
    case Err::refused: return "write refused (outside the allowlist)";
    case Err::convTimeout: return "no end of conversion (600 ms)";
    case Err::convFault: return "conversion fault";
    case Err::invalidData: return "no-data code 0x8000";
    case Err::open: return "open input (code at full scale)";
    case Err::shorted: return "shorted input (code 0 or negative)";
    case Err::implausible: return "temperature outside -40..125 C";
    case Err::badChannel: return "channel settings not encodable";
  }
  return "?";
}

struct Arbiter {
  uint32_t version = 0, count = 0;
  bool v7 = false;
};
inline Arbiter readArbiter(const Io &io)
{
  Arbiter a;
  a.version = io.rd(io.ctx, Win::core, 0);
  a.count = io.rd(io.ctx, Win::core, 4) & 0x7ffu;
  a.v7 = (a.version >> 28) == 7 && a.count && a.count <= kMaxApids;
  return a;
}

struct Apid {
  int apid = -1;          // -1: the PPID is not in the map
  uint32_t owner = 0;     // the owner EE of the entry chosen
  unsigned entries = 0;   // map entries with this PPID
};
// The APID of a PPID; an entry owned by EE 0 wins (Linux's choice for its writes).
inline Apid findApid(const Io &io, const Arbiter &a, uint16_t ppid)
{
  Apid f;
  for (uint32_t i = 0; a.v7 && i < a.count; ++i) {
    const uint32_t map = io.rd(io.ctx, Win::core, kApidMap + 4 * i);
    if (!map || ((map >> 8) & 0xfffu) != ppid) continue;
    ++f.entries;
    const uint32_t owner = io.rd(io.ctx, Win::cnfg, 4 * i) & 7u;
    if (f.apid < 0 || (owner == 0 && f.owner != 0)) {
      f.apid = int(i);
      f.owner = owner;
    }
  }
  return f;
}

// One arbiter channel's transactions. Reads go through EE 0's observer channel of any APID; writes only through the chnl window
// (the ADC SDAM's own APID), only while `writes` is set, only to writable() bytes.
struct Bus {
  Io io{};
  int apid = -1;
  bool writes = false;
  uint32_t lastStatus = 0;
  uint32_t reads = 0, writeCount = 0;

  Err wait(Win w, uint32_t channel)
  {
    uint32_t st = 0;
    for (unsigned i = 0; i < kChPolls; ++i) {
      st = io.rd(io.ctx, w, channel + kChStatus);
      if (st & kChDone) {
        lastStatus = st;
        return (st & kChDenied) ? Err::spmiDenied : (st & (kChFailure | kChDropped)) ? Err::spmiFailed : Err::none;
      }
      io.delayUs(io.ctx, 1);
    }
    lastStatus = st;
    return Err::spmiTimeout;
  }
  Err readApid(int a, unsigned reg, uint8_t *buf, unsigned bytes)
  {
    if (a < 0 || uint32_t(a) >= kMaxApids || !bytes || bytes > 8 || reg + bytes > 0x100) return Err::refused;
    const uint32_t ch = kObsvStride * uint32_t(a);
    // A failed or unfinished observer read is tried again up to four times 500 us apart (as PianoWifi's and PianoPower's
    // SpmiObserver.h): on the 2026-11-12 probes with several drivers on the bus, single reads came back failed. Not a denial.
    Err e = Err::none;
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
      if (attempt) {
        for (unsigned i = 0; i < 5000 && !(io.rd(io.ctx, Win::obsv, ch + kChStatus) & kChDone); ++i) io.delayUs(io.ctx, 1);
        io.delayUs(io.ctx, 500);
      }
      io.wr(io.ctx, Win::obsv, ch + kChCmd, kOpExtReadL << 27 | uint32_t(reg) << 4 | (bytes - 1));
      e = wait(Win::obsv, ch);
      if (e != Err::spmiFailed && e != Err::spmiTimeout) break;
    }
    if (e != Err::none) return e;
    const uint32_t lo = io.rd(io.ctx, Win::obsv, ch + kChRdata0), hi = bytes > 4 ? io.rd(io.ctx, Win::obsv, ch + kChRdata1) : 0;
    for (unsigned b = 0; b < bytes; ++b) buf[b] = uint8_t((b < 4 ? lo : hi) >> (8 * (b & 3)));
    ++reads;
    return Err::none;
  }
  Err read(unsigned reg, uint8_t *buf, unsigned bytes) { return readApid(apid, reg, buf, bytes); }
  Err write(unsigned reg, const uint8_t *buf, unsigned bytes)
  {
    if (!writes || apid < 0 || !writable(reg, bytes)) return Err::refused;
    uint32_t lo = 0, hi = 0;
    for (unsigned b = 0; b < bytes; ++b) (b < 4 ? lo : hi) |= uint32_t(buf[b]) << (8 * (b & 3));
    io.wr(io.ctx, Win::chnl, kChWdata0, lo);
    if (bytes > 4) io.wr(io.ctx, Win::chnl, kChWdata1, hi);
    io.wr(io.ctx, Win::chnl, kChCmd, kOpExtWriteL << 27 | uint32_t(reg) << 4 | (bytes - 1));
    ++writeCount;
    return wait(Win::chnl, 0);
  }
};

// The waits (Linux's worst cases with a margin). A wait spins at spinUs for its first spinForUs, then polls every sleepUs.
struct Timing {
  uint32_t spinUs = 100, spinForUs = 1000, sleepUs = 1000;
  uint32_t readyTimeoutUs = 20000;   // HS ready and CONV_REQ 0 (Linux: 150 x 100 us)
  uint32_t eocTimeoutUs = 600000;    // the end of a conversion (Linux: 501 ms)
};

struct Reading {
  uint16_t code = 0;
  uint64_t ohms = 0;
  int32_t milliC = 0;
  uint32_t us = 0;   // the conversion's duration, handshake to status clear
};

// The code of a channel as a resistance and a temperature, or why not.
inline Err scale(const ChannelDesc &c, Reading &r)
{
  const uint32_t fs = c.fullScale();
  if (c.gen == Gen::gen3 && r.code == kInvalidCodeGen3) return Err::invalidData;
  if (r.code == 0 || (c.gen == Gen::gen3 && (r.code & 0x8000))) return Err::shorted;
  if (r.code >= fs) return Err::open;
  r.ohms = ntc::ohmsOf(r.code, fs, kPullUpOhms);
  r.milliC = ntc::milliC(r.ohms);
  if (r.milliC < kMinMilliC || r.milliC > kMaxMilliC) return Err::implausible;
  return Err::none;
}

// The 7 command bytes of an immediate conversion of channel c, from the 7 found (DIG_PARAM keeps its other bits, as Linux does).
inline void commandBytes(const ChannelDesc &c, const uint8_t found[kConfigBytes], uint8_t out[kConfigBytes])
{
  out[0] = uint8_t(c.bus() << kSidBusShift | c.sid());
  out[1] = kChanConvReq | 0;   // SDAM channel 0, a conversion request
  out[2] = kTimeImmediate;
  out[3] = uint8_t((found[3] & ~(kDigCalMask | kDigDecMask)) | kCalRatiometric << kDigCalShift |
                   uint8_t(decimationIndex(c.decimation)) << kDigDecShift);
  out[4] = kFastAvgEnable | 0;  // one sample
  out[5] = c.channel();
  out[6] = uint8_t(settleIndex(c.settleUs)) & kSettleMask;
}

// What the SDAM holds before the first conversion (reads only).
struct Survey {
  uint8_t type = 0, subtype = 0, size = 0, hs = 0, status1 = 0, eoc = 0, convReq = 0;
  uint8_t command[kConfigBytes] = {};
};

struct PinState {
  int apid = -1;
  Err err = Err::none;
  uint8_t type = 0, mode = 0, pull = 0, en = 0;
  bool highZ() const { return !(en & kGpioMasterEn); }
  // A reading on this pin counts: it read, and it is high-impedance.
  bool clear() const { return err == Err::none && highZ(); }
  bool same(const PinState &o) const
  {
    return apid == o.apid && err == o.err && type == o.type && mode == o.mode && pull == o.pull && en == o.en;
  }
};

class Adc {
 public:
  Bus bus;
  Timing timing;
  Arbiter arbiter;
  Apid where;
  Err stopped = Err::none;   // set: no more conversions this boot
  uint8_t lastHs = 0, lastConvReq = 0, lastStatus1 = 0, lastEoc = 0;   // what a failed wait saw last
  uint32_t staleClears = 0, conversions = 0, failures = 0, maxUs = 0;
  uint64_t totalUs = 0;

  // Reads only: the arbiter, the APID of the ADC's SDAM 0 and its owner. none = the caller may map the APID's read/write channel
  // (kSpmiChnls + kChnlStride * where.apid) and call enableWrites().
  Err find(const Io &io)
  {
    bus.io = io;
    arbiter = readArbiter(io);
    if (!arbiter.v7) return stop(Err::notArbiterV7);
    where = findApid(io, arbiter, kAdcPpid);
    if (where.apid < 0) return stop(Err::noApid);
    bus.apid = where.apid;
    if (where.owner != 0) return stop(Err::notOwned);
    return Err::none;
  }
  void enableWrites() { bus.writes = stopped == Err::none && where.apid >= 0 && where.owner == 0; }

  Err survey(Survey &s)
  {
    uint8_t b[3];
    Err e;
    if ((e = bus.read(kRegType, b, 2)) != Err::none) return stop(e);
    s.type = b[0];
    s.subtype = b[1];
    if ((e = bus.read(kRegSdamSize, b, 3)) != Err::none) return stop(e);
    s.size = b[0];
    s.hs = b[1];
    s.status1 = b[2];
    if ((e = bus.read(kRegEocSts, &s.eoc, 1)) != Err::none) return stop(e);
    if ((e = bus.read(kRegSid, s.command, kConfigBytes)) != Err::none) return stop(e);
    if ((e = bus.read(kRegConvReq, &s.convReq, 1)) != Err::none) return stop(e);
    return Err::none;
  }

  PinState readPin(const ThermPin &p)
  {
    PinState s;
    const Apid a = findApid(bus.io, arbiter, gpioPpid(p.sid, p.gpio));
    s.apid = a.apid;
    if (a.apid < 0) {
      s.err = Err::noApid;
      return s;
    }
    if ((s.err = bus.readApid(a.apid, kGpioRegType, &s.type, 1)) != Err::none) return s;
    if ((s.err = bus.readApid(a.apid, kGpioRegMode, &s.mode, 1)) != Err::none) return s;
    if ((s.err = bus.readApid(a.apid, kGpioRegPull, &s.pull, 1)) != Err::none) return s;
    s.err = bus.readApid(a.apid, kGpioRegEn, &s.en, 1);
    return s;
  }

  // One immediate conversion of channel c: the state machine of the header comment. none: r holds the code, the resistance and
  // the temperature. A channel error: r.code holds what was read (if anything). A global error also sets `stopped`.
  Err convert(const ChannelDesc &c, Reading &r)
  {
    r = Reading();
    if (stopped != Err::none) return stopped;
    if (decimationIndex(c.decimation) < 0 || settleIndex(c.settleUs) < 0) return Err::badChannel;
    const uint64_t t0 = now();
    Err e = waitReady();
    if (e != Err::none) return fail(stop(e));
    uint8_t st[kStatusBytes];
    if ((e = bus.read(kRegStatus1, st, kStatusBytes)) != Err::none) return fail(stop(e));
    if (st[0] & kStatus1Fault) {   // a fault nobody cleared
      ++staleClears;
      if ((e = statusClear(kRegConvErrClr, kClearReq)) != Err::none || (e = waitReady()) != Err::none ||
          (e = bus.read(kRegStatus1, st, kStatusBytes)) != Err::none)
        return fail(stop(e));
      if (st[0] & kStatus1Fault) return fail(stop(Err::faultStuck));
    }
    if (st[4] & kEocChan0) {       // an end of conversion nobody cleared: it would end this one at once
      ++staleClears;
      if ((e = statusClear(kRegEocClr, kEocChan0)) != Err::none || (e = waitReady()) != Err::none ||
          (e = bus.read(kRegStatus1, st, kStatusBytes)) != Err::none)
        return fail(stop(e));
      if (st[4] & kEocChan0) return fail(stop(Err::eocStuck));
    }
    uint8_t found[kConfigBytes], cmd[kConfigBytes], back[kConfigBytes];
    if ((e = bus.read(kRegSid, found, kConfigBytes)) != Err::none) return fail(stop(e));
    commandBytes(c, found, cmd);
    if ((e = bus.write(kRegSid, cmd, kConfigBytes)) != Err::none) return fail(stop(e));
    if ((e = bus.read(kRegSid, back, kConfigBytes)) != Err::none) return fail(stop(e));
    for (unsigned i = 0; i < kConfigBytes; ++i)
      if (back[i] != cmd[i]) return fail(stop(Err::configMismatch));
    const uint8_t start = kConvReqStart;
    if ((e = bus.write(kRegConvReq, &start, 1)) != Err::none) return fail(stop(e));
    const uint64_t began = now();
    for (unsigned i = 0;; ++i) {
      if ((e = bus.read(kRegStatus1, st, kStatusBytes)) != Err::none) return fail(stop(e));
      lastStatus1 = st[0];
      lastEoc = st[4];
      if (st[0] & kStatus1Fault) {
        if ((e = statusClear(kRegConvErrClr, kClearReq)) != Err::none) return fail(stop(e));
        return fail(Err::convFault);
      }
      if (st[4] & kEocChan0) break;
      const uint64_t waited = now() - began;
      if (waited >= timing.eocTimeoutUs || i > maxPolls(timing.eocTimeoutUs)) return fail(Err::convTimeout);
      pause(waited);
    }
    uint8_t d[2];
    if ((e = bus.read(kRegData0, d, 2)) != Err::none) return fail(stop(e));
    r.code = uint16_t(d[0] | d[1] << 8);
    if ((e = statusClear(kRegEocClr, kEocChan0)) != Err::none) return fail(stop(e));
    r.us = uint32_t(now() - t0);
    ++conversions;
    totalUs += r.us;
    if (r.us > maxUs) maxUs = r.us;
    e = scale(c, r);
    return e == Err::none ? e : fail(e);
  }

 private:
  uint64_t now() const { return bus.io.nowUs(bus.io.ctx); }
  unsigned maxPolls(uint32_t timeoutUs) const { return timeoutUs / (timing.spinUs ? timing.spinUs : 1) + 16; }   // a stuck clock
  void pause(uint64_t waited) { bus.io.delayUs(bus.io.ctx, waited < timing.spinForUs ? timing.spinUs : timing.sleepUs); }
  Err stop(Err e)
  {
    if (isGlobal(e) && stopped == Err::none) stopped = e;
    return e;
  }
  Err fail(Err e)
  {
    ++failures;
    return e;
  }
  // HS ready and no trigger pending.
  Err waitReady()
  {
    const uint64_t began = now();
    for (unsigned i = 0;; ++i) {
      uint8_t hs = 0, req = 0;
      Err e = bus.read(kRegHs, &hs, 1);
      if (e != Err::none) return e;
      lastHs = hs;
      if (hs == kHsReady) {
        if ((e = bus.read(kRegConvReq, &req, 1)) != Err::none) return e;
        lastConvReq = req;
        if (!req) return Err::none;
      }
      const uint64_t waited = now() - began;
      if (waited >= timing.readyTimeoutUs || i > maxPolls(timing.readyTimeoutUs)) return Err::handshake;
      pause(waited);
    }
  }
  // Linux's status clear: the clear bit, then PERPH_CH = 0 (this request only clears) and the trigger.
  Err statusClear(uint8_t reg, uint8_t value)
  {
    const uint8_t zero = 0, start = kConvReqStart;
    Err e = bus.write(reg, &value, 1);
    if (e == Err::none) e = bus.write(kRegPerphCh, &zero, 1);
    if (e == Err::none) e = bus.write(kRegConvReq, &start, 1);
    return e;
  }
};

// ------------------------------------------------------------------------------------------------------------- the round
struct ChannelState {
  bool enabled = true, valid = false;
  Err err = Err::none;     // why it is disabled
  Reading last{};          // the last good reading (valid), or what the failing conversion read (or the one its pin held out)
  uint64_t atUs = 0;       // when `last` was taken
  uint32_t ok = 0;
  // A channel on the firmware's pin (AdcChannels.h gatePin): the pin as read before this round's conversion, whether that read
  // differs from the round before (the first read counts as a change), and how many good conversions the pin held out.
  PinState pin{};
  bool pinRead = false, pinChanged = false, pinHeld = false;
  uint32_t held = 0;
};

// Every enabled channel once. A channel error disables that channel; a global error stops the ADC and so disables them all. A
// channel on the firmware's pin is converted as any other, but its reading counts (valid) only while that pin, read just before,
// is high-impedance; otherwise it stays enabled, not valid, and is tried again next round.
class Board {
 public:
  Adc adc;
  ChannelState ch[kChannelCount];
  uint32_t rounds = 0;
  struct Event {
    unsigned channel;
    Err err;
  };
  Event events[kChannelCount];   // the channels disabled by the last round, where it happened
  unsigned eventCount = 0;

  unsigned enabledCount() const
  {
    unsigned n = 0;
    for (const ChannelState &s : ch) n += s.enabled;
    return n;
  }
  void stopAll(Err why)
  {
    for (ChannelState &s : ch) {
      if (s.enabled) s.err = why;
      s.enabled = s.valid = false;
    }
  }
  void round()
  {
    eventCount = 0;
    for (unsigned i = 0; i < kChannelCount; ++i) {
      ChannelState &s = ch[i];
      if (!s.enabled) continue;
      if (adc.stopped != Err::none) {
        stopAll(adc.stopped);
        break;
      }
      const int g = gatePin(i);
      if (g >= 0) {
        const PinState p = adc.readPin(kThermPins[g]);
        s.pinChanged = !s.pinRead || !p.same(s.pin);
        s.pin = p;
        s.pinRead = true;
      }
      Reading r;
      const Err e = adc.convert(kChannels[i], r);
      if (e == Err::none) {
        s.last = r;
        s.pinHeld = g >= 0 && !s.pin.clear();
        s.valid = !s.pinHeld;
        s.atUs = adc.bus.io.nowUs(adc.bus.io.ctx);
        if (s.pinHeld) ++s.held;
        else ++s.ok;
        continue;
      }
      s.enabled = s.valid = false;
      s.err = e;
      s.last = r;
      events[eventCount++] = {i, e};
      if (isGlobal(e)) {
        stopAll(e);
        break;
      }
    }
    ++rounds;
  }
};

}  // namespace pianothermal::adc
#endif
