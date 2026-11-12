/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The null PCM transport (AudioPcm.h): silence both ways, counted. */
#include "AudioPcm.h"

static PianoPcmNullStats gNull;

static void null_running(void *context, int output, int input)
{
    (void)context;
    if (output && !gNull.output) gNull.output_starts++;
    if (input && !gNull.input) gNull.input_starts++;
    gNull.output = output ? 1 : 0;
    gNull.input = input ? 1 : 0;
}

static uint32_t null_play(void *context, const int16_t *stereo, uint32_t frames)
{
    (void)context; (void)stereo;
    gNull.played += frames;
    return frames;
}

static uint32_t null_capture(void *context, int16_t *mono, uint32_t frames)
{
    (void)context; (void)mono; (void)frames;
    return 0;
}

static const PianoPcmTransport kNull = {"null", 0, 0, 0, null_running, null_play, null_capture, 0};

const PianoPcmTransport *piano_pcm_null(void) {return &kNull;}

void piano_pcm_null_stats(PianoPcmNullStats *out) {*out = gNull;}
