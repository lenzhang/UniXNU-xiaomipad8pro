// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// SensorsLoad.c over FatVolume.c on a FAT32 image made by mtools as build-esp.py makes the ESP (4096-byte sectors, one per cluster),
// holding a pack from tools/make-sensor-pack.py at SL_PATH, a damaged copy, a directory and a file too big:
//   load IMAGE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <sys/stat.h>
#include "check.h"
#include "../SensorsLoad.h"

static FILE *image;

static int image_read(void *context, uint64_t offset, void *to, size_t length)
{
    (void)context;
    if (fseeko(image, (off_t)offset, SEEK_SET)) return -1;
    return fread(to, 1, length, image) == length ? 0 : -1;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    image = fopen(argv[1], "rb");
    CHECK(image);
    struct stat st;
    CHECK(!stat(argv[1], &st));
    static FatVolume v;
    CHECK(!fat_mount(&v, (uint64_t)st.st_size, image_read, NULL) && v.fat32 && v.sector_bytes == 4096);
    static uint8_t buf[SL_MAX_BYTES];
    uint32_t size = 0;
    SspPack pack;
    CHECK(sl_load(&v, SL_PATH, buf, sizeof buf, &size, &pack) == SL_OK && size > SSP_HEADER && pack.count > 0);
    CHECK(sl_contents(&pack) == (SL_HAS_REG_CONFIG | SL_HAS_JSON_LIST | SL_HAS_REGISTRY | SL_HAS_SOC0));
    CHECK(ssp_find(&pack, "/vendor/etc/sensors/sns_reg_config") >= 0);
    CHECK(sl_load(&v, "efi/unixnu/PIANO-SENSORS.PACK", buf, sizeof buf, &size, &pack) == SL_OK);   // any case, as FAT does
    CHECK(sl_load(&v, SL_PATH, buf, 64, &size, &pack) == SL_TOO_BIG && size == 0);
    CHECK(sl_load(&v, "EFI/UniXNU/missing-sensors.pack", buf, sizeof buf, &size, &pack) == SL_NOT_FOUND);
    CHECK(sl_load(&v, "EFI/UniXNU", buf, sizeof buf, &size, &pack) == SL_NOT_FOUND);
    CHECK(sl_load(&v, "EFI/UniXNU/piano-sensors.pack/x", buf, sizeof buf, &size, &pack) == SL_NOT_FOUND);
    CHECK(sl_load(&v, "EFI/UniXNU/bad-sensors.pack", buf, sizeof buf, &size, &pack) == SL_INVALID);
    CHECK(sl_load(&v, "EFI/UniXNU/short-sensors.pack", buf, sizeof buf, &size, &pack) == SL_INVALID);
    CHECK(sl_load(&v, "EFI/UniXNU/big-sensors.pack", buf, sizeof buf, &size, &pack) == SL_TOO_BIG);
    SspPack empty = {0, 0, 0, 0};
    CHECK(sl_contents(&empty) == 0);
    for (int e = 0; e <= SL_INVALID; ++e) CHECK(strcmp(sl_error_name(e), "?"));
    fclose(image);
    printf("PASS pack loader: the ESP's FAT32 (4096-byte sectors) as build-esp.py makes it, a valid pack, its contents, refusals: %u checks\n",
           checks);
    return 0;
}
