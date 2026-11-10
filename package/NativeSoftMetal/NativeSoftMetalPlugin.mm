// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeSoftMetal: the Metal plugin of NativeSoftGpu's accelerator (package/NativeSoftGpu). It makes a Metal device with no GPU behind it,
// so that macOS 27's WindowServer gets past its wait for a Metal device and draws the desktop on the CPU.
//
// What WindowServer does with the device (macOS 26.6 SkyLight and QuartzCore read on the build Mac, 2026-11-11; 27.0.1 not checked):
//   - SkyLight waits up to 30 s for MTLCopyAllDevices to be non-empty, and aborts if it stays empty.
//   - The display's accelerator is the first device's -acceleratorPort. With 0, SkyLight's per-accelerator lookup returns its software
//     compositor (CompositorSW), not CompositorMetal.
//   - Core Animation's WindowServer display (CA::WindowServer::AccelServer) builds a Metal renderer on the first device. When that
//     fails (the device makes no command queue), it renders the update with its software renderer (CA::OGL::SWContext).
// So this device answers acceleratorPort 0 and nil for every command queue, buffer, texture and library. That is Metal's ordinary
// failure answer, and Metal clients check for it.
//
// Where: IOGPU's +[IOGPUMetalDevice registerAcceleratorService:] loads this bundle from /Library/GPUBundles for an accelerator whose
// MetalPluginName is NativeSoftMetal. It takes NSPrincipalClass NativeSoftMetalDevice (an IOGPUMetalDevice subclass) and registers what
// -initWithAcceleratorPort: returns. The bundle's CDHash must be in the boot's trust cache (tooling/k27/stage-soft-metal.py).
//
// Kernel boot arguments, read through kern.bootargs, so a test needs no restaging:
//   soft-metal=0       no device in any process (as if the plugin were missing)
//   soft-metal-all=1   a device in every process that asks (default: WindowServer only; the others then see no Metal device at all,
//                      which their frameworks handle as on a Mac without a GPU)
// Reports go to stderr ("NativeSoftMetal: <process>: ..."); kernel boot argument stderr-log=1 puts them in the console log.
// The IOGPU registration answers (_setAcceleratorService:, _setDeviceWrapper:, _deviceWrapper, the housekeeping no-ops and the stub
// survey) follow macOS-DroidVM's overlay/metal/NativeAdrenoMetalPlugin.mm (package/NativeSoftMetal/UPSTREAM).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <atomic>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

@interface IOGPUMetalDevice : NSObject
@end

@interface NativeSoftMetalDevice : IOGPUMetalDevice
- (id)initWithAcceleratorPort:(io_service_t)port;
@end

static const char *const kDeviceName = "NativeSoftMetal (software, no GPU)";

static bool bootArg(const char *name, unsigned *value)
{
    char args[1024] = {};
    size_t size = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0)) return false;
    size_t length = strlen(name);
    for (char *at = args; (at = strstr(at, name)); at += length) {
        if ((at == args || at[-1] == ' ') && at[length] == '=') {
            *value = (unsigned)strtoul(at + length + 1, nullptr, 0);
            return true;
        }
    }
    return false;
}

// ---- the device's answers ---------------------------------------------------------------------------------------------------------
static uint64_t gRegistryID;
static id gAcceleratorService;
static __weak id gDeviceWrapper;

// Names a Metal call that wanted GPU work, once per selector and process: the survey of what each client asks a GPU for.
static void noteRefused(SEL selector)
{
    static NSMutableSet *seen;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ seen = [NSMutableSet set]; });
    NSString *name = NSStringFromSelector(selector);
    @synchronized(seen) {
        if ([seen containsObject:name]) return;
        [seen addObject:name];
    }
    fprintf(stderr, "NativeSoftMetal: %s: %s -> nil (no GPU)\n", getprogname(), name.UTF8String);
}

static id refuse(id, SEL selector) { noteRefused(selector); return nil; }
static id refuse1(id, SEL selector, uint64_t) { noteRefused(selector); return nil; }
static id refuse2(id, SEL selector, uint64_t, uint64_t) { noteRefused(selector); return nil; }
static id refuse3(id, SEL selector, uint64_t, uint64_t, uint64_t) { noteRefused(selector); return nil; }
static id refuse4(id, SEL selector, uint64_t, uint64_t, uint64_t, uint64_t) { noteRefused(selector); return nil; }
// The forms with an NSError ** last: the error is set, as Metal's own refusals do, for callers that log it.
static void setError(NSError *__autoreleasing *error)
{
    if (error) *error = [NSError errorWithDomain:@"NativeSoftMetal" code:1 userInfo:@{NSLocalizedDescriptionKey: @"no GPU"}];
}
static id refuseError2(id, SEL selector, uint64_t, NSError *__autoreleasing *error) { noteRefused(selector); setError(error); return nil; }
static id refuseError3(id, SEL selector, uint64_t, uint64_t, NSError *__autoreleasing *error) { noteRefused(selector); setError(error); return nil; }

static NSString *name(id, SEL) { return @(kDeviceName); }
static uint64_t registryID(id, SEL) { return gRegistryID; }
static uint32_t acceleratorPort(id, SEL) { return 0; }   // SkyLight: no accelerator, so the software compositor
static void setAcceleratorService(id, SEL, id service) { gAcceleratorService = service; }
static void setDeviceWrapper(id, SEL, id wrapper) { gDeviceWrapper = wrapper; }
static id deviceWrapper(id self, SEL) { id wrapper = gDeviceWrapper; return wrapper ? wrapper : self; }
static BOOL yes(id, SEL) { return YES; }
static BOOL no(id, SEL) { return NO; }
static BOOL noWith(id, SEL, uint64_t) { return NO; }
static uint64_t zero(id, SEL) { return 0; }
static uint32_t one(id, SEL) { return 1; }
static void nothing(id, SEL) {}
static uint64_t workingSetSize(id, SEL) { return NSProcessInfo.processInfo.physicalMemory / 3 * 2; }
static MTLSize threadgroup(id, SEL) { return MTLSizeMake(1, 1, 1); }

// Any other IOGPUMetalDevice selector a client sends: a stub answering 0 (nil, NO), named once on stderr. Only selectors IOGPU's class
// has, so -respondsToSelector: stays NO for what a real device does not answer either.
static uint64_t stub(id, SEL) { return 0; }
static BOOL resolveStub(id cls, SEL, SEL selector)
{
    Class reference = objc_getClass("IOGPUMetalDevice");
    Method method = reference ? class_getInstanceMethod(reference, selector) : nullptr;
    if (!method) return NO;
    fprintf(stderr, "NativeSoftMetal: %s: stubbed %s\n", getprogname(), sel_getName(selector));
    return class_addMethod((Class)cls, selector, (IMP)stub, method_getTypeEncoding(method));
}

struct MethodSpec {
    const char *name;
    IMP imp;
    const char *types;
};

static const MethodSpec kDeviceMethods[] = {
    {"name", (IMP)name, "@16@0:8"},
    {"registryID", (IMP)registryID, "Q16@0:8"},
    {"acceleratorPort", (IMP)acceleratorPort, "I16@0:8"},
    {"_setAcceleratorService:", (IMP)setAcceleratorService, "v24@0:8@16"},
    {"_setDeviceWrapper:", (IMP)setDeviceWrapper, "v24@0:8@16"},
    {"_deviceWrapper", (IMP)deviceWrapper, "@16@0:8"},
    {"isBuiltIn", (IMP)yes, "B16@0:8"},
    {"hasUnifiedMemory", (IMP)yes, "B16@0:8"},
    {"isLowPower", (IMP)no, "B16@0:8"},
    {"isHeadless", (IMP)no, "B16@0:8"},
    {"isRemovable", (IMP)no, "B16@0:8"},
    {"supportPriorityBand", (IMP)no, "B16@0:8"},
    {"supportsBackgroundAppRole", (IMP)no, "B16@0:8"},
    {"isSlotted", (IMP)no, "B16@0:8"},
    {"supportsResourceDetachBacking", (IMP)no, "B16@0:8"},
    {"supportsVertexAmplification", (IMP)no, "B16@0:8"},
    {"supportsRaytracing", (IMP)no, "B16@0:8"},
    {"supportsFamily:", (IMP)noWith, "B24@0:8q16"},
    {"supportsFeatureSet:", (IMP)noWith, "B24@0:8Q16"},
    {"supportsTextureSampleCount:", (IMP)noWith, "B24@0:8Q16"},
    {"location", (IMP)zero, "Q16@0:8"},
    {"locationNumber", (IMP)zero, "Q16@0:8"},
    {"maxTransferRate", (IMP)zero, "Q16@0:8"},
    {"peerGroupID", (IMP)zero, "Q16@0:8"},
    {"peerIndex", (IMP)zero, "I16@0:8"},
    {"peerCount", (IMP)one, "I16@0:8"},
    {"dedicatedMemorySize", (IMP)zero, "Q16@0:8"},
    {"currentAllocatedSize", (IMP)zero, "Q16@0:8"},
    {"recommendedMaxWorkingSetSize", (IMP)workingSetSize, "Q16@0:8"},
    {"sharedMemorySize", (IMP)workingSetSize, "Q16@0:8"},
    {"maxThreadsPerThreadgroup", (IMP)threadgroup, "{?=QQQ}16@0:8"},
    {"featureProfile", (IMP)zero, "Q16@0:8"},
    {"gpuCoreCount", (IMP)zero, "Q16@0:8"},
    {"hwResourcePoolCount", (IMP)zero, "I16@0:8"},
    {"kickCleanupQueue", (IMP)nothing, "v16@0:8"},
    {"updateGPUSelectionProperties", (IMP)nothing, "v16@0:8"},
    {"updateResourcePoolPurgeability", (IMP)nothing, "v16@0:8"},
    {"periodicUpdateResourcePoolPurgeability", (IMP)nothing, "v16@0:8"},
    {"cancelPeriodicUpdateResourcePoolPurgeability", (IMP)nothing, "v16@0:8"},
    // No GPU work: Core Animation's WindowServer display then renders in software (its Metal context needs a command queue).
    {"newCommandQueue", (IMP)refuse, "@16@0:8"},
    {"newCommandQueueWithMaxCommandBufferCount:", (IMP)refuse1, "@24@0:8Q16"},
    {"newCommandQueueWithDescriptor:", (IMP)refuse1, "@24@0:8@16"},
    {"newBufferWithLength:options:", (IMP)refuse2, "@32@0:8Q16Q24"},
    {"newBufferWithBytes:length:options:", (IMP)refuse3, "@40@0:8r^v16Q24Q32"},
    {"newBufferWithBytesNoCopy:length:options:deallocator:", (IMP)refuse4, "@48@0:8^v16Q24Q32@?40"},
    {"newTextureWithDescriptor:", (IMP)refuse1, "@24@0:8@16"},
    {"newTextureWithDescriptor:iosurface:plane:", (IMP)refuse3, "@40@0:8@16^{__IOSurface=}24Q32"},
    {"newSharedTextureWithDescriptor:", (IMP)refuse1, "@24@0:8@16"},
    {"newHeapWithDescriptor:", (IMP)refuse1, "@24@0:8@16"},
    {"newSamplerStateWithDescriptor:", (IMP)refuse1, "@24@0:8@16"},
    {"newDepthStencilStateWithDescriptor:", (IMP)refuse1, "@24@0:8@16"},
    {"newEvent", (IMP)refuse, "@16@0:8"},
    {"newSharedEvent", (IMP)refuse, "@16@0:8"},
    {"newSharedEventWithHandle:", (IMP)refuse1, "@24@0:8@16"},
    {"newFence", (IMP)refuse, "@16@0:8"},
    {"newDefaultLibrary", (IMP)refuse, "@16@0:8"},
    {"newDefaultLibraryWithBundle:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newLibraryWithFile:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newLibraryWithURL:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newLibraryWithData:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newLibraryWithSource:options:error:", (IMP)refuseError3, "@40@0:8@16@24^@32"},
    {"newRenderPipelineStateWithDescriptor:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newComputePipelineStateWithFunction:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newBinaryArchiveWithDescriptor:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newCounterSampleBufferWithDescriptor:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newIOHandleWithURL:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newResidencySetWithDescriptor:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newLogStateWithDescriptor:error:", (IMP)refuseError2, "@32@0:8@16^@24"},
    {"newArgumentEncoderWithArguments:", (IMP)refuse1, "@24@0:8@16"},
    {"newIndirectCommandBufferWithDescriptor:maxCommandCount:options:", (IMP)refuse3, "@40@0:8@16Q24Q32"},
    {"newRasterizationRateMapWithDescriptor:", (IMP)refuse1, "@24@0:8@16"},
};

// One device per process, of a class made at run time over Metal's _MTLDevice (the base Metal's registration expects of a plugin's device).
static id makeDevice()
{
    Class base = objc_getClass("_MTLDevice");
    if (!base) {
        fprintf(stderr, "NativeSoftMetal: %s: no _MTLDevice in Metal.framework\n", getprogname());
        return nil;
    }
    Class cls = objc_allocateClassPair(base, "NativeSoftMetalDeviceObject", 0);
    if (!cls) return nil;
    for (const MethodSpec &method : kDeviceMethods)
        class_addMethod(cls, sel_registerName(method.name), method.imp, method.types);
    class_addMethod(object_getClass(cls), @selector(resolveInstanceMethod:), (IMP)resolveStub, "B24@0:8:16");
    class_addProtocol(cls, @protocol(MTLDevice));
    // Metal's registration (_MTLAddDevice) asserts its private device protocol too; the methods are still looked up one by one.
    if (Protocol *spi = objc_getProtocol("MTLDeviceSPI")) class_addProtocol(cls, spi);
    objc_registerClassPair(cls);
    return [cls new];
}

static id deviceForPort(io_service_t port)
{
    static id device;
    static bool tried;
    @synchronized([NativeSoftMetalDevice class]) {
        if (tried) return device;
        tried = true;
        unsigned on = 1, all = 0;
        bootArg("soft-metal", &on);
        bootArg("soft-metal-all", &all);
        const bool windowServer = !strcmp(getprogname(), "WindowServer");
        if (!on || (!all && !windowServer)) {
            fprintf(stderr, "NativeSoftMetal: %s: no device (%s)\n", getprogname(), !on ? "soft-metal=0" : "WindowServer only");
            return nil;
        }
        IORegistryEntryGetRegistryEntryID(port, &gRegistryID);
        device = makeDevice();
        fprintf(stderr, "NativeSoftMetal: %s: device %s, registry ID %#llx, accelerator port 0, no command queue: software compositing\n",
            getprogname(), device ? kDeviceName : "not made", (unsigned long long)gRegistryID);
        return device;
    }
}

@implementation NativeSoftMetalDevice
- (id)initWithAcceleratorPort:(io_service_t)port
{
    // The registered device is the object deviceForPort makes, not this one (IOGPUMetalDevice's own initializer would open the
    // accelerator's user client, which NativeSoftGpu refuses). This one is kept: -[IOGPUMetalDevice dealloc] tears down state its
    // initializer never made.
    static NSMutableArray *kept;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ kept = [NSMutableArray array]; });
    @synchronized(kept) {
        [kept addObject:self];
    }
    return deviceForPort(port);
}
@end
