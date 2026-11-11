// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: SoC temperatures, thermal protection and CPU frequency for piano (SM8750) under native macOS 27 at EL1. Design,
// facts, boot arguments and the test procedure: docs/drivers/thermal.md. Inert unless piano-thermal or piano-cpufreq is set.
//   piano-thermal=1  read only: every TSENS sensor of the four controllers (Tsens.h) each poll (piano-thermal-poll-ms, 1000), the
//                    hottest per controller on the console every piano-thermal-log-s (30) and as IORegistry properties; at start the
//                    controllers' version/enable/thresholds (TM and SROT loads), the SCMI channel's status word (a load), and every 30 s
//                    each core's clock measured with a chain of dependent adds (no device access; piano-thermal-clock=0 skips it).
//                    Nothing is written at this level: every MMIO access is a load.
//   piano-thermal=2  level 1 + the protection policy (Policy.h): warm (85 C) logged; at a sensor's stock passive trip the GPU is
//                    parked at its lowest level for the rest of the boot (PianoA830's exported adreno830_dcvs_park); at the stock
//                    critical trip - piano-thermal-margin (10 C) on two polls in a row: logged loudly every poll, and with
//                    piano-thermal-shutdown=1 the kernel's halt (reboot_kernel(RB_HALT): processes signalled, disks synced and
//                    unmounted, IOKit's shutdown notifications, PSCI SYSTEM_OFF). Default: log only.
//   piano-cpufreq=1  the CPU clusters over SCMI (Scmi.h): protocol, domains, levels, the level each cluster is at (LEVEL_GET), then
//                    every 30 s again. Messages only, no level is set. Writes: the SCMI channel's shared memory and the doorbell.
//   piano-cpufreq=2  fixed level per cluster: the fastest <= piano-cpufreq-cap0/cap1 (MHz, default 2000), thermal limit on top.
//   piano-cpufreq=3  the governor (Governor.h): load-based per cluster every piano-cpufreq-tick-ms (100), caps on battery / on power
//                    (the IOPMPowerSource's ExternalConnected; unknown = battery), thermal limit on top.
// piano-cpufreq >= 1 implies the level 1 readout (its thermal input). No clock, regulator or GDSC is voted by this kext.
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
#include "Governor.h"
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
  uint32_t level, cpufreq, shutdown, pollMs, logS, clock, srot, warmC, marginC, delay;
  uint32_t cap[2], bat[2], ac[2], minMhz[2], hispeed[2], tickMs, hotC, releaseC, cutC;
  bool capGiven[2];
};
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}
uint32_t clampU(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void readArgs()
{
  gArgs.level = bootArg("piano-thermal", 0);
  gArgs.cpufreq = bootArg("piano-cpufreq", 0);
  if (gArgs.cpufreq > 3) gArgs.cpufreq = 3;
  if (gArgs.level > 2) gArgs.level = 2;
  if (gArgs.cpufreq && !gArgs.level) gArgs.level = 1;
  gArgs.shutdown = bootArg("piano-thermal-shutdown", 0);
  gArgs.pollMs = clampU(bootArg("piano-thermal-poll-ms", 1000), 100, 10000);
  gArgs.logS = clampU(bootArg("piano-thermal-log-s", 30), 5, 3600);
  gArgs.clock = bootArg("piano-thermal-clock", 1);
  gArgs.srot = bootArg("piano-thermal-srot", 1);
  gArgs.warmC = clampU(bootArg("piano-thermal-warm", 85), 50, 104);
  gArgs.marginC = clampU(bootArg("piano-thermal-margin", 10), 5, 30);
  gArgs.delay = clampU(bootArg("piano-thermal-delay", 0), 0, 600);
  // piano-cpufreq-cap (both clusters) or -cap0/-cap1: the fixed level of mode 2 (default 2000 MHz); in mode 3 a hard ceiling, only
  // when given.
  uint32_t capBoth = 2000;
  const bool bothGiven = PE_parse_boot_argn("piano-cpufreq-cap", &capBoth, sizeof capBoth);
  for (unsigned d = 0; d < 2; ++d) {
    uint32_t v = capBoth;
    const bool given = PE_parse_boot_argn(d ? "piano-cpufreq-cap1" : "piano-cpufreq-cap0", &v, sizeof v);
    gArgs.cap[d] = clampU(v, 300, 5000);
    gArgs.capGiven[d] = given || bothGiven;
  }
  gArgs.bat[0] = bootArg("piano-cpufreq-bat0", 2200);
  gArgs.bat[1] = bootArg("piano-cpufreq-bat1", 2500);
  gArgs.ac[0] = bootArg("piano-cpufreq-ac0", 3100);
  gArgs.ac[1] = bootArg("piano-cpufreq-ac1", 3100);
  gArgs.minMhz[0] = bootArg("piano-cpufreq-min0", 0);
  gArgs.minMhz[1] = bootArg("piano-cpufreq-min1", 0);
  gArgs.hispeed[0] = bootArg("piano-cpufreq-hispeed0", 1500);
  gArgs.hispeed[1] = bootArg("piano-cpufreq-hispeed1", 1800);
  gArgs.tickMs = clampU(bootArg("piano-cpufreq-tick-ms", 100), 20, 1000);
  gArgs.hotC = clampU(bootArg("piano-cpufreq-hot", 85), 50, 104);
  gArgs.releaseC = clampU(bootArg("piano-cpufreq-release", gArgs.hotC - 5), 40, gArgs.hotC);
  gArgs.cutC = clampU(bootArg("piano-cpufreq-cut", 95), gArgs.hotC, 110);
}

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
  uint64_t idleTicks, atNs;
  bool loadOk;
};
Cpu gCpus[kMaxCpus];
unsigned gCpuCount;

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
    c.idleTicks = 0;
    c.atNs = 0;
    c.loadOk = false;
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

// Busiest core of each domain since the last call, per mille; -1 where no core of the domain could be read.
void domainBusy(int32_t busy[kDomains])
{
  for (unsigned d = 0; d < kDomains; ++d) busy[d] = -1;
  const uint64_t now = nowNs();
  for (unsigned i = 0; i < gCpuCount; ++i) {
    Cpu &c = gCpus[i];
    uint64_t idle = 0;
    if (c.domain >= kDomains || !idleNs(c.processor, idle)) {
      c.loadOk = false;
      continue;
    }
    if (c.loadOk && now > c.atNs) {
      const uint64_t di = idle >= c.idleTicks ? idle - c.idleTicks : 0;
      const int32_t b = int32_t(busyPermille(now - c.atNs, di));
      if (b > busy[c.domain]) busy[c.domain] = b;
    }
    c.idleTicks = idle;
    c.atNs = now;
    c.loadOk = true;
  }
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

// Each running core's clock, measured on that core (thread_bind + thread_block, as the kernel's own per-CPU loops do).
void measureClocks(uint32_t mhz[kMaxCpus], bool on[kMaxCpus])
{
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

}  // namespace

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
  void cpuTick(const Monitor &m, bool slow);
  void cpuReport(uint64_t spanNs);

  Monitor monitor_;
  Policy policy_;
  int32_t peakDeci_ = 0;
  unsigned peakC_ = 0, peakS_ = 0;
  bool peakValid_ = false, parked_ = false, halting_ = false;
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
  uint32_t busyMax_[kDomains] = {}, changes_[kDomains] = {}, refusals_[kDomains] = {};
  uint64_t levelSinceNs_[kDomains] = {};
  PowerSource power_ = PowerSource::unknown;
  uint64_t powerAtNs_ = 0;
  bool cpuOk_ = false;

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoThermal, IOService)

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
  pc.criticalPolls = gArgs.pollMs >= 1000 ? 2 : 2000 / gArgs.pollMs;
  pc.unknownPolls = gArgs.pollMs >= 1000 ? 5 : 5000 / gArgs.pollMs;
  policy_ = Policy(pc);
  setProperty("piano-thermal", gArgs.level, 32);
  setProperty("piano-cpufreq", gArgs.cpufreq, 32);
  registerService();
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoThermal::thread, this, &t) != KERN_SUCCESS) {
    note("FAILED: no thread");
    return true;
  }
  thread_deallocate(t);
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
  if (clock && gArgs.clock) {
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
    if (e.unknownAfter) note("[%us] NO READING: no TSENS sensor gave a usable reading on %u polls in a row; protection is blind", upS(),
                             policy_.config().unknownPolls);
    else note("[%us] readings are back", upS());
  }
  if (e.before != e.after)
    note("[%us] state %s -> %s: %s s%u %s %s C", upS(), stateName(e.before), stateName(e.after), kMap[e.controller].name, e.sensor,
         sn.name, Deci(e.deci).s);
  if (gArgs.level < 2) return;
  if (e.after >= State::passive && !parked_) {
    parked_ = true;
    adreno830_dcvs_park();
    const int32_t critical = sn.criticalDeci - int32_t(gArgs.marginC) * 10;
    const int32_t passive = passiveOf(sn) < critical - 50 ? passiveOf(sn) : critical - 50;
    note("[%us] PASSIVE: %s %s C >= %s C (stock passive %s C): GPU parked at its lowest level for the rest of the boot "
         "(adreno830_dcvs_park)%s",
         upS(), sn.name, Deci(e.deci).s, Deci(passive).s, Deci(passiveOf(sn)).s,
         gArgs.cpufreq >= 2 ? "; CPU clusters to their slowest level" : "");
  }
  if (e.after == State::critical) {
    note("[%us] CRITICAL: %s %s C >= %s C (stock critical %s C - %u C margin); %s", upS(), sn.name, Deci(e.deci).s,
         Deci(sn.criticalDeci - int32_t(gArgs.marginC) * 10).s, Deci(sn.criticalDeci).s, gArgs.marginC,
         gArgs.shutdown ? "HALTING the system (piano-thermal-shutdown=1)" : "piano-thermal-shutdown=0: NOT shutting down (log only)");
    if (gArgs.shutdown && !halting_) {
      halting_ = true;
      IOSleep(200);   // let the console drain
      static char why[] = "PianoThermal: critical temperature";
      reboot_kernel(kRbHalt, why);
      note("[%us] reboot_kernel(RB_HALT) returned: the system did not halt", upS());
    }
  } else if (e.criticalPending) {
    note("[%us] critical threshold met once (%s %s C): waiting for a second poll", upS(), sn.name, Deci(e.deci).s);
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
    gc.capKhz = mode == CpuMode::fixed || gArgs.capGiven[d] ? gArgs.cap[d] * 1000 : 0;
    gc.batteryKhz = gArgs.bat[d] * 1000;
    gc.externalKhz = gArgs.ac[d] * 1000;
    gc.floorKhz = gArgs.minMhz[d] * 1000;
    gc.hispeedKhz = gArgs.hispeed[d] * 1000;
    gc.hotDeci = int32_t(gArgs.hotC) * 10;
    gc.releaseDeci = int32_t(gArgs.releaseC) * 10;
    gc.cutDeci = int32_t(gArgs.cutC) * 10;
    gc.riseTicks = mode == CpuMode::govern ? 1000 / gArgs.tickMs : 1;
    if (!gc.riseTicks) gc.riseTicks = 1;
    governor_[d].init(dom.canSet ? mode : CpuMode::report, gc, khz, dom.count, dom.sustained, unsigned(idx));
    levelSinceNs_[d] = nowNs();
    domainOk_[d] = true;
    if (mode >= CpuMode::fixed) {
      if (!dom.canSet) note("cpufreq: domain %u: the firmware does not allow LEVEL_SET; reporting only", d);
      else if (mode == CpuMode::fixed)
        note("cpufreq: domain %u: fixed at the fastest level <= %u MHz (and <= sustained), thermal limit %u/%u/%u C (hot/release/cut)", d,
             gArgs.cap[d], gArgs.hotC, gArgs.releaseC, gArgs.cutC);
      else
        note("cpufreq: domain %u: governor every %u ms, target 80 %% busy, hispeed %u MHz, floor %u MHz, caps: battery %u MHz, power %u "
             "MHz%s, thermal %u/%u/%u C",
             d, gArgs.tickMs, gArgs.hispeed[d], gArgs.minMhz[d], gArgs.bat[d], gArgs.ac[d], gc.capKhz ? ", fixed cap given" : "",
             gArgs.hotC, gArgs.releaseC, gArgs.cutC);
    }
  }
  cpuOk_ = domainOk_[0] || domainOk_[1];
  power_ = readPowerSource();
  powerAtNs_ = nowNs();
  if (mode >= CpuMode::fixed) note("cpufreq: power source: %s", powerName(power_));
}

void PianoThermal::cpuTick(const Monitor &m, bool slow)
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
    if (p != power_ && governor_[0].mode() >= CpuMode::fixed) note("[%us] cpufreq: power source: %s -> %s", upS(), powerName(power_), powerName(p));
    power_ = p;
    powerAtNs_ = now;
  }
  int32_t busy[kDomains];
  domainBusy(busy);
  const bool protectNow = policy_.state() >= State::passive && gArgs.level >= 2;
  for (unsigned d = 0; d < kDomains; ++d) {
    if (!domainOk_[d]) continue;
    Governor &g = governor_[d];
    TickInput in{};
    in.busyPermille = busy[d] < 0 ? 0 : uint32_t(busy[d]);
    in.power = power_;
    unsigned s;
    in.thermalValid = m.hottest(d, in.thermalDeci, s, kMap[d].cpuFirst, kMap[d].cpuLast);
    in.protect = protectNow;
    if (busy[d] >= 0) {
      busySum_[d] += uint32_t(busy[d]);
      ++busyTicks_[d];
      if (uint32_t(busy[d]) > busyMax_[d]) busyMax_[d] = uint32_t(busy[d]);
    }
    if (g.mode() == CpuMode::report) {
      if (!slow) continue;   // the report mode asks LEVEL_GET only at the log cadence
      int idx = -1;
      if (!scmi::levelGet(channel_, domain_[d], d, idx) && idx >= 0 && unsigned(idx) != set_[d]) {
        residencyNs_[d][set_[d]] += now - levelSinceNs_[d];
        levelSinceNs_[d] = now;
        set_[d] = unsigned(idx);
        ++changes_[d];
      }
      continue;
    }
    const unsigned want = g.tick(in);
    if (want == set_[d]) continue;
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
      continue;
    }
    residencyNs_[d][set_[d]] += now - levelSinceNs_[d];
    levelSinceNs_[d] = now;
    set_[d] = want;
    ++changes_[d];
  }
}

void PianoThermal::cpuReport(uint64_t spanNs)
{
  if (!cpuOk_ && !channel_.messages) return;
  const uint64_t now = nowNs();
  for (unsigned d = 0; d < kDomains; ++d) {
    if (boot_[d] < 0) continue;
    const scmi::Domain &dom = domain_[d];
    residencyNs_[d][set_[d]] += now - levelSinceNs_[d];
    levelSinceNs_[d] = now;
    char line[480];
    int at = snprintf(line, sizeof line, "[%us] cpufreq d%u %s: now %u MHz (boot %u MHz);", upS(), d, dom.name,
                      dom.levels[set_[d]].khz / 1000, dom.levels[boot_[d]].khz / 1000);
    for (unsigned i = 0; i < dom.count && at < int(sizeof line) - 24; ++i) {
      const uint64_t pct = spanNs ? residencyNs_[d][i] * 100 / spanNs : 0;
      if (pct) at = catf(line, sizeof line, at, " %u MHz %llu%%", dom.levels[i].khz / 1000, (unsigned long long)pct);
      residencyNs_[d][i] = 0;
    }
    const Governor &g = governor_[d];
    if (at < int(sizeof line))
      at = catf(line, sizeof line, at, "; %u changes; busy avg %llu%% max %u%%; ceiling %u MHz (%s), thermal limit %u MHz",
               changes_[d], (unsigned long long)(busyTicks_[d] ? busySum_[d] / busyTicks_[d] / 10 : 0), busyMax_[d] / 10,
               g.khz(g.ceiling(power_)) / 1000, powerName(power_), g.khz(g.limit()) / 1000);
    note("%s", line);
    changes_[d] = 0;
    busySum_[d] = busyTicks_[d] = 0;
    busyMax_[d] = 0;
  }
  note("[%us] scmi: %u messages, %u failed, %llu us each on average, %u us at most", upS(), channel_.messages, channel_.failures,
       (unsigned long long)(channel_.messages ? channel_.totalUs / channel_.messages : 0), channel_.maxUs);
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
  const uint32_t period = governing ? gArgs.tickMs : gArgs.pollMs;
  note("running: %s every %u ms, the policy every %u ms, a report every %u s%s", governing ? "the governor and the sensors" : "the sensors",
       period, gArgs.pollMs, gArgs.logS, gMapFailures ? " (some windows not mapped)" : "");
  uint64_t lastPolicy = 0, lastLog = nowNs(), lastCpuLog = nowNs();
  bool first = true;
  for (;;) {
    monitor_.poll(kTsensIo);
    const uint64_t now = nowNs();
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
    if (first || now - lastPolicy >= uint64_t(gArgs.pollMs) * 1000000ull) {
      lastPolicy = now;
      const PolicyEvent e = policy_.update(monitor_);
      piano_thermal_state = int32_t(e.after);
      if (e.changed() || e.after == State::critical || e.criticalPending) protect(e);
    }
    const bool slow = first || now - lastLog >= uint64_t(gArgs.logS) * 1000000000ull;
    if (gArgs.cpufreq) cpuTick(monitor_, slow);
    if (slow) {
      report(monitor_, true);
      publish(monitor_);
      if (gArgs.cpufreq) {
        cpuReport(now - lastCpuLog);
        lastCpuLog = now;
      }
      lastLog = now;
      peakValid_ = false;   // the peak is per report window
    }
    first = false;
    IOSleep(period);
  }
}
