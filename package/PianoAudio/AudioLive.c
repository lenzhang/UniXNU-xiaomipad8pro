/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Levels 8-10, the macOS devices on the DSP: see AudioLive.h. */
#include "AudioLive.h"
#include "PianoGpr.h"

#define LOG(l, ...) do {if ((l)->ops.log) (l)->ops.log((l)->ops.ctx, __VA_ARGS__);} while (0)

static void zero(void *p, uint32_t n) {for (uint32_t i = 0; i < n; ++i) ((uint8_t *)p)[i] = 0;}
static int load(const int *p) {return __atomic_load_n(p, __ATOMIC_ACQUIRE);}
static void store(int *p, int v) {__atomic_store_n(p, v, __ATOMIC_RELEASE);}

/* ------------------------------------------------------------------------------------------------ levels in dB, as Q15 */
/* 32767 * 10^(-0.5 n / 20) for n = 0..100: the master volume's steps of -0.5 dB (percent 100 - n). */
static const int16_t kHalfDb[101] = {
    32767, 30934, 29204, 27570, 26028, 24572, 23197, 21900, 20675, 19518, 18426, 17395, 16422, 15504, 14636, 13818, 13045, 12315,
    11626, 10976, 10362, 9782, 9235, 8718, 8231, 7770, 7336, 6925, 6538, 6172, 5827, 5501, 5193, 4903, 4628, 4370, 4125, 3894, 3677,
    3471, 3277, 3093, 2920, 2757, 2603, 2457, 2320, 2190, 2067, 1952, 1843, 1740, 1642, 1550, 1464, 1382, 1304, 1232, 1163, 1098,
    1036, 978, 923, 872, 823, 777, 734, 693, 654, 617, 583, 550, 519, 490, 463, 437, 413, 389, 368, 347, 328, 309, 292, 276, 260,
    246, 232, 219, 207, 195, 184, 174, 164, 155, 146, 138, 130, 123, 116, 110, 104};

int32_t audio_volume_q15(int percent)
{
    if (percent <= 0) return 0;
    if (percent >= 100) return AUDIO_Q15_ONE;
    return kHalfDb[100 - percent];
}

int32_t audio_ceiling_db(unsigned db)
{
    if (db < 1 || db > 40) return AUDIO_CEILING_TEST;
    const int32_t c = kHalfDb[2 * db];
    return c > AUDIO_CEILING_MAX ? AUDIO_CEILING_MAX : c;
}

/* ------------------------------------------------------------------------------------------------ the two clocks */
/* The ring's fill, sampled once a period where its owner reads or writes it, is noisy by up to a device period (240 frames: where
 * the other side's last 5 ms landed) and biased by where in that cycle the sample falls; so its slow average (1/64 a period) is held
 * at the level it settled at over the first 64 periods, give or take AUDIO_LIVE_SLIP_BAND: beyond, one frame slips a period
 * (+1: one too many, drop one; -1: one too few, repeat one). Only a drifting clock moves the average that far. */
static void drift_reset(AudioLiveDrift *d) {d->avg = 0; d->setpoint = 0; d->samples = 0;}
static int drift_update(AudioLiveDrift *d, uint32_t fill)
{
    const int32_t f = (int32_t)fill * 64;
    d->avg = d->samples ? d->avg + (f - d->avg) / 64 : f;
    if (++d->samples < 64) return 0;
    if (d->samples == 64) {d->setpoint = d->avg; return 0;}
    d->samples = 65;
    if (d->avg > d->setpoint + (int32_t)AUDIO_LIVE_SLIP_BAND * 64) {d->avg -= 64; return 1;}
    if (d->avg < d->setpoint - (int32_t)AUDIO_LIVE_SLIP_BAND * 64) {d->avg += 64; return -1;}
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the devices' side */
void audio_live_shared_init(AudioLiveShared *sh, int16_t *out_storage, int16_t *in_storage, int32_t volume_q15)
{
    zero(sh, sizeof *sh);
    (void)audio_ring_init(&sh->out, out_storage, AUDIO_LIVE_RING, 2);
    (void)audio_ring_init(&sh->in, in_storage, AUDIO_LIVE_MIC_RING, 1);
    sh->volume_q15 = volume_q15 < 0 ? 0 : volume_q15 > AUDIO_Q15_ONE ? AUDIO_Q15_ONE : volume_q15;
}

void audio_live_running(AudioLiveShared *sh, int output, int input)
{
    if (output && !load(&sh->want_out)) store(&sh->sound, 0);   /* a new run: only its own sound starts the path */
    store(&sh->want_out, output ? 1 : 0);
    store(&sh->want_in, input ? 1 : 0);
}

uint32_t audio_live_play(AudioLiveShared *sh, const int16_t *stereo, uint32_t frames, uint32_t now_ms, int *wake)
{
    int any = 0;
    for (uint32_t i = 0; i < 2 * frames; ++i) any |= stereo[i];
    *wake = 0;
    sh->offered += frames;
    if (any) {
        __atomic_store_n(&sh->sound_at, now_ms, __ATOMIC_RELEASE);
        store(&sh->have_sound, 1);
    }
    if (!load(&sh->accepting)) {
        if (any && !__atomic_exchange_n(&sh->sound, 1, __ATOMIC_ACQ_REL)) *wake = 1;   /* the first sound while the path is down */
        sh->dropped += frames;
        return frames;
    }
    const uint32_t n = audio_ring_write(&sh->out, stereo, frames);
    sh->taken += n;
    sh->dropped += frames - n;
    return frames;
}

uint32_t audio_live_capture(AudioLiveShared *sh, int16_t *mono, uint32_t frames)
{
    if (!load(&sh->capturing)) return 0;
    const uint32_t n = audio_ring_read(&sh->in, mono, frames);
    sh->captured += n;
    return n;
}

void audio_live_control(AudioLiveShared *sh, int volume_percent, int mute)
{
    if (volume_percent >= 0 && volume_percent <= 100) __atomic_store_n(&sh->volume_q15, audio_volume_q15(volume_percent), __ATOMIC_RELEASE);
    if (mute == 0 || mute == 1) store(&sh->muted, mute);
    sh->controls++;
}

/* ------------------------------------------------------------------------------------------------ the configuration */
void audio_live_defaults(AudioLiveConfig *c, uint32_t level)
{
    zero(c, sizeof *c);
    c->level = level < 8 ? 8 : level > 10 ? 10 : level;
    c->gain = AUDIO_AMPS_TEST_GAIN;
    c->ceiling = c->level >= 9 ? audio_ceiling_db(6) : AUDIO_CEILING_TEST;
    c->idle_ms = AUDIO_LIVE_IDLE_MS;
    c->status_ms = AUDIO_LIVE_STATUS_MS;
    c->answer_ms = AUDIO_ANSWER_MS;
    c->mic = c->level >= 10;
}

const char *audio_live_state(int state)
{
    switch (state) {
    case LiveDown: return "down";
    case LiveStarting: return "starting";
    case LiveRunning: return "running";
    case LiveStopping: return "stopping";
    case LiveOff: return "OFF for this boot";
    default: return "?";
    }
}

static const char *op_name(uint32_t op)
{
    switch (op) {
    case AR_OP_GRAPH_OPEN: return "GRAPH_OPEN";
    case AR_OP_GRAPH_PREPARE: return "GRAPH_PREPARE";
    case AR_OP_GRAPH_START: return "GRAPH_START";
    case AR_OP_GRAPH_STOP: return "GRAPH_STOP";
    case AR_OP_GRAPH_CLOSE: return "GRAPH_CLOSE";
    case AR_OP_SET_CFG: return "SET_CFG";
    case AR_OP_MEM_MAP: return "MEM_MAP";
    case AR_OP_MEM_UNMAP: return "MEM_UNMAP";
    case AR_OP_PRM_REQUEST: return "PRM REQUEST";
    case AR_OP_PRM_RELEASE: return "PRM RELEASE";
    default: return "?";
    }
}

/* ------------------------------------------------------------------------------------------------ the plans (one command at a time) */
static void plan_start(AudioLive *l, AudioLivePlan *p, const char *name, const ArStep *steps, unsigned count, uint32_t token_base,
                       void *context)
{
    ar_seq_init(&p->seq, steps, count, l->ports->apm, l->ports->prm, token_base, context);
    p->name = name;
    p->active = 1;
    p->in_flight = 0;
    p->unsent = 0;
    LOG(l, "live: plan %s: %u steps (APM from port %#x, PRM from port %#x)", name, count, l->ports->apm, l->ports->prm);
}

static int plan_settled(const AudioLivePlan *p)
{
    return !p->in_flight && !p->unsent &&
           (p->seq.state == ArSeqUp || p->seq.state == ArSeqDown || p->seq.state == ArSeqFailed);
}

/* The answer timer, then the next packet of plan p, unless the other plan has one out (one command at a time over both) or
 * PianoPower asks for quiet. */
static void plan_send(AudioLive *l, AudioLivePlan *p, const AudioLivePlan *other, uint32_t now)
{
    if (!p->active || l->off) return;
    if (p->in_flight && now - p->sent_at >= l->cfg.answer_ms) {
        l->timeouts++;
        LOG(l, "live: dsp: no answer to %s%s within %u ms%s", p->pending_undo ? "the undo of " : "", ar_seq_step_name(&p->seq, p->pending_step),
            l->cfg.answer_ms, p->pending_undo ? " (counted; the undoing goes on)" : ": undoing what was done, that step too");
        p->in_flight = 0;
        ar_seq_timeout(&p->seq);
    }
    if (p->in_flight || l->held || other->in_flight || other->unsent) return;
    if (!p->unsent) {
        const uint32_t n = ar_seq_next(&p->seq, p->pkt, sizeof p->pkt);
        if (!n) return;
        p->unsent = n;
        p->pending_undo = p->seq.pending_undo;
        p->pending_step = p->pending_undo ? (int)p->seq.undo_next : (int)p->seq.done;
        LOG(l, "live: dsp: -> %s%s (%s, %u bytes, token %#x, from port %#x)", p->pending_undo ? "undo: " : "",
            ar_seq_step_name(&p->seq, p->pending_step), op_name(p->seq.pending_opcode), n, p->seq.pending_token, p->seq.pending_port);
    }
    if (l->ops.send(l->ops.ctx, p->pkt, p->unsent)) {l->queue_full++; return;}
    p->unsent = 0;
    p->in_flight = 1;
    p->sent_at = now;
    l->sends++;
}

static int plan_receive(AudioLive *l, AudioLivePlan *p, const uint8_t *in, uint32_t n, uint32_t now)
{
    if (!p->active || !p->in_flight) return 0;
    const int r = ar_seq_receive(&p->seq, in, n);
    if (r == ArSeqIgnored) return 0;
    p->in_flight = 0;
    const char *name = ar_seq_step_name(&p->seq, p->pending_step);
    if (r == ArSeqAdvanced) LOG(l, "live: dsp: <- %s%s: done in %u ms", p->pending_undo ? "undo: " : "", name, now - p->sent_at);
    else if (p->pending_undo) LOG(l, "live: dsp: <- undo: %s: status %#x (counted; the undoing goes on)", name, p->seq.last_status);
    else LOG(l, "live: dsp: <- %s: status %#x: undoing what was done", name, p->seq.last_status);
    return 1;
}

static void plan_drop(AudioLivePlan *p)
{
    if (p->active && p->seq.state != ArSeqDown && p->seq.state != ArSeqFailed) {
        ar_seq_abort(&p->seq, ArSeqErrDspDown, 1);
        uint8_t scratch[GPR_MAX_PACKET];
        (void)ar_seq_next(&p->seq, scratch, sizeof scratch);   /* without the DSP: only the local undos run */
    }
    p->in_flight = 0;
    p->unsent = 0;
}

static void dma_zero(AudioLive *l, uint32_t offset, uint32_t bytes)
{
    if (!l->ops.dma || offset + bytes > l->ops.dma_bytes) return;
    volatile uint64_t *d = (volatile uint64_t *)(void *)(l->ops.dma + offset);
    for (uint32_t i = 0; i < bytes / 8u; ++i) d[i] = 0;
    if (l->ops.publish) l->ops.publish(l->ops.ctx);
}

/* ------------------------------------------------------------------------------------------------ the speaker path */
static unsigned live_gain_cap(const AudioLive *l) {return l->cfg.level >= 9 ? FS19XX_GAIN_CAP : AUDIO_AMPS_TEST_GAIN;}

static int live_amps_on(void *owner)
{
    AudioLive *l = owner;
    const int r = audio_amps_start_capped(l->amps, l->cfg.gain, live_gain_cap(l));
    const int db10 = fs19xx_gain_db10(l->amps->amp[0].gain);
    if (r) LOG(l, "live: amplifiers on FAILED (%s): all four muted and powered down again", audio_amps_error(r));
    else LOG(l, "live: amplifiers on: all four powered up MUTED, gain %u (-%d.%d dB)", l->amps->amp[0].gain, -db10 / 10, -db10 % 10);
    return r;
}

static int live_amps_off(void *owner)
{
    AudioLive *l = owner;
    const unsigned bad = audio_amps_off(l->amps);
    LOG(l, "live: amplifiers off: all four muted and powered down%s", bad ? " (a bus error on some: every write was tried)" : "");
    return bad ? -1 : 0;
}

/* The speaker wants to play: the device runs, the master mute is off, and sound came within idle_ms. */
static int spk_active(const AudioLive *l, uint32_t now, int *silent)
{
    const AudioLiveShared *sh = l->sh;
    const int have = load(&sh->have_sound);
    const uint32_t at = __atomic_load_n(&sh->sound_at, __ATOMIC_ACQUIRE);
    *silent = !have || now - at >= l->cfg.idle_ms;
    return load(&sh->want_out) && !(l->cfg.level >= 9 && load(&sh->muted)) && !*silent;
}

static void spk_begin(AudioLive *l, uint32_t now)
{
    ar_speaker_defaults(&l->speaker);
    l->speaker.buffer_phys = l->ops.dma_phys + AUDIO_DMA_SPEAKER;
    l->speaker.buffer_bytes = AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES;
    l->speaker.owner = l;
    l->speaker.amps_on = live_amps_on;
    l->speaker.amps_off = live_amps_off;
    dma_zero(l, AUDIO_DMA_SPEAKER, AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES);
    unsigned count;
    const ArStep *steps = ar_speaker_steps(&count);
    l->spk_starts++;
    LOG(l, "live: speaker: sound from the Speaker device: the speaker plan (start %u)", l->spk_starts);
    plan_start(l, &l->splan, "speaker", steps, count, 0x8000, &l->speaker);
    l->spk_state = LiveStarting;
    (void)now;
}

static void stream_begin(AudioLive *l, uint32_t now)
{
    uint64_t base = 0;
    (void)ar_dsp_address(l->speaker.buffer_phys, l->speaker.stream_id, &base);
    l->spk_mode = SpkPriming;
    l->spk_ending = l->unmuted = l->dsp_error = l->stream_unsent = 0;
    l->amp_bad = 0;
    l->stream_at = l->next_status = now;
    l->spk_reason = 0;
    if (ar_pump_init(&l->pump, base, l->splan.seq.handle, l->ports->data, AUDIO_SPK_SLOTS, AUDIO_SPK_PERIOD_BYTES, AUDIO_LIVE_DEPTH,
                     AUDIO_LATE_MS)) {
        LOG(l, "live: speaker: the period pump refused its geometry");
        l->dsp_error = 1;
    }
    (void)audio_chain_init(&l->chain, 4);
    (void)audio_chain_set_ceiling(&l->chain, l->cfg.ceiling);
    l->gain_now = l->cfg.level >= 9 ? __atomic_load_n(&l->sh->volume_q15, __ATOMIC_ACQUIRE) : AUDIO_Q15_ONE;
    audio_guard_start(&l->sguard, now, 0);   /* no time limit: the device's stream */
    audio_ring_reset(&l->sh->out);
    drift_reset(&l->spk_drift);
    store(&l->sh->accepting, 1);
    l->spk_state = LiveRunning;
    LOG(l, "live: speaker: stream up: 10 ms periods, %u in flight, the ring's target %u frames; ceiling %d, gain %u; %u silent periods "
        "first, the amplifiers muted", AUDIO_LIVE_DEPTH, AUDIO_LIVE_TARGET, l->chain.ceiling, l->amps->amp[0].gain, AUDIO_UNMUTE_PERIODS);
}

/* The ring's frames for one period: the drift held near the target by single frames, a stale backlog skipped. */
static void take_period(AudioLive *l)
{
    AudioRing *r = &l->sh->out;
    uint32_t fill = audio_ring_fill(r);
    if (fill > AUDIO_LIVE_TARGET + 3u * AUDIO_PERIOD_FRAMES) {   /* a backlog (the device stalled, then caught up): skipped */
        audio_ring_skip(r, fill - AUDIO_LIVE_TARGET);
        l->skipped++;
        fill = AUDIO_LIVE_TARGET;
        drift_reset(&l->spk_drift);
    }
    uint32_t want = AUDIO_PERIOD_FRAMES;
    const int slip = drift_update(&l->spk_drift, fill);
    if (slip > 0 && fill > AUDIO_PERIOD_FRAMES) {
        audio_ring_skip(r, 1);   /* the device runs fast: one frame less */
        l->drops++;
    } else if (slip < 0 && fill >= AUDIO_PERIOD_FRAMES) {
        want = AUDIO_PERIOD_FRAMES - 1u;   /* the DSP runs fast: one frame repeated */
        l->repeats++;
    }
    const uint32_t got = audio_ring_read(r, l->stereo, want);
    if (want < AUDIO_PERIOD_FRAMES) {
        l->stereo[2 * want] = got ? l->stereo[2 * want - 2] : 0;
        l->stereo[2 * want + 1] = got ? l->stereo[2 * want - 1] : 0;
    }
    if (got < want) l->underruns++;
}

static void fill_period(AudioLive *l, uint32_t slot)
{
    const int listening = l->spk_mode == SpkPlaying || l->spk_mode == SpkFading || l->spk_mode == SpkMuting;
    if (listening) {
        take_period(l);
    } else {
        for (uint32_t i = 0; i < 2u * AUDIO_PERIOD_FRAMES; ++i) l->stereo[i] = 0;
        const uint32_t fill = audio_ring_fill(&l->sh->out);
        if (fill > AUDIO_LIVE_TARGET) audio_ring_skip(&l->sh->out, fill - AUDIO_LIVE_TARGET);   /* keep the latency for the next sound */
    }
    /* the master volume (level 9), ramped */
    const int32_t target = l->cfg.level >= 9 ? __atomic_load_n(&l->sh->volume_q15, __ATOMIC_ACQUIRE) : AUDIO_Q15_ONE;
    if (l->gain_now < target) l->gain_now = target - l->gain_now > AUDIO_VOLUME_STEP ? l->gain_now + AUDIO_VOLUME_STEP : target;
    else if (l->gain_now > target) l->gain_now = l->gain_now - target > AUDIO_VOLUME_STEP ? l->gain_now - AUDIO_VOLUME_STEP : target;
    /* full scale lands on the ceiling (no clipping at the cap; the clamp stays as the last guard) */
    (void)audio_chain_set_gain(&l->chain, (int32_t)((int64_t)l->gain_now * l->chain.ceiling / AUDIO_Q15_ONE));
    audio_stereo_to_quad(l->stereo, l->quad, AUDIO_PERIOD_FRAMES);
    audio_chain_run(&l->chain, l->quad, AUDIO_PERIOD_FRAMES);
    volatile uint64_t *d = (volatile uint64_t *)(void *)(l->ops.dma + AUDIO_DMA_SPEAKER + slot * AUDIO_SPK_PERIOD_BYTES);
    int sound = 0;
    for (uint32_t i = 0; i < AUDIO_PERIOD_FRAMES; ++i) {
        const int16_t *f = l->quad + 4u * i;
        for (unsigned c = 0; c < 4; ++c) {
            const int32_t v = f[c] < 0 ? -(int32_t)f[c] : f[c];
            if (v > l->peak) l->peak = v;
            sound |= v != 0;
        }
        d[i] = (uint64_t)(uint16_t)f[0] | (uint64_t)(uint16_t)f[1] << 16 | (uint64_t)(uint16_t)f[2] << 32 | (uint64_t)(uint16_t)f[3] << 48;
    }
    if (l->ops.publish) l->ops.publish(l->ops.ctx);
    l->periods++;
    if (sound) l->sound_periods++;
}

static void status_sample(AudioLive *l)
{
    const unsigned failed = audio_amps_read(l->amps);
    if (failed) {
        l->amp_bad |= failed;
        LOG(l, "live: amp: a status read failed (slots %#x)", failed);
        return;
    }
    const unsigned bad = audio_amps_check(l->amps);
    if (bad) {
        l->amp_bad |= bad;
        for (unsigned i = 0; i < AUDIO_AMPS; ++i) {
            if (!(bad >> i & 1u)) continue;
            char names[80];
            const Fs19xxStatus *st = &l->amps->status[i];
            fs19xx_status_names(st->status & FS19XX_STATUS_BITS, names, sizeof names);
            LOG(l, "live: amp %s: STATUS 0x%04x (%s) PWRCTRL 0x%04x DIGSTAT 0x%04x VOLCTRL 0x%04x: flagged by the group check", audio_amp_name(i),
                st->status, names[0] ? names : "none", st->pwrctrl, st->digstat, st->volctrl);
        }
    }
}

/* A fault while the stream runs: the amplifiers down at once, the buffer zeroed, the plan undone. */
static void stream_fault(AudioLive *l, int reason, uint32_t now)
{
    store(&l->sh->accepting, 0);
    const unsigned bad = audio_amps_off(l->amps);
    dma_zero(l, AUDIO_DMA_SPEAKER, AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES);
    audio_guard_trip(&l->sguard, reason);
    l->spk_reason = reason;
    l->unmuted = 0;
    LOG(l, "live: speaker: STOPPED: %s after %u ms (%u periods sent, %u returned, %u late, %u errors, last status %#x); the amplifiers "
        "muted and powered down at once%s; undoing the plan", audio_guard_reason(reason), now - l->stream_at, l->pump.sent, l->pump.returned,
        l->pump.late, l->pump.errors, l->pump.last_status, bad ? " (a bus error on some)" : "");
    ar_seq_abort(&l->splan.seq, ArSeqErrAbort, 0);
    l->spk_state = LiveStopping;
}

static void stream_end(AudioLive *l, uint32_t now)
{
    store(&l->sh->accepting, 0);
    LOG(l, "live: speaker: no sound for %u ms: down after %u ms (%u periods, %u with sound, %u underruns, %u skips, %u frames dropped, %u "
        "repeated; peak %d of ceiling %d): the plan undone, the amplifiers first", l->cfg.idle_ms, now - l->stream_at, l->pump.sent,
        l->sound_periods, l->underruns, l->skipped, l->drops, l->repeats, l->peak, l->chain.ceiling);
    ar_seq_stop(&l->splan.seq);
    l->spk_state = LiveStopping;
}

static void spk_stream_tick(AudioLive *l, uint32_t now)
{
    const int late = ar_pump_check(&l->pump, now);
    int r = audio_guard_check(&l->sguard, now, l->dsp_error, late, l->amp_bad);
    if (!r && l->held) r = AudioGuardQuiet;
    if (r) {stream_fault(l, r, now); return;}
    if (l->unmuted && (int32_t)(now - l->next_status) >= 0) {
        l->next_status = now + l->cfg.status_ms;
        status_sample(l);
        if (l->amp_bad) {stream_fault(l, AudioGuardAmplifier, now); return;}
    }
    int silent = 0;
    const int active = spk_active(l, now, &silent);
    switch (l->spk_mode) {
    case SpkPriming:
        if (l->pump.returned < AUDIO_UNMUTE_PERIODS) break;
        if (!active) {
            l->spk_mode = SpkQuiet;
            l->quiet_at = now;
            l->hold_from_sound = silent;
            break;
        }
        /* fallthrough: unmute */
    case SpkQuiet:
        if (l->spk_mode == SpkQuiet && !active) {
            const uint32_t from = l->hold_from_sound ? __atomic_load_n(&l->sh->sound_at, __ATOMIC_ACQUIRE) : l->quiet_at;
            if (now - from >= l->cfg.idle_ms || !load(&l->sh->have_sound)) l->spk_ending = 1;
            break;
        }
        l->spk_ending = 0;
        if (audio_amps_unmute(l->amps)) {
            LOG(l, "live: speaker: unmuting failed: the amplifiers muted and powered down");
            stream_fault(l, AudioGuardAmplifier, now);
            return;
        }
        audio_chain_restart(&l->chain);
        l->unmuted = 1;
        l->next_status = now + AUDIO_STATUS_MS;
        drift_reset(&l->spk_drift);
        l->spk_mode = SpkPlaying;
        LOG(l, "live: speaker: %u periods returned: the amplifiers unmuted, the sound fades in", l->pump.returned);
        break;
    case SpkPlaying:
        if (active) break;
        audio_chain_fade(&l->chain, -1);
        l->spk_mode = SpkFading;
        l->hold_from_sound = silent;
        break;
    case SpkFading:
        if (active) {audio_chain_fade(&l->chain, +1); l->spk_mode = SpkPlaying; break;}
        if (!audio_chain_silent(&l->chain)) break;
        l->mute_after = l->pump.sent;   /* the fade's last periods are still queued at the DSP: muted once they played */
        l->spk_mode = SpkMuting;
        break;
    case SpkMuting:
        if (active) {audio_chain_restart(&l->chain); l->spk_mode = SpkPlaying; break;}
        if (l->pump.returned < l->mute_after) break;
        if (audio_amps_mute(l->amps)) {stream_fault(l, AudioGuardAmplifier, now); return;}
        l->unmuted = 0;
        l->spk_mode = SpkQuiet;
        l->quiet_at = now;
        LOG(l, "live: speaker: %s: faded to silence, the amplifiers muted; down after %u ms more without sound", silent ? "silence" :
            !load(&l->sh->want_out) ? "the device stopped" : "muted", l->cfg.idle_ms);
        break;
    }
    if (l->stream_unsent) {
        if (l->ops.send(l->ops.ctx, l->spkt, l->stream_len)) {l->queue_full++; return;}
        l->stream_unsent = 0;
        l->sends++;
    }
    while (!l->spk_ending) {
        const int slot = ar_pump_free_slot(&l->pump);
        if (slot < 0) break;
        fill_period(l, (uint32_t)slot);
        l->stream_len = ar_pump_write(&l->pump, l->spkt, sizeof l->spkt, now);
        if (!l->stream_len) {l->dsp_error = 1; break;}
        if (l->ops.send(l->ops.ctx, l->spkt, l->stream_len)) {l->stream_unsent = 1; l->queue_full++; break;}
        l->sends++;
    }
    if (l->spk_ending && !l->stream_unsent && !l->pump.in_flight) stream_end(l, now);
}

static void spk_tick(AudioLive *l, uint32_t now)
{
    switch (l->spk_state) {
    case LiveDown: {
        /* held, muted or no device: the sound flag stays set, so play() wakes nobody until it is taken (the control, the device's
         * start and the end of quiet come with their own wake or the next tick) */
        if (l->held || l->aborting || !load(&l->sh->want_out) || (l->cfg.level >= 9 && load(&l->sh->muted))) return;
        int silent = 0;
        const int active = spk_active(l, now, &silent);
        const int fresh = __atomic_exchange_n(&l->sh->sound, 0, __ATOMIC_ACQ_REL);
        if (!active && !fresh) return;
        spk_begin(l, now);
        break;
    }
    case LiveRunning:
        spk_stream_tick(l, now);
        break;
    default:
        break;
    }
    plan_send(l, &l->splan, &l->mplan, now);
    if (!plan_settled(&l->splan)) return;
    const int up = l->splan.seq.state == ArSeqUp;
    const int down = (l->splan.seq.state == ArSeqDown || l->splan.seq.state == ArSeqFailed) && !l->splan.seq.undo_errors;
    if (l->spk_state == LiveStarting) {
        if (up) {stream_begin(l, now); return;}
        LOG(l, "live: speaker: the plan failed at %s (status %#x): the speaker path is OFF for this boot",
            ar_seq_step_name(&l->splan.seq, l->splan.seq.failed_step), l->splan.seq.error_status);
        l->splan.active = 0;
        l->spk_state = LiveOff;
        audio_amps_hold(l->amps);
        return;
    }
    if (l->spk_state == LiveStopping) {
        l->splan.active = 0;
        l->spk_stops++;
        const int reason = l->spk_reason;
        if (!down) {
            LOG(l, "live: speaker: undoing the plan failed (%u undo errors): the speaker path is OFF for this boot", l->splan.seq.undo_errors);
            l->spk_state = LiveOff;
        } else if (reason == AudioGuardLate && ++l->late_stops >= AUDIO_LIVE_LATE_STOPS) {
            LOG(l, "live: speaker: %u late stops: the speaker path is OFF for this boot", l->late_stops);
            l->spk_state = LiveOff;
        } else if (reason && reason != AudioGuardLate && reason != AudioGuardQuiet) {
            LOG(l, "live: speaker: stopped by %s: the speaker path is OFF for this boot", audio_guard_reason(reason));
            l->spk_state = LiveOff;
        } else {
            LOG(l, "live: speaker: down%s", reason ? " (it comes back with the next sound)" : "");
            l->spk_state = LiveDown;
            store(&l->sh->sound, 0);
        }
        if (l->spk_state == LiveOff) audio_amps_hold(l->amps);
    }
}

/* ------------------------------------------------------------------------------------------------ the mic path (level 10) */
static int live_va_on(void *owner)
{
    AudioLive *l = owner;
    const int r = l->ops.va_mic ? l->ops.va_mic(l->ops.ctx, 1) : -1;
    LOG(l, "live: mic: the VA macro's DMIC path %s", r ? "FAILED" : "on: DMIC0 -> DEC0, DMIC2 -> DEC1, 2.4 MHz, 48 kHz");
    if (!r) l->va_on = 1;
    return r;
}

static int live_va_off(void *owner)
{
    AudioLive *l = owner;
    if (!l->va_on) return 0;
    const int r = l->ops.va_mic ? l->ops.va_mic(l->ops.ctx, 0) : 0;
    l->va_on = 0;
    LOG(l, "live: mic: the VA macro's DMIC path off%s", r ? " (with an error)" : "");
    return r;
}

static void mic_begin(AudioLive *l)
{
    ar_mic_defaults(&l->micplan);
    l->micplan.buffer_phys = l->ops.dma_phys + AUDIO_DMA_MIC;
    l->micplan.buffer_bytes = AUDIO_MIC_SLOTS * AUDIO_MIC_PERIOD_BYTES;
    l->micplan.owner = l;
    l->micplan.va_on = live_va_on;
    l->micplan.va_off = live_va_off;
    dma_zero(l, AUDIO_DMA_MIC, AUDIO_MIC_SLOTS * AUDIO_MIC_PERIOD_BYTES);
    unsigned count;
    const ArStep *steps = ar_mic_live_steps(&count);
    l->mic_starts++;
    LOG(l, "live: mic: the Mic device runs: the capture plan (start %u)", l->mic_starts);
    plan_start(l, &l->mplan, "microphone", steps, count, 0x9000, &l->micplan);
    l->mic_state = LiveStarting;
}

static void capture_begin(AudioLive *l, uint32_t now)
{
    uint64_t base = 0;
    (void)ar_dsp_address(l->micplan.buffer_phys, l->micplan.stream_id, &base);
    l->mic_ending = l->cap_unsent = l->cap_error = 0;
    l->mic_reason = 0;
    l->mic_at = now;
    l->mic_idle_at = 0;
    if (ar_pump_init_capture(&l->cap, base, l->mplan.seq.handle, l->ports->data, AUDIO_MIC_SLOTS, AUDIO_MIC_PERIOD_BYTES,
                             AUDIO_LIVE_MIC_DEPTH, AUDIO_LATE_MS))
        l->cap_error = 1;
    l->sh->in.head = l->sh->in.tail = 0;   /* nobody reads it while capturing is 0 */
    drift_reset(&l->mic_drift);
    store(&l->sh->capturing, 1);
    l->mic_state = LiveRunning;
    LOG(l, "live: mic: capture up: 10 ms periods of two channels, %u in flight, mixed to mono; the first %u ms dropped",
        AUDIO_LIVE_MIC_DEPTH, AUDIO_LIVE_MIC_SETTLE_MS);
}

/* One returned capture period into the Mic's ring: two channels to mono, the drift held near the ring's target. */
static void capture_period(AudioLive *l, const ArAnswer *a, uint32_t now)
{
    const uint8_t *src = l->ops.dma + AUDIO_DMA_MIC + l->cap.last_slot * AUDIO_MIC_PERIOD_BYTES + a->offset;
    uint32_t frames = a->data_bytes / 4u;
    if (frames > AUDIO_PERIOD_FRAMES) frames = AUDIO_PERIOD_FRAMES;
    const int settle = now - l->mic_at < AUDIO_LIVE_MIC_SETTLE_MS;
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t a0 = (int16_t)(src[4 * i] | src[4 * i + 1] << 8), a1 = (int16_t)(src[4 * i + 2] | src[4 * i + 3] << 8);
        l->mono[i] = settle ? 0 : (int16_t)(((int32_t)a0 + a1) / 2);
    }
    l->mic_periods++;
    AudioRing *r = &l->sh->in;
    const uint32_t fill = audio_ring_fill(r);
    if (fill > AUDIO_LIVE_MIC_TARGET + 3u * AUDIO_PERIOD_FRAMES) {l->mic_full++; drift_reset(&l->mic_drift); return;}   /* not read: dropped */
    const int slip = drift_update(&l->mic_drift, fill);
    if (frames && slip > 0) {
        frames--;   /* the Mic's engine runs slow: one frame less */
        l->mic_drops++;
    } else if (frames && slip < 0) {
        l->mono[frames] = l->mono[frames - 1];   /* it runs fast: one frame repeated */
        frames++;
        l->mic_repeats++;
    }
    (void)audio_ring_write(r, l->mono, frames);
}

static void mic_fault(AudioLive *l, int reason, uint32_t now)
{
    store(&l->sh->capturing, 0);
    l->mic_reason = reason;
    LOG(l, "live: mic: STOPPED: %s after %u ms (%u periods sent, %u returned, %u late, %u errors, last status %#x): undoing the plan",
        audio_guard_reason(reason), now - l->mic_at, l->cap.sent, l->cap.returned, l->cap.late, l->cap.errors, l->cap.last_status);
    ar_seq_abort(&l->mplan.seq, ArSeqErrAbort, 0);
    l->mic_state = LiveStopping;
}

static void capture_tick(AudioLive *l, uint32_t now)
{
    int r = l->cap_error ? AudioGuardDsp : ar_pump_check(&l->cap, now) ? AudioGuardLate : AudioGuardOk;
    if (!r && l->held) r = AudioGuardQuiet;
    if (r) {mic_fault(l, r, now); return;}
    if (load(&l->sh->want_in)) l->mic_idle_at = 0;
    else if (!l->mic_idle_at) l->mic_idle_at = now ? now : 1;
    if (l->mic_idle_at && now - l->mic_idle_at >= l->cfg.idle_ms) l->mic_ending = 1;
    if (l->cap_unsent) {
        if (l->ops.send(l->ops.ctx, l->cpkt, l->cap_len)) {l->queue_full++; return;}
        l->cap_unsent = 0;
        l->sends++;
    }
    while (!l->mic_ending && ar_pump_free_slot(&l->cap) >= 0) {
        l->cap_len = ar_pump_write(&l->cap, l->cpkt, sizeof l->cpkt, now);
        if (!l->cap_len) {l->cap_error = 1; break;}
        if (l->ops.send(l->ops.ctx, l->cpkt, l->cap_len)) {l->cap_unsent = 1; l->queue_full++; break;}
        l->sends++;
    }
    if (l->mic_ending && !l->cap_unsent && !l->cap.in_flight) {
        store(&l->sh->capturing, 0);
        LOG(l, "live: mic: the Mic device stopped: down after %u ms (%u periods, %u dropped, %u repeated, %u times the ring was full): "
            "the plan undone", now - l->mic_at, l->mic_periods, l->mic_drops, l->mic_repeats, l->mic_full);
        ar_seq_stop(&l->mplan.seq);
        l->mic_state = LiveStopping;
    }
}

static void mic_tick(AudioLive *l, uint32_t now)
{
    if (!l->cfg.mic) return;
    switch (l->mic_state) {
    case LiveDown:
        if (l->held || l->aborting || !load(&l->sh->want_in)) return;
        mic_begin(l);
        break;
    case LiveRunning:
        capture_tick(l, now);
        break;
    default:
        break;
    }
    plan_send(l, &l->mplan, &l->splan, now);
    if (!plan_settled(&l->mplan)) return;
    const int up = l->mplan.seq.state == ArSeqUp;
    const int down = (l->mplan.seq.state == ArSeqDown || l->mplan.seq.state == ArSeqFailed) && !l->mplan.seq.undo_errors;
    if (l->mic_state == LiveStarting) {
        if (up) {capture_begin(l, now); return;}
        LOG(l, "live: mic: the capture plan failed at %s (status %#x): the mic path is OFF for this boot",
            ar_seq_step_name(&l->mplan.seq, l->mplan.seq.failed_step), l->mplan.seq.error_status);
        l->mplan.active = 0;
        l->mic_state = LiveOff;
        return;
    }
    if (l->mic_state == LiveStopping) {
        l->mplan.active = 0;
        const int reason = l->mic_reason;
        if (!down) l->mic_state = LiveOff;
        else if (reason == AudioGuardLate && ++l->mic_late_stops >= AUDIO_LIVE_LATE_STOPS) l->mic_state = LiveOff;
        else if (reason && reason != AudioGuardLate && reason != AudioGuardQuiet) l->mic_state = LiveOff;
        else l->mic_state = LiveDown;
        LOG(l, "live: mic: %s", l->mic_state == LiveOff ? "the mic path is OFF for this boot" : "down");
    }
}

/* ------------------------------------------------------------------------------------------------ the whole */
static void finish(AudioLive *l, uint32_t now)
{
    store(&l->sh->accepting, 0);
    store(&l->sh->capturing, 0);
    audio_amps_hold(l->amps);
    if (l->va_on && l->ops.va_mic) {(void)l->ops.va_mic(l->ops.ctx, 0); l->va_on = 0;}
    dma_zero(l, 0, AUDIO_DMA_BYTES);
    if (l->ops.tdm_pins) (void)l->ops.tdm_pins(l->ops.ctx, 0);
    l->splan.active = l->mplan.active = 0;
    if (l->spk_state != LiveOff) l->spk_state = LiveDown;
    if (l->mic_state != LiveOff) l->mic_state = LiveDown;
    LOG(l, "live: done after %u ms: the amplifiers held in reset, the TDM pins given back; speaker %s (%u starts, %u periods, %u with "
        "sound, %u underruns, %u skips, %u dropped, %u repeated, peak %d), mic %s (%u starts, %u periods); %u packets sent, %u received, "
        "%u ignored, %u unanswered, %u times the queue was full", now - l->started_at, audio_live_state(l->spk_state), l->spk_starts,
        l->periods, l->sound_periods, l->underruns, l->skipped, l->drops, l->repeats, l->peak, audio_live_state(l->mic_state),
        l->mic_starts, l->mic_periods, l->sends, l->received, l->stray, l->timeouts, l->queue_full);
    l->finished = 1;
}

/* PianoPower's health turned the guard off: no DSP command any more; the amplifiers down and held now. */
static void go_off(AudioLive *l, uint32_t now)
{
    l->off = 1;
    LOG(l, "live: dsp: guard OFF: %s: no DSP command goes out any more in this boot; the amplifiers muted, powered down and held in reset",
        adg_reason(l->guard ? l->guard->reason : AdgReasonOwner));
    store(&l->sh->accepting, 0);
    store(&l->sh->capturing, 0);
    audio_amps_hold(l->amps);
    l->va_on = 0;   /* the VA macro and the LPI pins are clocked by the DSP's votes: not touched once it may be gone */
    plan_drop(&l->splan);
    plan_drop(&l->mplan);
    finish(l, now);
}

int audio_live_init(AudioLive *l, const AudioLiveConfig *c, const AudioSession *s, AudioLiveShared *sh)
{
    zero(l, sizeof *l);
    if (!s || !s->kept || !s->guard || !s->ops.dma || !sh || !sh->out.data || !sh->in.data) return -1;
    l->cfg = *c;
    if (l->cfg.level < 8) l->cfg.level = 8;
    if (l->cfg.level > 10) l->cfg.level = 10;
    if (l->cfg.level < 10) l->cfg.mic = 0;
    const unsigned cap = l->cfg.level >= 9 ? FS19XX_GAIN_CAP : AUDIO_AMPS_TEST_GAIN;
    if (l->cfg.gain > cap) l->cfg.gain = cap;
    const int32_t ceiling = l->cfg.level >= 9 ? AUDIO_CEILING_MAX : AUDIO_CEILING_TEST;
    if (l->cfg.ceiling <= 0 || l->cfg.ceiling > ceiling) l->cfg.ceiling = ceiling;
    if (!l->cfg.idle_ms) l->cfg.idle_ms = AUDIO_LIVE_IDLE_MS;
    if (l->cfg.idle_ms > AUDIO_LIVE_IDLE_MAX_MS) l->cfg.idle_ms = AUDIO_LIVE_IDLE_MAX_MS;
    if (l->cfg.status_ms < 100) l->cfg.status_ms = 100;
    if (!l->cfg.answer_ms) l->cfg.answer_ms = AUDIO_ANSWER_MS;
    l->ops = s->ops;
    l->guard = s->guard;
    l->ports = s->ports;
    l->amps = (AudioAmps *)&s->amps;
    l->sh = sh;
    return 0;
}

uint32_t audio_live_tick(AudioLive *l, const AdgObservation *o, uint32_t now_ms)
{
    if (l->finished) return 0;
    if (!l->started_at) {
        l->started_at = now_ms ? now_ms : 1;
        const int db10 = fs19xx_gain_db10(l->cfg.gain);
        LOG(l, "live: level %u: the Speaker%s on the DSP: playing gain %u (-%d.%d dB, cap %u), ceiling %d (max %d), master volume %s; down after "
            "%u ms without sound; ports %s", l->cfg.level, l->cfg.mic ? " and the Mic" : "", l->cfg.gain, -db10 / 10, -db10 % 10,
            live_gain_cap(l), l->cfg.ceiling, l->cfg.level >= 9 ? AUDIO_CEILING_MAX : AUDIO_CEILING_TEST, l->cfg.level >= 9 ? "on" : "off (level 9)",
            l->cfg.idle_ms, l->ports->name);
    }
    l->now = now_ms;
    const int g = o ? adg_observe(l->guard, o, now_ms) : l->guard->state;
    if (g == AdgOff) {go_off(l, now_ms); return 0;}
    l->held = g == AdgWait;
    if (l->held != l->was_held)
        LOG(l, l->held ? "live: dsp: PianoPower asks for quiet: nothing new goes to the DSP until it ends" : "live: dsp: quiet over");
    l->was_held = l->held;
    spk_tick(l, now_ms);
    mic_tick(l, now_ms);
    if (l->aborting && l->spk_state != LiveStarting && l->spk_state != LiveRunning && l->spk_state != LiveStopping &&
        l->mic_state != LiveStarting && l->mic_state != LiveRunning && l->mic_state != LiveStopping)
        finish(l, now_ms);
    if (l->finished) return 0;
    if (l->spk_state == LiveRunning || l->mic_state == LiveRunning) return 5;   /* 40 ms queued at the DSP: a refill within 5 ms */
    if (l->spk_state == LiveStarting || l->spk_state == LiveStopping || l->mic_state == LiveStarting || l->mic_state == LiveStopping) return 2;
    return AUDIO_LIVE_IDLE_TICK_MS;
}

void audio_live_receive(AudioLive *l, const uint8_t *packet, uint32_t length, uint32_t now_ms)
{
    if (l->finished || l->off) return;
    l->received++;
    ArAnswer a;
    const int kind = ar_parse(packet, length, &a);
    if (kind == ArWriteDone && l->spk_state == LiveRunning) {
        const int r = ar_pump_done(&l->pump, &a);
        if (r == 1) return;
        if (r == -1) {
            LOG(l, "live: dsp: WRITE_DONE refused: status %#x for %#llx (token %#x)", a.status, (unsigned long long)a.address, a.token);
            l->dsp_error = 1;
            return;
        }
    }
    if (kind == ArReadDone && l->mic_state == LiveRunning) {
        const int r = ar_pump_done(&l->cap, &a);
        if (r == 1) {capture_period(l, &a, now_ms); return;}
        if (r == -1) {
            LOG(l, "live: dsp: READ_DONE refused: status %#x for %#llx (token %#x, %u bytes at %u)", a.status, (unsigned long long)a.address,
                a.token, a.data_bytes, a.offset);
            l->cap_error = 1;
            return;
        }
    }
    if (plan_receive(l, &l->splan, packet, length, now_ms) || plan_receive(l, &l->mplan, packet, length, now_ms)) return;
    if ((kind == ArWriteDone || kind == ArReadDone) && (l->spk_state == LiveStopping || l->mic_state == LiveStopping)) return;
    if (++l->stray <= 8)
        LOG(l, "live: dsp: a packet nobody waits for: opcode %#x token %#x to port %#x%s", a.opcode, a.token, a.dst_port, kind == ArBad ? " (malformed)" : "");
}

void audio_live_abort(AudioLive *l, int hard, uint32_t now_ms)
{
    if (l->finished) return;
    LOG(l, "live: stopped by the owner%s", hard ? " (without DSP commands)" : "");
    store(&l->sh->accepting, 0);
    store(&l->sh->capturing, 0);
    (void)audio_amps_off(l->amps);
    if (hard || l->off) {
        plan_drop(&l->splan);
        plan_drop(&l->mplan);
        finish(l, now_ms);
        return;
    }
    /* soft: each plan's undo commands; the owner ticks on until both are down (or stops hard) */
    l->aborting = 1;
    if (l->splan.active && l->splan.seq.state != ArSeqDown && l->splan.seq.state != ArSeqFailed) {
        ar_seq_abort(&l->splan.seq, ArSeqErrAbort, 0);
        l->spk_state = LiveStopping;
        l->spk_reason = AudioGuardOwner;
    }
    if (l->mplan.active && l->mplan.seq.state != ArSeqDown && l->mplan.seq.state != ArSeqFailed) {
        ar_seq_abort(&l->mplan.seq, ArSeqErrAbort, 0);
        l->mic_state = LiveStopping;
        l->mic_reason = AudioGuardOwner;
    }
    if (l->spk_state != LiveStopping && l->mic_state != LiveStopping) finish(l, now_ms);
}

