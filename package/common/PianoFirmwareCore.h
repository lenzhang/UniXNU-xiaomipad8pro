/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The portable part of the firmware loader (<PianoFirmware.h>): pins, the check, a file from a FAT volume, an entry of the boot's
 * addon. No allocation, hashing or decompression of its own: the caller's ops do them (the kernel: IOMallocData, libkern's SHA-256 and
 * zlib; the host tests: malloc, CommonCrypto, zlib). No C library either, so it compiles freestanding (package/common/tests). The addon
 * and the volume are untrusted: every length is bounds checked, every walk bounded, and nothing is returned before its size and SHA-256
 * match the pin. */
#ifndef PIANO_FIRMWARE_CORE_H
#define PIANO_FIRMWARE_CORE_H
#include <PianoFirmware.h>
#include "FatVolume.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *context;
    void *(*alloc)(void *context, size_t bytes);                                  /* NULL: no memory */
    void (*release)(void *context, void *data, size_t bytes);
    void (*sha256)(void *context, const void *data, size_t bytes, uint8_t digest[32]);
    /* zlib stream FROM into exactly TO_BYTES at TO: 0 on success (a stream that gives more or fewer bytes is an error) */
    int (*inflate)(void *context, uint8_t *to, size_t to_bytes, const uint8_t *from, size_t from_bytes);
} PianoFwOps;

/* The addon container (UniXNU scripts/make-gpu-addon.py's layout): "NGPUADD2", u32 count, u32 0, u64 total bytes, then per entry
 * u32 path bytes (with the NUL, at most 160), u32 mode, u64 size, u64 compressed size, the path padded to 8 bytes, the zlib stream
 * padded to 8 bytes. */
#define PFW_ADDON_MAGIC "NGPUADD2"
#define PFW_ADDON_HEADER 24u
#define PFW_ADDON_ENTRY 24u
#define PFW_ADDON_PATH_MAX 160u
#define PFW_ADDON_MAX_ENTRIES 4096u

typedef struct {
    const char *path;          /* inside the addon, NUL-terminated */
    uint32_t mode;
    uint64_t size;             /* inflated */
    const uint8_t *packed;     /* the zlib stream */
    uint64_t packed_bytes;
} PfwAddonEntry;

/* 1 when PIN is well formed (magic, a NUL-terminated path of the allowed characters without "." or ".." components, a size). */
int pfw_pin_ok(const PianoFirmwarePin *pin);
/* PIANO_FW_DIR "/" pin->path into out (cap bytes); its length, 0 when it does not fit. */
size_t pfw_full_path(const PianoFirmwarePin *pin, char *out, size_t cap);
/* PIANO_FW_OK when BYTES are the pin's size and DATA hashes to its SHA-256, else PIANO_FW_SIZE / PIANO_FW_HASH. */
int pfw_check(const PianoFirmwarePin *pin, const uint8_t *data, uint64_t bytes, const PianoFwOps *ops);
/* The pin's file from the mounted volume, at PIANO_FW_DIR/path: *data = allocated, checked bytes (pin->size of them). */
int pfw_from_fat(FatVolume *v, const PianoFirmwarePin *pin, const PianoFwOps *ops, uint8_t **data);
/* The addon entry named PATH (exactly, case included). PIANO_FW_OK, PIANO_FW_NOT_FOUND or PIANO_FW_DAMAGED (the whole container is
 * walked bounds checked first, so a damaged one is refused even when the entry comes before the damage). */
int pfw_addon_find(const uint8_t *addon, uint64_t bytes, const char *path, PfwAddonEntry *entry);
/* The pin's file from the addon: *data = allocated, inflated, checked bytes. */
int pfw_from_addon(const uint8_t *addon, uint64_t bytes, const PianoFirmwarePin *pin, const PianoFwOps *ops, uint8_t **data);
/* PianoFirmwareZlib.c: PianoFwOps.inflate over zlib (libkern's in the kernel) with the ops' own allocator: set ops.context = &ops. */
int pfw_zlib_inflate(void *context, uint8_t *to, size_t to_bytes, const uint8_t *from, size_t from_bytes);

#ifdef __cplusplus
}
#endif
#endif
