// SPDX-License-Identifier: BSD-2-Clause-Patent
// The staged plugin itself on the build Mac, over the host copy of Adreno830's kernel stack (tooling/metal/check-turnip-a830-host.py
// inserts its IOKit shim): the bundle's executable (NativeAdrenoMetal: the VM's plugin, frontend and metal2vulkan bridge) is
// dlopen'ed, its frontend opens a Metal device on the bundle's Turnip (TU_DARWIN_SERVICE=AGXAccelerator, as the plugin sets it), and
// the work a compositor gives a GPU goes through every layer down to the kernel ABI: AIR libraries translated (AIR -> SPIR-V -> NIR,
// then ir3 for the A830), a compute dispatch, a render pass into an IOSurface (the BO import WindowServer's framebuffers take), a
// blit, each command buffer committed and completed. The software CP executes nothing, so no pixel or value is checked: what is
// checked is that every object is made, every command buffer completes without an error, and the kernel saw the submissions and
// the IOSurface import.
//
//   plugin_host PLUGIN_EXECUTABLE TURNIP_DYLIB CACHE_DIR
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
        for (auto &m : gMissing)
            printf("missing selector: %s\n", m.c_str());
        if (surface)
            CFRelease(surface);
        if (gFailures) {
            printf("FAILED %u checks\n", gFailures);
            return 1;
        }
        printf("PASS: the plugin's frontend and bridge on Turnip (ABI v2): AIR compute and render pipelines, compute, render into an "
               "IOSurface, blit; %u kernel submissions, %u IOSurface import(s); nothing executed (software CP)\n", submitted, imported);
        return 0;
    }
}
