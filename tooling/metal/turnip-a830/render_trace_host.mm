// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the login window's renderers ask of the Metal device (docs/drivers/gpu-userspace.md, "The login window's rendering, 2026-11-12"):
// RenderBox (SwiftUI's renderer: RBDevice, RBDrawable, RBDisplayList, RBShape, RBFill, driven through the Objective-C runtime) and Core
// Animation (CARenderer over a layer tree: shape layers, a corner-radius layer, a CABackdropLayer with a blur, CASDFLayer glass
// elements) render a circle, a capsule and a rounded rect into an IOSurface texture of the given size, and every call they make on the
// device, the command buffers and the render encoders is printed: passes (attachments, formats, load/store, storage modes), pipelines
// (vertex/fragment function names), depth-stencil states, the bytes and buffers bound, draws, memory barriers. Only the behaviour of
// the system frameworks is observed; nothing of them is read or kept.
//
//   render_trace_host system W H OUT.png                          the Mac's own GPU, the picture written to OUT.png
//   render_trace_host plugin W H PLUGIN TURNIP CACHE [STATE]       the staged NativeAdrenoMetal over Adreno830's hostsim (as plugin_host:
//                                                                  DYLD_INSERT_LIBRARIES=<hostsim>/libhostiokit.dylib HOSTSIM_GPU=a830
//                                                                  TU_DARWIN_SERVICE=AGXAccelerator); no shader runs there
// Scenes (environment): RenderBox by default (RB_PATHS=1 CGPath shapes, RB_CLIP=1 a clipped avatar, RB_BLEND=<mode> a capsule in that
// blend mode, RB_DIST=1|2|3 a distance filter, a blur or a shadow); CA_MODE=1 Core Animation instead (CA_SDF=1 SDF fill elements,
// CA_GLASS=1 SDF glass elements, CA_BACKDROP=1 a blurred backdrop, CA_STROKE=1 a stroked circle); RB_ONLY_FAMILIES=1 prints the
// device's GPU family answers only. With the plugin, TU_DEBUG reaches its Turnip (e.g. sysmem, sysmem,nofbflush).
// Build: xcrun clang++ -target arm64e-apple-macos14.0 -fobjc-arc -std=c++17 -O1 -x objective-c++ render_trace_host.mm -o render_trace_host
//        -framework Foundation -framework Metal -framework IOSurface -framework CoreGraphics -framework ImageIO -framework QuartzCore
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string>
#include <map>

using Open = id (*)(const char *, const char *, uint64_t, void (*)(const char *, const char *));
static void missing(const char *cls, const char *selector) { fprintf(stderr, "MISSING %s %s\n", cls, selector); }

typedef struct { float r, g, b, a; } RBColor4;

@interface NSObject (RB)
- (id)initWithDevice:(id)d;
- (void)setCircleAtPoint:(CGPoint)p radius:(double)r;
- (void)setRoundedRect:(CGRect)r cornerRadius:(double)c cornerStyle:(int)s;
- (void)clipShape:(id)s mode:(int)m;
- (void)save;
- (void)restore;
- (void)setPath:(CGPathRef)p;
- (void)setStrokedCircleAtPoint:(CGPoint)p radius:(double)r lineWidth:(double)w;
- (void)drawShape:(id)s fill:(id)f alpha:(float)a blendMode:(int)m;
- (void)drawDisplayList:(id)d;
- (void)addDistanceFilterWithMaxDistance:(double)m scale:(double)s flags:(unsigned)f;
- (void)addBlurFilterWithRadius:(double)r flags:(unsigned)f;
- (void)addShadowStyleWithRadius:(double)r midpoint:(float)m offset:(CGSize)o color:(RBColor4)c colorSpace:(int)cs blendMode:(int)b flags:(unsigned)f;
- (void)renderDisplayList:(id)d flags:(unsigned)f;
- (void)setTexture:(id)t;
- (void)setPixelFormat:(NSUInteger)p;
- (void)setInitialState:(int)s;
- (void)finish;
- (void)setCompletedHandler:(void (^)(void))h;
- (void)setErrorHandler:(void (^)(NSError *))h;
@end

static std::map<void *, std::string> gPipelineNames;
static std::mutex gLock;
static unsigned gBarriers, gDraws, gPasses;

// --- tracing by swizzling the classes of the objects RenderBox gets
static void swizzle(Class c, SEL s, IMP imp, IMP *orig)
{
    Method m = class_getInstanceMethod(c, s);
    if (!m) { fprintf(stderr, "no %s on %s\n", sel_getName(s), class_getName(c)); return; }
    *orig = method_setImplementation(m, imp);
}
static IMP oVBytes, oVBuf, oDSS, oNewDSS, oFBytes, oFBuf, oVBufOff, oCull, oScissor, oStencilRef;
static IMP oRenderEnc, oSetPipe, oDraw, oDrawIdx, oDrawInst, oBarrierScope, oBarrierRes, oEnd, oNewPipe, oNewPipeAsync, oNewPipeRefl,
    oSetFragTex, oCommit, oTexBarrier, oDrawIdxInst, oCompute, oBlit, oNewPipeAsyncRefl;
static NSString *funcName(id f) { return f ? [f name] : @"-"; }
static id tRenderEnc(id self, SEL s, MTLRenderPassDescriptor *d)
{
    id t = d.colorAttachments[0].texture;
    fprintf(stderr, "PASS %u: color0 %lux%lu fmt %lu load %lu store %lu iosurface %d | color1 %s | depth %s stencil %s\n", ++gPasses,
            (unsigned long)[t width], (unsigned long)[t height], (unsigned long)[(id<MTLTexture>)t pixelFormat], (unsigned long)d.colorAttachments[0].loadAction,
            (unsigned long)d.colorAttachments[0].storeAction, [t iosurface] != nil, d.colorAttachments[1].texture ? "yes" : "no",
            d.depthAttachment.texture ? "yes" : "no", d.stencilAttachment.texture ? "yes" : "no");
    {
        MTLRenderPassDepthAttachmentDescriptor *da = d.depthAttachment;
        MTLRenderPassStencilAttachmentDescriptor *sa = d.stencilAttachment;
        fprintf(stderr, "   depth %lux%lu fmt %lu storage %lu load %lu store %lu clear %g | stencil fmt %lu load %lu store %lu clear %u same %d\n",
                (unsigned long)[da.texture width], (unsigned long)[da.texture height], (unsigned long)[(id<MTLTexture>)da.texture pixelFormat],
                (unsigned long)[da.texture storageMode], (unsigned long)da.loadAction, (unsigned long)da.storeAction, da.clearDepth,
                (unsigned long)[(id<MTLTexture>)sa.texture pixelFormat], (unsigned long)sa.loadAction, (unsigned long)sa.storeAction, sa.clearStencil,
                da.texture == sa.texture);
        fprintf(stderr, "   color0 clear %g %g %g %g storage %lu\n", d.colorAttachments[0].clearColor.red, d.colorAttachments[0].clearColor.green,
                d.colorAttachments[0].clearColor.blue, d.colorAttachments[0].clearColor.alpha, (unsigned long)[t storageMode]);
    }
    for (int i = 1; i < 4; i++) {
        id ti = d.colorAttachments[i].texture;
        if (ti) fprintf(stderr, "   color%d %lux%lu fmt %lu load %lu store %lu storage %lu\n", i, (unsigned long)[ti width], (unsigned long)[ti height],
                        (unsigned long)[(id<MTLTexture>)ti pixelFormat], (unsigned long)d.colorAttachments[i].loadAction, (unsigned long)d.colorAttachments[i].storeAction, (unsigned long)[ti storageMode]);
    }
    return ((id (*)(id, SEL, id))oRenderEnc)(self, s, d);
}
static void tSetPipe(id self, SEL s, id p)
{
    std::string n;
    { std::lock_guard<std::mutex> l(gLock); n = gPipelineNames[(__bridge void *)p]; }
    fprintf(stderr, "  pipeline %s\n", n.c_str());
    ((void (*)(id, SEL, id))oSetPipe)(self, s, p);
}
static void tDraw(id self, SEL s, NSUInteger prim, NSUInteger start, NSUInteger count)
{
    fprintf(stderr, "  draw prim %lu start %lu count %lu\n", (unsigned long)prim, (unsigned long)start, (unsigned long)count); gDraws++;
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger))oDraw)(self, s, prim, start, count);
}
static void tDrawInst(id self, SEL s, NSUInteger prim, NSUInteger start, NSUInteger count, NSUInteger inst)
{
    fprintf(stderr, "  draw prim %lu start %lu count %lu instances %lu\n", (unsigned long)prim, (unsigned long)start, (unsigned long)count, (unsigned long)inst); gDraws++;
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, NSUInteger))oDrawInst)(self, s, prim, start, count, inst);
}
static void tDrawIdx(id self, SEL s, NSUInteger prim, NSUInteger count, NSUInteger type, id buf, NSUInteger off)
{
    fprintf(stderr, "  drawIndexed prim %lu count %lu\n", (unsigned long)prim, (unsigned long)count); gDraws++;
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger))oDrawIdx)(self, s, prim, count, type, buf, off);
}
static void tDrawIdxInst(id self, SEL s, NSUInteger prim, NSUInteger count, NSUInteger type, id buf, NSUInteger off, NSUInteger inst)
{
    fprintf(stderr, "  drawIndexed prim %lu count %lu instances %lu\n", (unsigned long)prim, (unsigned long)count, (unsigned long)inst); gDraws++;
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger))oDrawIdxInst)(self, s, prim, count, type, buf, off, inst);
}
static void tBarrierScope(id self, SEL s, NSUInteger scope, NSUInteger after, NSUInteger before)
{
    fprintf(stderr, "  BARRIER scope 0x%lx after 0x%lx before 0x%lx\n", (unsigned long)scope, (unsigned long)after, (unsigned long)before); gBarriers++;
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger))oBarrierScope)(self, s, scope, after, before);
}
static void tBarrierRes(id self, SEL s, const id *r, NSUInteger n, NSUInteger after, NSUInteger before)
{
    fprintf(stderr, "  BARRIER resources %lu after 0x%lx before 0x%lx\n", (unsigned long)n, (unsigned long)after, (unsigned long)before); gBarriers++;
    ((void (*)(id, SEL, const id *, NSUInteger, NSUInteger, NSUInteger))oBarrierRes)(self, s, r, n, after, before);
}
static void tTexBarrier(id self, SEL s)
{
    fprintf(stderr, "  TEXTUREBARRIER\n"); gBarriers++;
    ((void (*)(id, SEL))oTexBarrier)(self, s);
}
static void tSetFragTex(id self, SEL s, id t, NSUInteger i)
{
    fprintf(stderr, "  fragTex[%lu] %lux%lu fmt %lu %s\n", (unsigned long)i, (unsigned long)[t width], (unsigned long)[t height], (unsigned long)[(id<MTLTexture>)t pixelFormat],
            [t iosurface] ? "iosurface" : "");
    ((void (*)(id, SEL, id, NSUInteger))oSetFragTex)(self, s, t, i);
}
static void tEnd(id self, SEL s)
{
    fprintf(stderr, "  end\n");
    ((void (*)(id, SEL))oEnd)(self, s);
}
static void note(id p, MTLRenderPipelineDescriptor *d)
{
    if (!p) return;
    NSString *n = [NSString stringWithFormat:@"%@/%@ c0fmt %lu blend %d c1fmt %lu", funcName(d.vertexFunction), funcName(d.fragmentFunction),
                   (unsigned long)d.colorAttachments[0].pixelFormat, d.colorAttachments[0].blendingEnabled, (unsigned long)d.colorAttachments[1].pixelFormat];
    std::lock_guard<std::mutex> l(gLock);
    gPipelineNames[(__bridge void *)p] = n.UTF8String;
}
static id tNewPipe(id self, SEL s, MTLRenderPipelineDescriptor *d, NSError **e)
{
    id p = ((id (*)(id, SEL, id, NSError **))oNewPipe)(self, s, d, e);
    note(p, d);
    if (!p) fprintf(stderr, "PIPELINE FAILED %s: %s\n", funcName(d.fragmentFunction).UTF8String, e && *e ? (*e).description.UTF8String : "?");
    return p;
}
static id tNewPipeRefl(id self, SEL s, MTLRenderPipelineDescriptor *d, NSUInteger o, id *r, NSError **e)
{
    id p = ((id (*)(id, SEL, id, NSUInteger, id *, NSError **))oNewPipeRefl)(self, s, d, o, r, e);
    note(p, d);
    if (!p) fprintf(stderr, "PIPELINE FAILED %s: %s\n", funcName(d.fragmentFunction).UTF8String, e && *e ? (*e).description.UTF8String : "?");
    return p;
}
static void tNewPipeAsync(id self, SEL s, MTLRenderPipelineDescriptor *d, void (^h)(id, NSError *))
{
    MTLRenderPipelineDescriptor *k = [d copy];
    ((void (*)(id, SEL, id, id))oNewPipeAsync)(self, s, d, ^(id p, NSError *e) {
        note(p, k);
        if (!p) fprintf(stderr, "PIPELINE FAILED async %s: %s\n", funcName(k.fragmentFunction).UTF8String, e.description.UTF8String);
        h(p, e);
    });
}
static void tNewPipeAsyncRefl(id self, SEL s, MTLRenderPipelineDescriptor *d, NSUInteger o, void (^h)(id, id, NSError *))
{
    MTLRenderPipelineDescriptor *k = [d copy];
    ((void (*)(id, SEL, id, NSUInteger, id))oNewPipeAsyncRefl)(self, s, d, o, ^(id p, id r, NSError *e) {
        note(p, k);
        if (!p) fprintf(stderr, "PIPELINE FAILED async %s: %s\n", funcName(k.fragmentFunction).UTF8String, e.description.UTF8String);
        h(p, r, e);
    });
}
static void tCommit(id self, SEL s)
{
    fprintf(stderr, "COMMIT\n");
    ((void (*)(id, SEL))oCommit)(self, s);
}
static id tCompute(id self, SEL s)
{
    fprintf(stderr, "COMPUTE ENCODER\n");
    return ((id (*)(id, SEL))oCompute)(self, s);
}
static id tBlit(id self, SEL s)
{
    fprintf(stderr, "BLIT ENCODER\n");
    return ((id (*)(id, SEL))oBlit)(self, s);
}


static std::map<void *, std::string> gDSSNames;
static void dumpFloats(const char *what, const void *p, size_t n, NSUInteger i)
{
    fprintf(stderr, "    %s[%lu] %zu bytes:", what, (unsigned long)i, n);
    const float *f = (const float *)p;
    for (size_t k = 0; k < n / 4 && k < 24; k++) fprintf(stderr, " %.4g", f[k]);
    fprintf(stderr, "\n");
}
static void tVBytes(id self, SEL s, const void *p, NSUInteger n, NSUInteger i)
{
    dumpFloats("vbytes", p, n, i);
    ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))oVBytes)(self, s, p, n, i);
}
static void tFBytes(id self, SEL s, const void *p, NSUInteger n, NSUInteger i)
{
    dumpFloats("fbytes", p, n, i);
    ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))oFBytes)(self, s, p, n, i);
}
static void tVBuf(id self, SEL s, id b, NSUInteger o, NSUInteger i)
{
    if (b && ((id<MTLBuffer>)b).contents) dumpFloats("vbuf", (const char *)((id<MTLBuffer>)b).contents + o, 96, i);
    else fprintf(stderr, "    vbuf[%lu] %p +%lu\n", (unsigned long)i, (__bridge void *)b, (unsigned long)o);
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))oVBuf)(self, s, b, o, i);
}
static void tFBuf(id self, SEL s, id b, NSUInteger o, NSUInteger i)
{
    if (b && ((id<MTLBuffer>)b).contents) dumpFloats("fbuf", (const char *)((id<MTLBuffer>)b).contents + o, 64, i);
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))oFBuf)(self, s, b, o, i);
}
static void tVBufOff(id self, SEL s, NSUInteger o, NSUInteger i)
{
    fprintf(stderr, "    vbufoffset[%lu] %lu\n", (unsigned long)i, (unsigned long)o);
    ((void (*)(id, SEL, NSUInteger, NSUInteger))oVBufOff)(self, s, o, i);
}
static void tDSS(id self, SEL s, id d)
{
    std::string n;
    { std::lock_guard<std::mutex> l(gLock); n = gDSSNames[(__bridge void *)d]; }
    fprintf(stderr, "  depthstencil %s\n", n.c_str());
    ((void (*)(id, SEL, id))oDSS)(self, s, d);
}
static void tStencilRef(id self, SEL s, uint32_t r)
{
    fprintf(stderr, "  stencilref %u\n", r);
    ((void (*)(id, SEL, uint32_t))oStencilRef)(self, s, r);
}
static id tNewDSS(id self, SEL s, MTLDepthStencilDescriptor *d)
{
    id r = ((id (*)(id, SEL, id))oNewDSS)(self, s, d);
    NSString *n = [NSString stringWithFormat:@"depth cmp %lu write %d | stencil front cmp %lu pass %lu fail %lu dfail %lu rmask %x wmask %x",
                   (unsigned long)d.depthCompareFunction, d.depthWriteEnabled, (unsigned long)d.frontFaceStencil.stencilCompareFunction,
                   (unsigned long)d.frontFaceStencil.depthStencilPassOperation, (unsigned long)d.frontFaceStencil.stencilFailureOperation,
                   (unsigned long)d.frontFaceStencil.depthFailureOperation, d.frontFaceStencil.readMask, d.frontFaceStencil.writeMask];
    std::lock_guard<std::mutex> l(gLock);
    gDSSNames[(__bridge void *)r] = n.UTF8String;
    return r;
}
static void trace(id device, id queue);
static void trace(id device, id queue)
{
    id cb = [queue commandBuffer];
    id enc = [cb renderCommandEncoderWithDescriptor:[MTLRenderPassDescriptor renderPassDescriptor]];
    (void)enc;
    Class dc = [device class], cc = [cb class];
    // a real render encoder's class: make one with a target
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> t = [device newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    id e = [cb renderCommandEncoderWithDescriptor:rp];
    Class ec = [e class];
    [e endEncoding];
    fprintf(stderr, "classes: device %s cb %s render encoder %s\n", class_getName(dc), class_getName(cc), class_getName(ec));
    swizzle(cc, @selector(renderCommandEncoderWithDescriptor:), (IMP)tRenderEnc, &oRenderEnc);
    swizzle(cc, @selector(commit), (IMP)tCommit, &oCommit);
    swizzle(cc, @selector(computeCommandEncoder), (IMP)tCompute, &oCompute);
    swizzle(cc, @selector(blitCommandEncoder), (IMP)tBlit, &oBlit);
    swizzle(ec, @selector(setRenderPipelineState:), (IMP)tSetPipe, &oSetPipe);
    swizzle(ec, @selector(drawPrimitives:vertexStart:vertexCount:), (IMP)tDraw, &oDraw);
    swizzle(ec, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:), (IMP)tDrawInst, &oDrawInst);
    swizzle(ec, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:), (IMP)tDrawIdx, &oDrawIdx);
    swizzle(ec, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:), (IMP)tDrawIdxInst, &oDrawIdxInst);
    swizzle(ec, @selector(memoryBarrierWithScope:afterStages:beforeStages:), (IMP)tBarrierScope, &oBarrierScope);
    swizzle(ec, @selector(memoryBarrierWithResources:count:afterStages:beforeStages:), (IMP)tBarrierRes, &oBarrierRes);
    if (class_getInstanceMethod(ec, sel_registerName("textureBarrier")))
        swizzle(ec, sel_registerName("textureBarrier"), (IMP)tTexBarrier, &oTexBarrier);
    swizzle(ec, @selector(setFragmentTexture:atIndex:), (IMP)tSetFragTex, &oSetFragTex);
    swizzle(ec, @selector(endEncoding), (IMP)tEnd, &oEnd);
    swizzle(ec, @selector(setVertexBytes:length:atIndex:), (IMP)tVBytes, &oVBytes);
    swizzle(ec, @selector(setFragmentBytes:length:atIndex:), (IMP)tFBytes, &oFBytes);
    swizzle(ec, @selector(setVertexBuffer:offset:atIndex:), (IMP)tVBuf, &oVBuf);
    swizzle(ec, @selector(setFragmentBuffer:offset:atIndex:), (IMP)tFBuf, &oFBuf);
    swizzle(ec, @selector(setVertexBufferOffset:atIndex:), (IMP)tVBufOff, &oVBufOff);
    swizzle(ec, @selector(setDepthStencilState:), (IMP)tDSS, &oDSS);
    swizzle(ec, @selector(setStencilReferenceValue:), (IMP)tStencilRef, &oStencilRef);
    swizzle(dc, @selector(newDepthStencilStateWithDescriptor:), (IMP)tNewDSS, &oNewDSS);
    swizzle(dc, @selector(newRenderPipelineStateWithDescriptor:error:), (IMP)tNewPipe, &oNewPipe);
    swizzle(dc, @selector(newRenderPipelineStateWithDescriptor:options:reflection:error:), (IMP)tNewPipeRefl, &oNewPipeRefl);
    swizzle(dc, @selector(newRenderPipelineStateWithDescriptor:completionHandler:), (IMP)tNewPipeAsync, &oNewPipeAsync);
    swizzle(dc, @selector(newRenderPipelineStateWithDescriptor:options:completionHandler:), (IMP)tNewPipeAsyncRefl, &oNewPipeAsyncRefl);
}

static void writePNG(IOSurfaceRef surface, const char *path)
{
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
    size_t w = IOSurfaceGetWidth(surface), h = IOSurfaceGetHeight(surface), row = IOSurfaceGetBytesPerRow(surface);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(IOSurfaceGetBaseAddress(surface), w, h, 8, row, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(path)], CFSTR("public.png"), 1, nullptr);
    CGImageDestinationAddImage(dst, img, nullptr);
    CGImageDestinationFinalize(dst);
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) return 2;
        const bool system = !strcmp(argv[1], "system");
        const size_t W = atoi(argv[2]), H = atoi(argv[3]);
        id<MTLDevice> device = nil;
        if (system) {
            device = MTLCreateSystemDefaultDevice();
        } else {
            void *plugin = dlopen(argv[4], RTLD_NOW | RTLD_LOCAL);
            if (!plugin) { printf("FAIL: dlopen %s\n", dlerror()); return 1; }
            auto open = (Open)dlsym(plugin, "native_bridge_metal_open");
            setenv("TU_DARWIN_SERVICE", "AGXAccelerator", 1);
            setenv("METAL2VULKAN_BLOCK_MIB", "16", 0);
            setenv("NMB_LOG_STDERR", "1", 1);
            device = open(argv[5], argv[6], 0x4e41, missing);
        }
        if (!device) { printf("FAIL: no device\n"); return 1; }
        fprintf(stderr, "device: %s\n", device.name.UTF8String);
        for (int f = 1001; f <= 1010; f++) fprintf(stderr, "family Apple%d: %d\n", f - 1000, [device supportsFamily:(MTLGPUFamily)f]);
        fprintf(stderr, "family Mac2: %d Common3: %d Metal3: %d\n", [device supportsFamily:MTLGPUFamilyMac2], [device supportsFamily:MTLGPUFamilyCommon3], [device supportsFamily:MTLGPUFamilyMetal3]);
        fprintf(stderr, "rasterOrderGroups %d readWrite tier %lu argbuf tier %lu\n", device.rasterOrderGroupsSupported, (unsigned long)device.readWriteTextureSupport, (unsigned long)device.argumentBuffersSupport);
        if (getenv("RB_ONLY_FAMILIES")) return 0;
        if (!dlopen("/System/Library/PrivateFrameworks/RenderBox.framework/RenderBox", RTLD_NOW)) { printf("no RB\n"); return 1; }
        id<MTLCommandQueue> q = [device newCommandQueue];
        trace(device, q);

        NSDictionary *props = @{(id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H), (id)kIOSurfaceBytesPerElement: @4,
                                (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA')};
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:td iosurface:surface plane:0];
        if (!target) { printf("FAIL: target\n"); return 1; }

        if (getenv("CA_MODE")) {
            CALayer *root = [CALayer layer];
            root.frame = CGRectMake(0, 0, W, H);
            root.backgroundColor = CGColorCreateGenericRGB(0.5, 0.5, 0.5, 1);
            root.geometryFlipped = YES;
            CAShapeLayer *circle = [CAShapeLayer layer];
            circle.frame = root.bounds;
            circle.path = CGPathCreateWithEllipseInRect(CGRectMake(50, 50, 200, 200), nullptr);
            circle.fillColor = CGColorCreateGenericRGB(0.2, 0.2, 0.2, 1);
            [root addSublayer:circle];
            CAShapeLayer *cap = [CAShapeLayer layer];
            cap.frame = root.bounds;
            cap.path = CGPathCreateWithRoundedRect(CGRectMake(300, 100, 300, 80), 40, 40, nullptr);
            cap.fillColor = CGColorCreateGenericRGB(1, 1, 1, 1);
            [root addSublayer:cap];
            CALayer *rr = [CALayer layer];
            rr.frame = CGRectMake(300, 250, 250, 150);
            rr.cornerRadius = 75;
            rr.backgroundColor = CGColorCreateGenericRGB(0.1, 0.3, 0.8, 1);
            [root addSublayer:rr];
            if (getenv("CA_SDF")) {
                circle.hidden = YES; cap.hidden = YES; rr.hidden = YES;
                CALayer *sdf = [objc_getClass("CASDFLayer") layer];
                sdf.frame = root.bounds;
                id effect = [objc_getClass("CASDFFillEffect") new];
                [effect setColor:CGColorCreateGenericRGB(0.9, 0.2, 0.2, 1)];
                [sdf setValue:effect forKey:@"effect"];
                CALayer *e1 = [objc_getClass("CASDFElementLayer") layer];
                e1.frame = CGRectMake(50, 50, 200, 200);
                e1.cornerRadius = 100;
                [sdf addSublayer:e1];
                CALayer *e2 = [objc_getClass("CASDFElementLayer") layer];
                e2.frame = CGRectMake(300, 100, 300, 80);
                e2.cornerRadius = 40;
                [sdf addSublayer:e2];
                CALayer *e3 = [objc_getClass("CASDFElementLayer") layer];
                e3.frame = CGRectMake(300, 250, 250, 150);
                e3.cornerRadius = 30;
                e3.cornerCurve = kCACornerCurveContinuous;
                [sdf addSublayer:e3];
                [root addSublayer:sdf];
            }
            if (getenv("CA_BACKDROP")) {
                CALayer *bd = [objc_getClass("CABackdropLayer") layer];
                bd.frame = CGRectMake(100, 80, 400, 300);
                id blur = [objc_getClass("CAFilter") performSelector:sel_registerName("filterWithType:") withObject:@"gaussianBlur"];
                [blur setValue:@20 forKey:@"inputRadius"];
                bd.filters = @[blur];
                [root addSublayer:bd];
                CALayer *tint = [CALayer layer];
                tint.frame = CGRectMake(100, 80, 400, 300);
                tint.backgroundColor = CGColorCreateGenericRGB(1, 1, 1, 0.3);
                [root addSublayer:tint];
            }
            if (getenv("CA_GLASS")) {
                CALayer *sdf = [objc_getClass("CASDFLayer") layer];
                sdf.frame = root.bounds;
                id effect = [objc_getClass("CASDFGlassDisplacementEffect") new];
                [sdf setValue:effect forKey:@"effect"];
                CALayer *e1 = [objc_getClass("CASDFElementLayer") layer];
                e1.frame = CGRectMake(450, 60, 200, 200);
                e1.cornerRadius = 100;
                [sdf addSublayer:e1];
                CALayer *e2 = [objc_getClass("CASDFElementLayer") layer];
                e2.frame = CGRectMake(100, 330, 400, 80);
                e2.cornerRadius = 40;
                [sdf addSublayer:e2];
                [root addSublayer:sdf];
            }
            if (getenv("CA_STROKE")) {
                CAShapeLayer *st = [CAShapeLayer layer];
                st.frame = root.bounds;
                st.path = CGPathCreateWithEllipseInRect(CGRectMake(560, 300, 120, 120), nullptr);
                st.fillColor = nil;
                st.strokeColor = CGColorCreateGenericRGB(1, 0, 0, 1);
                st.lineWidth = 6;
                [root addSublayer:st];
            }
            [CATransaction commit];
            [CATransaction flush];
            CARenderer *r = [CARenderer rendererWithMTLTexture:target options:@{kCARendererMetalCommandQueue: q}];
            r.layer = root;
            r.bounds = CGRectMake(0, 0, W, H);
            for (int frame = 0; frame < 2; frame++) {
                fprintf(stderr, "=== CA render %d\n", frame);
                [CATransaction flush];
                [r beginFrameAtTime:CACurrentMediaTime() + frame timeStamp:nullptr];
                fprintf(stderr, "update bounds %g %g\n", r.updateBounds.size.width, r.updateBounds.size.height);
                [r addUpdateRect:CGRectMake(0, 0, W, H)];
                [r render];
                [r endFrame];
            }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [cb commit];
            [cb waitUntilCompleted];
            fprintf(stderr, "passes %u draws %u barriers %u\n", gPasses, gDraws, gBarriers);
            if (system) writePNG(surface, argv[4]);
            return 0;
        }
        id rbDevice = [[objc_getClass("RBDevice") alloc] initWithDevice:device];
        id drawable = [[objc_getClass("RBDrawable") alloc] initWithDevice:rbDevice];
        [drawable setTexture:target];
        ((void (*)(id, SEL, CGSize))objc_msgSend)(drawable, sel_registerName("setSize:"), CGSizeMake(W, H));
        [drawable setPixelFormat:MTLPixelFormatBGRA8Unorm];
        ((void (*)(id, SEL, RBColor4))objc_msgSend)(drawable, sel_registerName("setClearColor:"), (RBColor4){0.5f, 0.5f, 0.5f, 1});
        [drawable setInitialState:argc > 7 ? atoi(argv[7]) : 0];
        __block bool done = false;
        [drawable setErrorHandler:^(NSError *e) { fprintf(stderr, "RB error: %s\n", e.description.UTF8String); }];

        id dl = [objc_getClass("RBDisplayList") new];
        id circle = [objc_getClass("RBShape") new];
        [circle setCircleAtPoint:CGPointMake(150, 150) radius:100];
        id fill = [objc_getClass("RBFill") new];
        ((void (*)(id, SEL, RBColor4))objc_msgSend)(fill, sel_registerName("setColor:"), (RBColor4){0.2f, 0.2f, 0.2f, 1});
        [dl drawShape:circle fill:fill alpha:1 blendMode:0];
        id capsule = [objc_getClass("RBShape") new];
        [capsule setRoundedRect:CGRectMake(300, 100, 300, 80) cornerRadius:40 cornerStyle:0];
        id white = [objc_getClass("RBFill") new];
        ((void (*)(id, SEL, RBColor4))objc_msgSend)(white, sel_registerName("setColor:"), (RBColor4){1, 1, 1, 0.9f});
        [dl drawShape:capsule fill:white alpha:1 blendMode:0];
        id rr = [objc_getClass("RBShape") new];
        [rr setRoundedRect:CGRectMake(300, 250, 250, 150) cornerRadius:30 cornerStyle:1];
        [dl drawShape:rr fill:fill alpha:0.6f blendMode:0];
        if (getenv("RB_PATHS")) {
            CGPathRef e = CGPathCreateWithEllipseInRect(CGRectMake(50, 260, 160, 160), nullptr);
            id ps = [objc_getClass("RBShape") new];
            [ps setPath:e];
            fprintf(stderr, "path ellipse shape added\n");
            [dl drawShape:ps fill:fill alpha:1 blendMode:0];
            CGPathRef c = CGPathCreateWithRoundedRect(CGRectMake(250, 20, 400, 60), 30, 30, nullptr);
            id pc = [objc_getClass("RBShape") new];
            [pc setPath:c];
            [dl drawShape:pc fill:fill alpha:1 blendMode:0];
            id st = [objc_getClass("RBShape") new];
            [st setStrokedCircleAtPoint:CGPointMake(600, 350) radius:60 lineWidth:6];
            [dl drawShape:st fill:fill alpha:1 blendMode:0];
        }
        if (getenv("RB_CLIP")) {
            id c2 = [objc_getClass("RBShape") new];
            [c2 setCircleAtPoint:CGPointMake(150, 330) radius:90];
            id sub = [objc_getClass("RBDisplayList") new];
            [sub clipShape:c2 mode:0];
            id sq = [objc_getClass("RBShape") new];
            [sq setRoundedRect:CGRectMake(40, 220, 220, 220) cornerRadius:0 cornerStyle:0];
            [sub drawShape:sq fill:fill alpha:1 blendMode:0];
            id body = [objc_getClass("RBShape") new];
            [body setCircleAtPoint:CGPointMake(150, 430) radius:70];
            [sub drawShape:body fill:white alpha:1 blendMode:0];
            [dl drawDisplayList:sub];
        }
        if (getenv("RB_BLEND")) {
            int mode = atoi(getenv("RB_BLEND"));
            id g = [objc_getClass("RBShape") new];
            [g setRoundedRect:CGRectMake(320, 300, 300, 80) cornerRadius:40 cornerStyle:0];
            [dl drawShape:g fill:white alpha:0.5f blendMode:mode];
        }
        if (getenv("RB_DIST")) {
            id sub = [objc_getClass("RBDisplayList") new];
            id c3 = [objc_getClass("RBShape") new];
            [c3 setCircleAtPoint:CGPointMake(150, 330) radius:90];
            [sub drawShape:c3 fill:fill alpha:1 blendMode:0];
            id cap = [objc_getClass("RBShape") new];
            [cap setRoundedRect:CGRectMake(320, 300, 300, 80) cornerRadius:40 cornerStyle:0];
            [sub drawShape:cap fill:fill alpha:1 blendMode:0];
            int kind = atoi(getenv("RB_DIST"));
            if (kind == 1) [sub addDistanceFilterWithMaxDistance:20 scale:1 flags:0];
            if (kind == 2) [sub addBlurFilterWithRadius:10 flags:0];
            if (kind == 3) [sub addShadowStyleWithRadius:10 midpoint:0.5f offset:CGSizeMake(0, 4) color:(RBColor4){0, 0, 0, 0.5f} colorSpace:0 blendMode:0 flags:0];
            [dl drawDisplayList:sub];
        }
        for (int frame = 0; frame < 2; frame++) {
            fprintf(stderr, "=== render %d\n", frame);
            [drawable renderDisplayList:dl flags:0];
            [drawable finish];
        }
        (void)done;
        fprintf(stderr, "passes %u draws %u barriers %u\n", gPasses, gDraws, gBarriers);
        if (system) writePNG(surface, argv[4]);
    }
    return 0;
}
