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
//    AGXDeviceUserClient (NativeAdrenoABI.h v2, open type 0x4147). Without a starter class or virtio-gpu paths (provenance:
//    ../UPSTREAM).
//  - The BootProgress hooks the driver calls (strong definitions; tooling/k27/coalesce-weak.py drops the driver's weak defaults):
//    status rows and stage results, the bring-up register trace (piano-gpu-trace) as "PianoA830: t ..." lines, all through a line
//    ring that a logger thread prints (the hooks run with interrupts off; see the ring below). piano-gpu-trace=3 prints the trace
//    synchronously instead, from the thread that makes the access, before it (see "the synchronous trace" below).
//  piano-gpu-delay=<s> (default 0): the survey or bring-up thread waits that long first (after the root is up, say), so a hang there
//  does not also stop the boot.
//  piano-gpu-hang-ms=<ms> (default 0: 2000): the time without progress the fence and recovery watchdogs call a hang (1000..60000).
//  The session's GPU events (NativeAdrenoPublish.cpp: failed calls, refused opens, recoveries and their snapshot, page faults, the
//  device lost, the clients) are printed as "PianoA830: gpu ..." through the same ring, whatever piano-gpu-trace says.
//  2026-11-11 (docs/drivers/gpu.md "System suspend", "The LLCC island", "Ceiling slots"):
//  - the LLCC ISLAND1 slice (PianoLlccIsland.h): read at the start of every GPU boot (read only), programmed with
//    piano-llcc-island=1 at piano-gpu=2, asked through "piano-llcc-island-ready" / piano_a830_llcc_island_ready();
//  - piano-gpu-suspend=1 (A830Suspend.h): in the power tree once the GPU runs; kOff powers it down (the full slumber), the platform
//    sleep action lowers its host votes, the wake action puts them back, kOn wakes it;
//  - "piano-gpu-ceiling" with one slot per caller (PianoGpuCeiling.h), the lowest wins; piano-gpu-ceiling-clients.
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
#include "A830Dcvs.h"
#include "Adreno830Status.h"
#include "Adreno830Device.h"
#include "NativeAdrenoPublish.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#define PIANO_SCM_LOCK_KERNEL 1
#include <PianoScmLock.h>
#include <PianoPowerContract.h>
#include "A830Suspend.h"
#include "A830PowerState.h"
#include "PianoLlccIsland.h"
#include "PianoGpuCeiling.h"

extern "C" const Adreno830Board *piano_a830_board(void);

extern "C" void proc_selfname(char *, int);
extern "C" IOReturn native_adreno_new_user_client(IOService *, task_t, void *, uint32_t, OSDictionary *, IOUserClient **, unsigned);
extern "C" void native_adreno_note_property_read(const char *);
extern "C" int vsnprintf(char *, size_t, const char *, va_list) __attribute__((format(printf, 3, 0)));
extern "C" int get_preemption_level(void);

namespace {
uint32_t gLevel;    // piano-gpu
uint32_t gTrace;    // the trace hook writes lines (piano-gpu=2 and piano-gpu-trace != 0)
uint32_t gTraceSync;   // piano-gpu-trace=3: the trace printed synchronously (below)
thread_t gRunThread;   // the starter thread (run below); piano-gpu-trace=3's heartbeat reports its IOLog in progress
uint64_t gRunNoteSince;   // mach_absolute_time at which the starter entered piano_a830_note's IOLog (0: not inside one)
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
  const bool starter = gTraceSync && gRunThread && current_thread() == gRunThread;
  if (starter) __atomic_store_n(&gRunNoteSince, mach_absolute_time(), __ATOMIC_RELAXED);
  IOLog("PianoA830: %s\n", line);
  if (starter) __atomic_store_n(&gRunNoteSince, 0ull, __ATOMIC_RELAXED);
}

// ---- the line ring ----------------------------------------------------------------------------------------------------------------
// The driver calls its BootProgress hooks in contexts where nothing may be printed: the register trace inside its tracer lock with
// this CPU's interrupts off (A830TraceLock), the status rows under the status block's spin lock. Kernel printf/IOLog end in
// os_log_with_args, which must not run with interrupts off: piano-gpu=2's first run printed the trace's first line and hung the
// system right after it (2026-11-11 07:20). So the hooks only copy their line into this ring (a short spin lock with interrupts off,
// a bounded memcpy, nothing else), and the logger thread prints it with IOLog from plain thread context, polling every millisecond.
// A full ring drops lines and counts them; the logger says how many. The trace line of an access is in the ring before the access
// is made, but the console shows it only once the logger has printed every line before it: with -v each console line is drawn
// (tens of ms), so the logger can be many seconds and the whole bring-up behind the driver (2026-11-11 "power1": S1's lines 5 s
// after adreno830_start had returned, docs/forensics-2026-11-11.md). The last printed line is then where the logger was, not the
// access in flight; piano-gpu-trace=3 (below) prints each line before its access instead.
namespace {
enum : unsigned { RingLines = 2048, RingBytes = 160 };
struct LineRing {
  char (*text)[RingBytes];
  uint64_t *stamp;   // piano-gpu-trace=3: mach_absolute_time of each line's push (else null)
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
    if (gRing.stamp) gRing.stamp[gRing.head % RingLines] = mach_absolute_time();
    gRing.head++;
  }
  ringUnlock(irq);
}

bool ringPop(char *out, uint32_t *drops, uint64_t *stamp = nullptr)
{
  const uintptr_t irq = ringLock();
  const bool any = gRing.tail != gRing.head;
  if (any) {
    memcpy(out, gRing.text[gRing.tail % RingLines], RingBytes);
    if (stamp) *stamp = gRing.stamp ? gRing.stamp[gRing.tail % RingLines] : 0;
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

// ---- the synchronous trace (piano-gpu-trace=3, 2026-11-11) ------------------------------------------------------------------------
// Every trace line is printed by the thread that pushed it, after the tracer's lock is given back and before the access the line
// names: Adreno830Start.cpp's traceAccess calls boot_progress_trace_sync once its line is in the ring, and that drains the ring
// (this line and every line before it) with IOLog. So the console's last "t" line is the access in flight or the one just made,
// whatever the logger was doing. Each line carries the uptime of its push: "PianoA830: [27.612] t g1.7 GPUCC w 3d99010 8000001".
// Printing only happens in thread context with interrupts on and preemption enabled (never with interrupts off: the 07:20 hang of
// 2026-11-11); a line pushed anywhere else (a caller that masked interrupts) stays in the ring for the logger, as with 1, and the
// heartbeat counts it. The driver itself gets piano-gpu-trace=1 (PianoA830Board.cpp): runs are counted, so a poll costs a few lines.
// The cost is the console's: with -v every line is drawn into the boot framebuffer, so the bring-up takes tens of seconds instead of
// one, and the time between two accesses grows to milliseconds (a sequence that only fails when it is slow fails here, see the
// forensics page). The driver's polls are all bounded already (iteration or time budgets); what cannot be bounded is one access
// or one SMC that never returns, and the heartbeat names that one instead of a guard.
// The heartbeat (the logger, once a second; every 30 s from 120 s after the bring-up returned): the uptime, the lines pushed and
// printed, what the bring-up thread is doing (printing a line, or past it: its access) and the starter thread's phase and whether it
// is inside an IOLog, each with how long. A stall then shows whether the rest of the system still ran, and where the thread stopped.
IOLock *gPrintLock;            // one printer at a time (the bring-up thread or the logger), so the lines keep their order
uint32_t gPrinted, gPrintedSync, gSyncSkipped;   // (lines pushed: gRing.head)
uint32_t gSyncState;           // 0 none yet, 1 printing a line, 2 past its print (the access), 3 adreno830_start returned
uint64_t gSyncSince;           // mach_absolute_time of the state's start
uint32_t gSyncLineNo;          // the number of the line the bring-up thread printed last
char gSyncLast[RingBytes];     // and its text (copied under gSyncSeq: even = stable)
uint32_t gSyncSeq;
uint32_t gRunPhase;            // RunPhase
uint64_t gRunSince;            // the phase's start
enum RunPhase : uint32_t { RunInit, RunDelay, RunSurvey, RunBringUp, RunReport, RunRoutes, RunWait, RunDone };
const char *const kRunPhases[] = {"starting", "piano-gpu-delay", "survey", "bring-up", "report", "SMMU routes", "waiting 60 s", "done"};

uint64_t msOf(uint64_t abs)
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(abs, &ns);
  return ns / 1000000ull;
}

void runPhase(uint32_t phase)
{
  __atomic_store_n(&gRunSince, mach_absolute_time(), __ATOMIC_RELAXED);
  __atomic_store_n(&gRunPhase, phase, __ATOMIC_RELEASE);
}

void printLine(const char *line, uint64_t stamp)
{
  if (gTraceSync && stamp) {
    const uint64_t ms = msOf(stamp);
    IOLog("PianoA830: [%llu.%03llu] %s\n", ms / 1000, ms % 1000, line);
  } else {
    IOLog("PianoA830: %s\n", line);
  }
}

// Drains the ring with IOLog; the caller holds gPrintLock. bringUp: the bring-up thread's drain (its state and last line kept).
void drainRing(bool bringUp)
{
  char line[RingBytes];
  uint32_t drops = 0;
  uint64_t stamp = 0;
  while (ringPop(line, &drops, &stamp)) {
    if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
    const uint32_t number = __atomic_add_fetch(&gPrinted, 1u, __ATOMIC_RELAXED);
    if (bringUp) {
      __atomic_add_fetch(&gSyncSeq, 1u, __ATOMIC_ACQ_REL);   // odd: being written
      memcpy(gSyncLast, line, RingBytes);
      gSyncLineNo = number;
      __atomic_add_fetch(&gSyncSeq, 1u, __ATOMIC_ACQ_REL);
      __atomic_store_n(&gSyncSince, mach_absolute_time(), __ATOMIC_RELAXED);
      __atomic_store_n(&gSyncState, 1u, __ATOMIC_RELEASE);
      __atomic_add_fetch(&gPrintedSync, 1u, __ATOMIC_RELAXED);
    }
    printLine(line, stamp);
  }
  if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
}

void heartbeat()
{
  const uint64_t now = mach_absolute_time();
  char last[RingBytes] = "-";
  uint32_t lineNo = 0;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {   // the bring-up thread may be copying it right now
    const uint32_t before = __atomic_load_n(&gSyncSeq, __ATOMIC_ACQUIRE);
    if (before & 1) continue;
    memcpy(last, gSyncLast, RingBytes);
    lineNo = gSyncLineNo;
    if (__atomic_load_n(&gSyncSeq, __ATOMIC_ACQUIRE) == before) break;
    strlcpy(last, "(changing)", sizeof last);
  }
  last[RingBytes - 1] = 0;
  const uint32_t state = __atomic_load_n(&gSyncState, __ATOMIC_ACQUIRE);
  const uint64_t stateMs = msOf(now - __atomic_load_n(&gSyncSince, __ATOMIC_RELAXED));
  const uint32_t phase = __atomic_load_n(&gRunPhase, __ATOMIC_ACQUIRE);
  const uint64_t noteSince = __atomic_load_n(&gRunNoteSince, __ATOMIC_RELAXED);
  const uint64_t ms = msOf(now);
  const uint32_t head = __atomic_load_n(&gRing.head, __ATOMIC_RELAXED), tail = __atomic_load_n(&gRing.tail, __ATOMIC_RELAXED);
  char what[RingBytes + 64], note[48] = "";
  if (state == 1) snprintf(what, sizeof what, "printing line %u for %llu ms (%s)", lineNo, stateMs, last);
  else if (state == 2) snprintf(what, sizeof what, "past line %u for %llu ms: its access (%s)", lineNo, stateMs, last);
  else if (state == 3) snprintf(what, sizeof what, "adreno830_start returned %llu ms ago", stateMs);
  else snprintf(what, sizeof what, "no line printed yet");
  if (noteSince) snprintf(note, sizeof note, ", inside IOLog for %llu ms", msOf(now - noteSince));
  IOLog("PianoA830: hb %llu.%03llu s: bring-up thread %s; starter: %s for %llu ms%s; lines pushed %u, printed %u (%u by the bring-up "
        "thread), sync prints skipped %u, backlog %u\n", ms / 1000, ms % 1000, what,
        phase < sizeof kRunPhases / sizeof kRunPhases[0] ? kRunPhases[phase] : "?",
        msOf(now - __atomic_load_n(&gRunSince, __ATOMIC_RELAXED)), note, head, __atomic_load_n(&gPrinted, __ATOMIC_RELAXED),
        __atomic_load_n(&gPrintedSync, __ATOMIC_RELAXED), __atomic_load_n(&gSyncSkipped, __ATOMIC_RELAXED), head - tail);
}

void logger(void *, wait_result_t)
{
  char line[RingBytes];
  canaryStart();
  uint64_t second = 0, lastBeat = 0;
  nanoseconds_to_absolutetime(1000000000ull, &second);
  for (unsigned tick = 0;; ++tick) {
    if (gTraceSync && gPrintLock) {
      // Never wait for the bring-up thread's print: if it is stuck inside IOLog, the heartbeat must still come.
      if (IOLockTryLock(gPrintLock)) {
        drainRing(false);
        IOLockUnlock(gPrintLock);
      }
      const uint64_t now = mach_absolute_time();
      const bool late = __atomic_load_n(&gSyncState, __ATOMIC_ACQUIRE) == 3 && now - gSyncSince > 120 * second;
      if (now - lastBeat >= (late ? 30 * second : second)) {
        lastBeat = now;
        heartbeat();
      }
    } else {
      uint32_t drops = 0;
      while (ringPop(line, &drops)) {
        if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
        IOLog("PianoA830: %s\n", line);
      }
      if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
    }
    if (tick % 1000 == 0) {
      canaryCheck();
      flushHeldRows();
    }
    IOSleep(1);
  }
}
}  // namespace

// piano-gpu-trace=3: called by traceAccess (Adreno830Start.cpp) after the tracer's lock, before the access (see above).
extern "C" void boot_progress_trace_sync(void)
{
  if (!gTraceSync || !gPrintLock || !gRing.text) return;
  if (!ml_get_interrupts_enabled() || ml_at_interrupt_context() || get_preemption_level() != 0) {
    __atomic_add_fetch(&gSyncSkipped, 1u, __ATOMIC_RELAXED);   // the logger prints it
    return;
  }
  IOLockLock(gPrintLock);
  drainRing(true);
  __atomic_store_n(&gSyncSince, mach_absolute_time(), __ATOMIC_RELAXED);
  __atomic_store_n(&gSyncState, 2u, __ATOMIC_RELEASE);
  IOLockUnlock(gPrintLock);
}

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

// The kernel-wide secure-call lock (package/include/PianoScmLock.h, docs/scm-lock.md). PianoA830 owns the word, as it owns the DRV-2
// word: it links before every other user, which import this single copy (their descriptors name com.unixnu.driver.PianoA830 as a
// library). Its own calls take it through adreno830_board_scm_hold: around every yielding request of the driver's Sm8750Scm (its
// ops->lock/unlock, after/before its SCM mutex) and once around the ZAP's PAS 13 sequence (init_image .. auth_and_reset).
extern "C" volatile uint32_t sm8750_scm_owner;
volatile uint32_t sm8750_scm_owner = 0;
namespace {
PIANO_SCM_KERNEL_LOCK(gScmLock, PianoScmTagA830, "PianoA830");
}  // namespace
extern "C" void adreno830_board_scm_hold(int take)
{
  if (take) piano_scm_kernel_take(&gScmLock);
  else piano_scm_kernel_give(&gScmLock);
}

// The WCN7860 PMU's shared rails (package/include/PianoWcnRails.h, docs/drivers/bluetooth.md §7.1): the state and lock words of the
// two radios' drivers (PianoWifi, PianoBluetooth), kept here for the same reason as the words above: PianoA830 links before both,
// which import this single copy. PianoA830 itself never touches them.
extern "C" volatile uint32_t piano_wcn_rails_state;
volatile uint32_t piano_wcn_rails_state = 0;
extern "C" volatile uint32_t piano_wcn_pmu_owner;
volatile uint32_t piano_wcn_pmu_owner = 0;

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

// The display's DDR floor level as the idle policy computed it (5 on piano): the suspend keeps it while the panel still scans.
namespace {
uint8_t gDisplayFloorLevel = 5;
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
  gDisplayFloorLevel = floor;   // the system suspend's display floor too (A830Suspend.h)
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

// ---- the LLCC island slice (PianoLlccIsland.h) -------------------------------------------------------------------------------------
// Read at the start of every GPU boot (read only), programmed with piano-llcc-island=1 at piano-gpu=2, asked by others through
// "piano-llcc-island-ready" (PianoPowerContract.h pianopower::llccIslandReady) and piano_a830_llcc_island_ready(). One uncached page per
// register group, mapped by the starter thread only; the ready query maps nothing (it reads through the ATTR page or answers false).
namespace {
struct LlccPage {
  uint64_t phys;
  IOMemoryMap *map;
  uintptr_t va;
};
LlccPage gLlccPages[6];
uint32_t gLlccArmed;       // the ATTR page is mapped and its first reading done (release/acquire)
uint8_t gIslandState;      // pianollcc::State of the last reading
bool gIslandProgrammed;

uintptr_t llccVa(uint64_t phys, bool mayMap)
{
  const uint64_t page = phys & ~0xfffull;
  for (const LlccPage &p : gLlccPages)
    if (p.va && p.phys == page) return p.va + uintptr_t(phys & 0xfff);
  if (!mayMap) return 0;
  for (LlccPage &p : gLlccPages)
    if (!p.va) {
      IODeviceMemory *memory = IODeviceMemory::withRange(page, 0x1000);
      IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
      if (memory) memory->release();
      if (!map) return 0;
      p = {page, map, uintptr_t(map->getVirtualAddress())};
      return p.va + uintptr_t(phys & 0xfff);
    }
  return 0;
}
// context: non-null = the starter thread (may map); null = any context (mapped pages only).
bool llccRead(void *context, pianollcc::Block block, uint32_t offset, uint32_t *value)
{
  const uintptr_t va = llccVa(pianollcc::kBlockPhys[block] + offset, context != nullptr);
  if (!va) return false;
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(va) : "memory");
  *value = v;
  return true;
}
bool llccWrite(void *context, pianollcc::Block block, uint32_t offset, uint32_t value)
{
  const uintptr_t va = llccVa(pianollcc::kBlockPhys[block] + offset, context != nullptr);
  if (!va) return false;
  __asm__ volatile("str %w0, [%1]\n\tdsb sy" : : "r"(value), "r"(va) : "memory");
  return true;
}
int gMayMap;
const pianollcc::Io kLlccThread = {&gMayMap, llccRead, llccWrite};
const pianollcc::Io kLlccAny = {nullptr, llccRead, llccWrite};

void islandLine(const char *what, const pianollcc::Reading &r, pianollcc::State state)
{
  piano_a830_note("llcc: ISLAND1 (SCID 12) %s: ATTR0 %#x (0x26841300, expect 0x7fffffff) ATTR1 %#x (0x26841308, expect 0) ATTR2 %#x "
                  "ATTR3 %#x; TRP_STATUS %#x (%s); hw-info %#x: %s", what, r.attr[0], r.attr[1], r.attr[2], r.attr[3], r.status,
                  r.status & pianollcc::kActive ? "active" : "not active", r.hwInfo, pianollcc::stateName(state));
}

void islandPublish(IOService *service)
{
  if (!service) return;
  const pianollcc::State s = pianollcc::State(gIslandState);
  char text[64];
  snprintf(text, sizeof text, "%s%s", pianollcc::stateName(s), gIslandProgrammed ? " (programmed by PianoA830)" : "");
  service->setProperty("piano-llcc-island", text);
}

// The starter thread, after piano-gpu-delay and before the survey or the bring-up: the read-only line, then with
// piano-llcc-island=1 (piano-gpu=2) the programming.
void islandStart(IOService *service, unsigned level)
{
  // Opt-in until a tablet boot has shown the reads are safe under Gunyah: without piano-llcc-island (0 = read only,
  // 1 = read and program) the GPU boot does not touch the LLCC at all and the island stays "unknown".
  if (bootArg("piano-llcc-island", 0xffffffffu) == 0xffffffffu) {
    piano_a830_note("llcc: ISLAND1 not read (piano-llcc-island=0 reads it, =1 programs it)");
    return;
  }
  piano_a830_note("llcc: reading the ISLAND1 slice, read only (0x26864000 hw-info, 0x26841300..0x26841314 ATTR0..3, 0x2680c004 status)");
  const pianollcc::Reading r = pianollcc::read(kLlccThread);
  pianollcc::State state = pianollcc::judge(r);
  islandLine("as found", r, state);
  gIslandState = uint8_t(state);
  __atomic_store_n(&gLlccArmed, 1u, __ATOMIC_RELEASE);
  const uint32_t want = bootArg("piano-llcc-island", 0);
  if (want == 1 && level < 2) piano_a830_note("llcc: piano-llcc-island=1 needs piano-gpu=2 (piano-gpu=1 writes nothing): not programmed");
  if (want == 1 && level >= 2) {
    piano_a830_note("llcc: piano-llcc-island=1: reserving ISLAND1's ways (ATTR0 0x7fffffff, ATTR1 0; ATTR2/ATTR3 only where 0)");
    const pianollcc::Program p = pianollcc::program(kLlccThread);
    state = pianollcc::judge(p.after);
    if (p.outcome == pianollcc::Outcome::programmed) gIslandProgrammed = true;
    piano_a830_note("llcc: piano-llcc-island=1: %s; ATTR0 %#x (AND view %#x) ATTR1 %#x ATTR2 %#x%s ATTR3 %#x%s; %u writes, %u banks%s",
                    pianollcc::outcomeName(p.outcome), p.after.attr[0], p.andAttr0, p.after.attr[1], p.after.attr[2],
                    p.wroteAttr2 ? " (written)" : "", p.after.attr[3], p.wroteAttr3 ? " (written)" : "", p.writes, p.banks,
                    p.outcome == pianollcc::Outcome::readbackFailed ? (p.restored ? "; restored as found" : "; NOT restored") : "");
    if (p.outcome == pianollcc::Outcome::readbackFailed)
      piano_a830_note("llcc: the first mismatch: +%#x read %#x, expected %#x (LLCC writes do not reach the slice here)", p.badOffset,
                      p.badValue, p.badExpect);
    gIslandState = uint8_t(state);
  } else if (want > 1) {
    piano_a830_note("llcc: piano-llcc-island=%u: unknown (1 programs); nothing written", want);
  }
  piano_a830_note("llcc: island %s", state == pianollcc::State::reserved ? "ready (reserved ways 0x7fffffff, no bonus ways)"
                                                                         : "NOT ready: CX-off sleep must stay off (docs/power.md §3.6)");
  islandPublish(service);
}
}  // namespace

// For kexts linked after PianoA830 (their descriptors name it as a library) and the platform function: the two words read again.
extern "C" bool piano_a830_llcc_island_ready(void)
{
  if (!__atomic_load_n(&gLlccArmed, __ATOMIC_ACQUIRE)) return false;
  return pianollcc::ready(kLlccAny);
}

// ---- the GPU's ceiling slots (PianoGpuCeiling.h) ---------------------------------------------------------------------------------
namespace {
IOLock *gCeilingLock;
pianogpu::CeilingSlots gCeiling;
uint32_t gCeilingChanges;

IOReturn ceilingSet(unsigned client, uint32_t khz)
{
  if (client >= pianogpu::kCeilingClients) return kIOReturnBadArgument;
  if (gCeilingLock) IOLockLock(gCeilingLock);
  bool changed = false;
  gCeiling.set(client, khz, &changed);
  const pianogpu::CeilingSlots now = gCeiling;
  if (changed) adreno830_dcvs_set_ceiling(now.effective);
  const uint32_t n = changed ? ++gCeilingChanges : 0;
  if (gCeilingLock) IOLockUnlock(gCeilingLock);
  if (changed && A830IdleLogged(n)) {
    if (now.effective)
      gpuLine("ceiling %u MHz (%s's; slots thermal %u, power profile %u, %u, %u MHz; the lowest wins; #%u)", now.effective / 1000,
              pianogpu::ceilingClientName(now.holder()), now.khz[0] / 1000, now.khz[1] / 1000, now.khz[2] / 1000, now.khz[3] / 1000, n);
    else
      gpuLine("ceiling cleared (no slot holds one; #%u)", n);
  }
  return kIOReturnSuccess;
}
}  // namespace

// ---- the system suspend (A830Suspend.h, piano-gpu-suspend=1) --------------------------------------------------------------------
// kOff and kOn on a thread call (acknowledgeSetPowerState when done: the wait for quiet and a recovery can take seconds); the host
// votes in the platform sleep and wake actions (IOPlatformSleepAction / IOPlatformWakeAction, installed from the service's properties:
// IOCPUSleepKernel runs them after every driver's kOff and before any kOn). Lines through piano_a830_note: all of it is thread context
// and the logger thread may not run again before the CPUs park.
namespace {
A830SuspendConfig gSuspendConfig;
A830SuspendMachine gSuspend;
IOService *gService;            // the PianoA830 instance
IOService *gDpu;                // PianoDpu, looked up at kOff (retained until kOn)
thread_call_t gPmCall;
unsigned long gPmTarget;
bool gJoined;
uint32_t gStartDone;
constexpr uint32_t kActionPriority = 100000;   // late in PreSleep, early in PostResume (the wake queue runs in reverse)
constexpr uint32_t kPmBudgetUs = 10 * 1000 * 1000;

const char *scanoutName(pianopower::Scanout s)
{
  return s == pianopower::Scanout::stopped ? "stopped" : s == pianopower::Scanout::running ? "running" : "unknown (running)";
}

void suspendPublish()
{
  if (!gService) return;
  char text[160];
  snprintf(text, sizeof text, "suspends %u, refused %u%s, votes down %u, vote failures %u, wakes %u (failed %u, over %u ms: %u), longest %u us",
           gSuspend.suspends, gSuspend.refusals, gSuspend.blocked ? " (off for this boot)" : "", gSuspend.voteDowns,
           gSuspend.voteFailures, gSuspend.wakes, gSuspend.wakeFailures, gSuspendConfig.wakeBoundMs, gSuspend.overBound,
           gSuspend.maxWakeUs);
  gService->setProperty("piano-gpu-suspend", text);
}

void suspendGpu()
{
  const unsigned n = gSuspend.suspends + gSuspend.refusals + 1;
  if (!gSuspend.sleepWanted()) {
    piano_a830_note("gpu suspend #%u: not taking part (%s): the GPU and its votes stay as they are", n,
                    gSuspend.blocked ? "off for this boot after a failed entry" : "its votes are still down");
    return;
  }
  if (gDpu) gDpu->release();
  gDpu = IOService::copyMatchingService(IOService::serviceMatching("PianoDpu"));
  A830SuspendReport r{};
  const IOReturn result = adreno830_suspend_gpu(gSuspendConfig.idleWaitMs, &r);
  uint8_t outcome = A830SuspendOk;
  if (result == kIOReturnSuccess) outcome = A830SuspendOk;
  else if (result == kIOReturnNotReady) outcome = A830SuspendOff;
  else if (result == kIOReturnBusy) outcome = A830SuspendBusy;
  else if (result == kIOReturnIOError) outcome = A830SuspendFailed;
  else outcome = r.powerPhase == A830PowerLost ? A830SuspendLost : A830SuspendBusy;   // still being recovered after the wait
  gSuspend.sleepDone(outcome);
  switch (outcome) {
  case A830SuspendOk:
    piano_a830_note("gpu suspend #%u: GPU down in %u.%03u ms (quiet after %u us; %s; %s): submissions wait for the resume", n,
                    r.waitedUs / 1000, r.waitedUs % 1000, r.waitedUs - r.entryUs,
                    r.alreadyAsleep ? "already in its full slumber" : "full slumber entered",
                    r.cxCollapsed || r.alreadyAsleep ? "CX GDSC collapsed" : "CX STILL ON: another voter keeps the GDSC");
    break;
  case A830SuspendOff:
    piano_a830_note("gpu suspend #%u: not taking part (the GPU is not running): its votes stay as they are", n);
    break;
  case A830SuspendBusy:
    piano_a830_note("gpu suspend #%u: REFUSED: still busy after %u ms (work in flight, a holder or a recovery): the GPU stays up as it "
                    "was, its votes too; the suspend goes on without the GPU's share", n, gSuspendConfig.idleWaitMs);
    break;
  case A830SuspendLost:
    piano_a830_note("gpu suspend #%u: REFUSED: the GPU is lost: nothing of it is touched, its votes stay", n);
    break;
  default: {
    // The recovery has the GPU: wait for it here, so the suspend never freezes a GPU half down.
    uint32_t waited = 0;
    A830PowerState *power = adreno830_power_state();
    while (power && waited < 3000 && power->phase() != A830PowerActive && power->phase() != A830PowerLost) {
      IOSleep(10);
      waited += 10;
    }
    const A830PowerPhase phase = power ? power->phase() : A830PowerLost;
    piano_a830_note("gpu suspend #%u: REFUSED: the slumber entry failed; the recovery %s after %u ms; no GPU suspend for the rest of the "
                    "boot; the suspend goes on without the GPU's share", n, phase == A830PowerActive ? "brought it back" :
                    phase == A830PowerLost ? "lost it" : "is STILL RUNNING", waited);
    break;
  }
  }
  suspendPublish();
}

void votesDown()
{
  if (!gSuspend.votesDownWanted()) return;
  pianopower::Scanout scanout = pianopower::displayScanout(gDpu);
  // PianoDpu's panel sleep runs on its own thread after the display went off: give it displayWaitMs to finish.
  uint32_t waited = 0;
  while (gDpu && scanout == pianopower::Scanout::running && waited < gSuspendConfig.displayWaitMs) {
    IOSleep(10);
    waited += 10;
    scanout = pianopower::displayScanout(gDpu);
  }
  A830SuspendReport r{};
  const IOReturn result = adreno830_suspend_votes(uint8_t(scanout), &gSuspendConfig, &r);
  gSuspend.votesDownDone(result == kIOReturnSuccess);
  if (result == kIOReturnSuccess)
    piano_a830_note("gpu suspend: votes down in %u us (scanout %s%s): DDR %u -> %u MB/s%s, CX %#x -> %#x, MX %#x -> %#x%s", r.votesUs,
                    scanoutName(scanout), waited ? " after waiting for the panel" : "", r.ddrBefore / 1000, r.ddrAfter / 1000,
                    r.ddrAfter ? " (the display's floor)" : " (keepalive minimum)", r.cxBefore, r.cxAfter, r.mxBefore, r.mxAfter,
                    r.cxAfter ? "" : " (the rails' votes gone)");
  else
    piano_a830_note("gpu suspend: votes NOT lowered (rpmh %d, %#x): put back as they were; the GPU stays down", r.rpmh, unsigned(result));
  suspendPublish();
}

void votesUp(const char *where)
{
  if (!gSuspend.votesUpWanted()) return;
  A830SuspendReport r{};
  const IOReturn result = adreno830_resume_votes(&r);
  gSuspend.votesUpDone(result == kIOReturnSuccess);
  if (result == kIOReturnSuccess)
    piano_a830_note("gpu resume: votes back in %u us (%s): CX %#x MX %#x, DDR %u MB/s", r.votesUs, where, r.cxAfter, r.mxAfter,
                    r.ddrAfter / 1000);
  else
    piano_a830_note("gpu resume: votes NOT back (%s, rpmh %d): %s", where, r.rpmh,
                    strcmp(where, "kOn") ? "kOn tries again" : "the GPU stays asleep (its gate shut) until a later kOn");
  suspendPublish();
}

void resumeGpu()
{
  votesUp("kOn");
  if (gSuspend.votesUpWanted() || !gSuspend.wakeWanted()) {
    suspendPublish();
    return;
  }
  A830SuspendReport r{};
  const IOReturn result = adreno830_resume_gpu(gSuspendConfig.eagerWake, &r);
  gSuspend.wakeDone(result == kIOReturnSuccess, r.wakeUs, gSuspendConfig.eagerWake ? gSuspendConfig.wakeBoundMs : 0);
  if (result != kIOReturnSuccess)
    piano_a830_note("gpu resume #%u: the WAKE FAILED (%#x): the recovery has the GPU", gSuspend.wakes + gSuspend.wakeFailures, unsigned(result));
  else if (gSuspendConfig.eagerWake)
    piano_a830_note("gpu resume #%u: GPU awake in %u.%03u ms (%s the %u ms bound): the first frame waits for nothing", gSuspend.wakes,
                    r.wakeUs / 1000, r.wakeUs % 1000, r.wakeUs > gSuspendConfig.wakeBoundMs * 1000 ? "OVER" : "within",
                    gSuspendConfig.wakeBoundMs);
  else
    piano_a830_note("gpu resume #%u: the gate open, the GPU left asleep (piano-gpu-suspend-wake=0): the first submission wakes it",
                    gSuspend.wakes);
  if (gDpu) {
    gDpu->release();
    gDpu = nullptr;
  }
  suspendPublish();
}

void pmWork(thread_call_param_t, thread_call_param_t)
{
  if (gPmTarget == pianopower::kOff) suspendGpu();
  else resumeGpu();
  if (gService) gService->acknowledgeSetPowerState();
}

void suspendConfigure(unsigned level)
{
  A830SuspendConfig &c = gSuspendConfig;
  c = A830SuspendConfig{};
  c.enabled = level >= 2 && bootArg("piano-gpu-suspend", 0) == 1;
  const uint32_t idle = bootArg("piano-gpu-suspend-idle-ms", 500);
  c.idleWaitMs = idle < 50 ? 50 : idle > 2000 ? 2000 : idle;
  c.railVlvl = bootArg("piano-gpu-suspend-rails", 0x140);
  c.eagerWake = bootArg("piano-gpu-suspend-wake", 1) != 0;
  const uint32_t bound = bootArg("piano-gpu-suspend-wake-ms", 100);
  c.wakeBoundMs = bound < 1 ? 1 : bound > 10000 ? 10000 : bound;
  const uint32_t display = bootArg("piano-gpu-suspend-display-ms", 300);
  c.displayWaitMs = display > 2000 ? 2000 : display;
}

// The starter thread, once the GPU runs: into the power tree (the contract's two states), so IOPMrootDomain takes the GPU down
// before a system sleep and up after it.
void suspendJoin()
{
  if (!gSuspendConfig.enabled || !gService || gJoined) return;
  for (unsigned i = 0; i < 500 && !__atomic_load_n(&gStartDone, __ATOMIC_ACQUIRE); ++i) IOSleep(10);   // start() returned
  gSuspendConfig.ddrFloorLevel = gDisplayFloorLevel;
  gPmCall = thread_call_allocate_with_options(pmWork, nullptr, THREAD_CALL_PRIORITY_KERNEL_HIGH, 0);
  gJoined = true;
  const IOReturn r = pianopower::join(gService);
  piano_a830_note("gpu suspend: piano-gpu-suspend=1: %s the power tree (%#x): kOff waits up to %u ms for quiet, then the full slumber; "
                  "PreSleep: DDR to the keepalive minimum (the display's floor, level %u, while it scans), CX/MX to %#x%s; PostResume "
                  "puts them back; kOn %s (bound %u ms)", r == kIOReturnSuccess ? "joined" : "COULD NOT JOIN", unsigned(r),
                  gSuspendConfig.idleWaitMs, gSuspendConfig.ddrFloorLevel, gSuspendConfig.railVlvl,
                  gSuspendConfig.railVlvl ? " (at least NOM_L1 while the panel scans)" : " (the votes gone; NOM_L1 while the panel scans)",
                  gSuspendConfig.eagerWake ? "wakes the GPU" : "opens the gate (the first submission wakes it)", gSuspendConfig.wakeBoundMs);
  if (r != kIOReturnSuccess) gJoined = false;
  suspendPublish();
}
}  // namespace

// ---- the starter ------------------------------------------------------------------------------------------------------------------
namespace {
void run(void *, wait_result_t)
{
  gRunThread = current_thread();
  runPhase(RunInit);
  const uint32_t delay = bootArg("piano-gpu-delay", 0);
  if (delay) {
    runPhase(RunDelay);
    piano_a830_note("piano-gpu-delay=%u: waiting %u s before the %s", delay, delay, gLevel == 1 ? "survey" : "bring-up");
    IOSleep(delay * 1000);
  }
  // The LLCC island first (read only at any GPU boot; piano-llcc-island=1 programs it at piano-gpu=2), after the delay like
  // everything of the GPU's, so a fault there does not stop the boot before its root.
  islandStart(gService, gLevel);
  const uint32_t defaultMask = gLevel == 1 ? uint32_t(PianoA830SurveyDefault) : 0u;
  const uint32_t mask = bootArg("piano-gpu-survey", defaultMask);
  if (gLevel == 1) {
    runPhase(RunSurvey);
    piano_a830_note("piano-gpu=1: read-only survey (mask %#x) around the driver's S0; nothing is written, nothing is published", mask);
    piano_a830_survey(mask & PianoA830SurveyInputs);
    piano_a830_note("S0 (board inputs, read only): adreno830_start with piano-gpu-stop=0");
    piano_a830_report(piano_a830_board_start(1));
    piano_a830_survey(mask & ~uint32_t(PianoA830SurveyInputs));
    piano_a830_note("survey done (piano-gpu=1)");
    runPhase(RunDone);
    return;
  }
  piano_a830_note("piano-gpu=2: staged bring-up S1..S10, polled (piano-gpu-stop=%d, trace %u%s)", int(bootArg("piano-gpu-stop", ~0u)),
                  gTrace, gTraceSync ? ", synchronous: piano-gpu-trace=3, a heartbeat every second" : "");
  if (mask) {
    runPhase(RunSurvey);
    piano_a830_survey(mask);   // piano-gpu-survey=<mask>: the read-only survey first
  }
  runPhase(RunBringUp);
  const IOReturn result = piano_a830_board_start(2);
  __atomic_store_n(&gSyncSince, mach_absolute_time(), __ATOMIC_RELAXED);
  __atomic_store_n(&gSyncState, 3u, __ATOMIC_RELEASE);
  runPhase(RunReport);
  piano_a830_report(result);
  piano_a830_note("bring-up %s", adreno830_running() ? "done: the GPU runs" : result == kIOReturnSuccess ? "stopped (piano-gpu-stop)" : "failed");
  if (adreno830_running()) suspendJoin();   // piano-gpu-suspend=1: the GPU's part of a system sleep (A830Suspend.h)
  else if (gSuspendConfig.enabled) piano_a830_note("gpu suspend: piano-gpu-suspend=1 but the GPU does not run: not in the power tree");
  // Where the GPU-side streams go, once S3 programmed the SMMU, and again once the session's Metal clients are up. Only while the GPU
  // stays on: piano-gpu-slumber would collapse CX, and an unclocked SMMU read can hang the bus.
  const Adreno830Board *board = piano_a830_board();
  if (result == kIOReturnSuccess && adreno830_status.stage >= A830SSmmu && board && !board->knobs.slumber) {
    runPhase(RunRoutes);
    piano_a830_smmu_routes("after the bring-up");
    if (adreno830_running()) {
      runPhase(RunWait);
      IOSleep(60000);
      runPhase(RunRoutes);
      // By now the GPU may sleep (piano-gpu-idle=2's slumber, a system suspend's full slumber): read only while it is up, holding it
      // so (a slumber entry waits for the reader).
      A830PowerState *power = adreno830_power_state();
      if (!power || power->accessBegin(false)) {
        piano_a830_smmu_routes("60 s later");
        if (power) power->accessEnd(false);
      } else {
        piano_a830_note("SMMU routes (60 s later): skipped, the GPU sleeps (CX may be off: an unclocked SMMU read can hang the bus)");
      }
    }
  }
  runPhase(RunDone);
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
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                void *param4) override;
  using IOService::callPlatformFunction;
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
  const uint32_t traceArg = bootArg("piano-gpu-trace", 1);
  gTrace = gLevel >= 2 && traceArg != 0;
  gTraceSync = gLevel >= 2 && traceArg == 3;   // the driver gets 1 (PianoA830Board.cpp)
  nanoseconds_to_absolutetime(10ull * 1000000000ull, &gRowEvery);
  setProperty("piano-gpu", gLevel, 32);
  gService = this;
  gCeilingLock = IOLockAlloc();
  setProperty("piano-gpu-ceiling-clients", pianogpu::kCeilingClients, 32);   // docs/policy.md §5.3: one slot per caller
  suspendConfigure(gLevel);
  if (gSuspendConfig.enabled) {
    // The platform sleep and wake actions (the host votes): installed from these properties when start() returns.
    setProperty("IOPlatformSleepAction", kActionPriority, 32);
    setProperty("IOPlatformWakeAction", kActionPriority, 32);
  }
  piano_a830_note("start: piano-gpu=%u (A830 chip %#x, k27 native kext, polled)", gLevel, 0x44050001u);
  if (gTraceSync) {
    gRing.stamp = static_cast<uint64_t *>(IOMallocZero(size_t(RingLines) * sizeof(uint64_t)));
    gPrintLock = IOLockAlloc();
    if (!gRing.stamp || !gPrintLock) {
      piano_a830_note("piano-gpu-trace=3: no memory for the stamps or the print lock; the trace stays asynchronous (as 1)");
      if (gRing.stamp) IOFree(gRing.stamp, size_t(RingLines) * sizeof(uint64_t));
      if (gPrintLock) IOLockFree(gPrintLock);
      gRing.stamp = nullptr;
      gPrintLock = nullptr;
      gTraceSync = 0;
    }
  }
  thread_t thread = nullptr;
  gRing.text = static_cast<char (*)[RingBytes]>(IOMallocZero(size_t(RingLines) * RingBytes));
  if (!gRing.text || kernel_thread_start(logger, nullptr, &thread) != KERN_SUCCESS) {
    piano_a830_note("FAILED: no line ring or logger thread (%u KiB); the trace stays off", RingLines * RingBytes / 1024);
    if (gRing.text) IOFree(gRing.text, size_t(RingLines) * RingBytes);
    gRing.text = nullptr;
    gTrace = 0;
    gTraceSync = 0;
  } else {
    thread_deallocate(thread);
    piano_a830_note("line ring %u x %u bytes, logger thread running%s", RingLines, RingBytes,
                    gTraceSync ? "; piano-gpu-trace=3: each trace line printed before its access by the thread making it" : "");
  }
  thread = nullptr;
  if (kernel_thread_start(run, nullptr, &thread) != KERN_SUCCESS) {
    piano_a830_note("FAILED: no kernel thread");
    return true;
  }
  thread_deallocate(thread);
  // For the display's fence lookup, the thermal and power-profile ceilings and the LLCC island query (callPlatformFunction below).
  // At piano-gpu=1 too since 2026-11-11 (the island query): the fences answer NotReady and a ceiling waits for a DCVS that never starts.
  registerService();
  __atomic_store_n(&gStartDone, 1u, __ATOMIC_RELEASE);
  return true;
}

// The contract's two states (PianoPowerContract.h), only once the GPU runs and piano-gpu-suspend=1 (suspendJoin): the work on a
// thread call, acknowledged when done.
IOReturn PianoA830::setPowerState(unsigned long state, IOService *)
{
  if (!gJoined) return pianopower::kAckNow;
  gPmTarget = state;
  if (gPmCall) {
    thread_call_enter(gPmCall);   // (one already queued takes the new target: IOPM never overlaps two changes of one driver)
    return kPmBudgetUs;
  }
  if (state == pianopower::kOff) suspendGpu();
  else resumeGpu();
  return pianopower::kAckNow;
}

IOReturn PianoA830::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                         void *param4)
{
  if (functionName && functionName->isEqualTo("native-adreno-fences")) {
    if (!param1 || !param2) return kIOReturnBadArgument;
    return native_adreno_fences(static_cast<uint32_t *>(param1), static_cast<uint32_t *>(param2)) ? kIOReturnSuccess : kIOReturnNotReady;
  }
  // PianoThermal's passive trip (docs/drivers/thermal.md §10.5) and the power profile (PianoPolicy, docs/policy.md §5.3): a reversible
  // DCVS ceiling, param1 = kHz (0 clears), param2 = the caller's slot (null: 0, thermal; 1 the power profile; 2..3 free). The lowest
  // non-zero slot is the ceiling (PianoGpuCeiling.h, adreno830_dcvs_set_ceiling; one set before S10 applies when the DCVS starts). A line
  // per change of the ceiling in force, the first 8 then every 10th, 100th, ...
  if (functionName && functionName->isEqualTo("piano-gpu-ceiling"))
    return ceilingSet(unsigned(uintptr_t(param2)), uint32_t(uintptr_t(param1)));
  // The LLCC island (PianoPowerContract.h pianopower::llccIslandReady): param1 = bool *, the two words read again. Any thread.
  if (functionName && functionName->isEqualTo("piano-llcc-island-ready")) {
    if (!param1) return kIOReturnBadArgument;
    if (!__atomic_load_n(&gLlccArmed, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
    *static_cast<bool *>(param1) = piano_a830_llcc_island_ready();
    return kIOReturnSuccess;
  }
  // The system suspend's host votes (piano-gpu-suspend=1): IOCPUSleepKernel's PreSleep and PostResume platform actions.
  if (functionName && functionName->isEqualTo("IOPlatformSleepAction")) {
    if (gJoined) votesDown();
    return kIOReturnSuccess;
  }
  if (functionName && functionName->isEqualTo("IOPlatformWakeAction")) {
    if (gJoined) votesUp("PostResume");
    return kIOReturnSuccess;
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
