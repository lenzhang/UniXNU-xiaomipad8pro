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
//  piano-gpu-hang-ms=<ms> (default 0: 2000): the time without progress the fence and recovery watchdogs call a hang (1000..60000).
//  The session's GPU events (NativeAdrenoPublish.cpp: failed calls, refused opens, recoveries and their snapshot, page faults, the
//  device lost, the clients) are printed as "PianoA830: gpu ..." through the same ring, whatever piano-gpu-trace says.
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
#include "PianoA830Rows.h"
#include "A830Idle.h"
#include "Adreno830Status.h"
#include "Adreno830Device.h"
#include "NativeAdrenoPublish.h"
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>

extern "C" const Adreno830Board *piano_a830_board(void);

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

// The console mailbox canary (2026-11-11): k27 0055/0058 mirror the console into pstore-console=<addr> (cvp@9cb00000, outside XNU's
// DRAM). Across a reset its DRAM decays (1 -> 0, worse when hot), which a dump shows as a damaged signature; during the run nothing
// decays, so a signature word that changes while XNU runs is a real overwrite (by a device, a GPU stream reaching memory untranslated
// above all: piano_a830_smmu_routes). The logger reads the word once a second through its own read-only uncached mapping.
struct Canary {
  IOMemoryMap *map;
  volatile const uint32_t *word;
  uint64_t phys;
  uint32_t expected, changes;
};
Canary gCanary;

void canaryStart()
{
  uint64_t base = 0;
  if (!PE_parse_boot_argn("pstore-console", &base, sizeof base) || !base || (base & 0xfff)) return;
  IODeviceMemory *memory = IODeviceMemory::withRange(base, 0x1000);
  gCanary.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
  if (memory) memory->release();
  if (!gCanary.map) return;
  gCanary.word = reinterpret_cast<volatile const uint32_t *>(gCanary.map->getVirtualAddress());
  gCanary.phys = base;
  gCanary.expected = *gCanary.word;
  IOLog("PianoA830: console mailbox canary at %#llx: signature %#x (watched once a second; a change while XNU runs is an overwrite)\n",
        base, gCanary.expected);
}

void canaryCheck()
{
  if (!gCanary.word) return;
  const uint32_t now = *gCanary.word;
  if (now == gCanary.expected) return;
  gCanary.changes++;
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  IOLog("PianoA830: CONSOLE MAILBOX OVERWRITTEN at %#llx while running (t=%llu s, change %u): signature %#x -> %#x; GPU %s\n", gCanary.phys,
        ns / 1000000000ull, gCanary.changes, gCanary.expected, now, adreno830_running() ? "running" : "not running");
  gCanary.expected = now;
}

void flushHeldRows();   // the status rows held back (below)

void logger(void *, wait_result_t)
{
  char line[RingBytes];
  canaryStart();
  for (unsigned tick = 0;; ++tick) {
    uint32_t drops = 0;
    while (ringPop(line, &drops)) {
      if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
      IOLog("PianoA830: %s\n", line);
    }
    if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
    if (tick % 1000 == 0) {
      canaryCheck();
      flushHeldRows();
    }
    IOSleep(1);
  }
}
}  // namespace

// ---- BootProgress hooks (Adreno830Status.h): into the ring only ------------------------------------------------------------------
// The overlay rows: each row whose text changed (row 0 the summary, rows 1-3 the diagnostic page of piano-gpu-status=2). Once the GPU
// runs ("GPU RUN ..."), the rows change every second (submissions, clock, temperature, the queue figures): the login run printed two
// of them a second for the whole session. From then on a row is printed when it changed and 10 s have passed since it was last, or at
// once when row 0's state (its second word: RUN, LOST, ...) or its fault or hang count changed; a row held back is printed by the
// logger thread when its 10 s are up, so the last state of an idle GPU still shows. Before the GPU runs (the bring-up) every change
// is printed at once, as before. Under the ring's lock (the hook runs under the status block's spin lock, the logger beside it).
namespace {
enum : unsigned { RowSlots = Adreno830StatusRows < 4 ? Adreno830StatusRows : 4 };
const char *const kRowPrefixes[] = {"row0 ", "row1 ", "row2 ", "row3 "};
char gRowHeld[RowSlots][Adreno830StatusRowBytes];
bool gRowPending[RowSlots];
uint64_t gRowShownAt[RowSlots];
uint64_t gRowEvery;   // 10 s in mach_absolute_time units (set by start())

}  // namespace

extern "C" void boot_progress_gpu_rows(const char (*rows)[Adreno830StatusRowBytes], unsigned count)
{
  char out[RowSlots][Adreno830StatusRowBytes];
  bool show[RowSlots] = {};
  const bool running = count && !strncmp(rows[0], "GPU RUN ", 8);
  const uint64_t now = mach_absolute_time();
  const uintptr_t irq = ringLock();
  for (unsigned i = 0; i < count && i < RowSlots; ++i) {
    if (!memcmp(gRows[i], rows[i], Adreno830StatusRowBytes)) {
      gRowPending[i] = false;   // back to what the console shows
      continue;
    }
    bool due = !running || !gRowEvery || now - gRowShownAt[i] >= gRowEvery;
    if (!due && i == 0) due = PianoA830RowZeroStateChanged(gRows[0], rows[0], Adreno830StatusRowBytes);
    if (due) {
      memcpy(gRows[i], rows[i], Adreno830StatusRowBytes);
      gRows[i][Adreno830StatusRowBytes - 1] = 0;
      memcpy(out[i], gRows[i], Adreno830StatusRowBytes);
      gRowShownAt[i] = now;
      gRowPending[i] = false;
      show[i] = true;
    } else {
      memcpy(gRowHeld[i], rows[i], Adreno830StatusRowBytes);
      gRowHeld[i][Adreno830StatusRowBytes - 1] = 0;
      gRowPending[i] = true;
    }
  }
  ringUnlock(irq);
  for (unsigned i = 0; i < RowSlots; ++i)
    if (show[i]) ringPush(kRowPrefixes[i], out[i], Adreno830StatusRowBytes);
}

namespace {
// The logger's once-a-second look: a row held back for 10 s is printed now.
void flushHeldRows()
{
  char out[RowSlots][Adreno830StatusRowBytes];
  bool show[RowSlots] = {};
  const uint64_t now = mach_absolute_time();
  const uintptr_t irq = ringLock();
  for (unsigned i = 0; i < RowSlots; ++i) {
    if (!gRowPending[i] || (gRowEvery && now - gRowShownAt[i] < gRowEvery)) continue;
    memcpy(gRows[i], gRowHeld[i], Adreno830StatusRowBytes);
    memcpy(out[i], gRowHeld[i], Adreno830StatusRowBytes);
    gRowShownAt[i] = now;
    gRowPending[i] = false;
    show[i] = true;
  }
  ringUnlock(irq);
  for (unsigned i = 0; i < RowSlots; ++i)
    if (show[i]) ringPush(kRowPrefixes[i], out[i], Adreno830StatusRowBytes);
}
}  // namespace

// UniXNU-piano's event lines (NativeAdrenoPublish.cpp, Adreno830Status.h): whatever piano-gpu-trace and piano-gpu-status say.
extern "C" void boot_progress_gpu_event(const char *text)
{
  if (text) ringPush("gpu ", text, RingBytes);
}
// piano-gpu-hang-ms=<ms>: the time without progress (fence, ring read pointer, BR's IB1) the fence and recovery watchdogs call a hang;
// absent or 0: their 2000 ms. Kept in 1000..60000.
extern "C" uint32_t adreno830_board_hang_ms(void)
{
  const uint32_t ms = bootArg("piano-gpu-hang-ms", 0);
  if (!ms) return 0;
  return ms < 1000 ? 1000 : ms > 60000 ? 60000 : ms;
}

namespace {
__attribute__((format(printf, 1, 2))) void gpuLine(const char *format, ...)
{
  char line[RingBytes];
  va_list ap;
  va_start(ap, format);
  vsnprintf(line, sizeof line, format, ap);
  va_end(ap);
  boot_progress_gpu_event(line);
}
}  // namespace

// The GPU's idle policy (adreno830/A830Idle.h):
//   piano-gpu-idle=0|1|2 (default 1): 0 as before; 1 the DCVS floor (no wake boost, the idle window); 2 + the idle votes and slumber;
//   piano-gpu-idle-ms=<ms> (1000, 100..60000): quiet this long -> the idle votes and slumber;
//   piano-gpu-idle-slumber=0|1 (1): with 2, also P1's full slumber (0: the votes alone);
//   piano-gpu-idle-ddr=<level> (the scanout's: twice the panel's read rate at 120 Hz, from the boot framebuffer): the host DDR bus level
//       while idle, never above the level held;
//   piano-gpu-idle-rails=<vlvl> (0x140, RPMh NOM_L1: the lit panel's CX through MMCX; 0 keeps them): the host CX/MX level while
//       idle, never above the level held.
extern "C" void adreno830_board_idle(A830IdleConfig *config)
{
  if (!config) return;
  config->mode = uint8_t(bootArg("piano-gpu-idle", 1) > 2 ? 2 : bootArg("piano-gpu-idle", 1));
  const uint32_t ms = bootArg("piano-gpu-idle-ms", 1000);
  config->idleMs = ms < 100 ? 100 : ms > 60000 ? 60000 : ms;
  config->slumber = bootArg("piano-gpu-idle-slumber", 1) != 0;
  config->railVlvl = bootArg("piano-gpu-idle-rails", 0x140);
  // The scanout: the GOP framebuffer the panel reads from DDR at ABL's 120 Hz (NativeGopDisplay copies frames into it).
  const Adreno830Board *board = piano_a830_board();
  PE_Video video = {};
  uint32_t width = 3200, height = 2136, scanout = 0;
  if (IOService::getPlatform() && IOService::getPlatform()->getConsoleInfo(&video) == kIOReturnSuccess && video.v_width && video.v_height) {
    width = uint32_t(video.v_width);
    height = uint32_t(video.v_height);
  }
  uint8_t floor = board && board->ddr_kbps ? A830IdleScanoutLevel(board->ddr_kbps, board->ddr_levels, width, height, 4, 120, 2000, &scanout) : 5;
  const uint32_t asked = bootArg("piano-gpu-idle-ddr", 0xff);
  if (asked != 0xff && board && asked < board->ddr_levels) floor = uint8_t(asked);
  config->ddrLevel = floor;
  gpuLine("idle: piano-gpu-idle=%u (slumber %u, after %u ms); idle DDR level %u (%u MB/s; %ux%u scanout %u MB/s at 120 Hz), "
          "rails vlvl %#x", config->mode, config->mode >= 2 && config->slumber ? 1u : 0u, config->idleMs, floor,
          board && board->ddr_kbps && floor < board->ddr_levels ? board->ddr_kbps[floor] / 1000 : 0, width, height, scanout / 1000,
          config->railVlvl);
}

extern "C" void adreno830_board_idle_event(const A830IdleEvent *e)
{
  if (!e) return;
  switch (e->kind) {
  case A830IdleEventDown:
    gpuLine("idle: votes down after %llu ms quiet (#%u): DDR level %u (%u MB/s), CX %#x MX %#x", (unsigned long long)e->ms, e->count,
            e->ddrLevel, e->ddrKbps / 1000, e->cxVlvl, e->mxVlvl);
    break;
  case A830IdleEventUp:
    gpuLine("idle: votes up after %llu ms down (#%u): DDR level %u (%u MB/s), CX %#x MX %#x", (unsigned long long)e->ms, e->count,
            e->ddrLevel, e->ddrKbps / 1000, e->cxVlvl, e->mxVlvl);
    break;
  case A830IdleEventSlumber:
    gpuLine("idle -> slumber after %llu ms quiet (#%u, entry %u us); votes CX %#x MX %#x DDR level %u (%u MB/s)",
            (unsigned long long)e->ms, e->count, e->us, e->cxVlvl, e->mxVlvl, e->ddrLevel, e->ddrKbps / 1000);
    break;
  case A830IdleEventWake:
    gpuLine("wake after %llu ms asleep (#%u): the wake took %u.%03u ms (what the first submission waited)", (unsigned long long)e->ms,
            e->count, e->us / 1000, e->us % 1000);
    break;
  case A830IdleEventFailed:
    gpuLine("idle: a host vote failed (rpmh %d, #%u): no idle votes for this boot; DDR level %u CX %#x MX %#x", e->result, e->count,
            e->ddrLevel, e->cxVlvl, e->mxVlvl);
    break;
  case A830IdleEventSlumberOff:
    gpuLine("idle: a slumber or wake failed (step %#x, %#x, %u failures): no slumber for this boot", e->us, unsigned(e->result), e->count);
    break;
  case A830IdleEventFloor:
    if (!e->result)
      gpuLine("idle: the boot DDR vote went to the display's floor, level %u (%u MB/s); the GMU votes the GPU's own", e->ddrLevel,
              e->ddrKbps / 1000);
    else
      gpuLine("idle: the boot DDR vote could not drop (rpmh %d): level %u (%u MB/s) stays", e->result, e->ddrLevel, e->ddrKbps / 1000);
    break;
  case A830IdleEventSummary:
    gpuLine("idle last %llu s: votes down %u, up %u, down %u.%u %% of the time; slumbers %u, wakes %u, longest wake %u.%03u ms",
            (unsigned long long)(e->ms / 1000), e->downs, e->ups, e->downPermille / 10, e->downPermille % 10, e->slumbers, e->wakes,
            e->maxWakeUs / 1000, e->maxWakeUs % 1000);
    break;
  default:
    break;
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
  piano_a830_note("bring-up %s", adreno830_running() ? "done: the GPU runs" : result == kIOReturnSuccess ? "stopped (piano-gpu-stop)" : "failed");
  // Where the GPU-side streams go, once S3 programmed the SMMU, and again once the session's Metal clients are up. Only while the GPU
  // stays on: piano-gpu-slumber would collapse CX, and an unclocked SMMU read can hang the bus.
  const Adreno830Board *board = piano_a830_board();
  if (result == kIOReturnSuccess && adreno830_status.stage >= A830SSmmu && board && !board->knobs.slumber) {
    piano_a830_smmu_routes("after the bring-up");
    if (adreno830_running()) {
      IOSleep(60000);
      piano_a830_smmu_routes("60 s later");
    }
  }
}
}  // namespace

// The display's fence rule without a link dependency either way (the KC resolves a native kext's imports only from the natives linked
// before it, and NativeGopDisplay comes first): NativeGopDisplay finds this registered service by class (serviceMatching("PianoA830"))
// and calls callPlatformFunction("native-adreno-fences", false, uint32_t *submitted, uint32_t *retired), which answers with
// native_adreno_fences() (NativeAdrenoPublish.cpp): kIOReturnSuccess with both, kIOReturnNotReady before S9 or after a device loss.
// Lock-free, any thread context.
extern "C" bool native_adreno_fences(uint32_t *submitted, uint32_t *retired);

class PianoA830 : public IOService {
  OSDeclareDefaultStructors(PianoA830)
public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                void *param4) override;
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
  nanoseconds_to_absolutetime(10ull * 1000000000ull, &gRowEvery);
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
  if (gLevel >= 2) registerService();   // for the display's fence lookup (callPlatformFunction below)
  return true;
}

IOReturn PianoA830::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                         void *param4)
{
  if (functionName && functionName->isEqualTo("native-adreno-fences")) {
    if (!param1 || !param2) return kIOReturnBadArgument;
    return native_adreno_fences(static_cast<uint32_t *>(param1), static_cast<uint32_t *>(param2)) ? kIOReturnSuccess : kIOReturnNotReady;
  }
  return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
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
  // Every Metal process opens twice (Turnip's enumeration, then its device) and a login session has a few dozen of them: the first 160
  // opens are logged (64 before 2026-11-11, which a session after login could use up); a refused GPU open also gets the publish
  // shell's "gpu open refused" line with its reason.
  static unsigned logged;
  if (logged < 160) {
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
  if (key && !strcmp(key, "MetalPluginName") && logged < 96) {   // 32 before 2026-11-11 (a session after login reads more)
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
