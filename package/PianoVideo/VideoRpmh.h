// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo V1: the rails (MXC, MMCX) and the video bus (BCM MM1) by RPMh, from apps DRV-2, under PianoA830's DRV-2 owner word.
// Portable (host tests in tests/bringup_host.cpp against a cmd-db image and a DRV-2 model).
//
// - cmd-db: the resources by name in a snapshot of the AOP command database (the layout of Linux drivers/soc/qcom/cmd-db.c as
//   PianoA830's Sm8750Rpmh.c and PianoWifi's WifiSurvey.h read it: a 144-byte header with eight 16-byte slave records, 24-byte
//   entries, aux data from the data area): an ARC's level table (u16 vlvl per hlvl, zero padded) and a BCM's address.
// - corners: the first hlvl whose vlvl reaches the request (Linux rpmhpd_set_performance_state, PianoA830's Sm8750RpmhArcLevel).
// - one transaction: PianoA830's Sm8750RpmhWriteActive protocol (Sm8750Rpmh.h, MIT; re-expressed here, nothing linked): the DRV-2
//   lock word taken first (10 us steps, at most 2 s: the owner's drv2Take), DRV-2's version (layout v2 or v3) and TCS/command counts
//   checked, the first idle ACTIVE TCS (CONTROL, CMD_ENABLE and WAIT_FOR_CMPL 0, its IRQ_STATUS bit clear, STATUS 1; one that never
//   completed earlier is never used again), its IRQ_ENABLE bit masked, MSGID/ADDR/DATA per command, WAIT_FOR_CMPL, CMD_ENABLE, AMC
//   mode, every write read back (100 x 1 us); the trigger; completion polled every 1 ms for at most 100 ms (STATUS 1 and every
//   CMD_STATUS issued + completed); the TCS put back (CONTROL, CMD_ENABLE, WAIT_FOR_CMPL 0, IRQ_CLEAR, IRQ_ENABLE as it was). A TCS
//   that does not complete is left triggered with its interrupt masked, as evidence, and marked stuck.
// - the votes this driver holds (docs/drivers/video.md section 12.1): MXC at svs and MMCX at max(low_svs, the display's floor)
//   while the core is powered; MXC back to 0 at power-off, MMCX never below the floor (the boot framebuffer's display needs NOM_L1
//   and a DRV-2 vote replaces the DRV's earlier one); MM1 on while powered, off after. Only what changes is sent. Since 2026-11-12
//   these needs go through the shared multimedia-vote helper (package/include/PianoMmVotes.h): the vote is the maximum of every
//   client's need (PianoCamera's level 3 is the other client) with the highest floor any client declares; alone, the same commands.
// The Hw adds to VideoOps.h's: bool drv2Take(uint32_t *waits) (the owner word; false after 2 s), void drv2Give(), and the shared
// votes' words: volatile uint32_t *mmWords(), volatile uint32_t *mmLock().
#pragma once
#include <stdint.h>
#include <PianoMmVotes.h>
#include "VideoOps.h"

namespace pianovideo {
namespace rpmh {

// --------------------------------------------------------------------------------------------------------------------- cmd-db
constexpr uint32_t kMagic = 0x0c0330db, kDataOffset = 144, kEntryBytes = 24, kSlaves = 8;
constexpr unsigned kSlaveArc = 3, kSlaveBcm = 5, kMaxLevels = 16;
enum : int { kFound = 0, kNotCmdDb = -1, kNotFound = -2, kCorrupt = -3 };

inline uint32_t le16(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

struct Entry {
  uint32_t addr = 0, slave = 0, auxOffset = 0, auxLen = 0;
};
// A unique entry of that name (1..8 characters, zero padded) in a slave of that type: its address carries the type in 31:16.
inline int find(const uint8_t *db, uint32_t bytes, const char *name, unsigned slave, Entry *out)
{
  if (!db || bytes < kDataOffset || le32(db + 4) != kMagic) return kNotCmdDb;
  uint8_t key[8] = {};
  unsigned length = 0;
  while (length < 8 && name[length]) {
    key[length] = uint8_t(name[length]);
    ++length;
  }
  if (!length || name[length]) return kNotFound;
  unsigned matches = 0;
  Entry e;
  for (unsigned s = 0; s < kSlaves; ++s) {
    const uint8_t *h = db + 8 + 16 * s;
    const uint32_t id = le16(h), table = kDataOffset + le16(h + 2), data = kDataOffset + le16(h + 4), count = le16(h + 6);
    if (!id) break;
    if (table > bytes || count > (bytes - table) / kEntryBytes || data > bytes) return kCorrupt;
    for (uint32_t n = 0; n < count; ++n) {
      const uint8_t *p = db + table + kEntryBytes * n;
      bool same = true;
      for (unsigned i = 0; i < 8 && same; ++i) same = p[i] == key[i];
      if (!same) continue;
      ++matches;
      e.slave = id;
      e.addr = le32(p + 16);
      e.auxLen = le16(p + 20);
      e.auxOffset = data + le16(p + 22);
    }
  }
  if (!matches) return kNotFound;
  if (matches != 1 || e.slave != slave || (e.addr >> 16) != slave || (e.addr & 3) || e.auxOffset > bytes || e.auxLen > bytes - e.auxOffset)
    return kCorrupt;
  *out = e;
  return kFound;
}

struct Arc {
  uint32_t addr = 0;
  uint16_t vlvl[kMaxLevels] = {};
  unsigned count = 0;   // usable levels (up to the zero padding)
};
inline int arc(const uint8_t *db, uint32_t bytes, const char *name, Arc *a)
{
  Entry e;
  const int rc = find(db, bytes, name, kSlaveArc, &e);
  if (rc) return rc;
  const uint32_t entries = e.auxLen / 2;
  if (!entries || entries > kMaxLevels) return kCorrupt;
  Arc out;
  out.addr = e.addr;
  for (uint32_t i = 0; i < entries; ++i) out.vlvl[i] = uint16_t(le16(db + e.auxOffset + 2 * i));
  out.count = 1;
  while (out.count < entries && out.vlvl[out.count]) ++out.count;   // hlvl 0 may be 0 (off); later zeros are padding
  if (out.count < 2) return kCorrupt;                                 // a dummy ARC: nothing to vote
  *a = out;
  return kFound;
}
inline int bcm(const uint8_t *db, uint32_t bytes, const char *name, uint32_t *addr)
{
  Entry e;
  const int rc = find(db, bytes, name, kSlaveBcm, &e);
  if (rc) return rc;
  *addr = e.addr;
  return kFound;
}
// The first hlvl whose vlvl reaches the request; false when every level is lower.
inline bool level(const Arc &a, uint32_t vlvl, uint32_t *hlvl)
{
  for (uint32_t h = 0; h < a.count; ++h)
    if (a.vlvl[h] >= vlvl) {
      *hlvl = h;
      return true;
    }
  return false;
}

// ------------------------------------------------------------------------------------------------------------ one transaction
constexpr uint32_t kRscId = 0x00, kRscConfig = 0x0c, kIrqEnable = kRscTcsBlock, kIrqStatus = kRscTcsBlock + 4, kIrqClear = kRscTcsBlock + 8;
constexpr unsigned kDrvId = 2;   // DRV_PRNT_CHLD_CONFIG: NTCS of DRV n at [6n+5:6n], NCMD [31:27]
constexpr uint32_t kMsgIdWrite = 0x00010108, kCmdComplete = 0x00010100, kAmcEnable = 1u << 16, kAmcTrigger = 1u << 24;
constexpr unsigned kMaxCommands = 4, kReadbackTries = 100, kCompletePolls = 100;
struct Layout {
  uint32_t wait, control, status, enable, msgid, stride;
};
inline Layout layoutFor(uint32_t id)   // Linux rpmh_rsc_reg_offset_ver_2_7 / _ver_3_0
{
  return ((id >> 16) & 0xffu) == 2 ? Layout{0x10, 0x14, 0x18, 0x1c, 0x30, 20} : Layout{0x20, 0x24, 0x28, 0x2c, 0x34, 24};
}
struct Cmd {
  uint32_t addr, data;
};
enum TcsStep : uint8_t { kTcsNone, kTcsLock, kTcsHardware, kTcsSelect, kTcsProgram, kTcsPoll, kTcsCleanup, kTcsDone };
struct TcsResult {
  uint32_t id = 0, config = 0, tcs = ~0u, status = 0, busy = 0, polls = 0, lockWaits = 0, irqEnable = 0;
  uint32_t cmdStatus[kMaxCommands] = {};
  uint32_t badReg = 0, badValue = 0;   // a write that did not read back
  TcsStep step = kTcsNone;
};

template <class O>
bool wrChecked(O &ops, uint32_t reg, uint32_t value, const char *what, TcsResult *r)
{
  ops.wr(kRscDrv2 + reg, value, what);
  uint32_t seen = 0;
  for (unsigned i = 0; i < kReadbackTries; ++i) {
    if ((seen = ops.rd(kRscDrv2 + reg)) == value) return true;
    ops.hw.delayUs(1);
  }
  r->badReg = reg;
  r->badValue = seen;
  return false;
}
inline bool commandOk(const Cmd &c)
{
  if (!c.addr || (c.addr & 3)) return false;
  switch (c.addr >> 16) {
  case kSlaveArc: return c.data <= 0xff;
  case kSlaveBcm: return !(c.data & 0x90000000u);
  default: return false;
  }
}

// Holding the DRV-2 lock: one request of n commands through an idle ACTIVE TCS. 0, or kErrRpmh with r->step where it stopped.
template <class O>
int transactLocked(O &ops, const Cmd *cmds, unsigned n, uint32_t waitMask, bool stuck[kRscActiveTcs], TcsResult *r)
{
  r->step = kTcsHardware;
  r->id = ops.rd(kRscDrv2 + kRscId);
  r->config = ops.rd(kRscDrv2 + kRscConfig);
  const uint32_t major = (r->id >> 16) & 0xffu, ntcs = (r->config >> (6 * kDrvId)) & 0x3fu, ncmd = (r->config >> 27) & 0x1fu;
  if ((major != 2 && major != 3) || ntcs < kRscActiveTcs || ntcs > 16 || ncmd < 3 || ncmd > 16 || n > ncmd) return kErrRpmh;
  const Layout l = layoutFor(r->id);
  r->step = kTcsSelect;
  unsigned slot = kRscActiveTcs;
  for (unsigned t = 0; t < kRscActiveTcs && slot == kRscActiveTcs; ++t) {
    const uint32_t b = kRscTcsBlock + t * kRscTcsStride;
    if (stuck[t] || ops.rd(kRscDrv2 + b + l.control) || ops.rd(kRscDrv2 + b + l.enable) || ops.rd(kRscDrv2 + b + l.wait) ||
        (ops.rd(kRscDrv2 + kIrqStatus) & (1u << t)) || ops.rd(kRscDrv2 + b + l.status) != 1) {
      r->busy |= 1u << t;
      continue;
    }
    slot = t;
  }
  if (slot == kRscActiveTcs) return kErrRpmh;
  r->tcs = slot;
  const uint32_t base = kRscTcsBlock + slot * kRscTcsStride, bit = 1u << slot, all = (1u << n) - 1;
  r->step = kTcsProgram;
  const uint32_t irq = r->irqEnable = ops.rd(kRscDrv2 + kIrqEnable);
  bool ok = wrChecked(ops, kIrqEnable, irq & ~bit, "rsc IRQ_ENABLE: this TCS masked", r);
  for (unsigned i = 0; ok && i < n; ++i) {
    const uint32_t at = base + l.msgid + i * l.stride;
    ok = wrChecked(ops, at, kMsgIdWrite, "rsc MSGID: write, response required", r) &&
         wrChecked(ops, at + 4, cmds[i].addr, "rsc ADDR", r) && wrChecked(ops, at + 8, cmds[i].data, "rsc DATA", r);
  }
  ok = ok && wrChecked(ops, base + l.wait, waitMask, "rsc WAIT_FOR_CMPL", r) && wrChecked(ops, base + l.enable, all, "rsc CMD_ENABLE", r) &&
       wrChecked(ops, base + l.control, kAmcEnable, "rsc CONTROL: AMC mode", r);
  int rc = ok ? kOk : kErrRpmh;
  if (ok) {
    ops.wr(kRscDrv2 + base + l.control, kAmcEnable | kAmcTrigger, "rsc CONTROL: AMC mode + trigger");
    r->step = kTcsPoll;
    rc = kErrRpmh;
    for (unsigned p = 0; p < kCompletePolls; ++p) {
      ops.hw.sleepMs(1);
      r->polls = p + 1;
      r->status = ops.rd(kRscDrv2 + base + l.status);
      bool done = r->status == 1;
      for (unsigned i = 0; i < n; ++i) {
        const uint32_t v = ops.rd(kRscDrv2 + base + l.msgid + 12 + i * l.stride);
        r->cmdStatus[i] = v;
        done = done && v != 0xffffffffu && (v & kCmdComplete) == kCmdComplete;
      }
      if (done) {
        rc = kOk;
        break;
      }
    }
    if (rc) {   // still the hardware's: left triggered with its interrupt masked, never used again
      stuck[slot] = true;
      return rc;
    }
  }
  r->step = kTcsCleanup;
  if (!wrChecked(ops, base + l.control, kAmcEnable, "rsc CONTROL: trigger off", r) ||
      !wrChecked(ops, base + l.control, 0, "rsc CONTROL: AMC mode off", r) || !wrChecked(ops, base + l.enable, 0, "rsc CMD_ENABLE: none", r) ||
      !wrChecked(ops, base + l.wait, 0, "rsc WAIT_FOR_CMPL: none", r))
    return kErrRpmh;
  ops.wr(kRscDrv2 + kIrqClear, bit, "rsc IRQ_CLEAR: this TCS");
  const uint32_t now = ops.rd(kRscDrv2 + kIrqEnable);
  if (!wrChecked(ops, kIrqEnable, (now & ~bit) | (irq & bit), "rsc IRQ_ENABLE: as it was", r)) return kErrRpmh;
  if (rc == kOk) r->step = kTcsDone;
  return rc;
}

template <class O>
int send(O &ops, const Cmd *cmds, unsigned n, uint32_t waitMask, bool stuck[kRscActiveTcs], TcsResult *r)
{
  *r = TcsResult{};
  if (!n || n > kMaxCommands || !waitMask || (waitMask & ~((1u << n) - 1))) return kErrArgument;
  for (unsigned i = 0; i < n; ++i)
    if (!commandOk(cmds[i])) return kErrArgument;
  r->step = kTcsLock;
  if (!ops.hw.drv2Take(&r->lockWaits)) return kErrRpmh;
  const int rc = transactLocked(ops, cmds, n, waitMask, stuck, r);
  ops.hw.drv2Give();
  return rc;
}

// -------------------------------------------------------------------------------------------------------- the video's votes
struct Resources {
  Arc mxc, mmcx;
  uint32_t mm1 = 0;
  int error = kNotCmdDb;
  const char *missing = nullptr;
};
inline Resources resources(const uint8_t *db, uint32_t bytes)
{
  Resources r;
  int rc = arc(db, bytes, kArcMxc, &r.mxc);
  if (rc) r.missing = kArcMxc;
  if (!rc && (rc = arc(db, bytes, kArcMmcx, &r.mmcx))) r.missing = kArcMmcx;
  if (!rc && (rc = bcm(db, bytes, kBcmMm1, &r.mm1))) r.missing = kBcmMm1;
  if (!rc && (r.mxc.addr == r.mmcx.addr || r.mm1 == r.mxc.addr || r.mm1 == r.mmcx.addr)) {
    rc = kCorrupt;
    r.missing = "distinct addresses";
  }
  r.error = rc;
  return r;
}

// The votes go through the shared multimedia-vote helper (package/include/PianoMmVotes.h, PianoA830's words): the video core is
// one client; PianoCamera's level 3 is another; the display's floor is the highest MMCX floor any client declares. With PianoVideo
// alone the commands are exactly those this file sent before the helper (railsUp: MXC svs and MMCX max(low_svs, floor) in one TCS,
// only what changes; railsDown: MXC 0, MMCX to the floor, only what changes and only for what was sent; bus: MM1 on/off when it
// changes); with another client the maximum of the needs is voted, so the video core's release never drops the camera's need.
// The Hw adds: volatile uint32_t *mmWords() (piano_mm_votes), volatile uint32_t *mmLock() (piano_mm_votes_owner).
struct Votes {
  uint32_t floorVlvl = kMmcxDisplayFloor;   // MMCX is never voted or lowered below this (piano-video-mmcx-floor)
  bool mm1On = false;                       // the video core's own MM1 need
  bool stuck[kRscActiveTcs] = {};
  uint32_t requests = 0, unchanged = 0;
  TcsResult last;
  PianoMmVotes mm{};                        // this client's view of the shared votes (its counters persist)
};
static_assert(kDrv2Tag == PianoMmTagVideo, "the shared votes' video tag is PianoVideo's DRV-2 tag");
static_assert(kBcmMm1On == PIANO_MM_BCM_ON && kBcmMm1Off == PIANO_MM_BCM_OFF, "MM1's command words");

template <class O>
struct MmContext {
  O *ops;
  const Resources *res;
  Votes *v;
  static uint32_t address(void *c, unsigned r)
  {
    const Resources &x = *static_cast<MmContext *>(c)->res;
    return r == PIANO_MM_MXC ? x.mxc.addr : r == PIANO_MM_MMCX ? x.mmcx.addr : r == PIANO_MM_MM1 ? x.mm1 : 0;
  }
  static int arcLevel(void *c, unsigned r, uint32_t vlvl, uint32_t *hlvl)
  {
    const Resources &x = *static_cast<MmContext *>(c)->res;
    if (r != PIANO_MM_MXC && r != PIANO_MM_MMCX) return -3;
    return level(r == PIANO_MM_MXC ? x.mxc : x.mmcx, vlvl, hlvl) ? 0 : -3;
  }
  static int sendCmds(void *c, const PianoMmCmd *cmds, unsigned n)
  {
    MmContext &k = *static_cast<MmContext *>(c);
    Cmd out[PIANO_MM_RESOURCES];
    for (unsigned i = 0; i < n && i < PIANO_MM_RESOURCES; ++i) out[i] = {cmds[i].addr, cmds[i].data};
    ++k.v->requests;
    return send(*k.ops, out, n, (1u << n) - 1, k.v->stuck, &k.v->last) ? -1 : 0;
  }
  static void sleepMs(void *c, uint32_t ms) { static_cast<MmContext *>(c)->ops->hw.sleepMs(ms); }
  static void report(void *c, uint32_t event, uint32_t a, uint32_t b)
  {
    if (event == PianoMmLockWaiting || event == PianoMmLockBusy || event == PianoMmLockLost)
      static_cast<MmContext *>(c)->ops->hw.note("rpmh: the shared multimedia-vote lock %s %#x%s", event == PianoMmLockWaiting ? "is held by"
                                                : event == PianoMmLockBusy ? "stayed held by" : "was lost to", a,
                                                event == PianoMmLockBusy ? ": nothing voted" : "");
    (void)b;
  }
};
// One update of the video client's needs (mask: the resources it changes). 0 or kErrRpmh.
template <class O>
int mmUpdate(O &ops, const Resources &res, Votes &v, uint32_t mask, const uint32_t need[PIANO_MM_RESOURCES])
{
  MmContext<O> ctx{&ops, &res, &v};
  const PianoMmVotesOps mmOps = {&ctx, MmContext<O>::address, MmContext<O>::arcLevel, MmContext<O>::sendCmds, MmContext<O>::sleepMs,
                                 MmContext<O>::report};
  v.mm.words = ops.hw.mmWords();
  v.mm.lock = ops.hw.mmLock();
  v.mm.tag = PianoMmTagVideo;
  v.mm.client = PIANO_MM_VIDEO;
  v.mm.floor[PIANO_MM_MMCX] = v.floorVlvl;
  v.mm.ops = &mmOps;
  const int rc = piano_mm_votes_update(&v.mm, mask, need);
  v.mm.ops = nullptr;
  if (!rc && !v.mm.count) ++v.unchanged;
  return rc ? kErrRpmh : kOk;
}

// The rails for a powered core: MXC to svs, MMCX to max(low_svs, floor), one TCS, only what changes. 0 or kErrRpmh.
template <class O>
int railsUp(O &ops, const Resources &res, Votes &v)
{
  if (res.error) return kErrRpmh;
  const uint32_t need[PIANO_MM_RESOURCES] = {kMxcNeed, kMmcxNeed, 0};
  const int rc = mmUpdate(ops, res, v, 1u << PIANO_MM_MXC | 1u << PIANO_MM_MMCX, need);
  const PianoMmVotes &m = v.mm;
  if (m.count)
    ops.hw.note("rpmh: rails up: mxc.lvl hlvl %u (vlvl %#x), mmcx.lvl hlvl %u (vlvl %#x, floor %#x; others mxc %#x mmcx %#x): %u "
                "command(s), TCS %d, %s after %u ms",
                m.level[PIANO_MM_MXC], m.vote[PIANO_MM_MXC], m.level[PIANO_MM_MMCX], m.vote[PIANO_MM_MMCX], v.floorVlvl,
                m.others[PIANO_MM_MXC], m.others[PIANO_MM_MMCX], m.count, int(v.last.tcs), rc ? "FAILED" : "completed", v.last.polls);
  else if (rc)
    ops.hw.note("rpmh: rails up: nothing sent (%s)", m.error == PianoMmBusy ? "the shared vote lock stayed held" : "no level or address");
  return rc;
}
// After the power-off: MXC to hlvl 0, MMCX to the floor (nothing when it already is there: the usual case). Only what changes; with
// another client's need (the camera's), its level instead.
template <class O>
int railsDown(O &ops, const Resources &res, Votes &v)
{
  if (res.error) return kErrRpmh;
  const uint32_t need[PIANO_MM_RESOURCES] = {0, 0, 0};
  const int rc = mmUpdate(ops, res, v, 1u << PIANO_MM_MXC | 1u << PIANO_MM_MMCX, need);
  const PianoMmVotes &m = v.mm;
  if (m.count)
    ops.hw.note("rpmh: rails down: mxc.lvl hlvl %u (others %#x), mmcx.lvl hlvl %u (vlvl %#x): %u command(s), TCS %d, %s after %u ms",
                m.level[PIANO_MM_MXC], m.others[PIANO_MM_MXC], m.level[PIANO_MM_MMCX], m.vote[PIANO_MM_MMCX], m.count,
                int(v.last.tcs), rc ? "FAILED" : "completed", v.last.polls);
  return rc;
}
// BCM MM1 on (x 0, y 1, commit) or off (commit, valid 0); waited on. Off is sent only when no other client needs it.
template <class O>
int bus(O &ops, const Resources &res, Votes &v, bool on)
{
  if (res.error) return kErrRpmh;
  const uint32_t need[PIANO_MM_RESOURCES] = {0, 0, on ? kBcmMm1On : 0u};
  const int rc = mmUpdate(ops, res, v, 1u << PIANO_MM_MM1, need);
  const PianoMmVotes &m = v.mm;
  if (m.count)
    ops.hw.note("rpmh: MM1 %s (%08x): TCS %d, %s after %u ms", on ? "on" : "off", m.level[PIANO_MM_MM1], int(v.last.tcs),
                rc ? "FAILED" : "completed", v.last.polls);
  else if (!rc && !on && m.others[PIANO_MM_MM1])
    ops.hw.note("rpmh: MM1 stays on (another client needs it)");
  if (!rc) v.mm1On = on;
  return rc;
}

}  // namespace rpmh
}  // namespace pianovideo
