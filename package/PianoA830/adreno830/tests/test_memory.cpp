/* SPDX-License-Identifier: MIT */
/* Real memory-management implementation linked to host-only IOKit doubles.
 * Production CPU/cache assembly is separately cross-built, never run here. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoMemory.h"
#include <cstdio>
#include <mutex>
#include <thread>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
extern "C" volatile uint32_t native_adreno_pool_shape[6],native_adreno_pool_stats[8];
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out)
{CHECK(out);*out={};return fixture.cpu_status;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && !(((uintptr_t)mapping|bytes)&4095));
    bool owned=false;
    for(auto *d:fixture.live)if((uintptr_t)mapping>=(uintptr_t)d->raw &&
        (uintptr_t)mapping-(uintptr_t)d->raw<=d->bytes &&
        bytes<=d->bytes-((uintptr_t)mapping-(uintptr_t)d->raw))owned=true;
    CHECK(owned);++fixture.cache_calls;fixture.cache_operations.push_back(op);
    return fixture.cache_calls==fixture.cache_fail_call?kIOReturnIOError:kIOReturnSuccess;
}
static void reset()
{CHECK(fixture.live.empty() && fixture.metadata.empty());fixture=MemoryFixture{};}
/* Host fixture teardown only. This is not a production reset/release path. */
static void discardFixtureAfterQuarantine()
{
    while(!fixture.live.empty())fixture.live.back()->release();
    while(!fixture.metadata.empty())fixtureMetadataFree(fixture.metadata.back());
    reset();
}
static unsigned successes()
{
    unsigned profiles=0,outputs[]={32,36,40,42,44,48};
    for(unsigned output:outputs)for(unsigned mode:{Contiguous,Scattered,Paired}) {
        reset();fixture.mode=static_cast<SegmentMode>(mode);NativeAdrenoMemory memory;
        CHECK(memory.allocate(16384,output,mode==Contiguous,16384)==kIOReturnSuccess);
        CHECK(memory.bytes()==16384 && memory.mapping() && memory.descriptor() && !memory.quarantined());
        size_t count=mode==Contiguous?1:mode==Scattered?4:2;
        CHECK(memory.spanCount()==count && fixture.prepared==1 && fixture.segment_calls==4);
        for(size_t i=0;i<16384;++i)CHECK(static_cast<unsigned char *>(memory.mapping())[i]==0);
        uint64_t bytes=0;
        for(size_t i=0;i<count;++i) {
            const auto &span=memory.spans()[i];CHECK(span.offset==bytes);
            CHECK(span.physical==0x30000000+span.offset+(mode==Scattered?span.offset:mode==Paired?(span.offset/8192)*4096:0));
            bytes+=span.bytes;
        }
        CHECK(bytes==16384 && fixture.cache_calls==1 && fixture.cache_operations[0]==NativeAdrenoClean);
        CHECK(memory.holdForDevice()==kIOReturnSuccess && memory.holdForDevice()==kIOReturnSuccess);
        CHECK(memory.deviceReferences()==2 && memory.release()==kIOReturnBusy && !fixture.completed);
        CHECK(memory.publishForDevice(4096,4096,false)==kIOReturnSuccess);
        CHECK(fixture.cache_operations.back()==NativeAdrenoClean);
        CHECK(memory.publishForDevice(0,16384,true)==kIOReturnSuccess);
        CHECK(fixture.cache_operations.back()==NativeAdrenoCleanInvalidate);
        CHECK(memory.acquireDeviceWrites(0,16384)==kIOReturnSuccess);
        CHECK(fixture.cache_operations.back()==NativeAdrenoInvalidate);
        CHECK(memory.retireAfterQuiescence()==kIOReturnSuccess);
        CHECK(memory.release()==kIOReturnBusy && !fixture.completed);
        CHECK(memory.retireAfterQuiescence()==kIOReturnSuccess && !memory.deviceReferences());
        CHECK(memory.retireAfterQuiescence()==kIOReturnBadArgument);
        CHECK(memory.release()==kIOReturnSuccess && !memory.bytes() && !memory.descriptor() && !memory.spans());
        CHECK(fixture.completed==1 && fixture.released==1 && fixture.live.empty());
        CHECK(memory.release()==kIOReturnSuccess && memory.holdForDevice()==kIOReturnNotReady);++profiles;
    }
    return profiles;
}
static unsigned failures()
{
    unsigned controls=0;
    for(unsigned failure=0;failure<12;++failure) {
        reset();NativeAdrenoMemory memory;IOReturn expected=kIOReturnUnsupported;
        if(failure==0) {fixture.allocation_fail=true;expected=kIOReturnNoMemory;}
        if(failure==1) {fixture.prepare_fail=true;expected=kIOReturnIOError;}
        if(failure==2)fixture.null_mapping=true;
        if(failure==3)fixture.unaligned_mapping=true;
        if(failure==4)fixture.wrong_length=true;
        if(failure==5) {fixture.metadata_fail=true;expected=kIOReturnNoMemory;}
        if(failure>=6 && failure<=9)fixture.mode=static_cast<SegmentMode>(ZeroPhysical+(failure-6));
        if(failure==10)fixture.mode=Scattered;
        if(failure==11) {fixture.cache_fail_call=1;expected=kIOReturnIOError;}
        CHECK(memory.allocate(16384,32,true,16384)==expected);
        CHECK(!memory.descriptor() && !memory.spans() && !memory.quarantined() && fixture.live.empty());
        CHECK(fixture.released==fixture.created);
        CHECK(fixture.completed==(failure<2?0:1));++controls;
    }
    for(unsigned cleanup_failure=0;cleanup_failure<3;++cleanup_failure) {
        reset();NativeAdrenoMemory memory;
        if(cleanup_failure==1)fixture.mode=ZeroPhysical;
        if(cleanup_failure==2)fixture.cache_fail_call=1;
        fixture.complete_fail=true;
        IOReturn allocated=memory.allocate(16384,32,false,16384);
        if(!cleanup_failure) {
            CHECK(allocated==kIOReturnSuccess);
            CHECK(memory.release()==kIOReturnIOError);
        } else CHECK(allocated==(cleanup_failure==1?kIOReturnUnsupported:kIOReturnIOError));
        CHECK(memory.quarantined() && memory.descriptor() && memory.bytes()==16384);
        unsigned completed=fixture.completed;
        CHECK(memory.release()==kIOReturnBusy && fixture.completed==completed && !fixture.released);
        CHECK(memory.holdForDevice()==kIOReturnNotReady);
        CHECK(memory.publishForDevice(0,4096,false)==kIOReturnNotReady);
        discardFixtureAfterQuarantine();++controls;
    }
    reset();NativeAdrenoMemory memory;
    CHECK(memory.allocate(16384,32,false,16384)==kIOReturnSuccess);
    CHECK(memory.holdForDevice()==kIOReturnSuccess);memory.quarantine();
    CHECK(memory.retireAfterQuiescence()==kIOReturnNotReady && memory.deviceReferences()==1);
    CHECK(memory.release()==kIOReturnBusy && !fixture.completed && !fixture.released);
    CHECK(memory.acquireDeviceWrites(0,4096)==kIOReturnNotReady);
    discardFixtureAfterQuarantine();++controls;
    return controls;
}
static unsigned arguments()
{
    reset();NativeAdrenoMemory memory;unsigned controls=0;
    CHECK(memory.allocate(0,32,false,16384)==kIOReturnBadArgument);++controls;
    CHECK(memory.allocate(4097,32,false,16384)==kIOReturnBadArgument);++controls;
    CHECK(memory.allocate(4096,33,false,16384)==kIOReturnBadArgument);++controls;
    CHECK(memory.allocate(4096,32,false,0)==kIOReturnBadArgument);++controls;
    CHECK(memory.allocate(4096,32,false,size_t(NativeAdrenoMemoryMaxBytes)+1)==kIOReturnBadArgument);++controls;   /* UniXNU-piano */
    CHECK(memory.allocate(16384,32,false,4096)==kIOReturnNoResources);++controls;
    CHECK(!fixture.created && !fixture.cache_calls);
    fixture.cpu_status=kIOReturnUnsupported;
    CHECK(memory.allocate(4096,32,false,4096)==kIOReturnUnsupported);++controls;
    CHECK(!fixture.created);fixture.cpu_status=kIOReturnSuccess;
    CHECK(memory.allocate(16384,32,false,16384)==kIOReturnSuccess);
    CHECK(memory.allocate(16384,32,false,16384)==kIOReturnBusy);++controls;
    for(auto range:std::vector<std::pair<size_t,size_t>>{{0,0},{1,4096},{0,4097},{16384,4096},{SIZE_MAX-4095,4096},{4096,SIZE_MAX-4095}}) {
        CHECK(memory.publishForDevice(range.first,range.second,false)==kIOReturnBadArgument);
        CHECK(memory.acquireDeviceWrites(range.first,range.second)==kIOReturnBadArgument);controls+=2;
    }
    CHECK(fixture.cache_calls==1);CHECK(memory.release()==kIOReturnSuccess);
    return controls;
}
/* M9: the scanout of the display hand-off as a BO from the presenter's device memory (no wiring, no kernel mapping, no CPU
 * maintenance), and the live-import count of the MEM row. */
static unsigned scanoutImports()
{
    reset();unsigned controls=0;
    IOMemoryDescriptor *scanout=IOMemoryDescriptor::withAddressRange(0x10000000,8*4096,kIODirectionInOut|kIOMemoryMapperNone,
        reinterpret_cast<task_t>(2));
    CHECK(scanout);
    const uint64_t base=scanout->physical_base;
    NativeAdrenoMemory memory;
    uint32_t pool=9,used=9,imports=9;
    NativeAdrenoMemory::usage(&pool,&used,&imports);CHECK(!pool && !used && !imports);
    CHECK(memory.importDevice(scanout,4096,4*4096)==kIOReturnSuccess);
    CHECK(memory.scanout() && memory.backed() && !memory.imported() && !memory.mapping() && memory.bytes()==4*4096);
    CHECK(memory.spanCount()==4 && fixture.prepared==0);   /* the fixture's pages are two apart: one span each; nothing wired */
    for(size_t i=0;i<4;++i)CHECK(memory.spans()[i].offset==i*4096 && memory.spans()[i].physical==base+2*(4096+i*4096));
    NativeAdrenoMemory::usage(nullptr,nullptr,&imports);CHECK(imports==1);
    const unsigned calls=fixture.cache_calls,physical=fixture.physical_calls;
    CHECK(memory.publishForDevice(0,4*4096,true)==kIOReturnSuccess && memory.acquireDeviceWrites(0,4096)==kIOReturnSuccess);
    CHECK(fixture.cache_calls==calls && fixture.physical_calls==physical);++controls;
    CHECK(memory.makeWriteCombined()==kIOReturnNotReady && memory.importDevice(scanout,0,4096)==kIOReturnBusy);controls+=2;
    CHECK(memory.holdForDevice()==kIOReturnSuccess && memory.release()==kIOReturnBusy);
    CHECK(memory.retireAfterQuiescence()==kIOReturnSuccess && memory.release()==kIOReturnSuccess && !memory.scanout());++controls;
    NativeAdrenoMemory::usage(nullptr,nullptr,&imports);CHECK(imports==0);
    NativeAdrenoMemory refused;
    CHECK(refused.importDevice(nullptr,0,4096)==kIOReturnBadArgument);
    CHECK(refused.importDevice(scanout,1,4096)==kIOReturnBadArgument);
    CHECK(refused.importDevice(scanout,0,0)==kIOReturnBadArgument);
    CHECK(refused.importDevice(scanout,0,4097)==kIOReturnBadArgument);
    CHECK(refused.importDevice(scanout,4*4096,5*4096)==kIOReturnBadArgument);   /* past the descriptor */
    controls+=5;
    fixture.metadata_fail=true;
    CHECK(refused.importDevice(scanout,0,4096)==kIOReturnNoMemory && !refused.backed());++controls;
    fixture.metadata_fail=false;
    /* A task import counts as a live import until it is released. */
    NativeAdrenoMemory task;
    CHECK(task.importTask(reinterpret_cast<void *>(2),0x20000000,4096,36,4096)==kIOReturnSuccess);
    NativeAdrenoMemory::usage(nullptr,nullptr,&imports);CHECK(imports==1);
    CHECK(task.release()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(nullptr,nullptr,&imports);CHECK(imports==0);++controls;
    scanout->release();
    CHECK(!fixture.imports_live);
    return controls;
}
/* ARCHITECTURE 3.4 S3: the BO pool of pagani-gpu-pool, halved until a reservation succeeds, write-combined once. Runs last: the
 * pool lives for the rest of the process, as it lives for the boot. */
static unsigned bestEffortPool()
{
    reset();unsigned controls=0;
    uint32_t reserved=7;
    CHECK(NativeAdrenoMemory::reservePoolBestEffort(36,512,16,true,nullptr)==kIOReturnBadArgument);
    CHECK(NativeAdrenoMemory::reservePoolBestEffort(33,512,16,true,&reserved)==kIOReturnBadArgument && !reserved);controls+=2;
    CHECK(NativeAdrenoMemory::reservePoolBestEffort(36,0,16,true,&reserved)==kIOReturnSuccess && !reserved);++controls;
#ifdef ADRENO830_MACOS
    /* UniXNU-piano (2026-11-12): XNU is never asked for a contiguous block of 2 GiB or more (NativeAdrenoContiguousMaxBytes: the kernel
     * map refuses it after cpm_allocate took the pages, which go back gobbled). The board's 2048 MiB pool is not requested, the halving
     * asks 1024 first; 2 GiB less a page is the largest request there is; a contiguous BO above it is refused unasked. Nothing is
     * allocated here (every request the fixture sees fails), so the host never maps the sizes. */
    {
        static size_t first_bytes,largest_bytes;static unsigned asked,scattered;
        first_bytes=largest_bytes=0;asked=scattered=0;
        fixture.allocation_hook=[](unsigned options,size_t bytes) {
            if(!(options&kIOMemoryPhysicallyContiguous)) {++scattered;return;}
            if(!asked++)first_bytes=bytes;
            if(bytes>largest_bytes)largest_bytes=bytes;
        };
        fixture.allocation_fail=true;
        CHECK(NativeAdrenoMemory::reservePoolBestEffort(40,2048,64,true,&reserved)==kIOReturnSuccess && !reserved);
        CHECK(asked==5 && first_bytes==(size_t(1024)<<20) && largest_bytes==(size_t(1024)<<20));++controls;   /* 1024..64 */
        asked=0;largest_bytes=0;
        CHECK(NativeAdrenoMemory::reservePool(40,size_t(2)<<30)==kIOReturnBadArgument && !asked);
        CHECK(NativeAdrenoMemory::reservePool(40,size_t(NativeAdrenoContiguousMaxBytes))==kIOReturnNoMemory &&
              asked==1 && largest_bytes==size_t(NativeAdrenoContiguousMaxBytes));++controls;
        asked=0;
        NativeAdrenoMemory big;
        CHECK(big.allocate((size_t(2)<<30),40,true,(size_t(2)<<30))==kIOReturnNoMemory && !asked && !big.backed());
        CHECK(big.allocate(size_t(NativeAdrenoContiguousMaxBytes),40,true,size_t(NativeAdrenoContiguousMaxBytes))==kIOReturnNoMemory &&
              asked==1);++controls;
        /* Scattered (non-contiguous) BOs keep their own bound, NativeAdrenoMemoryMaxBytes: the kernel allocates those without cpm. */
        asked=0;
        CHECK(big.allocate((size_t(2)<<30),40,false,(size_t(2)<<30))==kIOReturnNoMemory && !asked && scattered==1);++controls;
        fixture.allocation_fail=false;fixture.allocation_hook=nullptr;
        CHECK(fixture.live.empty());
    }
#endif
    fixture.contiguous_limit=64u<<20;   /* a fragmented boot: 512, 256 and 128 MiB fail */
    CHECK(NativeAdrenoMemory::reservePoolBestEffort(36,512,256,true,&reserved)==kIOReturnSuccess && !reserved);++controls;
    /* Not write-combined yet: the zero fill's failure below needs the clean a cacheable pool takes. */
    CHECK(NativeAdrenoMemory::reservePoolBestEffort(36,512,16,false,&reserved)==kIOReturnSuccess && reserved==64);++controls;
    CHECK(NativeAdrenoMemory::poolBytes()==(64u<<20) && NativeAdrenoMemory::poolPhysical());
    uint32_t pool=0,used=9,imports=9;
    NativeAdrenoMemory::usage(&pool,&used,&imports);CHECK(pool==64 && !used && !imports);
    NativeAdrenoMemory bo;
    CHECK(bo.allocate(2u<<20,36,true,2u<<20,true)==kIOReturnSuccess && bo.pooled() && bo.makeWriteCombined()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(pool==64 && used==2);++controls;
    /* allocatePooled leaves the zero fill to finishPooled (outside the Memory lock): the pages still hold what the last BO left, no
     * device reference until it is done, then one fill, once. */
    {
        NativeAdrenoMemory first,second;
        CHECK(first.allocatePooled(8192,36)==kIOReturnSuccess && first.pooled());
        unsigned char *pages=static_cast<unsigned char *>(first.mapping());
        std::memset(pages,0x5a,8192);
        CHECK(first.finishPooled()==kIOReturnSuccess && first.holdForDevice()==kIOReturnSuccess && first.retireAfterQuiescence()==kIOReturnSuccess);
        std::memset(pages,0x5a,8192);
        CHECK(first.release()==kIOReturnSuccess);
        const unsigned calls=fixture.cache_calls;
        CHECK(second.allocatePooled(8192,36)==kIOReturnSuccess && second.mapping()==pages && pages[0]==0x5a && pages[8191]==0x5a);
        CHECK(fixture.cache_calls==calls && second.holdForDevice()==kIOReturnNotReady && !second.deviceReferences());
        CHECK(second.finishPooled()==kIOReturnSuccess && fixture.cache_calls==calls+1 && fixture.cache_operations.back()==NativeAdrenoClean);
        for(size_t i=0;i<8192;++i)CHECK(!pages[i]);
        pages[0]=1;
        CHECK(second.finishPooled()==kIOReturnSuccess && fixture.cache_calls==calls+1 && pages[0]==1);   /* once */
        CHECK(second.holdForDevice()==kIOReturnSuccess && second.retireAfterQuiescence()==kIOReturnSuccess && second.release()==kIOReturnSuccess);
        controls+=2;
        /* A BO outside the pool has nothing to finish. */
        NativeAdrenoMemory own;
        CHECK(own.allocate(4096,36,false,4096)==kIOReturnSuccess && own.finishPooled()==kIOReturnSuccess && own.release()==kIOReturnSuccess);
        ++controls;
    }
    /* The fill's failure. allocate(pooled): the object gives its pages back and answers the failure (the pool's own figures show nothing
     * lost, nothing quarantined, the object usable again); allocatePooled's caller releases: until then no device reference. */
    {
        NativeAdrenoMemory failing;
        const uint32_t extents=native_adreno_pool_shape[0],alive=native_adreno_pool_shape[2];
        fixture.cache_fail_call=fixture.cache_calls+1;
        CHECK(failing.allocate(1u<<20,36,false,1u<<20,true)==kIOReturnIOError);
        CHECK(!failing.pooled() && !failing.backed() && !failing.bytes() && !failing.spans() && !failing.mapping() && !failing.quarantined());
        NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(used==2 && native_adreno_pool_shape[0]==extents && native_adreno_pool_shape[2]==alive);
        fixture.cache_fail_call=0;++controls;
        CHECK(failing.allocate(1u<<20,36,false,1u<<20,true)==kIOReturnSuccess && failing.pooled() && failing.release()==kIOReturnSuccess);++controls;
        fixture.cache_fail_call=fixture.cache_calls+1;
        CHECK(failing.allocatePooled(1u<<20,36)==kIOReturnSuccess && failing.finishPooled()==kIOReturnIOError);
        CHECK(failing.holdForDevice()==kIOReturnNotReady && failing.release()==kIOReturnSuccess);
        NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(used==2 && native_adreno_pool_shape[2]==alive);
        fixture.cache_fail_call=0;++controls;
        /* And a later failure does not leave the flag behind: the slot's next object is a fresh start. */
        CHECK(failing.allocatePooled(4096,36)==kIOReturnSuccess && failing.release()==kIOReturnSuccess);
        CHECK(failing.allocate(4096,36,false,4096)==kIOReturnSuccess && failing.holdForDevice()==kIOReturnSuccess &&
              failing.retireAfterQuiescence()==kIOReturnSuccess && failing.release()==kIOReturnSuccess);++controls;
    }
    CHECK(NativeAdrenoMemory::reservePoolBestEffort(36,512,16,true,&reserved)==kIOReturnSuccess && reserved==64);++controls;
    CHECK(bo.release()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(!used);
    /* Write-combined from here on: the zeros go to DRAM through the pool's own mapping, a barrier and no clean. */
    {
        const unsigned calls=fixture.cache_calls;
        NativeAdrenoMemory combined;
        CHECK(combined.allocatePooled(4096,36)==kIOReturnSuccess && combined.finishPooled()==kIOReturnSuccess && fixture.cache_calls==calls);
        CHECK(combined.release()==kIOReturnSuccess);++controls;
    }
    return controls;
}
/* The zero fill outside the Memory lock, as the user-client cores run it: threads take and give back pooled BOs under one lock (the
 * Memory lock) and fill and check them outside it. A fill that reached another thread's pages would show in that thread's pattern. */
static unsigned poolThreads()
{
    std::mutex memoryLock;
    enum : unsigned {Threads=4,Rounds=4000};
    const auto work=[&](unsigned id) {
        for(unsigned round=0;round<Rounds;++round) {
            const size_t bytes=size_t(1+(round*7+id)%16)*4096;
            NativeAdrenoMemory bo;
            {
                std::lock_guard<std::mutex> memory(memoryLock);
                CHECK(bo.allocatePooled(bytes,36)==kIOReturnSuccess);
            }
            CHECK(bo.finishPooled()==kIOReturnSuccess);
            unsigned char *pages=static_cast<unsigned char *>(bo.mapping());
            for(size_t i=0;i<bytes;i+=64)CHECK(!pages[i]);
            const unsigned char mark=static_cast<unsigned char>(id*16+1+round%15);
            std::memset(pages,mark,bytes);
            std::this_thread::yield();
            for(size_t i=0;i<bytes;i+=64)CHECK(pages[i]==mark);
            std::lock_guard<std::mutex> memory(memoryLock);
            CHECK(bo.release()==kIOReturnSuccess);
        }
    };
    std::vector<std::thread> threads;
    for(unsigned id=0;id<Threads;++id)threads.emplace_back(work,id);
    for(auto &thread:threads)thread.join();
    uint32_t pool=0,used=9;
    NativeAdrenoMemory::usage(&pool,&used,nullptr);
    CHECK(pool==64 && !used && native_adreno_pool_shape[0]==1 && !native_adreno_pool_shape[2]);   /* the pool whole again */
    return Threads*Rounds;
}
/* NativeAdrenoMaintainOwnedPages' loop (the cache instructions themselves are never run here): each run pins the CPU, reads the line size,
 * holds one barrier on either side and covers its lines once, in order; the pass covers the range exactly. */
struct RunModel {
    size_t line=64;
    unsigned pins=0,unpins=0,barriers=0,runBarriers=0,lines=0,longest=0,inRun=0;
    bool pinned=false;
    std::vector<uintptr_t> seen;
    void pin() {CHECK(!pinned);pinned=true;++pins;inRun=0;runBarriers=0;}
    void unpin() {CHECK(pinned);pinned=false;++unpins;if(inRun>longest)longest=inRun;}
    size_t lineBytes() {CHECK(pinned);return line;}
    void barrier() {CHECK(pinned);++barriers;++runBarriers;}
    void maintain(uintptr_t address) {CHECK(pinned && runBarriers==1);seen.push_back(address);++lines;++inRun;}
};
static unsigned maintenanceRuns()
{
    unsigned controls=0;
    const size_t run=NativeAdrenoMaintainRunBytes;
    CHECK(run==64*4096);
    const struct {size_t bytes,run,line;unsigned runs;} cases[]={
        {4096,run,64,1},{run,run,64,1},{run+4096,run,64,2},{2*run,run,64,2},{1024*4096,run,64,16},{1024*4096+4096,run,64,17},
        {1024*4096,run,4096,16},{1024*4096,run,4,16},{1024*4096,1024*4096,64,1},{8192,4096,128,2}};
    for(const auto &c:cases) {
        RunModel model;model.line=c.line;
        const uintptr_t base=0x40000000;
        CHECK(NativeAdrenoMaintainRuns(base,c.bytes,c.run,model)==kIOReturnSuccess);
        CHECK(model.pins==c.runs && model.unpins==c.runs && model.barriers==2*c.runs && !model.pinned);
        CHECK(model.lines==c.bytes/c.line && model.longest<=c.run/c.line);
        for(size_t i=0;i<model.seen.size();++i)CHECK(model.seen[i]==base+i*c.line);
        ++controls;
    }
    /* A line above a page: refused with the CPU unpinned, no barrier, no line. */
    RunModel refused;refused.line=8192;
    CHECK(NativeAdrenoMaintainRuns(0x40000000,8192,run,refused)==kIOReturnUnsupported);
    CHECK(refused.pins==1 && refused.unpins==1 && !refused.barriers && !refused.lines);++controls;
    return controls;
}
static uint32_t fellBack() {return native_adreno_pool_stats[2];}   /* pooled allocations served elsewhere */
/* The pool's free extents (after bestEffortPool: 64 MiB, empty). 1500 isolated gaps are three times the 512 slots of the old table,
 * which dropped every release that needed a slot, a merge included: the pool must come back whole (one extent, a 64 MiB BO from
 * it), nothing lost. Then the bound that keeps the table from overflowing: at most 16383 pooled BOs at a time, the next one goes
 * elsewhere although a page is free. */
static unsigned poolExtents()
{
    unsigned controls=0;
    CHECK(NativeAdrenoMemory::poolBytes()==(64u<<20));
    uint32_t pool=0,used=9;
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(pool==64 && !used && native_adreno_pool_shape[0]==1);
    enum : unsigned {Small=3000,Pages=16384};
    NativeAdrenoMemory *bos=new NativeAdrenoMemory[Pages];
    for(unsigned i=0;i<Small;++i)CHECK(bos[i].allocate(4096,36,false,4096,true)==kIOReturnSuccess && bos[i].pooled());
    CHECK(native_adreno_pool_shape[0]==1 && native_adreno_pool_shape[2]==Small);
    for(unsigned i=0;i<Small;i+=2)CHECK(bos[i].release()==kIOReturnSuccess);
    CHECK(native_adreno_pool_shape[0]==Small/2+1 && native_adreno_pool_shape[1]>=Small/2+1 && native_adreno_pool_shape[2]==Small/2);
    ++controls;   /* 1501 gaps held */
    for(unsigned i=1;i<Small;i+=2)CHECK(bos[i].release()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);
    CHECK(!used && native_adreno_pool_shape[0]==1 && !native_adreno_pool_shape[2] && !native_adreno_pool_shape[4] &&
          !native_adreno_pool_shape[5]);++controls;   /* merged back into one extent, nothing lost */
    NativeAdrenoMemory whole;
    CHECK(whole.allocate(64u<<20,36,true,64u<<20,true)==kIOReturnSuccess && whole.pooled());
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(used==64 && !native_adreno_pool_shape[0]);
    CHECK(whole.release()==kIOReturnSuccess);++controls;   /* the whole pool in one BO again */
    /* Mixed sizes freed out of order merge back too. */
    for(unsigned i=0;i<512;++i)CHECK(bos[i].allocate(4096u*(1+i%7),36,false,4096u*(1+i%7),true)==kIOReturnSuccess && bos[i].pooled());
    for(unsigned i=0;i<512;i+=3)CHECK(bos[i].release()==kIOReturnSuccess);
    for(unsigned i=1;i<512;i+=3)CHECK(bos[i].release()==kIOReturnSuccess);
    for(unsigned i=2;i<512;i+=3)CHECK(bos[i].release()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(!used && native_adreno_pool_shape[0]==1 && !native_adreno_pool_shape[2]);
    ++controls;
    const uint32_t refused=native_adreno_pool_shape[3],fell=fellBack();
    for(unsigned i=0;i<Pages-1;++i)CHECK(bos[i].allocate(4096,36,false,4096,true)==kIOReturnSuccess && bos[i].pooled());
    CHECK(native_adreno_pool_shape[2]==Pages-1 && native_adreno_pool_shape[3]==refused);
    CHECK(bos[Pages-1].allocate(4096,36,false,4096,true)==kIOReturnSuccess && !bos[Pages-1].pooled());
    CHECK(native_adreno_pool_shape[3]==refused+1 && fellBack()==fell+1);++controls;   /* the bound */
    for(unsigned i=0;i<Pages;++i)CHECK(bos[i].release()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);
    CHECK(!used && native_adreno_pool_shape[0]==1 && !native_adreno_pool_shape[2] && !native_adreno_pool_shape[4]);++controls;
    delete[] bos;
    return controls;
}
#ifdef ADRENO830_MACOS
/* UniXNU-piano (2026-11-12): a BO no single free extent of the (write-combined) pool holds is cut from several (desk-dev: 168 MiB free
 * in extents of at most 19 MiB, a 49 MiB BoAlloc sent to XNU, which had no pages). Runs after poolExtents: the pool is 64 MiB, empty,
 * write-combined. */
extern "C" volatile uint32_t native_adreno_pool_split[3];
static unsigned poolPieces()
{
    unsigned controls=0;
    uint32_t pool=0,used=9;
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(pool==64 && !used && native_adreno_pool_shape[0]==1);
    enum : size_t {Mib=size_t(1)<<20,Blocks=16};
    NativeAdrenoMemory blocks[Blocks];
    unsigned char *where[Blocks];
    for(size_t i=0;i<Blocks;++i) {
        CHECK(blocks[i].allocate(4*Mib,36,false,4*Mib,true)==kIOReturnSuccess && blocks[i].pooled() && blocks[i].spanCount()==1);
        where[i]=static_cast<unsigned char *>(blocks[i].mapping());
        std::memset(where[i],0x5a,4*Mib);
    }
    for(size_t i=0;i<Blocks;i+=2)CHECK(blocks[i].release()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(used==32 && native_adreno_pool_shape[0]==8 && NativeAdrenoMemory::poolLargestFree()==4*Mib);
    /* 12 MiB from three 4 MiB gaps: three spans in BO order, no kernel mapping, zeroed by span, write-combined by flag alone. */
    const uint32_t cut=native_adreno_pool_split[0],pieces=native_adreno_pool_split[1];
    NativeAdrenoMemory split;
    CHECK(split.allocatePooled(12*Mib,36)==kIOReturnSuccess && split.pooled() && split.spanCount()==3 && !split.mapping());
    CHECK(native_adreno_pool_split[0]==cut+1 && native_adreno_pool_split[1]==pieces+3);
    for(size_t i=0;i<3;++i)CHECK(split.spans()[i].offset==i*4*Mib && split.spans()[i].bytes==4*Mib);
    CHECK(split.holdForDevice()==kIOReturnNotReady && split.finishPooled()==kIOReturnSuccess);
    for(size_t i=0;i<Blocks;i+=2) {   /* the three gaps it took are zero, the other gap still holds the pattern */
        bool taken=false;
        for(size_t j=0;j<3;++j)taken|=split.spans()[j].physical==blocks[1].spans()[0].physical+(uint64_t)(where[i]-where[1]);
        if(taken)for(size_t k=0;k<4*Mib;k+=4096)CHECK(!where[i][k] && !where[i][k+4095]);
    }
    CHECK(split.makeWriteCombined()==kIOReturnSuccess && split.writeCombined() && split.holdForDevice()==kIOReturnSuccess &&
          split.retireAfterQuiescence()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(used==44 && native_adreno_pool_shape[0]==5);++controls;
    /* One span asked: not cut (the pool gives none, the caller goes elsewhere). More than is free: refused whole, nothing taken. */
    NativeAdrenoMemory whole,large;
    CHECK(whole.allocatePooled(8*Mib,36,true)==kIOReturnNoResources && !whole.backed());++controls;
    CHECK(large.allocatePooled(24*Mib,36)==kIOReturnNoResources && !large.backed());
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(used==44 && native_adreno_pool_shape[0]==5);++controls;
    /* Released: every extent goes back, merged with its neighbours. */
    CHECK(split.release()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(used==32 && native_adreno_pool_shape[0]==8);++controls;
    for(size_t i=1;i<Blocks;i+=2)CHECK(blocks[i].release()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(!used && native_adreno_pool_shape[0]==1 && !native_adreno_pool_shape[2]);++controls;
    /* More gaps than NativeAdrenoPoolMaxPieces would need: 200 single free pages between pooled pages, 100 pages asked (65 pieces
     * would be needed): refused, every piece taken given back. */
    NativeAdrenoMemory *pages=new NativeAdrenoMemory[400];
    for(size_t i=0;i<400;++i)CHECK(pages[i].allocate(4096,36,false,4096,true)==kIOReturnSuccess);
    NativeAdrenoMemory rest;
    CHECK(rest.allocate(64*Mib-400*4096,36,true,64*Mib-400*4096,true)==kIOReturnSuccess && rest.pooled());
    for(size_t i=0;i<400;i+=2)CHECK(pages[i].release()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);
    const uint32_t extents=native_adreno_pool_shape[0],alive=native_adreno_pool_shape[2],missed=native_adreno_pool_split[2];
    NativeAdrenoMemory scattered;
    CHECK(scattered.allocatePooled(100*4096,36)==kIOReturnNoResources && native_adreno_pool_split[2]==missed+1);
    CHECK(native_adreno_pool_shape[0]==extents && native_adreno_pool_shape[2]==alive);
    CHECK(scattered.allocatePooled(64*4096,36)==kIOReturnSuccess && scattered.spanCount()==64 && scattered.release()==kIOReturnSuccess);
    CHECK(native_adreno_pool_shape[0]==extents && native_adreno_pool_shape[2]==alive);++controls;
    for(size_t i=1;i<400;i+=2)CHECK(pages[i].release()==kIOReturnSuccess);
    CHECK(rest.release()==kIOReturnSuccess);
    NativeAdrenoMemory::usage(&pool,&used,nullptr);CHECK(!used && native_adreno_pool_shape[0]==1 && !native_adreno_pool_shape[2]);++controls;
    delete[] pages;
    return controls;
}
#endif
int main()
{
    auto profiles=successes(),failure_controls=failures(),argument_controls=arguments();reset();
    const unsigned scanout=scanoutImports(),pool=bestEffortPool(),threaded=poolThreads(),extents=poolExtents(),runs=maintenanceRuns();
#ifdef ADRENO830_MACOS
    const unsigned cut=poolPieces();
    std::printf("PASS: %u pool piece controls (a BO cut from several free extents, zeroed by span, given back whole; one span asked; "
                "more pieces than allowed)\n",cut);
#endif
    std::printf("PASS: %u cache maintenance run controls (64 pages at a time, each with its pin and barriers, the whole range once)\n",runs);
    std::printf("PASS: %u pooled BOs zeroed outside the lock by four threads, none reaching another's pages\n",threaded);
    std::printf("PASS: %u scanout/device-import controls and %u best-effort pool controls (the zero fill after the Memory lock, its failure "
                "given back whole)\n",scanout,pool);
    std::printf("PASS: %u pool extent controls (1501 gaps, mixed sizes merged back whole, at most 16383 pooled BOs, nothing lost)\n",
                extents);
    std::printf("PASS: %u allocation profiles, %u allocation/cache/lifetime failure controls, %u argument/CPU refusal controls\n",profiles,failure_controls,argument_controls);
    std::puts("Host IOKit doubles only; no real kernel allocation, physical provenance, cache/DMA, SMMU, GPU or fence qualification");
}
/* Physical-page maintenance (imported BOs): record it; no host page is touched. */
/* Host double: the page attribute change of user BOs has no host equivalent (no host page is touched). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool){return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}
