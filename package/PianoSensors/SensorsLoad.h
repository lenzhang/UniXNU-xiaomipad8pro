/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The sensor pack on the ESP (docs/drivers/sensors.md section 5): found by its path on the FAT volume, read whole into memory and
 * checked (header, table, names, every file inside the pack) before the file service may attach, so that no DSP file call ever waits
 * for UFS. The ESP is only read. Portable C over FatVolume.h; host-tested against an mtools FAT32 image (tests/piano.c). */
#ifndef SENSORS_LOAD_H
#define SENSORS_LOAD_H
#include <stdint.h>
#include "FatVolume.h"
#include "SensorsPack.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SL_PARTITION "esp"                   /* GPT partition 37 on LUN 0, FAT32 with 4096-byte sectors (persistent-boot.md) */
#define SL_PATH "EFI/UniXNU/piano-sensors.pack"   /* where tooling/persist/build-esp.py --sensor-pack puts it (a long name:
                                                * FAT keeps its case) */
#define SL_MAX_BYTES (4u << 20)              /* a pack is a few hundred KiB; more is refused */
enum {SL_OK = 0, SL_NOT_FAT = 1, SL_NOT_FOUND = 2, SL_TOO_BIG = 3, SL_READ = 4, SL_INVALID = 5};

/* Reads the file at path of the mounted volume into buf (capacity cap) and opens it as a pack: SL_OK with *size and *pack set. */
int sl_load(FatVolume *v, const char *path, uint8_t *buf, uint32_t cap, uint32_t *size, SspPack *pack);
const char *sl_error_name(int error);

/* What the pack holds of what the sensor PD reads (bit set: present): the registry configuration, the ODM list, the persist registry,
 * the soc0 identity. Paths only: nothing of the contents is looked at. */
enum {SL_HAS_REG_CONFIG = 1, SL_HAS_JSON_LIST = 2, SL_HAS_REGISTRY = 4, SL_HAS_SOC0 = 8};
uint32_t sl_contents(const SspPack *pack);

#ifdef __cplusplus
}
#endif
#endif
