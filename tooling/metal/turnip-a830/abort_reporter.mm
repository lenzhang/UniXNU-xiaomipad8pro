// SPDX-License-Identifier: BSD-2-Clause-Patent
// The abort reporter of the native A830 path's Metal plugin (linked into NativeAdrenoMetal by tooling/metal/build-turnip-a830.sh;
// docs/drivers/gpu-userspace.md "Desktop readiness"): NativeSoftMetal's (package/NativeSoftMetal/NativeSoftMetalPlugin.mm, this
// repository's own) in the processes of a desktop session that load the GPU's plugin. A WindowServer, Dock or Finder that dies after the
// login says why on the console: the console log otherwise has only the kernel's "exit: pid N <name> status 0x6" (SkyLight's and Core
// Animation's asserts go to os_log, and AMFI denies the core dump). On an uncaught Objective-C exception, a C++ terminate or a fatal signal
// it prints the reason, every loaded image's crash-reporter message (__DATA,__crash_info: abort() paths that explain themselves, SkyLight's
// and assert()'s among them, leave their text there) and the stack as "<image>+<offset>", which names the framework without symbols.
// Reports go to stderr, one write per line of at most 160 bytes ("NativeAdrenoMetal: <process>: ..."), which the kernel's stderr-log=1
// puts on the console.
//
// It chains: the process's own uncaught-exception and terminate handlers are called after the report, and each signal's previous action
// is put back before the signal is raised again (a fault re-executes into it), so the process ends, or carries on, as it would have.
// Kernel boot argument piano-metal-report=0 leaves it out, =2 installs it in every process that loads the plugin; the default (1) is the
// desktop's system processes below (WebKit's processes and the apps, which may keep signal handlers of their own, are left alone).
#import <Foundation/Foundation.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <exception>
#include <execinfo.h>
#include <initializer_list>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {
// The processes the reporter is installed in by default: the kernel's grant list (package/PianoA830 NativeAdrenoPublish.h) without WebKit.
const char *const kProcesses[] = {"WindowServer", "Dock", "WallpaperAgent", "iconservicesagent", "Finder", "loginwindow", "ControlCenter",
                                  "NotificationCenter", "WindowManager", "SystemUIServer", "Spotlight", "chronod", "universalaccessd",
                                  "screencaptureui", "SecurityAgent", "MenuBarAgent"};

unsigned bootArgValue(const char *name, unsigned fallback)
{
    char args[1024] = {};
    size_t size = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0)) return fallback;
    const size_t length = strlen(name);
    for (char *at = args; (at = strstr(at, name)); at += length)
        if ((at == args || at[-1] == ' ') && at[length] == '=') return (unsigned)strtoul(at + length + 1, nullptr, 0);
    return fallback;
}

// One line on fd 2, newline-terminated even when cut at the console's 160 bytes.
void writeLine(char *line, size_t size, int length)
{
    if (length <= 0) return;
    size_t n = (size_t)length < size ? (size_t)length : size - 1;
    if (n && line[n - 1] != '\n') {
        if (n == size - 1) n--;
        line[n++] = '\n';
    }
    (void)!write(2, line, n);
}

__attribute__((format(printf, 1, 2))) void report(const char *format, ...)
{
    char line[160], text[128];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    writeLine(line, sizeof line, snprintf(line, sizeof line, "NativeAdrenoMetal: %s: %s\n", getprogname(), text));
}

// In a signal handler: no allocation, no printf of the va kind.
void reportRaw(const char *text)
{
    char line[160];
    size_t at = 0;
    for (const char *from : {"NativeAdrenoMetal: ", getprogname(), ": ", text, "\n"})
        for (const char *c = from; *c && at < sizeof line - 1; c++) line[at++] = *c;
    (void)!write(2, line, at);
}

void reportFrames(void *const *frames, int count)
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
        writeLine(line, sizeof line, snprintf(line, sizeof line, "NativeAdrenoMetal: %s: #%d %.40s+0x%lx %.40s\n", getprogname(), i, image,
            offset, info.dli_sname ? info.dli_sname : ""));
    }
}

// The crash-reporter annotations of every loaded image (libsystem's crashreporter_annotations_t in __DATA,__crash_info: version, then the
// message and, at +32, message2), in 72-byte pieces.
void reportCrashMessages()
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
                const size_t length = strnlen(message, 600);
                for (size_t at = 0; at < length; at += 72)
                    writeLine(line, sizeof line, snprintf(line, sizeof line, "NativeAdrenoMetal: %s: crash message (%.20s): %.72s\n",
                        getprogname(), slash ? slash + 1 : path ? path : "?", message + at));
            }
        }
    }
}

const int kSignals[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
struct sigaction gPrevious[sizeof kSignals / sizeof kSignals[0]];
NSUncaughtExceptionHandler *gNextException;
std::terminate_handler gNextTerminate;

void onSignal(int number, siginfo_t *, void *)
{
    reportRaw(number == SIGABRT ? "SIGABRT" : number == SIGSEGV ? "SIGSEGV" : number == SIGBUS ? "SIGBUS" : number == SIGILL ? "SIGILL" :
              number == SIGTRAP ? "SIGTRAP" : "fatal signal");
    reportCrashMessages();
    void *frames[32];
    reportFrames(frames, backtrace(frames, 32));
    // The process's own action for it again, then the signal: the default ends the process as it would have; a handler of its own gets it.
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; i++)
        if (kSignals[i] == number) sigaction(number, &gPrevious[i], nullptr);
    raise(number);
}

void onException(NSException *exception)
{
    report("uncaught %.40s", exception.name.UTF8String ?: "?");
    const char *reason = exception.reason.UTF8String ?: "";
    for (size_t at = 0; at < strlen(reason) && at < 600; at += 100) report("reason: %.100s", reason + at);
    NSArray<NSNumber *> *addresses = exception.callStackReturnAddresses;
    void *frames[32];
    int count = 0;
    for (NSNumber *address in addresses) {
        if (count == 32) break;
        frames[count++] = (void *)(uintptr_t)address.unsignedLongLongValue;
    }
    reportFrames(frames, count);
    if (gNextException) gNextException(exception);
}

void onTerminate()
{
    if (std::type_info *type = abi::__cxa_current_exception_type()) {
        int status = 0;
        char *name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
        report("terminate: %.60s", name ? name : type->name());
        free(name);
        try {
            throw;
        } catch (NSException *exception) {
            report("uncaught %.40s: %.80s", exception.name.UTF8String ?: "?", exception.reason.UTF8String ?: "");
        } catch (const std::exception &error) {
            report("what: %.100s", error.what());
        } catch (...) {
            reportRaw("terminate: unknown exception");
        }
    } else {
        reportRaw("terminate without an exception");
    }
    reportCrashMessages();
    void *frames[32];
    reportFrames(frames, backtrace(frames, 32));
    if (gNextTerminate) gNextTerminate();
    abort();
}

__attribute__((constructor)) void installReporter()
{
    // PIANO_METAL_REPORT in the environment stands in for the boot argument (the build Mac's check, tooling/metal/check-turnip-a830-host.py).
    const char *forced = getenv("PIANO_METAL_REPORT");
    const unsigned mode = forced && *forced ? (unsigned)strtoul(forced, nullptr, 0) : bootArgValue("piano-metal-report", 1);
    if (!mode) return;
    bool listed = mode >= 2;
    for (const char *name : kProcesses) listed = listed || !strcmp(getprogname(), name);
    if (!listed) return;
    gNextException = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(onException);
    gNextTerminate = std::set_terminate(onTerminate);
    struct sigaction action = {};
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    action.__sigaction_u.__sa_sigaction = onSignal;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; i++) sigaction(kSignals[i], &action, &gPrevious[i]);
    report("abort reporter installed (piano-metal-report=0 leaves it out)");
}
}  // namespace
