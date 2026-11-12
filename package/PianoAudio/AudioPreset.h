/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Where level 5 finds the amplifier preset, the owner's own copy of the stock foursemi/piano/fs19xx.fsm: first through the shared
 * firmware loader (<PianoFirmware.h>) against the kext's pin PianoAudio/fs19xx.fsm, from a RAM image's addon or the ESP's
 * EFI/UniXNU/firmware/PianoAudio/fs19xx.fsm (AudioBoard.cpp presetPinned); then, as here, the copy tooling/persist/build-esp.py
 * --audio-preset FILE stages on the ESP (GPT partition "esp", FAT32 with 4096-byte sectors) as EFI/UniXNU/piano-fs19xx.fsm, read
 * whole through FatVolume.h (the reader PianoPower's firmware and PianoSensors' pack use); only this second path can give a valid
 * preset that is not the stock file (piano-audio-preset-any=1). The ESP is only read.
 *
 * Why the ESP and not odm_a itself: odm_a is a logical partition inside "super" (LP metadata) holding an Android EROFS image (its
 * files may be LZ4-compressed): a reader for that is a new filesystem in the kernel for one 4348-byte file. The staged copy is the
 * same bytes, made on the owner's Mac from the owner's own files, checked there and here against the stock size and SHA-256
 * (AudioSha256.h), never committed and never in a published image. An odm_a reader can replace it later for video, audio and sensors.
 * Portable C, new code for PianoAudio; host-tested on an mtools FAT32 image (tests/preset_host.c). */
#ifndef PIANO_AUDIO_PRESET_H
#define PIANO_AUDIO_PRESET_H
#include <stdint.h>
#include "FatVolume.h"
#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PRESET_PARTITION "esp"
#define AUDIO_PRESET_PATH "EFI/UniXNU/piano-fs19xx.fsm"   /* a long name: FAT keeps its case */
enum {AP_OK = 0, AP_NOT_FOUND = 1, AP_EMPTY = 2, AP_TOO_BIG = 3, AP_READ = 4};

/* Reads the file at path of the mounted volume into buf (capacity cap, at most AUDIO_PRESET_MAX_BYTES): AP_OK and *size. */
int audio_preset_load(FatVolume *v, const char *path, uint8_t *buf, uint32_t cap, uint32_t *size);
const char *audio_preset_load_error(int error);

#ifdef __cplusplus
}
#endif
#endif
