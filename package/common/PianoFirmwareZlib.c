/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The addon's zlib streams inflated with the loader's own allocator (PianoFwOps), the same code in the kernel and in the host tests.
 * The kernel's zlib is built with NO_ZCFUNCS: it has no default allocator, so its uncompress() (zalloc 0) would call a null function;
 * inflateInit/inflate/inflateEnd with zalloc/zfree set are what libkern's zlib supports. zfree gets no size, so every block carries
 * its size in a 16-byte prefix. */
#if KERNEL
#include <libkern/zlib.h>
#else
#include <zlib.h>
#endif
#include "PianoFirmwareCore.h"

#define PREFIX 16u

static voidpf block_alloc(voidpf opaque, uInt items, uInt size)
{
    const PianoFwOps *ops = opaque;
    const uint64_t bytes = (uint64_t)items * size;
    if (!bytes || bytes > PIANO_FW_MAX_BYTES) return Z_NULL;
    uint8_t *p = ops->alloc(ops->context, (size_t)bytes + PREFIX);
    if (!p) return Z_NULL;
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(bytes >> (8 * i));
    return p + PREFIX;
}

static void block_free(voidpf opaque, voidpf address)
{
    const PianoFwOps *ops = opaque;
    if (!address) return;
    uint8_t *p = (uint8_t *)address - PREFIX;
    uint64_t bytes = 0;
    for (unsigned i = 0; i < 8; ++i) bytes |= (uint64_t)p[i] << (8 * i);
    ops->release(ops->context, p, (size_t)bytes + PREFIX);
}

/* PianoFwOps.inflate with CONTEXT = the ops themselves (ops.context = &ops): FROM into exactly TO_BYTES at TO. */
int pfw_zlib_inflate(void *context, uint8_t *to, size_t to_bytes, const uint8_t *from, size_t from_bytes)
{
    z_stream s;
    uint8_t *raw = (uint8_t *)&s;
    for (size_t i = 0; i < sizeof s; ++i) raw[i] = 0;
    if (!to_bytes || to_bytes > PIANO_FW_MAX_BYTES || !from_bytes || from_bytes > PIANO_FW_ADDON_MAX_BYTES) return -1;
    s.next_in = (Bytef *)(uintptr_t)from;
    s.avail_in = (uInt)from_bytes;
    s.next_out = to;
    s.avail_out = (uInt)to_bytes;
    s.zalloc = block_alloc;
    s.zfree = block_free;
    s.opaque = context;
    if (inflateInit(&s) != Z_OK) return -1;
    const int rc = inflate(&s, Z_FINISH);
    const int ok = rc == Z_STREAM_END && s.total_out == to_bytes && s.avail_in == 0;
    inflateEnd(&s);
    return ok ? 0 : -1;
}
