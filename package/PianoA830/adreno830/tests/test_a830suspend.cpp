/* SPDX-License-Identifier: MIT */
/* host-test: A830PowerState.cpp A830Recovery.cpp A830Dcvs.cpp platform/sm8750/Sm8750Tsens.c
 * UniXNU-piano: the GPU's part of a system suspend (A830Suspend.h, piano-gpu-suspend=1). Both passes: the targets (the DDR at the
 * display's floor while it scans, the keepalive minimum once it stopped, never above what is held; the rails at least NOM_L1 while
 * it scans, 0 only on request and only with the scanout stopped, the hlvl of each table never above the held one) and the suspend's
 * state machine (kOff, PreSleep, PostResume, kOn in every order the system can give them, the refusals, the block after a failed
 * entry, the wake bound). ADRENO830_MACOS pass: A830PowerState's gate against the register model: the full slumber's exact sequence
 * from P0, from a CX-keeping slumber and from a full one already there; while shut no submission, touch, screen or IFPC wake and no
 * tick; a submission waits for the resume without the mutex and fails after suspendWaitMs; work in flight, a holder, a recovery
 * that does not finish and a lost GPU refuse with the gate open again and nothing changed; work that finishes within the wait does
 * not; a failed entry goes to the recovery; resume() with and without the wake, a failed wake to the recovery. */
#include "A830PowerFakes.h"
#include "A830Suspend.h"
#include "A830Profile.h"

static void testTargets()
{
    /* The DDR: level 11's 18.6 GB/s held, the display's level 5 floor. */
    CHECK(A830SuspendDdrKbps(18597656,A830ScanoutRunning,6671875)==6671875);
    CHECK(A830SuspendDdrKbps(18597656,A830ScanoutUnknown,6671875)==6671875);
    CHECK(A830SuspendDdrKbps(18597656,A830ScanoutStopped,6671875)==0);
    CHECK(A830SuspendDdrKbps(5285156,A830ScanoutRunning,6671875)==5285156);   /* below the floor already: kept */
    CHECK(A830SuspendDdrKbps(18597656,A830ScanoutRunning,0)==18597656);       /* no floor known: kept */
    /* The rails' vlvl. */
    CHECK(A830SuspendRailVlvl(0x140,A830ScanoutRunning)==0x140 && A830SuspendRailVlvl(0,A830ScanoutRunning)==0x140);
    CHECK(A830SuspendRailVlvl(0,A830ScanoutUnknown)==0x140 && A830SuspendRailVlvl(0,A830ScanoutStopped)==0);
    CHECK(A830SuspendRailVlvl(0x180,A830ScanoutRunning)==0x180 && A830SuspendRailVlvl(0x80,A830ScanoutStopped)==0x80);
    /* The hlvl on a cx-like table. */
    static const uint16_t cx[16]={0,0x10,0x30,0x38,0x40,0x60,0x80,0xc0,0xd0,0x100,0x120,0x140,0x150,0x180,0x1a0,0x1c0};
    CHECK(A830SuspendRailHlvl(cx,16,15,0)==0);              /* the vote gone */
    CHECK(A830SuspendRailHlvl(cx,16,15,0x140)==11);         /* NOM_L1 */
    CHECK(A830SuspendRailHlvl(cx,16,15,0x141)==12);         /* the first level reaching it */
    CHECK(A830SuspendRailHlvl(cx,16,4,0x140)==4);           /* never above what is held */
    CHECK(A830SuspendRailHlvl(cx,16,15,0x1d0)==15);         /* no such level: kept */
    CHECK(A830SuspendRailHlvl(nullptr,0,9,0x140)==9 && A830SuspendRailHlvl(cx,0,9,0x140)==9);
}

static void testMachine()
{
    /* The whole cycle: kOff, PreSleep, PostResume, kOn. */
    A830SuspendMachine m;
    CHECK(m.sleepWanted() && !m.votesDownWanted() && !m.votesUpWanted() && !m.wakeWanted());
    m.sleepDone(A830SuspendOk);
    CHECK(m.phase==A830SuspendGpuDown && !m.sleepWanted() && m.votesDownWanted() && m.wakeWanted() && m.suspends==1);
    m.votesDownDone(true);
    CHECK(m.phase==A830SuspendVotesDown && m.votesUpWanted() && !m.wakeWanted() && !m.votesDownWanted());
    m.votesUpDone(true);
    CHECK(m.phase==A830SuspendGpuDown && m.wakeWanted());
    m.wakeDone(true,4200,100);
    CHECK(m.phase==A830SuspendUp && m.wakes==1 && m.lastWakeUs==4200 && m.maxWakeUs==4200 && !m.overBound && m.sleepWanted());
    /* kOn without PreSleep (a sleep reverted before the CPUs went down): the GPU comes back, no vote to restore. */
    m.sleepDone(A830SuspendOk);
    CHECK(m.wakeWanted() && !m.votesUpWanted());
    m.wakeDone(true,150000,100);
    CHECK(m.phase==A830SuspendUp && m.overBound==1 && m.maxWakeUs==150000);
    /* A failed PreSleep vote: the votes as they were, the GPU still down; kOn wakes it. */
    m.sleepDone(A830SuspendOk);
    m.votesDownDone(false);
    CHECK(m.phase==A830SuspendGpuDown && m.voteFailures==1 && m.wakeWanted() && !m.votesUpWanted());
    m.wakeDone(true,3000,100);
    /* A failed PostResume restore: kOn tries the votes again before the GPU (no wake while they are down). */
    m.sleepDone(A830SuspendOk);
    m.votesDownDone(true);
    m.votesUpDone(false);
    CHECK(m.phase==A830SuspendVotesDown && m.votesUpWanted() && !m.wakeWanted() && m.voteFailures==2);
    m.votesUpDone(true);
    CHECK(m.wakeWanted());
    m.wakeDone(false,0,100);
    CHECK(m.phase==A830SuspendUp && m.wakeFailures==1 && m.wakes==3);
    /* Refusals: busy (nothing changed, next time again), lost, off (no count); a failed entry blocks the rest of the boot. */
    m.sleepDone(A830SuspendBusy);
    CHECK(m.phase==A830SuspendUp && m.refusals==1 && m.sleepWanted() && m.last==A830SuspendBusy);
    m.sleepDone(A830SuspendOff);
    CHECK(m.refusals==1 && m.sleepWanted());
    m.sleepDone(A830SuspendLost);
    CHECK(m.refusals==2 && m.sleepWanted() && !m.votesDownWanted());
    m.sleepDone(A830SuspendFailed);
    CHECK(m.refusals==3 && m.blocked && !m.sleepWanted() && !m.votesDownWanted() && !m.wakeWanted() && m.suspends==4);
}

#ifdef ADRENO830_MACOS
static uint32_t gSinkCause,gSinkCalls;
static void sink(void *,uint8_t cause,uint64_t) {gSinkCause=cause;++gSinkCalls;}
static A830PowerState gPower;

/* A clock whose sleeps can finish the work in flight after `retireAfterUs` of them (the wait for quiet polls every millisecond). */
static uint64_t gRetireAfterUs;
static bool gRetireArmed;
static void sleepRetiring(void *,uint32_t us)
{
    gEnv.now+=uint64_t(us)*1000;gEnv.slept+=us;
    if(gRetireArmed && gEnv.slept>=gRetireAfterUs) {gOps.retired=gOps.lastSubmitted;gRetireArmed=false;}
}
static const A830PowerEnv gSuspendEnv={nullptr,envNow,sleepRetiring,envDelay,envLock,envUnlock,envWait,envWake,nullptr};

static void boot(const A830PowerConfig &config)
{
    resetAll();
    gSinkCause=gSinkCalls=0;gRetireArmed=false;gRetireAfterUs=0;
    CHECK(gPower.init(config,gIo,gSuspendEnv,gPowerOps)==kIOReturnSuccess);
    gPower.setFaultSink(sink,nullptr);
    gPower.start(2,2);
    gOps.lastSubmitted=gOps.retired=7;
    CHECK(gPower.phase()==A830PowerActive && !gPower.suspended());
}

static void runFor(unsigned ms)
{
    const uint64_t end=gEnv.now+uint64_t(ms)*1000000;
    while(gEnv.now<end) {
        const uint64_t next=gPower.tick(gEnv.now);
        gEnv.now=next>gEnv.now && next<end?next:gEnv.now+1000000;
    }
}

static const std::vector<std::string> kFullEntry={"oobSet","irq:01:off","oobClear","hfi:33:ffff0002:00000002","rsccSleep",
                                                  "irq:3f:off","cxPowerOff"};
static const std::vector<std::string> kFullWake={"cxPowerOn","smmuRestore","cxTimerSeed","gmuStart","cpStart","secureResume",
                                                 "irq:3f:on"};

static void testFromP0()
{
    boot(A830PowerConfig{});   /* P0: the GPU never slumbers on its own */
    A830PowerState::SuspendInfo info{};
    CHECK(gPower.suspend(500000,&info)==kIOReturnSuccess);
    CHECK(gOps.calls==kFullEntry && gPower.phase()==A830PowerSlumber && gPower.suspended() && !gPower.cxOn() && gPower.cxLost());
    CHECK(!info.alreadyAsleep && info.waitedUs<1000 && gPower.stats().entries==1 && !gEnv.locked);
    /* Shut: no tick, no touch or screen wake, no IFPC cycle; a submission waits for the resume without the mutex, then fails. */
    gOps.calls.clear();
    const size_t logged=gHw.log.size();
    gPower.noteTouch();
    runFor(3000);
    CHECK(gOps.calls.empty() && gHw.log.size()==logged && gPower.phase()==A830PowerSlumber);
    CHECK(gPower.wake(A830WakeTouch)==A830ReturnNotResponding && gPower.wake(A830WakeExplicit)==A830ReturnNotResponding);
    const uint64_t before=gEnv.now;
    CHECK(gPower.acquire()==A830ReturnNotResponding && gPower.holders()==0 && gOps.calls.empty());
    CHECK(gEnv.now-before>=5000ull*1000000 && gEnv.now-before<5100ull*1000000 && !gEnv.locked);
    CHECK(!gPower.accessBegin(false));
    /* A second suspend while shut and asleep: nothing more. */
    CHECK(gPower.suspend(500000,&info)==kIOReturnSuccess && info.alreadyAsleep && gOps.calls.empty());
    /* resume(true): the wake now, the gate open, the GPU usable. */
    uint32_t wakeUs=99;
    gEnv.now+=1000;
    CHECK(gPower.resume(true,&wakeUs)==kIOReturnSuccess && gOps.calls==kFullWake && gPower.phase()==A830PowerActive);
    CHECK(!gPower.suspended() && gPower.stats().wakes==1 && wakeUs==gPower.stats().lastWakeUs);
    CHECK(gPower.acquire()==kIOReturnSuccess);
    gPower.release();
    CHECK(gPower.resume(true,&wakeUs)==kIOReturnSuccess && wakeUs==0 && gOps.calls==kFullWake);   /* not shut: nothing */
}

static void testRefusals()
{
    /* Work in flight for the whole wait: Busy, nothing done, the gate open again. */
    boot(A830PowerConfig{});
    gOps.lastSubmitted=9;
    A830PowerState::SuspendInfo info{};
    CHECK(gPower.suspend(500000,&info)==kIOReturnBusy && gOps.calls.empty() && gPower.phase()==A830PowerActive);
    CHECK(!gPower.suspended() && info.waitedUs>=500000 && info.waitedUs<502000 && gPower.acquire()==kIOReturnSuccess);
    gPower.release();
    /* ... finishing 120 ms into the wait: down. */
    gRetireAfterUs=gEnv.slept+120000;gRetireArmed=true;
    CHECK(gPower.suspend(500000,&info)==kIOReturnSuccess && gOps.calls==kFullEntry && info.waitedUs>=120000 && info.waitedUs<122000);
    CHECK(gPower.resume(false,nullptr)==kIOReturnSuccess && gPower.phase()==A830PowerSlumber && !gPower.suspended());
    /* Left asleep: the first submission wakes it (the gate is open). */
    gOps.calls.clear();
    CHECK(gPower.acquire()==kIOReturnSuccess && gOps.calls==kFullWake && gPower.phase()==A830PowerActive);
    gPower.release();
    /* A holder for the whole wait: Busy. */
    boot(A830PowerConfig{});
    CHECK(gPower.acquire()==kIOReturnSuccess);
    CHECK(gPower.suspend(200000,&info)==kIOReturnBusy && gOps.calls.empty() && !gPower.suspended() && gPower.holders()==1);
    gPower.release();
    CHECK(gPower.suspend(200000,&info)==kIOReturnSuccess);
    /* Lost: refused at once, nothing touched. */
    boot(A830PowerConfig{});
    gPower.markLost();
    const uint64_t at=gEnv.now;
    CHECK(gPower.suspend(500000,&info)==A830ReturnNotResponding && gOps.calls.empty() && !gPower.suspended() && gEnv.now==at);
    /* A recovery that does not finish within the wait: NotResponding, nothing touched. */
    boot(A830PowerConfig{});
    CHECK(gPower.beginRecovery()==kIOReturnSuccess);
    CHECK(gPower.suspend(300000,&info)==A830ReturnNotResponding && gOps.calls.empty() && !gPower.suspended());
    gPower.endRecovery(true,2,2);
    CHECK(gPower.suspend(300000,&info)==kIOReturnSuccess);
    /* A failed entry (the RSCC sleep): the recovery has the GPU, the gate is open, the suspend says so. */
    boot(A830PowerConfig{});
    gOps.fail["rsccSleep"]=kIOReturnTimeout;
    CHECK(gPower.suspend(500000,&info)==kIOReturnIOError && gSinkCalls==1 && gSinkCause==A830FaultSlumber);
    CHECK(gPower.phase()==A830PowerRecovering && !gPower.suspended() && gPower.stats().failures==1 && !gEnv.locked);
}

static void testFromSlumber()
{
    /* P1 full slumber already (piano-gpu-idle=2): nothing to do but shut the gate. */
    A830PowerConfig full;full.slumber=A830SlumberFull;full.idleMs=1000;
    boot(full);
    runFor(1100);
    CHECK(gPower.phase()==A830PowerSlumber && gOps.calls==kFullEntry);
    gOps.calls.clear();
    A830PowerState::SuspendInfo info{};
    CHECK(gPower.suspend(500000,&info)==kIOReturnSuccess && info.alreadyAsleep && gOps.calls.empty() && gPower.suspended());
    /* Its idle slumber no longer wakes for a touch (pre-wake) while shut. */
    gPower.noteTouch();
    runFor(500);
    CHECK(gOps.calls.empty() && gPower.phase()==A830PowerSlumber);
    CHECK(gPower.resume(true,nullptr)==kIOReturnSuccess && gOps.calls==kFullWake);
    /* A CX-keeping slumber: up through the suspend's own wake, then the full one (CX collapses). */
    A830PowerConfig keep;keep.slumber=A830SlumberCxKeep;keep.idleMs=1000;
    boot(keep);
    runFor(1100);
    CHECK(gPower.phase()==A830PowerSlumber && gPower.cxOn());
    gOps.calls.clear();
    CHECK(gPower.suspend(500000,&info)==kIOReturnSuccess && !info.alreadyAsleep && !gPower.cxOn());
    std::vector<std::string> expected={"gmuStart","cpStart","secureResume","irq:3f:on"};
    expected.insert(expected.end(),kFullEntry.begin(),kFullEntry.end());
    CHECK(gOps.calls==expected);
    /* A failed wake at the resume: the recovery, as for any wake. */
    gOps.calls.clear();
    gOps.fail["gmuStart"]=kIOReturnTimeout;
    CHECK(gPower.resume(true,nullptr)==A830ReturnNotResponding && gSinkCause==A830FaultWake && gPower.phase()==A830PowerRecovering);
    CHECK(!gPower.suspended());
}
#endif

int main()
{
    resetAll();
    testTargets();
    testMachine();
#ifdef ADRENO830_MACOS
    adreno830_profile_value=A830ProfileDesign;   /* the sequences as test_a830powerstate writes them (annibale: one more oobClear) */
    testFromP0();
    testRefusals();
    testFromSlumber();
    std::printf("PASS: system suspend (UniXNU-piano): the DDR and rail targets, the four-step machine in every order with its "
                "refusals and block, the power state's gate (full slumber from P0, from a CX-keeping slumber, kept from a full one; "
                "no wake, tick or submission while shut, a waiting submission without the mutex; busy, holder, lost, unfinished "
                "recovery and failed entry refused with nothing changed or with the recovery; resume with and without the wake) "
                "(%u checks)\n",gChecks);
#else
    std::printf("PASS: system suspend (UniXNU-piano): the DDR and rail targets, the four-step machine (%u checks)\n",gChecks);
#endif
    return 0;
}
