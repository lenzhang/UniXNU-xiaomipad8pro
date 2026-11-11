// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo V1 (piano-video=2): the video core powered on and off in Linux's VPU 3.5 order, at the lowest operating point (PLL0
// 720 MHz, mvs0 / 3 = 240 MHz, MXC svs). Portable (host tests in tests/bringup_host.cpp against a register model that refuses an
// access to a block whose rail, GDSC or clock is off, and that can make every wait fail).
//
// The specification (GPL-2.0, read only; nothing copied): iris_vpu_common.c iris_vpu_power_on / iris_vpu_power_off /
// iris_vpu35_vpu4x_power_on_controller / _power_off_controller / iris_vpu_power_off_hw / iris_vpu_set_preset_registers,
// iris_vpu3x.c iris_vpu35_power_on_hw / iris_vpu35_power_off_hw / iris_vpu33_power_off_hardware, iris_resources.c; for what the
// clock framework does underneath: videocc-sm8750.c, gcc-sm8750.c, clk-alpha-pll.c (Taycan ELU = Lucid EVO ops), clk-rcg2.c
// (shared RCG), clk-branch.c, gdsc.c, common.c qcom_cc_really_probe, reset.c qcom_reset.
//
// On (each step only what is not on yet; a step that fails powers everything down again before it returns):
//   10 rails: MXC svs, MMCX max(low_svs, the display floor) (VideoRpmh.h)     11 bus: BCM MM1 on
//   12 GCC: gcc_video_ahb_clk and gcc_video_xo_clk on if they are not (Linux turns them on at probe and never off; so do we)
//   13 VIDEO_CC as its probe leaves it (once per boot): PLL0's configuration when the PLL is not running, the three critical
//      branches, the shifter/spare bits, both GDSCs' wait values (HW control and SW override off)
//   14 controller GDSC MVS0C: out of collapse, CFG POWER_UP_COMPLETE (2 ms), RETAIN_FF
//   15 controller clocks: GCC AXI1; PLL0 (calibration if PCAL_NOT_DONE, RESET_N, RUN, LOCK_DET (1.5 ms), outputs); mvs0_clk_src on
//      PLL0 / 1 (forced root, CFG, UPDATE (0.5 ms)); the mvs0 divider must give 240 MHz; MVS0C free-running, MVS0C (halt 0.2 ms)
//   16 codec GDSC MVS0, as 14                                                     17 codec clocks: GCC AXI0, MVS0 free-running
//      (memory enable + ack, then the branch), MVS0
//   18 WRAPPER_CORE_CLOCK_CONFIG = 0 (the core clock runs). The A2H/watchdog interrupt stays masked: V1-V3 poll.
// Off (Linux's order; each part only for what is on; a wait that fails is reported and the rest still runs, as Linux does):
//   20 codec: if CORE_POWER_STATUS says powered: clock config, the four pipes idle (20 ms), the MVP NoC low-power handshake (1000
//      tries), its ack (2 ms), the AHB bridge reset; then MVS0 out of hardware mode, MVS0 collapsed (2 ms), MVS0 off, MVS0 free-running
//      off (memory first), GCC AXI0 off
//   21 controller: X2RPMH masked, the CPU NoC low-power handshake (2 ms), the video-ctl NoC handshake (1000 tries) and its ack, the
//      debug bridge; then MVS0C off, MVS0C free-running off, the RCG parked on XO, PLL0 standby, GCC AXI1 off, MVS0C collapsed,
//      the four resets (GCC AXI0/AXI1 ARES, VIDEO_CC MVS0C/MVS0 free-running ARES: assert, 1 us, deassert)
//   22 both GDSCs read back collapsed (before the rails go: VIDEO_CC is not read without them)    23 MM1 off, then the rails
#pragma once
#include <stdint.h>
#include "VideoRpmh.h"
#include "VideoSurvey.h"

namespace pianovideo {

struct PowerState {
  bool ccConfigured = false;     // VIDEO_CC's probe-time set-up done (kept across cycles, as Linux does it once)
  bool gccAhbOn = false, gccXoOn = false;   // we turned them on (and leave them on)
  bool railsUp = false, busOn = false;
  bool ctrlGdsc = false, axi1 = false, pll = false, rcg = false, ctrlFreerun = false, ctrlClk = false;
  bool codecGdsc = false, axi0 = false, codecFreerun = false, codecClk = false, hwMode = false;
  bool on = false;               // everything of 10-18 done
  int failedStep = 0, error = kOk;
  uint32_t failedValue = 0;      // the register value the failed wait last read
  // Off-side events (Linux goes on past them; they are reported).
  uint32_t offWarnings = 0;
  const char *lastWarning = nullptr;
};

template <class O>
class Power {
 public:
  Power(O &ops, const rpmh::Resources &res, rpmh::Votes &votes, PowerState &st) : ops(ops), res(res), votes(votes), st(st) {}
  O &ops;
  const rpmh::Resources &res;
  rpmh::Votes &votes;
  PowerState &st;
  int stopAt = 0;   // piano-video-stop: power everything down after this step

  bool controllerReadable() const { return st.ctrlGdsc && st.axi1 && st.ctrlFreerun && st.ctrlClk; }
  // VIDEO_CC can be read (its rails voted, its probe-time set-up done: GCC's video AHB is on from step 12).
  bool ccReadable() const { return st.ccConfigured && st.railsUp; }
  // A GDSC out of collapse by software request, whoever made it so.
  bool requestedOn(uint32_t gdscr) { return ccReadable() && !(ops.rd(kVideoCc + gdscr) & kGdscCollapse); }
  bool codecReadable() const { return controllerReadable() && st.codecGdsc && st.axi0 && st.codecFreerun && st.codecClk; }

  // ----------------------------------------------------------------------------------------------------------------- on
  int on()
  {
    int rc = onSteps();
    if (rc) {
      ops.hw.note("power: %s at step %d (last value %08x): powering everything down", errorName(rc), st.failedStep, st.failedValue);
      off();
      st.error = rc;
      return rc;
    }
    st.on = true;
    st.error = kOk;
    return kOk;
  }

  // ---------------------------------------------------------------------------------------------------------------- off
  // Everything that is on goes off, in Linux's order. Returns kErrOffCheck if a GDSC still reads on, kErrRpmh if a vote failed.
  int off()
  {
    st.on = false;
    step(kStepCodecOff, "codec power-off (Linux iris_vpu35_power_off_hw)");
    codecOff();
    step(kStepCtrlOff, "controller power-off (Linux iris_vpu35_vpu4x_power_off_controller)");
    controllerOff();
    int rc = kOk;
    if (st.ccConfigured && st.railsUp && branchOn(ops.rd(kGcc + kGccVideoAhb))) {
      step(kStepOffCheck, "both GDSCs collapsed");
      const uint32_t c = ops.rd(kVideoCc + kCcMvs0cGdsc), d = ops.rd(kVideoCc + kCcMvs0Gdsc);
      if (gdscOn(c) || gdscOn(d)) {
        ops.hw.note("power: a GDSC is still on after the power-off: mvs0c %08x mvs0 %08x", c, d);
        rc = kErrOffCheck;
      }
    }
    step(kStepVotesOff, "MM1 off, then the rails");
    if (st.busOn && rpmh::bus(ops, res, votes, false) == kOk) st.busOn = false;
    if (st.railsUp) {
      if (rpmh::railsDown(ops, res, votes) == kOk) st.railsUp = false;
      else rc = rc ? rc : kErrRpmh;
    }
    if (st.busOn) rc = rc ? rc : kErrRpmh;
    return rc;
  }

  // ------------------------------------------------------------------------------------------------------------- pieces
  void step(int n, const char *what) { ops.hw.stage(n, what); }
  int fail(int stepNo, int error, uint32_t value)
  {
    st.failedStep = stepNo;
    st.failedValue = value;
    return error;
  }
  void warn(const char *what)
  {
    ++st.offWarnings;
    st.lastWarning = what;
    ops.hw.note("power-off: %s (going on, as Linux does)", what);
  }
  bool stopHere(int n) const { return stopAt && stopAt == n; }

  // clk_branch_wait's kinds: BRANCH_HALT_SKIP, BRANCH_HALT, BRANCH_HALT_VOTED; hwcg: the branch has hardware clock gating at bit 1
  // of its CBCR (then, with that bit set, its halt is not checked).
  enum Halt : uint8_t { kHaltSkip, kHalt, kHaltVoted };
  // A branch on: CLK_ENABLE, then (halt checked) CLK_OFF clear or the NoC FSM on, within 200 us.
  bool branchEnable(uint64_t pa, Halt halt, bool hwcg, const char *what, uint32_t *last)
  {
    ops.set(pa, kCbcrEnable, what);
    if (halt == kHaltSkip || (hwcg && (ops.rd(pa) & kCbcrHwcg))) return true;
    return ops.pollFor(pa, [](uint32_t v) { return !(v & kCbcrOff) || ((v >> kCbcrNocFsmShift) & 7u) == kCbcrNocFsmOn; },
                       kBranchHaltUs, 1, last);
  }
  // A branch off: CLK_ENABLE cleared; a halt-checked branch must show CLK_OFF within 200 us, a voted one gets 10 us.
  void branchDisable(uint64_t pa, Halt halt, bool hwcg, const char *what)
  {
    ops.clear(pa, kCbcrEnable, what);
    if (halt == kHaltSkip || (hwcg && (ops.rd(pa) & kCbcrHwcg))) return;
    if (halt == kHaltVoted) {
      ops.hw.delayUs(kBranchVotedOffUs);
      return;
    }
    if (!ops.poll(pa, kCbcrOff, kCbcrOff, kBranchHaltUs, 1)) warn(what);
  }
  bool gdscEnable(uint32_t off, const char *name, uint32_t *last)
  {
    ops.clear(kVideoCc + off, kGdscCollapse, name);
    if (!ops.poll(kVideoCc + off + kGdscCfg, kGdscPowerUpComplete, kGdscPowerUpComplete, kGdscPollUs, 1, last)) return false;
    ops.hw.delayUs(1);
    ops.set(kVideoCc + off, kGdscRetainFf, name);
    return true;
  }
  void gdscDisable(uint32_t off, const char *name)
  {
    ops.set(kVideoCc + off, kGdscCollapse, name);
    if (!ops.poll(kVideoCc + off + kGdscCfg, kGdscPowerDownComplete, kGdscPowerDownComplete, kGdscPollUs, 1)) warn(name);
  }
  void reset(uint64_t pa, const char *what)
  {
    ops.set(pa, kCbcrAres, what);
    ops.hw.delayUs(1);
    ops.clear(pa, kCbcrAres, what);
  }

  bool pllRunning()
  {
    return (ops.rd(kVideoCc + kCcPll0 + kPllOpmode) & kPllRun) && (ops.rd(kVideoCc + kCcPll0) & kPllOutctrl);
  }
  // alpha_pll_lucid_evo_enable
  bool pllEnable(uint32_t *last)
  {
    const uint64_t p = kVideoCc + kCcPll0;
    if (ops.rd(p + kPllUserCtl) & kPllEnableVoteRun)   // FSM mode: the vote is the hardware's; wait for the lock
      return ops.poll(p, kPllLockDet, kPllLockDet, kPllLockUs, 1, last);
    if (pllRunning()) return true;
    ops.set(p, kPllResetN, "pll0 MODE: RESET_N");
    ops.wr(p + kPllOpmode, kPllRun, "pll0 OPMODE: run");
    if (!ops.poll(p, kPllLockDet, kPllLockDet, kPllLockUs, 1, last)) return false;
    ops.set(p + kPllUserCtl, kPllOutMask, "pll0 USER_CTL: outputs on");
    ops.set(p, kPllOutctrl, "pll0 MODE: OUTCTRL");
    return true;
  }
  // _alpha_pll_lucid_evo_disable (no reset)
  void pllDisable()
  {
    const uint64_t p = kVideoCc + kCcPll0;
    if (ops.rd(p + kPllUserCtl) & kPllEnableVoteRun) return;
    ops.clear(p, kPllOutctrl, "pll0 MODE: OUTCTRL off");
    ops.clear(p + kPllUserCtl, kPllOutMask, "pll0 USER_CTL: outputs off");
    ops.wr(p + kPllOpmode, kPllStandby, "pll0 OPMODE: standby");
  }
  // clk_rcg2_shared_enable with the 720 MHz entry / clk_rcg2_shared_disable (parked on XO).
  bool rcgSet(uint32_t cfg, const char *what, uint32_t *last)
  {
    const uint64_t r = kVideoCc + kCcMvs0Rcg;
    ops.set(r, kRcgRootEn, "mvs0_clk_src CMD: ROOT_EN (forced on)");
    bool ok = ops.poll(r, kRcgRootOff, 0, kRcgUpdateUs, 1, last);
    if (ok) {
      const uint32_t old = ops.rd(r + 4);
      ops.wr(r + 4, cfg == kRcgCfgXo ? kRcgCfgXo : (old & ~kRcgCfgMask) | cfg, what);
      ops.set(r, kRcgUpdate, "mvs0_clk_src CMD: UPDATE");
      ok = ops.poll(r, kRcgUpdate, 0, kRcgUpdateUs, 1, last);
    }
    ops.clear(r, kRcgRootEn, "mvs0_clk_src CMD: ROOT_EN released");
    return ok;
  }

  int onSteps()
  {
    uint32_t last = 0;
    // 10, 11: the votes
    step(kStepRails, "rails: MXC svs, MMCX at the display floor (RPMh, DRV-2 lock)");
    if (!st.railsUp) {
      if (rpmh::railsUp(ops, res, votes)) return fail(kStepRails, kErrRpmh, votes.last.status);
      st.railsUp = true;
    }
    if (stopHere(kStepRails)) return fail(kStepRails, kErrStopped, 0);
    step(kStepBus, "bus: BCM MM1 on");
    if (!st.busOn) {
      if (rpmh::bus(ops, res, votes, true)) return fail(kStepBus, kErrRpmh, votes.last.status);
      st.busOn = true;
    }
    if (stopHere(kStepBus)) return fail(kStepBus, kErrStopped, 0);
    // 12: GCC's video AHB/XO (critical in Linux)
    step(kStepGcc, "GCC video AHB and XO branches");
    const uint32_t gccCritical[] = {kGccVideoAhb, kGccVideoXo};
    for (uint32_t o : gccCritical) {
      const uint32_t v = ops.rd(kGcc + o);
      if (branchOn(v) && (v & kCbcrEnable)) continue;
      if (!branchEnable(kGcc + o, kHalt, false, o == kGccVideoAhb ? "gcc_video_ahb_clk: on" : "gcc_video_xo_clk: on", &last))
        return fail(kStepGcc, kErrGcc, last);
      (o == kGccVideoAhb ? st.gccAhbOn : st.gccXoOn) = true;
    }
    if (stopHere(kStepGcc)) return fail(kStepGcc, kErrStopped, 0);
    // 13: VIDEO_CC as its probe leaves it
    step(kStepCcInit, "VIDEO_CC set-up (PLL0 configuration, critical branches, GDSC wait values)");
    if (!st.ccConfigured) {
      const uint64_t p = kVideoCc + kCcPll0;
      if (!pllRunning()) {
        ops.wr(p + kPllL, kPllConfigL | kPllCalL, "pll0 L_VAL: 0x25 (720 MHz), CAL_L 0x44");
        ops.wr(p + kPllAlpha, kPllConfigAlpha, "pll0 ALPHA_VAL: 0x8000");
        ops.wr(p + kPllConfigCtl, kPll0ConfigCtl, "pll0 CONFIG_CTL");
        ops.wr(p + kPllConfigCtlU, kPll0ConfigCtlU, "pll0 CONFIG_CTL_U");
        ops.wr(p + kPllConfigCtlU1, kPll0ConfigCtlU1, "pll0 CONFIG_CTL_U1");
        ops.wr(p + kPllUserCtlU, kPll0UserCtlU, "pll0 USER_CTL_U");
        ops.clear(p, kPllOutctrl, "pll0 MODE: OUTCTRL off");
        ops.wr(p + kPllOpmode, kPllStandby, "pll0 OPMODE: standby");
        ops.set(p, kPllResetN, "pll0 MODE: RESET_N");
      } else {
        ops.hw.note("power: PLL0 already runs (L %#x): its configuration is left as found", ops.rd(p + kPllL) & 0xffffu);
      }
      const uint32_t critical[] = {kCcAhbClk, kCcSleepClk, kCcXoClk};
      for (uint32_t o : critical) ops.set(kVideoCc + o, kCbcrEnable, "critical branch: CLK_EN (Linux probe)");
      ops.set(kVideoCc + kCcShifterDoneMvs0, kCcShifterDoneBits, "mvs0 DLY_ACCU_RED_SHIFTER_DONE 0xf (Linux probe)");
      ops.set(kVideoCc + kCcShifterDoneMvs0c, kCcShifterDoneBits, "mvs0c DLY_ACCU_RED_SHIFTER_DONE 0xf (Linux probe)");
      ops.set(kVideoCc + kCcSpare9f24, 1, "0x9f24 bit 0 (Linux probe)");
      const uint32_t gdscs[] = {kCcMvs0cGdsc, kCcMvs0Gdsc};
      for (uint32_t g : gdscs) {
        ops.update(kVideoCc + g, kGdscHwControl | kGdscSwOverride | kGdscWaitMask, kGdscWaits, "GDSC: wait values, HW control off");
        if (gdscOn(ops.rd(kVideoCc + g))) ops.set(kVideoCc + g, kGdscRetainFf, "GDSC (found on): RETAIN_FF");
        // Out of hardware control, a GDSC whose SW_COLLAPSE is clear is requested on; the power-off (and any failure) collapses
        // every GDSC it finds requested on, ours or not (requestedOn below).
        if (!(ops.rd(kVideoCc + g) & kGdscCollapse))
          ops.hw.note("power: the %s GDSC was found requested on (SW_COLLAPSE clear): the power-off collapses it",
                      g == kCcMvs0cGdsc ? "controller" : "codec");
      }
      st.ccConfigured = true;
    }
    if (stopHere(kStepCcInit)) return fail(kStepCcInit, kErrStopped, 0);
    // 14: the controller's GDSC
    step(kStepCtrlGdsc, "controller GDSC MVS0C on");
    if (!st.ctrlGdsc) {
      st.ctrlGdsc = true;   // from the first write on: the off path collapses it again
      if (!gdscEnable(kCcMvs0cGdsc, "mvs0c GDSC: out of collapse", &last)) return fail(kStepCtrlGdsc, kErrGdsc, last);
    }
    if (stopHere(kStepCtrlGdsc)) return fail(kStepCtrlGdsc, kErrStopped, 0);
    // 15: the controller's clocks (iris_vpu35_vpu4x_power_on_controller: AXI1, CTRL_FREERUN, CTRL), the RCG and PLL0 under them
    step(kStepCtrlClocks, "controller clocks: GCC AXI1, PLL0 720 MHz, mvs0_clk_src, MVS0C free-running, MVS0C");
    if (!st.axi1) {
      branchEnable(kGcc + kGccVideoAxi1, kHaltSkip, true, "gcc_video_axi1_clk: on", &last);
      st.axi1 = true;
    }
    if (!st.pll) {
      st.pll = true;
      const uint64_t p = kVideoCc + kCcPll0;
      if (ops.rd(p) & kPllPcalNotDone) {   // _alpha_pll_lucid_evo_prepare: one enable/disable for the calibration
        if (!pllEnable(&last)) return fail(kStepCtrlClocks, kErrPll, last);
        pllDisable();
      }
      if (!pllEnable(&last)) return fail(kStepCtrlClocks, kErrPll, last);
      // A PLL0 found running keeps its configuration (as in Linux): it must be the 720 MHz one, or the core would run above the
      // rate the rails are voted for.
      const uint32_t l = ops.rd(p + kPllL) & 0xffffu, alpha = ops.rd(p + kPllAlpha) & 0xffffu;
      if (l != kPllConfigL || alpha != kPllConfigAlpha) {
        ops.hw.note("power: PLL0 runs at L %#x alpha %#x (%llu MHz), not Linux's 720 MHz configuration: refused", l, alpha,
                    (unsigned long long)(pllHz(l, alpha) / 1000000));
        return fail(kStepCtrlClocks, kErrPll, l);
      }
    }
    if (!st.rcg) {
      st.rcg = true;
      if (!rcgSet(kMvs0Cfg720, "mvs0_clk_src CFG: PLL0 / 1 (720 MHz)", &last)) return fail(kStepCtrlClocks, kErrRcg, last);
    }
    const uint32_t div = ops.rd(kVideoCc + kCcMvs0Div) & 0xfu;
    if (div != kMvs0DivFor240) {
      ops.hw.note("power: mvs0 divider %u: the core would run at %u MHz, not the 240 MHz the rails are voted for", div + 1,
                  720 / (div + 1));
      return fail(kStepCtrlClocks, kErrDivider, div);
    }
    if (!st.ctrlFreerun) {
      st.ctrlFreerun = true;
      if (!branchEnable(kVideoCc + kCcMvs0cFreerun, kHalt, false, "video_cc_mvs0c_freerun_clk: on", &last))
        return fail(kStepCtrlClocks, kErrBranch, last);
    }
    if (!st.ctrlClk) {
      st.ctrlClk = true;
      if (!branchEnable(kVideoCc + kCcMvs0cClk, kHalt, false, "video_cc_mvs0c_clk: on", &last))
        return fail(kStepCtrlClocks, kErrBranch, last);
    }
    if (stopHere(kStepCtrlClocks)) return fail(kStepCtrlClocks, kErrStopped, 0);
    // 16: the codec's GDSC
    step(kStepCodecGdsc, "codec GDSC MVS0 on");
    if (!st.codecGdsc) {
      st.codecGdsc = true;
      if (!gdscEnable(kCcMvs0Gdsc, "mvs0 GDSC: out of collapse", &last)) return fail(kStepCodecGdsc, kErrGdsc, last);
    }
    if (stopHere(kStepCodecGdsc)) return fail(kStepCodecGdsc, kErrStopped, 0);
    // 17: the codec's clocks (iris_vpu35_power_on_hw: AXI (gcc axi0), HW_FREERUN, HW)
    step(kStepCodecClocks, "codec clocks: GCC AXI0, MVS0 free-running, MVS0 (240 MHz)");
    if (!st.axi0) {
      branchEnable(kGcc + kGccVideoAxi0, kHaltSkip, true, "gcc_video_axi0_clk: on", &last);
      st.axi0 = true;
    }
    if (!st.codecFreerun) {
      st.codecFreerun = true;
      ops.clear(kVideoCc + kCcMvs0FreerunMem, kMemEnable, "video_cc_mvs0_freerun_clk memory: enabled (bit 3 cleared)");
      if (!ops.pollFor(kVideoCc + kCcMvs0FreerunMem, [](uint32_t v) { return (v & kMemAck) != 0; }, kBranchHaltUs, 1, &last))
        return fail(kStepCodecClocks, kErrBranch, last);
      if (!branchEnable(kVideoCc + kCcMvs0Freerun, kHalt, false, "video_cc_mvs0_freerun_clk: on", &last))
        return fail(kStepCodecClocks, kErrBranch, last);
    }
    if (!st.codecClk) {
      st.codecClk = true;
      if (!branchEnable(kVideoCc + kCcMvs0Clk, kHaltVoted, true, "video_cc_mvs0_clk: on", &last))
        return fail(kStepCodecClocks, kErrBranch, last);
    }
    if (stopHere(kStepCodecClocks)) return fail(kStepCodecClocks, kErrStopped, 0);
    // 18: the core's own preset
    step(kStepPreset, "WRAPPER_CORE_CLOCK_CONFIG = 0 (the core clock runs)");
    ops.wr(kIris + kCoreClockConfig, 0, "WRAPPER_CORE_CLOCK_CONFIG: run");
    if (stopHere(kStepPreset)) return fail(kStepPreset, kErrStopped, 0);
    return kOk;
  }

  // iris_vpu35_power_off_hw: iris_vpu33_power_off_hardware, then HW_FREERUN and AXI0.
  void codecOff()
  {
    if (codecReadable() && (ops.rd(kIris + kCorePowerStatus) & kCorePowered)) {
      do {
        if (ops.rd(kIris + kCoreClockConfig)) ops.wr(kIris + kCoreClockConfig, 0, "WRAPPER_CORE_CLOCK_CONFIG: run");
        bool idle = true;
        for (unsigned i = 0; i < kVppPipes && idle; ++i)
          idle = ops.poll(kIris + kVcodecIdleStatus + 4 * i, kVcodecIdle, kVcodecIdle, kIdlePollUs, kIdleStepUs);
        if (!idle) {
          warn("a VPP pipe is not idle after 20 ms");
          break;
        }
        uint32_t v = 0;
        bool done = false, busy = false;
        for (unsigned n = 0; n < kLpiRetries; ++n) {
          ops.wr(kIris + kAonNocLpiCtl, kLpiReq, "AON MVP NoC LPI: power-down request");
          ops.hw.delayUs(kLpiRetryUsCodec);
          v = ops.rd(kIris + kAonNocLpiStatus);
          done = v & kLpiDone;
          busy = v & (kLpiDeny | kLpiActive);
          if (done || !busy) break;
          ops.wr(kIris + kAonNocLpiCtl, 0, "AON MVP NoC LPI: request withdrawn");
          ops.hw.delayUs(kLpiRetryUsCodec);
        }
        if (!done && busy) warn("the MVP NoC low-power handshake was denied 1000 times");
        if (!ops.poll(kIris + kAonNocLpiStatus, kLpiDone, kLpiDone, kLpiPollUs, kLpiStepUs)) {
          warn("the MVP NoC low-power ack did not come");
          break;
        }
        ops.wr(kIris + kAonNocLpiCtl, 0, "AON MVP NoC LPI: request withdrawn");
        ops.wr(kIris + kAhbBridgeSyncReset, kBridgeSwReset | kBridgeHwResetDisable, "AHB bridge: sw reset");
        ops.wr(kIris + kAhbBridgeSyncReset, kBridgeHwResetDisable, "AHB bridge: hw reset disabled");
        ops.wr(kIris + kAhbBridgeSyncReset, 0, "AHB bridge: released");
      } while (false);
    }
    // disable_power: iris_vpu_power_off_hw
    if (st.hwMode) {
      ops.clear(kVideoCc + kCcMvs0Gdsc, kGdscHwControl, "mvs0 GDSC: software mode");
      ops.hw.delayUs(1);
      if (!ops.poll(kVideoCc + kCcMvs0Gdsc + kGdscCfg, kGdscPowerUpComplete, kGdscPowerUpComplete, kGdscPollUs, 1))
        warn("mvs0 GDSC did not report on after leaving hardware mode");
      st.hwMode = false;
    }
    if (st.codecGdsc || requestedOn(kCcMvs0Gdsc)) {
      gdscDisable(kCcMvs0Gdsc, "mvs0 GDSC: collapse");
      st.codecGdsc = false;
    }
    if (st.codecClk) {
      branchDisable(kVideoCc + kCcMvs0Clk, kHaltVoted, true, "video_cc_mvs0_clk: off");
      st.codecClk = false;
    }
    if (st.codecFreerun) {
      ops.set(kVideoCc + kCcMvs0FreerunMem, kMemEnable, "video_cc_mvs0_freerun_clk memory: disabled (bit 3 set)");
      branchDisable(kVideoCc + kCcMvs0Freerun, kHalt, false, "video_cc_mvs0_freerun_clk: off");
      st.codecFreerun = false;
    }
    if (st.axi0) {
      branchDisable(kGcc + kGccVideoAxi0, kHaltSkip, true, "gcc_video_axi0_clk: off");
      st.axi0 = false;
    }
  }

  // iris_vpu35_vpu4x_power_off_controller
  void controllerOff()
  {
    if (controllerReadable()) {
      do {
        ops.wr(kIris + kX2Rpmh, kX2RpmhMaskAll, "X2RPMH: both masked");
        ops.wr(kIris + kCpuNocLpiCtl, kLpiReq, "CPU NoC LPI: power-down request");
        if (!ops.poll(kIris + kCpuNocLpiStatus, kLpiDone, kLpiDone, kLpiPollUs, kLpiStepUs)) {
          warn("the CPU NoC low-power ack did not come");
          break;
        }
        ops.wr(kIris + kCpuNocLpiCtl, 0, "CPU NoC LPI: request withdrawn");
        uint32_t v = 0;
        bool done = false, busy = false;
        for (unsigned n = 0; n < kLpiRetries; ++n) {
          ops.wr(kIris + kAonVideoCtlNocLpiCtl, kLpiReq, "video-ctl NoC LPI: power-down request");
          ops.hw.delayUs(kLpiRetryUsCtl);
          v = ops.rd(kIris + kAonVideoCtlNocLpiStatus);
          done = v & kLpiDone;
          busy = v & (kLpiDeny | kLpiActive);
          if (done || !busy) break;
          ops.wr(kIris + kAonVideoCtlNocLpiCtl, 0, "video-ctl NoC LPI: request withdrawn");
          ops.hw.delayUs(kLpiRetryUsCtl);
        }
        if (!done && busy) warn("the video-ctl NoC low-power handshake was denied 1000 times");
        if (!ops.poll(kIris + kAonVideoCtlNocLpiStatus, kLpiDone, kLpiDone, kLpiPollUs, kLpiStepUs)) {
          warn("the video-ctl NoC low-power ack did not come");
          break;
        }
        ops.wr(kIris + kAonVideoCtlNocLpiCtl, 0, "video-ctl NoC LPI: request withdrawn");
        ops.wr(kIris + kDebugBridgeLpiCtl, 0, "debug bridge LPI: none");
        ops.poll(kIris + kDebugBridgeLpiStatus, ~0u, 0, kLpiPollUs, kLpiStepUs);   // result ignored, as Linux does
      } while (false);
    }
    // disable_power
    if (st.ctrlClk) {
      branchDisable(kVideoCc + kCcMvs0cClk, kHalt, false, "video_cc_mvs0c_clk: off");
      st.ctrlClk = false;
    }
    if (st.ctrlFreerun) {
      branchDisable(kVideoCc + kCcMvs0cFreerun, kHalt, false, "video_cc_mvs0c_freerun_clk: off");
      st.ctrlFreerun = false;
    }
    if (st.rcg) {   // its last child went off: the shared RCG parks on XO
      if (!rcgSet(kRcgCfgXo, "mvs0_clk_src CFG: XO (parked)", nullptr)) warn("mvs0_clk_src did not park on XO");
      st.rcg = false;
    }
    if (st.pll) {
      pllDisable();
      st.pll = false;
    }
    if (st.axi1) {
      branchDisable(kGcc + kGccVideoAxi1, kHaltSkip, true, "gcc_video_axi1_clk: off");
      st.axi1 = false;
    }
    if (st.ctrlGdsc || requestedOn(kCcMvs0cGdsc)) {
      gdscDisable(kCcMvs0cGdsc, "mvs0c GDSC: collapse");
      st.ctrlGdsc = false;
      // reset_control_bulk_reset(bus0, bus1, core, vcodec0_core)
      reset(kGcc + kGccVideoAxi0, "gcc_video_axi0 ARES (bus0)");
      reset(kGcc + kGccVideoAxi1, "gcc_video_axi1 ARES (bus1)");
      reset(kVideoCc + kCcMvs0cFreerun, "video_cc_mvs0c_freerun ARES (core)");
      reset(kVideoCc + kCcMvs0Freerun, "video_cc_mvs0_freerun ARES (vcodec0_core)");
    }
  }

  // MVS0 to hardware control after the firmware boots (iris_vpu_set_hwmode: no poll) and back (in codecOff).
  void hardwareMode()
  {
    ops.set(kVideoCc + kCcMvs0Gdsc, kGdscHwControl, "mvs0 GDSC: hardware mode");
    ops.hw.delayUs(1);
    st.hwMode = true;
  }
};

// V1's check of a powered core (the level 1 survey's readout with the gate open): both domains read, the codec powered, words that
// are not all ones (a bus error's pattern), no firmware (CTRL_STATUS 0), PLL0 locked and the core clock at the 240 MHz OPP. The
// version word's layout is only inferred for VPU 3.5 (VideoHw.h), so a 0 there is recorded, not refused.
inline const char *poweredVerdict(const Clocks &c, const Core &r)
{
  if (!c.controller || !r.controllerRead) return "the controller's gate is closed after the power-on";
  if (!c.codec || !r.codecRead) return "the codec's gate is closed after the power-on";
  if (!(r.powerStatus & kCorePowered)) return "CORE_POWER_STATUS does not report the codec powered";
  if (r.hwVersion == 0xffffffffu || r.powerStatus == 0xffffffffu || r.intrMask == 0xffffffffu)
    return "the core's words read all ones (a bus error?)";
  if (r.ctrlStatus) return "CTRL_STATUS is not 0 (a firmware runs?)";
  if (!pllLocked(c.pllMode) || oppIndex(mvs0Hz(c)) != 0) return "PLL0 is not locked or the core clock is not 240 MHz";
  return nullptr;
}

}  // namespace pianovideo
