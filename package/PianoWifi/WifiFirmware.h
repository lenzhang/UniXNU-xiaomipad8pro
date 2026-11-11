/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* PianoWifi level 4: the chip's firmware read in place from the tablet's own firmware partition, never from this repository.
 * `modem_a` (else `modem_b`) is a FAT volume read through NativeUfs, read only; the files are under `image/peach/`, else `image/`.
 * Nothing is copied whole into the heap: the caller reads the ranges it needs straight into its DMA buffers (the image is about
 * 6.6 MiB). The firmware is proprietary and must not be redistributed (docs/drivers/wifi.md section 11). */
#ifndef PIANO_WIFI_FIRMWARE_H
#define PIANO_WIFI_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    PianoWifiFwOk = 0,
    PianoWifiFwNotReady = -1,   /* NativeUfs has not published the partition yet: the caller tries again later */
    PianoWifiFwNoPartition = -2,
    PianoWifiFwNotFat = -3,
    PianoWifiFwNotFound = -4,
    PianoWifiFwReadError = -5,
    PianoWifiFwNoMemory = -6,
    PianoWifiFwRange = -7,
};

/* Mounts the firmware partition (idempotent). `partition` and `directory` get the names that worked (static strings). */
int piano_wifi_firmware_open(const char **partition);
/* The size of a firmware file, and the directory it was found in. */
int piano_wifi_firmware_stat(const char *name, uint32_t *bytes, const char **directory);
/* `length` bytes of it at `offset`, straight into `to`. */
int piano_wifi_firmware_read(const char *name, uint64_t offset, void *to, size_t length);
/* Drops the mount and its one-block cache (level 4 is over). */
void piano_wifi_firmware_close(void);

#ifdef __cplusplus
}
#endif
#endif
