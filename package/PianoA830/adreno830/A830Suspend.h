/* SPDX-License-Identifier: MIT */
#ifndef A830_SUSPEND_H
#define A830_SUSPEND_H
#include <stddef.h>
#include <stdint.h>

/* UniXNU-piano (macOS 27, compiled with ADRENO830_MACOS): the GPU's part of a system suspend, piano-gpu-suspend=1 (UniXNU-piano
 * docs/drivers/gpu.md "System suspend", docs/power.md §4, package/include/PianoPowerContract.h). Four steps, each on the thread
 * the system runs it on, in this order:
 *   1. kOff (setPowerState, the power tree's thread): the submission gate shut, the GPU quiet within idleWaitMs (no holder,
 *      everything retired, a recovery in progress waited out), then A830PowerState's full slumber (PREPARE_SLUMBER, the RSCC
 *      sleep that drops the GMU's own GX, CX and DDR votes through the GPU's RSC, GBIF halt, the CX power-off). The GPU then
 *      stays asleep: no submission, touch, screen or IFPC wake, and no idle vote, until step 4 (A830PowerState::suspend).
 *   2. the platform sleep action (IOCPUSleepKernel's PreSleep, after every driver of the power tree acknowledged its kOff): the
 *      host votes, under the orchestrator's host-vote mutex and the DRV-2 lock (Sm8750RpmhWriteActive): the DDR (SH0, MC0, ACV)
 *      to the keepalive minimum, or to the display's floor while the panel is still scanning out, then CX and MX to railVlvl
 *      (0: hlvl 0, the votes gone; never below the lit panel's NOM_L1 while it scans; never above what is held). These host
 *      votes are the only HLOS votes for CX, MX and the DDR on this system (UFS, the display and the CPU have none of their own),
 *      which is why they wait for PreSleep and why the rails' default is NOM_L1 (A830Idle.h's idle level, what UFS at HS-G5, USB
 *      and the lit panel need) and 0 only on request. CN0 stays (the CPU's config-NoC keepalive; only a SLEEP set drops it).
 *   3. the platform wake action (PostResume, before any driver's kOn): the rails back to what was held, then the DDR.
 *   4. kOn: the votes again if step 3 did not run or failed, the gate open and, with eagerWake, the GPU woken at once (its first
 *      frame then waits for nothing; the wake's time is reported against wakeBoundMs).
 * A failure in 1 leaves the GPU as it was (busy: nothing changed) or with the recovery (a failed slumber entry: kOff waits for it
 * to bring the GPU back), never half down, and ends the GPU's participation for the boot; the system suspend goes on without the
 * GPU's share (k27 0062 has no cancel a driver could ask for). A failure in 2 puts back what went down; in 3 it is tried again in 4.
 * The votes stay ACTIVE-only: what persists through the sleep is these lowered values (no SLEEP/WAKE TCS is written here). */

/* The display's scanout as the power contract reads it (PianoPowerContract.h pianopower::displayScanout). */
enum A830Scanout : uint8_t {A830ScanoutUnknown,A830ScanoutRunning,A830ScanoutStopped};

/* What CX/MX keep while the panel scans out: NOM_L1 (A830Idle.h: the boot framebuffer's MMCX, whose parent domain is CX). */
enum : uint32_t {A830SuspendDisplayRailVlvl=0x140};

struct A830SuspendConfig {
    bool enabled=false;          /* piano-gpu-suspend=1 */
    uint32_t idleWaitMs=500;     /* piano-gpu-suspend-idle-ms: quiet within this or the suspend goes on without the GPU (50..2000) */
    uint32_t railVlvl=0x140;     /* piano-gpu-suspend-rails: host CX/MX while suspended (RPMh vlvl); 0: the rails' votes gone */
    uint8_t ddrFloorLevel=5;     /* the display's DDR floor while it scans (piano-gpu-idle-ddr's level) */
    bool eagerWake=true;         /* piano-gpu-suspend-wake: wake the GPU in kOn (0: the first submission wakes it) */
    uint32_t wakeBoundMs=100;    /* piano-gpu-suspend-wake-ms: a wake slower than this is reported as over the bound */
    uint32_t displayWaitMs=300;  /* PreSleep: how long to wait for PianoDpu's panel sleep to finish (it runs on its own thread) */
    bool cancel=false;           /* piano-gpu-suspend-cancel=1 (UniXNU-piano, 2026-11-12): a failed entry cancels the sleep (0064) */
    uint32_t failTest=0;         /* piano-gpu-suspend-fail-test=N: suspend N counts as a failed entry, the GPU untouched (a test) */
};

/* The DDR vote while suspended: 0 kBps (Sm8750RpmhHostDdr's keepalive minimum: SH0/MC0 x = y = 1, ACV off) once the scanout
 * stopped; else the display's floor, never above what is held (a held vote below the floor stays). */
static inline uint32_t A830SuspendDdrKbps(uint32_t heldKbps,uint8_t scanout,uint32_t floorKbps)
{
    if(scanout==A830ScanoutStopped)return 0;
    return floorKbps && floorKbps<heldKbps?floorKbps:heldKbps;
}

/* The rails' RPMh level while suspended: the configured one, but at least the lit panel's while the scanout is not known stopped. */
static inline uint32_t A830SuspendRailVlvl(uint32_t railVlvl,uint8_t scanout)
{
    if(scanout==A830ScanoutStopped)return railVlvl;
    return railVlvl>A830SuspendDisplayRailVlvl?railVlvl:A830SuspendDisplayRailVlvl;
}

/* The hlvl of one ARC (its vlvl table, `count` levels) for `vlvl`: 0 for vlvl 0 (the vote gone), else the first level reaching
 * vlvl, never above the level held; a table without such a level keeps the held one. */
static inline uint32_t A830SuspendRailHlvl(const uint16_t *table,uint8_t count,uint32_t heldHlvl,uint32_t vlvl)
{
    if(!vlvl)return 0;
    for(uint32_t h=0;table && h<count;++h)
        if(table[h]>=vlvl)return h<heldHlvl?h:heldHlvl;
    return heldHlvl;
}

/* Where the suspend is (the board's, one per boot; kOff, PreSleep, PostResume and kOn run one after the other). */
enum A830SuspendPhase : uint8_t {A830SuspendUp,A830SuspendGpuDown,A830SuspendVotesDown};
/* kOff's outcome. */
enum A830SuspendOutcome : uint8_t {
    A830SuspendOk,          /* the GPU is down (full slumber, gate shut) */
    A830SuspendOff,         /* not participating: piano-gpu-suspend=0, the GPU not running, or blocked by an earlier failure */
    A830SuspendBusy,        /* still busy after idleWaitMs: nothing changed */
    A830SuspendFailed,      /* the slumber entry failed: the recovery has the GPU (kOff waited for it); participation off for the boot */
    A830SuspendLost,        /* the GPU is lost: nothing of it is touched, its votes stay */
};

struct A830SuspendMachine {
    uint8_t phase=A830SuspendUp;
    bool blocked=false;          /* a failed entry: no participation for the rest of the boot */
    uint8_t last=A830SuspendOff; /* the last kOff outcome */
    uint32_t suspends=0,refusals=0,voteDowns=0,voteFailures=0,wakes=0,wakeFailures=0,overBound=0;
    uint32_t lastWakeUs=0,maxWakeUs=0;
    bool sleepWanted() const {return phase==A830SuspendUp && !blocked;}
    void sleepDone(uint8_t outcome)
    {
        last=outcome;
        if(outcome==A830SuspendOk) {phase=A830SuspendGpuDown;++suspends;return;}
        if(outcome!=A830SuspendOff)++refusals;
        if(outcome==A830SuspendFailed)blocked=true;
    }
    /* UniXNU-piano (2026-11-12, docs/power.md 3.7): the outcome a system sleep must not go on with, for the board's opt-in cancel
     * (piano-gpu-suspend-cancel=1, k27 0064's piano-sleep-cancel=1): a failed slumber entry (the recovery had the GPU, possibly still
     * running it when the CPUs would freeze). Busy, lost or off cost only the GPU's share of the saving: no cancel. */
    static bool cancelWanted(uint8_t outcome) {return outcome==A830SuspendFailed;}
    bool votesDownWanted() const {return phase==A830SuspendGpuDown;}
    void votesDownDone(bool ok) {if(ok) {phase=A830SuspendVotesDown;++voteDowns;} else ++voteFailures;}
    bool votesUpWanted() const {return phase==A830SuspendVotesDown;}
    void votesUpDone(bool ok) {if(ok)phase=A830SuspendGpuDown; else ++voteFailures;}
    /* kOn: the GPU comes back only with its votes back (a GPU powered up on rails that went to 0 is not safe: Sm8750GpuPowerOn
     * needs the host's cx/mx votes held). */
    bool wakeWanted() const {return phase==A830SuspendGpuDown;}
    void wakeDone(bool ok,uint32_t us,uint32_t boundMs)
    {
        phase=A830SuspendUp;
        if(!ok) {++wakeFailures;return;}
        ++wakes;lastWakeUs=us;
        if(us>maxWakeUs)maxWakeUs=us;
        if(boundMs && us>uint64_t(boundMs)*1000u)++overBound;
    }
};

/* One step's evidence (the orchestrator fills it; the board prints it). Rails as RPMh vlvl, DDR in kBps. */
struct A830SuspendReport {
    int32_t result;              /* IOReturn of the step */
    int32_t rpmh;                /* Sm8750Rpmh status of a failed vote (0 none) */
    uint8_t powerPhase;          /* A830PowerPhase after the step */
    uint32_t waitedUs,entryUs,wakeUs,votesUs;
    uint32_t warnings;           /* Sm8750GpuPowerOff warnings of the entry (Sm8750GpuOffCxStillOn: another voter kept CX) */
    uint32_t ddrBefore,ddrAfter,cxBefore,cxAfter,mxBefore,mxAfter;
    bool cxCollapsed;            /* the entry's CX power-off completed */
    bool alreadyAsleep;          /* the GPU was in a full slumber already (piano-gpu-idle=2) */
};

#ifdef __cplusplus
extern "C" {
#endif
/* The orchestrator's steps (Adreno830Start.cpp; thread context; NotReady before S10 bound the power state). */
int32_t adreno830_suspend_gpu(uint32_t idleWaitMs,A830SuspendReport *report);
int32_t adreno830_suspend_votes(uint8_t scanout,const A830SuspendConfig *config,A830SuspendReport *report);
int32_t adreno830_resume_votes(A830SuspendReport *report);
int32_t adreno830_resume_gpu(bool wake,A830SuspendReport *report);
/* The host votes as held now (for lines; vlvl and kBps), and whether a suspend's votes are down. */
void adreno830_host_votes(uint32_t *ddrKbps,uint32_t *cxVlvl,uint32_t *mxVlvl,bool *votesDown);
#ifdef __cplusplus
}
#endif
#endif
