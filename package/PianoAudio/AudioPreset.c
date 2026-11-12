/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The amplifier preset on the ESP: see AudioPreset.h. */
#include "AudioPreset.h"
#include "AudioAmps.h"

int audio_preset_load(FatVolume *v, const char *path, uint8_t *buf, uint32_t cap, uint32_t *size)
{
    FatFile f;
    *size = 0;
    const int rc = fat_open(v, path, &f);
    if (rc == FatNotFound || rc == FatNotDirectory || rc == FatIsDirectory || (!rc && f.directory)) return AP_NOT_FOUND;
    if (rc) return AP_READ;
    if (!f.size) return AP_EMPTY;
    if (f.size > cap || f.size > AUDIO_PRESET_MAX_BYTES) return AP_TOO_BIG;
    if (fat_read(v, &f, 0, buf, f.size)) return AP_READ;
    *size = f.size;
    return AP_OK;
}

const char *audio_preset_load_error(int error)
{
    switch (error) {
    case AP_OK: return "ok";
    case AP_NOT_FOUND: return "no " AUDIO_PRESET_PATH " on the ESP either (stage the stock file for the pin, or build-esp.py --audio-preset)";
    case AP_EMPTY: return "the file on the ESP is empty";
    case AP_TOO_BIG: return "the file on the ESP is larger than 64 KiB";
    case AP_READ: return "the ESP could not be read";
    default: return "?";
    }
}
