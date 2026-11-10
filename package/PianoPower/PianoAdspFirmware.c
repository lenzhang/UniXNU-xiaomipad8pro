// SPDX-License-Identifier: BSD-2-Clause-Patent
// The audio DSP's firmware files (they are never part of the build). UniXNU-sheng's AdspFirmware.c (UPSTREAM) with piano's layout:
// - "modem:NAME": the device's own split image in /image of its firmware partition (modem_a, else modem_b), a FAT volume read in place
//   through NativeUfs (read only; nothing is installed and the boot image does not grow). piano keeps the headers in NAME.mdt (no
//   NAME.b00) and segment i in NAME.bNN; a segment without file bytes has no piece (adsp.b27 does not exist);
// - any other path: a joined .mbn on a mounted volume, read through VFS (piano-power-fw=DIR).
// Plain reads on the caller's thread: the DSP thread is the only caller.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include "FatVolume.h"
#include "QcomMdt.h"
#include "PianoAdsp.h"

// NativeUfs (package/NativeUfs): GPT lookup by name and 4096-byte block reads of any LUN, read only.
extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);

// ---- the firmware partition ----
struct partition {
    uint32_t lun;
    uint64_t first;
    uint64_t cached;  // block in block (UINT64_MAX: none)
    uint8_t block[4096];
};
static struct partition *gPart;
static FatVolume *gVolume;

// Bytes of the partition: whole blocks straight into to, the ends of a range through the one-block cache.
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
            out += (size_t)n * 4096; offset += (uint64_t)n * 4096; length -= (size_t)n * 4096;
            continue;
        }
        if (p->cached != lba) {
            p->cached = UINT64_MAX;
            if (!native_ufs_part_read(p->lun, p->first + lba, 1, p->block)) return EIO;
            p->cached = lba;
        }
        const size_t take = 4096 - within < length ? 4096 - within : length;
        memcpy(out, p->block + within, take);
        out += take; offset += take; length -= take;
    }
    return 0;
}

static int mountPartition(void)
{
    static const char *const names[] = {"modem_a", "modem_b"};
    if (gVolume) return 0;
    struct partition *p = IOMallocType(struct partition);
    FatVolume *v = IOMallocType(FatVolume);
    int rc = ENOENT;
    for (unsigned i = 0; i < 2 && rc; ++i) {
        uint64_t blocks = 0;
        // ENXIO while the UFS driver is not ready yet: the caller tries again later
        if (!native_ufs_part_find(names[i], &p->lun, &p->first, &blocks)) {rc = ENXIO; continue;}
        p->cached = UINT64_MAX;
        rc = fat_mount(v, blocks * 4096, partitionRead, p) ? EFTYPE : 0;
        if (!rc) IOLog("PianoPower: adsp: firmware partition %s (LUN %u, %llu blocks of 4 KiB)\n", names[i], p->lun, blocks);
    }
    if (rc) {IOFreeType(p, struct partition); IOFreeType(v, FatVolume); return rc;}
    gPart = p;
    gVolume = v;
    return 0;
}

struct modem_image {
    QcomMdtSplit split;
    FatFile piece[QCOM_MDT_MAX_SEGMENTS];
};

static int pieceRead(void *context, unsigned piece, uint64_t offset, void *to, size_t length)
{
    struct modem_image *m = context;
    return fat_read(gVolume, &m->piece[piece], offset, to, length);
}

static int openModem(const char *name, struct modem_image **image, uint64_t *size)
{
    char path[48];
    int rc = mountPartition();
    if (rc) return rc;
    struct modem_image *m = IOMallocType(struct modem_image);
    FatFile headers;
    // the headers: NAME.b00 where the image has one, else the start of NAME.mdt
    snprintf(path, sizeof path, "image/%s.b00", name);
    rc = fat_open(gVolume, path, &headers);
    if (rc == FatNotFound) {
        snprintf(path, sizeof path, "image/%s.mdt", name);
        rc = fat_open(gVolume, path, &headers);
    }
    if (rc == FatNotFound) rc = ENOENT;
    else if (rc) rc = EIO;
    else if (headers.size < 52 || headers.size > 1024 * 1024) rc = EFTYPE;
    const uint32_t bytes = rc ? 0 : (headers.size > 64 * 1024 ? 64 * 1024 : headers.size);  // the headers are at its start
    uint8_t *copy = rc ? NULL : IOMallocData(bytes);
    if (!rc && !copy) rc = ENOMEM;
    if (!rc && fat_read(gVolume, &headers, 0, copy, bytes)) rc = EIO;
    if (!rc && qcom_mdt_split(&m->split, copy, bytes)) rc = EFTYPE;
    if (copy) IOFreeData(copy, bytes);
    for (unsigned i = 0; !rc && i < m->split.count; ++i) {
        if (!m->split.piece[i].size) continue;
        if (i == 0) {m->piece[0] = headers; continue;}
        snprintf(path, sizeof path, "image/%s.b%02u", name, i);
        if (fat_open(gVolume, path, &m->piece[i])) rc = ENOENT;
        else if (m->piece[i].size < m->split.piece[i].size) rc = EFTYPE;
    }
    if (rc) {IOFreeType(m, struct modem_image); return rc;}
    *image = m;
    *size = m->split.size;
    return 0;
}

// ---- both kinds ----
struct piano_fw {
    vnode_t vnode;
    struct modem_image *modem;
};

int piano_fw_open(const char *path, void **handle, uint64_t *size)
{
    struct piano_fw *fw = IOMallocType(struct piano_fw);
    int error;
    if (!strncmp(path, "modem:", 6)) {
        error = openModem(path + 6, &fw->modem, size);
    } else {
        vfs_context_t ctx = vfs_context_kernel();
        struct vnode_attr va;
        error = vnode_open(path, FREAD, 0, 0, &fw->vnode, ctx);
        if (!error) {
            VATTR_INIT(&va);
            VATTR_WANTED(&va, va_data_size);
            if ((error = vnode_getattr(fw->vnode, &va, ctx)) || !VATTR_IS_SUPPORTED(&va, va_data_size) || !va.va_data_size) {
                vnode_close(fw->vnode, FREAD, ctx);
                fw->vnode = NULLVP;
                if (!error) error = EFTYPE;
            } else {
                *size = va.va_data_size;
            }
        }
    }
    if (error) {IOFreeType(fw, struct piano_fw); return error;}
    *handle = fw;
    return 0;
}

int piano_fw_read(void *handle, uint64_t offset, void *to, size_t length)
{
    struct piano_fw *fw = handle;
    if (fw->modem) return qcom_mdt_split_read(&fw->modem->split, pieceRead, fw->modem, offset, to, length) ? EIO : 0;
    vfs_context_t ctx = vfs_context_kernel();
    uint8_t *out = (uint8_t *)to;
    while (length) {
        const int want = length > (1u << 30) ? (1 << 30) : (int)length;
        int resid = want;
        const int error = vn_rdwr(UIO_READ, fw->vnode, (caddr_t)out, want, (off_t)offset, UIO_SYSSPACE, IO_NOAUTH,
                                  vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (error) return error;
        if (resid == want) return EIO;  /* end of file before the bytes asked for */
        out += want - resid;
        offset += (uint64_t)(want - resid);
        length -= (size_t)(want - resid);
    }
    return 0;
}

void piano_fw_close(void *handle)
{
    struct piano_fw *fw = handle;
    if (!fw) return;
    if (fw->modem) IOFreeType(fw->modem, struct modem_image);
    if (fw->vnode) vnode_close(fw->vnode, FREAD, vfs_context_kernel());
    IOFreeType(fw, struct piano_fw);
}
