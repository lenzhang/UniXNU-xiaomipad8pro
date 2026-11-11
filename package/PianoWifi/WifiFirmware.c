/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The firmware partition as PianoPower's PianoAdspFirmware.c reads it (same repository, UPSTREAM; PianoPower itself is not
 * modified): the GPT partition from NativeUfs, 4096-byte block reads with one block cached for the ends of a range, and FatVolume.c
 * over them. Only the ranges the caller asks for are read; no file is ever held whole. */
#include <sys/param.h>
#include <sys/systm.h>
#include <IOKit/IOLib.h>
#include "FatVolume.h"
#include "WifiFirmware.h"

/* NativeUfs (package/NativeUfs): GPT lookup by name and 4096-byte block reads of any LUN, read only. */
extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);

static const char *const kPartitions[] = {"modem_a", "modem_b"};
static const char *const kDirectories[] = {"image/peach", "image"};

struct partition {
    uint32_t lun;
    uint64_t first;
    uint64_t cached;   /* the block in `block`; UINT64_MAX: none */
    uint8_t block[4096];
};

static struct partition *gPart;
static FatVolume *gVolume;
static const char *gPartitionName;

static int partitionRead(void *context, uint64_t offset, void *to, size_t length)
{
    struct partition *p = context;
    uint8_t *out = to;
    while (length) {
        const uint64_t lba = offset / 4096;
        const size_t within = (size_t)(offset % 4096);
        if (!within && length >= 4096) {
            const uint32_t n = length / 4096 > 256 ? 256 : (uint32_t)(length / 4096);
            if (!native_ufs_part_read(p->lun, p->first + lba, n, out)) return EIO;
            out += (size_t)n * 4096;
            offset += (uint64_t)n * 4096;
            length -= (size_t)n * 4096;
            continue;
        }
        if (p->cached != lba) {
            p->cached = UINT64_MAX;
            if (!native_ufs_part_read(p->lun, p->first + lba, 1, p->block)) return EIO;
            p->cached = lba;
        }
        {
            const size_t take = 4096 - within < length ? 4096 - within : length;
            memcpy(out, p->block + within, take);
            out += take;
            offset += take;
            length -= take;
        }
    }
    return 0;
}

int piano_wifi_firmware_open(const char **partition)
{
    struct partition *p;
    FatVolume *v;
    int rc = PianoWifiFwNoPartition;
    unsigned i;
    if (gVolume) {
        if (partition) *partition = gPartitionName;
        return PianoWifiFwOk;
    }
    p = IOMallocType(struct partition);
    v = IOMallocType(FatVolume);
    if (!p || !v) {
        if (p) IOFreeType(p, struct partition);
        if (v) IOFreeType(v, FatVolume);
        return PianoWifiFwNoMemory;
    }
    for (i = 0; i < sizeof kPartitions / sizeof kPartitions[0] && rc; ++i) {
        uint64_t blocks = 0;
        if (!native_ufs_part_find(kPartitions[i], &p->lun, &p->first, &blocks)) {
            rc = PianoWifiFwNotReady;   /* the UFS driver may simply not be up yet */
            continue;
        }
        p->cached = UINT64_MAX;
        rc = fat_mount(v, blocks * 4096, partitionRead, p) ? PianoWifiFwNotFat : PianoWifiFwOk;
        if (!rc) {
            gPartitionName = kPartitions[i];
            IOLog("PianoWifi: firmware partition %s (LUN %u, %llu blocks of 4 KiB)\n", kPartitions[i], p->lun,
                  (unsigned long long)blocks);
        }
    }
    if (rc) {
        IOFreeType(p, struct partition);
        IOFreeType(v, FatVolume);
        return rc;
    }
    gPart = p;
    gVolume = v;
    if (partition) *partition = gPartitionName;
    return PianoWifiFwOk;
}

/* The file in the first directory that has it. */
static int open_file(const char *name, FatFile *file, const char **directory)
{
    char path[64];
    unsigned i;
    for (i = 0; i < sizeof kDirectories / sizeof kDirectories[0]; ++i) {
        size_t n = 0, k;
        const char *parts[2] = {kDirectories[i], name};
        unsigned part;
        for (part = 0; part < 2; ++part) {
            if (part) path[n++] = '/';
            for (k = 0; parts[part][k] && n + 1 < sizeof path; ++k) path[n++] = parts[part][k];
        }
        path[n] = 0;
        if (!fat_open(gVolume, path, file) && !file->directory) {
            if (directory) *directory = kDirectories[i];
            return PianoWifiFwOk;
        }
    }
    return PianoWifiFwNotFound;
}

int piano_wifi_firmware_stat(const char *name, uint32_t *bytes, const char **directory)
{
    FatFile file;
    int rc;
    if (!gVolume || !name) return PianoWifiFwNoPartition;
    rc = open_file(name, &file, directory);
    if (rc) return rc;
    if (bytes) *bytes = file.size;
    return PianoWifiFwOk;
}

int piano_wifi_firmware_read(const char *name, uint64_t offset, void *to, size_t length)
{
    FatFile file;
    int rc;
    if (!gVolume || !name || !to) return PianoWifiFwNoPartition;
    rc = open_file(name, &file, NULL);
    if (rc) return rc;
    if (offset > file.size || length > file.size - offset) return PianoWifiFwRange;
    return fat_read(gVolume, &file, offset, to, length) ? PianoWifiFwReadError : PianoWifiFwOk;
}

void piano_wifi_firmware_close(void)
{
    if (gVolume) IOFreeType(gVolume, FatVolume);
    if (gPart) IOFreeType(gPart, struct partition);
    gVolume = NULL;
    gPart = NULL;
    gPartitionName = NULL;
}
