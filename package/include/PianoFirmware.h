/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Firmware and data files a native kext reads at run time instead of carrying them in the kernel collection (docs/kc-space.md).
 *
 * The collection's room for the native kexts' __DATA_CONST and __DATA is a few holes between Apple's kexts (tooling/k27/kc-space.py
 * measures it); a firmware image in a kext takes it from every other driver. With this loader the kext carries only a pin (path, size,
 * SHA-256, about 136 bytes in its __TEXT) and gets the bytes at run time, checked against the pin, from (in this order):
 *
 *   1. the kext itself, when it was built with the image inside (the embedded path: the caller passes it, size 0 when not built in);
 *   2. the boot's firmware addon: a RAM image carries the files of its collection's pins as Payloads/kernel.macho.gpuaddon (the
 *      UniXNU addon container "NGPUADD2", entry paths EFI/UniXNU/firmware/...), which DarwinBoot hands over at
 *      /chosen/memory-map GpuAddon (tooling/bootimg/build-ram-images.py; k27 itself reads nothing there);
 *   3. the ESP on UFS (GPT partition "esp", read only through NativeUfs and FatVolume, as PianoSensors reads its pack): the installed
 *      system's EFI/UniXNU/firmware/<path> (tooling/persist/build-esp.py places every pin's file there).
 *
 * The files come from the ignored out/firmware/ (staged by the kexts' build scripts from their sources, never committed). A pin's
 * file that no source has, or has with another size or hash, is an error the kext reports; nothing unchecked is ever returned.
 * Include as <PianoFirmware.h> (package/include is on every kext's include path). The kernel side is package/common/PianoFirmware.cpp
 * with package/common/PianoFirmwareCore.c and a FatVolume.c (package/PianoPower's, or the kext's own identical copy). */
#ifndef PIANO_FIRMWARE_H
#define PIANO_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PIANO_FW_DIR "EFI/UniXNU/firmware"   /* on the ESP; the addon's entries carry the same full paths */
#define PIANO_FW_PATH_BYTES 88u              /* a pin's path below PIANO_FW_DIR, with its NUL */
#define PIANO_FW_MAX_BYTES (16u << 20)       /* one file; more is refused */
#define PIANO_FW_ADDON_MAX_BYTES (64ull << 20)   /* DarwinBoot reads at most 64 MiB of addon */

/* Pin flags. */
enum {
    PIANO_FW_PRIVATE = 1u,   /* this unit's own data (a calibration): never published; the tools print no hash of it */
};

/* A pin: what the kext accepts, kept in the kext (PIANO_FW_PIN) where tooling/persist/firmware.py finds it by its magic, so that the
 * ESP and RAM image builders place exactly the files the collection's kexts ask for. No pointers: it lands in the kext's __TEXT. */
typedef struct {
    char magic[8];                    /* PIANO_FW_PIN_MAGIC_INIT, no NUL */
    char path[PIANO_FW_PATH_BYTES];   /* "<Kext>/<file>": [A-Za-z0-9._-] and '/', no "..", NUL-terminated */
    uint32_t size;                    /* bytes, 1 .. PIANO_FW_MAX_BYTES */
    uint32_t flags;                   /* PIANO_FW_* */
    uint8_t sha256[32];
} PianoFirmwarePin;

#define PIANO_FW_PIN_MAGIC_INIT {'P', 'i', 'a', 'n', 'o', 'F', 'w', '1'}
/* PIANO_FW_PIN(kPinBoe, "PianoTouch/novatek_nt36532_piano_fw_boe.bin", 245760, 0, 0xb7, 0x99, ...): the 32 digest bytes last.
 * tooling/persist/firmware.py pin-header writes these lines from the staged files. */
#define PIANO_FW_PIN(var, path, size, flags, ...) \
    static __attribute__((used, aligned(8))) const PianoFirmwarePin var = {PIANO_FW_PIN_MAGIC_INIT, path, size, flags, {__VA_ARGS__}}

/* Where the bytes came from. */
enum {PIANO_FW_FROM_NONE = 0, PIANO_FW_FROM_EMBEDDED = 1, PIANO_FW_FROM_ADDON = 2, PIANO_FW_FROM_ESP = 3};

/* Results. */
enum {
    PIANO_FW_OK = 0,
    PIANO_FW_BAD_PIN = 1,     /* the pin is malformed (a build error) */
    PIANO_FW_NOT_FOUND = 2,   /* no source has the file */
    PIANO_FW_SIZE = 3,        /* a source has it with another size than the pin's */
    PIANO_FW_HASH = 4,        /* a source has it with the pin's size and another SHA-256 */
    PIANO_FW_READ = 5,        /* a read or decompression failed */
    PIANO_FW_NO_MEMORY = 6,
    PIANO_FW_NO_ESP = 7,      /* NativeUfs published no "esp" partition in time, or it is no FAT volume */
    PIANO_FW_DAMAGED = 8,     /* the addon or the volume's metadata is malformed */
};

typedef struct {
    const uint8_t *data;   /* pin->size checked bytes (virtually contiguous kernel memory: copy into a DMA buffer where hardware reads) */
    uint32_t size;
    uint8_t source;        /* PIANO_FW_FROM_* */
    uint8_t owned;         /* allocated by the loader: piano_fw_release frees it */
    uint8_t reserved[2];
    char trail[160];       /* what each source gave, for the kext's log: "addon: not in this boot; esp: ok" */
} PianoFirmware;

const char *piano_fw_error(int result);
const char *piano_fw_source(int source);

/* Kernel only. The checked file of PIN: EMBEDDED first when embedded_size is not 0 (its size and hash are checked too), then the
 * boot's addon, then the ESP, waiting up to wait_s seconds for NativeUfs to publish it (0: no wait). Thread context only: it reads
 * UFS and may sleep. A failure of one source is noted in out->trail and the next one tried; the result is the last failure. */
int piano_fw_load(const PianoFirmwarePin *pin, const uint8_t *embedded, uint64_t embedded_size, uint32_t wait_s, PianoFirmware *out);
/* Frees what piano_fw_load allocated (nothing for embedded bytes) and clears *fw. */
void piano_fw_release(PianoFirmware *fw);

#ifdef __cplusplus
}
#endif
#endif
