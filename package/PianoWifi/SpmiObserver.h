// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi's copy of package/PianoPower/SpmiObserver.h (same repository; only the namespace and include guard changed, see UPSTREAM).
// Read-only access to piano's PMICs through the SPMI PMIC arbiter v7 of SM8750 (DT spmi@c400000, qcom,ee = 0), for EE 0 (the
// application processor). The same windows and reads as package/PianoInput/SpmiObserver.h, plus reads of up to 8 bytes and the
// owner of an APID:
//   core  0x0C400000: version at +0, peripheral count at +4 (bits 10:0), the APID map at +0x2000 (4 bytes an APID, PPID in 19:8);
//   cnfg  0x0C42D000: the owner table, 4 bytes an APID, owner EE in bits 2:0;
//   obsv  0x0C440000 + 0x20 * APID: EE 0's observer (read) channel: command +0x00, status +0x08, read data +0x18 and +0x1c.
// Command = opcode << 27 | register << 4 | (bytes - 1), opcode 1 = extended read; status bit 0 done, bits 1-3 failure, denied,
// dropped. Nothing here writes a PMIC register: the only store is the observer channel's command. The observer channel reads any
// mapped peripheral; writing one needs the APID's owner EE, which for the charger is not ours (ChargerPmic.h logs it).
#ifndef PIANO_WIFI_SPMI_OBSERVER_H
#define PIANO_WIFI_SPMI_OBSERVER_H
#include <stdint.h>
#include "../include/PianoSpmi.h"   // the SPMI lock and the transactions every kext shares

namespace pianowifi {

constexpr uint64_t kSpmiCore = 0x0c400000, kSpmiCnfg = 0x0c42d000, kSpmiObsv = 0x0c440000;
constexpr uint32_t kSpmiCoreBytes = 0x3000, kSpmiCnfgBytes = 0x1000, kSpmiObsvBytes = 0x8000, kSpmiMaxApids = 1024;
constexpr uint32_t kSpmiApidMap = 0x2000, kObsvStride = 0x20, kObsvCmd = 0x00, kObsvStatus = 0x08, kObsvData0 = 0x18,
                   kObsvData1 = 0x1c;
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

// The map entry of APID i: 0 when unused; the PPID (SID << 8 | peripheral base >> 8) in bits 19:8.
inline uint32_t spmiMap(const Io &io, uint32_t apid) { return io.rd(io.ctx, Window::core, kSpmiApidMap + 4 * apid); }
inline uint16_t spmiPpid(uint32_t map) { return uint16_t((map >> 8) & 0xfffu); }
inline uint32_t spmiOwner(const Io &io, uint32_t apid) { return io.rd(io.ctx, Window::cnfg, 4 * apid) & 7u; }

struct Apid {
  int apid = -1;       // -1: the PPID is not in the map
  uint32_t owner = 0;  // the owner EE of the chosen entry
};

// The APID of a PPID; an entry owned by EE 0 wins over another one (observer reads need no ownership, either one reads).
inline Apid spmiFindApid(const Io &io, const Arbiter &a, uint16_t ppid)
{
  Apid found;
  for (uint32_t i = 0; i < a.count; ++i) {
    const uint32_t map = spmiMap(io, i);
    if (!map || spmiPpid(map) != ppid) continue;
    const uint32_t owner = spmiOwner(io, i);
    if (found.apid < 0 || (owner == 0 && found.owner != 0)) {
      found.apid = int(i);
      found.owner = owner;
    }
  }
  return found;
}

enum class ReadResult : uint8_t { ok, error, timeout };   // pianospmi::Result's values
inline ReadResult fromShared(pianospmi::Result r) { return ReadResult(uint8_t(r)); }

// One observer command: bytes (1..8) registers from `reg` of the peripheral at `apid`; *status gets the channel's last status word.
// The transaction itself is package/include/PianoSpmi.h's, under the SPMI lock every kext shares (one transaction at a time on the
// arbiter: the 2026-11-12/13 probes lost reads, and read wrong values with an "ok" status, while several kexts drove it at once).
inline ReadResult spmiReadOnce(const Io &io, int apid, uint8_t reg, uint8_t *value, unsigned bytes, uint32_t *status = nullptr)
{
  if (apid < 0 || uint32_t(apid) >= kSpmiMaxApids) return ReadResult::error;
  const uint32_t ch = kObsvStride * uint32_t(apid);
  return fromShared(pianospmi::readOnce([&](uint32_t off) { return io.rd(io.ctx, Window::obsv, ch + off); },
                                        [&](uint32_t off, uint32_t v) { io.wr(io.ctx, Window::obsv, ch + off, v); },
                                        [&](unsigned us) { io.delayUs(io.ctx, us); }, reg, value, bytes, status));
}

// spmiReadOnce, again up to four more times 500 us apart when the channel reports a failure or does not finish in time (a denial is
// final): with the DSP starting (piano-power=2 piano-power-delay=0) and PianoThermal's ADC rounds on the bus, single observer reads
// came back failed (the 2026-11-12 23:03 probe: three of PianoWifi's regulator TYPE reads, so level 2 found no regulator and stopped
// at step -201). PianoSpmi.h's read(): the lock is free between the tries, and a command that timed out was given up to 5 ms more
// inside its hold, so a new command never overtakes it.
inline ReadResult spmiRead(const Io &io, int apid, uint8_t reg, uint8_t *value, unsigned bytes, uint32_t *status = nullptr)
{
  if (apid < 0 || uint32_t(apid) >= kSpmiMaxApids) return ReadResult::error;
  const uint32_t ch = kObsvStride * uint32_t(apid);
  return fromShared(pianospmi::read([&](uint32_t off) { return io.rd(io.ctx, Window::obsv, ch + off); },
                                    [&](uint32_t off, uint32_t v) { io.wr(io.ctx, Window::obsv, ch + off, v); },
                                    [&](unsigned us) { io.delayUs(io.ctx, us); }, reg, value, bytes, status));
}

// spmiRead until two reads in a row agree (PianoSpmi.h readStable): for values whose misreading is dangerous, as a peripheral's
// TYPE/SUBTYPE, which decides whose registers the next reads take for a regulator's (v34p: an LDO's TYPE read 0x00 "ok").
inline ReadResult spmiReadStable(const Io &io, int apid, uint8_t reg, uint8_t *value, unsigned bytes, uint32_t *status = nullptr)
{
  if (apid < 0 || uint32_t(apid) >= kSpmiMaxApids) return ReadResult::error;
  const uint32_t ch = kObsvStride * uint32_t(apid);
  return fromShared(pianospmi::readStable([&](uint32_t off) { return io.rd(io.ctx, Window::obsv, ch + off); },
                                          [&](uint32_t off, uint32_t v) { io.wr(io.ctx, Window::obsv, ch + off, v); },
                                          [&](unsigned us) { io.delayUs(io.ctx, us); }, reg, value, bytes, status));
}

inline const char *resultName(ReadResult r) { return r == ReadResult::ok ? "ok" : r == ReadResult::error ? "error" : "timeout"; }

}  // namespace pianowifi
#endif
