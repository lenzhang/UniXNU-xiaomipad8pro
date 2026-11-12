// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Camera Control Interface's master 1 (CCI0 on piano; the rear sensor's bus): reset, timing, and register reads and writes of
// a 16-bit-addressed device through the controller's command queues, polled (no interrupt is attached: the IRQ status word is
// read and cleared). Portable over two callbacks (tests/camera_host.cpp runs it against a CCI model with a sensor behind it).
//
// The protocol, from the specification (CameraHw.h "CCI"; Linux's i2c-qcom-cci.c read as facts, nothing copied):
//   init    the IRQ mask (master 1's done/report/error bits, the reset and halt acknowledgements), the reset of the whole
//           controller (RESET_CMD, RST_DONE within 100 ms), then master 1's five timing words
//   write   queue 0: SET_PARAM (7-bit address << 4), WRITE (count << 4) followed by the bytes packed four to a word after the
//           command byte, REPORT with its IRQ bit; EXEC_WORD_CNT := CUR_WORD_CNT; QUEUE_START bit (2 m + q); Q0_REPORT
//   read    first a write of the register address (queue 0), then queue 1: SET_PARAM, READ (count << 4); QUEUE_START; RD_DONE;
//           READ_BUF_LEVEL words (count / 4 + 1) from READ_DATA: the first byte is the address, then the data
//   errors  any of master 1's error bits (NACK on either queue among them): HALT_REQ for master 1, its HALT_ACK, then master 1's
//           reset (RESET_CMD m1 mask) and RST_DONE; the transfer fails. A queue that does not finish in 100 ms: the same.
#pragma once
#include <stdint.h>
#include "CameraHw.h"

namespace pianocamera {

enum class CciResult : uint8_t { ok, nack, error, timeout, resetTimeout, queueBusy, tooLong, notReady, badRead };
inline const char *cciName(CciResult r)
{
  switch (r) {
  case CciResult::ok: return "ok";
  case CciResult::nack: return "no ack";
  case CciResult::error: return "bus error";
  case CciResult::timeout: return "queue timeout";
  case CciResult::resetTimeout: return "reset timeout";
  case CciResult::queueBusy: return "queue not empty";
  case CciResult::tooLong: return "transfer too long";
  case CciResult::notReady: return "not initialised";
  case CciResult::badRead: return "read count mismatch";
  }
  return "?";
}

struct CciIo {
  void *ctx = nullptr;
  uint32_t (*rd)(void *ctx, uint32_t off) = nullptr;                          // CCI0 + off
  void (*wr)(void *ctx, uint32_t off, uint32_t value, const char *what) = nullptr;
  void (*udelay)(void *ctx, unsigned us) = nullptr;
};

class CciMaster {
 public:
  explicit CciMaster(const CciIo &io) : io_(io) {}
  uint32_t version = 0, lastStatus = 0;
  unsigned transfers = 0, failures = 0, resets = 0;
  bool ready = false;

  CciResult init(const CciTiming &t)
  {
    version = io_.rd(io_.ctx, kCciHwVersion);
    const unsigned m = kCciMaster;
    io_.wr(io_.ctx, kCciIrqMask, cciRdDone(m) | cciQReport(m, 0) | cciQReport(m, 1) | kCciRstDone | cciHaltAck(m) | cciErrors(m),
           "CCI IRQ mask: master 1 done/report/error, reset and halt acknowledgements");
    clearIrq(0xffffffffu);
    io_.wr(io_.ctx, kCciResetCmd, kCciResetAll, "CCI RESET_CMD: the whole controller");
    if (!waitBits(kCciRstDone)) return CciResult::resetTimeout;
    clearIrq(lastStatus);
    io_.wr(io_.ctx, cciSclCtl(m), uint32_t(t.thigh) << 16 | t.tlow, "CCI M1 SCL_CTL: thigh, tlow");
    io_.wr(io_.ctx, cciSdaCtl0(m), uint32_t(t.tsuSto) << 16 | t.tsuSta, "CCI M1 SDA_CTL_0: tsu_sto, tsu_sta");
    io_.wr(io_.ctx, cciSdaCtl1(m), uint32_t(t.thdDat) << 16 | t.thdSta, "CCI M1 SDA_CTL_1: thd_dat, thd_sta");
    io_.wr(io_.ctx, cciSdaCtl2(m), t.tbuf, "CCI M1 SDA_CTL_2: tbuf");
    io_.wr(io_.ctx, cciMiscCtl(m), uint32_t(t.stretch) << 8 | uint32_t(t.trdhld) << 4 | t.tsp, "CCI M1 MISC_CTL: stretch, trdhld, tsp");
    ready = true;
    return CciResult::ok;
  }

  // Writes `count` (1..kCciMaxWrite) bytes to the device at 7-bit `address` (a register write is its address bytes, then the value).
  CciResult write(uint8_t address, const uint8_t *bytes, unsigned count)
  {
    if (!ready) return CciResult::notReady;
    if (!count || count > kCciMaxWrite) return CciResult::tooLong;
    ++transfers;
    const unsigned m = kCciMaster, q = 0;
    if (io_.rd(io_.ctx, cciCurCount(m, q))) return fail(CciResult::queueBusy);
    load(q, kCciSetParam | uint32_t(address & 0x7f) << 4, "CCI M1 Q0: SET_PARAM address");
    uint8_t packed[12] = {uint8_t(kCciWrite | count << 4)};
    for (unsigned i = 0; i < count; ++i) packed[1 + i] = bytes[i];
    for (unsigned i = 0; i < count + 1; i += 4)
      load(q, uint32_t(packed[i]) | uint32_t(packed[i + 1]) << 8 | uint32_t(packed[i + 2]) << 16 | uint32_t(packed[i + 3]) << 24,
           "CCI M1 Q0: WRITE command and data");
    load(q, kCciReport | kCciReportIrq, "CCI M1 Q0: REPORT");
    return run(q, cciQReport(m, q));
  }

  // Reads `count` (1..kCciMaxRead) bytes at 7-bit `address` after writing the register address (regBytes 1 or 2, big-endian).
  CciResult readReg(uint8_t address, uint16_t reg, unsigned regBytes, uint8_t *out, unsigned count)
  {
    if (!count || count > kCciMaxRead || !regBytes || regBytes > 2) return CciResult::tooLong;
    const uint8_t a[2] = {uint8_t(regBytes == 2 ? reg >> 8 : reg), uint8_t(reg)};
    CciResult r = write(address, a, regBytes);
    if (r != CciResult::ok) return r;
    ++transfers;
    const unsigned m = kCciMaster, q = 1;
    if (io_.rd(io_.ctx, cciCurCount(m, q))) return fail(CciResult::queueBusy);
    load(q, kCciSetParam | uint32_t(address & 0x7f) << 4, "CCI M1 Q1: SET_PARAM address");
    load(q, kCciRead | count << 4, "CCI M1 Q1: READ count");
    r = run(q, cciRdDone(m));
    if (r != CciResult::ok) return r;
    const unsigned words = io_.rd(io_.ctx, cciReadLevel(m)), want = count / 4 + 1;
    if (words != want) {
      for (unsigned i = 0; i < words && i < 8; ++i) (void)io_.rd(io_.ctx, cciReadData(m));   // drain what is there
      return fail(CciResult::badRead);
    }
    unsigned at = 0;
    bool first = true;
    for (unsigned w = 0; w < words; ++w) {
      const uint32_t v = io_.rd(io_.ctx, cciReadData(m));
      for (unsigned b = 0; b < 4 && at < count; ++b) {
        if (first) {
          first = false;   // the address byte
          continue;
        }
        out[at++] = uint8_t(v >> (8 * b));
      }
    }
    return CciResult::ok;
  }

  // Leaves the controller quiet: master 1 halted (if it was running) and reset, the IRQ mask restored to `mask`.
  void quiesce(uint32_t mask)
  {
    io_.wr(io_.ctx, kCciIrqMask, mask, "CCI IRQ mask: as found");
    clearIrq(io_.rd(io_.ctx, kCciIrqStatus));
    ready = false;
  }

 private:
  void load(unsigned q, uint32_t word, const char *what) { io_.wr(io_.ctx, cciLoad(kCciMaster, q), word, what); }
  void clearIrq(uint32_t bits)
  {
    io_.wr(io_.ctx, kCciIrqClear, bits, "CCI IRQ clear");
    io_.wr(io_.ctx, kCciIrqGlobalClear, 1, "CCI IRQ global clear");
  }
  // Polls the IRQ status for any of `bits` (or master 1's errors), 10 us a try, 100 ms at most. lastStatus gets the word.
  bool waitBits(uint32_t bits)
  {
    for (unsigned waited = 0;; waited += 10) {
      lastStatus = io_.rd(io_.ctx, kCciIrqStatus);
      if (lastStatus & (bits | cciErrors(kCciMaster))) return (lastStatus & bits) && !(lastStatus & cciErrors(kCciMaster));
      if (waited >= kCciTimeoutMs * 1000) return false;
      io_.udelay(io_.ctx, 10);
    }
  }
  CciResult run(unsigned q, uint32_t doneBit)
  {
    const unsigned m = kCciMaster;
    io_.wr(io_.ctx, cciExecCount(m, q), io_.rd(io_.ctx, cciCurCount(m, q)), "CCI M1: EXEC_WORD_CNT := CUR_WORD_CNT");
    io_.wr(io_.ctx, kCciQueueStart, 1u << (2 * m + q), "CCI QUEUE_START");
    const bool done = waitBits(doneBit);
    const uint32_t status = lastStatus;
    clearIrq(status);
    if (done) return CciResult::ok;
    const CciResult why = (status & (cciNack(m, 0) | cciNack(m, 1))) ? CciResult::nack
                          : (status & cciErrors(m))                   ? CciResult::error
                                                                      : CciResult::timeout;
    recover();
    lastStatus = status;
    return fail(why);
  }
  // Master 1 halted, then reset (as Linux's interrupt handler and timeout path do).
  void recover()
  {
    ++resets;
    const unsigned m = kCciMaster;
    io_.wr(io_.ctx, kCciHaltReq, 1u << m, "CCI HALT_REQ: master 1");
    waitBits(cciHaltAck(m));
    clearIrq(lastStatus);
    io_.wr(io_.ctx, kCciResetCmd, kCciResetM1, "CCI RESET_CMD: master 1");
    waitBits(kCciRstDone);
    clearIrq(lastStatus);
  }
  CciResult fail(CciResult r)
  {
    ++failures;
    return r;
  }

  CciIo io_;
};

}  // namespace pianocamera
