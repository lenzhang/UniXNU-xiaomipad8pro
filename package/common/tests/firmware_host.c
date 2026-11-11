// SPDX-License-Identifier: BSD-2-Clause-Patent
// The firmware loader's portable part (PianoFirmwareCore.c, PianoFirmwareZlib.c over package/PianoPower/FatVolume.c) against
// what tooling/persist/firmware.py makes: an ESP-like FAT32 image (4096-byte sectors, one per cluster, as build-esp.py formats it)
// and an NGPUADD2 addon (as build-ram-images.py puts in a RAM image), with the pins `firmware.py stage` wrote (pins.h). Synthetic
// files only.
//   firmware_host IMAGE ADDON
#define _DARWIN_C_SOURCE
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "PianoFirmwareCore.h"
#include "pins.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static FILE *image;
static long live_blocks, live_bytes;

static int image_read(void *context, uint64_t offset, void *to, size_t length)
{
    (void)context;
    if (fseeko(image, (off_t)offset, SEEK_SET)) return -1;
    return fread(to, 1, length, image) == length ? 0 : -1;
}

static void *op_alloc(void *context, size_t bytes)
{
    (void)context;
    void *p = malloc(bytes);
    if (p) {++live_blocks; live_bytes += (long)bytes;}
    return p;
}

static void op_release(void *context, void *data, size_t bytes)
{
    (void)context;
    --live_blocks;
    live_bytes -= (long)bytes;
    free(data);
}

static void op_sha256(void *context, const void *data, size_t bytes, uint8_t digest[32])
{
    (void)context;
    CC_SHA256(data, (CC_LONG)bytes, digest);
}

static uint8_t *slurp(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    struct stat st;
    if (!f || stat(path, &st)) return NULL;
    uint8_t *data = malloc((size_t)st.st_size);
    *bytes = fread(data, 1, (size_t)st.st_size, f);
    fclose(f);
    return data;
}

static void put32(uint8_t *p, uint32_t v) {for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> 8 * i);}
static void put64(uint8_t *p, uint64_t v) {for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> 8 * i);}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    if (argc != 3) return 1;
    PianoFwOps ops = {NULL, op_alloc, op_release, op_sha256, pfw_zlib_inflate};
    ops.context = &ops;

    // pins: the generated ones, then malformed ones
    CHECK(sizeof(PianoFirmwarePin) == 136);
    CHECK(pfw_pin_ok(&piano_fw_pianotest_good_pin) && pfw_pin_ok(&piano_fw_pianotest_big_pin));
    CHECK(piano_fw_pianotest_good_pin.flags == 0 && piano_fw_pianotest_unit_pin.flags == PIANO_FW_PRIVATE);
    char full[128];
    CHECK(pfw_full_path(&piano_fw_pianotest_good_pin, full, sizeof full) == strlen("EFI/UniXNU/firmware/PianoTest/good.bin") &&
          !strcmp(full, "EFI/UniXNU/firmware/PianoTest/good.bin"));
    CHECK(pfw_full_path(&piano_fw_pianotest_good_pin, full, 10) == 0);
    static const char *bad_paths[] = {"", "/x", "x/", "a//b", "../x", "a/../b", "./x", "a/.", "...", "a b", "a\\b", "x\x80"};
    for (unsigned i = 0; i < sizeof bad_paths / sizeof bad_paths[0]; ++i) {
        PianoFirmwarePin p = piano_fw_pianotest_good_pin;
        memset(p.path, 0, sizeof p.path);
        strcpy(p.path, bad_paths[i]);
        CHECK(!pfw_pin_ok(&p));
    }
    {
        PianoFirmwarePin p = piano_fw_pianotest_good_pin;
        memset(p.path, 'a', sizeof p.path);   // no NUL
        CHECK(!pfw_pin_ok(&p));
        p = piano_fw_pianotest_good_pin;
        p.magic[7] = '2';
        CHECK(!pfw_pin_ok(&p));
        p = piano_fw_pianotest_good_pin;
        p.size = 0;
        CHECK(!pfw_pin_ok(&p));
        p.size = PIANO_FW_MAX_BYTES + 1;
        CHECK(!pfw_pin_ok(&p));
        p = piano_fw_pianotest_good_pin;
        strcpy(p.path, "A-z_0.9/sub.dir/x-y.bin");
        CHECK(pfw_pin_ok(&p));
    }

    // the ESP: a FAT32 volume with 4096-byte sectors
    image = fopen(argv[1], "rb");
    CHECK(image != NULL);
    struct stat st;
    CHECK(!stat(argv[1], &st));
    static FatVolume v;
    CHECK(!fat_mount(&v, (uint64_t)st.st_size, image_read, NULL) && v.fat32 && v.sector_bytes == 4096);
    uint8_t *data = NULL;
    CHECK(pfw_from_fat(&v, &piano_fw_pianotest_good_pin, &ops, &data) == PIANO_FW_OK && data);
    CHECK(data && !memcmp(data, piano_fw_pianotest_good, piano_fw_pianotest_good_pin.size));   // the embedded copy: same bytes
    if (data) op_release(NULL, data, piano_fw_pianotest_good_pin.size);
    CHECK(pfw_from_fat(&v, &piano_fw_pianotest_big_pin, &ops, &data) == PIANO_FW_OK && data);
    if (data) op_release(NULL, data, piano_fw_pianotest_big_pin.size);
    CHECK(pfw_from_fat(&v, &piano_fw_pianotest_unit_pin, &ops, &data) == PIANO_FW_OK && data);
    if (data) op_release(NULL, data, piano_fw_pianotest_unit_pin.size);
    PianoFirmwarePin p = piano_fw_pianotest_good_pin;
    strcpy(p.path, "PianoTest/missing.bin");
    CHECK(pfw_from_fat(&v, &p, &ops, &data) == PIANO_FW_NOT_FOUND && !data);
    strcpy(p.path, "PianoTest/dir");
    CHECK(pfw_from_fat(&v, &p, &ops, &data) == PIANO_FW_NOT_FOUND && !data);
    strcpy(p.path, "PianoTest/good.bin/x");
    CHECK(pfw_from_fat(&v, &p, &ops, &data) == PIANO_FW_NOT_FOUND && !data);
    strcpy(p.path, "PianoTest/short.bin");   // the pin's size, another file's
    CHECK(pfw_from_fat(&v, &p, &ops, &data) == PIANO_FW_SIZE && !data);
    strcpy(p.path, "PianoTest/flipped.bin");   // the pin's size, one byte changed
    CHECK(pfw_from_fat(&v, &p, &ops, &data) == PIANO_FW_HASH && !data);
    strcpy(p.path, "piantest/GOOD.BIN");   // not the pin's path (a FAT match is case blind, the directory is not this one)
    CHECK(pfw_from_fat(&v, &p, &ops, &data) == PIANO_FW_NOT_FOUND);
    strcpy(p.path, "PIANOTEST/GOOD.BIN");   // FAT: any case
    CHECK(pfw_from_fat(&v, &p, &ops, &data) == PIANO_FW_OK && data);
    if (data) op_release(NULL, data, p.size);
    CHECK(live_blocks == 0 && live_bytes == 0);

    // the embedded path: the same check over the bytes in the kext
    CHECK(pfw_check(&piano_fw_pianotest_good_pin, piano_fw_pianotest_good, piano_fw_pianotest_good_size, &ops) == PIANO_FW_OK);
    CHECK(pfw_check(&piano_fw_pianotest_good_pin, piano_fw_pianotest_good, piano_fw_pianotest_good_size - 1, &ops) == PIANO_FW_SIZE);
    CHECK(pfw_check(&piano_fw_pianotest_big_pin, piano_fw_pianotest_good, piano_fw_pianotest_big_pin.size, &ops) != PIANO_FW_OK);

    // the addon
    size_t bytes = 0;
    uint8_t *addon = slurp(argv[2], &bytes);
    CHECK(addon && bytes > PFW_ADDON_HEADER);
    PfwAddonEntry e;
    CHECK(pfw_addon_find(addon, bytes, "EFI/UniXNU/firmware/PianoTest/good.bin", &e) == PIANO_FW_OK && e.size == piano_fw_pianotest_good_pin.size);
    CHECK(pfw_addon_find(addon, bytes, "EFI/UniXNU/firmware/PianoTest/good.bi", &e) == PIANO_FW_NOT_FOUND);
    CHECK(pfw_addon_find(addon, bytes, "EFI/UniXNU/firmware/PianoTest/good.bin2", &e) == PIANO_FW_NOT_FOUND);
    CHECK(pfw_from_addon(addon, bytes, &piano_fw_pianotest_good_pin, &ops, &data) == PIANO_FW_OK && data &&
          !memcmp(data, piano_fw_pianotest_good, piano_fw_pianotest_good_pin.size));
    if (data) op_release(NULL, data, piano_fw_pianotest_good_pin.size);
    CHECK(pfw_from_addon(addon, bytes, &piano_fw_pianotest_big_pin, &ops, &data) == PIANO_FW_OK && data);
    if (data) op_release(NULL, data, piano_fw_pianotest_big_pin.size);
    CHECK(pfw_from_addon(addon, bytes, &piano_fw_pianotest_unit_pin, &ops, &data) == PIANO_FW_OK && data);
    if (data) op_release(NULL, data, piano_fw_pianotest_unit_pin.size);
    p = piano_fw_pianotest_good_pin;
    strcpy(p.path, "PianoTest/missing.bin");
    CHECK(pfw_from_addon(addon, bytes, &p, &ops, &data) == PIANO_FW_NOT_FOUND && !data);
    p = piano_fw_pianotest_good_pin;
    p.size += 1;
    CHECK(pfw_from_addon(addon, bytes, &p, &ops, &data) == PIANO_FW_SIZE && !data);
    p = piano_fw_pianotest_good_pin;
    p.sha256[0] ^= 1;
    CHECK(pfw_from_addon(addon, bytes, &p, &ops, &data) == PIANO_FW_HASH && !data);
    CHECK(live_blocks == 0 && live_bytes == 0);   // zlib's own blocks too

    // damaged containers: every one refused, nothing returned, nothing leaked
    uint8_t *copy = malloc(bytes + 64);
    const char *good = "EFI/UniXNU/firmware/PianoTest/good.bin";
    CHECK(pfw_addon_find(addon, bytes - 8, good, &e) == PIANO_FW_DAMAGED);   // total says more
    CHECK(pfw_addon_find(addon, 23, good, &e) == PIANO_FW_DAMAGED);
    CHECK(pfw_addon_find(NULL, bytes, good, &e) == PIANO_FW_DAMAGED);
    memcpy(copy, addon, bytes); copy[0] = 'X';
    CHECK(pfw_addon_find(copy, bytes, good, &e) == PIANO_FW_DAMAGED);
    memcpy(copy, addon, bytes); put32(copy + 12, 1);
    CHECK(pfw_addon_find(copy, bytes, good, &e) == PIANO_FW_DAMAGED);
    memcpy(copy, addon, bytes); put32(copy + 8, 5000);
    CHECK(pfw_addon_find(copy, bytes, good, &e) == PIANO_FW_DAMAGED);
    memcpy(copy, addon, bytes); put32(copy + 8, 1);   // fewer entries than the total covers
    CHECK(pfw_addon_find(copy, bytes, good, &e) == PIANO_FW_DAMAGED);
    memcpy(copy, addon, bytes); put32(copy + 24, 1);   // a path of one byte
    CHECK(pfw_addon_find(copy, bytes, good, &e) == PIANO_FW_DAMAGED);
    memcpy(copy, addon, bytes); put32(copy + 24, 161);
    CHECK(pfw_addon_find(copy, bytes, good, &e) == PIANO_FW_DAMAGED);
    memcpy(copy, addon, bytes); put64(copy + 24 + 16, bytes);   // a stream past the end
    CHECK(pfw_addon_find(copy, bytes, good, &e) == PIANO_FW_DAMAGED);
    memcpy(copy, addon, bytes); put64(copy + 24 + 16, ~0ull);
    CHECK(pfw_addon_find(copy, bytes, good, &e) == PIANO_FW_DAMAGED);
    {
        const uint32_t name_bytes = (uint32_t)copy[24] | (uint32_t)copy[25] << 8;
        memcpy(copy, addon, bytes); copy[24 + 24 + name_bytes - 1] = 'x';   // the first path loses its NUL
        CHECK(pfw_addon_find(copy, bytes, good, &e) == PIANO_FW_DAMAGED);
        memcpy(copy, addon, bytes); copy[24 + 24 + 3] = 0;   // a NUL inside it
        CHECK(pfw_addon_find(copy, bytes, good, &e) == PIANO_FW_DAMAGED);
    }
    // a zlib stream that does not inflate, or inflates to another length than recorded: a read error, not data
    CHECK(pfw_addon_find(addon, bytes, good, &e) == PIANO_FW_OK);
    const size_t stream_at = (size_t)(e.packed - addon);
    memcpy(copy, addon, bytes); copy[stream_at + e.packed_bytes / 2] ^= 0x55;
    CHECK(pfw_from_addon(copy, bytes, &piano_fw_pianotest_good_pin, &ops, &data) == PIANO_FW_READ && !data);
    memcpy(copy, addon, bytes); copy[stream_at] = 0;   // not a zlib header
    CHECK(pfw_from_addon(copy, bytes, &piano_fw_pianotest_good_pin, &ops, &data) == PIANO_FW_READ && !data);
    uint8_t out[8];
    CHECK(pfw_zlib_inflate(&ops, out, sizeof out, e.packed, (size_t)e.packed_bytes) != 0);   // more than 8 bytes inside
    CHECK(pfw_zlib_inflate(&ops, out, 0, e.packed, (size_t)e.packed_bytes) != 0);
    CHECK(live_blocks == 0 && live_bytes == 0);
    for (int r = 0; r <= PIANO_FW_DAMAGED; ++r) CHECK(strcmp(piano_fw_error(r), "?"));
    CHECK(!strcmp(piano_fw_error(99), "?") && !strcmp(piano_fw_source(PIANO_FW_FROM_ESP), "the ESP"));
    free(copy);
    free(addon);
    fclose(image);
    if (failures) {
        printf("FAIL firmware loader: %u of %u checks\n", failures, checks);
        return 1;
    }
    printf("PASS firmware loader: pins, the ESP's FAT32 (4096-byte sectors), the embedded check, the NGPUADD2 addon and its damaged "
           "forms, zlib with the loader's allocator, no leak: %u checks\n", checks);
    return 0;
}
