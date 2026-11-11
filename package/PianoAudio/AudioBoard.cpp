// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio levels 4-6, the kext side (AudioBoard.h): every store of these levels to the hardware is here, each one a value the
// host-tested headers compute (AudioPins.h, SmmuStream.h, BitI2c.h), and only on the pins and registers listed below:
//   TLMM   gpio64/65 (the amplifiers' bus, bit-banged, given back as found after each burst), gpio129/108/128/107 (the amplifiers'
//          reset lines, driven), gpio121-124 (the TDM pins, the i2s1 function while levels 6-7 run, then given back)
//   SMMU   the stream-match groups, S2CRs and bypass banks of the DSP's PCM streams 0x1001, 0x1081, 0x1041, 0x1061 (SmmuStream.h)
//   DRAM   the first 128 KiB of PianoAudio's slice of the DSP-addressable memory (PianoDspMemory.h), mapped write-combined
// and loads only of the VA macro's identity and the LPI DMIC pins (level 4, while the DSP's votes hold) and of the ESP (NativeUfs).
// Nothing here touches GCC, RPMh, AOSS, the PAS calls or the DSP.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <PianoDspMemory.h>
#include "AudioBoard.h"
#include "AudioLog.h"
#include "AudioPins.h"
#include "AudioPreset.h"
#include "AudioSha256.h"
#include "AudioSurvey.h"
#include "BitI2c.h"
#include "SmmuStream.h"

extern "C" {
// NativeUfs (package/NativeUfs): GPT lookup by name and 4096-byte block reads of any LUN, read only.
extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
extern unsigned long gPhysBase, gPhysSize;
}

static_assert(AUDIO_DMA_BYTES <= PIANO_DSP_MEMORY_AUDIO_BYTES, "the buffers fit PianoAudio's slice");
static_assert(PIANO_DSP_MEMORY_AUDIO % 4096 == 0 && PIANO_DSP_MEMORY_AUDIO + AUDIO_DMA_BYTES <= 0x100000000ull, "DSP-addressable");

namespace pianoaudio {
namespace {

inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }
inline void dsb() { __asm__ volatile("dsb sy" ::: "memory"); }

uint64_t nowMs()
{
  uint64_t ns;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

struct Region {
  IOMemoryMap *map;
  uintptr_t va;
};
bool mapRegion(Region &r, uint64_t pa, uint64_t bytes, IOOptionBits cache)
{
  IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
  r.map = m ? m->map(kIOMapAnywhere | cache) : nullptr;
  if (m) m->release();
  r.va = r.map ? uintptr_t(r.map->getVirtualAddress()) : 0;
  return r.va != 0;
}
void unmapRegion(Region &r)
{
  if (r.map) r.map->release();
  r = {nullptr, 0};
}

BoardArgs gArgs;

// ------------------------------------------------------------------------------------------------------ the TLMM pins
struct BoardPin {
  Region page;
  uint32_t ctl, io;   // as found when first mapped
  bool noted;
};
BoardPin gPin[kPinCount];

bool pinMap(unsigned i)
{
  BoardPin &p = gPin[i];
  if (p.page.va) return true;
  if (!mapRegion(p.page, kTlmm + uint64_t(kPins[i].gpio) * kTlmmStride, 0x1000, kIOMapInhibitCache)) {
    piano_audio_log("pins: gpio%u (%s) does not map", kPins[i].gpio, kPins[i].name);
    return false;
  }
  p.ctl = rd32(p.page.va);
  p.io = rd32(p.page.va + 4);
  return true;
}

// ---- the amplifiers' reset lines (active low): the value first, then GPIO with the output enabled (AudioPins.h resetCtl)
int ampReset(void *, unsigned slot, int release)
{
  if (slot >= kAmpCount) return -1;
  const unsigned i = kAmps[slot].resetPin;
  if (!pinMap(i)) return -1;
  BoardPin &p = gPin[i];
  const uint32_t ctl = resetCtl(p.ctl);
  wr32(p.page.va + 4, resetIo(release != 0));
  wr32(p.page.va, ctl);
  if (!p.noted) {
    p.noted = true;
    piano_audio_log("reset %s gpio%u: CTL %08x IO %08x as found (%s%s) -> CTL %08x, driven", kAmps[slot].position, kPins[i].gpio, p.ctl, p.io,
                    resetName(resetState(decodePin(p.ctl, p.io))), foreign(p.ctl) ? ", eGPIO on the island side: claimed as Linux claims it" : "",
                    rd32(p.page.va));
  }
  return 0;
}

// ---- the bus: BitI2c on gpio64/65, open drain through the output enable (released = input with the pull-up)
struct Bus {
  unsigned sda = kPinSda, scl = kPinSda + 1;
  bool usable, muxed, acked, swapped;
  BitI2c i2c;
  uint32_t transfers, errors, bursts, restoreMismatches;
};
Bus gBus;

void lineSda(void *, bool release) { wr32(gPin[gBus.sda].page.va, i2cCtl(gPin[gBus.sda].ctl, !release)); }
void lineScl(void *, bool release) { wr32(gPin[gBus.scl].page.va, i2cCtl(gPin[gBus.scl].ctl, !release)); }
bool readSda(void *) { return rd32(gPin[gBus.sda].page.va + 4) & 1; }
bool readScl(void *) { return rd32(gPin[gBus.scl].page.va + 4) & 1; }
void delayUs(void *, unsigned us) { IODelay(us); }

void muxBus()
{
  const unsigned pins[2] = {gBus.sda, gBus.scl};
  for (unsigned i : pins) {
    wr32(gPin[i].page.va + 4, 0);
    wr32(gPin[i].page.va, i2cCtl(gPin[i].ctl, false));
  }
  gBus.muxed = true;
  IODelay(4 * gArgs.i2cHalfUs);
}

void restoreBus()
{
  bool same = true;
  const unsigned pins[2] = {gBus.sda, gBus.scl};
  for (unsigned i : pins) {
    wr32(gPin[i].page.va + 4, gPin[i].io & 2);
    wr32(gPin[i].page.va, gPin[i].ctl);
    same = same && rd32(gPin[i].page.va) == gPin[i].ctl;
  }
  gBus.muxed = false;
  ++gBus.bursts;
  if (!same && gBus.restoreMismatches++ < 4) piano_audio_log("bus: a pin's CTL reads back otherwise after the restore");
}

int busTransfer(void *, uint8_t address, const uint8_t *out, uint32_t outLen, uint8_t *in, uint32_t inLen)
{
  if (!gBus.usable) return -1;
  if (!gBus.muxed) muxBus();
  const I2c r = inLen ? gBus.i2c.writeRead(address, out, outLen, in, inLen) : gBus.i2c.write(address, out, outLen);
  ++gBus.transfers;
  if (r == I2c::ok) {
    if (!gBus.acked) {
      gBus.acked = true;
      piano_audio_log("bus: first acknowledge (%#x): SDA gpio%u, SCL gpio%u, software I2C at about %u kHz", address, kPins[gBus.sda].gpio,
                      kPins[gBus.scl].gpio, 500 / (gArgs.i2cHalfUs + 1));
    }
    return 0;
  }
  if (++gBus.errors <= 8 && gBus.acked)   // before the first acknowledge every probe poll fails: not logged one by one
    piano_audio_log("bus: %#x %s %u bytes: %s", address, inLen ? "read" : "write", inLen ? inLen : outLen, i2cName(r));
  return -1;
}

void busSleep(void *, uint32_t us)
{
  if (us >= 1000) IOSleep(us / 1000);
  if (us % 1000) IODelay(us % 1000);
}

void busDone(void *)
{
  if (gBus.muxed) restoreBus();
}

int busSwap(void *)
{
  if (gArgs.i2cSwap != 2 || gBus.swapped || gBus.acked) return 0;
  const unsigned t = gBus.sda;
  gBus.sda = gBus.scl;
  gBus.scl = t;
  gBus.swapped = true;
  piano_audio_log("bus: nobody answered with SDA on gpio%u: now SDA gpio%u, SCL gpio%u", kPins[gBus.scl].gpio, kPins[gBus.sda].gpio,
                  kPins[gBus.scl].gpio);
  return 1;
}

const Fs19xxBus kBus = {nullptr, busTransfer, busSleep};

bool busSetup()
{
  if (!pinMap(kPinSda) || !pinMap(kPinSda + 1)) return false;
  if (gArgs.i2cSwap == 1) {
    gBus.sda = kPinSda + 1;
    gBus.scl = kPinSda;
  }
  const BoardPin &a = gPin[kPinSda], &b = gPin[kPinSda + 1];
  gBus.usable = !foreign(a.ctl) && !foreign(b.ctl);
  gBus.i2c.configure({nullptr, lineSda, lineScl, readSda, readScl, delayUs}, gArgs.i2cHalfUs, 2000);
  piano_audio_log("bus: gpio64 CTL %08x IO %08x, gpio65 CTL %08x IO %08x as found: %s%s", a.ctl, a.io, b.ctl, b.io,
                  gBus.usable ? "software I2C, the pins given back after each burst" : "NOT used: a pin is routed to the other TLMM (eGPIO)",
                  gBus.usable && !(a.io & b.io & 1) ? " (a line reads low: the first transfer's recovery clocks free a held SDA)" : "");
  return gBus.usable;
}

// ---- the TDM pins: the i2s1 function (AudioPins.h tdmCtl) while levels 6-7 run, then as found
bool gTdmOn;
int tdmPins(void *, int on)
{
  for (unsigned k = 0; k < 4; ++k) {
    const unsigned i = kPinTdmFirst + k;
    if (!pinMap(i)) return -1;
    if (on && foreign(gPin[i].ctl)) {
      piano_audio_log("TDM: gpio%u is routed to the other TLMM: not muxed", kPins[i].gpio);
      return -1;
    }
  }
  uint32_t now[4];
  for (unsigned k = 0; k < 4; ++k) {
    BoardPin &p = gPin[kPinTdmFirst + k];
    if (!on) wr32(p.page.va + 4, p.io & 2);
    wr32(p.page.va, on ? tdmCtl(p.ctl) : p.ctl);
    now[k] = rd32(p.page.va);
  }
  gTdmOn = on != 0;
  piano_audio_log("TDM: gpio121-124 %s (CTL %08x %08x %08x %08x)", on ? "to i2s1 (function 1, 8 mA, no bias)" : "given back as found", now[0],
                  now[1], now[2], now[3]);
  return 0;
}

// ---------------------------------------------------------------------------------------------------- the apps SMMU
constexpr uint16_t kStreams[4] = {0x1001, 0x1081, 0x1041, 0x1061};   // the DT's <0x1001 0x80>, <0x1041 0x20> as exact ids
Region gSmmu;
char gSmmuText[120] = "not set";
uint32_t smmuRead(uint32_t off) { return rd32(gSmmu.va + off); }

int smmuSetup(void *)
{
  if (!gArgs.smmu) {
    strlcpy(gSmmuText, "left alone (piano-audio-smmu=0)", sizeof gSmmuText);
    piano_audio_log("SMMU: left alone (piano-audio-smmu=0): stream 0x1001 must bypass already");
    return 0;
  }
  if (!gSmmu.va && !mapRegion(gSmmu, kSmmu, kSmmuBytes, kIOMapInhibitCache)) {
    strlcpy(gSmmuText, "the apps SMMU does not map", sizeof gSmmuText);
    piano_audio_log("SMMU: the apps SMMU does not map");
    return -1;
  }
  int required = -1;
  size_t n = 0;
  gSmmuText[0] = 0;
  for (unsigned i = 0; i < 4; ++i) {
    const SmmuPlan p = planSmmuStream(kStreams[i], kSmmuBytes, smmuRead);
    piano_audio_log("SMMU stream %#x: %s slot %u (smr %08x s2cr %08x) bank %u%s%s%s", kStreams[i], p.reuse ? "existing entry" : "free",
                    p.slot, p.smr, p.s2cr, p.bank, p.keep ? ", already bypasses" : "", p.error ? ": " : "", p.error ? p.error : "");
    const uint64_t deadline = nowMs() + 100;
    const char *failure = applySmmuStream(p, kStreams[i], smmuRead, [](uint32_t off, uint32_t v) { wr32(gSmmu.va + off, v); dsb(); },
                                          [deadline] { return nowMs() >= deadline; });
    piano_audio_log("SMMU stream %#x: %s", kStreams[i], failure ? failure : p.keep ? "left as it was (bypass)" : "through a bypass bank now");
    if (n < sizeof gSmmuText - 1)
      n += size_t(snprintf(gSmmuText + n, sizeof gSmmuText - n, "%s%#x %s", i ? ", " : "", kStreams[i], failure ? "FAILED" : p.keep ? "kept" : "bypass"));
    if (n > sizeof gSmmuText - 1) n = sizeof gSmmuText - 1;
    if (!i) required = failure ? -1 : 0;
  }
  return required;
}

// ---------------------------------------------------------------------------------- the DMA slice (PianoDspMemory.h)
Region gDma;
char gDmaText[100] = "not mapped";
void publish(void *) { dsb(); }

bool dmaSetup(AudioSessionOps &ops)
{
  const uint64_t pa = PIANO_DSP_MEMORY_AUDIO, bytes = AUDIO_DMA_BYTES;
  if (pa < uint64_t(gPhysBase) + gPhysSize && pa + bytes > uint64_t(gPhysBase)) {
    strlcpy(gDmaText, "refused: inside XNU's memory", sizeof gDmaText);
    piano_audio_log("DMA: %#llx + %#llx lies in XNU's memory: refused", (unsigned long long)pa, (unsigned long long)bytes);
    return false;
  }
  if (!mapRegion(gDma, pa, bytes, kIOMapWriteCombineCache)) {
    strlcpy(gDmaText, "cannot map", sizeof gDmaText);
    piano_audio_log("DMA: %#llx + %#llx does not map", (unsigned long long)pa, (unsigned long long)bytes);
    return false;
  }
  volatile uint64_t *z = reinterpret_cast<volatile uint64_t *>(gDma.va);
  for (uint64_t i = 0; i < bytes / 8; ++i) z[i] = 0;
  dsb();
  ops.dma = reinterpret_cast<uint8_t *>(gDma.va);
  ops.dma_phys = pa;
  ops.dma_bytes = uint32_t(bytes);
  ops.publish = publish;
  snprintf(gDmaText, sizeof gDmaText, "%#llx + %#llx (the audio slice), write-combined", (unsigned long long)pa, (unsigned long long)bytes);
  piano_audio_log("DMA: %s, zeroed; speaker periods at +%#x, capture at +%#x", gDmaText, AUDIO_DMA_SPEAKER, AUDIO_DMA_MIC);
  return true;
}

// ------------------------------------------------------------------------------------------- level 4's LPASS loads
void lpassRead(void *)
{
  Region va = {nullptr, 0};
  if (mapRegion(va, kVaMacro, 0x1000, kIOMapInhibitCache)) {
    uint32_t id[4];
    for (unsigned i = 0; i < 4; ++i) id[i] = rd32(va.va + uintptr_t(kVaIds[i].pa - kVaMacro));
    const VaIdentity v = vaIdentity(id[0], id[1], id[2]);
    piano_audio_log("lpass: VA macro CORE_ID %08x %08x %08x %08x: revision %u.%u step %u, %s", id[0], id[1], id[2], id[3], v.major, v.minor, v.step,
                    v.hasVa ? "a VA macro" : "NO VA macro bit");
    unmapRegion(va);
  } else {
    piano_audio_log("lpass: the VA macro does not map");
  }
  for (unsigned k = 0; k < kDmicPinCount; ++k) {
    Region pin = {nullptr, 0};
    if (!mapRegion(pin, lpiPin(kDmicPins[k]), 0x1000, kIOMapInhibitCache)) continue;
    const uint32_t cfg = rd32(pin.va), value = rd32(pin.va + 4);
    const PinDecode d = decodePin(cfg, value);
    piano_audio_log("lpass: LPI gpio%u: CFG %08x VALUE %08x: function %u, pull %s, %u mA, output %s, in %u", kDmicPins[k], cfg, value, d.function,
                    pullName(d.pull), d.driveMa, d.outputEnable ? "enabled" : "disabled", d.in ? 1u : 0u);
    unmapRegion(pin);
  }
}

// -------------------------------------------------------------------------------------------- the preset on the ESP
struct Partition {
  uint32_t lun;
  uint64_t first, cached;
  uint8_t block[4096];
};

int partitionRead(void *context, uint64_t offset, void *to, size_t length)
{
  Partition *p = static_cast<Partition *>(context);
  uint8_t *out = static_cast<uint8_t *>(to);
  while (length) {
    const uint64_t lba = offset / 4096;
    const size_t within = size_t(offset % 4096);
    if (p->cached != lba) {
      p->cached = ~0ull;
      if (!native_ufs_part_read(p->lun, p->first + lba, 1, p->block)) return -1;
      p->cached = lba;
    }
    const size_t take = 4096 - within < length ? 4096 - within : length;
    memcpy(out, p->block + within, take);
    out += take;
    offset += take;
    length -= take;
  }
  return 0;
}

uint8_t *gPreset;
uint32_t gPresetBytes;
char gPresetText[160] = "not read";

void presetLoad(AudioSessionOps &ops)
{
  ops.preset = nullptr;
  ops.preset_bytes = 0;
  ops.preset_why = gPresetText;
  uint32_t lun = 0;
  uint64_t first = 0, blocks = 0;
  for (unsigned waited = 0; !native_ufs_part_find(AUDIO_PRESET_PARTITION, &lun, &first, &blocks); ++waited) {
    if (waited >= 120) {
      strlcpy(gPresetText, "no ESP (NativeUfs did not publish \"esp\" within 120 s)", sizeof gPresetText);
      piano_audio_log("preset: %s", gPresetText);
      return;
    }
    IOSleep(1000);
  }
  Partition *part = IOMallocType(Partition);
  FatVolume *volume = IOMallocType(FatVolume);
  gPreset = static_cast<uint8_t *>(IOMallocData(AUDIO_PRESET_MAX_BYTES));
  int rc = AP_READ;
  if (part && volume && gPreset) {
    part->lun = lun;
    part->first = first;
    part->cached = ~0ull;
    if (fat_mount(volume, blocks * 4096, partitionRead, part)) strlcpy(gPresetText, "the ESP is not a FAT volume", sizeof gPresetText);
    else if ((rc = audio_preset_load(volume, AUDIO_PRESET_PATH, gPreset, AUDIO_PRESET_MAX_BYTES, &gPresetBytes)) != AP_OK)
      strlcpy(gPresetText, audio_preset_load_error(rc), sizeof gPresetText);
  }
  if (part) IOFreeType(part, Partition);
  if (volume) IOFreeType(volume, FatVolume);
  if (rc != AP_OK) {
    if (gPreset) IOFreeData(gPreset, AUDIO_PRESET_MAX_BYTES);
    gPreset = nullptr;
    gPresetBytes = 0;
    piano_audio_log("preset: %s", gPresetText);
    return;
  }
  uint8_t digest[32];
  audio_sha256(gPreset, gPresetBytes, digest);
  snprintf(gPresetText, sizeof gPresetText, "%s: %u bytes, SHA-256 %02x%02x%02x%02x..., %s", AUDIO_PRESET_PATH, gPresetBytes, digest[0], digest[1],
           digest[2], digest[3], audio_preset_is_stock(gPreset, gPresetBytes) ? "the stock file" : "NOT the stock file");
  piano_audio_log("preset: %s", gPresetText);
  ops.preset = gPreset;
  ops.preset_bytes = gPresetBytes;
}

}  // namespace

void boardSetup(AudioSessionOps &ops, const BoardArgs &args)
{
  gArgs = args;
  if (!gArgs.i2cHalfUs || gArgs.i2cHalfUs > 50) gArgs.i2cHalfUs = 5;
  ops.lpass_read = lpassRead;
  if (args.level >= 5) {
    if (busSetup()) {
      ops.amps = AudioAmpsIo{nullptr, &kBus, ampReset, busDone};
      ops.bus_swap = busSwap;
    }
    presetLoad(ops);
  }
  if (args.level >= 6) {
    ops.tdm_pins = tdmPins;
    ops.smmu = smmuSetup;
    dmaSetup(ops);
  }
}

void boardTeardown()
{
  if (gBus.muxed) restoreBus();
  if (gTdmOn) tdmPins(nullptr, 0);
  if (gDma.va) {
    volatile uint64_t *z = reinterpret_cast<volatile uint64_t *>(gDma.va);
    for (uint32_t i = 0; i < AUDIO_DMA_BYTES / 8; ++i) z[i] = 0;
    dsb();
    unmapRegion(gDma);
    strlcpy(gDmaText, "zeroed and unmapped", sizeof gDmaText);
  }
  unmapRegion(gSmmu);
  for (BoardPin &p : gPin) unmapRegion(p.page);
  if (gPreset) IOFreeData(gPreset, AUDIO_PRESET_MAX_BYTES);
  gPreset = nullptr;
}

void boardSummary(char *line, size_t size)
{
  snprintf(line, size, "bus %s%s (%u transfers, %u errors); preset %s; SMMU %s; DMA %s", gBus.usable ? "software I2C" : "unused",
           gBus.swapped ? " swapped" : "", gBus.transfers, gBus.errors, gPresetText, gSmmuText, gDmaText);
}

}  // namespace pianoaudio
