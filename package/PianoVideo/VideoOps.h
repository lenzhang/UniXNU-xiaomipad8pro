// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo levels 2-4 (the ladder's V1-V3): the one path every register access of the bring-up takes. Portable (host tests in
// tests/bringup_host.cpp): the owner's Hw does the access itself; this layer
//   - logs every register write once: the first time an (address, value) pair is written, one line "write <block>+<offset> = <value>
//     (<what>)"; later writes of the same pair (the next power cycle, a retry loop) are only counted, so twenty power cycles log the
//     sequence of the first one and a count;
//   - does the read-modify-writes the way Linux's regmap_update_bits does (no write when nothing changes);
//   - bounds every wait: a poll reads at most timeout / step + 1 times, with the owner's delay between reads, and reports the last
//     value it read; nothing here waits without a bound.
// The Hw (a duck type; tests/bringup_host.cpp has the model, PianoVideo.cpp the kernel's):
//   uint32_t rd(uint64_t pa); void wr(uint64_t pa, uint32_t v); void delayUs(uint32_t us); void sleepMs(uint32_t ms);
//   uint64_t nowUs(); void note(const char *fmt, ...) (one log line, printf format); void stage(int step, const char *what).
#pragma once
#include <stdint.h>
#include "VideoHw.h"

namespace pianovideo {

// The blocks the bring-up writes, for the log.
struct Block {
  uint64_t base, bytes;
  const char *name;
};
constexpr Block kBlocks[] = {
    {kGcc, 0x200000, "gcc"},          {kVideoCc, kVideoCcBytes, "videocc"}, {kIris, kIrisBytes, "iris"},
    {kSmmu, kSmmuBytes, "smmu"},      {kRscDrv2, kRscDrv2Bytes, "rsc"},
};
inline const char *blockOf(uint64_t pa, uint32_t *offset)
{
  for (const Block &b : kBlocks)
    if (pa >= b.base && pa < b.base + b.bytes) {
      *offset = uint32_t(pa - b.base);
      return b.name;
    }
  *offset = uint32_t(pa);
  return "pa";
}

// Each (address, value) pair written, the first time (linear: a bring-up writes a few hundred distinct pairs).
constexpr unsigned kWriteLogSlots = 768;
struct WriteLog {
  uint64_t pa[kWriteLogSlots];
  uint32_t value[kWriteLogSlots];
  unsigned used = 0;
  uint32_t writes = 0, logged = 0, repeats = 0, overflow = 0;
  // True when the pair is new (and remembers it); a full table logs every further new pair (counted as overflow).
  bool first(uint64_t a, uint32_t v)
  {
    for (unsigned i = 0; i < used; ++i)
      if (pa[i] == a && value[i] == v) return false;
    if (used < kWriteLogSlots) {
      pa[used] = a;
      value[used++] = v;
    } else {
      ++overflow;
    }
    return true;
  }
};

template <class Hw>
class Ops {
 public:
  Ops(Hw &hw, WriteLog &log) : hw(hw), log(log) {}
  Hw &hw;
  WriteLog &log;
  uint32_t reads = 0, polls = 0, timeouts = 0;

  uint32_t rd(uint64_t pa)
  {
    ++reads;
    return hw.rd(pa);
  }
  void wr(uint64_t pa, uint32_t v, const char *what)
  {
    ++log.writes;
    if (log.first(pa, v)) {
      ++log.logged;
      uint32_t off = 0;
      const char *b = blockOf(pa, &off);
      hw.note("write %s+%#x = %08x (%s)", b, off, v, what);
    } else {
      ++log.repeats;
    }
    hw.wr(pa, v);
  }
  // regmap_update_bits: (old & ~mask) | value, written only when it changes. Returns the value now there.
  uint32_t update(uint64_t pa, uint32_t mask, uint32_t value, const char *what)
  {
    const uint32_t old = rd(pa), want = (old & ~mask) | (value & mask);
    if (want != old) wr(pa, want, what);
    return want;
  }
  uint32_t set(uint64_t pa, uint32_t bits, const char *what) { return update(pa, bits, bits, what); }
  uint32_t clear(uint64_t pa, uint32_t bits, const char *what) { return update(pa, bits, 0, what); }

  // Reads pa until ok(value), at most timeoutUs / stepUs + 1 times with stepUs between reads. *last gets the last value read.
  template <class Ok>
  bool pollFor(uint64_t pa, Ok ok, uint32_t timeoutUs, uint32_t stepUs, uint32_t *last = nullptr)
  {
    ++polls;
    const uint32_t tries = stepUs ? timeoutUs / stepUs : 0;
    uint32_t v = 0;
    for (uint32_t i = 0;; ++i) {
      v = rd(pa);
      if (ok(v)) {
        if (last) *last = v;
        return true;
      }
      if (i >= tries) break;
      hw.delayUs(stepUs);
    }
    ++timeouts;
    if (last) *last = v;
    return false;
  }
  bool poll(uint64_t pa, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t stepUs, uint32_t *last = nullptr)
  {
    return pollFor(pa, [mask, want](uint32_t v) { return (v & mask) == want; }, timeoutUs, stepUs, last);
  }
};

// The steps of levels 2-4 (the stage word and the log; level 1 uses 1-6).
enum Step : int {
  kStepRails = 10, kStepBus = 11, kStepGcc = 12, kStepCcInit = 13, kStepCtrlGdsc = 14, kStepCtrlClocks = 15, kStepCodecGdsc = 16,
  kStepCodecClocks = 17, kStepPreset = 18, kStepReadout = 19, kStepCodecOff = 20, kStepCtrlOff = 21, kStepOffCheck = 22,
  kStepVotesOff = 23,
  kStepFwFile = 30, kStepFwPin = 31, kStepFwParse = 32, kStepFwMeta = 33, kStepPasInit = 34, kStepPasMemSetup = 35,
  kStepPasSegments = 36, kStepPasAuth = 37, kStepMpVideoVar = 38, kStepPasShutdown = 39,
  kStepSmmuPlan = 40, kStepSmmuTables = 41, kStepSmmuRoute = 42, kStepQueues = 43, kStepBoot = 44, kStepCtrlStatus = 45,
  kStepHwMode = 46, kStepHfiSend = 47, kStepHfiWait = 48, kStepIdle = 49, kStepPcPrep = 50, kStepRemoteSuspend = 51,
  kStepCollapsed = 52, kStepResume = 53, kStepRemoteResume = 54, kStepReboot = 55, kStepPing = 56, kStepUnload = 57,
  kStepSmmuTeardown = 58, kStepDone = 59,
};

enum Error : int {
  kOk = 0, kErrStopped = -1, kErrArgument = -2, kErrRpmh = -3, kErrGcc = -4, kErrPll = -5, kErrRcg = -6, kErrDivider = -7,
  kErrBranch = -8, kErrGdsc = -9, kErrReadout = -10, kErrOffCheck = -11, kErrFwFile = -12, kErrFwPin = -13, kErrFwImage = -14,
  kErrNoMemory = -15, kErrScm = -16, kErrSmmu = -17, kErrBoot = -18, kErrHfi = -19, kErrHfiTimeout = -20, kErrSysError = -21,
  kErrPc = -22, kErrFault = -23,
};
inline const char *errorName(int e)
{
  switch (e) {
  case kOk: return "ok";
  case kErrStopped: return "stopped here on request (piano-video-stop)";
  case kErrArgument: return "bad arguments";
  case kErrRpmh: return "an RPMh vote was not sent or not completed";
  case kErrGcc: return "a GCC video branch did not start";
  case kErrPll: return "PLL0 did not lock";
  case kErrRcg: return "the core clock's RCG did not take its configuration";
  case kErrDivider: return "the mvs0 divider does not give the 240 MHz OPP";
  case kErrBranch: return "a VIDEO_CC branch did not start";
  case kErrGdsc: return "a GDSC did not power up";
  case kErrReadout: return "the powered core's words are not sane";
  case kErrOffCheck: return "a GDSC is still on after the power-off";
  case kErrFwFile: return "the firmware file could not be read from the ESP";
  case kErrFwPin: return "the firmware file is not the pinned vpu35_4v.mbn";
  case kErrFwImage: return "the firmware image does not parse or does not fit the carveout";
  case kErrNoMemory: return "no memory";
  case kErrScm: return "a secure call failed";
  case kErrSmmu: return "the SMMU bank could not be set up";
  case kErrBoot: return "the firmware did not start its HFI (CTRL_STATUS)";
  case kErrHfi: return "a malformed or failed HFI answer";
  case kErrHfiTimeout: return "no HFI answer within 1 s";
  case kErrSysError: return "the firmware reported a system error";
  case kErrPc: return "the firmware did not get ready for power collapse";
  case kErrFault: return "an SMMU fault on the video streams";
  }
  return "?";
}

}  // namespace pianovideo
