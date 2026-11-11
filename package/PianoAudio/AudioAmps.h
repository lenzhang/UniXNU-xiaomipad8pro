/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Level 5: piano's four FS19xx amplifiers as one group, on Fs19xx.h's core: out of reset, probed, the vendor preset checked and each
 * amplifier configured from its block, powered down and muted; their status; and for the test tone (level 7) the start at a test gain,
 * the unmute, the group check and the way back: muted, powered down, held in reset.
 *
 * The preset (foursemi/piano/fs19xx.fsm, odm_a) is data read at run time from the owner's own copy on the ESP (AudioPreset.h): never
 * committed, never disassembled. It is taken only when it is the stock file Ubuntu runs (size and SHA-256, AudioSha256.h), unless the
 * owner says otherwise; whatever it holds, Fs19xx.c's filter keeps every amplifier powered down, muted and capped while it is applied.
 *
 * The rules of this file: all four or none (a missing or failing amplifier shuts the others down and holds all four in reset); no
 * amplifier is powered up but by audio_amps_start, which caps the gain at FS19XX_GAIN_TEST (160, -35.6 dB: the unattended-test gain)
 * and leaves them muted; at the end they are held in reset (hardware shutdown), as Ubuntu leaves them unbound.
 * Portable C, new code for PianoAudio: the bus, the reset lines and the sleeps are the owner's. */
#ifndef PIANO_AUDIO_AMPS_H
#define PIANO_AUDIO_AMPS_H
#include <stdint.h>
#include "Fs19xx.h"
#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_AMPS 4u
/* In TDM slot order: top left, top right, bottom left, bottom right (landscape, camera up): AudioHw.h kAmps, the DT's codec order. */
#define AUDIO_AMP_ADDRESSES {0x35u, 0x37u, 0x34u, 0x36u}
#define AUDIO_AMPS_TEST_GAIN FS19XX_GAIN_TEST
#define AUDIO_PRESET_MAX_BYTES 65536u     /* the stock file is 4348 bytes */

enum {AudioPresetOk = 0, AudioPresetMissing = 1, AudioPresetTooBig = 2, AudioPresetNotStock = 3, AudioPresetBadContainer = 4};
enum {AudioAmpsOk = 0, AudioAmpsErrMissing = -1, AudioAmpsErrPreset = -2, AudioAmpsErrBlock = -3, AudioAmpsErrConfigure = -4,
      AudioAmpsErrState = -5, AudioAmpsErrStart = -6, AudioAmpsErrBus = -7};

typedef struct {
    void *owner;
    const Fs19xxBus *bus;
    /* The reset line of the amplifier in this slot (active low): release 1 drives it high (out of shutdown), 0 low (held in
     * shutdown). 0, or an error. */
    int (*reset)(void *owner, unsigned slot, int release);
    /* A burst of bus traffic is over (the owner may give the pins back); may be null. */
    void (*done)(void *owner);
} AudioAmpsIo;

typedef struct {
    AudioAmpsIo io;
    Fs19xx amp[AUDIO_AMPS];
    int probe[AUDIO_AMPS], find[AUDIO_AMPS], config[AUDIO_AMPS];   /* each amplifier's last result (Fs19xx*, Fsm*) */
    unsigned present, configured, started;                           /* bit per slot */
    int released;                                                    /* the reset lines were driven high */
    int preset, preset_fsm, stock;                                   /* the preset's verdict (AudioPreset*), fsm_check's, stock 1/0 */
    uint32_t preset_bytes;
    Fs19xxStatus status[AUDIO_AMPS], first[AUDIO_AMPS];
    int have_first;
    unsigned reset_errors, off_errors;
} AudioAmps;

void audio_amps_init(AudioAmps *g, const AudioAmpsIo *io);
const char *audio_amp_name(unsigned slot);
uint8_t audio_amp_address(unsigned slot);

/* The preset as read: AudioPresetOk, or why it is refused. any: a valid container that is not the stock file is taken. */
int audio_preset_check(const uint8_t *data, uint32_t size, int any, int *fsm_error, int *stock);
const char *audio_preset_error(int error);
const char *audio_amps_error(int error);

/* The reset lines held low 10 ms, then released; 5 ms later each amplifier probed (DEVID polled for up to 100 ms). Returns the slots
 * that answered as an FS19xx (bit per slot). */
unsigned audio_amps_probe(AudioAmps *g);
/* The preset checked, then each amplifier's block found and applied (soft reset, coefficient, common table, music scene, shut down).
 * AudioAmpsOk when all four are configured; otherwise all four are held in reset again and the first error is returned. */
int audio_amps_configure(AudioAmps *g, const uint8_t *preset, uint32_t size, int any);
/* The status of the amplifiers that answered (into g->status; the first reading after an unmute also into g->first). Returns the
 * slots whose reads failed. */
unsigned audio_amps_read(AudioAmps *g);
/* After audio_amps_read: every configured amplifier powered down (PWDN set). */
int audio_amps_all_down(const AudioAmps *g);
/* Power all four configured amplifiers up, muted, at min(gain, AUDIO_AMPS_TEST_GAIN). 0; or all of them shut down again. */
int audio_amps_start(AudioAmps *g, unsigned gain);
/* Unmute the started amplifiers; on a bus error all are muted and powered down again. */
int audio_amps_unmute(AudioAmps *g);
/* After audio_amps_read while playing: the slots that look wrong (fs19xx_group_check against the first reading after the unmute). */
unsigned audio_amps_check(AudioAmps *g);
/* Mute all, then power each down (every write tried); returns the amplifiers with a bus error. */
unsigned audio_amps_off(AudioAmps *g);
/* audio_amps_off, then the reset lines low. */
void audio_amps_hold(AudioAmps *g);

#ifdef __cplusplus
}
#endif
#endif
