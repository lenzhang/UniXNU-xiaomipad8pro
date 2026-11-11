/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The sensor pack carried by a RAM test image: see SensorsAddon.h. */
#include "SensorsAddon.h"

static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t rd64(const uint8_t *p) {return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {++a; ++b;}
    return *a == *b;
}

int sa_find(const uint8_t *blob, uint64_t length, const char *path, SaEntry *out)
{
    static const char magic[] = SA_MAGIC;
    if (!blob || !path || length < SA_HEADER || length > SA_MAX_BYTES) return SA_INVALID;
    for (unsigned i = 0; i < 8; ++i)
        if (blob[i] != (uint8_t)magic[i]) return SA_INVALID;
    const uint32_t count = rd32(blob + 8);
    if (rd32(blob + 12) || rd64(blob + 16) != length || count > SA_MAX_ENTRIES) return SA_INVALID;
    uint64_t at = SA_HEADER;
    int found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (length - at < SA_ENTRY) return SA_INVALID;
        const uint32_t name_bytes = rd32(blob + at), mode = rd32(blob + at + 4);
        const uint64_t size = rd64(blob + at + 8), compressed = rd64(blob + at + 16);
        at += SA_ENTRY;
        if (name_bytes < 2 || name_bytes > SA_MAX_PATH) return SA_INVALID;
        const uint64_t name_padded = (name_bytes + 7u) & ~7u;
        if (length - at < name_padded) return SA_INVALID;
        const char *name = (const char *)(blob + at);
        if (name[name_bytes - 1]) return SA_INVALID;
        for (uint32_t k = 0; k + 1 < name_bytes; ++k)
            if (!name[k]) return SA_INVALID;
        at += name_padded;
        if (compressed > length - at) return SA_INVALID;
        const uint64_t stream_padded = (compressed + 7u) & ~(uint64_t)7u;
        if (stream_padded > length - at) return SA_INVALID;
        if (!found && same(name, path)) {
            found = 1;
            if (out) {
                out->path = name;
                out->mode = mode;
                out->size = size;
                out->compressed = compressed;
                out->data = blob + at;
            }
        }
        at += stream_padded;
    }
    if (at != length) return SA_INVALID;
    return found ? SA_OK : SA_NOT_FOUND;
}

const char *sa_error_name(int error)
{
    switch (error) {
    case SA_OK: return "ok";
    case SA_NOT_FOUND: return "no sensor pack in it";
    case SA_INVALID: return "not a valid addon";
    }
    return "?";
}
