// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeSoftMetal: the Metal plugin of NativeSoftGpu's accelerator (package/NativeSoftGpu). It makes a Metal device with no GPU behind it,
// so that macOS 27's WindowServer gets past its wait for a Metal device and draws the desktop on the CPU.
//
// What WindowServer does with the device (macOS 26.6 SkyLight and QuartzCore read on the build Mac, 2026-11-11; 27.0.1 not checked):
//   - SkyLight waits up to 30 s for MTLCopyAllDevices to be non-empty, and aborts if it stays empty.
//   - The display's accelerator is the first device's -acceleratorPort. With 0, SkyLight's per-accelerator lookup returns its software
//     compositor (CompositorSW), not CompositorMetal.
//   - Core Animation's WindowServer display asks the first device for a command queue; with none it renders the update with its software
//     renderer (CA::OGL::SWContext) and presents it through IOMobileFramebuffer, which is what NativeGopDisplay copies to the panel.
// So this device answers acceleratorPort 0 and nil for every command queue, buffer, texture and library. That is Metal's ordinary
// failure answer, and Metal clients check for it.
//
// Every selector is answered. Metal's own `_MTLDevice`, the base class a plugin's device must have, implements most of the device
// protocols but not all of them (macOS 26.6: 33 of MTLDevice's 139 and 48 of MTLDeviceSPI's 507 are the driver's own, among them
// maxBufferLength, newCommandQueue and the limits SkyLight reads). An unanswered one is an unrecognized selector, whose exception
// aborts the process with nothing on stderr, which is what the 2026-11-11 run saw: the last line from here was "stubbed
// maxBufferLength", then WindowServer took SIGABRT. So every selector of both protocols now gets an answer (zero, or the value in
// kValues), and anything still unknown is swallowed by -forwardInvocation: instead of throwing. Each one is named once on stderr.
//
// Where: IOGPU's +[IOGPUMetalDevice registerAcceleratorService:] loads this bundle from /Library/GPUBundles for an accelerator whose
// MetalPluginName is NativeSoftMetal. It takes NSPrincipalClass NativeSoftMetalDevice (an IOGPUMetalDevice subclass) and registers what
// -initWithAcceleratorPort: returns. The bundle's CDHash must be in the boot's trust cache (tooling/k27/stage-soft-metal.py).
//
// Kernel boot arguments, read through kern.bootargs, so a test needs no restaging:
//   soft-metal=0          no device in any process (as if the plugin were missing)
//   soft-metal-all=1      a device in every process that asks (default: WindowServer only; the others then see no Metal device at all,
//                         which their frameworks handle as on a Mac without a GPU)
//   soft-metal-stubs=0    answer only the selectors in kValues and whatever _MTLDevice implements; everything else goes to
//                         -forwardInvocation: (so -respondsToSelector: stays NO for it, and a client that asks first takes its other path)
//   soft-metal-family=N   -supportsFamily:/-supportsFeatureSet: answer YES for arguments up to N (default 0: no family). A client that
//                         insists on a GPU family can be given one without a rebuild
//   soft-metal-report=0   do not install the abort reporter below
//   soft-metal-fb8=0      leave Core Animation's framebuffer format alone (see "The framebuffer format" below)
// Reports go to stderr ("NativeSoftMetal: <process>: ..."); kernel boot argument stderr-log=1 puts them in the console log, one line per
// write and at most 160 bytes a line, so the lines here are short.
//
// The abort reporter: this bundle is loaded inside WindowServer, so it can say why WindowServer dies, which the console log otherwise
// does not show (SkyLight's own asserts go to os_log, and AMFI denies the core dump). It installs an uncaught-exception handler, a C++
// terminate handler and handlers for the fatal signals; each prints the reason and the stack as "<image>+<offset>", which names the
// framework that aborted even without the 27.0.1 symbols. Before the stack it prints every loaded image's crash-reporter message
// (__DATA,__crash_info): SkyLight puts its abort text there before abort(), so the reason is in the console log, not only in os_log.
//
// The framebuffer format (2026-11-11, second run): WindowServer got past the device and died in SkyLight's
// WSCompositeDestinationCreateWithIOSurface, which aborts with "Unimplemented pixel format of %i" for any surface SkyLight's software
// compositor cannot write. On 26.6 it takes 'BGRA', 'RGBA', 32ARGB, 'RGfA', 'R10k', 'b64a' and 'l10r', not the half-float 'RGhA' or the
// 10-bit 'b3a8' a display can get from Core Animation (CA::WindowServer::IOMFBDisplay::framebuffer_pixel_format). That function gives
// 'RGhA' when debug option CA_FORCE_FP16_FRAMEBUFFER is on (26.6: option 124), 'w40a' for CA_FORCE_CIF10_FRAMEBUFFER (125), or for an
// EDR display; else 'BGRA'. So, in WindowServer, unless soft-metal-fb8=0: those two options are found by name
// (CAGetDebugOptionEnvVariableName, so 27.0.1's numbering is not assumed), reported, and cleared whenever they are on during the first
// minute; and Core Animation's CAEnableDeepFramebuffer preference is set false for the process. Every debug option that is on is listed
// once (the survey), so a run shows what else Core Animation was told.
//
// The IOGPU registration answers (_setAcceleratorService:, _setDeviceWrapper:, _deviceWrapper, the stub survey) follow the VM-era
// Metal plugin (provenance: package/NativeSoftMetal/UPSTREAM).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <atomic>
#include <cxxabi.h>
#include <dlfcn.h>
#include <exception>
#include <initializer_list>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

@interface IOGPUMetalDevice : NSObject
@end

@interface NativeSoftMetalDevice : IOGPUMetalDevice
- (id)initWithAcceleratorPort:(io_service_t)port;
@end

static const char *const kDeviceName = "NativeSoftMetal (software, no GPU)";

// ---- boot arguments and short stderr lines ----------------------------------------------------------------------------------------
static bool bootArg(const char *name, unsigned *value)
{
    static char args[1024];
    static bool read;
    if (!read) {
        size_t size = sizeof(args) - 1;
        if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0)) args[0] = 0;
        read = true;
    }
    size_t length = strlen(name);
    for (char *at = args; (at = strstr(at, name)); at += length) {
        if ((at == args || at[-1] == ' ') && at[length] == '=') {
            *value = (unsigned)strtoul(at + length + 1, nullptr, 0);
            return true;
        }
    }
    return false;
}

static unsigned bootArgValue(const char *name, unsigned fallback)
{
    unsigned value = fallback;
    bootArg(name, &value);
    return value;
}

// One line on fd 2, newline-terminated even when cut at the console's 160 bytes.
static void writeLine(char *line, size_t size, int length)
{
    if (length <= 0) return;
    size_t n = (size_t)length < size ? (size_t)length : size - 1;
    if (n && line[n - 1] != '\n') {
        if (n == size - 1) n--;
        line[n++] = '\n';
    }
    (void)!write(2, line, n);
}

// One write(2) per line: the kernel's stderr-log logs a write at a time.
__attribute__((format(printf, 1, 2))) static void report(const char *format, ...)
{
    char line[160], text[128];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    writeLine(line, sizeof line, snprintf(line, sizeof line, "NativeSoftMetal: %s: %s\n", getprogname(), text));
}

// In a signal handler: no allocation, no printf of the va kind.
static void reportRaw(const char *text)
{
    char line[160];
    size_t at = 0;
    for (const char *from : {"NativeSoftMetal: ", getprogname(), ": ", text, "\n"})
        for (const char *c = from; *c && at < sizeof line - 1; c++) line[at++] = *c;
    (void)!write(2, line, at);
}

// ---- the abort reporter -----------------------------------------------------------------------------------------------------------
static void reportFrames(void *const *frames, int count)
{
    for (int i = 0; i < count && i < 24; i++) {
        Dl_info info = {};
        const char *image = "?";
        unsigned long offset = (unsigned long)frames[i];
        if (dladdr(frames[i], &info) && info.dli_fname) {
            const char *slash = strrchr(info.dli_fname, '/');
            image = slash ? slash + 1 : info.dli_fname;
            offset = (unsigned long)((const char *)frames[i] - (const char *)info.dli_fbase);
        }
        char line[160];
        writeLine(line, sizeof line, snprintf(line, sizeof line, "NativeSoftMetal: %s: #%d %.40s+0x%lx %.40s\n", getprogname(), i, image,
            offset, info.dli_sname ? info.dli_sname : ""));
    }
}

// The crash-reporter annotations of every loaded image (libsystem's crashreporter_annotations_t in __DATA,__crash_info: version, then
// the message and, at +32, message2). abort() paths that explain themselves (SkyLight's "Unimplemented pixel format ...", assert, libobjc)
// leave their text there. Printed in 72-byte pieces: the console takes one write of at most 160 bytes a line.
static void reportCrashMessages()
{
    for (uint32_t i = 0, count = _dyld_image_count(); i < count; i++) {
        const struct mach_header_64 *header = reinterpret_cast<const struct mach_header_64 *>(_dyld_get_image_header(i));
        if (!header) continue;
        for (const char *segment : {"__DATA", "__DATA_DIRTY"}) {
            unsigned long size = 0;
            const uint8_t *section = getsectiondata(header, segment, "__crash_info", &size);
            if (!section || size < 40) continue;
            for (unsigned offset : {8u, 32u}) {
                const char *message = *reinterpret_cast<const char *const *>(section + offset);
                if (!message || !*message) continue;
                const char *path = _dyld_get_image_name(i), *slash = path ? strrchr(path, '/') : nullptr;
                char line[160];
                size_t length = strnlen(message, 600);
                for (size_t at = 0; at < length; at += 72)
                    writeLine(line, sizeof line, snprintf(line, sizeof line, "NativeSoftMetal: %s: crash message (%.20s): %.72s\n",
                        getprogname(), slash ? slash + 1 : path ? path : "?", message + at));
            }
        }
    }
}

static void reportHere(const char *why)
{
    reportRaw(why);
    reportCrashMessages();
    void *frames[32];
    reportFrames(frames, backtrace(frames, 32));
}

static void onSignal(int number, siginfo_t *, void *)
{
    reportRaw(number == SIGABRT ? "SIGABRT" : number == SIGSEGV ? "SIGSEGV" : number == SIGBUS ? "SIGBUS" :
        number == SIGILL ? "SIGILL" : number == SIGTRAP ? "SIGTRAP" : "fatal signal");
    reportCrashMessages();
    void *frames[32];
    reportFrames(frames, backtrace(frames, 32));
    raise(number);   // SA_RESETHAND: the default action now, so the process dies as it would have
}

static void onException(NSException *exception)
{
    report("uncaught %.40s", exception.name.UTF8String ?: "?");
    const char *reason = exception.reason.UTF8String ?: "";
    for (size_t at = 0; at < strlen(reason); at += 100) report("reason: %.100s", reason + at);
    NSArray<NSNumber *> *addresses = exception.callStackReturnAddresses;
    void *frames[32];
    int count = 0;
    for (NSNumber *address in addresses) {
        if (count == 32) break;
        frames[count++] = (void *)(uintptr_t)address.unsignedLongLongValue;
    }
    reportFrames(frames, count);
}

static std::terminate_handler gNextTerminate;

static void onTerminate()
{
    if (std::type_info *type = abi::__cxa_current_exception_type()) {
        int status = 0;
        char *name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
        report("terminate: %.60s", name ? name : type->name());
        free(name);
        try {
            throw;
        } catch (NSException *exception) {
            onException(exception);
        } catch (const std::exception &error) {
            report("what: %.100s", error.what());
        } catch (...) {
            reportHere("terminate: unknown exception");
        }
    } else {
        reportHere("terminate without an exception");
    }
    if (gNextTerminate) gNextTerminate();
    abort();
}

static void installReporter()
{
    if (!bootArgValue("soft-metal-report", 1)) return;
    NSSetUncaughtExceptionHandler(onException);
    gNextTerminate = std::set_terminate(onTerminate);
    struct sigaction action = {};
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    action.__sigaction_u.__sa_sigaction = onSignal;
    sigemptyset(&action.sa_mask);
    for (int number : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE}) sigaction(number, &action, nullptr);
    report("abort reporter installed (soft-metal-report=0 to leave it out)");
}

// ---- Core Animation's framebuffer format ------------------------------------------------------------------------------------------
// QuartzCore's debug options, by the environment variable name each has (the numbering differs between releases).
using GetOption = bool (*)(unsigned);
using SetOption = void (*)(unsigned, bool);
using OptionName = const char *(*)(unsigned);
static const unsigned kMaxOption = 1024;   // the accessors answer 0 / NULL past their table

static bool isFramebufferForce(const char *name)
{
    return !strcmp(name, "CA_FORCE_FP16_FRAMEBUFFER") || !strcmp(name, "CA_FORCE_CIF10_FRAMEBUFFER");
}

// Lists each option that is on (once), and with CLEAR turns the framebuffer-forcing ones off again.
static void coreAnimationOptions(const char *when, bool clear)
{
    static GetOption get = reinterpret_cast<GetOption>(dlsym(RTLD_DEFAULT, "CAGetDebugOption"));
    static SetOption set = reinterpret_cast<SetOption>(dlsym(RTLD_DEFAULT, "CASetDebugOption"));
    static OptionName name = reinterpret_cast<OptionName>(dlsym(RTLD_DEFAULT, "CAGetDebugOptionEnvVariableName"));
    static bool reported[kMaxOption], missing;
    if (!get || !set || !name) {
        if (!missing) report("QuartzCore has no CAGetDebugOption / CASetDebugOption / CAGetDebugOptionEnvVariableName");
        missing = true;
        return;
    }
    for (unsigned id = 0; id < kMaxOption; id++) {
        const char *option = name(id);
        if (!option || !get(id)) continue;
        if (!reported[id]) {
            reported[id] = true;
            report("%s: CA option %u %.60s is on", when, id, option);
        }
        if (clear && isFramebufferForce(option)) {
            set(id, false);
            report("%s: CA option %u %.60s cleared (soft-metal-fb8=0 keeps it)", when, id, option);
        }
    }
}

// In WindowServer: 8-bit framebuffers, the only kind SkyLight's software compositor writes. The preference first (Core Animation reads
// it when it makes a display's framebuffers), then the debug options now and once a second for a minute, since a display's
// framebuffers are made after this bundle is loaded and something may set an option on the way.
static void framebufferPolicy()
{
    const bool clear = bootArgValue("soft-metal-fb8", 1);
    if (clear) {
        CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
        report("CAEnableDeepFramebuffer set false for this process (soft-metal-fb8=0 leaves it)");
    }
    coreAnimationOptions("load", clear);
    static dispatch_source_t timer;
    static std::atomic<unsigned> ticks;
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (!timer) return;
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(timer, ^{
        char when[16];
        snprintf(when, sizeof when, "t+%us", ticks.load() + 1);
        coreAnimationOptions(when, clear);
        if (++ticks >= 60) dispatch_source_cancel(timer);
    });
    dispatch_resume(timer);
}

// ---- the device's answers ---------------------------------------------------------------------------------------------------------
static uint64_t gRegistryID;
static id gAcceleratorService;
static __weak id gDeviceWrapper;
static unsigned gFamily;

// Each selector that is answered with a zero, or swallowed, is named once. The cap keeps a per-frame selector from filling the log.
static void noteOnce(const char *kind, SEL selector)
{
    static NSMutableSet *seen;
    static dispatch_once_t once;
    static std::atomic<unsigned> count;
    dispatch_once(&once, ^{ seen = [NSMutableSet set]; });
    if (count.load() >= 240) return;
    NSString *name = NSStringFromSelector(selector);
    @synchronized(seen) {
        if ([seen containsObject:name]) return;
        [seen addObject:name];
    }
    report("%s %.90s", kind, name.UTF8String);
    if (++count == 240) report("240 selectors named; no more");
}

// The stubs, by what the selector returns. Arguments are ignored; on arm64 the extra integer parameters are simply the registers the
// caller set, and a 16- or 24-byte structure comes back the way the real method's would.
struct PairValue { uint64_t a, b; };
struct TripleValue { uint64_t a, b, c; };
static void stubVoid(id, SEL selector) { noteOnce("void", selector); }
static uint64_t stubZero(id, SEL selector) { noteOnce("zero", selector); return 0; }
static float stubFloat(id, SEL selector) { noteOnce("0.0f", selector); return 0; }
static double stubDouble(id, SEL selector) { noteOnce("0.0", selector); return 0; }
static PairValue stubPair(id, SEL selector) { noteOnce("{0,0}", selector); return {0, 0}; }
static TripleValue stubTriple(id, SEL selector) { noteOnce("{0,0,0}", selector); return {0, 0, 0}; }

static IMP stubFor(const char *types)
{
    switch (types[0]) {
        case 'v': return (IMP)stubVoid;
        case 'f': return (IMP)stubFloat;
        case 'd': return (IMP)stubDouble;
        case '{': {   // only the plain integer structures Metal's device protocols return ({QQ}, {QQQ}: MTLSize, size and align)
            const char *equals = strchr(types, '=');
            if (!equals) return nullptr;
            size_t fields = 0;
            for (const char *at = equals + 1; *at && *at != '}'; at++, fields++)
                if (*at != 'Q' && *at != 'q') return nullptr;
            return fields == 2 ? (IMP)stubPair : fields == 3 ? (IMP)stubTriple : nullptr;
        }
        case '[': case '(': case '^': return types[0] == '^' ? (IMP)stubZero : nullptr;
        default: return (IMP)stubZero;   // integers, booleans, pointers, objects, classes, selectors, C strings
    }
}

// The values worth answering with something other than zero. Everything else of the device protocols is answered by the stubs above.
static NSString *deviceName(id, SEL) { return @(kDeviceName); }
static uint64_t registryID(id, SEL) { return gRegistryID; }
static uint32_t acceleratorPort(id, SEL) { return 0; }   // SkyLight: no accelerator, so the software compositor
static void setAcceleratorService(id, SEL, id service) { gAcceleratorService = service; }
static void setDeviceWrapper(id, SEL, id wrapper) { gDeviceWrapper = wrapper; }
static id deviceWrapper(id self, SEL) { id wrapper = gDeviceWrapper; return wrapper ? wrapper : self; }
static BOOL yes(id, SEL) { return YES; }
static BOOL no(id, SEL) { return NO; }
static uint64_t zero(id, SEL) { return 0; }
static uint32_t one(id, SEL) { return 1; }
static void nothing(id, SEL) {}
// soft-metal-family=N: a client that insists on a GPU family gets one. 0 (the default) is the honest answer for a device that renders
// nothing, and every Metal client must cope with it, because -supportsFamily: is how they ask.
static BOOL family(id, SEL, int64_t which) { return gFamily && which > 0 && (uint64_t)which <= gFamily; }
static BOOL familyUnsigned(id, SEL, uint64_t which) { return family(nil, nullptr, (int64_t)which); }
static uint64_t workingSetSize(id, SEL) { return NSProcessInfo.processInfo.physicalMemory / 3 * 2; }
// Limits a client may size something by, or divide by. Plausible values, not a real GPU's.
static uint64_t bufferLength(id, SEL) { return 256u << 20; }
static uint64_t threadgroupMemory(id, SEL) { return 32 << 10; }
static uint64_t alignment(id, SEL, uint64_t) { return 256; }
static uint64_t textureSide(id, SEL) { return 16384; }
static uint64_t count8(id, SEL) { return 8; }
static uint64_t count16(id, SEL) { return 16; }
static uint64_t count31(id, SEL) { return 31; }
static uint64_t count128(id, SEL) { return 128; }
static uint64_t inlineDataSize(id, SEL) { return 4 << 10; }
static TripleValue threadsPerThreadgroup(id, SEL) { return {512, 512, 512}; }

struct MethodSpec {
    const char *name;
    IMP imp;
    const char *types;   // used when neither the device protocols nor IOGPUMetalDevice declare the selector
};

static const MethodSpec kValues[] = {
    {"name", (IMP)deviceName, "@16@0:8"},
    {"registryID", (IMP)registryID, "Q16@0:8"},
    {"acceleratorPort", (IMP)acceleratorPort, "I16@0:8"},
    {"_setAcceleratorService:", (IMP)setAcceleratorService, "v24@0:8@16"},
    {"_setDeviceWrapper:", (IMP)setDeviceWrapper, "v24@0:8@16"},
    {"_deviceWrapper", (IMP)deviceWrapper, "@16@0:8"},
    {"isBuiltIn", (IMP)yes, "B16@0:8"},
    {"hasUnifiedMemory", (IMP)yes, "B16@0:8"},
    {"isSystemDefaultDevice", (IMP)yes, "B16@0:8"},
    {"isLowPower", (IMP)no, "B16@0:8"},
    {"isHeadless", (IMP)no, "B16@0:8"},
    {"isRemovable", (IMP)no, "B16@0:8"},
    {"supportPriorityBand", (IMP)no, "B16@0:8"},
    {"supportsFamily:", (IMP)family, "B24@0:8q16"},
    {"supportsFeatureSet:", (IMP)familyUnsigned, "B24@0:8Q16"},
    {"peerCount", (IMP)one, "I16@0:8"},
    {"peerIndex", (IMP)zero, "I16@0:8"},
    {"peerGroupID", (IMP)zero, "Q16@0:8"},
    {"recommendedMaxWorkingSetSize", (IMP)workingSetSize, "Q16@0:8"},
    {"sharedMemorySize", (IMP)workingSetSize, "Q16@0:8"},
    {"maxBufferLength", (IMP)bufferLength, "Q16@0:8"},
    {"maxThreadgroupMemoryLength", (IMP)threadgroupMemory, "Q16@0:8"},
    {"maxThreadsPerThreadgroup", (IMP)threadsPerThreadgroup, "{_MTLSize=QQQ}16@0:8"},
    {"minimumLinearTextureAlignmentForPixelFormat:", (IMP)alignment, "Q24@0:8Q16"},
    {"minimumTextureBufferAlignmentForPixelFormat:", (IMP)alignment, "Q24@0:8Q16"},
    {"maxTextureWidth", (IMP)textureSide, "Q16@0:8"},
    {"maxTextureHeight", (IMP)textureSide, "Q16@0:8"},
    {"maxTextureDepth", (IMP)count16, "Q16@0:8"},
    {"maxTextureLayers", (IMP)count16, "Q16@0:8"},
    {"maxColorAttachments", (IMP)count8, "Q16@0:8"},
    {"maxVertexAttributes", (IMP)count31, "Q16@0:8"},
    {"maxVertexBuffers", (IMP)count31, "Q16@0:8"},
    {"maxVertexTextures", (IMP)count128, "Q16@0:8"},
    {"maxVertexSamplers", (IMP)count16, "Q16@0:8"},
    {"maxFragmentBuffers", (IMP)count31, "Q16@0:8"},
    {"maxFragmentTextures", (IMP)count128, "Q16@0:8"},
    {"maxFragmentSamplers", (IMP)count16, "Q16@0:8"},
    {"maxVertexInlineDataSize", (IMP)inlineDataSize, "Q16@0:8"},
    {"maxFragmentInlineDataSize", (IMP)inlineDataSize, "Q16@0:8"},
    {"maxArgumentBufferSamplerCount", (IMP)count16, "Q16@0:8"},
    {"kickCleanupQueue", (IMP)nothing, "v16@0:8"},
    {"updateGPUSelectionProperties", (IMP)nothing, "v16@0:8"},
    {"updateResourcePoolPurgeability", (IMP)nothing, "v16@0:8"},
    {"periodicUpdateResourcePoolPurgeability", (IMP)nothing, "v16@0:8"},
    {"cancelPeriodicUpdateResourcePoolPurgeability", (IMP)nothing, "v16@0:8"},
};

// ---- building the device's class --------------------------------------------------------------------------------------------------
static const char *typesForSelector(SEL selector)
{
    for (const char *name : {"MTLDevice", "MTLDeviceSPI"}) {
        Protocol *protocol = objc_getProtocol(name);
        if (!protocol) continue;
        for (BOOL required : {YES, NO}) {
            struct objc_method_description description = protocol_getMethodDescription(protocol, selector, required, YES);
            if (description.types) return description.types;
        }
    }
    if (Class iogpu = objc_getClass("IOGPUMetalDevice")) {
        if (Method method = class_getInstanceMethod(iogpu, selector)) return method_getTypeEncoding(method);
    }
    return nullptr;
}

// Every selector of MTLDevice and MTLDeviceSPI that neither kValues nor _MTLDevice answers: a stub of the right shape, so no client
// gets an unrecognized selector. Those whose shape is not covered are left to -forwardInvocation:.
static unsigned addProtocolStubs(Class cls)
{
    unsigned added = 0, left = 0;
    for (const char *name : {"MTLDevice", "MTLDeviceSPI"}) {
        Protocol *protocol = objc_getProtocol(name);
        if (!protocol) {
            report("no protocol %s", name);
            continue;
        }
        for (BOOL required : {YES, NO}) {
            unsigned count = 0;
            struct objc_method_description *list = protocol_copyMethodDescriptionList(protocol, required, YES, &count);
            for (unsigned i = 0; i < count; i++) {
                if (!list[i].name || !list[i].types) continue;
                if ([cls instancesRespondToSelector:list[i].name]) continue;
                IMP imp = stubFor(list[i].types);
                if (!imp) {
                    left++;
                    continue;
                }
                if (class_addMethod(cls, list[i].name, imp, list[i].types)) added++;
            }
            free(list);
        }
    }
    report("%u selectors stubbed, %u left to forwarding", added, left);
    return added;
}

// The last net: anything still unanswered is logged and answered with zeros, rather than throwing. The signature comes from the device
// protocols or IOGPUMetalDevice; without one, a generic integer-returning shape (the arguments are not read).
@interface NativeSoftMetalForwarder : NSObject
@end

static NSMethodSignature *signatureForSelector(id, SEL, SEL selector)
{
    const char *types = typesForSelector(selector);
    if (!types) {
        noteOnce("unknown shape", selector);
        types = "q72@0:8@16@24@32@40@48@56@64";
    }
    return [NSMethodSignature signatureWithObjCTypes:types];
}

static void forwardInvocation(id, SEL, NSInvocation *invocation)
{
    noteOnce("forwarded", invocation.selector);
    NSUInteger length = invocation.methodSignature.methodReturnLength;
    if (!length) return;
    void *zeros = calloc(1, length);
    if (zeros) {
        [invocation setReturnValue:zeros];
        free(zeros);
    }
}

static id makeDevice()
{
    Class base = objc_getClass("_MTLDevice");   // the base class Metal's registration expects of a plugin's device
    if (!base) {
        report("no _MTLDevice in Metal.framework");
        return nil;
    }
    Class cls = objc_allocateClassPair(base, "NativeSoftMetalDeviceObject", 0);
    if (!cls) return nil;
    for (const MethodSpec &method : kValues) {
        SEL selector = sel_registerName(method.name);
        const char *types = typesForSelector(selector);
        class_addMethod(cls, selector, method.imp, types ? types : method.types);
        class_replaceMethod(cls, selector, method.imp, types ? types : method.types);
    }
    if (bootArgValue("soft-metal-stubs", 1)) addProtocolStubs(cls);
    class_addMethod(cls, @selector(methodSignatureForSelector:), (IMP)signatureForSelector, "@24@0:8:16");
    class_addMethod(cls, @selector(forwardInvocation:), (IMP)forwardInvocation, "v24@0:8@16");
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
        const bool windowServer = !strcmp(getprogname(), "WindowServer");
        if (!bootArgValue("soft-metal", 1) || (!bootArgValue("soft-metal-all", 0) && !windowServer)) {
            report("no device (%s)", !bootArgValue("soft-metal", 1) ? "soft-metal=0" : "WindowServer only");
            return nil;
        }
        gFamily = bootArgValue("soft-metal-family", 0);
        IORegistryEntryGetRegistryEntryID(port, &gRegistryID);
        device = makeDevice();
        report("device %s, registry ID %#llx, port 0, no queue: software compositing", device ? "made" : "NOT made",
            (unsigned long long)gRegistryID);
        if (gFamily) report("supportsFamily: yes up to %u (soft-metal-family)", gFamily);
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

// Installed when the bundle is loaded, which is before any of the above runs: whatever aborts this process afterwards is reported,
// whether it is the Metal device's doing or not.
__attribute__((constructor)) static void start()
{
    if (!strcmp(getprogname(), "WindowServer") || bootArgValue("soft-metal-all", 0)) {
        installReporter();
        framebufferPolicy();
    }
}
