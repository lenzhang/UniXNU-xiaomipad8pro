/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_MAP_LINES_H
#define NATIVE_ADRENO_MAP_LINES_H
/* UniXNU-piano (macOS 27, compiled with ADRENO830_MACOS, 2026-11-12): the console's GPU address-space lines, as pure functions the host
 * tests check (tests/test_vamap.cpp). The publish shell (NativeAdrenoPublish.cpp) prints them through boot_progress_gpu_event, whose
 * ring keeps 159 characters with its "gpu ": every line here stays under NativeAdrenoMapLineBytes.
 *  - NativeAdrenoMapRefusalLine: after a refused BoMap's "call failed" line, why (NativeAdrenoUserClient::lastMapRefusal): which
 *    argument or which limit, with its figures. The first macOS desktop (run attend4) said only "BoMap -> NoResources" and
 *    "BoMap -> BadArgument".
 *  - NativeAdrenoAllocRefusalLine (2026-11-12): after a refused BoAlloc's or BoImport's "call failed" line, why
 *    (NativeAdrenoUserClient::lastAllocRefusal): the per-object limit and the window behind it, the pool's room and XNU's answer.
 *  - NativeAdrenoVaTotalsLine / NativeAdrenoVaClientEntry: the shared VM's records and table ranges, the BO pool and what lies outside
 *    it, then per connection its mappings, ranges, mapped MiB against its window and the highest end it reached, so a run shows the
 *    trend before a limit is hit. */
#ifdef ADRENO830_MACOS
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoPublish.h"   /* NativeAdrenoReturnName */
#ifdef ADRENO830_HOST_TEST
#include <stdio.h>
#else
#include <libkern/libkern.h>   /* snprintf */
#endif
#include <string.h>

enum : unsigned {NativeAdrenoMapLineBytes=150};

static inline const char *NativeAdrenoVmWhyText(uint8_t why)
{
    switch(why) {
    case NativeAdrenoVmWhyRequest: return "the attributes or an unusable object";
    case NativeAdrenoVmWhyAperture: return "outside the GPU aperture";
    case NativeAdrenoVmWhyReserved: return "a reserved window (null guard, GMEM)";
    case NativeAdrenoVmWhyReadOnly: return "writable read-only memory or a write-back scanout";
    case NativeAdrenoVmWhyObjects: return "mapping records full";
    case NativeAdrenoVmWhySpans: return "table ranges full";
    case NativeAdrenoVmWhyOverlap: return "overlaps a mapping";
    case NativeAdrenoVmWhyPages: return "the object's pages";
    case NativeAdrenoVmWhyAlias: return "a page mapped with another cache class";
    case NativeAdrenoVmWhyReserveObjects: return "the render server's reserve of records";
    case NativeAdrenoVmWhyReserveSpans: return "the render server's reserve of ranges";
    case NativeAdrenoVmWhyHold: return "the object refused a device reference";
    case NativeAdrenoVmWhyNotReady: return "the VM is not attached or poisoned";
    default: return "no reason recorded";
    }
}

/* The detail of a refused BoMap (line gets at most NativeAdrenoMapLineBytes, NUL included). */
static inline void NativeAdrenoMapRefusalLine(char *line,size_t size,const NativeAdrenoMapRefusal &r)
{
    if(!line || !size)return;
    typedef unsigned long long u64;
    const u64 a=r.a,b=r.b,c=r.c,d=r.d;
    switch(r.why) {
    case NativeAdrenoMapWhyNotOpen: snprintf(line,size,"BoMap refused: the connection is not open");break;
    case NativeAdrenoMapWhyRequest:
        snprintf(line,size,"BoMap refused: the request: permissions %#llx writeback %llu shareability %llu, reserved/iova low bits %#llx",
                 a,b,c,d);
        break;
    case NativeAdrenoMapWhyHandle:
        snprintf(line,size,"BoMap refused: handle %#llx %s",a,b?"is not ready (being made, or its release failed)":"is no live object");
        break;
    case NativeAdrenoMapWhyMapped: snprintf(line,size,"BoMap refused: handle %#llx is mapped already, at %#llx",a,b);break;
    case NativeAdrenoMapWhyWindow:
        snprintf(line,size,"BoMap refused: iova %#llx + %llu KiB ends past this connection's window %#llx-%#llx",a,b>>10,c,d);
        break;
    case NativeAdrenoMapWhyScanout: snprintf(line,size,"BoMap refused: the scanout asked without write (permissions %#llx)",a);break;
    case NativeAdrenoMapWhyVm: {
        const u64 x=r.vm.a,y=r.vm.b,z=r.vm.c;
        char why[96];
        switch(r.vm.why) {
        case NativeAdrenoVmWhyObjects: snprintf(why,sizeof why,"mapping records full, %llu of %llu",x,y);break;
        case NativeAdrenoVmWhySpans: snprintf(why,sizeof why,"table ranges full, %llu needed, %llu free of %llu",x,y,z);break;
        case NativeAdrenoVmWhyReserveObjects: snprintf(why,sizeof why,"the render server's reserve, %llu records used, %llu kept",x,y);break;
        case NativeAdrenoVmWhyReserveSpans:
            snprintf(why,sizeof why,"the render server's reserve, %llu ranges needed, %llu free, %llu kept",x,y,z);break;
        case NativeAdrenoVmWhyOverlap: snprintf(why,sizeof why,"overlaps the mapping at %#llx",y);break;
        case NativeAdrenoVmWhyAperture: snprintf(why,sizeof why,"outside the GPU aperture (it ends at %#llx)",z);break;
        case NativeAdrenoVmWhyPages: snprintf(why,sizeof why,"the object's pages (span %llu, %#llx + %llu KiB)",x,y,z>>10);break;
        case NativeAdrenoVmWhyAlias: snprintf(why,sizeof why,"physical %#llx mapped with another cache class",x);break;
        case NativeAdrenoVmWhyHold: snprintf(why,sizeof why,"the object refused a device reference (%#llx)",x);break;
        default: snprintf(why,sizeof why,"%s",NativeAdrenoVmWhyText(r.vm.why));break;
        }
        snprintf(line,size,"BoMap refused by the VM: %s%s (iova %#llx, %llu KiB in %llu ranges)",why,
                 r.vm_captured?"":", another call's",a,b>>10,c);
        break;
    }
    default: snprintf(line,size,"BoMap refused: no reason recorded");break;
    }
}

/* The detail of a refused BoAlloc or BoImport (2026-11-12; NativeAdrenoUserClient::lastAllocRefusal): the per-object limit with the
 * window behind it, the pool's room and XNU's answer, the object slots, the import's stage. Sizes in MiB, rounded up. */
static inline void NativeAdrenoAllocRefusalLine(char *line,size_t size,const NativeAdrenoAllocRefusal &r)
{
    if(!line || !size)return;
    typedef unsigned long long u64;
    const char *what=r.method==NativeAdrenoMethodBoImport?"BoImport":"BoAlloc";
    const auto mib=[](uint64_t bytes) {return u64((bytes+(UINT64_C(1)<<20)-1)>>20);};
    switch(r.why) {
    case NativeAdrenoAllocWhyNotOpen: snprintf(line,size,"%s refused: the connection is not open",what);break;
    case NativeAdrenoAllocWhyRequest:
        if(r.method==NativeAdrenoMethodBoImport)
            snprintf(line,size,"%s refused: the request: %llu bytes at %#llx (both whole 4 KiB pages, nonzero)",what,u64(r.a),u64(r.b));
        else snprintf(line,size,"%s refused: the request: %llu bytes (whole 4 KiB pages), alignment 2^%llu (12..24), contiguous %llu",what,
                      u64(r.a),u64(r.b),u64(r.c));
        break;
    case NativeAdrenoAllocWhyLimit:
        if(r.c)snprintf(line,size,"%s refused: %llu MiB is past this connection's per-object limit of %llu MiB (its GPU window)",what,
                        mib(r.a),u64(r.b>>20));
        else snprintf(line,size,"%s refused: %llu MiB is past the per-object limit of %llu MiB",what,mib(r.a),u64(r.b>>20));
        break;
    case NativeAdrenoAllocWhyObjects: snprintf(line,size,"%s refused: no object slot free, %llu live of %llu",what,u64(r.a),u64(r.b));break;
    case NativeAdrenoAllocWhyMemory: {
        const char *code=NativeAdrenoReturnName(IOReturn(r.b));
        char kept[40]="";
        if(r.e)snprintf(kept,sizeof kept,", %llu kept for the render server",mib(r.e));
        snprintf(line,size,"%s refused: %llu MiB: pool %llu MiB free, largest %llu%s; then no pages from XNU (%#llx%s%s)",what,mib(r.a),
                 u64(r.c>>20),u64(r.d>>20),kept,u64(r.b),code?" ":"",code?code:"");
        break;
    }
    case NativeAdrenoAllocWhyWriteCombine:
        snprintf(line,size,"%s refused: %llu MiB: its pages' write-combined attribute was refused (%#llx)",what,mib(r.a),u64(r.b));
        break;
    case NativeAdrenoAllocWhyImport: {
        const char *code=NativeAdrenoReturnName(IOReturn(r.b));
        snprintf(line,size,"%s refused: %llu MiB at %#llx: the task's pages were not pinned (%#llx%s%s, stage %llu)",what,mib(r.a),u64(r.d),
                 u64(r.b),code?" ":"",code?code:"",u64(r.c));
        break;
    }
    default: snprintf(line,size,"%s refused: no reason recorded",what);break;
    }
}

/* The VM's totals: "va/map: VM <records>/<max> records, <ranges>/<capacity> ranges (<permille/10>%), <MiB> MiB mapped; pool <used>/<size>
 * MiB, <MiB> MiB outside it". */
static inline void NativeAdrenoVaTotalsLine(char *line,size_t size,uint64_t objects,uint64_t max_objects,uint64_t spans,uint64_t capacity,
    uint64_t mapped_mib,uint32_t pool_used_mib,uint32_t pool_mib,uint32_t outside_mib)
{
    if(!line || !size)return;
    const uint64_t permille=capacity?spans*1000/capacity:0;
    snprintf(line,size,"va/map: VM %llu/%llu records, %llu/%llu ranges (%llu.%llu%%), %llu MiB mapped; pool %u/%u MiB, %u MiB outside it",
             (unsigned long long)objects,(unsigned long long)max_objects,(unsigned long long)spans,(unsigned long long)capacity,
             (unsigned long long)(permille/10),(unsigned long long)(permille%10),(unsigned long long)mapped_mib,pool_used_mib,pool_mib,
             outside_mib);
}

/* One connection: " <slot>:<name> <mapped MiB>/<window MiB>M pk<MiB of the window's highest end> <records>o <ranges>r", ranges in
 * thousands ("k") from 10000. The name is cut to 14 characters. */
static inline int NativeAdrenoVaClientEntry(char *entry,size_t size,unsigned slot,const char *name,const NativeAdrenoMapUsage &u)
{
    if(!entry || !size)return 0;
    char cut[15]={};
    if(name)strlcpy(cut,name,sizeof cut);
    const unsigned long long mib=(u.bytes+(1u<<20)-1)>>20,window=u.window_bytes>>20,
                             peak=u.window_peak>u.window_start?(u.window_peak-u.window_start+(1u<<20)-1)>>20:0;
    if(u.spans>=10000)
        return snprintf(entry,size," %u:%s %llu/%lluM pk%llu %lluo %llukr",slot,cut,mib,window,peak,(unsigned long long)u.objects,
                        (unsigned long long)(u.spans/1000));
    return snprintf(entry,size," %u:%s %llu/%lluM pk%llu %lluo %llur",slot,cut,mib,window,peak,(unsigned long long)u.objects,
                    (unsigned long long)u.spans);
}

/* 2026-11-12 (desk-dev: WindowServer's 1.2 GiB of own BOs, nothing said what they were): one connection's live objects by size,
 * "va/sizes: <slot>:<name> <1M <n>/<MiB>M 1-16M <n>/<MiB>M 16-64M <n>/<MiB>M 64M+ <n>/<MiB>M imp <n>/<MiB>M" (MiB rounded up): the
 * runtime's blocks (16-64 MiB), large textures and conversions (64 MiB and more) and imports told apart. */
static inline int NativeAdrenoVaSizesLine(char *line,size_t size,unsigned slot,const char *name,const NativeAdrenoSizeUsage &u)
{
    if(!line || !size)return 0;
    char cut[15]={};
    if(name)strlcpy(cut,name,sizeof cut);
    typedef unsigned long long u64;
    const auto m=[](uint64_t bytes) {return u64((bytes+(UINT64_C(1)<<20)-1)>>20);};
    return snprintf(line,size,"va/sizes: %u:%s <1M %llu/%lluM 1-16M %llu/%lluM 16-64M %llu/%lluM 64M+ %llu/%lluM imp %llu/%lluM",slot,cut,
                    u64(u.count[0]),m(u.bytes[0]),u64(u.count[1]),m(u.bytes[1]),u64(u.count[2]),m(u.bytes[2]),u64(u.count[3]),
                    m(u.bytes[3]),u64(u.count[4]),m(u.bytes[4]));
}
#endif
#endif
