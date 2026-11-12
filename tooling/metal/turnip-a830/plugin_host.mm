// SPDX-License-Identifier: BSD-2-Clause-Patent
// The staged plugin itself on the build Mac, over the host copy of Adreno830's kernel stack (tooling/metal/check-turnip-a830-host.py
// inserts its IOKit shim): the bundle's executable (NativeAdrenoMetal: the VM's plugin, frontend and metal2vulkan bridge) is
// dlopen'ed, its frontend opens a Metal device on the bundle's Turnip (TU_DARWIN_SERVICE=AGXAccelerator, as the plugin sets it), and
// the work a compositor gives a GPU goes through every layer down to the kernel ABI: AIR libraries translated (AIR -> SPIR-V -> NIR,
// then ir3 for the A830), a compute dispatch, a render pass into an IOSurface (the BO import WindowServer's framebuffers take), a
// blit, each command buffer committed and completed. The software CP executes nothing, so no pixel or value is checked: what is
// checked is that every object is made, every command buffer completes without an error, and the kernel saw the submissions and
// the IOSurface import. Then (2026-11-12) the per-object limit end to end (maxBufferLength against the kernel's limit and the window,
// one byte more refused before the kernel), Metal's largest 2D textures (16384x16384 BGRA8, own and over an IOSurface: made where the
// limit takes 1 GiB, else refused before the kernel) and 4096x2160 video frames sampled ('444v' as format 503, '420v' as 500), with
// the hostsim's figures (host_gpu_object_limit, host_gpu_alloc_largest, host_gpu_import_largest, host_gpu_vm_spans).
//
//   plugin_host PLUGIN_EXECUTABLE TURNIP_DYLIB CACHE_DIR
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

using Open = id (*)(const char *, const char *, uint64_t, void (*)(const char *, const char *));
using LibraryWithAir = id (*)(const void *, size_t);
using FixtureAir = const void *(*)(size_t *);
using Counter = unsigned (*)(void);
using MethodCounter = unsigned (*)(uint32_t);
using Bytes = uint64_t (*)(void);

static std::vector<std::string> gMissing;
static void missing(const char *cls, const char *selector) { gMissing.push_back(std::string(cls) + " " + selector); }
static unsigned gFailures;
#define CHECK(cond, ...) do { if (!(cond)) { ++gFailures; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static bool finish(id<MTLCommandBuffer> cb, const char *what)
{
    [cb commit];
    [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "%s: status %ld error %s", what, (long)cb.status,
          cb.error.localizedDescription.UTF8String ?: "none");
    return cb.status == MTLCommandBufferStatusCompleted;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 4)
            return 2;
        void *plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!plugin) {
            printf("FAIL: dlopen %s\n", dlerror());
            return 1;
        }
        auto open = (Open)dlsym(plugin, "native_bridge_metal_open");
        auto libraryWithAir = (LibraryWithAir)dlsym(plugin, "native_bridge_metal_library_with_air");
        auto fixture = (FixtureAir)dlsym(plugin, "nmb_fixture_air");
        auto lastError = (const char *(*)(void))dlsym(plugin, "nmb_last_error");
        auto submissions = (Counter)dlsym(RTLD_DEFAULT, "host_gpu_submissions");
        auto successes = (MethodCounter)dlsym(RTLD_DEFAULT, "host_gpu_successes");
        if (!open || !libraryWithAir || !fixture || !submissions || !successes) {
            printf("FAIL: entry points (the plugin's or the host kernel's)\n");
            return 1;
        }
        // What the plugin's deviceForPort sets before it opens the device (NativeAdrenoMetalPlugin.mm).
        setenv("TU_DARWIN_SERVICE", "AGXAccelerator", 1);
        setenv("METAL2VULKAN_BLOCK_MIB", "16", 0);
        setenv("NMB_LOG_STDERR", "1", 1);
        // The render server's slot is WindowServer's: the plugin has its runtime read 4:2:0 video frames' planes in place there.
        const char *slotEnv = getenv("HOSTSIM_SLOT");
        if (!slotEnv || !strtoul(slotEnv, nullptr, 0))
            setenv("METAL2VULKAN_YCBCR_PLANES_ONLY", "1", 0);
        id<MTLDevice> device = open(argv[2], argv[3], 0x4e41, missing);
        if (!device) {
            printf("FAIL: no device: %s\n", lastError ? lastError() : "?");
            return 1;
        }
        printf("device: %s\n", device.name.UTF8String);
        const unsigned submittedBefore = submissions(), importsBefore = successes(16 /* NativeAdrenoMethodBoImport */);

        size_t bytes = 0;
        const void *air = fixture(&bytes);
        id<MTLLibrary> library = air ? libraryWithAir(air, bytes) : nil;
        CHECK(library, "the bridge's fixture library");
        if (!library)
            return 1;
        NSError *error = nil;
        id<MTLComputePipelineState> compute =
            [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"scale_and_offset"] error:&error];
        CHECK(compute, "compute pipeline: %s", error.localizedDescription.UTF8String ?: "");
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [library newFunctionWithName:@"colored_vertex"];
        rd.fragmentFunction = [library newFunctionWithName:@"colored_fragment"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> render = [device newRenderPipelineStateWithDescriptor:rd error:&error];
        CHECK(render, "render pipeline: %s", error.localizedDescription.UTF8String ?: "");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        CHECK(queue, "command queue");
        if (!compute || !render || !queue)
            return 1;

        // 1. compute over shared buffers
        std::vector<float> in(1000);
        for (unsigned i = 0; i < in.size(); ++i)
            in[i] = (float)i;
        id<MTLBuffer> input = [device newBufferWithBytes:in.data() length:in.size() * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> output = [device newBufferWithLength:in.size() * 4 options:MTLResourceStorageModeShared];
        const float scale = 2;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:compute];
        [ce setBuffer:input offset:0 atIndex:0];
        [ce setBuffer:output offset:0 atIndex:1];
        [ce setBytes:&scale length:4 atIndex:2];
        [ce dispatchThreads:MTLSizeMake(in.size(), 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [ce endEncoding];
        finish(cb, "compute");

        // 2. a render pass into an IOSurface, as WindowServer draws into its framebuffers: the surface's pages become a BO (BoImport)
        NSDictionary *properties = @{(id)kIOSurfaceWidth: @256, (id)kIOSurfaceHeight: @256, (id)kIOSurfaceBytesPerElement: @4,
                                     (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA')};
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256
                                                                                     height:256 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = surface ? [device newTextureWithDescriptor:td iosurface:surface plane:0] : nil;
        CHECK(target, "IOSurface texture");
        if (target) {
            static const float positions[6][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}, {-1, -1}};
            std::vector<float> colors;
            for (unsigned i = 0; i < 6; ++i)
                colors.insert(colors.end(), {1.0f, 0.2f, 0.0f, 1.0f});
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
            cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
            [re setRenderPipelineState:render];
            [re setVertexBytes:positions length:sizeof positions atIndex:0];
            [re setVertexBytes:colors.data() length:colors.size() * sizeof(float) atIndex:1];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [re endEncoding];
            finish(cb, "render into an IOSurface");
        }

        // 3. a blit
        cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be fillBuffer:output range:NSMakeRange(0, output.length) value:0x5a];
        [be endEncoding];
        finish(cb, "blit");

        const unsigned submitted = submissions() - submittedBefore, imported = successes(16) - importsBefore;
        CHECK(submitted >= 3, "the kernel saw %u submissions for 3 command buffers", submitted);
        CHECK(imported >= 1, "the kernel saw no IOSurface import (BoImport)");

        // 4. What a desktop session asks for beyond the login screen (2026-11-11), through the same layers: a heap and resources placed
        //    in it, a 4x MSAA pass resolved into a texture, a concurrent compute encoder (Core ML, MPSGraph), a shared event signalled by
        //    a command buffer, and IOSurface churn: panel-sized surfaces (3200x2136 BGRA, 26 MiB each) made, rendered into and released
        //    one after another, as WindowServer and the apps' windows do, more of them than the client window holds at once. Every
        //    import must be released with its surface: the kernel's live objects after the churn are those before it.
        const char *slotText = getenv("HOSTSIM_SLOT");
        const bool renderServer = !slotText || !strtoul(slotText, nullptr, 0);
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.size = 16 << 20;
        hd.storageMode = MTLStorageModePrivate;
        id<MTLHeap> heap = [device newHeapWithDescriptor:hd];
        CHECK(heap, "a 16 MiB private heap");
        MTLTextureDescriptor *htd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256
                                                                                   mipmapped:NO];
        htd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        htd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> heapTexture = heap ? [heap newTextureWithDescriptor:htd] : nil;
        id<MTLBuffer> heapBuffer = heap ? [heap newBufferWithLength:65536 options:MTLResourceStorageModePrivate] : nil;
        CHECK(heapTexture && heapBuffer, "a texture and a buffer in the heap");
        if (heapTexture && heapBuffer) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = heapTexture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
            [re useHeap:heap];
            [re endEncoding];
            id<MTLBlitCommandEncoder> hbe = [cb blitCommandEncoder];
            [hbe fillBuffer:heapBuffer range:NSMakeRange(0, heapBuffer.length) value:1];
            [hbe endEncoding];
            finish(cb, "a render pass and a fill on heap resources");
        }
        if ([device supportsTextureSampleCount:4]) {
            MTLRenderPipelineDescriptor *rd4 = [rd copy];
            rd4.rasterSampleCount = 4;
            id<MTLRenderPipelineState> render4 = [device newRenderPipelineStateWithDescriptor:rd4 error:&error];
            CHECK(render4, "4x MSAA render pipeline: %s", error.localizedDescription.UTF8String ?: "");
            MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256
                                                                                      mipmapped:NO];
            md.textureType = MTLTextureType2DMultisample;
            md.sampleCount = 4;
            md.usage = MTLTextureUsageRenderTarget;
            md.storageMode = MTLStorageModePrivate;
            id<MTLTexture> msaa = [device newTextureWithDescriptor:md];
            id<MTLTexture> resolved = [device newTextureWithDescriptor:td];
            CHECK(msaa && resolved, "a 4x MSAA target and its resolve texture");
            if (render4 && msaa && resolved) {
                static const float positions[3][2] = {{-1, -1}, {1, -1}, {0, 1}};
                static const float colors[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = msaa;
                pass.colorAttachments[0].resolveTexture = resolved;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
                cb = [queue commandBuffer];
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
                [re setRenderPipelineState:render4];
                [re setVertexBytes:positions length:sizeof positions atIndex:0];
                [re setVertexBytes:colors length:sizeof colors atIndex:1];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                finish(cb, "a 4x MSAA pass resolved");
            }
        } else
            printf("note: the device reports no 4x MSAA\n");
        cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> concurrent = [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
        CHECK(concurrent, "a concurrent compute encoder (run serially by the frontend)");
        if (concurrent) {
            [concurrent setComputePipelineState:compute];
            [concurrent setBuffer:input offset:0 atIndex:0];
            [concurrent setBuffer:output offset:0 atIndex:1];
            [concurrent setBytes:&scale length:4 atIndex:2];
            [concurrent dispatchThreads:MTLSizeMake(in.size(), 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [concurrent memoryBarrierWithScope:MTLBarrierScopeBuffers];
            [concurrent dispatchThreads:MTLSizeMake(in.size(), 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [concurrent endEncoding];
            finish(cb, "a concurrent compute encoder");
        }
        id<MTLSharedEvent> event = [device newSharedEvent];
        CHECK(event, "a shared event");
        if (event) {
            cb = [queue commandBuffer];
            [cb encodeSignalEvent:event value:7];
            finish(cb, "a command buffer that signals a shared event");
            if (event.signaledValue != 7)
                printf("note: the shared event reads %llu after its command buffer completed (the software CP executes nothing)\n",
                       (unsigned long long)event.signaledValue);
        }
        auto liveObjects = (Counter)dlsym(RTLD_DEFAULT, "host_gpu_live_objects");
        const unsigned importsBeforeChurn = successes(16), liveBeforeChurn = liveObjects ? liveObjects() : 0;
        const unsigned churn = 24;
        unsigned churned = 0;
        for (unsigned i = 0; i < churn; ++i) {
            @autoreleasepool {
                NSDictionary *big = @{(id)kIOSurfaceWidth: @3200, (id)kIOSurfaceHeight: @2136, (id)kIOSurfaceBytesPerElement: @4,
                                      (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA')};
                IOSurfaceRef frame = IOSurfaceCreate((__bridge CFDictionaryRef)big);
                MTLTextureDescriptor *fd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:3200
                                                                                            height:2136 mipmapped:NO];
                fd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                fd.storageMode = MTLStorageModeShared;
                id<MTLTexture> frameTexture = frame ? [device newTextureWithDescriptor:fd iosurface:frame plane:0] : nil;
                if (frameTexture) {
                    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.colorAttachments[0].texture = frameTexture;
                    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLCommandBuffer> fcb = [queue commandBuffer];
                    [[fcb renderCommandEncoderWithDescriptor:pass] endEncoding];
                    if (finish(fcb, "a pass into a 3200x2136 IOSurface"))
                        ++churned;
                }
                frameTexture = nil;
                if (frame)
                    CFRelease(frame);
            }
        }
        const unsigned churnImports = successes(16) - importsBeforeChurn, liveAfterChurn = liveObjects ? liveObjects() : 0;
        CHECK(churned == churn, "%u of %u panel-sized IOSurfaces rendered into", churned, churn);
        CHECK(churnImports >= churn, "the kernel saw %u imports for %u surfaces", churnImports, churn);
        CHECK(!liveObjects || liveAfterChurn <= liveBeforeChurn + 2, "kernel objects live %u before the churn, %u after it: imports leak",
              liveBeforeChurn, liveAfterChurn);
        // A large texture in the render server's window: 8192x4096 RGBA8 (128 MiB). Larger ones and the per-object limit: sections 5 and 6.
        if (renderServer) {
            MTLTextureDescriptor *large = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8192
                                                                                            height:4096 mipmapped:NO];
            large.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            large.storageMode = MTLStorageModePrivate;
            id<MTLTexture> largeTexture = [device newTextureWithDescriptor:large];
            CHECK(largeTexture, "an 8192x4096 texture (128 MiB) in the render server's window");
        }
        printf("desktop: heap, 4x MSAA, concurrent compute, shared event, %u panel-sized IOSurface imports (live objects %u -> %u)%s\n",
               churnImports, liveBeforeChurn, liveAfterChurn, renderServer ? ", an 8192x4096 texture" : "");

        // 5. The per-allocation limit: a kernel BO holds at most Query's max_bo_bytes (Adreno830 upstream: 256 MiB; piano's macOS kernel,
        //    2026-11-12: the connection's GPU window) and lies in the connection's window. Every layer must say so before an allocation
        //    gets that far: Turnip's maxMemoryAllocationSize/maxBufferSize/maxResourceSize (0004, within the window since 0008), the
        //    device's maxBufferLength. A buffer of maxBufferLength is made (up to 1.25 GiB, the host's memory); one past it is refused
        //    without any BoAlloc reaching the kernel and refused there; with upstream's 256 MiB so is an 8192x8192 RGBA8 texture.
        auto calls = (MethodCounter)dlsym(RTLD_DEFAULT, "host_gpu_calls");
        auto objectLimit = (Bytes)dlsym(RTLD_DEFAULT, "host_gpu_object_limit");
        auto windowBytes = (Bytes)dlsym(RTLD_DEFAULT, "host_gpu_window_bytes");
        auto allocLargest = (Bytes)dlsym(RTLD_DEFAULT, "host_gpu_alloc_largest");
        auto allocBytes = (Bytes)dlsym(RTLD_DEFAULT, "host_gpu_alloc_bytes");
        auto importLargest = (Bytes)dlsym(RTLD_DEFAULT, "host_gpu_import_largest");
        const uint64_t kernelLimit = objectLimit ? objectLimit() : (uint64_t)256 << 20, window = windowBytes ? windowBytes() : 0;
        const uint64_t perObject = (window && window < kernelLimit ? window : kernelLimit) & ~(uint64_t)4095;
        const NSUInteger maxBuffer = device.maxBufferLength;
        printf("limits: maxBufferLength %lu MiB (the kernel's per-object limit %llu MiB, GPU window %llu MiB)\n", (unsigned long)(maxBuffer >> 20),
               (unsigned long long)(kernelLimit >> 20), (unsigned long long)(window >> 20));
        CHECK(maxBuffer && maxBuffer <= perObject, "maxBufferLength %lu MiB is past the per-object limit of %llu MiB", (unsigned long)(maxBuffer >> 20),
              (unsigned long long)(perObject >> 20));
        CHECK(maxBuffer + 65536 > perObject, "maxBufferLength %lu MiB is below the per-object limit of %llu MiB", (unsigned long)(maxBuffer >> 20),
              (unsigned long long)(perObject >> 20));
        if (calls) {
            const unsigned refusedBefore = calls(2 /* BoAlloc */) - successes(2);
            // A buffer of the whole window fits only an empty window: made where the limit is the kernel's own, below the window.
            const bool fitsHost = maxBuffer <= ((NSUInteger)5 << 28) && (!window || maxBuffer + ((NSUInteger)64 << 20) <= window);
            if (fitsHost) {
                @autoreleasepool {
                    id<MTLBuffer> largest = [device newBufferWithLength:maxBuffer options:MTLResourceStorageModePrivate];
                    CHECK(largest, "a buffer of maxBufferLength (%lu MiB)", (unsigned long)(maxBuffer >> 20));
                }
            }
            @autoreleasepool {
                id<MTLBuffer> over = [device newBufferWithLength:maxBuffer + 65536 options:MTLResourceStorageModePrivate];
                CHECK(!over, "a buffer past maxBufferLength was made");
                if (perObject < ((uint64_t)288 << 20)) {
                    MTLTextureDescriptor *huge = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8192
                                                                                                   height:8192 mipmapped:NO];
                    huge.storageMode = MTLStorageModePrivate;
                    huge.usage = MTLTextureUsageShaderRead;
                    id<MTLTexture> hugeTexture = [device newTextureWithDescriptor:huge];
                    CHECK(!hugeTexture, "an 8192x8192 RGBA8 texture past the per-object limit was made");
                }
            }
            const unsigned refusedAfter = calls(2) - successes(2);
            CHECK(refusedAfter == refusedBefore, "%u BoAlloc call(s) the kernel refused: the limit was not taken before the kernel",
                  refusedAfter - refusedBefore);
            printf("limits: %s; one past it%s refused before the kernel\n",
                   fitsHost ? "a buffer of maxBufferLength made" : "no buffer of maxBufferLength (the whole window, or past the host's 1.25 GiB)",
                   perObject < ((uint64_t)288 << 20) ? " and an 8192x8192 RGBA8 texture" : "");
        }

        // 6. Large resources (2026-11-12): Metal's largest 2D texture, 16384x16384 BGRA8 (1 GiB and its UBWC metadata), rendered into, as a
        //    texture of its own and over a 16384x16384 BGRA IOSurface (a 1 GiB import), where the per-object limit takes it (the render
        //    server's 3.5 GiB window of piano's macOS kernel); where it does not, both are refused before any kernel call.
        if (allocLargest && importLargest && calls) {
            const bool fits = perObject >= ((uint64_t)1 << 30) + ((uint64_t)64 << 20);
            const unsigned refusedBefore = calls(2) - successes(2), importRefusedBefore = calls(16) - successes(16);
            MTLTextureDescriptor *giant = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:16384
                                                                                            height:16384 mipmapped:NO];
            giant.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            giant.storageMode = MTLStorageModePrivate;
            uint64_t textureBo = 0, surfaceImport = 0;
            auto vmSpans = (size_t (*)(void))dlsym(RTLD_DEFAULT, "host_gpu_vm_spans");
            auto poolUsed = (Bytes)dlsym(RTLD_DEFAULT, "host_gpu_pool_used_mib");
            size_t spansBefore = vmSpans ? vmSpans() : 0, spansWith = spansBefore;
            const uint64_t poolBefore = poolUsed ? poolUsed() : 0;
            uint64_t poolWith = poolBefore;
            @autoreleasepool {
                id<MTLTexture> texture = [device newTextureWithDescriptor:giant];
                if (vmSpans)
                    spansWith = vmSpans();
                if (poolUsed)
                    poolWith = poolUsed();
                CHECK(fits ? texture != nil : texture == nil, "a 16384x16384 BGRA8 texture %s", fits ? "was refused" : "past the limit was made");
                if (texture) {
                    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.colorAttachments[0].texture = texture;
                    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    cb = [queue commandBuffer];
                    [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
                    finish(cb, "a pass into a 16384x16384 BGRA8 texture");
                    textureBo = allocLargest();
                    CHECK(textureBo >= ((uint64_t)1 << 30), "the largest BO the kernel made is %llu MiB, not the 16384x16384 texture's 1 GiB",
                          (unsigned long long)(textureBo >> 20));
                }
            }
            NSDictionary *giantSurface = @{(id)kIOSurfaceWidth: @16384, (id)kIOSurfaceHeight: @16384, (id)kIOSurfaceBytesPerElement: @4,
                                           (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA')};
            IOSurfaceRef surface16k = IOSurfaceCreate((__bridge CFDictionaryRef)giantSurface);
            if (surface16k) {
                @autoreleasepool {
                    MTLTextureDescriptor *shared = [giant copy];
                    shared.storageMode = MTLStorageModeShared;
                    id<MTLTexture> texture = [device newTextureWithDescriptor:shared iosurface:surface16k plane:0];
                    CHECK(fits ? texture != nil : texture == nil, "a texture over a 16384x16384 BGRA IOSurface %s",
                          fits ? "was refused" : "past the limit was made");
                    if (texture) {
                        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                        pass.colorAttachments[0].texture = texture;
                        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                        cb = [queue commandBuffer];
                        [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
                        finish(cb, "a pass into a 16384x16384 BGRA IOSurface");
                        surfaceImport = importLargest();
                        CHECK(surfaceImport + 4095 >= IOSurfaceGetAllocSize(surface16k), "the largest import is %llu MiB, not the surface's %zu MiB",
                              (unsigned long long)(surfaceImport >> 20), IOSurfaceGetAllocSize(surface16k) >> 20);
                    }
                }
                CFRelease(surface16k);
            } else
                printf("note: IOSurfaceCreate made no 16384x16384 BGRA surface on this Mac\n");
            CHECK(calls(2) - successes(2) == refusedBefore && calls(16) - successes(16) == importRefusedBefore,
                  "the kernel refused a BoAlloc or BoImport: a limit was not taken before the kernel");
            if (fits)
                printf("large: a 16384x16384 BGRA8 texture (a BO of %llu MiB, %zu table ranges of the VM, %llu MiB more of the BO pool) and a "
                       "texture over a 16384x16384 BGRA IOSurface (an import of %llu MiB), each rendered into\n", (unsigned long long)(textureBo >> 20),
                       spansWith - spansBefore, (unsigned long long)(poolWith - poolBefore), (unsigned long long)(surfaceImport >> 20));
            else
                printf("large: a 16384x16384 BGRA8 texture and one over a 16384x16384 IOSurface refused before the kernel (per-object limit "
                       "%llu MiB)\n", (unsigned long long)(perObject >> 20));
        }

        // 7. Video frames (2026-11-12): a 4096x2160 '444v' frame as WindowServer gets the login window's aerial wallpaper (a texture of
        //    format 503, YCBCR8_444_2P, over plane 0; the runtime converts it into an image of its own) and a '420v' one (format 500,
        //    whose planes WindowServer's runtime reads in place), each sampled in a render pass. Made where the per-object limit is at
        //    least 256 MiB (the render server's and every window of piano's): the frontend has no fixed YCbCr cap since its 0003.
        id<MTLFunction> sampledFragment = [library newFunctionWithName:@"sampled_fragment"];
        CHECK(sampledFragment, "the fixture's sampled_fragment");
        MTLRenderPipelineDescriptor *sd = [rd copy];
        sd.fragmentFunction = sampledFragment;
        id<MTLRenderPipelineState> sampling = sampledFragment ? [device newRenderPipelineStateWithDescriptor:sd error:&error] : nil;
        CHECK(sampling, "the sampling pipeline: %s", error.localizedDescription.UTF8String ?: "");
        unsigned framesDrawn = 0;
        const bool videoFits = perObject >= ((uint64_t)256 << 20);
        const std::pair<OSType, MTLPixelFormat> frames[] = {{'444v', (MTLPixelFormat)503}, {'420v', (MTLPixelFormat)500}};
        for (const auto &frame : frames) {
            if (!sampling || !allocBytes || !videoFits)
                break;
            CVPixelBufferRef pixels = nullptr;
            NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
            const CVReturn made = CVPixelBufferCreate(kCFAllocatorDefault, 4096, 2160, frame.first, (__bridge CFDictionaryRef)attributes, &pixels);
            IOSurfaceRef surface = made == kCVReturnSuccess ? CVPixelBufferGetIOSurface(pixels) : nullptr;
            const char code[5] = {(char)(frame.first >> 24), (char)(frame.first >> 16), (char)(frame.first >> 8), (char)frame.first, 0};
            CHECK(surface, "a 4096x2160 '%s' pixel buffer over an IOSurface (CVReturn %d)", code, made);
            if (!surface) {
                if (pixels)
                    CVPixelBufferRelease(pixels);
                continue;
            }
            @autoreleasepool {
                const uint64_t before = allocBytes();
                MTLTextureDescriptor *vd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:frame.second width:4096 height:2160 mipmapped:NO];
                vd.usage = MTLTextureUsageShaderRead;
                vd.storageMode = MTLStorageModeShared;
                id<MTLTexture> video = [device newTextureWithDescriptor:vd iosurface:surface plane:0];
                CHECK(video, "a 4096x2160 format %lu texture over a '%s' surface", (unsigned long)frame.second, code);
                MTLTextureDescriptor *td2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1024 height:1024
                                                                                            mipmapped:NO];
                td2.usage = MTLTextureUsageRenderTarget;
                td2.storageMode = MTLStorageModePrivate;
                id<MTLTexture> target2 = [device newTextureWithDescriptor:td2];
                if (video && target2) {
                    static const float positions[6][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}, {-1, -1}};
                    static const float colors[6][4] = {};
                    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.colorAttachments[0].texture = target2;
                    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    cb = [queue commandBuffer];
                    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
                    [re setRenderPipelineState:sampling];
                    [re setVertexBytes:positions length:sizeof positions atIndex:0];
                    [re setVertexBytes:colors length:sizeof colors atIndex:1];
                    [re setFragmentTexture:video atIndex:0];
                    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                    [re endEncoding];
                    if (finish(cb, "a pass sampling a 4096x2160 video frame"))
                        ++framesDrawn;
                }
                printf("video: 4096x2160 '%s' (%zu planes, rows %zu and %zu, %zu bytes) as format %lu: %s, %llu MiB of kernel BOs for it and its pass\n",
                       code, IOSurfaceGetPlaneCount(surface), IOSurfaceGetBytesPerRowOfPlane(surface, 0), IOSurfaceGetBytesPerRowOfPlane(surface, 1),
                       IOSurfaceGetAllocSize(surface), (unsigned long)frame.second, video ? "sampled" : "REFUSED",
                       (unsigned long long)((allocBytes() - before) >> 20));
            }
            CVPixelBufferRelease(pixels);
        }
        if (!videoFits)
            printf("video: no 4096x2160 frame with a per-object limit of %llu MiB (below 256 MiB: no window of piano's)\n",
                   (unsigned long long)(perObject >> 20));
        for (auto &m : gMissing)
            printf("missing selector: %s\n", m.c_str());
        if (surface)
            CFRelease(surface);
        if (gFailures) {
            printf("FAILED %u checks\n", gFailures);
            return 1;
        }
        printf("PASS: the plugin's frontend and bridge on Turnip (ABI v2): AIR compute and render pipelines, compute, render into an "
               "IOSurface, blit; heap, 4x MSAA, concurrent compute, shared event, %u panel-sized IOSurfaces churned; %u kernel submissions, "
               "%u IOSurface import(s) before the desktop part; per-object limit %llu MiB; %u 4096x2160 video frame(s) sampled; nothing "
               "executed (software CP)\n", churnImports, submitted, imported, (unsigned long long)(perObject >> 20), framesDrawn);
        return 0;
    }
}
