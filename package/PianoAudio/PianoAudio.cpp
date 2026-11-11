// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio: the speakers and microphones of piano (SM8750: four FourSemi FS19xx amplifiers on the I2C hub's SE0 and the LPAIF
// secondary TDM interface, DMICs on the LPASS VA macro, all driven through the audio DSP's AudioReach graph manager) for macOS 27,
// native at EL1. Design, ladder and test plan: docs/drivers/audio.md. Facts and their sources: AudioHw.h. This is level 1 only:
//   piano-audio=1  a read-only survey of what the boot stages left of the audio path, logged as "PianoAudio: ..." lines: the main
//                  TLMM's configuration and level of the amplifiers' bus (gpio64/65), reset (gpio129/108/128/107) and interrupt
//                  (gpio84/67/66/81) pins and of the TDM pins (gpio121-124), and the apps SMMU's stream-match entries of the DSP's
//                  PCM streams 0x1001/0x1041. Nothing is written: every MMIO access of this level is a load (AudioSurvey.h has no
//                  store at all). Not read: the amplifiers (a bus transaction needs writes), the I2C hub's engine, every LPASS block
//                  (their clocks are the DSP's), the DSP. No firmware, preset or topology file is read.
// Other arguments: piano-audio-delay=S (wait S seconds first, default 0), piano-audio-repeat=S with piano-audio-samples=N (resample
// the pins every S s, N times, default 3, one summary line each), piano-audio-smmu=0 (skip the SMMU reads). Without piano-audio the
// driver declines in probe() and maps nothing. Levels above 1 are not implemented; they run level 1.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "AudioSurvey.h"

using namespace pianoaudio;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_audio_stage = 0;
}

namespace {

struct Args {
  uint32_t level, smmu, delay, repeat, samples;
};
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}

void readArgs()
{
  gArgs.level = bootArg("piano-audio", 0);
  gArgs.smmu = bootArg("piano-audio-smmu", 1);
  gArgs.delay = bootArg("piano-audio-delay", 0);
  if (gArgs.delay > 600) gArgs.delay = 600;
  gArgs.repeat = bootArg("piano-audio-repeat", 0);
  if (gArgs.repeat > 3600) gArgs.repeat = 3600;
  gArgs.samples = bootArg("piano-audio-samples", 3);
  if (gArgs.samples > 100) gArgs.samples = 100;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoAudio: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_audio_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

// Explicit ldr, as PianoDwc3, PianoPower and PianoWifi do. This file has no store helper.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}

// ------------------------------------------------------------------------------------------------ MMIO pages, mapped on demand
// The survey decides which addresses it loads (AudioSurvey.h, host-tested); a page is mapped the first time one of its words is
// loaded (PianoWifi's scheme): fourteen TLMM pages and the SMMU's first (global) page.
struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
constexpr unsigned kMaxPages = 24;
Page gPages[kMaxPages];
unsigned gPageCount, gMapFailures;
uint64_t gLoads;

uintptr_t pageVa(uint64_t pa)
{
  const uint64_t base = pa & ~0xfffull;
  for (unsigned i = 0; i < gPageCount; ++i)
    if (gPages[i].pa == base) return gPages[i].va;
  if (gPageCount == kMaxPages) return 0;
  IODeviceMemory *m = IODeviceMemory::withRange(base, 0x1000);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return 0;
  gPages[gPageCount] = {base, map, uintptr_t(map->getVirtualAddress())};
  return gPages[gPageCount++].va;
}

uint32_t load(uint64_t pa)
{
  const uintptr_t va = pageVa(pa);
  if (!va) {
    ++gMapFailures;
    return 0xffffffffu;
  }
  ++gLoads;
  return rd32(va + uintptr_t(pa & 0xfff));
}

void unmapAll()
{
  for (unsigned i = 0; i < gPageCount; ++i) gPages[i].map->release();
  gPageCount = 0;
}

// ------------------------------------------------------------------------------------------------------------- the report
void reportPins(const Survey &s)
{
  for (unsigned i = 0; i < kPinCount; ++i) {
    const PinDecode d = decodePin(s.pins[i].cfg, s.pins[i].io);
    note("pin %s gpio%u: cfg %08x io %08x: function %u pull %s %u mA output %s, out %u in %u", kPins[i].name, kPins[i].gpio,
         s.pins[i].cfg, s.pins[i].io, d.function, pullName(d.pull), d.driveMa, d.outputEnable ? "enabled" : "disabled", d.out, d.in);
  }
}

void reportLines(const Survey &s, const Summary &m)
{
  for (unsigned a = 0; a < kAmpCount; ++a) {
    const Amp &amp = kAmps[a];
    const PinDecode rd = decodePin(s.pins[amp.resetPin].cfg, s.pins[amp.resetPin].io);
    const char *how = m.reset[a] == Reset::held       ? "held in shutdown (driven low)"
                      : m.reset[a] == Reset::released ? "released (driven high)"
                      : m.reset[a] == Reset::undriven ? "not driven by the SoC"
                                                      : "not on the GPIO function";
    note("amp %s %#x slot %u: reset gpio%u %s, function %u, reads %u; irq gpio%u %s, reads %u (active low)", amp.position,
         amp.address, amp.slot, kPins[amp.resetPin].gpio, how, rd.function, m.resetIn[a] ? 1u : 0u, kPins[amp.irqPin].gpio,
         m.irqGpio[a] ? "GPIO" : "not GPIO", m.irqIn[a] ? 1u : 0u);
  }
  note("bus i2c hub 0 (i2c@980000): SDA gpio%u function %u, SCL gpio%u function %u: %s, %s, %s", kPins[kPinSda].gpio,
       m.busFunction[0], kPins[kPinScl].gpio, m.busFunction[1],
       m.busMuxed ? "both on an alternate function (the hub's)" : "not both muxed to the hub (GPIO or mixed)",
       m.busPullUp ? "pull-ups on" : "pull-ups not both on", m.busIdle ? "idle (both high)" : "not idle (a line reads low)");
  note("TDM (LPAIF secondary, i2s1): sck %u data0 %u ws %u data1 %u: %s", m.tdmFunction[0], m.tdmFunction[1], m.tdmFunction[2],
       m.tdmFunction[3],
       m.tdmMuxed == 4 ? "all four on an alternate function (the i2s1 pins' mux is set)"
       : m.tdmMuxed    ? "some pins muxed, some GPIO"
                       : "all four on GPIO (not muxed: a later level sets sec-tdm-default-state)");
}

void reportSmmu(char *word, size_t size)
{
  const uint16_t streams[2] = {kStreamApm, kStreamApm2};
  const SmmuStreams s = surveySmmu(streams, kSmmuBytes, [](uint32_t off) { return load(kSmmu + off); });
  note("SMMU idr0 %08x idr1 %08x: %u stream-match groups, %u banks, %u pages%s%s", s.idr0, s.idr1, s.groups, s.banks, s.pages,
       s.error ? ": " : "", s.error ? s.error : "");
  size_t n = 0;
  for (unsigned k = 0; k < 2 && n < size; ++k) {
    const char *first = s.error ? "unread" : !s.count[k] ? "none" : s2crType(s.found[k][0].s2cr);
    n += size_t(snprintf(word + n, size - n, "%s%#x %s", k ? " " : "", streams[k], first));
    if (s.error) continue;
    if (!s.count[k]) note("SMMU stream %#x: no stream-match entry (the firmware left none)", streams[k]);
    for (unsigned i = 0; i < s.count[k]; ++i) {
      const SmmuMatch &m = s.found[k][i];
      note("SMMU stream %#x: group %u smr %08x (id %#x mask %#x) s2cr %08x (%s, bank %u)", streams[k], m.group, m.smr,
           m.smr & 0x7fffu, (m.smr >> 16) & 0x7fffu, m.s2cr, s2crType(m.s2cr), m.s2cr & 0xffu);
    }
    if (s.more[k]) note("SMMU stream %#x: %u more matching groups not listed", streams[k], s.more[k]);
  }
}

void reportLpass()
{
  char line[400];
  size_t n = 0;
  for (unsigned i = 0; i < kLpassBlockCount && n < sizeof line; ++i)
    n += size_t(snprintf(line + n, sizeof line - n, "%s%s %#llx (DT %s)", i ? ", " : "", kLpassBlocks[i].name,
                         (unsigned long long)kLpassBlocks[i].base, kLpassBlocks[i].enabled ? "okay" : "disabled"));
  note("LPASS not read: %s", line);
}

// One line for ioreg and for the repeated samples.
void summary(const Summary &m, const char *smmu, char *line, size_t size)
{
  const char r[] = {'H', 'R', 'U', 'O'};   // held, released, undriven, other
  char resets[kAmpCount + 1], irqs[kAmpCount + 1];
  for (unsigned a = 0; a < kAmpCount; ++a) {
    resets[a] = r[unsigned(m.reset[a])];
    irqs[a] = m.irqIn[a] ? '1' : '0';
  }
  resets[kAmpCount] = irqs[kAmpCount] = 0;
  snprintf(line, size, "resets(TL,TR,BL,BR) %s irq %s bus %s/%s tdm-muxed %u/4 smmu %s", resets, irqs, m.busMuxed ? "hub" : "gpio",
           m.busIdle ? "idle" : "low", m.tdmMuxed, smmu);
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoAudio : public IOService {
  OSDeclareDefaultStructors(PianoAudio)
  void run();
  static void thread(void *self, wait_result_t);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoAudio, IOService)

IOService *PianoAudio::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoAudio::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-audio=%u smmu=%u delay=%u repeat=%u samples=%u (4 x FS19xx on i2c hub 0 %#x, secondary TDM on gpio121-124)",
       gArgs.level, gArgs.smmu, gArgs.delay, gArgs.repeat, gArgs.samples, 0x980000u);
  if (gArgs.level > 1) note("piano-audio=%u: only level 1 exists (docs/drivers/audio.md section 8); running level 1", gArgs.level);
  note("level 1: loads only, nothing written");
  setProperty("piano-audio", gArgs.level, 32);
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoAudio::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;
  }
  thread_deallocate(t);
  return true;
}

void PianoAudio::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-audio");
  static_cast<PianoAudio *>(self)->run();
}

void PianoAudio::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  stage(1, "TLMM: the amplifiers' bus, reset and interrupt pins, the TDM pins (read only)");
  Survey s;
  surveyPins(s, load);
  reportPins(s);
  const Summary m = summarize(s);
  reportLines(s, m);
  char smmu[64] = "skipped";
  if (gArgs.smmu) {
    stage(2, "SMMU streams 0x1001/0x1041, the DSP's PCM buffers (read only)");
    reportSmmu(smmu, sizeof smmu);
  }
  stage(3, "the LPASS blocks: not read (their clocks are the DSP's; nothing here says whether they run)");
  reportLpass();
  char line[200];
  summary(m, smmu, line, sizeof line);
  setProperty("piano-audio-survey", line);
  note("summary: %s", line);
  note("verdict: %s", resetVerdict(m));
  for (uint32_t i = 1; gArgs.repeat && i <= gArgs.samples; ++i) {
    IOSleep(gArgs.repeat * 1000);
    Survey again;
    surveyPins(again, load);
    summary(summarize(again), smmu, line, sizeof line);
    note("sample %u (+%u s): %s", i, i * gArgs.repeat, line);
  }
  stage(4, "done");
  note("done: %llu MMIO loads, 0 stores; %u pages mapped, %u map failures", (unsigned long long)gLoads, gPageCount, gMapFailures);
  unmapAll();
}
