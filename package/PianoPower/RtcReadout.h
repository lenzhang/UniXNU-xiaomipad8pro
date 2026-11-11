// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoPower's readout of the PMIC RTC (PmicRtc.h, docs/drivers/rtc.md) over the SPMI observer channels of SpmiObserver.h: read
// only, like the rest of level 1. The offset is the boot argument piano-rtc-offset when given, else the SDAM cell; the same rule
// as the kernel's calendar (k27 0060), so the two report the same date.
#ifndef PIANO_POWER_RTC_READOUT_H
#define PIANO_POWER_RTC_READOUT_H
#include "SpmiObserver.h"
#include "PmicRtc.h"

namespace pianopower {

enum class RtcOffsetSource : uint8_t { none, arg, sdam };

struct RtcReading {
  bool ok = false;           // the count was read
  const char *why = "";      // when not ok
  Apid rtc, sdam;
  uint8_t type = 0, subtype = 0, enCtl = 0;
  bool enabled = false;      // EN_CTL bit 7: the counter runs
  uint32_t count = 0;
  bool sdamOk = false;
  uint32_t sdamOffset = 0;   // the SDAM cell as read
  RtcOffsetSource source = RtcOffsetSource::none;
  uint32_t offset = 0;       // the one used
  uint32_t utc = 0;          // count + offset
  bool dated = false;        // utc is in [2026, 2100)
};

struct RtcChannel {
  const Io *io;
  int apid;
};

inline int rtcSpmiRead(void *ctx, uint8_t reg, uint8_t *value, unsigned n)
{
  const RtcChannel *c = static_cast<const RtcChannel *>(ctx);
  return spmiRead(*c->io, c->apid, reg, value, n) == ReadResult::ok ? 0 : -1;
}

inline const char *rtcSourceName(RtcOffsetSource s)
{
  return s == RtcOffsetSource::arg ? "piano-rtc-offset" : s == RtcOffsetSource::sdam ? "the SDAM cell" : "none";
}

// Finds the two peripherals (once: pass the previous reading to skip the APID scan) and reads them.
inline RtcReading readRtc(const Io &io, const Arbiter &a, bool hasArg, uint32_t argOffset, const RtcReading *previous = nullptr)
{
  RtcReading r;
  if (previous && previous->rtc.apid >= 0) {
    r.rtc = previous->rtc;
    r.sdam = previous->sdam;
    r.type = previous->type;
    r.subtype = previous->subtype;
  } else {
    r.rtc = spmiFindApid(io, a, PIANO_RTC_PPID);
    r.sdam = spmiFindApid(io, a, PIANO_RTC_SDAM_PPID);
    uint8_t id[2] = {};
    if (r.rtc.apid >= 0 && spmiRead(io, r.rtc.apid, PIANO_RTC_TYPE, id, 2) == ReadResult::ok) {
      r.type = id[0];
      r.subtype = id[1];
    }
  }
  if (r.rtc.apid < 0) {
    r.why = "RTC_RW (PPID 0x061) is not in the arbiter's map";
    return r;
  }
  RtcChannel rtc = {&io, r.rtc.apid};
  if (rtcSpmiRead(&rtc, PIANO_RTC_EN_CTL, &r.enCtl, 1) != 0) {
    r.why = "EN_CTL read failed";
    return r;
  }
  r.enabled = (r.enCtl & PIANO_RTC_ENABLED) != 0;
  if (piano_rtc_read_count(rtcSpmiRead, &rtc, &r.count) != 0) {
    r.why = "RDATA read failed";
    return r;
  }
  r.ok = true;
  uint8_t cell[4] = {};
  if (r.sdam.apid >= 0 && spmiRead(io, r.sdam.apid, PIANO_RTC_SDAM_OFFSET, cell, 4) == ReadResult::ok) {
    r.sdamOk = true;
    r.sdamOffset = piano_rtc_le32(cell);
  }
  if (hasArg) {
    r.source = RtcOffsetSource::arg;
    r.offset = argOffset;
  } else if (r.sdamOk) {
    r.source = RtcOffsetSource::sdam;
    r.offset = r.sdamOffset;
  }
  if (r.source != RtcOffsetSource::none) {
    r.utc = piano_rtc_utc(r.count, r.offset);
    r.dated = piano_rtc_plausible(r.utc) != 0;
  }
  return r;
}

}  // namespace pianopower
#endif
