// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio: the speakers and microphones of piano (SM8750: four FourSemi FS19xx amplifiers on the I2C hub's SE0 and the LPAIF
// secondary TDM interface, DMICs on the LPASS VA macro, all driven through the audio DSP's AudioReach graph manager) for macOS 27,
// native at EL1. Design, ladder and test plan: docs/drivers/audio.md. Facts and their sources: AudioHw.h. The levels (each runs the
// ones below it):
//   piano-audio=1  a read-only survey of what the boot stages left of the audio path, logged as "PianoAudio: ..." lines: the main
//                  TLMM's configuration and level of the amplifiers' bus (gpio64/65), reset (gpio129/108/128/107) and interrupt
//                  (gpio84/67/66/81) pins and of the TDM pins (gpio121-124), and the apps SMMU's stream-match entries of the DSP's
//                  PCM streams 0x1001/0x1041. Every MMIO access of this level is a load (AudioSurvey.h has no store at all).
//   piano-audio=2  the DSP link (AudioDsp.cpp), only with piano-power=2 in the same boot (PianoPower owns and starts the DSP; this
//                  driver never starts, stops or restarts it and votes nothing): the channel adsp_apps registered through PianoPower's
//                  client contract, its health watched, and one question asked of the DSP's graph manager: is it ready. Nothing else.
//   piano-audio=3  also the macOS devices (PianoAudioDevice.cpp): "Speaker" and "Mic" as IOAudio2Devices on the null transport (their
//                  own clock, silence both ways: nothing reaches any hardware).
//   piano-audio=4  also the LPASS clocks (AudioSession.c after level 2): the TDM bit clock, the VA macro's votes and mclk requested
//                  and released through the DSP's resource manager; the VA macro's identity and the LPI DMIC pins loaded meanwhile.
//   piano-audio=5  also the amplifiers (AudioAmps.c, AudioBoard.cpp): reset lines released, the bit-banged bus on gpio64/65, DEVID x 4,
//                  the owner's preset from the ESP (EFI/UniXNU/piano-fs19xx.fsm, the stock file only), configured powered down and
//                  muted, held in reset again at the end. Without the DSP (piano-power<2 or piano-audio-dsp=0) this level runs alone.
//   piano-audio=6  also the speaker graphs with a silent stream (the amplifiers stay down) and the microphone graphs opened and
//                  closed: the TDM pins muxed, the DSP's PCM streams through bypass, the buffers in the audio slice (PianoDspMemory.h).
//   piano-audio=7  also the tone walk, only with piano-audio-tone=1 (<= -20 dBFS, <= 3 s of tone, gain 160, 5 s watchdog): attended.
// Other arguments: piano-audio-delay=S (wait S seconds first, default 0), piano-audio-repeat=S with piano-audio-samples=N (resample
// the pins every S s, N times, default 3, one summary line each), piano-audio-smmu=0 (skip the SMMU reads; level 6 leaves the SMMU
// alone), piano-audio-dsp=0 (skip the DSP link), piano-audio-dsp-wait=S (give it up after S seconds, default 300),
// piano-audio-settle=S (the charger domain up this long before the first packet, default 20), piano-audio-device=0 (no macOS
// devices), piano-audio-ports=N (the GPR port set: 0 Ubuntu's, 1 sheng's, 2 SunUEFI's; AudioDspLink.h), piano-audio-ports-try=1
// (walk the other sets while nobody answers), piano-audio-lpass-read=0, piano-audio-preset-any=1, piano-audio-i2c-swap=0|1|2 (2),
// piano-audio-i2c-half=US (5), piano-audio-mic=0, piano-audio-silence=MS (2000), piano-audio-tone-mask=M (0xf),
// piano-audio-tone-ms=MS (600), piano-audio-tone-gap=MS (150), piano-audio-tone-gain=G (160, never more). docs/drivers/audio.md
// section 6 has them all. Without piano-audio the driver declines in probe() and maps nothing.
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
#include "AudioLog.h"
#include "AudioPcm.h"

using namespace pianoaudio;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_audio_stage = 0;
}

namespace {

struct Args {
  uint32_t level, smmu, delay, repeat, samples, dsp, dspWait, settle, device, power;
  uint32_t ports, portsTry;
  PianoAudioLevelArgs levels;
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
  gArgs.dsp = bootArg("piano-audio-dsp", 1);
  gArgs.dspWait = bootArg("piano-audio-dsp-wait", 300);
  if (gArgs.dspWait > 3600) gArgs.dspWait = 3600;
  gArgs.settle = bootArg("piano-audio-settle", 20);
  if (gArgs.settle > 600) gArgs.settle = 600;
  gArgs.device = bootArg("piano-audio-device", 1);
  gArgs.power = bootArg("piano-power", 0);   // PianoPower's level: the DSP runs only with 2 or more
  gArgs.ports = bootArg("piano-audio-ports", 0);
  if (gArgs.ports > 2) gArgs.ports = 0;
  gArgs.portsTry = bootArg("piano-audio-ports-try", 0);
  PianoAudioLevelArgs &l = gArgs.levels;
  l.level = gArgs.level > 7 ? 7 : gArgs.level;
  l.lpassRead = bootArg("piano-audio-lpass-read", 1);
  l.presetAny = bootArg("piano-audio-preset-any", 0);
  l.mic = bootArg("piano-audio-mic", 1);
  l.silenceMs = bootArg("piano-audio-silence", 2000);
  l.tone = bootArg("piano-audio-tone", 0);   // only 1 plays (AudioSession.c); level 7 needs it explicitly
  l.toneMask = bootArg("piano-audio-tone-mask", 0xf);
  l.toneMs = bootArg("piano-audio-tone-ms", 600);
  l.gapMs = bootArg("piano-audio-tone-gap", 150);
  l.toneGain = bootArg("piano-audio-tone-gain", 160);   // capped at 160 in AudioAmps.c whatever is given
  l.i2cSwap = bootArg("piano-audio-i2c-swap", 2);
  l.i2cHalfUs = bootArg("piano-audio-i2c-half", 5);
  l.smmu = gArgs.smmu;
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

}  // namespace

extern "C" void piano_audio_log(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoAudio: %s\n", line);
}

namespace {

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
  void devices();
  static void thread(void *self, wait_result_t);
  static void devicesThread(void *self, wait_result_t);

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
  if (gArgs.level > 7) note("piano-audio=%u: levels 8 and up are not built (docs/drivers/audio.md section 8); running 1-7", gArgs.level);
  note("level 1: loads only, nothing written%s%s", gArgs.level >= 2 ? "; level 2: the DSP link (one readiness query)" : "",
       gArgs.level >= 3 && gArgs.device ? "; level 3: the macOS devices on the null transport" : "");
  if (gArgs.level >= 4)
    note("levels 4-%u: %s%s%s%s", gArgs.levels.level, "4 the LPASS clocks (PRM)", gArgs.level >= 5 ? ", 5 the amplifiers (powered down)" : "",
         gArgs.level >= 6 ? ", 6 the graphs with silence" : "",
         gArgs.level >= 7 ? (gArgs.levels.tone == 1 ? ", 7 the TONE WALK (piano-audio-tone=1: attended)" : ", 7 not without piano-audio-tone=1") : "");
  setProperty("piano-audio", gArgs.level, 32);
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoAudio::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;
  }
  thread_deallocate(t);
  if (gArgs.level >= 3 && gArgs.device) {
    if (kernel_thread_start(&PianoAudio::devicesThread, this, &t) == KERN_SUCCESS) thread_deallocate(t);
    else note("devices: no thread");
  }
  return true;
}

void PianoAudio::devicesThread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-audio-devices");
  static_cast<PianoAudio *>(self)->devices();
}

// Level 3: the macOS devices once IOBSD is up (the family's kext load and the HAL's matching come after it; the VM-era audio kext's
// starter matched IOResources' IOBSD for the same reason, UPSTREAM).
void PianoAudio::devices()
{
  OSDictionary *bsd = IOService::resourceMatching("IOBSD");
  IOService *matched = bsd ? IOService::waitForMatchingService(bsd, 120ull * 1000 * 1000 * 1000) : nullptr;
  OSSafeReleaseNULL(bsd);
  if (!matched) {
    note("devices: IOBSD not up within 120 s; no devices");
    return;
  }
  matched->release();
  const PianoPcmTransport *transport = piano_pcm_null();
  const int r = piano_audio_devices_attach(transport);
  char line[160];
  if (r < 0)
    snprintf(line, sizeof line, "none (stage %d: %s)", piano_audio_devices_stage(),
             piano_audio_devices_stage() == -2 ? "IOAudio2Family's IOAudio2Device / IOAudio2DeviceUserClient not found" : "publishing failed");
  else
    snprintf(line, sizeof line, "%s%s%s as IOAudio2Devices on the %s transport (silence: nothing reaches the hardware)",
             r & 1 ? "Speaker" : "", r == 3 ? " and " : "", r & 2 ? "Mic" : "", transport->name);
  setProperty("piano-audio-devices", line);
  note("devices: %s", line);
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
  stage(4, "survey done");
  note("survey done: %llu MMIO loads, 0 stores; %u pages mapped, %u map failures", (unsigned long long)gLoads, gPageCount, gMapFailures);
  unmapAll();
  if (gArgs.level < 2) return;
  int ready = 0;
  if (!gArgs.dsp) {
    note("level 2: the DSP link skipped (piano-audio-dsp=0)");
  } else if (gArgs.power < 2) {
    note("level 2: piano-power=%u: the DSP does not run in this boot (PianoPower starts it at 2); no channel registered", gArgs.power);
    setProperty("piano-audio-dsp", "not run: piano-power below 2");
  } else {
    stage(5, "the DSP link: adsp_apps, PianoPower's health, the APM readiness query (nothing else is sent)");
    char summary[200];
    const int r = piano_audio_dsp_link(gArgs.dspWait, gArgs.settle * 1000, int(gArgs.ports), int(gArgs.portsTry), summary, sizeof summary);
    setProperty("piano-audio-dsp", summary);
    stage(r ? -5 : 6, r ? "the DSP link did not reach APM ready" : "the DSP link: APM ready");
    ready = r == 0;
  }
  if (gArgs.level < 4) return;
  if (!ready && gArgs.level < 5) {
    note("level 4: needs the DSP link (piano-power=2 and level 2's APM ready): not run");
    setProperty("piano-audio-levels", "4 skipped (no DSP link)");
    return;
  }
  stage(7, ready ? "levels 4 and up on the DSP link" : "level 5 alone (no DSP link: levels 4, 6 and 7 need it)");
  PianoAudioLevelArgs l = gArgs.levels;
  l.dsp = ready;
  char levels[200];
  const int r = piano_audio_levels(&l, levels, sizeof levels);
  setProperty("piano-audio-levels", levels);
  stage(r ? -8 : 8, r ? "a level failed (the lines above say where)" : "levels done");
}
