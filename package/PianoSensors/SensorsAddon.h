/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The sensor pack carried by a RAM test image (docs/drivers/sensors.md section 5b, build-ram-images.py's PIANO_SENSOR_PACK), so that a
 * RAM boot needs no pack on the installed ESP. DarwinBoot hands the kernel one file of the boot payload besides the ramdisk and the
 * trust cache: Payloads/kernel.macho.gpuaddon, copied into the kernel's memory and named by the ADT's /chosen/memory-map "GpuAddon"
 * {address, size} (UniXNU's addon slot; DarwinBoot checks only the header, the kernel checks the rest; the k27 kernel has no GPU
 * addon of its own, so the slot is free natively). The layout is UniXNU's (scripts/make-gpu-addon.py), little endian:
 *   "NGPUADD2", u32 count, u32 0, u64 total bytes (= the file's size)
 *   per file: u32 path bytes (with its NUL, at most 160), u32 mode, u64 size, u64 compressed size, the path padded to 8 bytes, the
 *   zlib stream padded to 8 bytes.
 * PianoSensors takes the one entry SA_PACK_PATH (package/PianoSensors/tools/sensor_addon.py writes it) and ignores any other; the
 * container is checked whole before anything is used. Portable C, no allocation, no C library; host-tested (tests/addon.c). */
#ifndef SENSORS_ADDON_H
#define SENSORS_ADDON_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SA_MAGIC "NGPUADD2"
#define SA_PACK_PATH "sensors/piano-sensors.pack"
#define SA_HEADER 24u
#define SA_ENTRY 24u
#define SA_MAX_PATH 160u
#define SA_MAX_ENTRIES 4096u
#define SA_MAX_BYTES (64u << 20)             /* DarwinBoot reads at most 64 MiB of it */
enum {SA_OK = 0, SA_NOT_FOUND = 1, SA_INVALID = 2};

typedef struct {
    const char *path;                        /* inside the container, NUL terminated */
    uint32_t mode;
    uint64_t size;                           /* the file's bytes */
    uint64_t compressed;                     /* its zlib stream's bytes */
    const uint8_t *data;                     /* the zlib stream */
} SaEntry;

/* Checks the whole container (header, every entry inside the total, paths terminated and without other NULs, nothing after the last
 * entry) and finds path: SA_OK with *out, SA_NOT_FOUND (a valid container without it), SA_INVALID. */
int sa_find(const uint8_t *blob, uint64_t length, const char *path, SaEntry *out);
const char *sa_error_name(int error);

#ifdef __cplusplus
}
#endif
#endif
