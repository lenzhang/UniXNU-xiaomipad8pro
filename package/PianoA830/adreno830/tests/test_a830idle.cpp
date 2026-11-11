/* SPDX-License-Identifier: MIT */
/* host-test: A830Dcvs.cpp A830PowerState.cpp A830Recovery.cpp platform/sm8750/Sm8750Tsens.c
 * UniXNU-piano: the GPU's idle policy (A830Idle.h, piano-gpu-idle). Both passes: the display scanout's DDR floor on piano's bus
 * table, the idle votes' state machine (down after the quiet time, up at the next submission or when the GPU is not quiet, a failure
 * ends them), which transitions get a line. ADRENO830_MACOS pass: the DCVS without its wake boost keeps an isolated frame, the login
 * screen's frames and a light 120 Hz desktop at the floor, its idle window stretches to 250 ms and the first submission still wakes the
 * service thread (16 ms windows again), the idle tick hook sees quiet and the newest submission, and a failed P1 slumber (entry or wake)
 * ends P1 for the boot. The idle rails' TCS: test_sm8750_rpmh. */
#include "A830PowerFakes.h"
#include "A830Idle.h"

static void testScanoutLevel()
{
    uint32_t scanout=0;
    /* 3200x2136 x 4 bytes x 120 Hz = 3.28 GB/s; twice that is 6.56 GB/s: level 5 (6.67). */
    CHECK(A830IdleScanoutLevel(kDdr,12,3200,2136,4,120,2000,&scanout)==5 && scanout==3280896);
    CHECK(A830IdleScanoutLevel(kDdr,12,3200,2136,4,120,1000,nullptr)==3);        /* 5.29 GB/s holds the rate itself */
    CHECK(A830IdleScanoutLevel(kDdr,12,3200,2136,4,60,2000,nullptr)==3);         /* 60 Hz: 3.28 GB/s doubled */
    CHECK(A830IdleScanoutLevel(kDdr,12,7680,4320,4,120,2000,nullptr)==11);       /* more than the table: its top */
    CHECK(A830IdleScanoutLevel(nullptr,0,3200,2136,4,120,2000,nullptr)==0);
}

static void testVotes()
{
    constexpr uint64_t Ms=1000000;
    A830IdleVotes v;
    uint64_t next=0;
    const uint64_t t0=100*Ms;
    CHECK(v.step(t0+500*Ms,true,t0,1000,&next)==A830IdleKeep && next==t0+1000*Ms);
    CHECK(v.step(t0+999*Ms,false,t0,1000,&next)==A830IdleKeep && next==0);      /* work in flight: no deadline, no change */
    CHECK(v.step(t0,true,t0+5,1000,&next)==A830IdleKeep && next==t0+5+1000*Ms); /* stamped after now was read */
    CHECK(v.step(t0+1000*Ms,true,t0,1000,&next)==A830IdleDown);
    v.done(A830IdleDown,true,t0+1000*Ms,t0);
    CHECK(v.down && v.downs==1);
    CHECK(v.step(t0+60000*Ms,true,t0,1000,&next)==A830IdleKeep && next==0);     /* still quiet: held */
    CHECK(v.step(t0+60001*Ms,true,t0+60000*Ms,1000,&next)==A830IdleUp);         /* a submission */
    v.done(A830IdleUp,true,t0+60001*Ms,t0+60000*Ms);
    CHECK(!v.down && v.ups==1);
    CHECK(v.step(t0+61001*Ms,true,t0+60000*Ms,1000,&next)==A830IdleDown);
    v.done(A830IdleDown,true,t0+61001*Ms,t0+60000*Ms);
    CHECK(v.step(t0+61002*Ms,false,t0+60000*Ms,1000,&next)==A830IdleUp);        /* not quiet (a recovery): up */
    v.done(A830IdleUp,false,t0+61002*Ms,t0+60000*Ms);
    CHECK(v.blocked && v.down && v.failures==1);
    CHECK(v.step(t0+61003*Ms,true,t0+61003*Ms,1000,&next)==A830IdleUp);          /* recorded down: Up is tried again on activity */
    v.done(A830IdleUp,true,t0+61003*Ms,t0+61003*Ms);
    CHECK(!v.down && v.step(t0+90000*Ms,true,t0+61003*Ms,1000,&next)==A830IdleKeep);   /* blocked: never down again */
    A830IdleVotes w;
    CHECK(w.step(t0+1000*Ms,true,t0,1000,&next)==A830IdleDown);
    w.done(A830IdleDown,false,t0+1000*Ms,t0);
    CHECK(w.blocked && !w.down && w.step(t0+5000*Ms,true,t0,1000,&next)==A830IdleKeep);
    unsigned logged=0;
    for(uint32_t n=0;n<=1000;++n)logged+=A830IdleLogged(n)?1:0;
    CHECK(logged==8+3 && A830IdleLogged(10) && !A830IdleLogged(9) && !A830IdleLogged(0));   /* 1..8, 10, 100, 1000 */
}

#ifdef ADRENO830_MACOS
static A830PowerState gPower;
static A830Recovery gRecovery;
static A830Dcvs gDcvs;
static unsigned gIdleTicks,gIdleQuiet;
static uint64_t gIdleLast;
static uint64_t idleTick(void *,uint64_t,bool quiet,uint64_t lastSubmit)
{
    ++gIdleTicks;
    gIdleQuiet+=quiet?1:0;
    gIdleLast=lastSubmit;
    return 0;
}
static constexpr uint64_t WindowNs=16000000ull;

static void boot(A830DcvsConfig config,const A830PowerConfig &power=A830PowerConfig{},bool hook=false)
{
    resetAll();
    CHECK(gPower.init(power,gIo,gPowerEnv,gPowerOps)==kIOReturnSuccess);
    CHECK(gRecovery.init(A830RecoveryConfig{},gIo,gPowerEnv,gPowerOps,gRecoveryOps,&gPower)==kIOReturnSuccess);
    gPower.start(config.initialLevel,config.initialBus);
    A830DcvsOps ops=gDcvsOps;
    if(hook)ops.idleTick=idleTick;
    CHECK(gDcvs.init(config,kTable,gIo,gPowerEnv,ops,&gPower,&gRecovery,nullptr)==kIOReturnSuccess);
    CHECK(gDcvs.start()==kIOReturnSuccess);
    gIdleTicks=gIdleQuiet=0;gIdleLast=0;
}

/* One window of `busy` permille; `submit` adds a submission (and retires it at once). Returns the next deadline. */
static uint64_t step(uint32_t busy,bool submit,uint64_t ns=WindowNs)
{
    gEnv.now+=ns;
    gHw.busyTicks+=uint64_t(A830_XO_HZ)*ns/1000000000ull*busy/1000;
    if(submit) {gDcvs.noteSubmit();++gOps.lastSubmitted;gOps.retired=gOps.lastSubmitted;}
    return gDcvs.serviceTick(gEnv.now);
}

static void testDcvsFloor()
{
    A830DcvsConfig config;
    config.initialLevel=1;config.initialBus=2;
    config.wakeBoostKhz=0;config.idleWindowMs=250;
    boot(config);
    for(int i=0;i<10;++i)step(0,false);
    for(int i=0;i<10;++i)step(0,false,100000000ull);   /* quiet for a second */
    CHECK(gDcvs.level()==1);
    /* Idle at the floor: one window per 250 ms, not one per 16 ms (looks between them do not sample). */
    const uint32_t windows=gDcvs.stats().windows;
    for(int i=0;i<10;++i)step(0,false);
    CHECK(gDcvs.stats().windows<=windows+1);
    /* An isolated frame after idle: no wake boost, the floor stays (a light frame), and the submission woke the thread. */
    const unsigned wakes=gEnv.wakes;
    const uint64_t next=step(150,true,1000000ull);
    CHECK(gEnv.wakes==wakes+1 && next<=gEnv.now+WindowNs);   /* and the windows are 16 ms again */
    for(int i=0;i<4;++i)step(150,false);
    CHECK(gDcvs.level()==1 && gDcvs.stats().wakeBoosts>=1);
    /* A saturated load still climbs. */
    for(int i=0;i<4;++i)step(950,true);
    CHECK(gDcvs.level()>1);
    /* The design's wake boost, for comparison: the same isolated frame goes to 525 MHz. */
    A830DcvsConfig boosted;
    boosted.initialLevel=1;boosted.initialBus=2;
    boot(boosted);
    for(int i=0;i<10;++i)step(0,false,100000000ull);
    step(150,true,1000000ull);
    CHECK(gDcvs.level()>=gDcvs.levelAtLeast(525000));
    /* The login screen of 2026-11-11: about 10 frames a second of 2.5 ms of 342 MHz work, 5.3 ms at 160 MHz (a third of a window):
     * the floor throughout without the boost; with it, every frame above the floor. And a light 120 Hz desktop (a window dragged,
     * 2.5 ms of 342 MHz work every 8.3 ms frame: 64 % busy at 160 MHz, demand 137 MHz) stays at the floor too. */
    for(const bool boost:{false,true}) {
        boot(boost?boosted:config);
        for(int i=0;i<20;++i)step(0,false,100000000ull);
        unsigned above=0;
        for(int frame=0;frame<100;++frame) {
            step(330,true);
            above+=gDcvs.level()>1?1:0;
            for(int i=0;i<5;++i) {step(0,false);above+=gDcvs.level()>1?1:0;}
        }
        CHECK(boost?above>=100:above==0);
    }
    boot(config);
    for(int i=0;i<200;++i) {step(640,true);CHECK(gDcvs.level()==1);}
}

static void testIdleHook()
{
    A830DcvsConfig config;
    config.initialLevel=1;config.initialBus=2;
    boot(config,A830PowerConfig{},true);
    step(0,true);
    CHECK(gIdleTicks>=1 && gIdleQuiet>=1 && gIdleLast==gEnv.now);
    const unsigned quiet=gIdleQuiet;
    gOps.lastSubmitted+=1;   /* in flight */
    step(500,false);
    CHECK(gIdleQuiet==quiet);
    gOps.retired=gOps.lastSubmitted;
    step(0,false);
    CHECK(gIdleQuiet==quiet+1);
}

/* The external ceiling (adreno830_dcvs_set_ceiling: PianoThermal's passive trip, later Low Power Mode): it wakes the thread once per
 * change, holds the governor and every boost under it from the next tick, below the table means the lowest level, 0 gives the other
 * ceilings back, and one set before S10 holds from the first vote of the fixed mode. */
static void testCeiling()
{
    A830DcvsConfig config;
    config.initialLevel=1;config.initialBus=2;
    boot(config);
    for(int i=0;i<10;++i)step(950,true);
    CHECK(gDcvs.level()>=5 && gDcvs.externalCeiling()==0);
    const unsigned wakes=gEnv.wakes;
    adreno830_dcvs_set_ceiling(342000);
    adreno830_dcvs_set_ceiling(342000);
    CHECK(gEnv.wakes==wakes+1);
    step(950,true);
    CHECK(gDcvs.externalCeiling()==3 && gDcvs.ceiling()==3 && gDcvs.level()==3 && gOps.votes.back().first==3);
    for(int i=0;i<20;++i) {step(950,true);CHECK(gDcvs.level()<=3);}
    adreno830_dcvs_set_ceiling(1);
    step(950,true);
    CHECK(gDcvs.ceiling()==1 && gDcvs.level()==1);
    adreno830_dcvs_set_ceiling(0);
    for(int i=0;i<10;++i)step(950,true);
    CHECK(gDcvs.externalCeiling()==0 && gDcvs.level()>3);
    /* Fixed mode (pagani-gpu-dcvs=0, perf level 9), the ceiling set before the DCVS starts: the start's vote is under it. */
    adreno830_dcvs_set_ceiling(443000);
    A830DcvsConfig fixed;
    fixed.governor=false;fixed.fixedLevel=9;fixed.initialLevel=1;fixed.initialBus=2;
    boot(fixed);
    CHECK(!gOps.votes.empty() && gOps.votes[0].first==5 && gDcvs.externalCeiling()==5);
    step(0,false);
    CHECK(gDcvs.level()==5);
    adreno830_dcvs_set_ceiling(0);
    step(0,false);
    CHECK(gDcvs.level()==gDcvs.ceiling() && gDcvs.level()>5);
}

static uint32_t gSinkCause;
static void sink(void *,uint8_t cause,uint64_t) {gSinkCause=cause;}
static void runPower(unsigned ms)
{
    const uint64_t end=gEnv.now+uint64_t(ms)*1000000;
    while(gEnv.now<end) {
        const uint64_t next=gPower.tick(gEnv.now);
        gEnv.now=next>gEnv.now && next<end?next:gEnv.now+1000000;
    }
}

static void testSlumberBlocked()
{
    A830PowerConfig power;power.slumber=A830SlumberFull;power.idleMs=1000;power.touchPrewake=false;
    resetAll();gSinkCause=0;
    CHECK(gPower.init(power,gIo,gPowerEnv,gPowerOps)==kIOReturnSuccess);
    gPower.setFaultSink(sink,nullptr);
    gPower.start(1,2);
    gOps.fail["rsccSleep"]=kIOReturnTimeout;
    runPower(1100);
    CHECK(gPower.phase()==A830PowerRecovering && gSinkCause==A830FaultSlumber && gPower.stats().slumberBlocked);
    gPower.lock();CHECK(gPower.beginRecovery()==kIOReturnSuccess);gPower.endRecovery(true,1,2);gPower.unlock();
    gOps.calls.clear();
    runPower(5000);
    CHECK(gPower.phase()==A830PowerActive && gPower.stats().entries==0 && gOps.calls.empty());
    /* A failed wake: the same. */
    resetAll();gSinkCause=0;
    CHECK(gPower.init(power,gIo,gPowerEnv,gPowerOps)==kIOReturnSuccess);
    gPower.setFaultSink(sink,nullptr);
    gPower.start(1,2);
    runPower(1100);
    CHECK(gPower.phase()==A830PowerSlumber && !gPower.stats().slumberBlocked);
    gOps.fail["gmuStart"]=kIOReturnTimeout;
    CHECK(gPower.acquire()!=kIOReturnSuccess);
    CHECK(gPower.phase()==A830PowerRecovering && gSinkCause==A830FaultWake && gPower.stats().slumberBlocked);
    gPower.lock();gPower.beginRecovery();gPower.endRecovery(true,1,2);gPower.unlock();
    const uint32_t entries=gPower.stats().entries;
    runPower(5000);
    CHECK(gPower.phase()==A830PowerActive && gPower.stats().entries==entries);
    /* Without a failure P1 sleeps and wakes again and again. */
    resetAll();
    CHECK(gPower.init(power,gIo,gPowerEnv,gPowerOps)==kIOReturnSuccess);
    gPower.setFaultSink(sink,nullptr);
    gPower.start(1,2);
    for(int i=0;i<3;++i) {
        runPower(1100);
        CHECK(gPower.phase()==A830PowerSlumber);
        CHECK(gPower.acquire()==kIOReturnSuccess);
        gPower.release();
    }
    CHECK(gPower.stats().entries==3 && gPower.stats().wakes==3 && !gPower.stats().slumberBlocked);
}
#endif

int main()
{
    adreno830_profile_value=A830ProfileDesign;
    resetAll();
    testScanoutLevel();
    testVotes();
#ifdef ADRENO830_MACOS
    testDcvsFloor();
    testIdleHook();
    testCeiling();
    testSlumberBlocked();
    std::printf("PASS: GPU idle policy: the scanout's DDR floor (level 5 for 3200x2136 at 120 Hz), the idle votes' state machine and "
                "its lines; DCVS without the wake boost (an isolated frame, the login screen and a light 120 Hz desktop at the floor), "
                "the idle window and its wake, the idle tick hook, the external ceiling (one wake per change, the next tick, the lowest "
                "level below the table, cleared by 0, before S10 in fixed mode), a failed P1 slumber or wake ends P1 for the boot "
                "(%u checks)\n",gChecks);
#else
    std::printf("PASS: GPU idle policy helpers: the scanout's DDR floor, the idle votes' state machine and its lines (%u checks)\n",
                gChecks);
#endif
    return 0;
}
