// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo levels 2-4 (the ladder's V1-V3, docs/drivers/video.md sections 8 and 12) as one portable sequence over the owner's Hw:
//   piano-video=2  V1: N power cycles (piano-video-cycles, default 1; the test plan's 20): on (VideoPower.h), the level 1 readout
//                  with the gate open and its verdict, hold (piano-video-hold-ms), off; every cycle must pass
//   piano-video=3  V2: N cycles of on, the firmware (VideoFirmware.h: the pinned file, PAS 9, MP_VIDEO_VAR), hold, PAS
//                  shutdown, off
//   piano-video=4  V3: the SMMU bank (VideoSmmu.h) and the UC region, on, the firmware, the boot words, MVS0 to hardware mode, the
//                  handshake (SYS_INIT, IMAGE_VERSION, interframe power collapse; the answers within 1 s; the version string), no
//                  SMMU fault; then idle: the core collapses piano-video-idle-ms (1.5 s) after its last activity (prepare-PC,
//                  SET_REMOTE_STATE(0), off; the firmware stays in the carveout); piano-video-resumes=N more resume (on,
//                  SET_REMOTE_STATE(1), boot words, hardware mode, interframe power collapse, IMAGE_VERSION as a ping) / idle /
//                  collapse cycles; piano-video-unload=1 ends with Linux's deinit (resume, PAS shutdown, off, the bank released).
// Any failure powers everything down (PAS shutdown while the core is powered, the power-off, the votes, the bank and its memory)
// before run() returns; piano-video-stop=STEP stops the same way after that step (the bisecting aid). Every register write is
// logged once (VideoOps.h); every wait is bounded.
//
// The Hw (a duck type: the kernel's in PianoVideo.cpp, the model in tests/bringup_host.cpp): VideoOps.h's rd/wr/delayUs/sleepMs/
// nowUs/note/stage, VideoRpmh.h's drv2Take/drv2Give, VideoFirmware.h's firmware, carveout and secure-call members, and:
//   const uint8_t *cmdDb(uint32_t *bytes)   the AOP command database (a snapshot, kCmdDbBytes; nullptr when not readable)
//   bool dmaAlloc(Dma &d) / void dmaFree(Dma &d)   V3's memory (below); zeroed, the tables cleaned by cleanToPoc
// The includer declares snprintf (libkern/libkern.h in the kernel, <stdio.h> on the host).
#pragma once
#include <stdint.h>
#include "VideoFirmware.h"
#include "VideoHfi.h"
#include "VideoPower.h"
#include "VideoSmmu.h"

namespace pianovideo {

struct Options {
  unsigned level = 0;                     // piano-video
  unsigned cycles = 1;                    // piano-video-cycles (V1, V2)
  unsigned holdMs = 50;                   // piano-video-hold-ms (V1, V2)
  unsigned idleMs = kIdleCollapseMs;      // piano-video-idle-ms (V3)
  unsigned resumes = 0;                   // piano-video-resumes (V3)
  bool unload = false;                    // piano-video-unload (V3)
  int stop = 0;                           // piano-video-stop
  uint32_t mmcxFloor = kMmcxDisplayFloor; // piano-video-mmcx-floor
};

// V3's memory, the owner's: the UC region (kUcBytes) as 4 KiB physical pages and a write-combined CPU view; one zeroed scratch page;
// the table's four contiguous 4 KiB pages and their cacheable CPU view.
struct Dma {
  volatile uint32_t *uc = nullptr;
  uint64_t pages[vsmmu::kUcPages] = {};
  uint64_t scratch = 0;
  uint64_t *tables = nullptr;
  uint64_t tablesPa = 0;
  void *owner = nullptr;   // the Hw's bookkeeping
};

// "Off 1.5 s after the last frame" (docs/drivers/video.md section 4.5): the collapse decision, from the last activity.
struct IdleClock {
  uint64_t lastUs = 0;
  uint32_t delayMs = kIdleCollapseMs;
  void touch(uint64_t now) { lastUs = now; }
  uint64_t leftUs(uint64_t now) const
  {
    const uint64_t due = lastUs + uint64_t(delayMs) * 1000;
    return now >= due ? 0 : due - now;
  }
};

struct Report {
  int level = 0, error = kOk, failedStep = 0;
  unsigned cycles = 0, cyclesOk = 0, resumesOk = 0, collapses = 0;
  uint64_t onUsMax = 0, offUsMax = 0, handshakeUs = 0;
  uint32_t ctrlStatus = 0, fsr = 0, debugLines = 0;
  bool version = false;
  char versionText[kHfiVersionChars] = {};
  char summary[240] = {};
};

template <class Hw>
class Bringup {
 public:
  explicit Bringup(Hw &hw) : hw(hw), ops(hw, log), power(ops, res, votes, pst), firmware(ops, fst) {}
  Hw &hw;
  WriteLog log;
  Ops<Hw> ops;
  rpmh::Resources res;
  rpmh::Votes votes;
  PowerState pst;
  Power<Ops<Hw>> power;
  FwState fst;
  Firmware<Ops<Hw>> firmware;
  vsmmu::Plan plan;
  vsmmu::Routed routed;
  vsmmu::Tables tables;
  Dma dma;
  bool dmaHeld = false;
  hfi::Mem mem;
  hfi::Ids ids;
  uint32_t tx[hfi::kBufferWords] = {}, rx[hfi::kBufferWords] = {};
  IdleClock idle;
  Report rep;
  Options opt;

  // ------------------------------------------------------------------------------------------------------------------- run
  int run(const Options &o)
  {
    opt = o;
    rep = Report{};
    rep.level = int(o.level);
    if (o.level < 2 || o.level > 4) return kErrArgument;   // inert below piano-video=2: nothing is read or written
    power.stopAt = o.stop;
    firmware.stopAt = o.stop;
    votes.floorVlvl = o.mmcxFloor;
    idle.delayMs = o.idleMs;
    uint32_t bytes = 0;
    const uint8_t *db = hw.cmdDb(&bytes);
    res = rpmh::resources(db, bytes);
    if (res.error) {
      hw.note("rpmh: cmd-db %s: %s (error %d): nothing powered", db ? "read" : "not readable", res.missing ? res.missing : "-", res.error);
      return finish(kStepRails, kErrRpmh);
    }
    hw.note("rpmh: mxc.lvl %#x (%u levels, top vlvl %#x), mmcx.lvl %#x (%u levels), MM1 %#x; MMCX floor %#x", res.mxc.addr,
            res.mxc.count, res.mxc.vlvl[res.mxc.count - 1], res.mmcx.addr, res.mmcx.count, res.mm1, votes.floorVlvl);
    const int rc = o.level == 2 ? v1() : o.level == 3 ? v2() : v3();
    return finish(rc ? failedStep() : kStepDone, rc);
  }

  int failedStep() const
  {
    return pst.error ? pst.failedStep : fst.error ? fst.failedStep : rep.failedStep;
  }
  int finish(int stepNo, int rc)
  {
    rep.error = rc;
    rep.failedStep = rc ? stepNo : 0;
    if (rc) hw.stage(-stepNo, errorName(rc));
    else hw.stage(kStepDone, "done");
    summarize();
    return rc;
  }
  void summarize()
  {
    char *s = rep.summary;
    const unsigned size = sizeof rep.summary;
    const char *v = rep.level == 2 ? "V1" : rep.level == 3 ? "V2" : rep.level == 4 ? "V3" : "V?";
    if (rep.error) {
      snprintf(s, size, "%s FAILED at step %d: %s; cycles %u/%u; writes %u (%u logged)", v, rep.failedStep, errorName(rep.error),
               rep.cyclesOk, rep.cycles, log.writes, log.logged);
      return;
    }
    if (rep.level == 4)
      snprintf(s, size, "V3 ok: firmware %.48s; handshake %llu us; collapse/resume %u/%u; fsr %08x; fw lines %u; writes %u (%u logged)",
               rep.versionText, (unsigned long long)rep.handshakeUs, rep.resumesOk, opt.resumes, rep.fsr, rep.debugLines, log.writes,
               log.logged);
    else
      snprintf(s, size, "%s ok: %u/%u cycles; on max %llu us, off max %llu us; rpmh requests %u; writes %u (%u logged)", v,
               rep.cyclesOk, rep.cycles, (unsigned long long)rep.onUsMax, (unsigned long long)rep.offUsMax, votes.requests,
               log.writes, log.logged);
  }

  // ------------------------------------------------------------------------------------------------------------ V1 pieces
  int on()
  {
    const uint64_t t0 = hw.nowUs();
    const int rc = power.on();
    const uint64_t us = hw.nowUs() - t0;
    if (us > rep.onUsMax) rep.onUsMax = us;
    return rc;
  }
  int off()
  {
    const uint64_t t0 = hw.nowUs();
    const int rc = power.off();
    const uint64_t us = hw.nowUs() - t0;
    if (us > rep.offUsMax) rep.offUsMax = us;
    return rc;
  }
  // The level 1 survey with the gate open; V1's verdict.
  const char *readout(bool firmwareExpected)
  {
    hw.stage(kStepReadout, "the readout with the gate open");
    unsigned loads = 0;
    auto ld = [this](uint64_t pa) { return ops.rd(pa); };
    const Clocks c = surveyClocks(ld, 1, &loads);
    const Core r = surveyCore(c, ld, &loads);
    rep.ctrlStatus = r.ctrlStatus;
    hw.note("readout: pll0 %s %llu MHz, mvs0 %llu MHz (OPP %d); gdsc mvs0c %08x mvs0 %08x; core version %08x ctrl status %08x power "
            "status %08x (codec %s) intr mask %08x tz cpu %08x",
            pllLocked(c.pllMode) ? "locked" : "NOT locked", (unsigned long long)(pllHz(c.pllL, c.pllAlpha) / 1000000),
            (unsigned long long)(mvs0Hz(c) / 1000000), oppIndex(mvs0Hz(c)), c.mvs0cGdsc, c.mvs0Gdsc, r.hwVersion, r.ctrlStatus,
            r.powerStatus, (r.powerStatus & kCorePowered) ? "powered" : "collapsed", r.intrMask, r.tzCpuStatus);
    if (firmwareExpected) return c.codec && r.codecRead ? nullptr : "the codec's gate is closed after the power-on";
    return poweredVerdict(c, r);
  }

  int v1()
  {
    hw.note("V1: %u power cycle(s) at 240 MHz, %u ms powered each", opt.cycles, opt.holdMs);
    for (unsigned c = 1; c <= opt.cycles; ++c) {
      ++rep.cycles;
      int rc = on();
      if (rc) return rc;
      const char *bad = readout(false);
      if (opt.stop == kStepReadout) {
        off();
        return fail(kStepReadout, kErrStopped);
      }
      hw.sleepMs(opt.holdMs);
      const int offRc = off();
      if (bad) {
        hw.note("V1: cycle %u: %s", c, bad);
        return fail(kStepReadout, kErrReadout);
      }
      if (offRc) return fail(kStepOffCheck, offRc);
      ++rep.cyclesOk;
      if (c == 1 || c == opt.cycles || c % 5 == 0)
        hw.note("V1: cycle %u/%u ok (on max %llu us, off max %llu us; %u writes, %u logged)", c, opt.cycles,
                (unsigned long long)rep.onUsMax, (unsigned long long)rep.offUsMax, log.writes, log.logged);
    }
    return kOk;
  }
  int fail(int stepNo, int rc)
  {
    rep.failedStep = stepNo;
    pst.error = kOk;   // the step is ours, not the power sequence's
    fst.error = kOk;
    return rc;
  }

  // ----------------------------------------------------------------------------------------------------------------- V2
  int v2()
  {
    hw.note("V2: %u cycle(s): power on, PAS 9 with the pinned file (the boot's addon or the ESP), MP_VIDEO_VAR, %u ms, PAS shutdown, "
            "power off", opt.cycles, opt.holdMs);
    for (unsigned c = 1; c <= opt.cycles; ++c) {
      ++rep.cycles;
      int rc = on();
      if (rc) return rc;
      rc = firmware.load();
      if (rc) {
        off();
        return rc;
      }
      const uint32_t status = ops.rd(kIris + kCtrlStatus), tz = ops.rd(kIris + kTzCpuStatus);
      hw.note("V2: cycle %u: the firmware is authenticated; CTRL_STATUS %08x (no HFI yet), tz cpu status %08x", c, status, tz);
      hw.sleepMs(opt.holdMs);
      const int un = firmware.unload();
      const int offRc = off();
      if (un) return fail(kStepPasShutdown, un);
      if (offRc) return fail(kStepOffCheck, offRc);
      ++rep.cyclesOk;
    }
    return kOk;
  }

  // ----------------------------------------------------------------------------------------------------------------- V3
  // Everything down: PAS shutdown while powered, the power-off, the bank and its memory.
  void teardown()
  {
    if ((fst.loaded || fst.initDone) && pst.on) firmware.unload();
    else if (fst.loaded) hw.note("teardown: the core is collapsed: PAS 9 is left to the next boot (no shutdown without power)");
    off();
    if (routed.bank || routed.group[0] || routed.group[1]) {
      hw.stage(kStepSmmuTeardown, "the video streams released, the bank back as found");
      if (!vsmmu::teardown(plan, ops, routed)) hw.note("teardown: an SMMU TLB sync did not finish");
    }
    if (dmaHeld) {
      hw.dmaFree(dma);
      dmaHeld = false;
    }
  }
  int v3fail(int stepNo, int rc)
  {
    char sfr[96];
    if (mem.uc && pst.on) {
      hfi::sfrText(mem, sfr, sizeof sfr);
      if (sfr[0]) hw.note("hfi: SFR: %s", sfr);
    }
    if (routed.bank) checkFault();   // a fault on the video bank says why the firmware did not answer
    hw.note("V3: %s at step %d: powering everything down", errorName(rc), stepNo);
    teardown();
    return fail(stepNo, rc);
  }

  int smmuFail(int stepNo, int rc)
  {
    rep.failedStep = stepNo;
    return rc;
  }
  int smmuSetup()
  {
    hw.stage(kStepSmmuPlan, "SMMU: two free groups and a free bank for 0x1940/0x1947");
    plan = vsmmu::plan([this](uint32_t off) { return ops.rd(kSmmu + off); });
    hw.note("smmu: idr0 %08x idr1 %08x idr2 %08x scr0 %08x: %u groups (%u free), %u banks; groups %d/%d bank %d PASize %u%s%s", plan.idr0,
            plan.idr1, plan.idr2, plan.scr0, plan.groups, plan.freeGroups, plan.banks, int(plan.group[0]), int(plan.group[1]),
            int(plan.bank), plan.pasize, plan.error ? ": " : "", plan.error ? plan.error : "");
    if (plan.error) return smmuFail(kStepSmmuPlan, kErrSmmu);
    hw.stage(kStepSmmuTables, "the UC region's memory and our table");
    if (!hw.dmaAlloc(dma)) return smmuFail(kStepSmmuTables, kErrNoMemory);
    dmaHeld = true;
    tables.cpu = dma.tables;
    tables.pa = dma.tablesPa;
    if (!vsmmu::build(tables, dma.pages, vsmmu::kUcPages, dma.scratch)) {
      hw.note("smmu: a page of the UC region or the table is not a 4 KiB page of XNU's RAM");
      return smmuFail(kStepSmmuTables, kErrSmmu);
    }
    hw.cleanToPoc(tables.cpu, vsmmu::kTablePages * 4096);
    for (unsigned i = 0; i < vsmmu::kWindowPages; ++i) {   // the self-check: every page of the window as the SMMU will walk it
      const uint64_t want = i < vsmmu::kUcPages ? dma.pages[i] : dma.scratch;
      if (vsmmu::walk(tables, kIovaBase + 4096 * i + 0x10) != want + 0x10) {
        hw.note("smmu: the table's walk of IOVA %#x does not give its page", kIovaBase + 4096 * i);
        return smmuFail(kStepSmmuTables, kErrSmmu);
      }
    }
    hw.note("smmu: table at %#llx maps IOVA %#x + %#x (%u pages of the UC region, the rest on scratch %#llx)",
            (unsigned long long)tables.pa, kIovaBase, kUcRegionBytes, vsmmu::kUcPages, (unsigned long long)dma.scratch);
    if (opt.stop == kStepSmmuTables) return smmuFail(kStepSmmuTables, kErrStopped);
    hw.stage(kStepSmmuRoute, "SMMU: the bank, then the two streams through it");
    if (const char *why = vsmmu::apply(plan, tables.pa, ops, routed)) {
      hw.note("smmu: %s", why);
      return smmuFail(kStepSmmuRoute, kErrSmmu);
    }
    if (opt.stop == kStepSmmuRoute) return smmuFail(kStepSmmuRoute, kErrStopped);
    hw.stage(kStepQueues, "the queue table, three queues and the SFR");
    mem.uc = dma.uc;
    mem.iova = kIovaBase;
    hfi::initQueues(mem);
    hw.barrier();
    return kOk;
  }

  void ring() { ops.wr(kIris + kCpuIcSoftInt, kSoftIntH2a, "IC_SOFTINT: doorbell"); }
  int send(uint32_t bytes, const char *what)
  {
    const int rc = hfi::write(mem, hfi::kCmdq, tx, bytes, [this] { hw.barrier(); });
    hw.note("hfi: %s: %u bytes, header %u, %u packet(s)%s", what, bytes, tx[2], tx[7], rc ? ": the command queue has no room" : "");
    if (rc) return kErrHfi;
    ring();
    return kOk;
  }
  void drainDebug()
  {
    uint32_t bytes = 0;
    char line[120];
    for (unsigned n = 0; n < 64; ++n) {
      const int rc = hfi::read(mem, hfi::kDbgq, rx, sizeof rx, &bytes, [this] { hw.barrier(); });
      if (rc == 1) return;
      if (rc || !hfi::debugText(rx, bytes, line, sizeof line)) continue;
      ++rep.debugLines;
      if (rep.debugLines <= 32 || rep.debugLines % 64 == 0) hw.note("fw: %s", line);
    }
  }
  // The message queue until what is wanted came, an error, or ms. kOk, kErrHfi, kErrSysError or kErrHfiTimeout.
  int waitFor(bool wantInit, bool wantVersion, hfi::Reply &r, uint32_t ms)
  {
    const uint64_t start = hw.nowUs();
    for (;;) {
      uint32_t bytes = 0;
      for (unsigned n = 0; n < 64; ++n) {
        const int rc = hfi::read(mem, hfi::kMsgq, rx, sizeof rx, &bytes, [this] { hw.barrier(); });
        if (rc == 1) break;
        if (rc == -2) {
          hw.note("hfi: an oversized message was dropped");
          continue;
        }
        if (rc || !hfi::parse(rx, bytes, r)) {
          hw.note("hfi: a malformed message (read %d, %u bytes, word 0 %08x)", rc, bytes, rx[0]);
          return kErrHfi;
        }
      }
      drainDebug();
      if (r.sysError) {
        hw.note("hfi: system error %08x (flags %08x)", r.errorType, r.errorFlags);
        return kErrSysError;
      }
      if (r.initFailed) {
        hw.note("hfi: the firmware answered INIT without success");
        return kErrHfi;
      }
      if ((!wantInit || r.initDone) && (!wantVersion || r.version)) return kOk;
      if (hw.nowUs() - start >= uint64_t(ms) * 1000) {
        hw.note("hfi: no answer within %u ms (init %s, version %s; %u messages)", ms, r.initDone ? "done" : "not answered",
                r.version ? "read" : "not read", r.headers);
        return kErrHfiTimeout;
      }
      hw.sleepMs(1);
    }
  }
  void noteVersion(const hfi::Reply &r)
  {
    unsigned n = 0;
    while (n < sizeof rep.versionText - 1 && r.text[n]) {
      rep.versionText[n] = r.text[n];
      ++n;
    }
    while (n && rep.versionText[n - 1] == ' ') --n;
    rep.versionText[n] = 0;
    rep.version = true;
    hw.note("hfi: firmware version: %s", rep.versionText);
  }
  int checkFault()
  {
    const vsmmu::Fault f = vsmmu::fault(plan, ops);
    rep.fsr = f.fsr;
    if (!(f.fsr & vsmmu::kFsrFaults)) return kOk;
    hw.note("smmu: FAULT on the video bank: fsr %08x far %08x%08x fsynr0 %08x", f.fsr, f.farHi, f.farLo, f.fsynr0);
    return kErrFault;
  }
  // The boot words and hardware mode (iris_core_init / iris_hfi_pm_resume after the power-on).
  int bootCore()
  {
    hw.stage(kStepBoot, "the boot words: UC region, queue table, SFR, SPARE_0, CTRL_INIT");
    uint32_t status = 0;
    const int rc = hfi::boot(ops, kIovaBase, &status);
    hw.stage(kStepCtrlStatus, "CTRL_STATUS");
    rep.ctrlStatus = status;
    hw.note("hfi: CTRL_STATUS %08x: %s", status,
            rc == kOk ? "init done"
            : (status & kCtrlStatusErrorMask) == kCtrlStatusErrorUcRegion ? "error 0x4: the UC region is refused"
            : status ? "an error code" : "no answer after 1000 polls (100 ms)");
    if (rc) return rc;
    hw.stage(kStepHwMode, "MVS0 GDSC to hardware mode");
    power.hardwareMode();
    return kOk;
  }
  // iris_vpu_prepare_pc + SET_REMOTE_STATE(0) + the power-off: the firmware stays.
  int collapse()
  {
    hw.stage(kStepPcPrep, "prepare power collapse");
    bool ready = false;
    for (unsigned attempt = 0; attempt < 10 && !ready; ++attempt) {
      const uint32_t s = ops.rd(kIris + kCtrlStatus);
      if (s & kCtrlStatusPcReady) {
        ready = true;
        break;
      }
      const uint32_t wfi = ops.rd(kIris + kTzCpuStatus) & 1u;
      if (!wfi || !(s & kCtrlStatusIdle)) {   // Linux: "skip power collapse" (-EAGAIN), tried again later
        hw.note("hfi: not idle yet (ctrl %08x, wfi %u): collapse tried again in 100 ms", s, wfi);
        hw.sleepMs(100);
        continue;
      }
      if (send(hfi::pcPrep(tx, ids), "SYS_PC_PREP")) return kErrHfi;
      ready = ops.poll(kIris + kCtrlStatus, kCtrlStatusPcReady, kCtrlStatusPcReady, kPcPollUs, kPcStepUs) &&
              ops.poll(kIris + kTzCpuStatus, 1, 1, kPcPollUs, kPcStepUs);
      if (!ready) break;
    }
    if (!ready) return kErrPc;
    if (opt.stop == kStepPcPrep) return kErrStopped;
    hw.stage(kStepRemoteSuspend, "SET_REMOTE_STATE(0, 0)");
    if (hw.scmRemoteState(kRemoteStateSuspend)) return kErrScm;
    hw.stage(kStepCollapsed, "power off (the firmware stays in the carveout)");
    const int rc = off();
    if (rc) return rc;
    ++rep.collapses;
    hw.note("V3: collapsed after %u ms idle (collapse %u); the firmware kept", opt.idleMs, rep.collapses);
    return kOk;
  }
  // iris_hfi_pm_resume: on, SET_REMOTE_STATE(1), the boot words, hardware mode, interframe power collapse; then a version ping.
  int resume(bool ping)
  {
    hw.stage(kStepResume, "resume: power on");
    int rc = on();
    if (rc) return rc;
    hw.stage(kStepRemoteResume, "SET_REMOTE_STATE(1, 0)");
    if (hw.scmRemoteState(kRemoteStateResume)) return kErrScm;
    hw.stage(kStepReboot, "the boot words again (no PAS: the firmware stayed)");
    if ((rc = bootCore())) return rc;
    if ((rc = send(hfi::interframePowerCollapse(tx, ids), "INTRA_FRAME_POWER_COLLAPSE"))) return rc;
    if (!ping) return kOk;
    hw.stage(kStepPing, "IMAGE_VERSION as a ping");
    if ((rc = send(hfi::imageVersion(tx, ids), "IMAGE_VERSION"))) return rc;
    hfi::Reply r;
    if ((rc = waitFor(false, true, r, kHfiResponseMs))) return rc;
    return checkFault();
  }
  void waitIdle()
  {
    hw.stage(kStepIdle, "idle: the collapse delay after the last activity");
    idle.touch(hw.nowUs());
    for (uint64_t left; (left = idle.leftUs(hw.nowUs())) > 0;) {
      const uint64_t ms = (left + 999) / 1000;
      hw.sleepMs(uint32_t(ms > 100 ? 100 : ms));
    }
  }

  int v3()
  {
    hw.note("V3: SMMU bank + UC region, power on, PAS 9, HFI boot and handshake; collapse after %u ms idle; %u resume(s)%s", opt.idleMs,
            opt.resumes, opt.unload ? "; then unload" : "");
    ++rep.cycles;
    int rc = smmuSetup();
    if (rc) return v3fail(rep.failedStep, rc);
    if ((rc = on())) return v3fail(pst.failedStep, rc);
    if ((rc = firmware.load())) return v3fail(fst.failedStep, rc);
    if ((rc = bootCore())) return v3fail(kStepCtrlStatus, rc);
    if (opt.stop == kStepHwMode) return v3fail(kStepHwMode, kErrStopped);
    hw.stage(kStepHfiSend, "SYS_INIT, IMAGE_VERSION, INTRA_FRAME_POWER_COLLAPSE");
    const uint64_t t0 = hw.nowUs();
    if ((rc = send(hfi::sysInit(tx, ids), "SYS_INIT (+7 UBWC properties)")) || (rc = send(hfi::imageVersion(tx, ids), "IMAGE_VERSION")) ||
        (rc = send(hfi::interframePowerCollapse(tx, ids), "INTRA_FRAME_POWER_COLLAPSE")))
      return v3fail(kStepHfiSend, rc);
    hw.stage(kStepHfiWait, "the answers (1 s)");
    hfi::Reply r;
    if ((rc = waitFor(true, true, r, kHfiResponseMs))) return v3fail(kStepHfiWait, rc);
    rep.handshakeUs = hw.nowUs() - t0;
    hw.note("hfi: INIT answered with success after %llu us (%u messages, %u packets)", (unsigned long long)rep.handshakeUs, r.headers,
            r.packets);
    noteVersion(r);
    if ((rc = checkFault())) return v3fail(kStepHfiWait, rc);
    if (opt.stop == kStepHfiWait) return v3fail(kStepHfiWait, kErrStopped);
    waitIdle();
    if ((rc = collapse())) return v3fail(kStepPcPrep, rc);
    for (unsigned i = 1; i <= opt.resumes; ++i) {
      if ((rc = resume(true))) return v3fail(kStepResume, rc);
      waitIdle();
      if ((rc = collapse())) return v3fail(kStepPcPrep, rc);
      ++rep.resumesOk;
      if (i == 1 || i == opt.resumes || i % 5 == 0) hw.note("V3: collapse/resume %u/%u ok", i, opt.resumes);
    }
    if (opt.unload) {   // iris_core_deinit: resumed first, then PAS shutdown, then off
      hw.stage(kStepUnload, "unload: resume, PAS shutdown, power off, the bank released");
      if ((rc = resume(false))) return v3fail(kStepUnload, rc);
      teardown();
      hw.note("V3: unloaded: PAS 9 shut down, the core off, the video streams released");
    } else {
      hw.note("V3: steady state: the core collapsed, the firmware in the carveout, the bank and the UC region kept");
    }
    return kOk;
  }
};

}  // namespace pianovideo
