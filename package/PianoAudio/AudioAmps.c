/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The four amplifiers as a group: see AudioAmps.h. */
#include "AudioAmps.h"
#include "AudioSha256.h"

static const uint8_t kAddress[AUDIO_AMPS] = AUDIO_AMP_ADDRESSES;
static const char *const kName[AUDIO_AMPS] = {"Top Left", "Top Right", "Bottom Left", "Bottom Right"};

const char *audio_amp_name(unsigned slot) {return slot < AUDIO_AMPS ? kName[slot] : "?";}
uint8_t audio_amp_address(unsigned slot) {return slot < AUDIO_AMPS ? kAddress[slot] : 0;}

static void sleep_ms(AudioAmps *g, uint32_t ms) {if (g->io.bus->sleep_us) g->io.bus->sleep_us(g->io.bus->context, ms * 1000u);}
static void done(AudioAmps *g) {if (g->io.done) g->io.done(g->io.owner);}
static int reset_line(AudioAmps *g, unsigned slot, int release)
{
    const int r = g->io.reset ? g->io.reset(g->io.owner, slot, release) : -1;
    if (r) g->reset_errors++;
    return r;
}

void audio_amps_init(AudioAmps *g, const AudioAmpsIo *io)
{
    for (unsigned i = 0; i < sizeof *g; ++i) ((uint8_t *)g)[i] = 0;
    g->io = *io;
    g->preset = AudioPresetMissing;
    for (unsigned s = 0; s < AUDIO_AMPS; ++s) {
        fs19xx_init_amp(&g->amp[s], g->io.bus, kAddress[s]);
        g->probe[s] = g->find[s] = g->config[s] = 1;   /* not tried */
    }
}

int audio_preset_check(const uint8_t *data, uint32_t size, int any, int *fsm_error, int *stock)
{
    int st = 0, fe = FsmOk;
    int r = AudioPresetOk;
    if (!data || !size) r = AudioPresetMissing;
    else if (size > AUDIO_PRESET_MAX_BYTES) r = AudioPresetTooBig;
    else {
        st = audio_preset_is_stock(data, size);
        fe = fsm_check(data, size, 0);
        if (fe != FsmOk) r = AudioPresetBadContainer;
        else if (!st && !any) r = AudioPresetNotStock;
    }
    if (fsm_error) *fsm_error = fe;
    if (stock) *stock = st;
    return r;
}

const char *audio_preset_error(int error)
{
    switch (error) {
    case AudioPresetOk: return "ok";
    case AudioPresetMissing: return "no preset";
    case AudioPresetTooBig: return "larger than 64 KiB";
    case AudioPresetNotStock: return "not the stock fs19xx.fsm (size or SHA-256 differ; piano-audio-preset-any=1 takes it)";
    case AudioPresetBadContainer: return "not a valid fs19xx.fsm container";
    default: return "?";
    }
}

const char *audio_amps_error(int error)
{
    switch (error) {
    case AudioAmpsOk: return "ok";
    case AudioAmpsErrMissing: return "not all four amplifiers answered";
    case AudioAmpsErrPreset: return "the preset was refused";
    case AudioAmpsErrBlock: return "the preset has no usable block for an amplifier";
    case AudioAmpsErrConfigure: return "configuring an amplifier failed";
    case AudioAmpsErrState: return "the amplifiers are not in the state this needs";
    case AudioAmpsErrStart: return "starting an amplifier failed";
    case AudioAmpsErrBus: return "a bus error";
    default: return "?";
    }
}

unsigned audio_amps_probe(AudioAmps *g)
{
    for (unsigned s = 0; s < AUDIO_AMPS; ++s) reset_line(g, s, 0);
    sleep_ms(g, 10);
    for (unsigned s = 0; s < AUDIO_AMPS; ++s) reset_line(g, s, 1);
    g->released = 1;
    sleep_ms(g, 5);
    g->present = g->configured = g->started = 0;
    for (unsigned s = 0; s < AUDIO_AMPS; ++s) {
        fs19xx_init_amp(&g->amp[s], g->io.bus, kAddress[s]);
        g->probe[s] = fs19xx_probe(&g->amp[s]);
        if (!g->probe[s]) g->present |= 1u << s;
    }
    done(g);
    return g->present;
}

int audio_amps_configure(AudioAmps *g, const uint8_t *preset, uint32_t size, int any)
{
    g->preset_bytes = size;
    g->preset = audio_preset_check(preset, size, any, &g->preset_fsm, &g->stock);
    int r = AudioAmpsOk;
    if (g->present != (1u << AUDIO_AMPS) - 1u) r = AudioAmpsErrMissing;
    else if (g->preset != AudioPresetOk) r = AudioAmpsErrPreset;
    Fs19xxPreset blocks[AUDIO_AMPS];
    for (unsigned s = 0; !r && s < AUDIO_AMPS; ++s) {
        g->find[s] = fsm_find(preset, size, kAddress[s], g->amp[s].devid, &blocks[s]);
        if (g->find[s] != FsmOk) r = AudioAmpsErrBlock;
    }
    for (unsigned s = 0; !r && s < AUDIO_AMPS; ++s) {
        g->config[s] = fs19xx_configure(&g->amp[s], &blocks[s]);
        if (g->config[s]) r = AudioAmpsErrConfigure;
        else g->configured |= 1u << s;
    }
    done(g);
    if (r) audio_amps_hold(g);
    return r;
}

unsigned audio_amps_read(AudioAmps *g)
{
    unsigned failed = 0;
    for (unsigned s = 0; s < AUDIO_AMPS; ++s) {
        if (!(g->present >> s & 1u)) {g->status[s].ok = 0; continue;}
        if (fs19xx_read_status(&g->amp[s], &g->status[s])) failed |= 1u << s;
    }
    if (g->started && !g->have_first && !failed) {
        for (unsigned s = 0; s < AUDIO_AMPS; ++s) g->first[s] = g->status[s];
        g->have_first = 1;
    }
    done(g);
    return failed;
}

int audio_amps_all_down(const AudioAmps *g)
{
    if (!g->configured) return 0;
    for (unsigned s = 0; s < AUDIO_AMPS; ++s)
        if ((g->configured >> s & 1u) && !fs19xx_status_down(&g->status[s])) return 0;
    return 1;
}

int audio_amps_start(AudioAmps *g, unsigned gain)
{
    if (g->configured != (1u << AUDIO_AMPS) - 1u || g->started) return AudioAmpsErrState;
    if (gain > AUDIO_AMPS_TEST_GAIN) gain = AUDIO_AMPS_TEST_GAIN;
    g->have_first = 0;
    int r = AudioAmpsOk;
    for (unsigned s = 0; !r && s < AUDIO_AMPS; ++s) {
        if (fs19xx_start(&g->amp[s], 48000, 6144000, gain, 1)) r = AudioAmpsErrStart;
        else g->started |= 1u << s;
    }
    done(g);
    if (r) {audio_amps_off(g); return r;}
    return AudioAmpsOk;
}

int audio_amps_unmute(AudioAmps *g)
{
    if (g->started != (1u << AUDIO_AMPS) - 1u) return AudioAmpsErrState;
    int r = AudioAmpsOk;
    for (unsigned s = 0; !r && s < AUDIO_AMPS; ++s)
        if (fs19xx_mute(&g->amp[s], 0)) r = AudioAmpsErrBus;
    g->have_first = 0;   /* the first reading of the stream: after this */
    done(g);
    if (r) audio_amps_off(g);
    return r;
}

unsigned audio_amps_check(AudioAmps *g)
{
    if (!g->started) return 0;
    return fs19xx_group_check(g->status, g->have_first ? g->first : 0, AUDIO_AMPS);
}

unsigned audio_amps_off(AudioAmps *g)
{
    unsigned bad = 0;
    for (unsigned s = 0; s < AUDIO_AMPS; ++s)   /* every DAC muted first, then each powered down (35 ms each) */
        if (g->present >> s & 1u) (void)fs19xx_mute(&g->amp[s], 1);
    for (unsigned s = 0; s < AUDIO_AMPS; ++s)
        if ((g->present >> s & 1u) && fs19xx_shutdown(&g->amp[s])) bad |= 1u << s;
    if (bad) g->off_errors++;
    g->started = 0;
    g->have_first = 0;
    done(g);
    return bad;
}

void audio_amps_hold(AudioAmps *g)
{
    if (g->present) audio_amps_off(g);
    for (unsigned s = 0; s < AUDIO_AMPS; ++s) reset_line(g, s, 0);
    g->released = 0;
    g->present = g->configured = g->started = 0;
    done(g);
}
