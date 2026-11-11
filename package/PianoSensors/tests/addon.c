// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// SensorsAddon.c and SensorsLoad.c's sl_check on the addon a RAM test image carries (tools/sensor_addon.py, the same as
// build-ram-images.py's PIANO_SENSOR_PACK writes), inflated with zlib as the kext does with libkern's: the pack comes out byte for
// byte and checks; then the container's refusals (every field of the header and the entry, a second entry before ours, nothing
// after the last entry):
//   addon ADDON PACK
#define _DARWIN_C_SOURCE
#include <zlib.h>
#include "check.h"
#include "../SensorsAddon.h"
#include "../SensorsLoad.h"

static uint8_t *slurp(const char *path, uint64_t *length)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    const long n = ftell(f);
    CHECK(n > 0 && !fseek(f, 0, SEEK_SET));
    uint8_t *data = malloc((size_t)n);
    CHECK(data && fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *length = (uint64_t)n;
    return data;
}

static void wr32(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));}
static void wr64(uint8_t *p, uint64_t v) {wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));}

// The kext's inflate (PianoSensors.cpp inflateEntry), here with the host's zlib: exactly size bytes and the stream's end.
static int inflate_entry(const SaEntry *e, uint8_t *out)
{
    z_stream z;
    memset(&z, 0, sizeof z);
    z.next_in = (Bytef *)(uintptr_t)e->data;
    z.avail_in = (uInt)e->compressed;
    z.next_out = out;
    z.avail_out = (uInt)e->size;
    if (inflateInit(&z) != Z_OK) return -1;
    const int rc = inflate(&z, Z_FINISH);
    const int ok = rc == Z_STREAM_END && z.total_out == e->size;
    inflateEnd(&z);
    return ok ? 0 : -1;
}

static uint32_t put_entry(uint8_t *at, const char *name, const uint8_t *data, uint32_t size)
{
    const uint32_t name_bytes = (uint32_t)strlen(name) + 1, name_padded = (name_bytes + 7) & ~7u;
    uLongf compressed = compressBound(size);
    CHECK(compress2(at + SA_ENTRY + name_padded, &compressed, data, size, 6) == Z_OK);
    wr32(at, name_bytes);
    wr32(at + 4, 0644);
    wr64(at + 8, size);
    wr64(at + 16, compressed);
    memset(at + SA_ENTRY, 0, name_padded);
    memcpy(at + SA_ENTRY, name, name_bytes);
    const uint32_t stream_padded = ((uint32_t)compressed + 7) & ~7u;
    memset(at + SA_ENTRY + name_padded + compressed, 0, stream_padded - compressed);
    return SA_ENTRY + name_padded + stream_padded;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    uint64_t length = 0, pack_length = 0;
    uint8_t *blob = slurp(argv[1], &length), *pack = slurp(argv[2], &pack_length);
    SaEntry e;
    CHECK(sa_find(blob, length, SA_PACK_PATH, &e) == SA_OK && e.size == pack_length && e.mode == 0600 && !strcmp(e.path, SA_PACK_PATH));
    uint8_t *out = malloc(e.size);
    CHECK(out && !inflate_entry(&e, out) && !memcmp(out, pack, pack_length));
    SspPack table;
    CHECK(sl_check(out, (uint32_t)e.size, &table) == SL_OK && table.count > 0 && sl_contents(&table) & SL_HAS_REG_CONFIG);
    CHECK(sl_check(out, 16, &table) == SL_INVALID && sl_check(out, SL_MAX_BYTES + 1, &table) == SL_TOO_BIG);
    out[40] ^= 0xff;                                                      // a damaged table: refused after the inflate
    CHECK(sl_check(out, (uint32_t)e.size, &table) == SL_INVALID);
    CHECK(sa_find(blob, length, "sensors/other.pack", &e) == SA_NOT_FOUND);
    // the container's refusals, each on a copy of the valid one
    uint8_t *bad = malloc(length + 64);
    CHECK(bad);
    static const struct {uint32_t at; uint8_t value;} flips[] = {
        {0, 'X'},          // magic
        {12, 1},           // the reserved word
        {16, 0x01},        // total bytes != the length
        {8, 0xff},         // count beyond what is there
        {24, 0xff},        // path bytes beyond 160
        {24, 1},           // path bytes 1: no room for a name
    };
    for (unsigned k = 0; k < sizeof flips / sizeof flips[0]; ++k) {
        memcpy(bad, blob, length);
        bad[flips[k].at] = flips[k].value;
        CHECK(sa_find(bad, length, SA_PACK_PATH, &e) == SA_INVALID);
    }
    memcpy(bad, blob, length);
    bad[SA_HEADER + SA_ENTRY + strlen(SA_PACK_PATH)] = 'x';                // the path's NUL gone
    CHECK(sa_find(bad, length, SA_PACK_PATH, &e) == SA_INVALID);
    memcpy(bad, blob, length);
    bad[SA_HEADER + SA_ENTRY + 3] = 0;                                     // a NUL inside the path
    CHECK(sa_find(bad, length, SA_PACK_PATH, &e) == SA_INVALID);
    memcpy(bad, blob, length);
    wr64(bad + SA_HEADER + 16, length);                                    // the stream beyond the end
    CHECK(sa_find(bad, length, SA_PACK_PATH, &e) == SA_INVALID);
    memcpy(bad, blob, length);                                             // bytes after the last entry
    memset(bad + length, 0, 8);
    wr64(bad + 16, length + 8);
    CHECK(sa_find(bad, length + 8, SA_PACK_PATH, &e) == SA_INVALID);
    CHECK(sa_find(blob, 16, SA_PACK_PATH, &e) == SA_INVALID && sa_find(NULL, length, SA_PACK_PATH, &e) == SA_INVALID);
    // two entries (another file first, as a GPU addon's would be): ours is found after it, the other ignored
    static uint8_t two[(8u << 20)];
    memcpy(two, SA_MAGIC, 8);
    wr32(two + 8, 2);
    wr32(two + 12, 0);
    static const uint8_t other[] = "a file of someone else's";
    uint32_t at = SA_HEADER;
    at += put_entry(two + at, "addons/Other.bundle/Info.plist", other, sizeof other);
    at += put_entry(two + at, SA_PACK_PATH, pack, (uint32_t)pack_length);
    wr64(two + 16, at);
    CHECK(sa_find(two, at, SA_PACK_PATH, &e) == SA_OK && e.mode == 0644 && e.size == pack_length);
    memset(out, 0, pack_length);
    CHECK(!inflate_entry(&e, out) && !memcmp(out, pack, pack_length));
    e.compressed -= 1;                                                     // a cut stream is refused by the inflate
    CHECK(inflate_entry(&e, out));
    for (int k = 0; k <= SA_INVALID; ++k) CHECK(strcmp(sa_error_name(k), "?"));
    free(bad);
    free(out);
    free(blob);
    free(pack);
    printf("PASS addon: the RAM image's carrier from sensor_addon.py, inflated and checked; header, entry and trailing refusals; "
           "a second entry skipped: %u checks\n", checks);
    return 0;
}
