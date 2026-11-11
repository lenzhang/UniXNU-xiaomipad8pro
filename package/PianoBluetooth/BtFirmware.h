/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* PianoBluetooth level 4: the controller's rampatch and NVM read in place from the tablet's own firmware partition, never from this
 * repository. `bluetooth_a` (else `bluetooth_b`) is a FAT volume read through NativeUfs, read only; the files are under `image/`
 * (gngbtfw20.mbn, gngbtnv20.bXX by board id, gngbtnv20.bin). The firmware is proprietary and must not be redistributed
 * (docs/drivers/bluetooth.md section 11); nothing of it is logged but names and sizes. */
#ifndef PIANO_BT_FIRMWARE_H
#define PIANO_BT_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    PianoBtFwOk = 0,
    PianoBtFwNotReady = -1,   /* NativeUfs has not published the partition yet */
    PianoBtFwNoPartition = -2,
    PianoBtFwNotFat = -3,
    PianoBtFwNotFound = -4,
    PianoBtFwReadError = -5,
    PianoBtFwNoMemory = -6,
    PianoBtFwRange = -7,
};

/* Mounts the firmware partition (idempotent). `partition` gets the name that worked (a static string). */
int piano_bt_firmware_open(const char **partition);
/* The size of a firmware file. */
int piano_bt_firmware_stat(const char *name, uint32_t *bytes);
/* `length` bytes of it at `offset`, straight into `to`. */
int piano_bt_firmware_read(const char *name, uint64_t offset, void *to, size_t length);
/* Drops the mount and its one-block cache (level 4 is over). */
void piano_bt_firmware_close(void);

#ifdef __cplusplus
}
#endif
#endif
