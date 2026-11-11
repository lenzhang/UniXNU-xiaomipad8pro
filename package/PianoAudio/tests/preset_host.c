// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of level 5's preset source (AudioPreset.c over FatVolume.c, AudioAmps.c's verdict) on a FAT32 image made by mtools the
// way build-esp.py makes the ESP (4096-byte sectors, one per cluster), holding a synthetic container (synthetic_fsm.h: made-up data)
// at EFI/UniXNU/piano-fs19xx.fsm, an empty file and one too big:
//   preset_host --write DIR     writes those three files into DIR (tooling/k27/build-native-piano-audio.sh copies them in)
//   preset_host IMAGE           the loader and the verdict on the image
//   preset_host                 the verdict alone (no mtools)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioAmps.h"
#include "../AudioPreset.h"
#include "../AudioSha256.h"
#include "synthetic_fsm.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static uint8_t fsm[4096];
static unsigned fsm_bytes;

static int write_file(const char *dir, const char *name, const void *data, size_t n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const size_t w = n ? fwrite(data, 1, n, f) : 0;
    return fclose(f) || w != n ? -1 : 0;
}

static FILE *image;
static int image_read(void *context, uint64_t offset, void *to, size_t length)
{
    (void)context;
    return fseeko(image, (off_t)offset, SEEK_SET) || fread(to, 1, length, image) != length ? -1 : 0;
}

static void testVerdict(void)
{
    int fe = -99, stock = -1;
    CHECK(audio_preset_check(0, 0, 1, &fe, &stock) == AudioPresetMissing);
    CHECK(audio_preset_check(fsm, AUDIO_PRESET_MAX_BYTES + 1, 1, &fe, &stock) == AudioPresetTooBig);
    CHECK(audio_preset_check(fsm, fsm_bytes, 0, &fe, &stock) == AudioPresetNotStock && fe == FsmOk && stock == 0);
    CHECK(audio_preset_check(fsm, fsm_bytes, 1, &fe, &stock) == AudioPresetOk && stock == 0);
    uint8_t bad[4096];
    memcpy(bad, fsm, sizeof bad);
    bad[60] ^= 1;
    CHECK(audio_preset_check(bad, fsm_bytes, 1, &fe, &stock) == AudioPresetBadContainer && fe == FsmBadCrc);
    CHECK(audio_preset_check(fsm, fsm_bytes - 1, 1, &fe, &stock) == AudioPresetBadContainer && fe == FsmBadSize);
    // the stock file's identity: its size and digest are pinned; a container of the same size with other bytes is not it
    CHECK(AUDIO_PRESET_BYTES == 4348u && !audio_preset_is_stock(fsm, fsm_bytes));
    static uint8_t same_size[4348];
    CHECK(!audio_preset_is_stock(same_size, sizeof same_size));
    CHECK(strstr(audio_preset_error(AudioPresetNotStock), "piano-audio-preset-any=1") && !strcmp(audio_preset_error(42), "?"));
}

static void testImage(const char *path)
{
    image = fopen(path, "rb");
    CHECK(image);
    CHECK(fseeko(image, 0, SEEK_END) == 0);
    const uint64_t size = (uint64_t)ftello(image);
    static FatVolume v;
    CHECK(fat_mount(&v, size, image_read, 0) == FatOk && v.fat32 && v.sector_bytes == 4096);
    static uint8_t buf[AUDIO_PRESET_MAX_BYTES];
    uint32_t n = 0;
    CHECK(audio_preset_load(&v, AUDIO_PRESET_PATH, buf, sizeof buf, &n) == AP_OK && n == fsm_bytes && !memcmp(buf, fsm, n));
    int fe, stock;
    CHECK(audio_preset_check(buf, n, 0, &fe, &stock) == AudioPresetNotStock && audio_preset_check(buf, n, 1, &fe, &stock) == AudioPresetOk);
    CHECK(audio_preset_load(&v, "efi/unixnu/PIANO-FS19XX.FSM", buf, sizeof buf, &n) == AP_OK && n == fsm_bytes);   // any case
    CHECK(audio_preset_load(&v, "EFI/UniXNU/none.fsm", buf, sizeof buf, &n) == AP_NOT_FOUND && n == 0);
    CHECK(audio_preset_load(&v, "EFI/UniXNU", buf, sizeof buf, &n) == AP_NOT_FOUND);                // a directory
    CHECK(audio_preset_load(&v, "EFI/UniXNU/empty.fsm", buf, sizeof buf, &n) == AP_EMPTY);
    CHECK(audio_preset_load(&v, "EFI/UniXNU/big.fsm", buf, sizeof buf, &n) == AP_TOO_BIG);
    CHECK(audio_preset_load(&v, AUDIO_PRESET_PATH, buf, 100, &n) == AP_TOO_BIG);                    // the caller's buffer
    CHECK(strstr(audio_preset_load_error(AP_NOT_FOUND), "--audio-preset"));
    fclose(image);
}

int main(int argc, char **argv)
{
    fsm_bytes = sfsm_build(fsm, sizeof fsm, 0);
    CHECK(fsm_bytes > 100);
    if (argc == 3 && !strcmp(argv[1], "--write")) {
        static uint8_t big[AUDIO_PRESET_MAX_BYTES + 4096];
        CHECK(!write_file(argv[2], "piano-fs19xx.fsm", fsm, fsm_bytes) && !write_file(argv[2], "empty.fsm", "", 0) &&
              !write_file(argv[2], "big.fsm", big, sizeof big));
        return 0;
    }
    testVerdict();
    if (argc == 2) testImage(argv[1]);
    printf("PASS preset: %u checks%s\n", checks, argc == 2 ? "" : " (no FAT image: the loader not run)");
    return 0;
}
