// SPDX-License-Identifier: BSD-2-Clause-Patent
// The plugin's abort reporter (tooling/metal/turnip-a830/abort_reporter.mm) on the build Mac (tooling/metal/check-turnip-a830-host.py):
// the staged NativeAdrenoMetal is dlopen'ed with PIANO_METAL_REPORT=2 in the environment (its constructor installs the reporter), then
// the process dies the way a desktop process would: abort() with a message, an uncaught Objective-C exception, or a bad access. The
// check reads stderr and the exit signal.
//
//   reporter_host PLUGIN_EXECUTABLE abort|exception|fault
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// What abort() paths that explain themselves leave for the crash reporter (libsystem's crashreporter_annotations_t).
extern "C" {
struct crashreporter_annotations_t {
    uint64_t version;
    uint64_t message;
    uint64_t signature_string;
    uint64_t backtrace;
    uint64_t message2;
    uint64_t thread;
    uint64_t dialog_mode;
    uint64_t abort_cause;
};
__attribute__((used, section("__DATA,__crash_info"))) struct crashreporter_annotations_t gCRAnnotations = {5, 0, 0, 0, 0, 0, 0, 0};
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    if (!dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)) {
        printf("FAIL: dlopen %s\n", dlerror());
        return 1;
    }
    fflush(stdout);
    if (!strcmp(argv[2], "abort")) {
        gCRAnnotations.message = (uint64_t)(uintptr_t)"reporter check: an abort that explains itself";
        abort();
    }
    if (!strcmp(argv[2], "exception"))
        @throw [NSException exceptionWithName:@"PianoReporterCheck" reason:@"an uncaught exception" userInfo:nil];
    if (!strcmp(argv[2], "fault"))
        *(volatile int *)(uintptr_t)16 = 1;
    return 3;
}
