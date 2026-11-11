// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// PianoSensors: the ADSP sensor hub of piano (SSC in msm/adsp/sensor_pd) for macOS 27, native at EL1, as a client of PianoPower's
// running DSP through package/include/PianoAdspContract.h (charging.md §9). Design, facts, ladder: docs/drivers/sensors.md. Never
// starts, stops or restarts the DSP, never votes a rail; everything stops on the guard's first strike (SensorsGuard.h, sensors.md §7).
//   piano-sensors=1  survey, read only: PianoPower's health (the DSP, the charger domain, the link) through the guard; the FastRPC
//                    channel registered (PianoPower answers the DSP's open, the state Linux keeps from boot; nothing is sent on it);
//                    a QRTR port following service 400 (no traffic); the sensor pack read from the ESP and checked; the apps SMMU's
//                    entries of the sensors PD's streams planned (loads only); the arena address checked, not mapped.
//   piano-sensors=2  + the FastRPC reverse file service (SensorsRpc.c): the arena mapped, the streams through a bypass bank, attach to
//                    the sensors PD, the listener, every DSP file call answered from the pack in memory, writes declined; the guard's
//                    tokens before each bring-up call, its strikes after, its decline mode once off.
//   piano-sensors=3  + the SNS client (SensorsClient.c): SUID lookups and attributes, the sensor list published; no stream.
//   piano-sensors=4  + the streams (SensorsStream.c, sensors.md §5c): the ambient light on change and the accelerometer at 10 Hz,
//                    each on a QRTR port (a QMI client) of its own, only while a consumer needs it (the light: PianoBacklight's
//                    automatic mode, the display on, the lid open; the accelerometer: piano-rotated, the display on, the cover
//                    detached); piano-sensors-lux, -light-target, -edge, -seq published; joins the power tree: a system sleep
//                    disables both first (a port closed if the hub does not answer in time); the guard's strike ends them.
// Other arguments: piano-sensors-delay=S, -settle=S (20), -rpc-pa=ADDR (the arena; default the sensors' slice of the CDSP carveout,
// PianoDspMemory.h; 0: no file service), -bank=N (3),
// -smmu=0 (leave the SMMU alone), -nopack=1 (level 2 without a pack: every open fails, the paths are logged), -channel=0 (level 1
// does not register the channel), -log=0..2 (1); level 4: -als=0|1|2 (1: follow PianoBacklight's automatic mode; 2: always; 0:
// never), -rotate=0|1|2 (1: follow piano-rotate; 2: always; 0: never), -accel-hz=N (10), -accel-batch=MS (0), -axes=a.. (a+y+x+z).
// Without piano-sensors the driver declines in probe() and maps nothing.
// Properties on the PianoSensors service (stable; sensors.md §5 lists them with their units).
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMessage.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoAdspContract.h>
#include <PianoPowerContract.h>
extern "C" {
#include "FatVolume.h"
#include "SensorsClient.h"
#include "SensorsGuard.h"
#include "SensorsHealth.h"
#include "SensorsLoad.h"
#include "SensorsPolicy.h"
#include "SensorsRpc.h"
#include "SensorsStream.h"
#include "SensorsText.h"
// NativeUfs (package/NativeUfs): GPT lookup by name and 4096-byte block reads of any LUN, read only.
extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
extern unsigned long gPhysBase, gPhysSize;
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_sensors_stage = 0;
}
#include "SmmuStream.h"

namespace {

// ------------------------------------------------------------------------------------------------------------ arguments, time
struct Args {
  uint32_t level, delay, settle, bank, smmu, nopack, channel, log;
  uint64_t rpcPa;
  // level 4 (sensors.md §5c), and the consumers' own arguments it follows
  uint32_t als, rotate, accelHz, accelBatch;
  uint32_t backlight, backlightAuto, rotateAgent, rotateTest;
  char axesText[32];
  SfxAxes axes;
  int axesGiven, axesOk;
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
  gArgs.level = bootArg("piano-sensors", 0);
  if (gArgs.level > 4) gArgs.level = 4;
  gArgs.delay = bootArg("piano-sensors-delay", 0);
  if (gArgs.delay > 600) gArgs.delay = 600;
  gArgs.settle = bootArg("piano-sensors-settle", 20);
  if (gArgs.settle > 600) gArgs.settle = 600;
  gArgs.bank = bootArg("piano-sensors-bank", PS_CONTEXT_BANK);
  gArgs.smmu = bootArg("piano-sensors-smmu", 1);
  gArgs.nopack = bootArg("piano-sensors-nopack", 0);
  gArgs.channel = bootArg("piano-sensors-channel", 1);
  gArgs.log = bootArg("piano-sensors-log", 1);
  if (gArgs.log > 2) gArgs.log = 2;
  uint64_t pa = 0;
  if (!PE_parse_boot_argn("piano-sensors-rpc-pa", &pa, sizeof pa)) pa = PS_ARENA_DEFAULT;   // 0 given: no arena
  gArgs.rpcPa = pa;
  gArgs.als = bootArg("piano-sensors-als", 1);
  gArgs.rotate = bootArg("piano-sensors-rotate", 1);
  gArgs.accelHz = bootArg("piano-sensors-accel-hz", SS_ACCEL_HZ);
  if (gArgs.accelHz < 1 || gArgs.accelHz > 100) gArgs.accelHz = SS_ACCEL_HZ;
  gArgs.accelBatch = bootArg("piano-sensors-accel-batch", 0);
  if (gArgs.accelBatch > 1000) gArgs.accelBatch = 1000;
  gArgs.backlight = bootArg("piano-backlight", 0);         // PianoBacklight's level: its automatic mode needs 2
  gArgs.backlightAuto = bootArg("piano-backlight-auto", 0);
  gArgs.rotateAgent = bootArg("piano-rotate", 0);          // piano-rotated exits at 0
  gArgs.rotateTest = bootArg("piano-rotate-test", 0);      // 1: the agent takes its edges from a preference, not from here
  SfxAxes gyro, mag;
  sfx_axes_identity(&gArgs.axes);
  sfx_axes_identity(&gyro);
  sfx_axes_identity(&mag);
  gArgs.axesGiven = PE_parse_boot_arg_str("piano-sensors-axes", gArgs.axesText, sizeof gArgs.axesText);
  if (!gArgs.axesGiven) strlcpy(gArgs.axesText, PS_ACCEL_AXES, sizeof gArgs.axesText);
  gArgs.axesOk = sfx_axes_parse(gArgs.axesText, &gArgs.axes, &gyro, &mag) == 0;
  if (!gArgs.axesOk) {
    sfx_axes_identity(&gArgs.axes);
    sfx_axes_parse(PS_ACCEL_AXES, &gArgs.axes, &gyro, &mag);
  }
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoSensors: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_sensors_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

// Events of a thread: bits set by any thread (also PianoPower's DSP thread: no lock, only an atomic or and a wakeup), waited on
// with a deadline (PianoAdsp's pattern).
volatile uint32_t gMainEvents, gRpcEvents;
enum : uint32_t { kEvChannel = 1, kEvResponse = 2, kEvSns = 4, kEvPacket = 8, kEvPack = 16, kEvPower = 32 };

void wake(volatile uint32_t *events, uint32_t bits)
{
  __atomic_fetch_or(events, bits, __ATOMIC_ACQ_REL);
  thread_wakeup((event_t)events);
}

uint32_t waitEvents(volatile uint32_t *events, uint32_t ms)
{
  uint64_t deadline;
  clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
  for (;;) {
    uint32_t bits = __atomic_exchange_n(events, 0, __ATOMIC_ACQ_REL);
    if (bits) return bits;
    const uint64_t now = mach_absolute_time();
    if (now >= deadline) return 0;
    uint64_t left = 0;
    absolutetime_to_nanoseconds(deadline - now, &left);
    assert_wait_timeout((event_t)events, THREAD_UNINT, uint32_t(left / 1000000) + 1, kMillisecondScale);
    bits = __atomic_exchange_n(events, 0, __ATOMIC_ACQ_REL);
    if (bits) {clear_wait(current_thread(), THREAD_AWAKENED); return bits;}
    thread_block(THREAD_CONTINUE_NULL);
  }
}

// ------------------------------------------------------------------------------------------------------------ the guard
IOLock *gGuardLock;
SgGuard gGuard;

int guardState()
{
  IOLockLock(gGuardLock);
  const int s = gGuard.state;
  IOLockUnlock(gGuardLock);
  return s;
}
int guardMayRequest()
{
  IOLockLock(gGuardLock);
  const int ok = sg_may_request(&gGuard, nowMs());
  IOLockUnlock(gGuardLock);
  return ok;
}
void guardResult(int ok)
{
  IOLockLock(gGuardLock);
  sg_result(&gGuard, ok, nowMs());
  IOLockUnlock(gGuardLock);
}
int guardAdmit()
{
  IOLockLock(gGuardLock);
  sg_file_call(&gGuard, nowMs());
  const int serve = sg_serve_files(&gGuard);
  IOLockUnlock(gGuardLock);
  return serve;
}

// ------------------------------------------------------------------------------------------------------------ MMIO, the SMMU
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }
inline void dsb() { __asm__ volatile("dsb sy" ::: "memory"); }

constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000;   // the apps SMMU (arm-smmu-500), as PianoWifi's WifiHw.h
IOMemoryMap *gSmmuMap;
uintptr_t gSmmuVa;
uint32_t smmuRead(uint32_t off) { return rd32(gSmmuVa + off); }

bool mapSmmu()
{
  if (gSmmuVa) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(kSmmu, kSmmuBytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return false;
  gSmmuMap = map;
  gSmmuVa = uintptr_t(map->getVirtualAddress());
  return true;
}

// The sensors PD's stream ids for the context bank: compute-cb@3's 0x1003 (mask 0x80) and 0x1043 (mask 0x20) as exact ids, the first
// required (Pagani's planner does the same for its bank 4).
void streamsOf(uint32_t bank, uint16_t out[4])
{
  const uint16_t base = uint16_t(0x1000 + bank);
  out[0] = base;
  out[1] = uint16_t(base + 0x80);
  out[2] = uint16_t(base + 0x40);
  out[3] = uint16_t(base + 0x60);
}

char gSmmuText[200] = "not read";

// Level 1: the plan only (every access a load).
void surveySmmu()
{
  if (!mapSmmu()) {strlcpy(gSmmuText, "cannot map the apps SMMU", sizeof gSmmuText); return;}
  uint16_t sids[4];
  streamsOf(gArgs.bank, sids);
  size_t n = 0;
  gSmmuText[0] = 0;
  for (unsigned i = 0; i < 4 && n < sizeof gSmmuText - 1; ++i) {
    const pianosensors::SmmuPlan p = pianosensors::planSmmuStream(sids[i], kSmmuBytes, smmuRead);
    note("SMMU stream %#x: %s slot %u (smr %08x s2cr %08x) bank %u%s%s%s", sids[i], p.reuse ? "existing entry" : "free", p.slot,
         p.smr, p.s2cr, p.bank, p.keep ? ", already bypasses" : "", p.error ? ": " : "", p.error ? p.error : "");
    n += size_t(snprintf(gSmmuText + n, sizeof gSmmuText - n, "%s%#x %s", i ? ", " : "", sids[i],
                         p.error ? p.error : p.keep ? "bypass already" : p.reuse ? "entry to redirect" : "free"));
  }
}

// Level 2 (the file service's prepare, just before the first attach): the streams through bypass banks.
int applySmmu()
{
  if (!gArgs.smmu) {note("SMMU: left alone (piano-sensors-smmu=0): the streams must bypass already"); return 0;}
  if (!mapSmmu()) return -12;
  uint16_t sids[4];
  streamsOf(gArgs.bank, sids);
  int required = -19;
  size_t n = 0;
  for (unsigned i = 0; i < 4; ++i) {
    const pianosensors::SmmuPlan p = pianosensors::planSmmuStream(sids[i], kSmmuBytes, smmuRead);
    const uint64_t deadline = nowMs() + 100;
    const char *failure = pianosensors::applySmmuStream(p, sids[i], smmuRead, [](uint32_t off, uint32_t v) { wr32(gSmmuVa + off, v); dsb(); },
                                                         [deadline] { return nowMs() >= deadline; });
    note("SMMU stream %#x: %s (slot %u bank %u)", sids[i], failure ? failure : p.keep ? "left as it was" : "bypass set", p.slot, p.bank);
    if (n < sizeof gSmmuText - 1)
      n += size_t(snprintf(gSmmuText + n, sizeof gSmmuText - n, "%s%#x %s", i ? ", " : "", sids[i], failure ? failure : p.keep ? "kept" : "bypass"));
    if (n > sizeof gSmmuText - 1) n = sizeof gSmmuText - 1;
    if (!i) required = failure ? -16 : 0;
  }
  return required;
}

// ------------------------------------------------------------------------------------------------------------ the pack (the ESP)
struct Partition {
  uint32_t lun;
  uint64_t first;
  uint64_t cached;
  uint8_t block[4096];
};

int partitionRead(void *context, uint64_t offset, void *to, size_t length)
{
  Partition *p = static_cast<Partition *>(context);
  uint8_t *out = static_cast<uint8_t *>(to);
  while (length) {
    const uint64_t lba = offset / 4096;
    const size_t within = size_t(offset % 4096);
    if (!within && length >= 4096) {
      const uint32_t n = length / 4096 > 256 ? 256 : uint32_t(length / 4096);
      if (!native_ufs_part_read(p->lun, p->first + lba, n, out)) return -1;
      out += size_t(n) * 4096;
      offset += uint64_t(n) * 4096;
      length -= size_t(n) * 4096;
      continue;
    }
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

uint8_t *gPack;
uint32_t gPackBytes, gPackBits;
volatile int32_t gPackState = -1;   // -1 not yet, else SL_*
char gPackText[160] = "not read";

void loadPack()
{
  uint32_t lun = 0;
  uint64_t first = 0, blocks = 0;
  for (unsigned waited = 0; !native_ufs_part_find(SL_PARTITION, &lun, &first, &blocks); ++waited) {
    if (waited >= 120) {
      strlcpy(gPackText, "no ESP partition (NativeUfs did not publish \"" SL_PARTITION "\" within 120 s)", sizeof gPackText);
      __atomic_store_n(&gPackState, SL_NOT_FOUND, __ATOMIC_RELEASE);
      return;
    }
    IOSleep(1000);
  }
  Partition *part = IOMallocType(Partition);
  FatVolume *volume = IOMallocType(FatVolume);
  int rc = SL_READ;
  if (part && volume) {
    part->lun = lun;
    part->first = first;
    part->cached = ~0ull;
    FatFile f;
    if (fat_mount(volume, blocks * 4096, partitionRead, part)) {
      rc = SL_NOT_FAT;
    } else if (fat_open(volume, SL_PATH, &f) || f.directory) {
      rc = SL_NOT_FOUND;
    } else if (f.size > SL_MAX_BYTES || f.size < SSP_HEADER) {
      rc = f.size > SL_MAX_BYTES ? SL_TOO_BIG : SL_INVALID;
    } else if (!(gPack = static_cast<uint8_t *>(IOMallocData(f.size)))) {
      rc = SL_READ;
    } else {
      SspPack table;
      uint32_t size = 0;
      rc = sl_load(volume, SL_PATH, gPack, f.size, &size, &table);
      if (rc == SL_OK) {
        gPackBytes = size;
        gPackBits = sl_contents(&table);
        snprintf(gPackText, sizeof gPackText, "ok: %u entries, %u bytes;%s%s%s%s", table.count, size,
                 gPackBits & SL_HAS_REG_CONFIG ? " sns_reg_config" : " NO sns_reg_config", gPackBits & SL_HAS_JSON_LIST ? " json.lst" : " NO json.lst",
                 gPackBits & SL_HAS_REGISTRY ? " registry" : " NO registry (stock defaults)", gPackBits & SL_HAS_SOC0 ? " soc0" : " NO soc0");
      } else {
        IOFreeData(gPack, f.size);
        gPack = nullptr;
      }
    }
  }
  if (part) IOFreeType(part, Partition);
  if (volume) IOFreeType(volume, FatVolume);
  if (rc != SL_OK) snprintf(gPackText, sizeof gPackText, "%s (%s on the ESP)", sl_error_name(rc), SL_PATH);
  note("pack: %s", gPackText);
  __atomic_store_n(&gPackState, rc, __ATOMIC_RELEASE);
  wake(&gRpcEvents, kEvPack);
}

// ------------------------------------------------------------------------------------------------------------ the file service
SsrCore *gCore;
PianoAdspChannel *gChannel;
IOMemoryMap *gArenaMap;
uint8_t *gArena, *gBounce;
volatile uint32_t gChannelUp, gChannelUps;
char gArenaText[200] = "none (piano-sensors-rpc-pa=0)";

void channelState(void *, int up)   // PianoPower's DSP thread: record and wake only
{
  __atomic_store_n(&gChannelUp, up ? 1u : 0u, __ATOMIC_RELEASE);
  if (up) __atomic_fetch_add(&gChannelUps, 1u, __ATOMIC_ACQ_REL);
  if (gCore) ssr_core_channel(gCore, up);
  wake(&gRpcEvents, kEvChannel);
  wake(&gMainEvents, kEvChannel);
}
void channelReceive(void *, const uint8_t *data, uint32_t length)
{
  if (gCore) ssr_core_receive(gCore, data, length);
  wake(&gRpcEvents, kEvResponse);
}

uint64_t opNow(void *) {return nowMs();}
int opSend(void *, const uint8_t *message, uint32_t length) {return piano_adsp_send(gChannel, message, length) ? -1 : 0;}
void opWait(void *, uint32_t ms) {waitEvents(&gRpcEvents, ms ? ms : 1);}
void opTrace(void *, const char *line) {note("rpc:%s%s", line[0] == ' ' ? "" : " ", line);}
void opPublish(void *) {dsb();}
int opPrepare(void *) {return applySmmu();}
int opPackSize(void *, uint64_t *size)
{
  const int32_t state = __atomic_load_n(&gPackState, __ATOMIC_ACQUIRE);
  if (state < 0) return SsrPackNotYet;
  *size = state == SL_OK ? gPackBytes : 0;
  return state == SL_OK ? SsrPackOk : state == SL_NOT_FOUND ? SsrPackAbsent : SsrPackError;
}
int opPackRead(void *, uint64_t offset, void *to, uint32_t length)
{
  if (!gPack || offset > gPackBytes || length > gPackBytes - offset) return -1;
  memcpy(to, gPack + offset, length);   // to: SensorsRpc's bounce buffer (normal memory)
  return 0;
}
void *opAlloc(void *, uint32_t bytes) {return IOMallocData(bytes);}
int opAdmit(void *) {return guardAdmit();}
const SsrOps kRpcOps = {nullptr, opNow, opSend, opWait, opTrace, opPublish, opPrepare, opPackSize, opPackRead, opAlloc, opAdmit};

// The arena: checked against the known owners of low DRAM and XNU's window, mapped write-combined, zeroed with 8-byte stores.
bool mapArena()
{
  const uint64_t pa = gArgs.rpcPa, bytes = SSR_ARENA_BYTES;
  const char *why = nullptr;
  if (!pa) return false;
  if (ps_arena_check(pa, bytes, &why) == 0 && pa < uint64_t(gPhysBase) + gPhysSize && pa + bytes > uint64_t(gPhysBase)) why = "XNU's memory";
  if (why) {
    snprintf(gArenaText, sizeof gArenaText, "%#llx + %#llx refused: %s", (unsigned long long)pa, (unsigned long long)bytes, why);
    return false;
  }
  const char *which = pa == PS_ARENA_DEFAULT ? "the default, the CDSP carveout's sensors slice" : "named by piano-sensors-rpc-pa";
  if (gArgs.level < 2) {
    snprintf(gArenaText, sizeof gArenaText, "%#llx + %#llx (%s) acceptable, not mapped at level 1", (unsigned long long)pa,
             (unsigned long long)bytes, which);
    return false;
  }
  IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
  gArenaMap = m ? m->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
  if (m) m->release();
  gBounce = static_cast<uint8_t *>(IOMallocData(SSR_BOUNCE));
  if (!gArenaMap || !gBounce) {
    snprintf(gArenaText, sizeof gArenaText, "%#llx + %#llx: cannot map", (unsigned long long)pa, (unsigned long long)bytes);
    return false;
  }
  gArena = reinterpret_cast<uint8_t *>(gArenaMap->getVirtualAddress());
  volatile uint64_t *z = reinterpret_cast<volatile uint64_t *>(gArena);
  for (uint64_t i = 0; i < bytes / 8; ++i) z[i] = 0;
  dsb();
  snprintf(gArenaText, sizeof gArenaText, "%#llx + %#llx (%s) mapped write-combined", (unsigned long long)pa, (unsigned long long)bytes,
           which);
  return true;
}

// ------------------------------------------------------------------------------------------------------------ the SNS ports
// Port 0 (gPort) carries the level-3 list; at level 4 each stream has a port of its own (a QMI client of its own at the hub). Every
// packet goes through one ring to the main thread, tagged with its port's source.
int gPort = -1;
struct Packet {
  uint32_t length, source;   // source: 0 the list, 1 + SS_LIGHT / SS_ACCEL
  uint8_t data[2048];
};
constexpr unsigned kRing = 32;
struct Ring {
  volatile uint32_t head, tail, dropped;   // head: the DSP thread's, tail: the main thread's
  Packet slot[kRing];
};
Ring *gRing;
const uint32_t kSources[1 + SS_STREAMS] = {0, 1 + SS_LIGHT, 1 + SS_ACCEL};

void snsServer(void *, int, uint32_t, uint32_t, uint32_t) { wake(&gMainEvents, kEvSns); }
void snsReceive(void *context, uint32_t, uint32_t, const uint8_t *data, uint32_t length)   // the DSP thread: copy and wake
{
  if (!gRing || length > sizeof gRing->slot[0].data) return;
  const uint32_t head = __atomic_load_n(&gRing->head, __ATOMIC_RELAXED), tail = __atomic_load_n(&gRing->tail, __ATOMIC_ACQUIRE);
  if (head - tail >= kRing) {__atomic_fetch_add(&gRing->dropped, 1u, __ATOMIC_RELAXED); return;}
  Packet &p = gRing->slot[head % kRing];
  memcpy(p.data, data, length);
  p.length = length;
  p.source = context ? *static_cast<const uint32_t *>(context) : 0;
  __atomic_store_n(&gRing->head, head + 1, __ATOMIC_RELEASE);
  wake(&gMainEvents, kEvPacket);
}
const PianoQrtrClient kSnsClient = {snsServer, snsReceive};

PianoQrtrServer gSns;
ScClient *gClient;
volatile int gTypesDirty;
unsigned gSnsLines;

uint64_t scNow(void *) {return nowMs();}
int scSend(void *, const uint8_t *data, uint32_t length)
{
  if (!gSns.up) return -1;
  return piano_qrtr_send(gPort, gSns.node, gSns.port, data, length) ? -1 : 0;
}
int scMayRequest(void *) {return guardMayRequest();}
void scResult(void *, int ok) {guardResult(ok);}
void scTrace(void *, const char *line)
{
  if (gArgs.log >= 2 || gSnsLines++ < 80) note("%s", line);
}
void scChanged(void *) {gTypesDirty = 1;}
const ScOps kScOps = {nullptr, scNow, scSend, scMayRequest, scResult, scTrace, scChanged};

// ------------------------------------------------------------------------------------------------------------ level 4: the streams
SsEngine *gEngine;
int gStreamPort[SS_STREAMS] = {-1, -1};
PianoQrtrServer gStreamServer[SS_STREAMS];
IOService *gSelf;

// A stream's own port, following service 400 like the list's; the engine starts the stream afresh on it.
void openStreamPort(int stream)
{
  gStreamServer[stream] = PianoQrtrServer{};
  gStreamPort[stream] = piano_qrtr_open(PS_QMI_SNS, PIANO_QRTR_ANY, &kSnsClient, const_cast<uint32_t *>(&kSources[1 + stream]));
  note("%s: QRTR port %d for service %#x (its own QMI client)", ss_stream_name(stream), gStreamPort[stream], PS_QMI_SNS);
  if (gStreamPort[stream] >= 0) ss_port_opened(gEngine, stream);
}

uint64_t ssNow(void *) {return nowMs();}
int ssSend(void *, int stream, const uint8_t *data, uint32_t length)
{
  const PianoQrtrServer &s = gStreamServer[stream];
  if (gStreamPort[stream] < 0 || !s.up) return -1;
  return piano_qrtr_send(gStreamPort[stream], s.node, s.port, data, length) ? -1 : 0;
}
void ssTrace(void *, const char *line) {note("%s", line);}   // the engine limits its own lines
void ssPublish(void *, const SsOutputs *o)
{
  // the values first, the sequence last: a consumer that sees the new sequence reads the new values
  gSelf->setProperty("piano-sensors-lux", uint64_t(o->lux), 32);
  if (o->target >= 0) gSelf->setProperty("piano-sensors-light-target", uint64_t(o->target), 32);
  gSelf->setProperty("piano-sensors-edge", ps_edge_name(o->edge));
  gSelf->setProperty("piano-sensors-seq", o->seq, 64);
  if (o->changed & SS_OUT_EDGE) gSelf->messageClients(kIOMessageServicePropertyChange);   // piano-rotated's interest notification
}
void ssClosePort(void *, int stream)
{
  if (gStreamPort[stream] < 0) return;
  piano_qrtr_close(gStreamPort[stream]);   // DEL_CLIENT: the hub drops this client's requests
  note("%s: QRTR port %d closed", ss_stream_name(stream), gStreamPort[stream]);
  gStreamPort[stream] = -1;
  gStreamServer[stream] = PianoQrtrServer{};
}

// The services whose properties are the streams' conditions, found by class (registered or not, as PianoBacklight finds PianoSensors):
// looked up every 2 s for 2 minutes, then every 30 s; retained once found.
struct Watched {
  const char *cls;
  IOService *service;
  uint64_t next;
  unsigned lookups;
};
Watched gDisplay = {"NativeGopDisplay", nullptr, 0, 0}, gKeyboard = {"PianoKeyboard", nullptr, 0, 0},
        gBacklight = {"PianoBacklight", nullptr, 0, 0};

IOService *watched(Watched &w, uint64_t now)
{
  if (w.service || now < w.next) return w.service;
  w.next = now + (++w.lookups < 60 ? 2000 : 30000);
  OSDictionary *matching = IOService::serviceMatching(w.cls);
  w.service = matching ? IOService::copyMatchingService(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (!w.service) {
    IOService *provider = gSelf->getProvider();
    OSIterator *it = provider ? provider->getChildIterator(gIOServicePlane) : nullptr;
    while (OSObject *o = it ? it->getNextObject() : nullptr)
      if (o->metaCast(w.cls) && (w.service = OSDynamicCast(IOService, o))) {
        w.service->retain();
        break;
      }
    OSSafeReleaseNULL(it);
  }
  if (w.service) note("streams: found %s (its properties are conditions of the streams)", w.cls);
  return w.service;
}

// A number or boolean property of a watched service: its value, or fallback when absent.
int64_t property(IOService *s, const char *key, int64_t fallback)
{
  OSObject *o = s ? s->copyProperty(key) : nullptr;
  int64_t v = fallback;
  if (OSNumber *n = OSDynamicCast(OSNumber, o)) v = int64_t(n->unsigned64BitValue());
  else if (OSBoolean *b = OSDynamicCast(OSBoolean, o)) v = b->isTrue() ? 1 : 0;
  OSSafeReleaseNULL(o);
  return v;
}

const char *gLightWhy = "?", *gAccelWhy = "?";

SsConditions readConditions(uint64_t now)
{
  SsConditions c;
  IOService *display = watched(gDisplay, now), *keyboard = watched(gKeyboard, now);
  c.display = int(property(display, "piano-display-power", -1));   // published with piano-backlight=2: unknown counts as on
  c.lid = int(property(keyboard, "piano-keyboard-hall", -1));
  c.attached = property(keyboard, "piano-keyboard-attached", 0) == 1;
  // The light: PianoBacklight's automatic mode takes the targets (piano-backlight=2, piano-backlight-auto=1), unless the user chose a
  // level (its published piano-backlight-auto false after a target was taken: manual by choice, until set to 1 again).
  if (gArgs.als == 0) {
    c.light_consumer = 0;
    gLightWhy = "piano-sensors-als=0";
  } else if (gArgs.als >= 2) {
    c.light_consumer = 1;
    gLightWhy = "piano-sensors-als=2";
  } else if (gArgs.backlight < 2 || gArgs.backlightAuto != 1) {
    c.light_consumer = 0;
    gLightWhy = "no automatic brightness (piano-backlight=2 piano-backlight-auto=1)";
  } else if (IOService *backlight = watched(gBacklight, now)) {
    const bool manual = property(backlight, "piano-backlight-auto", -1) == 0 &&
                        property(backlight, "piano-backlight-ambient-permille", -1) >= 0;
    c.light_consumer = !manual;
    gLightWhy = manual ? "PianoBacklight is in manual mode" : "PianoBacklight's automatic mode";
  } else {
    c.light_consumer = 0;
    gLightWhy = "no PianoBacklight service";
  }
  // The accelerometer: piano-rotated reads the edge (piano-rotate>=1) unless it tests with its own edges (piano-rotate-test=1).
  c.accel_consumer = gArgs.rotate >= 2 || (gArgs.rotate == 1 && gArgs.rotateAgent >= 1 && gArgs.rotateTest != 1);
  gAccelWhy = gArgs.rotate == 0 ? "piano-sensors-rotate=0" : gArgs.rotate >= 2 ? "piano-sensors-rotate=2"
              : c.accel_consumer ? "piano-rotated" : "no rotation agent (piano-rotate=0 or piano-rotate-test=1)";
  return c;
}

// The light's consumer told to leave automatic mode (PianoBacklightAmbient -1, sensors.md 5a.4): the stream failed or the guard struck.
void ssLeave(void *)
{
  IOService *backlight = watched(gBacklight, ~0ull >> 1);
  const IOReturn r = backlight ? backlight->callPlatformFunction("PianoBacklightAmbient", false, reinterpret_cast<void *>(intptr_t(-1)),
                                                                 const_cast<char *>("PianoSensors"), nullptr, nullptr)
                               : kIOReturnNotFound;
  note("light: PianoBacklight told to leave automatic mode: %#x", r);
}

const SsOps kSsOps = {nullptr, ssNow, ssSend, scMayRequest, scResult, ssTrace, ssPublish, ssLeave, ssClosePort};

// The power contract (PianoPowerContract.h): kOff asks the thread to disable the streams and acknowledge (at most kSleepWaitMs, then
// the ports of streams not off are closed); kOn lets them start again.
volatile uint32_t gAsleep, gAckDue;
constexpr uint32_t kSleepWaitMs = 800;

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoSensors : public IOService {
  OSDeclareDefaultStructors(PianoSensors)
  void run();
  void runRpc();
  uint32_t runStreams(uint64_t now, int state, int reason, const PianoAdspHealth &h);
  void publishTypes();
  void publishStatus(const SgDsp &d, const PianoAdspHealth &h);
  static void mainThread(void *self, wait_result_t);
  static void rpcThread(void *self, wait_result_t);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
};

OSDefineMetaClassAndStructors(PianoSensors, IOService)

IOService *PianoSensors::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoSensors::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-sensors=%u delay=%u settle=%u bank=%u smmu=%u rpc-pa=%#llx nopack=%u channel=%u log=%u", gArgs.level,
       gArgs.delay, gArgs.settle, gArgs.bank, gArgs.smmu, (unsigned long long)gArgs.rpcPa, gArgs.nopack, gArgs.channel, gArgs.log);
  note("a client of PianoPower's DSP (PianoAdspContract.h): it never starts, stops or restarts the DSP");
  if (gArgs.level >= 4)
    note("level 4: als=%u rotate=%u accel-hz=%u accel-batch=%u axes=%s%s; follows piano-backlight=%u piano-backlight-auto=%u "
         "piano-rotate=%u piano-rotate-test=%u", gArgs.als, gArgs.rotate, gArgs.accelHz, gArgs.accelBatch, gArgs.axesText,
         gArgs.axesOk ? "" : " (not a map: the default " PS_ACCEL_AXES ")", gArgs.backlight, gArgs.backlightAuto, gArgs.rotateAgent,
         gArgs.rotateTest);
  gSelf = this;
  setProperty("piano-sensors", gArgs.level, 32);
  registerService();   // consumers (PianoBacklight's automatic mode, piano-rotated) find the service by matching
  gGuardLock = IOLockAlloc();
  gCore = static_cast<SsrCore *>(IOMallocZeroData(sizeof(SsrCore)));
  gRing = static_cast<Ring *>(IOMallocZeroData(sizeof(Ring)));
  if (!gGuardLock || !gCore || !gRing || gArgs.bank < 3 || gArgs.bank > 8) {
    stage(-1, gArgs.bank < 3 || gArgs.bank > 8 ? "piano-sensors-bank must be 3..8 (the devicetree's compute banks)" : "no memory");
    return true;
  }
  SgConfig gc;
  sg_defaults(&gc);
  gc.settle_ms = gArgs.settle * 1000u;
  sg_init(&gGuard, &gc);
  // The arena first (level 2 only), then the core with its final configuration, then the channel: an open the DSP made already is
  // answered at the registration, and its state must find the core ready.
  const bool arena = mapArena();
  const SsrConfig cfg = {gArena, gArgs.rpcPa, SSR_ARENA_BYTES, gBounce, 1 /* Linux's first client id */, gArgs.bank, gArgs.log};
  ssr_core_init(gCore, &kRpcOps, &cfg);
  if (gArgs.channel || gArgs.level >= 2) {
    static const PianoAdspClient client = {channelState, channelReceive};
    gChannel = piano_adsp_channel(PS_GLINK_CHANNEL, 512, 16, 0 /* the DSP opens it */, &client, this);
    note("channel %s: %s", PS_GLINK_CHANNEL, gChannel ? "registered (the DSP's open is answered by PianoPower)" : "NOT registered (taken or no slot)");
  }
  gPort = piano_qrtr_open(PS_QMI_SNS, PIANO_QRTR_ANY, &kSnsClient, const_cast<uint32_t *>(&kSources[0]));
  note("QRTR port for service %#x: %d", PS_QMI_SNS, gPort);
  if (gArgs.level >= 3) {
    gClient = static_cast<ScClient *>(IOMallocZeroData(sizeof(ScClient)));
    if (gClient) sc_init(gClient, &kScOps);
  }
  if (gArgs.level >= 4 && gClient && (gEngine = static_cast<SsEngine *>(IOMallocZeroData(sizeof(SsEngine))))) {
    SsConfig sc;
    ss_defaults(&sc);
    sc.accel_hz = gArgs.accelHz;
    sc.accel_batch_ms = gArgs.accelBatch;
    sc.axes = gArgs.axes;
    ss_init(gEngine, &kSsOps, &sc);
    for (int i = 0; i < SS_STREAMS; ++i) openStreamPort(i);
    setProperty("piano-sensors-edge", ps_edge_name(PS_EDGE_NONE));   // the level-4 interface from the start (sensors.md §5)
    setProperty("piano-sensors-seq", 0ull, 64);
  } else if (gArgs.level >= 4) {
    note("level 4: no memory for the streams");
  }
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoSensors::mainThread, this, &t) != KERN_SUCCESS) {stage(-1, "no thread"); return true;}
  thread_deallocate(t);
  if (gArgs.level >= 2) {
    if (!arena) {
      note("level %u: no file service: the arena: %s", gArgs.level, gArenaText);
    } else if (gChannel) {
      thread_t r = nullptr;
      if (kernel_thread_start(&PianoSensors::rpcThread, this, &r) == KERN_SUCCESS) thread_deallocate(r);
      else note("no file service thread");
    }
  }
  return true;
}

void PianoSensors::mainThread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-sensors");
  static_cast<PianoSensors *>(self)->run();
}

void PianoSensors::rpcThread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-sensors-rpc");
  static_cast<PianoSensors *>(self)->runRpc();
}

void PianoSensors::publishStatus(const SgDsp &d, const PianoAdspHealth &h)
{
  char line[200];
  snprintf(line, sizeof line, "stage %d up %u fatal %u/%u link %u charger_pd %#x gen %u quiet %#x%s%s", h.stage, h.dsp_up, h.fatal,
           h.fatal_reports, h.link_generation, h.charger_pd, h.charger_generation, h.quiet, h.crash[0] ? " crash: " : "", h.crash);
  setProperty("piano-sensors-dsp", line);
  IOLockLock(gGuardLock);
  const int state = gGuard.state, reason = gGuard.reason;
  IOLockUnlock(gGuardLock);
  setProperty("piano-sensors-guard", sg_state_name(state));
  setProperty("piano-sensors-guard-reason", sg_reason_name(reason));
  setProperty("piano-sensors-channel", uint64_t(__atomic_load_n(&gChannelUp, __ATOMIC_ACQUIRE)), 32);
  setProperty("piano-sensors-channel-ups", uint64_t(__atomic_load_n(&gChannelUps, __ATOMIC_ACQUIRE)), 32);
  if (gSns.up) snprintf(line, sizeof line, "node %u port %u instance %#x (generation %u)", gSns.node, gSns.port, gSns.instance, gSns.generation);
  else snprintf(line, sizeof line, "not announced (generation %u)", gSns.generation);
  setProperty("piano-sensors-sns", line);
  setProperty("piano-sensors-pack", gPackText);
  setProperty("piano-sensors-smmu", gSmmuText);
  setProperty("piano-sensors-arena", gArenaText);
  const SsrStatus &s = gCore->status;   // a snapshot for a line: a torn counter does no harm
  snprintf(line, sizeof line, "stage %d error %d attaches %u calls %u opens %u missing %u reads %u refused %u declined %u bytes %llu%s%s",
           s.stage, s.error, s.attaches, s.calls, s.opens, s.missing, s.reads, s.refused, s.declined, (unsigned long long)s.bytes,
           s.note[0] ? " first missing " : "", s.note);
  setProperty("piano-sensors-rpc", line);
  if (gEngine) {
    if (OSDictionary *streams = OSDictionary::withCapacity(SS_STREAMS)) {
      for (int i = 0; i < SS_STREAMS; ++i) {
        char text[320];
        ss_status(gEngine, i, text, sizeof text);
        strlcat(text, "; consumer: ", sizeof text);
        strlcat(text, i == SS_LIGHT ? gLightWhy : gAccelWhy, sizeof text);
        if (OSString *v = OSString::withCString(text)) {
          streams->setObject(ss_stream_name(i), v);
          v->release();
        }
      }
      setProperty("piano-sensors-streams", streams);
      streams->release();
    }
  }
  (void)d;
}

void PianoSensors::publishTypes()
{
  if (!gClient) return;
  OSDictionary *dict = OSDictionary::withCapacity(SC_TYPES);
  if (!dict) return;
  for (unsigned i = 0; i < SC_TYPES; ++i) {
    const ScSensor &s = gClient->sensor[i];
    char line[160];
    if (!gClient->up) snprintf(line, sizeof line, "service not announced");
    else if (s.lookup != SC_LOOKUP_ANSWERED) snprintf(line, sizeof line, "pending");
    else if (s.lookup_gave_up) snprintf(line, sizeof line, "no answer");
    else if (!s.found) snprintf(line, sizeof line, "none");
    else if (s.attr != SC_ATTR_READ) snprintf(line, sizeof line, "found %u%s, attributes %s", s.found, s.more ? "+" : "",
                                              s.attr == SC_ATTR_GAVE_UP ? "not read" : "pending");
    else {
      const SscAttrs &a = s.attrs;
      const int32_t low = a.rate_count ? sfx_milli(a.rates[0]) / 1000 : 0, high = a.rate_count ? sfx_milli(a.rates[a.rate_count - 1]) / 1000 : 0;
      snprintf(line, sizeof line, "found %u%s: %s (%s), available %d, %s, rates %d-%d Hz", s.found, s.more ? "+" : "", a.name, a.vendor,
               a.available, a.stream_type == SSC_STREAM_ON_CHANGE ? "on change" : a.stream_type == SSC_STREAM_STREAMING ? "streaming" : "other",
               low, high);
    }
    if (OSString *v = OSString::withCString(line)) {
      dict->setObject(sc_type_name[i], v);
      v->release();
    }
  }
  setProperty("piano-sensors-types", dict);
  dict->release();
}

void PianoSensors::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  stage(1, "survey: health, the channel, the SNS service, the SMMU entries (loads only), the pack");
  surveySmmu();
  note("arena: %s", gArenaText);
  loadPack();
  stage(2, gArgs.level >= 2 ? "survey done; the file service waits for the guard" : "survey done (level 1: nothing else runs)");
  PianoAdspHealth last;
  bzero(&last, sizeof last);
  int lastState = -1;
  uint64_t published = 0;
  bool first = true;
  uint32_t waitMs = gClient ? 250 : 1000;
  if (gEngine) {   // joined once this loop runs, so that a sleep's kOff always finds the thread that acknowledges it
    const IOReturn r = pianopower::join(this);
    note("streams: joined the power tree (%#x): a system sleep disables them first", r);
  }
  for (;;) {
    waitEvents(&gMainEvents, waitMs);
    const uint64_t t = nowMs();
    PianoAdspHealth h;
    piano_adsp_health(&h);
    const SgDsp d = sensors_dsp_of(&h);
    IOLockLock(gGuardLock);
    const int state = sg_observe(&gGuard, &d, t), reason = gGuard.reason;
    IOLockUnlock(gGuardLock);
    if (first || h.stage != last.stage || h.dsp_up != last.dsp_up || h.fatal != last.fatal || h.fatal_reports != last.fatal_reports ||
        h.link_generation != last.link_generation || h.charger_pd != last.charger_pd || h.charger_generation != last.charger_generation ||
        h.quiet != last.quiet || strncmp(h.crash, last.crash, sizeof h.crash)) {
      note("dsp: stage %d up %u fatal %u/%u link %u charger_pd %#x gen %u quiet %#x%s%s%s", h.stage, h.dsp_up, h.fatal, h.fatal_reports,
           h.link_generation, h.charger_pd, h.charger_generation, h.quiet, h.crash[0] ? " crash: " : "", h.crash,
           h.crash[0] && sg_crash_is_sensors(h.crash) ? " (the sensor framework)" : "");
      last = h;
    }
    if (state != lastState) {
      note("guard: %s%s%s", sg_state_name(state), state == SG_OFF ? ": " : "", state == SG_OFF ? sg_reason_name(reason) : "");
      if (state == SG_OFF) note("off for the rest of the boot: no request is sent; the DSP's file calls are answered with an error");
      lastState = state;
    }
    PianoQrtrServer s;
    if (gPort >= 0 && !piano_qrtr_server(gPort, &s) && (s.generation != gSns.generation || s.up != gSns.up)) {
      gSns = s;
      note("SNS service %#x: %s node %u port %u instance %#x (generation %u)", PS_QMI_SNS, s.up ? "announced at" : "not announced,", s.node,
           s.port, s.instance, s.generation);
      if (gClient) sc_service(gClient, s.up, s.generation);
    }
    if (gClient) {
      while (gRing->tail != __atomic_load_n(&gRing->head, __ATOMIC_ACQUIRE)) {
        const Packet &p = gRing->slot[gRing->tail % kRing];
        if (!p.source) sc_receive(gClient, p.data, p.length);
        else if (gEngine && p.source <= SS_STREAMS) ss_receive(gEngine, int(p.source) - 1, p.data, p.length);
        __atomic_store_n(&gRing->tail, gRing->tail + 1, __ATOMIC_RELEASE);
      }
      if (state == SG_RUN || state == SG_BACKOFF) sc_tick(gClient);
      if (gTypesDirty) {
        gTypesDirty = 0;
        publishTypes();
        if (sc_settled(gClient)) note("sensor list settled (piano-sensors-types)");
      }
    }
    if (gEngine) waitMs = runStreams(t, state, reason, h);
    if (first || t - published >= 1000) {
      publishStatus(d, h);
      published = t;
    }
    first = false;
  }
}

// Level 4, on the main thread after the list: the guard's word, the ports, the list's sensors, the conditions, a sleep under way, then
// the engine's tick. Returns how long the thread may wait: 250 ms while the display is on or a stream is busy (the conditions are
// polled), 1 s at rest with the display off, shorter when the engine has a deadline, 20 ms while a sleep waits for the disables.
uint32_t PianoSensors::runStreams(uint64_t t, int state, int reason, const PianoAdspHealth &h)
{
  static bool closedAfterStrike;
  static uint32_t engineAsleep;
  static uint64_t sleepSince, reopenAt;
  const uint32_t asleep = __atomic_load_n(&gAsleep, __ATOMIC_ACQUIRE);
  if (state == SG_OFF && !gEngine->off) ss_off(gEngine);
  if (gEngine->off) {
    // The ports that carried a request are closed (the hub forgets their streams) only while the DSP may still be told: the strike
    // was ours or the charger's restart, the DSP is up without a fatal sign, the charger domain up again and PianoPower not asking
    // for quiet. A dead or dying DSP is never touched.
    const bool tellable = reason == SG_REASON_ERRORS || reason == SG_REASON_FILE_STORM || reason == SG_REASON_DISABLED ||
                          reason == SG_REASON_CHARGER;
    if (!closedAfterStrike && tellable && h.version == PIANO_ADSP_HEALTH_VERSION && h.dsp_up && !h.fatal && !h.fatal_reports &&
        !h.crash[0] && !h.quiet && h.charger_pd == SG_CHARGER_UP) {
      ss_close_all(gEngine);
      closedAfterStrike = true;
    }
    if (__atomic_exchange_n(&gAckDue, 0u, __ATOMIC_ACQ_REL)) acknowledgeSetPowerState();
    return 1000;
  }
  // A port the engine closed (a sleep's wait ran out, a new default sensor) is opened again, never during a sleep.
  for (int i = 0; i < SS_STREAMS; ++i) {
    if (gStreamPort[i] < 0 && !asleep && t >= reopenAt) {
      openStreamPort(i);
      if (gStreamPort[i] < 0) reopenAt = t + 5000;
    }
    PianoQrtrServer s;
    if (gStreamPort[i] >= 0 && !piano_qrtr_server(gStreamPort[i], &s) &&
        (s.generation != gStreamServer[i].generation || s.up != gStreamServer[i].up)) {
      gStreamServer[i] = s;
      ss_service(gEngine, i, s.up, s.generation);
    }
  }
  ss_from_list(gEngine, gClient);
  const SsConditions c = readConditions(t);
  ss_conditions(gEngine, &c);
  if (asleep != engineAsleep) {
    engineAsleep = asleep;
    sleepSince = t;
    ss_sleep(gEngine, int(asleep));
    note("streams: %s", asleep ? "system sleep: disabling them" : "awake");
  }
  ss_tick(gEngine);
  if (asleep && __atomic_load_n(&gAckDue, __ATOMIC_ACQUIRE)) {
    const bool idle = ss_idle(gEngine) != 0;
    if (!idle && t - sleepSince < kSleepWaitMs) return 20;
    // Not off in time: their ports are closed (the hub drops a gone client's requests), unless PianoPower asks for quiet (then they
    // stay on through the sleep: nothing is sent while it configures the charger).
    if (!idle && !h.quiet) ss_force_off(gEngine);
    note("streams: ready for the sleep after %llu ms%s", (unsigned long long)(t - sleepSince),
         idle ? "" : h.quiet ? " (not off: PianoPower asks for quiet)" : " (ports closed)");
    __atomic_store_n(&gAckDue, 0u, __ATOMIC_RELEASE);
    acknowledgeSetPowerState();
  }
  uint32_t wait = c.display != 0 || !ss_idle(gEngine) || !sc_settled(gClient) ? 250 : 1000;
  const uint64_t due = ss_deadline(gEngine);
  if (due && due <= t) wait = 10;
  else if (due && due - t < wait) wait = due - t < 10 ? 10 : uint32_t(due - t);
  return wait;
}

// The power contract (PianoPowerContract.h, docs/power.md §4): kOff is acknowledged by the thread once both streams are disabled (at
// most kSleepWaitMs: then the ports of those not off are closed); kOn lets them start again (fresh sessions: the room's light at once).
IOReturn PianoSensors::setPowerState(unsigned long state, IOService *)
{
  if (!gEngine) return kIOPMAckImplied;
  if (state == pianopower::kOff) {
    if (__atomic_load_n(&gAsleep, __ATOMIC_ACQUIRE)) return kIOPMAckImplied;
    __atomic_store_n(&gAckDue, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&gAsleep, 1u, __ATOMIC_RELEASE);
    wake(&gMainEvents, kEvPower);
    return (kSleepWaitMs + 1200) * 1000;   // microseconds: the thread acknowledges within kSleepWaitMs
  }
  __atomic_store_n(&gAsleep, 0u, __ATOMIC_RELEASE);
  wake(&gMainEvents, kEvPower);
  return kIOPMAckImplied;
}

void PianoSensors::runRpc()
{
  // Level 2: a pack first (or piano-sensors-nopack=1), then the guard's word before each bring-up call, its strikes after.
  while (__atomic_load_n(&gPackState, __ATOMIC_ACQUIRE) < 0) waitEvents(&gRpcEvents, 1000);
  if (gPackState != SL_OK && !gArgs.nopack) {
    note("file service: not started without a valid pack (%s); piano-sensors-nopack=1 serves an empty one", gPackText);
    return;
  }
  stage(3, "the file service: attach when the guard allows, then serve");
  bool parked = false;
  for (;;) {
    const int state = guardState();
    const bool serving = gCore->status.stage >= SsrStListen;
    if (!serving) {
      if (state == SG_OFF) {
        if (!parked) {
          IOLockLock(gGuardLock);
          const int reason = gGuard.reason;
          IOLockUnlock(gGuardLock);
          note("file service: not attaching (the guard is off: %s)", sg_reason_name(reason));
        }
        parked = true;
        waitEvents(&gRpcEvents, 60000);
        continue;
      }
      if (state != SG_RUN || (gCore->status.stage >= SsrStPack && !guardMayRequest())) {
        waitEvents(&gRpcEvents, 250);
        continue;
      }
    }
    const uint32_t attaches = gCore->status.attaches;
    if (!ssr_core_step(gCore)) {
      note("file service stopped: stage %d error %d", gCore->status.stage, gCore->status.error);
      guardResult(0);
      return;
    }
    if (gCore->status.attaches != attaches) guardResult(gCore->status.stage >= SsrStAttach);
  }
}
