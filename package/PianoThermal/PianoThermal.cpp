// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: SoC temperatures, thermal protection and CPU frequency for piano (SM8750) under native macOS 27 at EL1. Design,
// facts, boot arguments and the test procedure: docs/drivers/thermal.md. Inert unless piano-thermal or piano-cpufreq is set.
//   piano-thermal=1  read only: every TSENS sensor of the four controllers (Tsens.h) each poll (piano-thermal-poll-ms, 1000; every
//                    piano-thermal-fast-ms, 20, while a sensor is within 10 C of its trip or a cluster is thermally limited), the
//                    hottest per controller on the console every piano-thermal-log-s (30) and as IORegistry properties; at start the
//                    controllers' version/enable/thresholds (TM and SROT loads), the SCMI channel's status word (a load), and every 30 s
//                    each core's clock measured with a chain of dependent adds (no device access; piano-thermal-clock=0 skips it;
//                    since 2026-11-12 not while the display is off, which would wake every idle core for a log line: =2 measures
//                    then too, as before).
//                    Nothing is written at this level: every MMIO access is a load.
//   piano-thermal=2  level 1 + the protection policy (Policy.h): warm (85 C) logged; at a sensor's stock passive trip the GPU is
//                    capped (PianoA830's "piano-gpu-ceiling", reversible) or, while PianoA830 has none, parked at its lowest level
//                    for the rest of the boot (its exported adreno830_dcvs_park; at critical always); at the stock
//                    critical trip - piano-thermal-margin (10 C) held for 1 s: logged loudly (every 5 s), and with
//                    piano-thermal-shutdown=1 the kernel's halt (reboot_kernel(RB_HALT): processes signalled, disks synced and
//                    unmounted, IOKit's shutdown notifications, PSCI SYSTEM_OFF). Default: log only.
//   piano-cpufreq=1  the CPU clusters over SCMI (Scmi.h): protocol, domains, levels, the level each cluster is at (LEVEL_GET), then
//                    every 30 s again. Messages only, no level is set. Writes: the SCMI channel's shared memory and the doorbell.
//   piano-cpufreq=2  fixed level per cluster: the fastest <= piano-cpufreq-cap0/cap1 (MHz, default Ubuntu's Power Saver profile:
//                    1785 / 1958), thermal limit on top (Cluster.h: Linux step_wise on the stock trips).
//   piano-cpufreq=3  the governor (Governor.h): load-based per cluster every piano-cpufreq-tick-ms (100), caps on battery (Power Saver)
//                    / on power (Balanced: 3532 / 4089) from the IOPMPowerSource's ExternalConnected (unknown = battery), thermal
//                    limit on top. Its busy figure leaves out this thread's own time on each core (2026-11-12, P9), and the window
//                    holding the per-core clock measurement makes no decision.
// The thermal limit of modes 2 and 3 (Cluster.h) works on a hard ceiling since 2026-11-12: one level down per 10 ms while any cpu-*
// sensor of the cluster is at piano-cpufreq-hot (default 90 C, never above the zone's stock passive trip; 0 = the stock trips),
// whatever piano-thermal says.
// piano-cpufreq >= 1 implies the level 1 readout (its thermal input). No clock, regulator or GDSC is voted by this kext.
// Suspend hooks (piano_thermal_suspend_prepare/finish, below): the threshold interrupt masks across system suspend, the only TSENS
// writes, and only when a caller asks: this service's setPowerState (in the power tree, docs/power.md 4) with
// piano-thermal-sleep-mask=1, or another driver through the platform functions. Across a system sleep the thread is parked (no
// SCMI request, no TSENS read in flight; Governor.h SleepPlan); the first zone update after the wake follows at once.
// The power profile (docs/policy.md 5.2): callPlatformFunction("piano-thermal-profile", mode) from PianoPolicy: 0 the caps by the
// power source, 1 Power Saver, 2 Balanced, whatever the source (Governor.h Profile); the governor reads it every tick, the fixed
// level ignores it; the property piano-thermal-profile and one line per change.
// piano-thermal-adc=1|2 (B6, PianoThermalAdc.cpp; implies piano-thermal=1): the eleven board thermistors on the PMIC ADC, on a thread
// of their own, and piano_thermal_board_temps() for charging safety. Its only writes: the ADC SDAM's conversion bytes.
// piano-thermal-irq=1 (PianoThermalIrq.cpp, TsensIrq.h; with piano-thermal >= 1): the TSENS upper/lower threshold interrupts; at
// rest the base poll relaxes, an interrupt raises the tier at once; any failure: today's polling (docs/drivers/thermal.md section 13).
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/mach_types.h>
#include <mach/processor_info.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "Cluster.h"
#include "Governor.h"
#include <PianoPowerContract.h>
#include <PianoDisplayFollow.h>
#include "PianoThermalAdc.h"
#include "PianoThermalIrq.h"
#include "Policy.h"
#include "Scmi.h"
#include "Tsens.h"

using namespace pianothermal;

extern "C" {
// PianoA830 (listed before this kext; tooling/k27/kext/PianoThermal.json names it as a library). Both are lock-free no-ops while
// the GPU driver is inert (piano-gpu unset: no DCVS).
void adreno830_dcvs_park(void);
int32_t adreno830_thermal_state(uint32_t *flags);
// The kernel (resolved from its symbol table by the KC builder).
int reboot_kernel(int howto, char *message);
kern_return_t processor_info(processor_t processor, processor_flavor_t flavor, host_t *host, processor_info_t info,
                             mach_msg_type_number_t *count);
processor_t thread_bind(processor_t processor);
processor_t cpu_to_processor(int cpu);
extern int hz;
// Post-mortem breadcrumbs: the protection state (Policy State) and the last hottest reading in deci-C.
volatile int32_t piano_thermal_state = 0;
volatile int32_t piano_thermal_hottest_deci = 0;
}

namespace {

constexpr uint32_t kA830Valid = 1u << 0, kA830Limiting = 1u << 1, kA830Emergency = 1u << 2;   // A830Dcvs.h A830Thermal*
constexpr int kRbHalt = 0x08;                                                                   // bsd/sys/reboot.h RB_HALT

struct Args {
  uint32_t level, cpufreq, shutdown, pollMs, logS, clock, srot, warmC, marginC, delay, sleepMask;
  uint32_t cap[2], bat[2], ac[2], minMhz[2], hispeed[2], tickMs, fastMs, warmMs, warmPollC, fastMhz, fastTempC, clockMs, gpuMhz, hotC,
      releaseC, cutC;
  bool capGiven[2];
};
// The panel's display power, followed through NativeGopDisplay's messages (package/include/PianoDisplayFollow.h): the clock line's rule.
pianodisplay::Follow gDisplay;
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}
uint32_t clampU(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void buildTrips();

void readArgs()
{
  gArgs.level = bootArg("piano-thermal", 0);
  gArgs.cpufreq = bootArg("piano-cpufreq", 0);
  if (gArgs.cpufreq > 3) gArgs.cpufreq = 3;
  if (gArgs.level > 2) gArgs.level = 2;
  if (gArgs.cpufreq && !gArgs.level) gArgs.level = 1;
  if (adc::hookLevel() && !gArgs.level) gArgs.level = 1;   // B6: the board thermistors come with the level 1 readout
  gArgs.shutdown = bootArg("piano-thermal-shutdown", 0);
  gArgs.pollMs = clampU(bootArg("piano-thermal-poll-ms", 1000), 100, 10000);
  gArgs.logS = clampU(bootArg("piano-thermal-log-s", 30), 5, 3600);
  gArgs.clock = bootArg("piano-thermal-clock", 1);
  gArgs.srot = bootArg("piano-thermal-srot", 1);
  gArgs.warmC = clampU(bootArg("piano-thermal-warm", 85), 50, 104);
  gArgs.marginC = clampU(bootArg("piano-thermal-margin", 10), 5, 30);
  gArgs.delay = clampU(bootArg("piano-thermal-delay", 0), 0, 600);
  gArgs.sleepMask = bootArg("piano-thermal-sleep-mask", 0);   // a system sleep also masks the threshold interrupts (the hooks)
  // Ubuntu's GNOME profiles on piano (docs/ubuntu-history/device.md 2.6): Power Saver caps cpu0-5 at 1785.6 MHz and cpu6-7 at
  // 1958.4 MHz, Balanced at 3532.8 / 4089.6 MHz (boost off). piano-cpufreq-cap (both clusters) or -cap0/-cap1: the fixed level of
  // mode 2 (default Power Saver's); in mode 3 a hard ceiling, only when given. A cap in MHz admits the levels of that whole MHz.
  static const uint32_t kSaver[2] = {1785, 1958}, kBalanced[2] = {3532, 4089};
  uint32_t capBoth = 0;
  const bool bothGiven = PE_parse_boot_argn("piano-cpufreq-cap", &capBoth, sizeof capBoth);
  for (unsigned d = 0; d < 2; ++d) {
    uint32_t v = bothGiven ? capBoth : kSaver[d];
    const bool given = PE_parse_boot_argn(d ? "piano-cpufreq-cap1" : "piano-cpufreq-cap0", &v, sizeof v);
    gArgs.cap[d] = clampU(v, 300, 5000);
    gArgs.capGiven[d] = given || bothGiven;
  }
  gArgs.bat[0] = bootArg("piano-cpufreq-bat0", kSaver[0]);
  gArgs.bat[1] = bootArg("piano-cpufreq-bat1", kSaver[1]);
  gArgs.ac[0] = bootArg("piano-cpufreq-ac0", kBalanced[0]);
  gArgs.ac[1] = bootArg("piano-cpufreq-ac1", kBalanced[1]);
  gArgs.minMhz[0] = bootArg("piano-cpufreq-min0", 0);
  gArgs.minMhz[1] = bootArg("piano-cpufreq-min1", 0);
  gArgs.hispeed[0] = bootArg("piano-cpufreq-hispeed0", 1500);
  gArgs.hispeed[1] = bootArg("piano-cpufreq-hispeed1", 1800);
  gArgs.tickMs = clampU(bootArg("piano-cpufreq-tick-ms", 100), 20, 1000);
  // Poll tiers (Cluster.h pollTier): 10 ms near a trip (Linux: 10 ms while passive), 250 ms from 70 C, the base below.
  gArgs.fastMs = clampU(bootArg("piano-thermal-fast-ms", 10), 5, 1000);
  gArgs.warmMs = clampU(bootArg("piano-thermal-warm-ms", 250), 10, 10000);
  gArgs.warmPollC = clampU(bootArg("piano-thermal-warm-poll", 70), 30, 104);
  gArgs.fastMhz = clampU(bootArg("piano-thermal-fast-mhz", 2500), 300, 5000);
  // Above the fast-poll clock: 10 ms only from piano-thermal-fast-temp (70 C) up, else every piano-thermal-clock-ms (100: one tick of
  // the governor, so no wakeup of its own while it governs). Cluster.h pollTier has the arithmetic.
  gArgs.fastTempC = clampU(bootArg("piano-thermal-fast-temp", 70), 30, 104);
  gArgs.clockMs = clampU(bootArg("piano-thermal-clock-ms", 100), 5, 100);
  gArgs.gpuMhz = clampU(bootArg("piano-thermal-gpu-mhz", 160), 100, 1100);   // the GPU ceiling at passive (160: the A830's floor)
  // The cluster thermal limit's thresholds (Cluster.h): the ceiling piano-cpufreq-hot, absolute degrees C for every cpu-* zone,
  // default 90 C (2026-11-12, P9), each zone's stock passive trip (105 C or 118 C) when that is lower; 0 = the stock DT's per zone
  // (105 C or 118 C passive, 5 or 8 C hysteresis). The release point piano-cpufreq-release (default: 5 C under the ceiling, or the
  // zone's hysteresis with the stock trips); critical 125 C - margin unless piano-cpufreq-cut.
  gArgs.hotC = clampU(bootArg("piano-cpufreq-hot", 90), 0, 118);
  if (gArgs.hotC && gArgs.hotC < 40) gArgs.hotC = 40;
  gArgs.releaseC = clampU(bootArg("piano-cpufreq-release", 0), 0, 118);
  gArgs.cutC = clampU(bootArg("piano-cpufreq-cut", 0), 0, 125);
  buildTrips();
}

ClusterTrips gTrips;
char gTripsText[160];

void buildTrips()
{
  gTrips.hotDeci = int32_t(gArgs.hotC) * 10;
  gTrips.releaseDeci = int32_t(gArgs.releaseC) * 10;
  gTrips.cutDeci = int32_t(gArgs.cutC) * 10;
  gTrips.marginDeci = int32_t(gArgs.marginC) * 10;
  if (gTrips.hotDeci)
    snprintf(gTripsText, sizeof gTripsText, "one level down per %u ms at %u C (the stock 105/118 C if lower), up per 1 s under %u C, "
             "slowest at %u C", gArgs.fastMs, gArgs.hotC, gArgs.releaseC ? gArgs.releaseC : gArgs.hotC - 5,
             gArgs.cutC ? gArgs.cutC : 125 - gArgs.marginC);
  else
    snprintf(gTripsText, sizeof gTripsText, "stock trips (cpu-* passive 105/118 C, hysteresis 5/8 C): one level down per %u ms, up "
             "per 1 s, slowest at %u C", gArgs.fastMs, gArgs.cutC ? gArgs.cutC : 125 - gArgs.marginC);
}
const char *tripsText() { return gTripsText; }

uint64_t nowNs()
{
  uint64_t ns;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns;
}
uint64_t gStartNs;
uint32_t upS() { return uint32_t((nowNs() - gStartNs) / 1000000000ull); }

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[480];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoThermal: %s\n", line);
}

// Append to line[at]: the new end, clamped to the buffer (snprintf returns what the text wanted, not what fitted).
int catf(char *line, size_t size, int at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
int catf(char *line, size_t size, int at, const char *fmt, ...)
{
  if (at < 0 || size_t(at) + 1 >= size) return at;
  va_list ap;
  va_start(ap, fmt);
  const int wanted = vsnprintf(line + at, size - size_t(at), fmt, ap);
  va_end(ap);
  if (wanted < 0) return at;
  return size_t(at) + size_t(wanted) >= size ? int(size - 1) : at + wanted;
}

struct Deci {
  char s[16];
  explicit Deci(int32_t d) { deciText(d, s, sizeof s); }
};

// Explicit ldr/str, as the other piano kexts do.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(a) : "memory"); }

// ------------------------------------------------------------------------------------------------------------- MMIO
struct Window {
  IOMemoryMap *map;
  uintptr_t va;
  uint32_t bytes;
};
Window gTm[kControllers], gSrot[kControllers], gShmem, gMbox;
unsigned gMapFailures;

bool mapWindow(Window &w, uint64_t pa, uint32_t bytes)
{
  IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) {
    ++gMapFailures;
    return false;
  }
  w = {map, uintptr_t(map->getVirtualAddress()), bytes};
  return true;
}

uint32_t tsensRead(void *, unsigned c, uint32_t off)
{
  if (c >= kControllers || !gTm[c].va || off + 4 > gTm[c].bytes) return 0xffffffffu;
  return rd32(gTm[c].va + off);
}
bool tsensMapped(void *, unsigned c) { return c < kControllers && gTm[c].va; }
const TsensIo kTsensIo = {nullptr, tsensRead, tsensMapped};

uint32_t srotRead(unsigned c, uint32_t off)
{
  if (c >= kControllers || !gSrot[c].va || off + 4 > gSrot[c].bytes) return 0xffffffffu;
  return rd32(gSrot[c].va + off);
}

// SCMI channel (level 1 only reads it; the transport writes only with piano-cpufreq >= 1)
uint32_t shmRead(void *, uint32_t off) { return gShmem.va && off + 4 <= gShmem.bytes ? rd32(gShmem.va + off) : 0; }
void shmWrite(void *, uint32_t off, uint32_t v)
{
  if (gShmem.va && off + 4 <= gShmem.bytes) wr32(gShmem.va + off, v);
}
void ring(void *)
{
  __builtin_arm_dsb(0xf);
  if (gMbox.va) wr32(gMbox.va + scmi::kDoorbell, 1);
  __builtin_arm_dsb(0xf);
}
uint64_t scmiNowUs(void *) { return nowNs() / 1000; }
void scmiPause(void *, uint32_t us) { IODelay(us); }

// ------------------------------------------------------------------------------------------------------------- CPU topology
constexpr unsigned kMaxCpus = 16, kDomains = 2;
struct Cpu {
  unsigned id, domain;
  uint32_t phys;
  processor_t processor;
  uint64_t selfNs;           // this thread's own time on the core (noteSelf), cumulative
  CoreLoad load;             // the load window (Governor.h): idle and own time at the last reading
};
Cpu gCpus[kMaxCpus];
unsigned gCpuCount;
uint64_t gSelfNsTotal;       // this thread's own time on all cores, for the report
bool gRebase;                // the next load reading only starts a window (the clock measurement ran in the last one)

// This thread's own time (a poll's work, measured around it) on the core it ran on: taken out of that core's busy figure.
void noteSelf(uint64_t ns)
{
  gSelfNsTotal += ns;
  const unsigned me = unsigned(cpu_number());
  for (unsigned i = 0; i < gCpuCount; ++i)
    if (gCpus[i].id == me) {
      gCpus[i].selfNs += ns;
      return;
    }
}

void readTopology()
{
  const ml_topology_info_t *t = ml_get_topology_info();
  gCpuCount = 0;
  for (unsigned i = 0; t && i < t->num_cpus && gCpuCount < kMaxCpus; ++i) {
    Cpu &c = gCpus[gCpuCount++];
    c.id = t->cpus[i].cpu_id;
    c.phys = t->cpus[i].phys_id;
    c.domain = (c.phys >> 16) & 0xff;   // MPIDR Aff2: cluster 0 = cpu0-5 (domain 0), cluster 1 = cpu6-7 (domain 1)
    c.processor = cpu_to_processor(int(c.id));
    c.selfNs = 0;
    c.load = CoreLoad();
  }
}

bool processorRunning(processor_t p)
{
  processor_basic_info_data_t info;
  mach_msg_type_number_t count = PROCESSOR_BASIC_INFO_COUNT;
  host_t host;
  return p && processor_info(p, PROCESSOR_BASIC_INFO, &host, processor_info_t(&info), &count) == KERN_SUCCESS && info.running;
}

// The idle time of one core in ns (XNU's per-processor recount idle time, in scheduler ticks of 1/hz s: processor_info's
// PROCESSOR_CPU_LOAD_INFO, the same numbers host_processor_info gives top(1)).
bool idleNs(processor_t p, uint64_t &ns)
{
  processor_cpu_load_info_data_t info;
  mach_msg_type_number_t count = PROCESSOR_CPU_LOAD_INFO_COUNT;
  host_t host;
  if (!p || processor_info(p, PROCESSOR_CPU_LOAD_INFO, &host, processor_info_t(&info), &count) != KERN_SUCCESS) return false;
  ns = uint64_t(info.cpu_ticks[CPU_STATE_IDLE]) * (1000000000ull / uint64_t(hz > 0 ? hz : 100));
  return true;
}

// Busiest core of each domain since the last call, per mille, this thread's own time on each core left out; -1 where no core of the
// domain could be read, and for every domain right after the clock measurement (its window is dropped, gRebase).
void domainBusy(int32_t busy[kDomains])
{
  for (unsigned d = 0; d < kDomains; ++d) busy[d] = -1;
  const uint64_t now = nowNs();
  for (unsigned i = 0; i < gCpuCount; ++i) {
    Cpu &c = gCpus[i];
    uint64_t idle = 0;
    if (c.domain >= kDomains || !idleNs(c.processor, idle)) {
      c.load.fail();
      continue;
    }
    const int32_t b = c.load.read(now, idle, c.selfNs, gRebase);
    if (b > busy[c.domain]) busy[c.domain] = b;
  }
  gRebase = false;
}

// About the core's clock in MHz: 200000 dependent adds (one cycle each on an in-order chain), the fastest of three runs; no device
// access. Interrupts stay enabled (an interrupt only makes a run slower, and the fastest run wins).
uint32_t chainMhz()
{
  uint64_t best = ~0ull;
  for (unsigned run = 0; run < 3; ++run) {
    uint64_t x = 0;
    const uint64_t t0 = mach_absolute_time();
    for (unsigned i = 0; i < 200; ++i) __asm__ volatile(".rept 1000\n\tadd %0, %0, #1\n\t.endr" : "+r"(x));
    const uint64_t dt = mach_absolute_time() - t0;
    if (dt < best) best = dt;
  }
  uint64_t ns;
  absolutetime_to_nanoseconds(best, &ns);
  return ns ? uint32_t(200000ull * 1000ull / ns) : 0;
}

// Each running core's clock, measured on that core (thread_bind + thread_block, as the kernel's own per-CPU loops do). It runs on
// every core, so the governor's next load window would count it as load there: that window is dropped (gRebase).
void measureClocks(uint32_t mhz[kMaxCpus], bool on[kMaxCpus])
{
  gRebase = true;
  for (unsigned i = 0; i < gCpuCount; ++i) {
    mhz[i] = 0;
    on[i] = false;
    if (!processorRunning(gCpus[i].processor)) continue;
    thread_bind(gCpus[i].processor);
    thread_block(THREAD_CONTINUE_NULL);
    if (unsigned(cpu_number()) == gCpus[i].id) {
      mhz[i] = chainMhz();
      on[i] = true;
    }
  }
  thread_bind(PROCESSOR_NULL);
  thread_block(THREAD_CONTINUE_NULL);
}

// ------------------------------------------------------------------------------------------------------------- power source
PowerSource readPowerSource()
{
  OSDictionary *match = IOService::serviceMatching("IOPMPowerSource");
  if (!match) return PowerSource::unknown;
  IOService *ps = IOService::copyMatchingService(match);
  match->release();
  if (!ps) return PowerSource::unknown;
  PowerSource r = PowerSource::unknown;
  if (OSBoolean *b = OSDynamicCast(OSBoolean, ps->getProperty("ExternalConnected"))) r = b->isTrue() ? PowerSource::external : PowerSource::battery;
  ps->release();
  return r;
}

// PianoA830's reversible GPU ceiling (kHz, 0 clears) through its registered service, no link dependency: true when PianoA830 took it
// (kIOReturnSuccess). Until PianoA830 implements "piano-gpu-ceiling" its service answers kIOReturnUnsupported, and without
// piano-gpu=2 there is no service at all: false.
bool gpuCeiling(uint32_t khz)
{
  OSDictionary *match = IOService::serviceMatching("PianoA830");
  if (!match) return false;
  IOService *gpu = IOService::copyMatchingService(match);
  match->release();
  if (!gpu) return false;
  const IOReturn r = gpu->callPlatformFunction("piano-gpu-ceiling", false, reinterpret_cast<void *>(uintptr_t(khz)), nullptr, nullptr,
                                               nullptr);
  gpu->release();
  return r == kIOReturnSuccess;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------- suspend hooks
// For the system-suspend driver (docs/power.md's contract when it lands; until then these two C functions, or
// callPlatformFunction("piano-thermal-suspend-prepare" / "-finish") on the registered PianoThermal service):
//   prepare  masks every sensor's lower-threshold interrupt on the four controllers and pulses their clears (Linux 08e750858038 on
//            piano: a lower threshold crossed while the device cools in suspend refires, wake-armed, and aborts or ends every
//            sleep); the upper ones too while PianoThermal does not service the uplow interrupt itself (today: nothing would clear
//            it); the critical interrupts stay as they are. The masks found are kept.
//   finish   puts the masks found back.
// They work whatever piano-thermal says (the controllers are mapped by the hooks themselves), and are the only TSENS writes of
// this kext. prepare returns the number of controllers masked (0..4), -1 when already prepared.
namespace {
Window gSuspendTm[kControllers];
uint32_t gSavedUpLowMask[kControllers];
bool gSuspendSaved[kControllers];
uint32_t gSuspendState;            // 0 idle, 1 prepared
constexpr bool kServicesUplow = false;   // this path: the uplow interrupt not serviced (piano-thermal-irq live: irq::hookSuspend*)
}

extern "C" int piano_thermal_suspend_prepare(void)
{
  uint32_t idle = 0;
  if (!__atomic_compare_exchange_n(&gSuspendState, &idle, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return -1;
  const int uplow = irq::hookSuspendPrepare();   // piano-thermal-irq live: lower masked, upper kept armed (PianoThermalIrq.cpp)
  if (uplow >= 0) return uplow;
  int done = 0;
  char line[200];
  int at = 0;
  line[0] = 0;
  for (unsigned c = 0; c < kControllers; ++c) {
    gSuspendSaved[c] = false;
    if (!gSuspendTm[c].va && !mapWindow(gSuspendTm[c], kMap[c].tm, kTmBytes)) {
      gSuspendTm[c] = {};
      continue;
    }
    const uintptr_t va = gSuspendTm[c].va;
    const uint32_t found = rd32(va + kTmUpLowIntMask);
    const SuspendMask sm = suspendMask(found, kMap[c].count, !kServicesUplow);
    wr32(va + kTmUpLowIntMask, sm.mask);
    wr32(va + kTmUpLowIntClear, sm.clear);
    wr32(va + kTmUpLowIntClear, 0);
    gSavedUpLowMask[c] = found;
    gSuspendSaved[c] = true;
    ++done;
    at = catf(line, sizeof line, at, " tsens%u %#x->%#x", c, found, sm.mask);
  }
  __builtin_arm_dsb(0xf);
  note("suspend prepare: lower%s threshold interrupts masked on %d controllers:%s", kServicesUplow ? "" : " and upper", done, line);
  return done;
}

extern "C" void piano_thermal_suspend_finish(void)
{
  if (__atomic_load_n(&gSuspendState, __ATOMIC_ACQUIRE) != 1) return;
  if (irq::hookSuspendFinish()) {   // the masks are the uplow path's: nothing of this path's to restore
    __atomic_store_n(&gSuspendState, 0u, __ATOMIC_RELEASE);
    return;
  }
  int done = 0;
  for (unsigned c = 0; c < kControllers; ++c) {
    if (!gSuspendSaved[c] || !gSuspendTm[c].va) continue;
    wr32(gSuspendTm[c].va + kTmUpLowIntMask, gSavedUpLowMask[c]);
    gSuspendSaved[c] = false;
    ++done;
  }
  __builtin_arm_dsb(0xf);
  __atomic_store_n(&gSuspendState, 0u, __ATOMIC_RELEASE);
  note("suspend finish: the threshold interrupt masks found before suspend are back on %d controllers", done);
}

// ------------------------------------------------------------------------------------------------------------- the service
class PianoThermal : public IOService {
  OSDeclareDefaultStructors(PianoThermal)

  static void thread(void *self, wait_result_t);
  void run();
  void surveyControllers();
  void peekChannel();
  void report(const Monitor &m, bool clock);
  void publish(const Monitor &m);
  void protect(const PolicyEvent &e);
  void cpuStart();
  void cpuTick(const Monitor &m, bool slow, bool loadTick);
  void cpuApply(unsigned d, unsigned want, uint64_t now);
  void cpuReport(uint64_t spanNs);

  Monitor monitor_;
  Policy policy_;
  int32_t peakDeci_ = 0;
  unsigned peakC_ = 0, peakS_ = 0;
  bool peakValid_ = false, parked_ = false, gpuCapped_ = false, halting_ = false, criticalPendingLogged_ = false;
  uint64_t criticalLogNs_ = 0;
  // cpufreq
  scmi::Channel channel_;
  scmi::Protocol protocol_{};
  scmi::Domain domain_[kDomains];
  Governor governor_[kDomains];
  bool domainOk_[kDomains] = {};
  int boot_[kDomains] = {-1, -1};
  unsigned set_[kDomains] = {};          // the level last set (or found)
  uint64_t residencyNs_[kDomains][scmi::kMaxLevels] = {};
  uint64_t busySum_[kDomains] = {}, busyTicks_[kDomains] = {};
  uint32_t busyMax_[kDomains] = {}, changes_[kDomains] = {}, refusals_[kDomains] = {}, cuts_[kDomains] = {};
  uint64_t limitedPolls_[kDomains] = {};
  uint64_t levelSinceNs_[kDomains] = {};
  PowerSource power_ = PowerSource::unknown;
  uint64_t powerAtNs_ = 0;
  bool cpuOk_ = false;
  // PianoPolicy's profile (any thread writes it; the governor reads it each tick) and what the 30 s report last named
  volatile uint32_t profile_ = 0;
  // system sleep: the thread parks between kOff and kOn (SleepPlan says what each does)
  IOLock *sleepLock_ = nullptr;
  SleepPlan sleep_;
  bool sleepWanted_ = false, threadParked_ = false, threadRunning_ = false;
  bool park();
  void pollWait(uint32_t ms);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                void *param4) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
};

OSDefineMetaClassAndStructors(PianoThermal, IOService)

// "piano-thermal-suspend-prepare" (param1: int *, the result, may be null) and "piano-thermal-suspend-finish": the suspend hooks
// above for a caller that does not link against this kext.
IOReturn PianoThermal::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                            void *param3, void *param4)
{
  if (functionName && functionName->isEqualTo("piano-thermal-suspend-prepare")) {
    const int r = piano_thermal_suspend_prepare();
    if (param1) *static_cast<int *>(param1) = r;
    return r < 0 ? kIOReturnBusy : kIOReturnSuccess;
  }
  if (functionName && functionName->isEqualTo("piano-thermal-suspend-finish")) {
    piano_thermal_suspend_finish();
    return kIOReturnSuccess;
  }
  if (functionName && functionName->isEqualTo("piano-thermal-board-temps"))   // B6: param1 = struct piano_board_temps *
    return adc::hookBoardTemps(param1) ? kIOReturnSuccess : kIOReturnBadArgument;
  // docs/policy.md 5.2: param1 = the mode (0 by the power source, 1 Power Saver, 2 Balanced). Taken by the governor (mode 3) at its
  // next tick and by the fixed level (mode 2, which has no power caps: ignored there); without CPU frequency control unsupported.
  if (functionName && functionName->isEqualTo("piano-thermal-profile")) {
    const uintptr_t mode = uintptr_t(param1);
    if (!profileValid(mode)) return kIOReturnBadArgument;
    if (gArgs.cpufreq < 2) return kIOReturnUnsupported;
    const uint32_t before = __atomic_exchange_n(&profile_, uint32_t(mode), __ATOMIC_ACQ_REL);
    if (before != mode) {
      setProperty("piano-thermal-profile", uint64_t(mode), 32);
      note("[%us] cpufreq: profile %s (PianoPolicy)%s", upS(), profileName(Profile(mode)),
           gArgs.cpufreq == 2 ? ": the fixed level (piano-cpufreq=2) has no power caps, ignored" : "");
    }
    return kIOReturnSuccess;
  }
  return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

// The thread's wait between polls; a system sleep's park request ends it early.
void PianoThermal::pollWait(uint32_t ms)
{
  if (irq::hookSleep(ms, &sleepWanted_)) return;   // piano-thermal-irq live: an uplow interrupt or the park request ends it
  IOLockLock(sleepLock_);
  if (!sleepWanted_) {
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    IOLockSleepDeadline(sleepLock_, &sleepWanted_, deadline, THREAD_UNINT);
  }
  IOLockUnlock(sleepLock_);
}

// At the top of each poll (no SCMI request or TSENS read in flight): parked here from kOff to kOn. True after a park.
bool PianoThermal::park()
{
  IOLockLock(sleepLock_);
  const bool parking = sleepWanted_;
  if (parking) {
    threadParked_ = true;
    IOLockWakeup(sleepLock_, &threadParked_, false);
    while (sleepWanted_) IOLockSleep(sleepLock_, &sleepWanted_, THREAD_UNINT);
    threadParked_ = false;
  }
  IOLockUnlock(sleepLock_);
  return parking;
}

// docs/power.md 4: off = the thread parked (no SCMI request: the clusters stay at their level; no TSENS access) and, with
// piano-thermal-sleep-mask=1, the threshold interrupts masked (the suspend hooks; Ubuntu's 08e750858038); on = the masks back, the
// thread on (its first zone update at once). Synchronous: the park waits for the poll in progress (at most a few ms of SCMI).
IOReturn PianoThermal::setPowerState(unsigned long state, IOService *)
{
  if (!sleepLock_) return pianopower::kAckNow;
  IOLockLock(sleepLock_);
  const SleepSteps s = state == pianopower::kOff ? sleep_.off() : sleep_.on();
  if (s.park && threadRunning_) {
    sleepWanted_ = true;
    IOLockWakeup(sleepLock_, &sleepWanted_, false);
    irq::hookKick();   // and the uplow path's wait
    uint64_t deadline = 0;
    clock_interval_to_deadline(500, kMillisecondScale, &deadline);
    while (!threadParked_ && IOLockSleepDeadline(sleepLock_, &threadParked_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
    }
  }
  const bool parked = threadParked_;
  IOLockUnlock(sleepLock_);
  if (s.park) {
    int masked = 0;
    if (s.mask) sleep_.maskResult(masked = piano_thermal_suspend_prepare());
    note("[%us] sleep: %s; %s", upS(), !threadRunning_ ? "no thread to park" : parked ? "the thread is parked (no SCMI, no TSENS)"
                                                                                       : "the thread did not park within 500 ms",
         !s.mask ? "threshold interrupts as they are (piano-thermal-sleep-mask=0)"
                 : masked < 0 ? "threshold interrupts already masked by another caller" : "threshold interrupts masked");
  }
  if (s.finish) piano_thermal_suspend_finish();
  if (s.unpark) {
    IOLockLock(sleepLock_);
    sleepWanted_ = false;
    IOLockWakeup(sleepLock_, &sleepWanted_, false);
    IOLockUnlock(sleepLock_);
  }
  return pianopower::kAckNow;
}

IOService *PianoThermal::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoThermal::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  gStartNs = nowNs();
  note("start: piano-thermal=%u piano-cpufreq=%u shutdown=%u poll %u ms, log every %u s, warm %u C, critical margin %u C, clock %u, "
       "srot %u, delay %u s",
       gArgs.level, gArgs.cpufreq, gArgs.shutdown, gArgs.pollMs, gArgs.logS, gArgs.warmC, gArgs.marginC, gArgs.clock, gArgs.srot,
       gArgs.delay);
  note("level %u: %s%s", gArgs.level,
       gArgs.level >= 2 ? "readout + protection (GPU park at the stock passive trip, alarm at critical - margin)"
                        : "readout only: loads, nothing written",
       gArgs.cpufreq ? "; piano-cpufreq: SCMI messages to the CPUCP (shared memory + doorbell)" : "");
  PolicyConfig pc;
  pc.warmDeci = int32_t(gArgs.warmC) * 10;
  pc.criticalMarginDeci = int32_t(gArgs.marginC) * 10;
  policy_ = Policy(pc);
  setProperty("piano-thermal", gArgs.level, 32);
  setProperty("piano-cpufreq", gArgs.cpufreq, 32);
  if (gArgs.cpufreq >= 2) setProperty("piano-thermal-profile", 0ull, 32);
  sleepLock_ = IOLockAlloc();
  // piano-thermal-irq=1 uses the threshold interrupts, so a sleep masks them (the hooks: lower only while it is live, as Linux)
  sleep_ = SleepPlan(gArgs.sleepMask != 0 || irq::hookLevel() != 0);
  registerService();
  if (gArgs.clock == 1) gDisplay.start(nullptr, nullptr);   // the clock line waits while the display is off
  adc::hookStart(this, adc::hookLevel(), gArgs.logS, gArgs.delay);   // B6 (PianoThermalAdc.cpp): nothing without piano-thermal-adc
  thread_t t = nullptr;
  if (!sleepLock_ || kernel_thread_start(&PianoThermal::thread, this, &t) != KERN_SUCCESS) {
    note("FAILED: no thread");
    return true;
  }
  thread_deallocate(t);
  threadRunning_ = true;
  note("system sleep: in the power tree (%#x): the thread parks while suspended%s", pianopower::join(this),
       gArgs.sleepMask ? ", the threshold interrupts masked (piano-thermal-sleep-mask=1)"
       : irq::hookLevel() ? ", the threshold interrupts masked (piano-thermal-irq=1: the lower ones while it is live)" : "");
  return true;
}

void PianoThermal::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-thermal");
  static_cast<PianoThermal *>(self)->run();
}

void PianoThermal::surveyControllers()
{
  for (unsigned c = 0; c < kControllers; ++c) {
    const Controller &k = kMap[c];
    if (!gTm[c].va) {
      note("tsens%u %s @%#llx: NOT MAPPED", c, k.name, (unsigned long long)k.tm);
      continue;
    }
    char srot[160] = "SROT skipped (piano-thermal-srot=0)";
    if (gSrot[c].va) {
      const uint32_t ver = srotRead(c, kSrotVersion), ctrl = srotRead(c, kSrotCtrl);
      const Version v = decodeVersion(ver);
      const Ctrl k2 = decodeCtrl(ctrl);
      snprintf(srot, sizeof srot, "version %u.%u.%u (%#x), ctrl %#x: %s, %u sensors enabled (DT %u%s), result %s", v.major, v.minor,
               v.step, ver, ctrl, k2.enabled ? "enabled" : "NOT ENABLED", popcount(k2.sensorMask), k.count,
               popcount(k2.sensorMask) == k.count ? ", matches" : ", DIFFERS", k2.temperatureFormat ? "temperature" : "ADC CODES");
    } else if (gArgs.srot) {
      snprintf(srot, sizeof srot, "SROT @%#llx not mapped", (unsigned long long)k.srot);
    }
    int32_t critLo = 0, critHi = 0, upLo = 0, upHi = 0;
    for (unsigned s = 0; s < k.count; ++s) {
      const Thresholds th = decodeThresholds(tsensRead(nullptr, c, kTmUpLowThreshold + 4 * s), tsensRead(nullptr, c, kTmCriticalThreshold + 4 * s));
      if (!s || th.criticalDeci < critLo) critLo = th.criticalDeci;
      if (!s || th.criticalDeci > critHi) critHi = th.criticalDeci;
      if (!s || th.upperDeci < upLo) upLo = th.upperDeci;
      if (!s || th.upperDeci > upHi) upHi = th.upperDeci;
    }
    note("tsens%u %s @%#llx: %s", c, k.name, (unsigned long long)k.tm, srot);
    note("tsens%u %s: TM int-en %#x, up/low status %#x, critical status %#x, trdy %#x; hardware thresholds: critical %s..%s C, upper "
         "%s..%s C over %u sensors",
         c, k.name, tsensRead(nullptr, c, kTmIntEn), tsensRead(nullptr, c, kTmUpLowIntStatus), tsensRead(nullptr, c, kTmCriticalIntStatus),
         tsensRead(nullptr, c, kTmTrdy), Deci(critLo).s, Deci(critHi).s, Deci(upLo).s, Deci(upHi).s, k.count);
  }
}

void PianoThermal::peekChannel()
{
  if (!gShmem.va) {
    note("scmi: tx channel @%#llx not mapped", (unsigned long long)scmi::kShmemTx);
    return;
  }
  const uint32_t st = shmRead(nullptr, scmi::kChanStatus), h = shmRead(nullptr, scmi::kHeader);
  const scmi::Header hd = scmi::decodeHeader(h);
  note("scmi: tx channel @%#llx (read only): status %#x (%s%s), flags %#x, length %u, last header %#x (protocol %#x message %#x token %u)",
       (unsigned long long)scmi::kShmemTx, st, (st & scmi::kStatusFree) ? "free" : "BUSY", (st & scmi::kStatusError) ? ", error" : "",
       shmRead(nullptr, scmi::kChanFlags), shmRead(nullptr, scmi::kLength), h, hd.protocol, hd.message, hd.token);
}

void PianoThermal::report(const Monitor &m, bool clock)
{
  char line[480];
  int at = snprintf(line, sizeof line, "[%us]", upS());
  for (unsigned c = 0; c < kControllers && at < int(sizeof line); ++c) {
    int32_t d;
    unsigned s;
    if (m.hottest(c, d, s))
      at = catf(line, sizeof line, at, "%s %s %s C (s%u %s)", c ? "," : "", kMap[c].name, Deci(d).s, s, kMap[c].sensors[s].name);
    else
      at = catf(line, sizeof line, at, "%s %s no reading", c ? "," : "", kMap[c].name);
  }
  if (at < int(sizeof line)) {
    if (peakValid_)
      at = catf(line, sizeof line, at, "; peak %s C (%s s%u)", Deci(peakDeci_).s, kMap[peakC_].name, peakS_);
    at = catf(line, sizeof line, at, "; state %s%s", stateName(policy_.state()), policy_.unknown() ? " UNKNOWN" : "");
  }
  uint32_t flags = 0;
  const int32_t gpu = adreno830_thermal_state(&flags);
  if (at < int(sizeof line) && (flags & kA830Valid))
    at = catf(line, sizeof line, at, "; a830 %s C%s%s%s", Deci(gpu).s, (flags & kA830Limiting) ? " limiting" : "",
             (flags & kA830Emergency) ? " EMERGENCY" : "", parked_ ? " parked" : "");
  note("%s", line);
  note("[%us] reads: %llu polls, %llu loads, %llu without VALID, %llu implausible, %llu stale stand-ins", upS(),
       (unsigned long long)m.polls(), (unsigned long long)m.reads(), (unsigned long long)m.invalidReads(),
       (unsigned long long)m.implausible(), (unsigned long long)m.staleUses());
  // Not while the display is off (2026-11-12): binding a thread onto every running core wakes each idle one for a diagnostic line.
  if (clock && gArgs.clock && (gArgs.clock >= 2 || !gDisplay.dark())) {
    uint32_t mhz[kMaxCpus];
    bool on[kMaxCpus];
    measureClocks(mhz, on);
    at = snprintf(line, sizeof line, "[%us] clock (dependent adds, MHz):", upS());
    for (unsigned i = 0; i < gCpuCount && at < int(sizeof line); ++i) {
      if (on[i]) at = catf(line, sizeof line, at, " cpu%u/%u %u", gCpus[i].id, gCpus[i].domain, mhz[i]);
      else at = catf(line, sizeof line, at, " cpu%u/%u off", gCpus[i].id, gCpus[i].domain);
    }
    note("%s", line);
  }
}

void PianoThermal::publish(const Monitor &m)
{
  OSDictionary *t = OSDictionary::withCapacity(8);
  if (!t) return;
  for (unsigned c = 0; c < kControllers; ++c) {
    int32_t d;
    unsigned s;
    if (!m.hottest(c, d, s)) continue;
    if (OSNumber *n = OSNumber::withNumber(uint64_t(int64_t(d)), 32)) {
      t->setObject(kMap[c].name, n);
      n->release();
    }
  }
  setProperty("TemperaturesDeciC", t);
  t->release();
  int16_t all[kControllers][kMaxSensors];
  for (unsigned c = 0; c < kControllers; ++c)
    for (unsigned s = 0; s < kMaxSensors; ++s) all[c][s] = m.usable(c, s) ? int16_t(m.deci(c, s)) : int16_t(-32768);
  if (OSData *data = OSData::withBytes(all, sizeof all)) {
    setProperty("SensorsDeciC", data);   // [controller][sensor] int16, -32768 = no reading
    data->release();
  }
  setProperty("ThermalState", stateName(policy_.state()));
  if (peakValid_) setProperty("PeakDeciC", uint64_t(int64_t(peakDeci_)), 32);
}

void PianoThermal::protect(const PolicyEvent &e)
{
  const Sensor &sn = kMap[e.controller].sensors[e.sensor];
  if (e.unknownAfter != e.unknownBefore) {
    if (e.unknownAfter) note("[%us] NO READING: no TSENS sensor gave a usable reading for %u s; protection is blind", upS(),
                             policy_.config().unknownMs / 1000);
    else note("[%us] readings are back", upS());
  }
  if (e.before != e.after)
    note("[%us] state %s -> %s: %s s%u %s %s C", upS(), stateName(e.before), stateName(e.after), kMap[e.controller].name, e.sensor,
         sn.name, Deci(e.deci).s);
  if (gArgs.level < 2) return;
  // The GPU: at passive a reversible ceiling (PianoA830's "piano-gpu-ceiling", once it has one: docs/drivers/thermal.md section
  // 10), else the boot-long park; at critical always the park.
  if (e.after >= State::passive && !parked_ && !gpuCapped_) {
    const int32_t critical = sn.criticalDeci - int32_t(gArgs.marginC) * 10;
    const int32_t passive = passiveOf(sn) < critical - 50 ? passiveOf(sn) : critical - 50;
    if (gpuCeiling(gArgs.gpuMhz * 1000u)) {
      gpuCapped_ = true;
      note("[%us] PASSIVE: %s %s C >= %s C (stock passive %s C): GPU capped at %u MHz until the state is below passive "
           "(PianoA830 piano-gpu-ceiling)",
           upS(), sn.name, Deci(e.deci).s, Deci(passive).s, Deci(passiveOf(sn)).s, gArgs.gpuMhz);
    } else {
      parked_ = true;
      adreno830_dcvs_park();
      note("[%us] PASSIVE: %s %s C >= %s C (stock passive %s C): GPU parked at its lowest level for the rest of the boot "
           "(adreno830_dcvs_park; PianoA830 has no reversible ceiling)",
           upS(), sn.name, Deci(e.deci).s, Deci(passive).s, Deci(passiveOf(sn)).s);
    }
  }
  if (e.after == State::critical && !parked_) {
    parked_ = true;
    adreno830_dcvs_park();
    note("[%us] CRITICAL: GPU parked at its lowest level for the rest of the boot (adreno830_dcvs_park)", upS());
  }
  if (e.after < State::passive && gpuCapped_ && !parked_) {
    gpuCapped_ = false;
    gpuCeiling(0);
    note("[%us] GPU ceiling cleared (state %s)", upS(), stateName(e.after));
  }
  if (!e.criticalPending && e.after != State::critical) criticalPendingLogged_ = false;
  const uint64_t now = nowNs();
  if (e.after == State::critical && (e.before != State::critical || now - criticalLogNs_ >= 5000000000ull)) {
    criticalLogNs_ = now;   // at entry, then every 5 s while it lasts
    note("[%us] CRITICAL: %s %s C >= %s C (stock critical %s C - %u C margin)%s; %s", upS(), sn.name, Deci(e.deci).s,
         Deci(sn.criticalDeci - int32_t(gArgs.marginC) * 10).s, Deci(sn.criticalDeci).s, gArgs.marginC,
         gArgs.cpufreq >= 2 ? "; CPU clusters at their slowest level" : "",
         gArgs.shutdown ? "HALTING the system (piano-thermal-shutdown=1)" : "piano-thermal-shutdown=0: NOT shutting down (log only)");
    if (gArgs.shutdown && !halting_) {
      halting_ = true;
      IOSleep(200);   // let the console drain
      static char why[] = "PianoThermal: critical temperature";
      reboot_kernel(kRbHalt, why);
      note("[%us] reboot_kernel(RB_HALT) returned: the system did not halt", upS());
    }
  } else if (e.criticalPending && !criticalPendingLogged_) {
    criticalPendingLogged_ = true;
    note("[%us] critical threshold met (hottest %s s%u %s C): critical if it holds for %u ms", upS(), kMap[e.controller].name, e.sensor,
         Deci(e.deci).s, policy_.config().criticalHoldMs);
  }
}

// ------------------------------------------------------------------------------------------------------------- cpufreq
void PianoThermal::cpuStart()
{
  readTopology();
  if (!gShmem.va || !gMbox.va) {
    note("cpufreq: SCMI channel or doorbell not mapped (%#llx, %#llx); off", (unsigned long long)scmi::kShmemTx,
         (unsigned long long)scmi::kMboxTx);
    return;
  }
  channel_.io = {nullptr, shmRead, shmWrite, ring, scmiNowUs, scmiPause};
  int32_t r = scmi::queryProtocol(channel_, protocol_);
  if (r) {
    note("cpufreq: SCMI performance protocol: no answer (%d, %s); off for this boot", r, scmi::statusName(r));
    return;
  }
  note("cpufreq: SCMI performance protocol %u.%u, %u domains", protocol_.version >> 16, protocol_.version & 0xffff, protocol_.domains);
  const CpuMode mode = CpuMode(gArgs.cpufreq);
  for (unsigned d = 0; d < kDomains && d < protocol_.domains; ++d) {
    scmi::Domain &dom = domain_[d];
    r = scmi::describeDomain(channel_, protocol_.version, d, dom);
    if (r) {
      note("cpufreq: domain %u: no description (%d, %s)", d, r, scmi::statusName(r));
      continue;
    }
    char list[300];
    int at = 0;
    list[0] = 0;
    for (unsigned i = 0; i < dom.count && at < int(sizeof list) - 12; ++i)
      at = catf(list, sizeof list, at, " %u", dom.levels[i].khz / 1000);
    note("cpufreq: domain %u \"%s\": attributes %#x (%s, %s), rate limit %u us, sustained %u kHz (level %u -> %u MHz), %u levels (MHz):%s", d,
         dom.name, dom.attributes, dom.canSet ? "level set allowed" : "LEVEL SET NOT ALLOWED", dom.indexing ? "by index" : "by value",
         dom.rateLimitUs, dom.sustainedKhz, dom.sustainedLevel, dom.levels[dom.sustained].khz / 1000, dom.count, list);
    int idx = -1;
    r = scmi::levelGet(channel_, dom, d, idx);
    if (r || idx < 0) {
      note("cpufreq: domain %u: current level unknown (%d, %s)", d, r, scmi::statusName(r));
      continue;
    }
    boot_[d] = idx;
    set_[d] = unsigned(idx);
    note("cpufreq: domain %u: at boot %u MHz (level %d of %u%s)", d, dom.levels[idx].khz / 1000, idx + 1, dom.count,
         unsigned(idx) >= dom.sustained ? ", at or above sustained: the boot stages left it fast" : "");
    uint32_t khz[scmi::kMaxLevels];
    for (unsigned i = 0; i < dom.count; ++i) khz[i] = dom.levels[i].khz;
    GovernorConfig gc;
    gc.capKhz = mode == CpuMode::fixed || gArgs.capGiven[d] ? capKhz(gArgs.cap[d]) : 0;
    gc.batteryKhz = capKhz(gArgs.bat[d]);
    gc.externalKhz = capKhz(gArgs.ac[d]);
    gc.floorKhz = gArgs.minMhz[d] * 1000;
    gc.hispeedKhz = gArgs.hispeed[d] * 1000;
    gc.stepMs = gArgs.fastMs;
    governor_[d].init(dom.canSet ? mode : CpuMode::report, gc, khz, dom.count, dom.sustained, unsigned(idx));
    levelSinceNs_[d] = nowNs();
    domainOk_[d] = true;
    if (mode >= CpuMode::fixed) {
      if (!dom.canSet) note("cpufreq: domain %u: the firmware does not allow LEVEL_SET; reporting only", d);
      else if (mode == CpuMode::fixed)
        note("cpufreq: domain %u: fixed at the fastest level <= %u MHz (and <= sustained): %u MHz; thermal limit: %s", d, gArgs.cap[d],
             dom.levels[governor_[d].ceiling(PowerSource::battery)].khz / 1000, tripsText());
      else
        note("cpufreq: domain %u: governor every %u ms, target 80 %% busy, hispeed %u MHz, floor %u MHz, caps: battery %u MHz, power %u "
             "MHz%s; thermal limit: %s",
             d, gArgs.tickMs, gArgs.hispeed[d], gArgs.minMhz[d], gArgs.bat[d], gArgs.ac[d], gc.capKhz ? ", fixed cap given" : "",
             tripsText());
    }
  }
  cpuOk_ = domainOk_[0] || domainOk_[1];
  power_ = readPowerSource();
  powerAtNs_ = nowNs();
  if (mode >= CpuMode::fixed) note("cpufreq: power source: %s", powerName(power_));
}

// The level the governor wants for domain d, sent when it differs from the one set last.
void PianoThermal::cpuApply(unsigned d, unsigned want, uint64_t now)
{
  Governor &g = governor_[d];
  if (want == set_[d]) return;
  const int32_t r = scmi::levelSet(channel_, domain_[d], d, want);
  if (r) {
    if (++refusals_[d] <= 3 || refusals_[d] % 100 == 0)
      note("[%us] cpufreq: domain %u: %u MHz refused (%d, %s; %u refusals)", upS(), d, domain_[d].levels[want].khz / 1000, r,
           scmi::statusName(r), refusals_[d]);
    g.setLevel(set_[d]);
    if (refusals_[d] >= 20) {
      note("[%us] cpufreq: domain %u: 20 refusals: left at %u MHz for this boot", upS(), d, domain_[d].levels[set_[d]].khz / 1000);
      domainOk_[d] = false;
    }
    return;
  }
  residencyNs_[d][set_[d]] += now - levelSinceNs_[d];
  levelSinceNs_[d] = now;
  set_[d] = want;
  ++changes_[d];
}

// Every sensor poll: the thermal limit (fixed and governor modes) and, at most once per piano-cpufreq-tick-ms (governor) or at
// once (fixed), the level; report mode asks LEVEL_GET at the log cadence only (slow).
void PianoThermal::cpuTick(const Monitor &m, bool slow, bool loadTick)
{
  if (!cpuOk_) return;
  if (channel_.lastError) {
    note("[%us] cpufreq: the SCMI channel failed (%d, %s): no more messages this boot; the clusters stay where they are", upS(),
         channel_.lastError, scmi::statusName(channel_.lastError));
    cpuOk_ = false;
    return;
  }
  const uint64_t now = nowNs();
  if (now - powerAtNs_ >= 10000000000ull) {
    const PowerSource p = readPowerSource();
    if (p != power_ && gArgs.cpufreq == 3) note("[%us] cpufreq: power source: %s -> %s", upS(), powerName(power_), powerName(p));
    power_ = p;
    powerAtNs_ = now;
  }
  const PowerSource power = profilePower(power_, Profile(__atomic_load_n(&profile_, __ATOMIC_ACQUIRE)));
  int32_t busy[kDomains] = {-1, -1};
  if (loadTick) domainBusy(busy);
  // A load decision needs a busy figure: none on the first reading (it only starts the window), after the clock measurement, or
  // when no core of the domain could be read. Before 2026-11-12 such a tick counted as 0 % busy, a step toward the slowest level.
  const bool critical = gArgs.level >= 2 && policy_.state() == State::critical;
  for (unsigned d = 0; d < kDomains; ++d) {
    if (!domainOk_[d]) continue;
    Governor &g = governor_[d];
    if (g.mode() == CpuMode::report) {
      if (!slow) continue;
      int idx = -1;
      if (!scmi::levelGet(channel_, domain_[d], d, idx) && idx >= 0 && unsigned(idx) != set_[d]) {
        residencyNs_[d][set_[d]] += now - levelSinceNs_[d];
        levelSinceNs_[d] = now;
        set_[d] = unsigned(idx);
        ++changes_[d];
      }
      continue;
    }
    const unsigned limitBefore = g.limit();
    if (g.thermal(clusterThermal(m, d, gTrips, critical), now / 1000000ull) && g.limit() < limitBefore && g.limit() == 0)
      ++cuts_[d];
    if (g.limit() < g.count() - 1u) ++limitedPolls_[d];
    if (busy[d] >= 0) {
      busySum_[d] += uint32_t(busy[d]);
      ++busyTicks_[d];
      if (uint32_t(busy[d]) > busyMax_[d]) busyMax_[d] = uint32_t(busy[d]);
    }
    const unsigned want =
        loadTick && busy[d] >= 0 && g.mode() == CpuMode::govern ? g.tick({uint32_t(busy[d]), power}) : g.enforce(power);
    cpuApply(d, want, now);
  }
}

void PianoThermal::cpuReport(uint64_t spanNs)
{
  if (!cpuOk_ && !channel_.messages) return;
  const uint64_t now = nowNs();
  const Profile profile = Profile(__atomic_load_n(&profile_, __ATOMIC_ACQUIRE));
  const PowerSource power = profilePower(power_, profile);
  for (unsigned d = 0; d < kDomains; ++d) {
    if (boot_[d] < 0) continue;
    const scmi::Domain &dom = domain_[d];
    residencyNs_[d][set_[d]] += now - levelSinceNs_[d];
    levelSinceNs_[d] = now;
    char line[480];
    int at = snprintf(line, sizeof line, "[%us] cpufreq d%u %s: now %u MHz (boot %u MHz);", upS(), d, dom.name,
                      dom.levels[set_[d]].khz / 1000, dom.levels[boot_[d]].khz / 1000);
    // The window these residencies cover: the first one starts at the domain's init, before the thread's first report (whose
    // spanNs is a few ms: "1996 MHz 26718%", queue5 P5); later ones equal spanNs.
    uint64_t windowNs = 0;
    for (unsigned i = 0; i < dom.count; ++i) windowNs += residencyNs_[d][i];
    if (!windowNs) windowNs = spanNs;
    for (unsigned i = 0; i < dom.count && at < int(sizeof line) - 24; ++i) {
      const uint64_t pct = windowNs ? residencyNs_[d][i] * 100 / windowNs : 0;
      if (pct) at = catf(line, sizeof line, at, " %u MHz %llu%%", dom.levels[i].khz / 1000, (unsigned long long)pct);
      residencyNs_[d][i] = 0;
    }
    const Governor &g = governor_[d];
    if (at < int(sizeof line))
      at = catf(line, sizeof line, at, "; %u changes; busy avg %llu%% max %u%%; ceiling %u MHz (%s%s%s), thermal limit %u MHz (%llu "
               "limited polls, %u cuts)",
               changes_[d], (unsigned long long)(busyTicks_[d] ? busySum_[d] / busyTicks_[d] / 10 : 0), busyMax_[d] / 10,
               g.khz(g.ceiling(power)) / 1000, powerName(power_), profile == Profile::bySource ? "" : "; profile ",
               profile == Profile::bySource ? "" : profileName(profile), g.khz(g.limit()) / 1000, (unsigned long long)limitedPolls_[d],
               cuts_[d]);
    note("%s", line);
    changes_[d] = 0;
    busySum_[d] = busyTicks_[d] = 0;
    busyMax_[d] = 0;
    limitedPolls_[d] = 0;
    cuts_[d] = 0;
  }
  note("[%us] scmi: %u messages, %u failed, %llu us each on average, %u us at most; own work %llu us/s (left out of busy)", upS(),
       channel_.messages, channel_.failures, (unsigned long long)(channel_.messages ? channel_.totalUs / channel_.messages : 0),
       channel_.maxUs, (unsigned long long)(spanNs >= 1000000000ull ? gSelfNsTotal / (spanNs / 1000000000ull) / 1000 : 0));
  gSelfNsTotal = 0;
  OSDictionary *dict = OSDictionary::withCapacity(4);
  if (dict) {
    for (unsigned d = 0; d < kDomains; ++d) {
      if (boot_[d] < 0) continue;
      if (OSNumber *n = OSNumber::withNumber(uint64_t(domain_[d].levels[set_[d]].khz / 1000), 32)) {
        dict->setObject(d ? "cluster1-MHz" : "cluster0-MHz", n);
        n->release();
      }
    }
    setProperty("CpuFreq", dict);
    dict->release();
  }
}

// ------------------------------------------------------------------------------------------------------------- the thread
void PianoThermal::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  for (unsigned c = 0; c < kControllers; ++c) {
    if (!mapWindow(gTm[c], kMap[c].tm, kTmBytes)) gTm[c] = {};
    if (gArgs.srot && !mapWindow(gSrot[c], kMap[c].srot, kSrotBytes)) gSrot[c] = {};
  }
  if (!mapWindow(gShmem, scmi::kShmemTx, 0x1000)) gShmem = {};
  else gShmem.bytes = scmi::kShmemBytes;
  if (gArgs.cpufreq && !mapWindow(gMbox, scmi::kMboxTx, 0x1000)) gMbox = {};
  surveyControllers();
  peekChannel();
  if (gArgs.cpufreq) cpuStart();
  else readTopology();
  const bool governing = cpuOk_ && gArgs.cpufreq == 3;
  // The poll period by tier (Cluster.h pollTier): base = the governor's tick (100 ms) or 1 s; warm (hottest >= 70 C) 250 ms; clock
  // (a cluster above piano-thermal-fast-mhz, every sensor under piano-thermal-fast-temp) 100 ms; fast (10 ms) within 10 C of a trip
  // or of the ceiling, while a cluster is thermally limited, or above the fast-poll clock from the fast-poll temperature up.
  const uint32_t base = governing ? gArgs.tickMs : gArgs.pollMs;
  note("running: %s every %u ms, %u ms from %u C, %u ms above %u MHz (%u ms from %u C), %u ms near a trip; the policy every %u ms; a "
       "report every %u s%s",
       governing ? "the governor and the sensors" : "the sensors", base, pollPeriodMs(Poll::warm, base, gArgs.warmMs, gArgs.fastMs),
       gArgs.warmPollC, pollPeriodMs(Poll::clock, base, gArgs.warmMs, gArgs.fastMs, gArgs.clockMs), gArgs.fastMhz, gArgs.fastMs,
       gArgs.fastTempC, gArgs.fastMs, gArgs.pollMs, gArgs.logS, gMapFailures ? " (some windows not mapped)" : "");
  const uintptr_t tmVa[kControllers] = {gTm[0].va, gTm[1].va, gTm[2].va, gTm[3].va};
  irq::hookStart(this, tmVa, base, governing, gArgs.logS, gStartNs);   // piano-thermal-irq (PianoThermalIrq.cpp): nothing without it
  uint64_t lastPolicy = 0, lastLoad = 0, lastLog = nowNs(), lastCpuLog = nowNs(), tierSince = nowNs();
  uint64_t tierNs[kPollTiers] = {};
  uint32_t tierChanges = 0;
  Poll tier = Poll::base;
  bool first = true, fastNoted = false;
  for (;;) {
    if (park()) {   // back from a system sleep: the power source read again at the next CPU tick, the zones now
      powerAtNs_ = 0;
      note("[%us] wake %u: polling again", upS(), sleep_.wakes());
    }
    const uint64_t workStart = mach_absolute_time();   // this poll's own work, up to the wait: noteSelf, left out of busy
    monitor_.poll(kTsensIo);
    const uint64_t now = nowNs();
    irq::hookPolled(monitor_, now / 1000000ull);
    int32_t d;
    unsigned c, s;
    if (monitor_.hottestAll(d, c, s)) {
      piano_thermal_hottest_deci = d;
      if (!peakValid_ || d > peakDeci_) {
        peakValid_ = true;
        peakDeci_ = d;
        peakC_ = c;
        peakS_ = s;
      }
    }
    bool limited = false, clusterFast = false;
    for (unsigned k = 0; k < kDomains; ++k) {
      if (!domainOk_[k]) continue;
      limited |= governor_[k].mode() >= CpuMode::fixed && governor_[k].limit() < governor_[k].count() - 1u;
      clusterFast |= boot_[k] >= 0 && domain_[k].levels[set_[k]].khz > gArgs.fastMhz * 1000u;
    }
    // with piano-thermal-irq at rest: the same without the warm rule and the fast-clock rule (the upper interrupt stands in for them)
    const Poll next = irq::hookTier(pollTier(monitor_, gTrips, limited, clusterFast, int32_t(gArgs.warmPollC) * 10,
                                             int32_t(gArgs.fastTempC) * 10),
                                    pollTier(monitor_, gTrips, limited, false, 0x7fffffff), now / 1000000ull);
    if (next != tier) {
      tierNs[unsigned(tier)] += now - tierSince;
      tierSince = now;
      ++tierChanges;
      if (next == Poll::fast && !fastNoted) {   // the first fast spell of a report window, with its reason
        fastNoted = true;
        note("[%us] polling every %u ms: %s", upS(), gArgs.fastMs,
             irq::hookHolding(now / 1000000ull) ? "an upper-threshold interrupt"
             : limited ? "a cluster's thermal limit holds it down"
                     : clusterFast ? "a cluster runs above the fast-poll clock, a sensor at the fast-poll temperature"
                                   : "a sensor is within 10 C of its trip or of the ceiling");
      }
      tier = next;
    }
    if (first || tier != Poll::base || now - lastPolicy >= uint64_t(gArgs.pollMs) * 1000000ull) {
      lastPolicy = now;
      const PolicyEvent e = policy_.update(monitor_, now / 1000000ull);
      piano_thermal_state = int32_t(e.after);
      if (e.changed() || (e.after == State::critical && !halting_) || e.criticalPending) protect(e);
    }
    const bool slow = first || now - lastLog >= uint64_t(gArgs.logS) * 1000000000ull;
    const bool loadTick = first || now - lastLoad >= uint64_t(gArgs.tickMs) * 1000000ull;
    if (loadTick) lastLoad = now;
    if (gArgs.cpufreq) cpuTick(monitor_, slow, loadTick);
    if (slow) {
      report(monitor_, true);
      publish(monitor_);
      if (gArgs.cpufreq) {
        cpuReport(now - lastCpuLog);
        lastCpuLog = now;
      }
      tierNs[unsigned(tier)] += now - tierSince;
      tierSince = now;
      if (tierNs[1] || tierNs[2] || tierNs[3])
        note("[%us] polling: base %llu ms, warm %llu ms, clock %llu ms, fast %llu ms; %u changes", upS(),
             (unsigned long long)(tierNs[0] / 1000000), (unsigned long long)(tierNs[1] / 1000000),
             (unsigned long long)(tierNs[2] / 1000000), (unsigned long long)(tierNs[3] / 1000000), tierChanges);
      for (unsigned k = 0; k < kPollTiers; ++k) tierNs[k] = 0;
      tierChanges = 0;
      fastNoted = false;
      lastLog = now;
      peakValid_ = false;   // the peak is per report window
    }
    first = false;
    uint64_t workNs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - workStart, &workNs);
    noteSelf(workNs);
    // the base relaxed at rest (piano-thermal-irq)
    pollWait(pollPeriodMs(tier, irq::hookBase(base), gArgs.warmMs, gArgs.fastMs, gArgs.clockMs));
  }
}
