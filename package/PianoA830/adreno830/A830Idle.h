/* SPDX-License-Identifier: MIT */
#ifndef A830_IDLE_H
#define A830_IDLE_H
#include <stddef.h>
#include <stdint.h>

/* UniXNU-piano (macOS 27, compiled with ADRENO830_MACOS): the GPU's idle power policy, piano-gpu-idle (docs/drivers/gpu.md §6).
 *
 * What the driver held all day before: P0 (no slumber), the DCVS's wake boost to the ceiling for 50 ms on every submission after 64 ms
 * of quiet (a login screen at ~10 frames a second spent 47 % of its time at 342 MHz for 2.5 ms of work a frame), the DCVS service
 * thread awake every 16 ms window whether or not anything ran, and the host votes on apps DRV-2 that nothing ever lowers: CX and MX at
 * the top of their cmd-db tables and DDR at bus level 11 (18.6 GB/s, the table's top), with ACV. Those host votes are the HLOS's only
 * votes on this system (ARCHITECTURE 4.3.3: UFS and MDP have none of their own), so they keep the DDR at its top frequency and the
 * CX/MX rails at their top voltage for everything, all the time.
 *
 * The levels:
 *   0: as before.
 *   1: the DCVS floor: no wake boost (an isolated frame runs at the floor: 2.5 ms of 342 MHz work is about 5 ms at 160 MHz, inside a
 *      120 Hz frame; a sustained load still climbs through the governor's up rules within a window or two), and while the governor sits
 *      idle at the floor its window is windowMs (250 ms) instead of 16 ms: the first submission wakes the thread anyway
 *      (A830Dcvs::noteSubmit), so nothing waits for that window.
 *   2: 1, and the boot bandwidth vote goes once the GMU votes the GPU's own (Linux on this GPU drops its boot vote the same way; a
 *      kept one holds the DDR at its top speed): at S10 the host DDR vote falls to the display's floor for the rest of the boot
 *      (unless pagani-gpu-ddr-floor chose a floor), the GMU adding what each GPU level needs (its bus table) while the GPU runs.
 *      And after idleMs (1000 ms) with nothing submitted, nothing in flight and no holder: the idle votes, the host DDR vote at the
 *      display's floor (when a chosen floor is above it) and the host CX/MX votes at railVlvl (NOM_L1; never raised by this), held
 *      until the next submission, and with slumber (piano-gpu-idle-slumber=1) the GPU's full slumber (A830PowerState P1, CX
 *      collapsed, the GMU's own votes dropped through its RSC's sleep) after the same idleMs.
 *      The GPU needs none of the host votes for its own work: the GMU votes its GX, CX and DDR through the GPU's RSC, so the host
 *      votes go back up (rails first, then DDR) on the DCVS thread right after the first submission and the GPU's work does not wait
 *      for them; a wake from slumber starts at the host's idle rails, which are above every CX level the GMU's own table asks (its
 *      500/650 MHz at LOW_SVS/SVS).
 *      The DDR floor: what the panel's scanout reads from DDR (the GOP framebuffer, width x height x 4 bytes at 120 Hz: 3.28 GB/s at
 *      3200x2136) twice over (the fetch is bursty, and the CPU copies frames into it), at the lowest bus level that holds it: level 5,
 *      6.67 GB/s, on piano. Linux on this tablet (the Ubuntu port's DT, read as facts) votes the boot framebuffer's display path at
 *      3.28 GB/s average, 4.26 GB/s peak (the 2310-line vertical total), and its DPU at least 0.8 GB/s peak: level 5 is above all three.
 *      NOM_L1 for CX and MX: the same DT holds MMCX at NOM_L1 for the boot framebuffer (the MDP at 575 MHz), and on SM8750 MMCX has CX
 *      as its parent domain, so Linux keeps CX at NOM_L1 while the panel is lit; UFS at HS-G5 (403 MHz), USB and PCIe Gen3 need NOM.
 *      MMCX itself is never voted by this driver (what the boot stages voted stays). A failed vote ends the idle votes for the boot (the
 *      held votes are put back first, best effort); a failed slumber entry or wake ends the slumber for the boot (A830PowerState, after
 *      the recovery).
 * Slumber and the lowered host votes never ran on any hardware before piano (Adreno830 PROGRESS: the slumber rounds of M13 were never
 * run on the 13T; Linux slumbers this GPU 66 ms after its last work and runs IFPC): level 1 is the default; level 2 is the A/B's
 * (docs/drivers/gpu.md, "GPU idle power"). */

struct A830IdleConfig {
    uint8_t mode=0;            /* piano-gpu-idle: 0, 1 or 2 (above) */
    bool slumber=true;         /* piano-gpu-idle-slumber: with mode 2, also the full slumber */
    uint32_t idleMs=1000;      /* piano-gpu-idle-ms: quiet this long -> the idle votes (and slumber) */
    uint32_t windowMs=250;     /* the DCVS window while the governor idles at the floor */
    uint8_t ddrLevel=5;        /* piano-gpu-idle-ddr: the host DDR bus level while idle (the board computes the default) */
    uint32_t railVlvl=0x140;   /* piano-gpu-idle-rails: host CX/MX vlvl while idle (0x140 NOM_L1); 0 = keep the rails */
};

/* The display scanout's DDR floor: the lowest level of the bus table (kBps, ascending) holding the scanout's read rate (width x height x
 * bytesPerPixel x hz) times marginPermille / 1000; the top level when none does. *scanoutKbps (may be null) gets the scanout's own rate. */
static inline uint8_t A830IdleScanoutLevel(const uint32_t *kbps,uint8_t levels,uint32_t width,uint32_t height,uint32_t bytesPerPixel,
    uint32_t hz,uint32_t marginPermille,uint32_t *scanoutKbps)
{
    const uint64_t rate=uint64_t(width)*height*bytesPerPixel*hz/1000;   /* kBps */
    const uint64_t need=rate*marginPermille/1000;
    if(scanoutKbps)*scanoutKbps=rate>0xffffffffu?0xffffffffu:uint32_t(rate);
    if(!kbps || !levels)return 0;
    for(uint8_t level=0;level<levels;++level)
        if(kbps[level]>=need)return level;
    return uint8_t(levels-1);
}

/* (The idle rails' level arithmetic is Sm8750RpmhHostRailsIdle's.) What the DCVS service thread does with the host votes at each look. */
enum A830IdleAction : uint8_t {A830IdleKeep,A830IdleDown,A830IdleUp};

/* The idle votes' state (the DCVS thread's alone). step: quiet = nothing in flight and no holder, the GPU up or asleep (not being
 * recovered); lastActivity = the newest submission's time (ns, may be later than now: it was stamped after now was read). Returns the
 * action and sets *next to the time of the next look it needs (0: none). done: the action's outcome; a failure blocks the idle votes for
 * the boot (an Up that failed leaves them recorded down: the caller tries Up again on the next activity, a failed Down is not retried). */
struct A830IdleVotes {
    bool down=false,blocked=false;
    uint64_t downAt=0,activityAtDown=0;
    uint32_t downs=0,ups=0,failures=0;
    A830IdleAction step(uint64_t now,bool quiet,uint64_t lastActivity,uint32_t idleMs,uint64_t *next)
    {
        if(next)*next=0;
        const uint64_t wait=uint64_t(idleMs)*1000000u;
        if(down) {
            /* Any work since the votes went down, or the GPU not quiet (a recovery, work in flight): up at once. */
            if(!quiet || lastActivity!=activityAtDown)return A830IdleUp;
            return A830IdleKeep;
        }
        if(blocked || !quiet)return A830IdleKeep;
        if(lastActivity>now || now-lastActivity<wait) {
            if(next)*next=lastActivity+wait;
            return A830IdleKeep;
        }
        return A830IdleDown;
    }
    void done(A830IdleAction action,bool ok,uint64_t now,uint64_t lastActivity)
    {
        if(action==A830IdleDown) {
            if(ok) {down=true;downAt=now;activityAtDown=lastActivity;++downs;}
            else {blocked=true;++failures;}
        } else if(action==A830IdleUp) {
            if(ok) {down=false;++ups;}
            else {blocked=true;++failures;}
        }
    }
};

/* Which transitions get a console line of their own: the first 8, then every 10th, 100th, ...; the rest go into the minute's summary. */
static inline bool A830IdleLogged(uint32_t count)
{
    if(count<=8)return count!=0;
    uint64_t mark=1;
    while(mark<count)mark*=10;
    return mark==count;
}

/* One transition (or the minute's summary) for the console: the orchestrator decides which (A830IdleLogged), the board prints it as a
 * "PianoA830: gpu idle ..." line (adreno830_board_idle_event). */
enum A830IdleEventKind : uint8_t {
    A830IdleEventDown,      /* the idle votes went down after quietMs */
    A830IdleEventUp,        /* back up after they held downMs */
    A830IdleEventSlumber,   /* the GPU slumbered (A830PowerState P1), entryUs */
    A830IdleEventWake,      /* woke after asleepMs, the wake (what the first submission waited) wakeUs */
    A830IdleEventFailed,    /* a vote failed (result: the Sm8750Rpmh status); the idle votes are off for the boot */
    A830IdleEventSlumberOff,/* a slumber or a wake failed (A830PowerState); P1 is off for the boot */
    A830IdleEventSummary,   /* the last minute: counts, time down, the longest wake */
    A830IdleEventFloor,     /* S10, mode 2: the boot DDR vote dropped to the display's floor (result 0) or could not (result) */
};
struct A830IdleEvent {
    uint8_t kind;
    uint32_t count;               /* the transition's own count since the boot */
    uint64_t ms;                  /* Down: quiet before it; Up: how long the votes were down; Wake: asleep; Summary: the period */
    uint32_t us;                  /* Slumber: the entry; Wake: the wake */
    uint32_t ddrLevel,ddrKbps;    /* the host DDR vote after the transition */
    uint32_t cxVlvl,mxVlvl;       /* the host rails after it */
    int32_t result;               /* Failed */
    uint32_t downs,ups,slumbers,wakes,maxWakeUs,downPermille;   /* Summary: in the period */
};

#ifdef __cplusplus
extern "C" {
#endif
/* Board hooks (board/PianoA830Board.cpp, PianoA830.cpp); weak defaults in Adreno830Start.cpp: mode 0 and no line. */
void adreno830_board_idle(A830IdleConfig *config);
void adreno830_board_idle_event(const A830IdleEvent *event);
#ifdef __cplusplus
}
#endif
#endif
