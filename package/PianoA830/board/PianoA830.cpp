// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoA830's IOKit side (k27 native kext, tooling/k27/kext/PianoA830.json):
//  - PianoA830, matched on the platform expert like PianoDwc3/PianoPower: probe() declines unless piano-gpu=N is given, so without
//    the boot argument nothing is mapped, read or published. N=1: the read-only survey (PianoA830Survey.cpp) around the driver's S0,
//    which only reads (pins, cmd-db, SMEM, the speed-bin fuse); N=2: the staged bring-up S1..S10 (Adreno830Start.cpp) in polled mode
//    (no interrupt is attached: the k27 KC has no armcpu_spi_enable/disable, the driver's weak defaults refuse the attach and every
//    stage takes its polled path, as the annibale profile is designed to), piano-gpu-stop=N stopping after S_N. Both run on a
//    kernel thread of their own, never in matching.
//  - AGXAccelerator: the macOS accelerator, an ordinary IOAccelerator subclass of IOGraphicsFamily. Its personality matches the
//    NativeAdrenoGpuPublished service the driver's S9 registers (only after S0..S8 passed) and carries MetalPluginName
//    NativeAdrenoMetal / MetalPluginClassName NativeAdrenoMetalDevice, the VM's plugin contract; opens go to the driver's
//    AGXDeviceUserClient (NativeAdrenoABI.h v2, open type 0x4147). Taken from macOS-DroidVM overlay/kext/DroidVMGpuKext.cpp (11ac316,
//    BSD-2-Clause-Patent) without its DroidVMGpuStarter and the virtio-gpu paths.
//  - The BootProgress hooks the driver calls (strong definitions; tooling/k27/coalesce-weak.py drops the driver's weak defaults):
//    status rows and stage results, the bring-up register trace (piano-gpu-trace) as "PianoA830: t ..." lines, all through a line
//    ring that a logger thread prints (the hooks run with interrupts off; see the ring below).
//  piano-gpu-delay=<s> (default 0): the survey or bring-up thread waits that long first (after the root is up, say), so a hang there
//  does not also stop the boot.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>
#include "PianoA830.h"
#include "Adreno830Status.h"
#include "Adreno830Device.h"
#include "NativeAdrenoPublish.h"

extern "C" void proc_selfname(char *, int);
extern "C" IOReturn native_adreno_new_user_client(IOService *, task_t, void *, uint32_t, OSDictionary *, IOUserClient **, unsigned);
extern "C" void native_adreno_note_property_read(const char *);
extern "C" int vsnprintf(char *, size_t, const char *, va_list) __attribute__((format(printf, 3, 0)));

namespace {
uint32_t gLevel;    // piano-gpu
uint32_t gTrace;    // the trace hook writes lines (piano-gpu=2 and piano-gpu-trace != 0)
char gRows[Adreno830StatusRows][Adreno830StatusRowBytes];

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t value = fallback;
  if (!PE_parse_boot_argn(name, &value, sizeof value)) value = fallback;
  return value;
}

const char *stageName(unsigned stage)
{
  static const char *const names[] = {"S0 inputs", "S1 power", "S2 secure CX", "S3 SMMU", "S4 firmware memory", "S5 GMU/HFI",
                                      "S6 CP", "S7 ZAP", "S8 fence", "S9 Metal", "S10 DCVS"};
  return stage < sizeof names / sizeof names[0] ? names[stage] : stage == A830SRunning ? "running" : "?";
}
}  // namespace

extern "C" void piano_a830_note(const char *format, ...)
{
  char line[256];
  va_list ap;
  va_start(ap, format);
  vsnprintf(line, sizeof line, format, ap);
  va_end(ap);
  IOLog("PianoA830: %s\n", line);
}

// ---- the line ring ----------------------------------------------------------------------------------------------------------------
// The driver calls its BootProgress hooks in contexts where nothing may be printed: the register trace inside its tracer lock with
// this CPU's interrupts off (A830TraceLock), the status rows under the status block's spin lock. Kernel printf/IOLog end in
// os_log_with_args, which must not run with interrupts off: piano-gpu=2's first run printed the trace's first line and hung the
// system right after it (2026-11-11 07:20). So the hooks only copy their line into this ring (a short spin lock with interrupts off,
// a bounded memcpy, nothing else), and the logger thread prints it with IOLog from plain thread context, polling every millisecond.
// A full ring drops lines and counts them; the logger says how many. The trace line of an access is in the ring before the access
// is made, and the logger runs on another CPU, so a hung access is still named on the console.
namespace {
enum : unsigned { RingLines = 2048, RingBytes = 160 };
struct LineRing {
  char (*text)[RingBytes];
  uint32_t head, tail, drops, lock;
};
LineRing gRing;

uintptr_t ringLock()
{
  const uintptr_t irq = uintptr_t(ml_set_interrupts_enabled(FALSE));
  while (__atomic_exchange_n(&gRing.lock, 1u, __ATOMIC_ACQUIRE)) {
  }
  return irq;
}
void ringUnlock(uintptr_t irq)
{
  __atomic_store_n(&gRing.lock, 0u, __ATOMIC_RELEASE);
  ml_set_interrupts_enabled(boolean_t(irq));
}

// Any context but a primary interrupt handler; never blocks, never prints.
void ringPush(const char *prefix, const char *text, size_t length)
{
  if (!gRing.text) return;
  const uintptr_t irq = ringLock();
  if (gRing.head - gRing.tail >= RingLines) {
    gRing.drops++;
  } else {
    char *line = gRing.text[gRing.head % RingLines];
    size_t n = 0;
    while (prefix[n] && n < RingBytes - 1) {
      line[n] = prefix[n];
      n++;
    }
    for (size_t i = 0; i < length && text[i] && n < RingBytes - 1; ++i) line[n++] = text[i];
    line[n] = 0;
    gRing.head++;
  }
  ringUnlock(irq);
}

bool ringPop(char *out, uint32_t *drops)
{
  const uintptr_t irq = ringLock();
  const bool any = gRing.tail != gRing.head;
  if (any) {
    memcpy(out, gRing.text[gRing.tail % RingLines], RingBytes);
    gRing.tail++;
  }
  *drops = gRing.drops;
  gRing.drops = 0;
  ringUnlock(irq);
  return any;
}

void logger(void *, wait_result_t)
{
  char line[RingBytes];
  for (;;) {
    uint32_t drops = 0;
    while (ringPop(line, &drops)) {
      if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
      IOLog("PianoA830: %s\n", line);
    }
    if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
    IOSleep(1);
  }
}
}  // namespace

// ---- BootProgress hooks (Adreno830Status.h): into the ring only ------------------------------------------------------------------
// The overlay rows: each row whose text changed (row 0 the summary, rows 1-3 the diagnostic page of piano-gpu-status=2).
extern "C" void boot_progress_gpu_rows(const char (*rows)[Adreno830StatusRowBytes], unsigned count)
{
  static const char *const prefixes[] = {"row0 ", "row1 ", "row2 ", "row3 "};
  for (unsigned i = 0; i < count && i < Adreno830StatusRows && i < 4; ++i) {
    if (!memcmp(gRows[i], rows[i], Adreno830StatusRowBytes)) continue;
    memcpy(gRows[i], rows[i], Adreno830StatusRowBytes);
    gRows[i][Adreno830StatusRowBytes - 1] = 0;
    ringPush(prefixes[i], gRows[i], Adreno830StatusRowBytes);
  }
}
extern "C" void boot_progress_display_status(const char *detail, uint64_t value)
{
  char text[96];
  snprintf(text, sizeof text, "%s %#llx", detail ? detail : "-", value);
  ringPush("status ", text, sizeof text);
}
// The register trace: called before each traced access, inside the tracer's lock with interrupts off.
extern "C" void boot_progress_trace(const char *text)
{
  if (gTrace && text) ringPush("t ", text, RingBytes);
}
extern "C" int boot_progress_trace_enabled(void) { return gTrace ? 1 : 0; }

extern "C" void piano_a830_report(IOReturn result)
{
  const Adreno830Status &s = adreno830_status;
  piano_a830_note("adreno830_start: %#x; stage %u (%s) state %u step %#x reason %u ioreturn %#x reg %#llx value %#llx expect %#llx",
                  result, s.stage, stageName(s.stage), s.state, s.step, s.reason, s.ioreturn, s.reg, s.value, s.expect);
  const Adreno830Failure *f = adreno830_last_failure();
  if (f && (f->stage || f->step || f->reason || f->ior))
    piano_a830_note("last failure: S%u step %#x reason %u ior %#x reg %#llx value %#llx expect %#llx", f->stage, f->step, f->reason,
                    f->ior, f->reg, f->value, f->expect);
  piano_a830_note("S0 facts: soc %u feature %#x speed-bin %#x profile %u (set %u) boot %u ms; aux0 %#llx aux1 %#llx", s.soc_id,
                  s.feature_code, s.speed_bin, s.profile, s.profile_set, s.boot_ms, s.aux[0], s.aux[1]);
  for (unsigned i = 0; i < A830StageCount; ++i)
    if (s.stage_us[i]) piano_a830_note("stage %s: %u us", stageName(i), s.stage_us[i]);
  for (unsigned i = 0; i < Adreno830StatusRows; ++i)
    if (s.rows[i][0]) piano_a830_note("row%u %.*s", i, int(Adreno830StatusRowBytes), s.rows[i]);
}

// ---- the starter ------------------------------------------------------------------------------------------------------------------
namespace {
void run(void *, wait_result_t)
{
  const uint32_t delay = bootArg("piano-gpu-delay", 0);
  if (delay) {
    piano_a830_note("piano-gpu-delay=%u: waiting %u s before the %s", delay, delay, gLevel == 1 ? "survey" : "bring-up");
    IOSleep(delay * 1000);
  }
  const uint32_t defaultMask = gLevel == 1 ? uint32_t(PianoA830SurveyDefault) : 0u;
  const uint32_t mask = bootArg("piano-gpu-survey", defaultMask);
  if (gLevel == 1) {
    piano_a830_note("piano-gpu=1: read-only survey (mask %#x) around the driver's S0; nothing is written, nothing is published", mask);
    piano_a830_survey(mask & PianoA830SurveyInputs);
    piano_a830_note("S0 (board inputs, read only): adreno830_start with piano-gpu-stop=0");
    piano_a830_report(piano_a830_board_start(1));
    piano_a830_survey(mask & ~uint32_t(PianoA830SurveyInputs));
    piano_a830_note("survey done (piano-gpu=1)");
    return;
  }
  piano_a830_note("piano-gpu=2: staged bring-up S1..S10, polled (piano-gpu-stop=%d, trace %u)", int(bootArg("piano-gpu-stop", ~0u)),
                  gTrace);
  if (mask) piano_a830_survey(mask);   // piano-gpu-survey=<mask>: the read-only survey first
  const IOReturn result = piano_a830_board_start(2);
  piano_a830_report(result);
  piano_a830_note("bring-up %s", result == kIOReturnSuccess ? "done: the GPU runs" : "stopped");
}
}  // namespace

class PianoA830 : public IOService {
  OSDeclareDefaultStructors(PianoA830)
public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoA830, IOService)

IOService *PianoA830::probe(IOService *provider, SInt32 *score)
{
  gLevel = bootArg("piano-gpu", 0);
  if (!gLevel || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoA830::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  static bool started;
  if (started) return true;
  started = true;
  if (gLevel > 2) {
    piano_a830_note("piano-gpu=%u: unknown level (1 survey, 2 bring-up); nothing done", gLevel);
    return true;
  }
  gTrace = gLevel >= 2 && bootArg("piano-gpu-trace", 1) != 0;
  setProperty("piano-gpu", gLevel, 32);
  piano_a830_note("start: piano-gpu=%u (A830 chip %#x, k27 native kext, polled)", gLevel, 0x44050001u);
  thread_t thread = nullptr;
  gRing.text = static_cast<char (*)[RingBytes]>(IOMallocZero(size_t(RingLines) * RingBytes));
  if (!gRing.text || kernel_thread_start(logger, nullptr, &thread) != KERN_SUCCESS) {
    piano_a830_note("FAILED: no line ring or logger thread (%u KiB); the trace stays off", RingLines * RingBytes / 1024);
    if (gRing.text) IOFree(gRing.text, size_t(RingLines) * RingBytes);
    gRing.text = nullptr;
    gTrace = 0;
  } else {
    thread_deallocate(thread);
    piano_a830_note("line ring %u x %u bytes, logger thread running", RingLines, RingBytes);
  }
  thread = nullptr;
  if (kernel_thread_start(run, nullptr, &thread) != KERN_SUCCESS) {
    piano_a830_note("FAILED: no kernel thread");
    return true;
  }
  thread_deallocate(thread);
  return true;
}

// ---- the macOS accelerator --------------------------------------------------------------------------------------------------------
// Metal's discovery (MTLIOAccelServiceGlobalContext) watches IOServiceMatching("IOAccelerator"); IOGPU's +[IOGPUMetalDevice
// registerAcceleratorService:] loads <MetalPluginName>.bundle from /Library/GPUBundles (trusted by gpu_bundle_find_trusted) and takes
// MetalPluginClassName if it subclasses IOGPUMetalDevice. Named after Apple's accelerator, as on iOS and in the VM, so sandbox profiles
// that name the class accept the plugin's open.
class AGXAccelerator : public IOAccelerator {
  OSDeclareDefaultStructors(AGXAccelerator)
public:
  bool start(IOService *provider) override;
  IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) override;
  OSObject *copyProperty(const OSSymbol *key) const override;
  OSObject *copyProperty(const OSString *key) const override;
  OSObject *copyProperty(const char *key) const override;
};

OSDefineMetaClassAndStructors(AGXAccelerator, IOAccelerator)

bool AGXAccelerator::start(IOService *provider)
{
  if (!IOAccelerator::start(provider)) return false;
  // The processes an XNU sandbox-grant patch would let open the user client (inert without one; NativeAdrenoPublish.h, macOS list).
  setProperty("NativeSandboxGrantProcess", NativeAdrenoSandboxGrantList);
  registerService();
  piano_a830_note("Metal accelerator AGXAccelerator on %s (MetalPluginName NativeAdrenoMetal)", provider->getName());
  return true;
}

IOReturn AGXAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
  const unsigned limit = bootArg("piano-gpu-metal-opens", 0);   // connections ever granted to the plugin (0 = no limit)
  const IOReturn status = native_adreno_new_user_client(this, owningTask, securityID, type, properties, handler, limit);
  static unsigned logged;
  if (logged < 64) {
    logged++;
    char name[20] = {};
    proc_selfname(name, sizeof name);
    piano_a830_note("%s opens the accelerator, type %#x: %#x", name, unsigned(type), status);
  }
  return status;
}

static void notePropertyRead(const char *key)
{
  native_adreno_note_property_read(key);
  static unsigned logged;
  if (key && !strcmp(key, "MetalPluginName") && logged < 32) {
    logged++;
    char name[20] = {};
    proc_selfname(name, sizeof name);
    piano_a830_note("%s reads MetalPluginName", name);
  }
}

OSObject *AGXAccelerator::copyProperty(const OSSymbol *key) const
{
  notePropertyRead(key ? key->getCStringNoCopy() : nullptr);
  return IORegistryEntry::copyProperty(key);
}

OSObject *AGXAccelerator::copyProperty(const OSString *key) const
{
  notePropertyRead(key ? key->getCStringNoCopy() : nullptr);
  return IORegistryEntry::copyProperty(key);
}

OSObject *AGXAccelerator::copyProperty(const char *key) const
{
  notePropertyRead(key);
  return IORegistryEntry::copyProperty(key);
}
