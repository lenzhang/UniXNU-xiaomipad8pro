/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Levels 4-7 as one state machine: see AudioSession.h. */
#include "AudioSession.h"
#include "PianoGpr.h"

#define LOG(s, ...) do {if ((s)->ops.log) (s)->ops.log((s)->ops.ctx, __VA_ARGS__);} while (0)

enum {PhBegin = 0, PhClock, PhVa, PhAmps, PhSpeaker, PhSpeakerStream, PhSpeakerDown, PhMic, PhTone, PhToneStream, PhToneDown, PhAbort,
      PhEnd, PhDone};

static void zero(void *p, uint32_t n) {for (uint32_t i = 0; i < n; ++i) ((uint8_t *)p)[i] = 0;}

const char *audio_session_phase(int phase)
{
    static const char *const names[] = {"begin", "level 4 bit clock", "level 4 VA clocks", "level 5 amplifiers", "level 6 speaker plan",
                                        "level 6 silent stream", "level 6 speaker undo", "level 6 microphone plan", "level 7 speaker plan",
                                        "level 7 tone stream", "level 7 speaker undo", "stopping", "end", "done"};
    return phase >= 0 && phase <= PhDone ? names[phase] : "?";
}

const char *audio_level_result(int result)
{
    switch (result) {
    case AudioLevelNotRun: return "not run";
    case AudioLevelPassed: return "passed";
    case AudioLevelFailed: return "FAILED";
    case AudioLevelSkipped: return "skipped";
    default: return "?";
    }
}

void audio_session_defaults(AudioSessionConfig *c)
{
    zero(c, sizeof *c);
    c->level = 4;
    c->ports = AudioPortsUbuntu;
    c->lpass_read = 1;
    c->mic = 1;
    c->silence_ms = 2000;
    c->tone_mask = 0xf;
    c->tone_ms = 600;
    c->gap_ms = 150;
    c->tone_gain = AUDIO_AMPS_TEST_GAIN;
    c->answer_ms = AUDIO_ANSWER_MS;
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

static unsigned level_of(int phase)
{
    switch (phase) {
    case PhClock: case PhVa: return 4;
    case PhAmps: return 5;
    case PhSpeaker: case PhSpeakerStream: case PhSpeakerDown: case PhMic: return 6;
    case PhTone: case PhToneStream: case PhToneDown: return 7;
    default: return 0;
    }
}

/* The first verdict of a level stays; a failure replaces a pass (the mic plan after the speaker's). */
static void result(AudioSession *s, unsigned level, int r, const char *why)
{
    if (level < 4 || level > 7 || s->result[level] == AudioLevelFailed) return;
    if (s->result[level] == AudioLevelPassed && r != AudioLevelFailed) return;
    s->result[level] = r;
    s->why[level] = why;
}
static void fail_step(AudioSession *s, unsigned level, const char *why)
{
    if (level >= 4 && level <= 7 && s->result[level] != AudioLevelFailed) {
        s->failed_step[level] = s->seq.failed_step >= 0 ? ar_seq_step_name(&s->seq, s->seq.failed_step) : s->plan;
        s->failed_status[level] = s->seq.error_status;
    }
    result(s, level, AudioLevelFailed, why);
}
static void skip_from(AudioSession *s, unsigned level, const char *why)
{
    for (unsigned l = level; l <= s->cfg.level && l <= 7; ++l) result(s, l, AudioLevelSkipped, why);
}

static void enter(AudioSession *s, int phase, uint32_t now) {s->phase = phase; s->phase_at = now;}
static void begin(AudioSession *s, int phase, uint32_t now);

/* ------------------------------------------------------------------------------------------------------------ the buffers */
static void dma_zero(AudioSession *s, uint32_t offset, uint32_t bytes)
{
    if (!s->ops.dma || offset + bytes > s->ops.dma_bytes) return;
    volatile uint64_t *d = (volatile uint64_t *)(void *)(s->ops.dma + offset);
    for (uint32_t i = 0; i < bytes / 8u; ++i) d[i] = 0;
    if (s->ops.publish) s->ops.publish(s->ops.ctx);
}

/* --------------------------------------------------------------------------------------------- the plan in progress */
static void plan_start(AudioSession *s, const char *name, const ArStep *steps, unsigned count, uint32_t token_base, void *context,
                       unsigned level, uint32_t now)
{
    ar_seq_init(&s->seq, steps, count, s->ports->apm, s->ports->prm, token_base, context);
    s->plan = name;
    s->plan_level = (int)level;
    s->seq_active = 1;
    s->in_flight = 0;
    s->unsent = 0;
    LOG(s, "plan %s: %u steps, one at a time (APM from port %#x, PRM from port %#x)", name, count, s->ports->apm, s->ports->prm);
    (void)now;
}

static int settled(const AudioSession *s)
{
    return !s->in_flight && !s->unsent && (s->seq.state == ArSeqUp || s->seq.state == ArSeqDown || s->seq.state == ArSeqFailed);
}

/* The answer timer, then the next packet of the plan (local steps run inside ar_seq_next), held while PianoPower asks for quiet. */
static void plan_send(AudioSession *s, uint32_t now)
{
    if (!s->seq_active || s->off) return;
    if (s->in_flight && now - s->sent_at >= s->cfg.answer_ms) {
        s->timeouts++;
        LOG(s, "dsp: no answer to %s%s within %u ms%s", s->pending_undo ? "the undo of " : "", ar_seq_step_name(&s->seq, s->pending_step),
            s->cfg.answer_ms, s->pending_undo ? " (counted; the undoing goes on)" : ": undoing what was done, that step too");
        s->in_flight = 0;
        ar_seq_timeout(&s->seq);
    }
    if (s->in_flight || s->held) return;
    if (!s->unsent) {
        const uint32_t n = ar_seq_next(&s->seq, s->pkt, sizeof s->pkt);
        if (!n) return;
        s->unsent = n;
        s->pending_undo = s->seq.pending_undo;
        s->pending_step = s->pending_undo ? (int)s->seq.undo_next : (int)s->seq.done;
        LOG(s, "dsp: -> %s%s (%s, %u bytes, token %#x, from port %#x)", s->pending_undo ? "undo: " : "",
            ar_seq_step_name(&s->seq, s->pending_step), op_name(s->seq.pending_opcode), n, s->seq.pending_token, s->seq.pending_port);
    }
    if (s->ops.send(s->ops.ctx, s->pkt, s->unsent)) {s->queue_full++; return;}   /* offered again at the next tick */
    s->unsent = 0;
    s->in_flight = 1;
    s->sent_at = now;
    s->sends++;
}

static int plan_receive(AudioSession *s, const uint8_t *in, uint32_t n, uint32_t now)
{
    if (!s->seq_active || !s->in_flight) return 0;
    const int r = ar_seq_receive(&s->seq, in, n);
    if (r == ArSeqIgnored) return 0;
    s->in_flight = 0;
    const char *name = ar_seq_step_name(&s->seq, s->pending_step);
    if (r == ArSeqAdvanced)
        LOG(s, "dsp: <- %s%s: done in %u ms%s", s->pending_undo ? "undo: " : "", name, now - s->sent_at,
            s->seq.handle && s->seq.last_opcode == AR_OP_MEM_MAP && !s->pending_undo ? " (map handle)" : "");
    else if (s->pending_undo)
        LOG(s, "dsp: <- undo: %s: status %#x (counted; the undoing goes on)", name, s->seq.last_status);
    else
        LOG(s, "dsp: <- %s: %s %#x: undoing what was done", name, s->seq.error == ArSeqErrAnswer ? "not the answer asked for, status" : "status",
            s->seq.last_status);
    return 1;
}

/* ------------------------------------------------------------------------------------------------- the amplifiers' steps */
static int l6_amps_on(void *owner)
{
    AudioSession *s = owner;
    LOG(s, "plan: amplifiers on: not at level 6 (they stay %s)", s->amps_ready ? "configured, muted and powered down" : "held in reset");
    return 0;
}

static int l7_amps_on(void *owner)
{
    AudioSession *s = owner;
    const int r = audio_amps_start(&s->amps, s->cfg.tone_gain);
    const int db10 = fs19xx_gain_db10(s->amps.amp[0].gain);
    if (r) LOG(s, "plan: amplifiers on FAILED (%s): all four muted and powered down again", audio_amps_error(r));
    else LOG(s, "plan: amplifiers on: all four powered up MUTED, gain %u (-%d.%d dB), 48 kHz from the 6.144 MHz bit clock", s->amps.amp[0].gain,
             -db10 / 10, -db10 % 10);
    return r;
}

static int amps_off_step(void *owner)
{
    AudioSession *s = owner;
    if (!s->amps.present) return 0;
    const unsigned bad = audio_amps_off(&s->amps);
    LOG(s, "plan: amplifiers off: all four muted and powered down%s", bad ? " (a bus error on some: every write was tried)" : "");
    return bad ? -1 : 0;
}

/* Level 5, in one go (the bus only: no DSP traffic). */
static void level5(AudioSession *s)
{
    LOG(s, "level 5: the amplifiers: resets held 10 ms and released, each probed, configured from the preset, left powered down and muted");
    if (!s->ops.amps.bus) {
        result(s, 5, AudioLevelFailed, "no bus to the amplifiers");
        return;
    }
    audio_amps_init(&s->amps, &s->ops.amps);
    unsigned present = audio_amps_probe(&s->amps);
    if (!present && s->ops.bus_swap && s->ops.bus_swap(s->ops.ctx)) {
        LOG(s, "level 5: no amplifier answered: SDA and SCL swapped, the resets cycled and the four probed again");
        present = audio_amps_probe(&s->amps);
    }
    for (unsigned i = 0; i < AUDIO_AMPS; ++i) {
        const Fs19xx *a = &s->amps.amp[i];
        if (present >> i & 1u) LOG(s, "amp %s %#x (slot %u): DEVID 0x%04x REVID 0x%02x", audio_amp_name(i), a->address, i, a->devid, a->revid);
        else LOG(s, "amp %s %#x (slot %u): %s", audio_amp_name(i), a->address, i,
                 s->amps.probe[i] == Fs19xxErrId ? "answered, but not an FS19xx (DEVID high byte not 0x29)" : "no answer within 100 ms");
    }
    if (!s->ops.preset) LOG(s, "preset: none: %s", s->ops.preset_why ? s->ops.preset_why : "not read");
    const int r = audio_amps_configure(&s->amps, s->ops.preset, s->ops.preset_bytes, s->cfg.preset_any);
    if (s->ops.preset)
        LOG(s, "preset: %u bytes, %s; container %s; %s", s->ops.preset_bytes,
            s->amps.stock ? "the stock fs19xx.fsm (OS3.0.303.0, SHA-256 as Ubuntu publishes it)" : "NOT the stock file",
            fsm_error(s->amps.preset_fsm), audio_preset_error(s->amps.preset));
    for (unsigned i = 0; i < AUDIO_AMPS; ++i) {
        const Fs19xx *a = &s->amps.amp[i];
        if (s->amps.config[i] == 1) continue;   /* not tried */
        LOG(s, "amp %s: %s: %u writes, %u reads, %u changed by the safety filter, %u unknown units, %u waits missed, %u OTP key writes",
            audio_amp_name(i), s->amps.config[i] ? "configuring FAILED" : "configured", a->writes, a->reads, a->filtered, a->skipped,
            a->waits_missed, a->otp_unlocks);
    }
    if (r) {
        LOG(s, "level 5 FAILED: %s; all four held in reset", audio_amps_error(r));
        result(s, 5, AudioLevelFailed, audio_amps_error(r));
        return;
    }
    const unsigned failed = audio_amps_read(&s->amps);
    for (unsigned i = 0; i < AUDIO_AMPS; ++i) {
        const Fs19xxStatus *st = &s->amps.status[i];
        char names[80];
        fs19xx_status_names(st->status, names, sizeof names);
        LOG(s, "amp %s: STATUS 0x%04x (%s) PWRCTRL 0x%04x SYSCTRL 0x%04x VOLCTRL 0x%04x DACCTRL 0x%04x: %s", audio_amp_name(i), st->status,
            names[0] ? names : "none", st->pwrctrl, st->sysctrl, st->volctrl, st->dacctrl,
            !st->ok ? "NO ANSWER" : fs19xx_status_down(st) ? "powered down" : "NOT powered down");
    }
    if (failed || !audio_amps_all_down(&s->amps)) {
        audio_amps_hold(&s->amps);
        LOG(s, "level 5 FAILED: %s; all four held in reset", failed ? "a status read failed" : "an amplifier is not powered down");
        result(s, 5, AudioLevelFailed, failed ? "a status read failed" : "an amplifier is not powered down after configuring");
        return;
    }
    s->amps_ready = 1;
    result(s, 5, AudioLevelPassed, "four FS19xx answered, configured from the preset, powered down and muted");
    if (s->cfg.level == 5) LOG(s, "level 5: done; the amplifiers are held in reset again at the end");
}

/* ----------------------------------------------------------------------------------------------------------- the stream */
static void stream_stop(AudioSession *s, int reason, uint32_t now);

static void fill_period(AudioSession *s, uint32_t slot)
{
    int16_t *q = s->quad;
    if (s->tone_stream && s->unmuted && !s->walk.done) {
        s->tone_frames += audio_walk_fill(&s->walk, q, AUDIO_PERIOD_FRAMES);
    } else {
        for (uint32_t i = 0; i < AUDIO_PERIOD_FRAMES * 4u; ++i) q[i] = 0;
        if (s->tone_stream && s->walk.done) s->tail++;
    }
    audio_chain_run(&s->chain, q, AUDIO_PERIOD_FRAMES);
    volatile uint64_t *d = (volatile uint64_t *)(void *)(s->ops.dma + AUDIO_DMA_SPEAKER + slot * AUDIO_SPK_PERIOD_BYTES);
    for (uint32_t i = 0; i < AUDIO_PERIOD_FRAMES; ++i) {
        const int16_t *f = q + 4u * i;
        for (unsigned c = 0; c < 4; ++c) {
            const int32_t v = f[c] < 0 ? -(int32_t)f[c] : f[c];
            if (v > s->peak) s->peak = v;
        }
        d[i] = (uint64_t)(uint16_t)f[0] | (uint64_t)(uint16_t)f[1] << 16 | (uint64_t)(uint16_t)f[2] << 32 | (uint64_t)(uint16_t)f[3] << 48;
    }
    if (s->ops.publish) s->ops.publish(s->ops.ctx);
}

static void stream_begin(AudioSession *s, int tone, uint32_t now)
{
    uint64_t base = 0;
    (void)ar_dsp_address(s->speaker.buffer_phys, s->speaker.stream_id, &base);
    s->streaming = 1;
    s->stopping = s->ending = s->unmuted = s->dsp_error = s->stream_unsent = s->have_last = 0;
    s->tone_stream = tone;
    s->amp_bad = s->tail = 0;
    s->stream_at = s->next_status = now;
    s->tone_frames = s->periods_sent = s->stop_reason = 0;
    s->peak = 0;
    if (ar_pump_init(&s->pump, base, s->seq.handle, s->ports->data, AUDIO_SPK_SLOTS, AUDIO_SPK_PERIOD_BYTES, AUDIO_SPK_DEPTH, AUDIO_LATE_MS)) {
        LOG(s, "stream: the period pump refused its geometry");
        stream_stop(s, AudioGuardDsp, now);
        return;
    }
    s->pump_live = 1;
    (void)audio_chain_init(&s->chain, 4);
    if (tone) (void)audio_walk_init(&s->walk, s->cfg.tone_mask, s->cfg.tone_ms, s->cfg.gap_ms, AUDIO_TEST_PEAK);   /* checked at init */
    const uint32_t limit = tone ? AUDIO_WATCHDOG_MS : s->cfg.silence_ms + 2000u;
    audio_guard_start(&s->sguard, now, limit);
    LOG(s, "stream: %s: 10 ms periods of %u bytes, %u slots, %u in flight, WRITE from port %#x, buffer %#llx (DSP address %#llx), handle "
        "%#x, watchdog %u ms", tone ? "the tone walk (silence first, the amplifiers muted)" : "silence (the amplifiers stay down)",
        AUDIO_SPK_PERIOD_BYTES, AUDIO_SPK_SLOTS, AUDIO_SPK_DEPTH, s->ports->data, (unsigned long long)s->speaker.buffer_phys,
        (unsigned long long)base, s->seq.handle, limit);
}

/* Every 100 ms: the amplifiers' status. Level 6: they must all stay down; level 7, once unmuted: the group check. */
static void status_sample(AudioSession *s, uint32_t now)
{
    if (!s->amps.present) return;
    const unsigned failed = audio_amps_read(&s->amps);
    for (unsigned i = 0; i < AUDIO_AMPS; ++i) {
        if (!(s->amps.present >> i & 1u)) continue;
        const Fs19xxStatus *st = &s->amps.status[i];
        const uint16_t bits = st->status & FS19XX_STATUS_BITS;
        if (st->ok && s->have_last && bits == s->last_status[i]) continue;
        char names[80];
        fs19xx_status_names(bits, names, sizeof names);
        LOG(s, "amp %s: %s STATUS 0x%04x (%s) PWRCTRL 0x%04x DIGSTAT 0x%04x at +%u ms", audio_amp_name(i), s->have_last ? "now" : "first",
            st->status, st->ok ? (names[0] ? names : "none") : "no answer", st->pwrctrl, st->digstat, now - s->stream_at);
        s->last_status[i] = bits;
    }
    s->have_last = 1;
    if (failed) {
        s->amp_bad |= failed;
        LOG(s, "amp: a status read failed (slots %#x)", failed);
    }
    if (!s->tone_stream) {
        if (!audio_amps_all_down(&s->amps)) {
            s->amp_bad |= 0xfu;
            LOG(s, "amp: an amplifier is NOT powered down while level 6 runs");
        }
    } else if (s->unmuted) {
        const unsigned bad = audio_amps_check(&s->amps);
        if (bad) {
            s->amp_bad |= bad;
            LOG(s, "amp: the group check flags slots %#x (not running, a STATUS that differs from the others or from its first reading, "
                "or a gain above the cap)", bad);
        }
    }
}

static void stream_stop(AudioSession *s, int reason, uint32_t now)
{
    if (s->stopping) return;
    s->stopping = 1;
    s->stop_reason = (uint32_t)reason;
    const unsigned level = level_of(s->phase);
    if (reason) {
        if (s->amps.started) {
            const unsigned bad = audio_amps_off(&s->amps);
            LOG(s, "stream: the amplifiers muted and powered down at once%s", bad ? " (a bus error on some: every write was tried)" : "");
        }
        dma_zero(s, AUDIO_DMA_SPEAKER, AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES);
        audio_guard_trip(&s->sguard, reason);
        LOG(s, "stream: STOPPED: %s after %u ms (%u periods sent, %u returned, %u late, %u errors, last status %#x): undoing the plan",
            audio_guard_reason(reason), now - s->stream_at, s->pump.sent, s->pump.returned, s->pump.late, s->pump.errors, s->pump.last_status);
        s->failed_step[level] = "the stream";
        s->failed_status[level] = s->pump.last_status;
        result(s, level, AudioLevelFailed, audio_guard_reason(reason));
        ar_seq_abort(&s->seq, ArSeqErrAbort, 0);
    } else {
        LOG(s, "stream: done after %u ms: %u periods sent, %u returned, %u late, %u errors; %u frames of tone, peak %d (ceiling %d): "
            "undoing the plan, the amplifiers first", now - s->stream_at, s->pump.sent, s->pump.returned, s->pump.late, s->pump.errors,
            s->tone_frames, s->peak, s->chain.ceiling);
        ar_seq_stop(&s->seq);
    }
    enter(s, s->phase == PhToneStream || s->phase == PhTone ? PhToneDown : PhSpeakerDown, now);
}

static int ending(const AudioSession *s, uint32_t now)
{
    return s->tone_stream ? s->walk.done && s->tail >= 2 : now - s->stream_at >= s->cfg.silence_ms;
}

static void stream_tick(AudioSession *s, uint32_t now)
{
    if (s->stopping) return;
    const int late = ar_pump_check(&s->pump, now);
    int r = audio_guard_check(&s->sguard, now, s->dsp_error, late, s->amp_bad);
    if (!r && s->held) r = AudioGuardQuiet;
    if (r) {stream_stop(s, r, now); return;}
    if ((int32_t)(now - s->next_status) >= 0) {
        s->next_status = now + AUDIO_STATUS_MS;
        status_sample(s, now);
        if (s->amp_bad) {stream_stop(s, AudioGuardAmplifier, now); return;}
    }
    if (s->tone_stream && !s->unmuted && s->pump.returned >= AUDIO_UNMUTE_PERIODS) {
        if (audio_amps_unmute(&s->amps)) {
            LOG(s, "stream: unmuting failed: the amplifiers muted and powered down again");
            stream_stop(s, AudioGuardAmplifier, now);
            return;
        }
        s->unmuted = 1;
        s->unmuted_at = now;
        s->next_status = now + AUDIO_STATUS_MS;
        LOG(s, "stream: %u silent periods returned: the amplifiers unmuted; the walk starts (slots %#x, %u ms each, %u ms apart, "
            "-20 dBFS)", s->pump.returned, s->cfg.tone_mask, s->cfg.tone_ms, s->cfg.gap_ms);
    }
    if (s->stream_unsent) {
        if (s->ops.send(s->ops.ctx, s->spkt, s->stream_len)) {s->queue_full++; return;}
        s->stream_unsent = 0;
        s->periods_sent++;
        s->sends++;
    }
    s->ending = ending(s, now);
    while (!s->ending) {
        const int slot = ar_pump_free_slot(&s->pump);
        if (slot < 0) break;
        fill_period(s, (uint32_t)slot);
        s->stream_len = ar_pump_write(&s->pump, s->spkt, sizeof s->spkt, now);
        if (!s->stream_len) {s->dsp_error = 1; break;}
        if (s->ops.send(s->ops.ctx, s->spkt, s->stream_len)) {s->stream_unsent = 1; s->queue_full++; break;}
        s->periods_sent++;
        s->sends++;
        s->ending = ending(s, now);
    }
    if (s->ending && !s->stream_unsent && !s->pump.in_flight) stream_stop(s, 0, now);
}

/* ------------------------------------------------------------------------------------------------------ the phases */
static int speaker_prepare(AudioSession *s)
{
    LOG(s, "level 6: the TDM pins, the DSP's PCM streams, the buffers in the audio slice, then the speaker plan with the amplifiers "
        "left down and a silent stream");
    if (!s->ops.dma || s->ops.dma_bytes < AUDIO_DMA_BYTES || (s->ops.dma_phys & 0xfffu) || s->ops.dma_phys + AUDIO_DMA_BYTES > 0x100000000ull) {
        result(s, 6, AudioLevelFailed, "no DSP-addressable buffer (the audio slice is not mapped)");
        return 0;
    }
    if (s->ops.tdm_pins && !s->pins_on) {
        if (s->ops.tdm_pins(s->ops.ctx, 1)) {
            result(s, 6, AudioLevelFailed, "the TDM pins could not be muxed");
            return 0;
        }
        s->pins_on = 1;
    }
    if (!s->smmu_done) {
        if (s->ops.smmu && s->ops.smmu(s->ops.ctx)) {
            result(s, 6, AudioLevelFailed, "the DSP's PCM stream 0x1001 is not through bypass");
            return 0;
        }
        s->smmu_done = 1;
    }
    dma_zero(s, 0, AUDIO_DMA_BYTES);
    ar_speaker_defaults(&s->speaker);
    s->speaker.buffer_phys = s->ops.dma_phys + AUDIO_DMA_SPEAKER;
    s->speaker.buffer_bytes = AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES;
    s->speaker.owner = s;
    s->speaker.amps_off = amps_off_step;
    ar_mic_defaults(&s->mic);
    s->mic.buffer_phys = s->ops.dma_phys + AUDIO_DMA_MIC;
    s->mic.buffer_bytes = AUDIO_MIC_SLOTS * AUDIO_MIC_PERIOD_BYTES;
    return 1;
}

static void finish(AudioSession *s, uint32_t now)
{
    if (s->amps.io.bus && (s->amps.present || s->amps.released)) {
        audio_amps_hold(&s->amps);
        LOG(s, "the amplifiers muted, powered down and held in reset");
    }
    if (s->smmu_done) dma_zero(s, 0, AUDIO_DMA_BYTES);
    if (s->pins_on && s->ops.tdm_pins) {
        (void)s->ops.tdm_pins(s->ops.ctx, 0);
        s->pins_on = 0;
    }
    s->streaming = s->pump_live = s->seq_active = 0;
    for (unsigned l = 4; l <= s->cfg.level && l <= 7; ++l) {
        if (s->failed_step[l])
            LOG(s, "level %u: %s: %s (at %s, status %#x)", l, audio_level_result(s->result[l]), s->why[l] ? s->why[l] : "", s->failed_step[l],
                s->failed_status[l]);
        else
            LOG(s, "level %u: %s%s%s", l, audio_level_result(s->result[l]), s->why[l] ? ": " : "", s->why[l] ? s->why[l] : "");
    }
    LOG(s, "levels done in %u ms: %u packets sent, %u received, %u ignored, %u unanswered, %u times the queue was full", now - s->started_at,
        s->sends, s->received, s->stray, s->timeouts, s->queue_full);
    s->finished = 1;
    s->phase = PhDone;
}

static void begin(AudioSession *s, int phase, uint32_t now)
{
    unsigned count, prepared;
    for (;;) {
        switch (phase) {
        case PhClock:
            if (!s->cfg.dsp) {
                result(s, 4, AudioLevelSkipped, "needs the DSP link (piano-power=2, level 2's APM ready)");
                phase = PhAmps;
                continue;
            }
            LOG(s, "level 4: the LPASS clocks are the DSP's: requested and released through its resource manager (PRM, port %#x)", s->ports->prm);
            {
                const ArStep *steps = ar_clock_steps(&count);
                plan_start(s, "TDM bit clock", steps, count, 0x1000, 0, 4, now);
            }
            enter(s, PhClock, now);
            return;
        case PhVa: {
            const ArStep *steps = ar_va_steps(&count);
            plan_start(s, "VA macro clocks", steps, count, 0x2000, 0, 4, now);
            enter(s, PhVa, now);
            return;
        }
        case PhAmps:
            if (s->cfg.level < 5) {phase = PhEnd; continue;}
            enter(s, PhAmps, now);
            level5(s);
            phase = PhSpeaker;
            continue;
        case PhSpeaker:
            if (s->cfg.level < 6) {phase = PhEnd; continue;}
            if (!s->cfg.dsp) {skip_from(s, 6, "needs the DSP link (piano-power=2, level 2's APM ready)"); phase = PhEnd; continue;}
            if (s->result[4] != AudioLevelPassed) {skip_from(s, 6, "level 4 did not pass (no DSP command after a failed one)"); phase = PhEnd; continue;}
            if (!speaker_prepare(s)) {skip_from(s, 7, "level 6 did not pass"); phase = PhEnd; continue;}
            s->speaker.amps_on = l6_amps_on;
            {
                const ArStep *steps = ar_speaker_steps(&count);
                plan_start(s, "speaker (level 6: the amplifiers stay down)", steps, count, 0x3000, &s->speaker, 6, now);
            }
            enter(s, PhSpeaker, now);
            return;
        case PhMic:
            if (!s->cfg.mic) {phase = PhTone; continue;}
            LOG(s, "level 6: the microphone graphs: the VA clocks, open, configure, map, prepare; then all undone (no capture yet)");
            {
                const ArStep *steps = ar_mic_steps(&count, &prepared);
                plan_start(s, "microphone (level 6: open and prepare)", steps, prepared, 0x6000, &s->mic, 6, now);
            }
            enter(s, PhMic, now);
            return;
        case PhTone:
            if (s->cfg.level < 7) {phase = PhEnd; continue;}
            if (!s->cfg.tone) {result(s, 7, AudioLevelSkipped, "piano-audio-tone=1 not given: no sound"); phase = PhEnd; continue;}
            if (!s->speaker_ok) {result(s, 7, AudioLevelSkipped, "level 6's speaker plan did not pass"); phase = PhEnd; continue;}
            if (!s->amps_ready) {result(s, 7, AudioLevelSkipped, "the amplifiers are not configured (level 5 did not pass)"); phase = PhEnd; continue;}
            LOG(s, "level 7: the tone walk: the speaker plan with the amplifiers started muted at gain %u, unmuted after %u silent periods",
                s->cfg.tone_gain, AUDIO_UNMUTE_PERIODS);
            s->speaker.amps_on = l7_amps_on;
            dma_zero(s, AUDIO_DMA_SPEAKER, AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES);
            {
                const ArStep *steps = ar_speaker_steps(&count);
                plan_start(s, "speaker (level 7: the tone walk)", steps, count, 0x7000, &s->speaker, 7, now);
            }
            enter(s, PhTone, now);
            return;
        case PhEnd:
        default:
            finish(s, now);
            return;
        }
    }
}

/* A plan's phase: send what comes next; once the plan settles, act on where it got to. */
static void plan_phase(AudioSession *s, uint32_t now)
{
    plan_send(s, now);
    if (!settled(s)) return;
    const int up = s->seq.state == ArSeqUp, down = s->seq.state == ArSeqDown && !s->seq.undo_errors;
    switch (s->phase) {
    case PhClock:
        if (up) {LOG(s, "level 4: the TDM bit clock is granted (6.144 MHz); releasing it"); ar_seq_stop(&s->seq); return;}
        if (!down) {
            fail_step(s, 4, s->seq.state == ArSeqFailed ? "the bit clock request failed" : "the bit clock's release failed");
            begin(s, PhAmps, now);
            return;
        }
        begin(s, PhVa, now);
        return;
    case PhVa:
        if (up) {
            if (s->cfg.lpass_read && s->ops.lpass_read) {
                LOG(s, "level 4: both votes and the VA mclk granted: the VA macro's and the LPI DMIC pins' registers (loads only)");
                s->ops.lpass_read(s->ops.ctx);
            }
            ar_seq_stop(&s->seq);
            return;
        }
        if (!down) fail_step(s, 4, s->seq.state == ArSeqFailed ? "a VA clock request failed" : "a VA clock release failed");
        else result(s, 4, AudioLevelPassed, "the bit clock, both votes and the VA mclk granted and released");
        begin(s, PhAmps, now);
        return;
    case PhSpeaker:
        if (up) {stream_begin(s, 0, now); if (!s->stopping) enter(s, PhSpeakerStream, now); return;}
        fail_step(s, 6, "a speaker plan step failed");
        skip_from(s, 7, "level 6 did not pass");
        begin(s, PhEnd, now);
        return;
    case PhSpeakerDown:
        s->streaming = s->pump_live = 0;
        if (down && !s->stop_reason) {
            s->speaker_ok = 1;
            result(s, 6, AudioLevelPassed, s->cfg.mic ? "the speaker plan up and down, a silent stream, the mic graphs opened and closed"
                                                      : "the speaker plan up and down, a silent stream");
            begin(s, PhMic, now);
            return;
        }
        if (!s->stop_reason) fail_step(s, 6, "undoing the speaker plan failed");
        skip_from(s, 7, "level 6 did not pass");
        begin(s, PhEnd, now);
        return;
    case PhMic:
        if (up) {LOG(s, "level 6: the microphone graphs are open and prepared; closing them"); ar_seq_stop(&s->seq); return;}
        if (!down) {
            fail_step(s, 6, s->seq.state == ArSeqFailed ? "a microphone plan step failed" : "undoing the microphone plan failed");
            skip_from(s, 7, "level 6 did not pass");
            begin(s, PhEnd, now);
            return;
        }
        begin(s, PhTone, now);
        return;
    case PhTone:
        if (up) {stream_begin(s, 1, now); if (!s->stopping) enter(s, PhToneStream, now); return;}
        fail_step(s, 7, "a speaker plan step failed");
        begin(s, PhEnd, now);
        return;
    case PhToneDown:
        s->streaming = s->pump_live = 0;
        if (down && !s->stop_reason) result(s, 7, AudioLevelPassed, "the walk played through; the amplifiers down; the plan undone");
        else if (!s->stop_reason) fail_step(s, 7, "undoing the speaker plan failed");
        begin(s, PhEnd, now);
        return;
    case PhAbort:
        begin(s, PhEnd, now);
        return;
    default:
        return;
    }
}

/* PianoPower's health turned the guard off: nothing goes to the DSP any more; the amplifiers down and held now. */
static void go_off(AudioSession *s, uint32_t now)
{
    s->off = 1;
    LOG(s, "dsp: guard OFF: %s: no DSP command goes out any more in this boot; the amplifiers muted, powered down and held in reset",
        adg_reason(s->guard ? s->guard->reason : AdgReasonOwner));
    if (s->amps.io.bus && (s->amps.present || s->amps.released)) audio_amps_hold(&s->amps);
    if (s->smmu_done) dma_zero(s, 0, AUDIO_DMA_BYTES);
    if (s->seq_active) {
        ar_seq_abort(&s->seq, ArSeqErrDspDown, 1);
        (void)ar_seq_next(&s->seq, s->pkt, sizeof s->pkt);   /* without the DSP: only the local undos run */
    }
    s->in_flight = 0;
    s->unsent = 0;
    const unsigned level = level_of(s->phase);
    if (level) result(s, level, AudioLevelFailed, "the DSP link went off (PianoPower's health)");
    skip_from(s, level ? level + 1 : 4, "the DSP link went off");
    begin(s, PhEnd, now);
}

void audio_session_init(AudioSession *s, const AudioSessionConfig *c, const AudioSessionOps *ops, AudioDspGuard *guard)
{
    zero(s, sizeof *s);
    s->cfg = *c;
    s->ops = *ops;
    s->guard = guard;
    if (s->cfg.level < 4) s->cfg.level = 4;
    if (s->cfg.level > 7) s->cfg.level = 7;
    s->ports = audio_ports(s->cfg.ports);
    if (!s->ports) {s->cfg.ports = AudioPortsUbuntu; s->ports = audio_ports(AudioPortsUbuntu);}
    if (s->cfg.silence_ms > AUDIO_SILENCE_MAX_MS) s->cfg.silence_ms = AUDIO_SILENCE_MAX_MS;
    if (s->cfg.tone_gain > AUDIO_AMPS_TEST_GAIN) s->cfg.tone_gain = AUDIO_AMPS_TEST_GAIN;
    if (!s->cfg.answer_ms) s->cfg.answer_ms = AUDIO_ANSWER_MS;
    if (!guard) s->cfg.dsp = 0;
    AudioWalk w;
    if (s->cfg.tone && audio_walk_init(&w, s->cfg.tone_mask, s->cfg.tone_ms, s->cfg.gap_ms, AUDIO_TEST_PEAK)) s->cfg.tone = -1;   /* refused */
    s->phase = PhBegin;
}

uint32_t audio_session_tick(AudioSession *s, const AdgObservation *o, uint32_t now_ms)
{
    if (s->finished) return 0;
    if (s->phase == PhBegin) {
        s->started_at = now_ms;
        LOG(s, "levels 4-%u: ports %s (APM %#x, PRM %#x, data %#x); %s; preset %s; tone %s", s->cfg.level, s->ports->name, s->ports->apm,
            s->ports->prm, s->ports->data, s->cfg.dsp ? "the DSP link is up" : "no DSP link (level 5 only)",
            s->cfg.preset_any ? "any valid one" : "the stock file only",
            s->cfg.tone == 1 ? "on (piano-audio-tone=1)" : s->cfg.tone < 0 ? "REFUSED (the walk's limits: <= 3 s of tone, <= 4 s in all)" : "off");
        if (s->cfg.tone < 0) s->cfg.tone = 0;
        begin(s, PhClock, now_ms);
        if (s->finished) return 0;
    }
    if (s->cfg.dsp && !s->off) {
        const int g = o ? adg_observe(s->guard, o, now_ms) : s->guard->state;
        if (g == AdgOff) {go_off(s, now_ms); return 0;}
        s->held = g == AdgWait;
        if (s->held != s->was_held)
            LOG(s, s->held ? "dsp: PianoPower asks for quiet: nothing new goes to the DSP until it ends" : "dsp: quiet over: commands go out again");
        s->was_held = s->held;
    }
    switch (s->phase) {
    case PhSpeakerStream:
    case PhToneStream:
        plan_send(s, now_ms);
        stream_tick(s, now_ms);
        if (s->phase != PhSpeakerStream && s->phase != PhToneStream) plan_send(s, now_ms);   /* the undo starts at once */
        break;
    case PhAmps:
    case PhEnd:
    case PhDone:
    case PhBegin:
        break;
    default:
        plan_phase(s, now_ms);
        break;
    }
    if (s->finished) return 0;
    return s->streaming && !s->stopping ? 2u : 10u;
}

void audio_session_receive(AudioSession *s, const uint8_t *packet, uint32_t length, uint32_t now_ms)
{
    if (s->finished || s->off) return;
    s->received++;
    ArAnswer a;
    const int kind = ar_parse(packet, length, &a);
    if (kind == ArWriteDone && s->pump_live) {
        const int r = ar_pump_done(&s->pump, &a);
        if (r == 1) return;
        if (r == -1) {
            if (!s->stopping) {
                LOG(s, "dsp: WRITE_DONE refused: status %#x for %#llx (token %#x)", a.status, (unsigned long long)a.address, a.token);
                s->dsp_error = 1;
            }
            return;
        }
    }
    if (plan_receive(s, packet, length, now_ms)) return;
    if (kind == ArWriteDone && s->stopping) return;   /* periods the DSP gives back while the stream is undone */
    if (++s->stray <= 8)
        LOG(s, "dsp: a packet nobody waits for: opcode %#x token %#x to port %#x%s", a.opcode, a.token, a.dst_port, kind == ArBad ? " (malformed)" : "");
}

void audio_session_abort(AudioSession *s, int hard, uint32_t now_ms)
{
    if (s->finished) return;
    LOG(s, "stopped by the owner%s: the amplifiers muted and powered down, the plan undone", hard ? " (without DSP commands)" : "");
    if (s->amps.io.bus && s->amps.present) (void)audio_amps_off(&s->amps);
    if (s->smmu_done) dma_zero(s, 0, AUDIO_DMA_BYTES);
    const unsigned level = level_of(s->phase);
    if (level) result(s, level, AudioLevelFailed, "stopped by the owner");
    skip_from(s, level ? level + 1 : 4, "stopped by the owner");
    s->streaming = 0;
    if (hard || s->off || !s->seq_active || s->phase == PhBegin) {
        if (s->seq_active) {
            ar_seq_abort(&s->seq, ArSeqErrDspDown, 1);
            (void)ar_seq_next(&s->seq, s->pkt, sizeof s->pkt);
        }
        s->in_flight = 0;
        s->unsent = 0;
        begin(s, PhEnd, now_ms);
        return;
    }
    s->stopping = 1;
    ar_seq_abort(&s->seq, ArSeqErrAbort, 0);
    enter(s, PhAbort, now_ms);
}
