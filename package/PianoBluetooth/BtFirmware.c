/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The firmware partition as PianoWifi's WifiFirmware.c and PianoPower's PianoAdspFirmware.c read theirs (same repository, UPSTREAM;
 * neither is modified): the GPT partition from NativeUfs, 4096-byte block reads with one block cached for the ends of a range, and
 * package/PianoPower/FatVolume.c (compiled into this kext, not copied) over them. Only the ranges the caller asks for are read. */
#include <sys/param.h>
#include <sys/systm.h>
#include <IOKit/IOLib.h>
#include "FatVolume.h"
#include "BtFirmware.h"

/* NativeUfs (package/NativeUfs): GPT lookup by name and 4096-byte block reads of any LUN, read only. */
extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);

static const char *const kPartitions[] = {"bluetooth_a", "bluetooth_b"};
static const char *const kDirectory = "image";

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

int piano_bt_firmware_open(const char **partition)
{
    struct partition *p;
    FatVolume *v;
    int rc = PianoBtFwNoPartition;
    unsigned i;
    if (gVolume) {
        if (partition) *partition = gPartitionName;
        return PianoBtFwOk;
    }
    p = IOMallocType(struct partition);
    v = IOMallocType(FatVolume);
    if (!p || !v) {
        if (p) IOFreeType(p, struct partition);
        if (v) IOFreeType(v, FatVolume);
        return PianoBtFwNoMemory;
    }
    for (i = 0; i < sizeof kPartitions / sizeof kPartitions[0] && rc; ++i) {
        uint64_t blocks = 0;
        if (!native_ufs_part_find(kPartitions[i], &p->lun, &p->first, &blocks)) {
            rc = PianoBtFwNotReady;   /* the UFS driver may simply not be up yet */
            continue;
        }
        p->cached = UINT64_MAX;
        rc = fat_mount(v, blocks * 4096, partitionRead, p) ? PianoBtFwNotFat : PianoBtFwOk;
        if (!rc) {
            gPartitionName = kPartitions[i];
            IOLog("PianoBluetooth: firmware partition %s (LUN %u, %llu blocks of 4 KiB)\n", kPartitions[i], p->lun,
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
    return PianoBtFwOk;
}

/* image/<name>. */
static int open_file(const char *name, FatFile *file)
{
    char path[64];
    size_t n = 0, k;
    for (k = 0; kDirectory[k] && n + 1 < sizeof path; ++k) path[n++] = kDirectory[k];
    path[n++] = '/';
    for (k = 0; name[k] && n + 1 < sizeof path; ++k) path[n++] = name[k];
    path[n] = 0;
    if (fat_open(gVolume, path, file) || file->directory) return PianoBtFwNotFound;
    return PianoBtFwOk;
}

int piano_bt_firmware_stat(const char *name, uint32_t *bytes)
{
    FatFile file;
    int rc;
    if (!gVolume || !name) return PianoBtFwNoPartition;
    rc = open_file(name, &file);
    if (rc) return rc;
    if (bytes) *bytes = file.size;
    return PianoBtFwOk;
}

int piano_bt_firmware_read(const char *name, uint64_t offset, void *to, size_t length)
{
    FatFile file;
    int rc;
    if (!gVolume || !name || !to) return PianoBtFwNoPartition;
    rc = open_file(name, &file);
    if (rc) return rc;
    if (offset > file.size || length > file.size - offset) return PianoBtFwRange;
    return fat_read(gVolume, &file, offset, to, length) ? PianoBtFwReadError : PianoBtFwOk;
}

void piano_bt_firmware_close(void)
{
    if (gVolume) IOFreeType(gVolume, FatVolume);
    if (gPart) IOFreeType(gPart, struct partition);
    gVolume = NULL;
    gPart = NULL;
    gPartitionName = NULL;
}
