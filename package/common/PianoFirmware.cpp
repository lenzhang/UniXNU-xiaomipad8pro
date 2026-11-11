// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel side of <PianoFirmware.h>: the three sources (built in, the boot's addon, the ESP through NativeUfs) over the portable
// PianoFirmwareCore.c / PianoFirmwareZlib.c. Compiled into each kext that uses it (with one FatVolume.c), so no kext depends on
// another for it; the only imports are NativeUfs's partition reader (the kext links after NativeUfs, as every user of the ESP does)
// and the kernel's (IOMallocData, libkern's SHA-256 and zlib, the device tree, ml_static_ptovirt).
//
// piano-fw-sources=MASK (default 7) leaves sources out for a test: 1 built in, 2 the addon, 4 the ESP; e.g. 4 makes a RAM image that
// carries the addon read the tablet's ESP instead.
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/crypto/sha2.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoFirmware.h>
#include "PianoFirmwareCore.h"

extern "C" {
// NativeUfs (package/NativeUfs): GPT lookup by name and 4096-byte block reads of any LUN, read only.
extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
extern unsigned long gPhysBase, gPhysSize;
}

namespace {

constexpr const char *kEsp = "esp";   // GPT partition 37 on LUN 0, FAT32 with 4096-byte sectors (docs/persistent-boot.md)
enum : uint32_t {kFromEmbedded = 1, kFromAddon = 2, kFromEsp = 4};

void *opAlloc(void *, size_t bytes) {return IOMallocData(bytes);}
void opRelease(void *, void *data, size_t bytes) {IOFreeData(data, bytes);}
void opSha256(void *, const void *data, size_t bytes, uint8_t digest[32])
{
  SHA256_CTX c;
  SHA256_Init(&c);
  SHA256_Update(&c, data, bytes);
  SHA256_Final(digest, &c);
}

struct Partition {
  uint32_t lun;
  uint64_t first;
  uint64_t cached;   // the block in `block`; ~0: none
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

// The boot's addon: /chosen/memory-map GpuAddon {physical, bytes}, which DarwinBoot fills when the payload carries
// kernel.macho.gpuaddon (inside the boot's static memory, below topOfKernelData: never allocated or freed here).
bool bootAddon(const uint8_t **data, uint64_t *bytes)
{
  IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
  if (!map) return false;
  OSData *range = OSDynamicCast(OSData, map->getProperty("GpuAddon"));
  uint64_t physical = 0, length = 0;
  if (range && range->getLength() == 16) {
    memcpy(&physical, range->getBytesNoCopy(), 8);
    memcpy(&length, static_cast<const uint8_t *>(range->getBytesNoCopy()) + 8, 8);
  }
  map->release();
  if (!length || length > PIANO_FW_ADDON_MAX_BYTES || physical < gPhysBase || length > gPhysSize || physical - gPhysBase > gPhysSize - length)
    return false;
  *data = reinterpret_cast<const uint8_t *>(ml_static_ptovirt(physical));
  *bytes = length;
  return *data != nullptr;
}

int fromEsp(const PianoFirmwarePin *pin, uint32_t waitS, const PianoFwOps *ops, uint8_t **data)
{
  uint32_t lun = 0;
  uint64_t first = 0, blocks = 0;
  for (uint32_t waited = 0; !native_ufs_part_find(kEsp, &lun, &first, &blocks); ++waited) {
    if (waited >= waitS) return PIANO_FW_NO_ESP;
    IOSleep(1000);
  }
  Partition *part = IOMallocType(Partition);
  FatVolume *volume = IOMallocType(FatVolume);
  int rc = PIANO_FW_NO_MEMORY;
  if (part && volume) {
    part->lun = lun;
    part->first = first;
    part->cached = ~0ull;
    rc = fat_mount(volume, blocks * 4096, partitionRead, part) ? PIANO_FW_NO_ESP : pfw_from_fat(volume, pin, ops, data);
  }
  if (part) IOFreeType(part, Partition);
  if (volume) IOFreeType(volume, FatVolume);
  return rc;
}

__attribute__((format(printf, 2, 3))) void trail(PianoFirmware *out, const char *format, ...)
{
  const size_t used = strnlen(out->trail, sizeof out->trail);
  if (used + 1 >= sizeof out->trail) return;
  va_list ap;
  va_start(ap, format);
  vsnprintf(out->trail + used, sizeof out->trail - used, format, ap);
  va_end(ap);
}

}   // namespace

extern "C" int piano_fw_load(const PianoFirmwarePin *pin, const uint8_t *embedded, uint64_t embeddedSize, uint32_t waitS, PianoFirmware *out)
{
  bzero(out, sizeof *out);
  if (!pfw_pin_ok(pin)) {
    IOLog("PianoFirmware: a malformed pin\n");
    return PIANO_FW_BAD_PIN;
  }
  uint32_t sources = kFromEmbedded | kFromAddon | kFromEsp;
  PE_parse_boot_argn("piano-fw-sources", &sources, sizeof sources);
  PianoFwOps ops = {nullptr, opAlloc, opRelease, opSha256, pfw_zlib_inflate};
  ops.context = &ops;   // pfw_zlib_inflate allocates through the ops
  int last = PIANO_FW_NOT_FOUND;
  auto keep = [&last](int rc) {if (rc != PIANO_FW_NOT_FOUND || last == PIANO_FW_NOT_FOUND) last = rc;};
  if (embeddedSize && (sources & kFromEmbedded)) {
    const int rc = pfw_check(pin, embedded, embeddedSize, &ops);
    trail(out, "built in: %s", piano_fw_error(rc));
    if (!rc) {
      out->data = embedded;
      out->source = PIANO_FW_FROM_EMBEDDED;
    }
    keep(rc);
  }
  if (!out->data && (sources & kFromAddon)) {
    const uint8_t *addon = nullptr;
    uint64_t bytes = 0;
    uint8_t *data = nullptr;
    const int rc = bootAddon(&addon, &bytes) ? pfw_from_addon(addon, bytes, pin, &ops, &data) : PIANO_FW_NOT_FOUND;
    trail(out, "%saddon: %s", out->trail[0] ? "; " : "", addon ? piano_fw_error(rc) : "none in this boot");
    if (!rc) {
      out->data = data;
      out->owned = 1;
      out->source = PIANO_FW_FROM_ADDON;
    }
    keep(rc);
  }
  if (!out->data && (sources & kFromEsp)) {
    uint8_t *data = nullptr;
    const int rc = fromEsp(pin, waitS, &ops, &data);
    trail(out, "%sesp: %s", out->trail[0] ? "; " : "", piano_fw_error(rc));
    if (!rc) {
      out->data = data;
      out->owned = 1;
      out->source = PIANO_FW_FROM_ESP;
    }
    keep(rc);
  }
  if (out->data) {
    out->size = pin->size;
    last = PIANO_FW_OK;
  }
  IOLog("PianoFirmware: %s/%.*s: %s, %u bytes (%s)\n", PIANO_FW_DIR, int(PIANO_FW_PATH_BYTES), pin->path,
        out->data ? piano_fw_source(out->source) : piano_fw_error(last), out->size, out->trail[0] ? out->trail : "no source tried");
  return last;
}

extern "C" void piano_fw_release(PianoFirmware *fw)
{
  if (!fw) return;
  if (fw->owned && fw->data) IOFreeData(const_cast<uint8_t *>(fw->data), fw->size);
  bzero(fw, sizeof *fw);
}
