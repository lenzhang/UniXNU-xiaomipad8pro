// SPDX-License-Identifier: BSD-2-Clause-Patent
// One VRM transaction on the application processor's RSC (apps DRV-2), for the shared rails helper's `vrm` (package/include/
// PianoWcnRails.h) when PianoBluetooth brings the rails up itself: an idle ACTIVE TCS (2, 1, then 0), its commands (message id
// "write, response required", address, data), WAIT_FOR_CMPL and CMD_ENABLE for them, its IRQ enable bit, the trigger, completion
// polled for at most 100 ms (the TCS's IRQ status bit, or every command's CMD_STATUS completed), then the TCS reclaimed with
// CMD_ENABLE, WAIT_FOR_CMPL and CONTROL back at 0 (PianoA830 takes a TCS as idle only when all three read 0). A TCS that did not
// complete is left as it is and never used again. The caller holds the DRV-2 lock (PianoA830's sm8750_rpmh_drv2_owner) around it,
// as every other user does, so nothing in here may sleep: with `delayUs` the poll is a busy wait of kTcsPollUs steps (P17,
// docs/drivers/wifi.md section 10.22: a holder that slept here, and logged before letting go, kept PianoA830 out for 2 s and the
// GPU's bring-up failed); `sleepMs` (1 ms steps) is only the fallback of an owner that gives no `delayUs`.
// The same sequence as PianoWifi.cpp's vrmLocked and PianoKeyboard.cpp's voteLocked (this repository, BSD-2-Clause-Patent), over
// callbacks so the host tests (tests/bt_bringup_host.cpp) run it against an RSC model; the register layout and the cmd-db lookup are
// package/PianoKeyboard/CmdDb.h's (included, not copied).
#pragma once
#include <stdint.h>
#include "CmdDb.h"
#include "PianoWcnRails.h"

namespace pianobt {

using pianokeyboard::RscLayout;

constexpr uint32_t kAmcEnable = 1u << 16, kAmcTrigger = 1u << 24, kMsgidWriteResp = 8u | 1u << 8 | 1u << 16, kCmdCompleted = 1u << 16;
constexpr unsigned kTcsPollMs = 100, kTcsPollUs = 10;

// Offsets are from the DRV's TCS block (DRV-2 + 0xd00); the DRV id is read once by the owner (DRV-2 + 0) for the layout.
struct RscIo {
  void *ctx = nullptr;
  uint32_t (*rd)(void *ctx, uint32_t off) = nullptr;
  void (*wr)(void *ctx, uint32_t off, uint32_t value) = nullptr;
  void (*sleepMs)(void *ctx, unsigned ms) = nullptr;
  void (*delayUs)(void *ctx, unsigned us) = nullptr;   // a busy wait (IODelay): the poll under the DRV-2 lock
};

struct Rsc {
  RscLayout layout{};
  bool stuck[3] = {false, false, false};
};

struct VrmResult {
  int tcs = -1;
  bool done = false;
  unsigned waited = 0;       // in ms, whole (the polls' own steps, not a clock)
  unsigned waitedUs = 0;     // the same in us
  uint32_t status[3] = {0, 0, 0};
};

// `base` is the resource's RPMh address (cmd-db); each command's register is an offset from it. 0, -16 (no idle TCS), -22 (bad
// arguments) or -60 (not completed within 100 ms).
inline int vrmSend(Rsc &rsc, const RscIo &io, uint32_t base, const PianoWcnVrmCmd *cmds, unsigned count, VrmResult &res)
{
  res = VrmResult();
  if (!io.rd || !io.wr || (!io.sleepMs && !io.delayUs) || !base || !cmds || !count || count > 3) return -22;
  const RscLayout &l = rsc.layout;
  static const unsigned kOrder[3] = {2, 1, 0};
  for (unsigned k = 0; k < 3 && res.tcs < 0; ++k)
    if (!rsc.stuck[kOrder[k]] && io.rd(io.ctx, l.tcsStride * kOrder[k] + l.status)) res.tcs = int(kOrder[k]);
  if (res.tcs < 0) return -16;
  const uint32_t at = l.tcsStride * unsigned(res.tcs), bit = 1u << res.tcs, irqEnable = io.rd(io.ctx, 0), mask = (1u << count) - 1;
  if (io.rd(io.ctx, l.irqStatus) & bit) io.wr(io.ctx, l.irqClear, bit);
  for (unsigned c = 0; c < count; ++c) {
    const uint32_t cmd = at + l.cmdStride * c;
    io.wr(io.ctx, cmd + l.cmdMsgid, kMsgidWriteResp);
    io.wr(io.ctx, cmd + l.cmdAddr, base + cmds[c].reg);
    io.wr(io.ctx, cmd + l.cmdData, cmds[c].value);
  }
  io.wr(io.ctx, at + l.waitForCmpl, mask);
  io.wr(io.ctx, at + l.cmdEnable, mask);
  io.wr(io.ctx, 0, irqEnable | bit);
  const uint32_t control = io.rd(io.ctx, at + l.control);
  io.wr(io.ctx, at + l.control, control & ~kAmcTrigger);
  io.wr(io.ctx, at + l.control, control & ~(kAmcTrigger | kAmcEnable));
  io.wr(io.ctx, at + l.control, kAmcEnable);
  io.wr(io.ctx, at + l.control, kAmcEnable | kAmcTrigger);
  for (;;) {
    uint32_t all = kCmdCompleted;
    for (unsigned c = 0; c < count; ++c) {
      res.status[c] = io.rd(io.ctx, at + l.cmdStride * c + l.cmdData + 4);
      all &= res.status[c];
    }
    res.done = (io.rd(io.ctx, l.irqStatus) & bit) || all == kCmdCompleted;
    res.waited = res.waitedUs / 1000;
    if (res.done || res.waitedUs >= kTcsPollMs * 1000) break;
    if (io.delayUs) {
      io.delayUs(io.ctx, kTcsPollUs);
      res.waitedUs += kTcsPollUs;
    } else {
      io.sleepMs(io.ctx, 1);
      res.waitedUs += 1000;
    }
  }
  if (res.done) {
    io.wr(io.ctx, at + l.cmdEnable, 0);
    io.wr(io.ctx, at + l.waitForCmpl, 0);
    io.wr(io.ctx, l.irqClear, bit);
    io.wr(io.ctx, at + l.control, io.rd(io.ctx, at + l.control) & ~kAmcTrigger);
    io.wr(io.ctx, at + l.control, 0);
  } else {
    rsc.stuck[res.tcs] = true;
  }
  io.wr(io.ctx, 0, irqEnable);
  return res.done ? 0 : -60;
}

}  // namespace pianobt
