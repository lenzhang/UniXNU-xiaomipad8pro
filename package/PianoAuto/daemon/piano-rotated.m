// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// piano-rotated: automatic display rotation on native macOS for piano (docs/drivers/sensors.md section 5a). A launchd agent
// (com.unixnu.piano-rotated.plist: the login window's session and each user's, so it acts at the login window too) that
//   reads    PianoSensors' "piano-sensors-edge" (the panel edge up: 0 top, 1 left, 2 bottom, 3 right, anything else none) and
//            PianoKeyboard's "piano-keyboard-attached" (the cover, 0/1), IORegistry properties of their services, on each
//            kIOMessageServicePropertyChange they send (no polling);
//   decides  AutoRotate.c over SensorsPolicy's ps_rotation_step, the functions the host tests run;
//   rotates  WindowServer's own rotation of the panel's display, the one System Settings > Displays > Rotation sets. No public API
//            sets it (CoreGraphics only reads it: CGDisplayRotation); the setter used is MonitorPanel's (a private framework):
//            +[MPDisplayMgr sharedMgr], -displayWithID:, -[MPDisplay canChangeOrientation], -orientation, -setOrientation:, found by
//            their runtime names and checked against the type encodings they had on macOS 26.6 before any call; if anything differs
//            the agent rotates nothing and says why. Every rotation is read back with the public CGDisplayRotation;
//   tells    PianoTouch the rotation shown (its "piano-touch-rotation" property, TouchRotation.h), never one only asked for.
// Nothing runs unless the kernel boot argument piano-rotate is 1 or more (sysctl kern.bootargs):
//   piano-rotate=1   observe: inputs and decisions logged, nothing changed
//   piano-rotate=2   + WindowServer's rotation
//   piano-rotate=3   + PianoTouch told
//   piano-rotate-map=T,L,B,R   degrees for the top/left/bottom/right edge up (default 0,90,180,270: CGDisplayRotation's sense, 90 = the
//                              panel's left edge up)
//   piano-rotate-settle=MS (600), -interval=MS (1000), -detach=MS (1500)   the policy's times (AutoRotate.h)
//   piano-rotate-cover=N       2 (default) held at 0 while the cover is attached, 1 turned to 0 at attach only, 0 the cover ignored
//   piano-rotate-test=1        the edge from the preference TestEdge instead of PianoSensors (a test of everything but the sensor)
// The user's rotation lock: `defaults write com.unixnu.piano-rotated Locked -bool true`, then `notifyutil -p
// com.unixnu.piano-rotated.prefs` (or SIGHUP). Command line, for the USB root shell: --probe (what it sees; changes nothing),
// --set DEGREES [--touch] (one rotation, read back), --level N (instead of the boot argument), --stderr (the log on stderr too).
// Log: os_log subsystem com.unixnu.piano-rotated (`log show --last 5m --predicate 'subsystem == "com.unixnu.piano-rotated"'`).
// Build: tooling/k27/build-piano-rotated.sh (the host tests, then arm64, ad-hoc signed by the linker).
#import <Foundation/Foundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include "AutoRotate.h"
#include "TouchRotation.h"

#define APP "com.unixnu.piano-rotated"
#define PREFS_NOTIFICATION APP ".prefs"
#define EDGE_PROPERTY "piano-sensors-edge"
#define COVER_PROPERTY "piano-keyboard-attached"
// NativeGopDisplay's ProductAttributes: ManufacturerID "XMI" (EISA: X=24, M=13, I=9), ProductID 0x8750.
static const uint32_t kPanelVendor = (24u << 10) | (13u << 5) | 9u, kPanelModel = 0x8750;

static os_log_t gLog;
static int gStderr, gLevel, gTestEdges, gRunning;   // gRunning: the agent's loop (not --probe / --set)
static ParAgent gAgent;

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    os_log(gLog, "%{public}s", line);
    if (gStderr) fprintf(stderr, "piano-rotated: %s\n", line);
}

static uint64_t nowMs(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}

// ------------------------------------------------------------------------------------------------------------- boot arguments
static char gBootArgs[2048];

static void readBootArgs(void)
{
    size_t n = sizeof gBootArgs - 1;
    if (sysctlbyname("kern.bootargs", gBootArgs, &n, NULL, 0) != 0) n = 0;
    gBootArgs[n < sizeof gBootArgs ? n : sizeof gBootArgs - 1] = 0;
}

// NAME=VALUE (or a bare NAME, "1") from the boot arguments, or NULL.
static const char *bootArg(const char *name, char *out, size_t cap)
{
    const size_t len = strlen(name);
    for (const char *p = gBootArgs; *p;) {
        while (*p == ' ') ++p;
        const char *word = p;
        while (*p && *p != ' ') ++p;
        const size_t n = (size_t)(p - word);
        if (n == len && !strncmp(word, name, len)) {
            snprintf(out, cap, "1");
            return out;
        }
        if (n > len && !strncmp(word, name, len) && word[len] == '=') {
            size_t v = n - len - 1;
            if (v >= cap) v = cap - 1;
            memcpy(out, word + len + 1, v);
            out[v] = 0;
            return out;
        }
    }
    return NULL;
}

static long bootNumber(const char *name, long fallback, long lo, long hi)
{
    char value[64];
    if (!bootArg(name, value, sizeof value)) return fallback;
    char *end = NULL;
    const long n = strtol(value, &end, 0);
    if (end == value) return fallback;
    return n < lo ? lo : n > hi ? hi : n;
}

static void configure(ParConfig *c)
{
    par_defaults(c);
    char map[64];
    if (bootArg("piano-rotate-map", map, sizeof map)) {
        int d[4], k = 0;
        for (char *p = map, *end; k < 4; ++k, p = end + 1) {
            d[k] = (int)strtol(p, &end, 10);
            if (end == p || !par_valid_degrees(d[k]) || (k < 3 && *end != ',')) break;
        }
        if (k == 4)
            for (int e = 0; e < 4; ++e) c->rot.degrees[e] = (uint16_t)d[e];
        else
            note("piano-rotate-map=%s is not four of 0/90/180/270: the default map", map);
    }
    c->rot.settle_ms = (uint32_t)bootNumber("piano-rotate-settle", c->rot.settle_ms, 0, 10000);
    c->rot.min_interval_ms = (uint32_t)bootNumber("piano-rotate-interval", c->rot.min_interval_ms, 0, 60000);
    c->detach_hold_ms = (uint32_t)bootNumber("piano-rotate-detach", c->detach_hold_ms, 0, 60000);
    c->cover_lock = (int)bootNumber("piano-rotate-cover", PAR_LOCK_HOLD, PAR_LOCK_IGNORE, PAR_LOCK_HOLD);
    gTestEdges = gTestEdges || bootNumber("piano-rotate-test", 0, 0, 1);
}

// ------------------------------------------------------------------------------------------------------------- the display
static CGDirectDisplayID gDisplayOverride;

static CGDirectDisplayID panelDisplay(void)
{
    if (gDisplayOverride) return gDisplayOverride;
    CGDirectDisplayID ids[8];
    uint32_t n = 0;
    if (CGGetOnlineDisplayList(8, ids, &n) != kCGErrorSuccess) n = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (CGDisplayVendorNumber(ids[i]) == kPanelVendor && CGDisplayModelNumber(ids[i]) == kPanelModel) return ids[i];
    return n == 1 ? ids[0] : CGMainDisplayID();
}

static int degreesShown(CGDirectDisplayID d)
{
    if (!d) return -1;
    const int deg = ((int)(CGDisplayRotation(d) + 0.5) % 360 + 360) % 360;
    return par_valid_degrees(deg) ? deg : -1;
}

static int onConsole(void)
{
    CFDictionaryRef s = CGSessionCopyCurrentDictionary();
    static int told;
    if (!s) {
        if (!told++) note("no session dictionary (the login window?): acting as the console's session");
        return 1;
    }
    CFBooleanRef b = (CFBooleanRef)CFDictionaryGetValue(s, kCGSessionOnConsoleKey);
    const int on = b && CFGetTypeID(b) == CFBooleanGetTypeID() && CFBooleanGetValue(b);
    CFRelease(s);
    return on;
}

// ------------------------------------------------------------------------------------------------------------- WindowServer's rotation
// MonitorPanel (System Settings' display code), by its runtime names; the type encodings are compared without their offsets.
static struct {
    int ready;
    char why[200];
    Class mgr, display;
    SEL shared, withID, can, get, set, sensor;
} gMp;

static int typesAre(Method m, const char *want)
{
    if (!m) return 0;
    const char *t = method_getTypeEncoding(m);
    char s[64];
    size_t k = 0;
    for (; t && *t && k + 1 < sizeof s; ++t)
        if (*t < '0' || *t > '9') s[k++] = *t;
    s[k] = 0;
    return !strcmp(s, want);
}

static void backendInit(void)
{
    if (!dlopen("/System/Library/PrivateFrameworks/MonitorPanel.framework/MonitorPanel", RTLD_LAZY)) {
        snprintf(gMp.why, sizeof gMp.why, "MonitorPanel does not load (%s)", dlerror());
        return;
    }
    gMp.mgr = objc_getClass("MPDisplayMgr");
    gMp.display = objc_getClass("MPDisplay");
    gMp.shared = sel_registerName("sharedMgr");
    gMp.withID = sel_registerName("displayWithID:");
    gMp.can = sel_registerName("canChangeOrientation");
    gMp.get = sel_registerName("orientation");
    gMp.set = sel_registerName("setOrientation:");
    gMp.sensor = sel_registerName("hasRotationSensor");
    const char *missing = !gMp.mgr ? "class MPDisplayMgr" : !gMp.display ? "class MPDisplay"
                        : !typesAre(class_getClassMethod(gMp.mgr, gMp.shared), "@@:") ? "+[MPDisplayMgr sharedMgr] as @@:"
                        : !typesAre(class_getInstanceMethod(gMp.mgr, gMp.withID), "@@:i") ? "-[MPDisplayMgr displayWithID:] as @@:i"
                        : !typesAre(class_getInstanceMethod(gMp.display, gMp.can), "B@:") ? "-[MPDisplay canChangeOrientation] as B@:"
                        : !typesAre(class_getInstanceMethod(gMp.display, gMp.get), "i@:") ? "-[MPDisplay orientation] as i@:"
                        : !typesAre(class_getInstanceMethod(gMp.display, gMp.set), "v@:i") ? "-[MPDisplay setOrientation:] as v@:i"
                        : NULL;
    if (missing) {
        snprintf(gMp.why, sizeof gMp.why, "MonitorPanel has no %s (another macOS build?): no rotation", missing);
        return;
    }
    gMp.ready = 1;
    snprintf(gMp.why, sizeof gMp.why, "MonitorPanel MPDisplay -setOrientation:");
}

static id mpDisplay(CGDirectDisplayID d)
{
    if (!gMp.ready) return nil;
    id mgr = ((id (*)(Class, SEL))objc_msgSend)(gMp.mgr, gMp.shared);
    return mgr ? ((id (*)(id, SEL, int))objc_msgSend)(mgr, gMp.withID, (int)d) : nil;
}

static int mpBool(id display, SEL sel, int fallback)
{
    if (!display || !class_getInstanceMethod(object_getClass(display), sel) ||
        !typesAre(class_getInstanceMethod(object_getClass(display), sel), "B@:"))
        return fallback;
    return ((BOOL (*)(id, SEL))objc_msgSend)(display, sel) ? 1 : 0;
}

static int applyRotation(CGDirectDisplayID d, int degrees, char *why, size_t cap)
{
    if (!gMp.ready) {
        snprintf(why, cap, "%s", gMp.why);
        return 0;
    }
    id display = mpDisplay(d);
    if (!display) {
        snprintf(why, cap, "MonitorPanel has no display %#x", d);
        return 0;
    }
    if (!mpBool(display, gMp.can, 0)) {
        snprintf(why, cap, "WindowServer offers no rotation for display %#x (canChangeOrientation NO)", d);
        return 0;
    }
    ((void (*)(id, SEL, int))objc_msgSend)(display, gMp.set, degrees);
    snprintf(why, cap, "asked");
    return 1;
}

// ------------------------------------------------------------------------------------------------------------- the drivers
typedef struct {
    const char *cls;
    io_service_t service;
    io_object_t interest;
    io_iterator_t matching;
} Watched;
static Watched gSensors = {"PianoSensors", 0, 0, 0}, gKeyboard = {"PianoKeyboard", 0, 0, 0}, gTouch = {"PianoTouch", 0, 0, 0};
static IONotificationPortRef gPort;

static int readNumber(io_service_t s, const char *key, long *out)
{
    if (!s) return 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = k ? IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0) : NULL;
    int ok = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) ok = CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, out);
    else if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) *out = CFBooleanGetValue((CFBooleanRef)v), ok = 1;
    if (v) CFRelease(v);
    if (k) CFRelease(k);
    return ok;
}

// PianoTouch told the rotation shown. Three failures in a row (no PianoTouch, or one without the property: kIOReturnUnsupported) end
// the retries until the rotation changes again, so a missing driver costs no wake-ups.
static int pushTouch(int degrees)
{
    static int failures, failedDegrees = -1;
    if (degrees != failedDegrees) failures = 0;
    kern_return_t kr = KERN_FAILURE;
    if (gTouch.service) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &degrees);
        kr = IORegistryEntrySetCFProperty(gTouch.service, CFSTR(PAT_PROPERTY), n);
        CFRelease(n);
    }
    if (kr == KERN_SUCCESS) {
        note("PianoTouch told %d degrees: taken", degrees);
        failures = 0;
        return 1;
    }
    failedDegrees = degrees;
    if (++failures < 3) {
        note("PianoTouch told %d degrees: %s (%#x)", degrees, gTouch.service ? "REFUSED" : "not found", kr);
        return 0;
    }
    if (failures == 3) note("PianoTouch did not take %d degrees three times: not told again until the rotation changes", degrees);
    return 1;
}

// ------------------------------------------------------------------------------------------------------------- the policy
static CFRunLoopTimerRef gTimer;
static int gInPolicy, gAgain, gLastShown = -2, gLastWant = -2, gLastEdge = -2, gLastCover = -2;
static const char *gLastWhy;

static const char *coverName(int c)
{
    return c == PAR_COVER_ATTACHED ? "attached" : c == PAR_COVER_DETACHED ? "detached" : "unknown (no PianoKeyboard)";
}

static void schedule(uint64_t deadline)
{
    if (!gTimer) return;
    const uint64_t now = nowMs();
    const double in = !deadline ? 1e9 : deadline <= now ? 0.001 : (double)(deadline - now) / 1000.0;
    CFRunLoopTimerSetNextFireDate(gTimer, CFAbsoluteTimeGetCurrent() + in);
}

static void runPolicy(void)
{
    if (gInPolicy) {
        gAgain = 1;
        return;
    }
    gInPolicy = 1;
    for (int pass = 0; pass < 6; ++pass) {
        gAgain = 0;
        const CGDirectDisplayID display = panelDisplay();
        const int shown = degreesShown(display);
        uint64_t now = nowMs();
        if (shown != gLastShown) {
            note("display %#x shows %d degrees", display, shown);
            gLastShown = shown;
        }
        if (shown >= 0) par_observed(&gAgent, shown, now);
        const int can = gLevel >= 2 && gMp.ready && display && !CGDisplayIsAsleep(display) && onConsole();
        const ParDecision d = par_step(&gAgent, now, can);
        if (d.want != gLastWant || d.why != gLastWhy) {
            if (d.want >= 0) note("wants %d degrees: %s%s", d.want, d.why, d.apply < 0 && gLevel < 2 ? " (observing only)" : "");
            else note("%s", d.why);
            gLastWant = d.want;
            gLastWhy = d.why;
        }
        if (d.apply >= 0) {
            char why[240];
            const int ok = applyRotation(display, d.apply, why, sizeof why);
            note("rotate %d -> %d degrees: %s", shown, d.apply, why);
            par_applied(&gAgent, d.apply, ok, nowMs());
            continue;
        }
        if (d.touch >= 0) {
            par_touch_done(&gAgent, d.touch, gLevel >= 3 ? pushTouch(d.touch) : 1, nowMs());
            continue;
        }
        if (!gAgain) break;
    }
    gInPolicy = 0;
    schedule(par_deadline(&gAgent, nowMs()));
}

static void readInputs(void)
{
    const uint64_t now = nowMs();
    long v = -1;
    int edge = PS_EDGE_NONE;
    if (gTestEdges) {
        CFPreferencesAppSynchronize(CFSTR(APP));
        Boolean valid = false;
        const CFIndex e = CFPreferencesGetAppIntegerValue(CFSTR("TestEdge"), CFSTR(APP), &valid);
        edge = valid && e >= PS_EDGE_TOP && e <= PS_EDGE_RIGHT ? (int)e : PS_EDGE_NONE;
    } else if (readNumber(gSensors.service, EDGE_PROPERTY, &v) && v >= PS_EDGE_TOP && v <= PS_EDGE_RIGHT) {
        edge = (int)v;
    }
    int cover = PAR_COVER_UNKNOWN;
    if (readNumber(gKeyboard.service, COVER_PROPERTY, &v)) cover = v ? PAR_COVER_ATTACHED : PAR_COVER_DETACHED;
    if (edge != gLastEdge) {
        note("edge up: %s%s", ps_edge_name(edge), gTestEdges ? " (test preference)" : gSensors.service ? "" : " (no PianoSensors)");
        gLastEdge = edge;
    }
    if (cover != gLastCover) {
        note("keyboard cover %s", coverName(cover));
        gLastCover = cover;
    }
    par_cover(&gAgent, cover, now);
    par_edge(&gAgent, edge, now);
}

static void reloadPrefs(void)
{
    CFPreferencesAppSynchronize(CFSTR(APP));
    Boolean valid = false;
    const int locked = CFPreferencesGetAppBooleanValue(CFSTR("Locked"), CFSTR(APP), &valid) && valid;
    if (locked != gAgent.locked) note("rotation lock %s", locked ? "on" : "off");
    par_lock(&gAgent, locked);
}

static void interest(void *ctx, io_service_t service, natural_t type, void *argument)
{
    (void)service;
    (void)argument;
    Watched *w = ctx;
    if (type == kIOMessageServiceIsTerminated) {
        note("%s went away", w->cls);
        if (w->interest) IOObjectRelease(w->interest);
        if (w->service) IOObjectRelease(w->service);
        w->interest = w->service = 0;
    }
    if (gRunning && (type == kIOMessageServicePropertyChange || type == kIOMessageServiceIsTerminated)) {
        readInputs();
        runPolicy();
    }
}

static void matched(void *ctx, io_iterator_t iterator)
{
    Watched *w = ctx;
    for (io_service_t s; (s = IOIteratorNext(iterator));) {
        if (w->service) {
            IOObjectRelease(s);
            continue;
        }
        w->service = s;
        if (w != &gTouch) IOServiceAddInterestNotification(gPort, s, kIOGeneralInterest, interest, w, &w->interest);
        note("found %s", w->cls);
    }
    if (gRunning) {
        readInputs();
        runPolicy();
    }
}

static void watch(Watched *w)
{
    if (IOServiceAddMatchingNotification(gPort, kIOFirstMatchNotification, IOServiceMatching(w->cls), matched, w, &w->matching) ==
        KERN_SUCCESS) {
        for (io_service_t s; (s = IOIteratorNext(w->matching));) {   // arms the notification and takes what is there now
            if (w->service) {
                IOObjectRelease(s);
                continue;
            }
            w->service = s;
            if (w != &gTouch) IOServiceAddInterestNotification(gPort, s, kIOGeneralInterest, interest, w, &w->interest);
            note("found %s", w->cls);
        }
    }
}

static void reconfigured(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags, void *ctx)
{
    (void)display;
    (void)ctx;
    if (!(flags & kCGDisplayBeginConfigurationFlag)) runPolicy();
}

// ------------------------------------------------------------------------------------------------------------- one-shot modes
static void probe(void)
{
    CGDirectDisplayID ids[8];
    uint32_t n = 0;
    if (CGGetOnlineDisplayList(8, ids, &n) != kCGErrorSuccess) n = 0;
    printf("piano-rotated --probe: level %d (boot arguments: piano-rotate=%ld), test edges %d\n", gLevel, bootNumber("piano-rotate", 0, 0, 3),
           gTestEdges);
    for (uint32_t i = 0; i < n; ++i) {
        const CGRect b = CGDisplayBounds(ids[i]);
        printf("  display %#x: vendor %#x model %#x%s, %.0fx%.0f points, rotation %.0f degrees, %s%s\n", ids[i], CGDisplayVendorNumber(ids[i]),
               CGDisplayModelNumber(ids[i]),
               CGDisplayVendorNumber(ids[i]) == kPanelVendor && CGDisplayModelNumber(ids[i]) == kPanelModel ? " (the panel)" : "",
               b.size.width, b.size.height, CGDisplayRotation(ids[i]), CGDisplayIsAsleep(ids[i]) ? "asleep" : "awake",
               ids[i] == CGMainDisplayID() ? ", main" : "");
    }
    const CGDirectDisplayID d = panelDisplay();
    printf("  panel display: %#x\n  backend: %s\n", d, gMp.why);
    if (gMp.ready) {
        id display = mpDisplay(d);
        printf("  MPDisplay for %#x: %s; canChangeOrientation %d, orientation %d, hasRotationSensor %d\n", d, display ? "found" : "NONE",
               mpBool(display, gMp.can, -1),
               display ? ((int (*)(id, SEL))objc_msgSend)(display, gMp.get) : -1, mpBool(display, gMp.sensor, -1));
    }
    long v = -1;
    printf("  PianoSensors: %s, %s %s\n", gSensors.service ? "found" : "not found", EDGE_PROPERTY,
           readNumber(gSensors.service, EDGE_PROPERTY, &v) ? ps_edge_name(v >= 0 && v <= 3 ? (int)v : PS_EDGE_NONE) : "absent");
    printf("  PianoKeyboard: %s, %s %s\n", gKeyboard.service ? "found" : "not found", COVER_PROPERTY,
           readNumber(gKeyboard.service, COVER_PROPERTY, &v) ? (v ? "1 (attached)" : "0 (detached)") : "absent");
    printf("  PianoTouch: %s, %s %s\n", gTouch.service ? "found" : "not found", PAT_PROPERTY,
           readNumber(gTouch.service, PAT_PROPERTY, &v) ? (v == 0 ? "0" : v == 90 ? "90" : v == 180 ? "180" : v == 270 ? "270" : "?") : "absent");
    printf("  session on the console: %d; rotation lock (preference Locked): %d\n", onConsole(), gAgent.locked);
}

static int setOnce(int degrees, int touch)
{
    const CGDirectDisplayID d = panelDisplay();
    char why[240];
    const int before = degreesShown(d);
    if (!applyRotation(d, degrees, why, sizeof why)) {
        printf("piano-rotated --set %d: display %#x at %d degrees: NOT asked: %s\n", degrees, d, before, why);
        return 1;
    }
    int shown = before;
    for (int i = 0; i < 30 && shown != degrees; ++i) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
        shown = degreesShown(d);
    }
    printf("piano-rotated --set %d: display %#x %d -> %d degrees: %s\n", degrees, d, before, shown, shown == degrees ? "shown" : "NOT shown in 3 s");
    if (touch) pushTouch(shown);
    return shown == degrees ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------------------- main
int main(int argc, char **argv)
{
    @autoreleasepool {
        gLog = os_log_create(APP, "agent");
        int doProbe = 0, set = -1, touch = 0, level = -1;
        for (int i = 1; i < argc; ++i) {
            if (!strcmp(argv[i], "--probe")) doProbe = 1;
            else if (!strcmp(argv[i], "--set") && i + 1 < argc) set = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--touch")) touch = 1;
            else if (!strcmp(argv[i], "--level") && i + 1 < argc) level = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--display") && i + 1 < argc) gDisplayOverride = (CGDirectDisplayID)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(argv[i], "--test-edges")) gTestEdges = 1;
            else if (!strcmp(argv[i], "--stderr")) gStderr = 1;
            else {
                fprintf(stderr, "usage: piano-rotated [--probe | --set DEGREES [--touch]] [--level N] [--display ID] [--test-edges] [--stderr]\n");
                return 2;
            }
        }
        readBootArgs();
        gLevel = level >= 0 ? (level > 3 ? 3 : level) : (int)bootNumber("piano-rotate", 0, 0, 3);
        ParConfig config;
        configure(&config);
        par_init(&gAgent, &config);
        backendInit();
        gPort = IONotificationPortCreate(kIOMainPortDefault);
        CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(gPort), kCFRunLoopDefaultMode);
        watch(&gSensors);
        watch(&gKeyboard);
        watch(&gTouch);
        reloadPrefs();
        if (doProbe) {
            probe();
            return 0;
        }
        if (set >= 0) {
            if (!par_valid_degrees(set)) {
                fprintf(stderr, "piano-rotated: --set takes 0, 90, 180 or 270\n");
                return 2;
            }
            return setOnce(set, touch);
        }
        if (!gLevel) {
            note("piano-rotate is 0 or absent: not running");
            return 0;
        }
        const CGDirectDisplayID d = panelDisplay();
        id display = mpDisplay(d);
        note("start: level %d (%s), map %u/%u/%u/%u, settle %u ms, interval %u ms, detach hold %u ms, cover %s; display %#x (vendor %#x "
             "model %#x) at %d degrees; backend %s (canChangeOrientation %d, hasRotationSensor %d)%s",
             gLevel, gLevel == 1 ? "observe" : gLevel == 2 ? "rotate" : "rotate + touch", (unsigned)config.rot.degrees[0],
             (unsigned)config.rot.degrees[1], (unsigned)config.rot.degrees[2], (unsigned)config.rot.degrees[3], config.rot.settle_ms, config.rot.min_interval_ms, config.detach_hold_ms,
             config.cover_lock == PAR_LOCK_HOLD ? "held at 0 while attached" : config.cover_lock == PAR_LOCK_AT_ATTACH ? "0 at attach" : "ignored",
             d, CGDisplayVendorNumber(d), CGDisplayModelNumber(d), degreesShown(d), gMp.why, mpBool(display, gMp.can, -1),
             mpBool(display, gMp.sensor, -1), gTestEdges ? "; edges from the test preference TestEdge" : "");
        gTimer = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0, 0, ^(CFRunLoopTimerRef t) {
            (void)t;
            runPolicy();
        });
        CFRunLoopAddTimer(CFRunLoopGetMain(), gTimer, kCFRunLoopDefaultMode);
        CGDisplayRegisterReconfigurationCallback(reconfigured, NULL);
        signal(SIGHUP, SIG_IGN);
        dispatch_source_t hup = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGHUP, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(hup, ^{
            reloadPrefs();
            readInputs();
            runPolicy();
        });
        dispatch_resume(hup);
        int token = 0;
        notify_register_dispatch(PREFS_NOTIFICATION, &token, dispatch_get_main_queue(), ^(int t) {
            (void)t;
            reloadPrefs();
            readInputs();
            runPolicy();
        });
        gRunning = 1;
        readInputs();
        runPolicy();
        CFRunLoopRun();
    }
    return 0;
}
