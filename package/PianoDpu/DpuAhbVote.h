// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu: piano-dpu-ahb-vote=N, GCC's display AHB branch (GCC_DISP_AHB_CBCR, GCC 0x27004) before the level 1 survey: the only
// stores of level 1, all to that one register, under PianoA830's GCC vote lock word (sm8750_gcc_vote_owner, as PianoTouch, PianoDwc3,
// PianoWifi and PianoBluetooth take it). Portable (host tests in tests/dpu_host.cpp): the hardware through Load / Store, the lock
// through Lock (bool take(unsigned *tries), void give()). docs/drivers/dpu.md section 8.
//
// What the tablet showed (2026-11-12, queue5 P13, the dpu1 and dpu-c0 boots, the panel lit by NativeGopDisplay): the branch read
// 0x88000003, CLK_ENABLE (bit 0) set, HW_CTL (bit 1) set, CLK_OFF (bit 31) set; hf-axi 0x08200001 (running). The software vote the
// documented next step was to write is there already; the branch is hardware-gated and CLK_OFF only says it is idle. Linux's GCC
// driver only sets CLK_ENABLE on it (its "kept on" list) before its display clock controller reads DISPCC behind it, and its halt
// check skips CLK_OFF for a branch in hardware clock gating mode (gcc-sm8750.c, clk-branch.c: the specification only).
//   1  the vote: CLK_ENABLE set if clear (one read-modify-write under the lock; none when it is set, as on the tablet); then the
//      gate takes Linux's rule for this branch: enabled and hardware-gated counts as on (DISPCC's GDSC and branches are checked as
//      before, strictly).
//   2  as 1, and HW_CTL cleared (the branch forced on, in the same read-modify-write): the gate then keeps its strict rule, CLK_OFF
//      must read clear (within 500 us) before anything behind it is loaded.
// The vote stays while anything after the survey reads MDSS (piano-dpu=2, a writing slice); else undone after the survey: only
// the bits this vote changed are put back, under the lock. Neither bit can stop the scan-out (inferred from the clock tree: the
// panel's pixels come over the hf-axi branch and DISPCC's MDP clock; this AHB branch is the register interface, and both bits
// only ever make it run more, never less, until the undo restores what was found).
#pragma once
#include <stdint.h>
#include "DpuSurvey.h"

namespace pianodpu {

constexpr uint32_t kDpuGccVoteTag = 0x44505531;   // "DPU1", PianoDpu's tag in the lock word
constexpr unsigned kGccLockTries = 5000;          // every 10 us: 50 ms, as the other holders
constexpr unsigned kAhbOnWaitUs = 500;            // the strict rule's wait for CLK_OFF to clear (Linux's branch wait is 200 us)
constexpr uint32_t kUnreadable = 0xffffffffu;     // the kernel's load of a page that did not map: never written back

struct AhbVote {
  unsigned mode = 0;                  // piano-dpu-ahb-vote
  uint32_t found = 0, written = 0, after = 0, undoneTo = 0;
  bool wrote = false, setEnable = false, clearedHwCtl = false, lockFailed = false, on = false, undone = false, undoLockFailed = false;
  unsigned lockTries = 0, waitedUs = 0;
};

inline uint32_t ahbWanted(unsigned mode, uint32_t cbcr)
{
  uint32_t w = cbcr | kCbcrEnable;
  if (mode >= 2) w &= ~kCbcrHwCtl;
  return w;
}

// Before the survey. v.on: the branch now passes the gate's rule for this mode (the gate decides again by itself).
template <class Load, class Store, class Delay, class Lock>
AhbVote ahbVote(unsigned mode, Load load, Store store, Delay delayUs, Lock &lock)
{
  AhbVote v;
  v.mode = mode;
  const uint64_t pa = kGcc + kGccDispAhb;
  v.found = load(pa);
  if (mode && v.found != kUnreadable && ahbWanted(mode, v.found) != v.found) {
    if (!lock.take(&v.lockTries)) {
      v.lockFailed = true;   // nothing written: the gate reads what it finds
    } else {
      const uint32_t now = load(pa);   // again, under the lock
      const uint32_t w = ahbWanted(mode, now);
      if (now != kUnreadable && w != now) {
        store(pa, w);
        v.wrote = true;
        v.written = w;
        v.setEnable = !(now & kCbcrEnable);
        v.clearedHwCtl = (now & kCbcrHwCtl) && !(w & kCbcrHwCtl);
      }
      lock.give();
    }
  }
  for (v.waitedUs = 0;; ++v.waitedUs) {
    v.after = load(pa);
    v.on = branchOn(v.after) || (mode == 1 && branchHwGated(v.after));
    if (v.on || !mode || v.waitedUs >= kAhbOnWaitUs) break;
    delayUs(1u);
  }
  return v;
}

// After the survey, when nothing later needs the registers: the bits this vote changed back as found.
template <class Load, class Store, class Lock>
void ahbUndo(AhbVote &v, Load load, Store store, Lock &lock)
{
  if (!v.setEnable && !v.clearedHwCtl) return;
  const uint64_t pa = kGcc + kGccDispAhb;
  unsigned tries = 0;
  if (!lock.take(&tries)) {
    v.undoLockFailed = true;   // left as voted
    return;
  }
  const uint32_t now = load(pa);
  uint32_t w = now;
  if (v.setEnable) w &= ~kCbcrEnable;
  if (v.clearedHwCtl) w |= kCbcrHwCtl;
  if (now != kUnreadable && w != now) store(pa, w);
  lock.give();
  v.undone = true;
  v.undoneTo = load(pa);
}

}  // namespace pianodpu
