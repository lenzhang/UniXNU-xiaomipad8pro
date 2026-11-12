// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only access to piano's PMICs through the SPMI PMIC arbiter v7 of SM8750 (DT spmi@c400000, qcom,ee = 0), for EE 0 (the
// application processor):
//   core  0x0C400000: version at +0, peripheral count at +4 (bits 10:0), the APID map at +0x2000 (4 bytes an APID, PPID in 19:8);
//   cnfg  0x0C42D000: the owner table, 4 bytes an APID, owner EE in bits 2:0;
//   obsv  0x0C440000 + 0x20 * APID: EE 0's observer (read) channel: command +0x00, status +0x08, read data +0x18.
// Command = opcode << 27 | register << 4 | (bytes - 1), opcode 1 = extended read; status bit 0 done, bits 1-3 failure, denied,
// dropped. Nothing here writes a PMIC register: the only store is the observer channel's command.
// The same reads as PianoDwc3's probeRepeater (package/PianoDwc3/PianoDwc3.cpp spmiApid/spmiRead), k27 0057's SDAM read and
// piano-uefi's KeypadDeviceLib (UPSTREAM); written as a portable core over an Io so tests/buttons_host.cpp can run it on a model.
#ifndef PIANO_INPUT_SPMI_OBSERVER_H
#define PIANO_INPUT_SPMI_OBSERVER_H
#include <stdint.h>
#include "../include/PianoSpmi.h"   // the SPMI lock and the transactions every kext shares

namespace pianoinput {

constexpr uint64_t kSpmiCore = 0x0c400000, kSpmiCnfg = 0x0c42d000, kSpmiObsv = 0x0c440000;
constexpr uint32_t kSpmiCoreBytes = 0x3000, kSpmiCnfgBytes = 0x1000, kSpmiObsvBytes = 0x8000, kSpmiMaxApids = 1024;
constexpr uint32_t kSpmiApidMap = 0x2000, kObsvStride = 0x20, kObsvCmd = 0x00, kObsvStatus = 0x08, kObsvData = 0x18;
constexpr uint32_t kOpExtReadL = 1, kStatusDone = 1u << 0, kStatusErrors = 0xeu;

enum class Window : uint8_t { core, cnfg, obsv };

// The mapped windows. rd/wr take a window and a byte offset into it; delayUs waits.
struct Io {
  void *ctx;
  uint32_t (*rd)(void *ctx, Window w, uint32_t offset);
  void (*wr)(void *ctx, Window w, uint32_t offset, uint32_t value);
  void (*delayUs)(void *ctx, unsigned us);
};

struct Arbiter {
  uint32_t version = 0, count = 0;
  bool v7 = false;
};

inline Arbiter spmiArbiter(const Io &io)
{
  Arbiter a;
  a.version = io.rd(io.ctx, Window::core, 0);
  a.count = io.rd(io.ctx, Window::core, 4) & 0x7ffu;
  a.v7 = a.version >= 0x70000000u && a.version < 0x80000000u && a.count && a.count <= kSpmiMaxApids;
  return a;
}

struct Apid {
  int apid = -1;       // -1: the PPID is not in the map
  uint32_t owner = 0;  // the owner EE of the chosen entry
};

// The APID of a PPID (SID << 8 | peripheral base >> 8); an entry owned by EE 0 wins over another one.
inline Apid spmiFindApid(const Io &io, const Arbiter &a, uint16_t ppid)
{
  Apid found;
  for (uint32_t i = 0; i < a.count; ++i) {
    const uint32_t map = io.rd(io.ctx, Window::core, kSpmiApidMap + 4 * i);
    if (!map || ((map >> 8) & 0xfffu) != ppid) continue;
    const uint32_t owner = io.rd(io.ctx, Window::cnfg, 4 * i) & 7u;
    if (found.apid < 0 || (owner == 0 && found.owner != 0)) {
      found.apid = int(i);
      found.owner = owner;
    }
  }
  return found;
}

enum class ReadResult : uint8_t { ok, error, timeout };   // pianospmi::Result's values

// One byte of register `reg` of the peripheral at `apid`; *status gets the channel's last status word. One try (the poll loop reads
// again at its next sample). The transaction is package/include/PianoSpmi.h's, under the SPMI lock every kext shares.
inline ReadResult spmiRead8(const Io &io, int apid, uint8_t reg, uint8_t *value, uint32_t *status = nullptr)
{
  if (apid < 0 || uint32_t(apid) >= kSpmiMaxApids) return ReadResult::error;
  const uint32_t ch = kObsvStride * uint32_t(apid);
  return ReadResult(uint8_t(pianospmi::readOnce([&](uint32_t off) { return io.rd(io.ctx, Window::obsv, ch + off); },
                                                [&](uint32_t off, uint32_t v) { io.wr(io.ctx, Window::obsv, ch + off, v); },
                                                [&](unsigned us) { io.delayUs(io.ctx, us); }, reg, value, 1, status)));
}

}  // namespace pianoinput
#endif
