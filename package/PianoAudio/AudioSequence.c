/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioSequence.h"
#include "PianoGpr.h"

#define UNDO_TOKEN 0x100u

void ar_seq_init(ArSeq *s, const ArStep *steps, unsigned count, uint32_t port, uint32_t prm_port, uint32_t token_base, void *context)
{
    for (unsigned i = 0; i < sizeof *s; ++i) ((uint8_t *)s)[i] = 0;
    s->steps = steps; s->count = count; s->port = port; s->prm_port = prm_port; s->token_base = token_base; s->context = context;
    s->state = ArSeqIdle; s->failed_step = -1; s->timed_out_step = -1;
}

const char *ar_seq_step_name(const ArSeq *s, int step)
{
    return step >= 0 && (unsigned)step < s->count && s->steps[step].name ? s->steps[step].name : "none";
}

/* Undo the steps done (and the one that got no answer: the DSP may have run it), last first. */
static void unwind(ArSeq *s, int error)
{
    if (!s->error) s->error = error;
    s->undo_next = s->done + (s->timed_out_step >= 0 && (unsigned)s->timed_out_step == s->done ? 1u : 0u);
    if (s->undo_next > s->count) s->undo_next = s->count;
    s->state = ArSeqUnwinding;
}

static void fail(ArSeq *s, int error)
{
    if (s->failed_step < 0) s->failed_step = (int)s->done;
    unwind(s, error);
}

static uint32_t send(ArSeq *s, uint32_t n, uint32_t token, uint32_t opcode, int answer, int prm, int undo)
{
    s->pending_token = token; s->pending_opcode = opcode; s->pending_answer = answer; s->pending_undo = undo;
    s->pending_port = prm ? s->prm_port : s->port;
    s->state = ArSeqWaiting;
    return n;
}

uint32_t ar_seq_next(ArSeq *s, uint8_t *out, uint32_t capacity)
{
    for (;;) {
        if (s->state == ArSeqIdle) {
            if (s->done == s->count) {s->state = ArSeqUp; return 0;}
            const ArStep *st = &s->steps[s->done];
            if (!st->build) {
                if (st->local) {
                    s->locals++;
                    /* a local action that failed may have half run (two of four amplifiers up): it is undone too */
                    if (st->local(s)) {s->timed_out_step = (int)s->done; fail(s, ArSeqErrLocal); continue;}
                }
                s->done++;
                continue;
            }
            const uint32_t token = s->token_base + s->done;
            const uint32_t n = st->build(s, out, capacity, token);
            if (!n) {fail(s, ArSeqErrBuild); continue;}
            return send(s, n, token, st->opcode, st->answer, st->prm, 0);
        }
        if (s->state == ArSeqUnwinding) {
            if (!s->undo_next) {s->state = s->error ? ArSeqFailed : ArSeqDown; return 0;}
            const unsigned i = --s->undo_next;
            const ArStep *st = &s->steps[i];
            if (st->undo_local) {
                s->locals++;
                if (st->undo_local(s)) s->undo_errors++;
                continue;
            }
            if (!st->undo || s->dsp_down) continue;
            const uint32_t token = s->token_base + UNDO_TOKEN + i;
            const uint32_t n = st->undo(s, out, capacity, token);
            if (!n) continue;   /* nothing to undo (e.g. no handle was ever returned) */
            return send(s, n, token, st->undo_opcode, st->undo_answer, st->prm, 1);
        }
        return 0;
    }
}

int ar_seq_receive(ArSeq *s, const uint8_t *in, uint32_t length)
{
    ArAnswer a;
    const int kind = ar_parse(in, length, &a);
    if (s->state != ArSeqWaiting || kind <= 0 || a.dst_port != s->pending_port || a.src_domain != GPR_DOMAIN_ADSP ||
        a.token != s->pending_token) {
        s->ignored++;
        return ArSeqIgnored;
    }
    uint32_t status;
    int error = ArSeqOk;
    if (kind == ArBasicResult) {
        if (a.result_opcode != s->pending_opcode) {s->ignored++; return ArSeqIgnored;}
        status = a.status;
        if (!status && s->pending_answer != ArBasicResult) error = ArSeqErrAnswer;   /* "done" without the handle or state asked for */
    } else if (kind != s->pending_answer) {
        s->ignored++;
        return ArSeqIgnored;
    } else if (kind == ArMapped) {
        status = 0;
        if (!a.handle) error = ArSeqErrAnswer;
    } else if (kind == ArFrameworkState) {
        status = a.status;
        if (status != 1) error = ArSeqErrAnswer;   /* not ready */
        else status = 0;
    } else {
        status = a.status;
    }
    if (status && !error) error = ArSeqErrStatus;
    s->answers++;
    s->last_status = kind == ArFrameworkState ? a.status : status;
    s->last_opcode = s->pending_opcode;
    if (s->pending_undo) {
        if (error) s->undo_errors++;
        s->state = ArSeqUnwinding;
        return error ? ArSeqError : ArSeqAdvanced;
    }
    if (error) {s->error_status = s->last_status; fail(s, error); return ArSeqError;}
    if (kind == ArMapped) s->handle = a.handle;
    s->done++;
    s->state = ArSeqIdle;
    return ArSeqAdvanced;
}

void ar_seq_timeout(ArSeq *s)
{
    if (s->state != ArSeqWaiting) return;
    s->timeouts++;
    if (s->pending_undo) {s->undo_errors++; s->state = ArSeqUnwinding; return;}
    s->timed_out_step = (int)s->done;
    fail(s, ArSeqErrTimeout);
}

void ar_seq_abort(ArSeq *s, int error, int dsp_down)
{
    if (s->state == ArSeqDown || s->state == ArSeqFailed) return;
    if (dsp_down) s->dsp_down = 1;
    if (!error) error = ArSeqErrAbort;
    if (s->state == ArSeqUnwinding || (s->state == ArSeqWaiting && s->pending_undo)) {
        if (!s->error) s->error = error;
        if (s->state == ArSeqWaiting && dsp_down) s->state = ArSeqUnwinding;   /* no answer will come */
        return;
    }
    if (s->state == ArSeqWaiting) s->timed_out_step = (int)s->done;   /* in flight: it may have run */
    if (s->state == ArSeqUp) {s->failed_step = -1; unwind(s, error); return;}
    fail(s, error);
}

void ar_seq_stop(ArSeq *s)
{
    if (s->state != ArSeqUp) {ar_seq_abort(s, ArSeqErrAbort, 0); return;}
    unwind(s, ArSeqOk);
}

/* ---------------------------------------------------------------------------------------------------- piano's speaker plan */
static const ArSpeakerPlan *plan(const ArSeq *s) {return (const ArSpeakerPlan *)s->context;}
static uint32_t clock_on(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_prm_clock(o, c, s->prm_port, t, AR_CLK_SEC_TDM_IBIT, AR_TDM_BIT_CLOCK);}
static uint32_t clock_off(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_prm_clock(o, c, s->prm_port, t, AR_CLK_SEC_TDM_IBIT, 0);}
static uint32_t open_dev(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_open_speaker_device(o, c, s->port, t);}
static uint32_t close_dev(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_CLOSE, AR_SG_SPEAKER);}
static uint32_t dev_log(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_speaker_logger(o, c, s->port, t);}
static uint32_t dev_mfc(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_speaker_mfc(o, c, s->port, t, &plan(s)->slots);}
static uint32_t dev_tdm(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_speaker_tdm(o, c, s->port, t, &plan(s)->tdm, &plan(s)->slots);}
static uint32_t open_str(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_open_playback_stream(o, c, s->port, t);}
static uint32_t close_str(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_CLOSE, AR_SG_PLAY);}
static uint32_t str_shmem(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_playback_shmem(o, c, s->port, t, &plan(s)->stream);}
static uint32_t str_pcm(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_playback_stream(o, c, s->port, t, &plan(s)->stream);}
static uint32_t str_log(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_playback_logger(o, c, s->port, t);}
static uint32_t map(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const ArSpeakerPlan *p = plan(s);
    uint64_t address;
    if (p->buffer_phys % 4096u || ar_dsp_address(p->buffer_phys, p->stream_id, &address)) return 0;   /* not DSP-addressable */
    return ar_map(o, c, s->port, t, address, p->buffer_bytes);
}
static uint32_t unmap(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return s->handle ? ar_unmap(o, c, s->port, t, s->handle) : 0;}
static uint32_t prep_dev(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_PREPARE, AR_SG_SPEAKER);}
static uint32_t prep_str(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_PREPARE, AR_SG_PLAY);}
static uint32_t start_dev(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_START, AR_SG_SPEAKER);}
static uint32_t stop_dev(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_STOP, AR_SG_SPEAKER);}
static uint32_t start_str(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_START, AR_SG_PLAY);}
static uint32_t stop_str(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_STOP, AR_SG_PLAY);}
static int amps_on(const ArSeq *s) {const ArSpeakerPlan *p = plan(s); return p->amps_on ? p->amps_on(p->owner) : -1;}
static int amps_off(const ArSeq *s) {const ArSpeakerPlan *p = plan(s); return p->amps_off ? p->amps_off(p->owner) : -1;}

#define CMD(name, b, op, ans) name, b, op, ans, 0
#define UNDO(b, op, ans) b, op, ans, 0
#define NOUNDO 0, 0, 0, 0
static const ArStep kSpeaker[] = {
    {CMD("tdm bit clock 6.144 MHz", clock_on, AR_OP_PRM_REQUEST, ArPrmDone), UNDO(clock_off, AR_OP_PRM_RELEASE, ArPrmDone), 1},
    {CMD("open speaker device graph", open_dev, AR_OP_GRAPH_OPEN, ArBasicResult), UNDO(close_dev, AR_OP_GRAPH_CLOSE, ArBasicResult), 0},
    {CMD("device logger", dev_log, AR_OP_SET_CFG, ArBasicResult), NOUNDO, 0},
    {CMD("device MFC 4 x 32-bit FL FR LS RS", dev_mfc, AR_OP_SET_CFG, ArBasicResult), NOUNDO, 0},
    {CMD("device TDM interface", dev_tdm, AR_OP_SET_CFG, ArBasicResult), NOUNDO, 0},
    {CMD("open playback stream graph", open_str, AR_OP_GRAPH_OPEN, ArBasicResult), UNDO(close_str, AR_OP_GRAPH_CLOSE, ArBasicResult), 0},
    {CMD("stream shared memory format", str_shmem, AR_OP_SET_CFG, ArBasicResult), NOUNDO, 0},
    {CMD("stream decoder and converter", str_pcm, AR_OP_SET_CFG, ArBasicResult), NOUNDO, 0},
    {CMD("stream logger", str_log, AR_OP_SET_CFG, ArBasicResult), NOUNDO, 0},
    {CMD("map the period buffer", map, AR_OP_MEM_MAP, ArMapped), UNDO(unmap, AR_OP_MEM_UNMAP, ArBasicResult), 0},
    {CMD("prepare device", prep_dev, AR_OP_GRAPH_PREPARE, ArBasicResult), NOUNDO, 0},
    {CMD("prepare stream", prep_str, AR_OP_GRAPH_PREPARE, ArBasicResult), NOUNDO, 0},
    {CMD("start device (bit clock runs)", start_dev, AR_OP_GRAPH_START, ArBasicResult), UNDO(stop_dev, AR_OP_GRAPH_STOP, ArBasicResult), 0},
    {CMD("start stream", start_str, AR_OP_GRAPH_START, ArBasicResult), UNDO(stop_str, AR_OP_GRAPH_STOP, ArBasicResult), 0},
    {"amplifiers on", 0, 0, 0, amps_on, 0, 0, 0, amps_off, 0},
};

const ArStep *ar_speaker_steps(unsigned *count)
{
    *count = sizeof kSpeaker / sizeof kSpeaker[0];
    return kSpeaker;
}

void ar_speaker_defaults(ArSpeakerPlan *p)
{
    for (unsigned i = 0; i < sizeof *p; ++i) ((uint8_t *)p)[i] = 0;
    ar_piano_tdm(&p->tdm);
    p->stream.rate = 48000; p->stream.bits = 16; p->stream.channels = 4;
    p->slots.rate = 48000; p->slots.bits = 32; p->slots.channels = 4;
    p->stream_id = 0x1001;
}

/* ------------------------------------------------------------------------------------------- level 4: the PRM clocks alone */
static const ArStep kClock[] = {
    {CMD("tdm bit clock 6.144 MHz", clock_on, AR_OP_PRM_REQUEST, ArPrmDone), UNDO(clock_off, AR_OP_PRM_RELEASE, ArPrmDone), 1},
};

const ArStep *ar_clock_steps(unsigned *count)
{
    *count = sizeof kClock / sizeof kClock[0];
    return kClock;
}

static uint32_t macro_on(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_prm_vote(o, c, s->prm_port, t, AR_HW_MACRO, 1);}
static uint32_t macro_off(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_prm_vote(o, c, s->prm_port, t, AR_HW_MACRO, 0);}
static uint32_t dcodec_on(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_prm_vote(o, c, s->prm_port, t, AR_HW_DCODEC, 1);}
static uint32_t dcodec_off(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_prm_vote(o, c, s->prm_port, t, AR_HW_DCODEC, 0);}
static uint32_t va_mclk_on(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_prm_clock(o, c, s->prm_port, t, AR_CLK_TX_CORE_MCLK, AR_VA_MCLK_HZ);}
static uint32_t va_mclk_off(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_prm_clock(o, c, s->prm_port, t, AR_CLK_TX_CORE_MCLK, 0);}

#define VA_STEPS \
    {CMD("LPASS macro vote", macro_on, AR_OP_PRM_REQUEST, ArPrmDone), UNDO(macro_off, AR_OP_PRM_RELEASE, ArPrmDone), 1}, \
    {CMD("LPASS digital codec vote", dcodec_on, AR_OP_PRM_REQUEST, ArPrmDone), UNDO(dcodec_off, AR_OP_PRM_RELEASE, ArPrmDone), 1}, \
    {CMD("VA macro mclk 19.2 MHz (TX core mclk)", va_mclk_on, AR_OP_PRM_REQUEST, ArPrmDone), UNDO(va_mclk_off, AR_OP_PRM_RELEASE, ArPrmDone), 1}
static const ArStep kVa[] = {VA_STEPS};

const ArStep *ar_va_steps(unsigned *count)
{
    *count = sizeof kVa / sizeof kVa[0];
    return kVa;
}

/* ------------------------------------------------------------------------------------------------- the microphone plan */
static const ArMicPlan *mic(const ArSeq *s) {return (const ArMicPlan *)s->context;}
static uint32_t open_mic(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_open_mic_device(o, c, s->port, t);}
static uint32_t close_mic(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_CLOSE, AR_SG_MIC);}
static uint32_t mic_dev(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_mic_device(o, c, s->port, t, &mic(s)->device, mic(s)->channel_mask);}
static uint32_t mic_log(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_mic_logger(o, c, s->port, t);}
static uint32_t open_cap(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_open_capture_stream(o, c, s->port, t);}
static uint32_t close_cap(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_CLOSE, AR_SG_CAPTURE);}
static uint32_t cap_cfg(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_capture_stream(o, c, s->port, t, &mic(s)->stream);}
static uint32_t cap_log(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_capture_logger(o, c, s->port, t);}
static uint32_t map_cap(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const ArMicPlan *p = mic(s);
    uint64_t address;
    if (p->buffer_phys % 4096u || ar_dsp_address(p->buffer_phys, p->stream_id, &address)) return 0;   /* not DSP-addressable */
    return ar_map(o, c, s->port, t, address, p->buffer_bytes);
}
static uint32_t prep_mic(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_PREPARE, AR_SG_MIC);}
static uint32_t prep_cap(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_PREPARE, AR_SG_CAPTURE);}
static uint32_t start_mic(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_START, AR_SG_MIC);}
static uint32_t stop_mic(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_STOP, AR_SG_MIC);}
static uint32_t start_cap(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_START, AR_SG_CAPTURE);}
static uint32_t stop_cap(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, AR_OP_GRAPH_STOP, AR_SG_CAPTURE);}

#define MIC_PREPARED 12u
#define MIC_DEVICE_STEPS \
    {CMD("open mic device graph", open_mic, AR_OP_GRAPH_OPEN, ArBasicResult), UNDO(close_mic, AR_OP_GRAPH_CLOSE, ArBasicResult), 0}, \
    {CMD("mic device VA TX0 codec DMA", mic_dev, AR_OP_SET_CFG, ArBasicResult), NOUNDO, 0}, \
    {CMD("mic device logger", mic_log, AR_OP_SET_CFG, ArBasicResult), NOUNDO, 0}, \
    {CMD("open capture stream graph", open_cap, AR_OP_GRAPH_OPEN, ArBasicResult), UNDO(close_cap, AR_OP_GRAPH_CLOSE, ArBasicResult), 0}, \
    {CMD("capture MFC, converter and encoder", cap_cfg, AR_OP_SET_CFG, ArBasicResult), NOUNDO, 0}, \
    {CMD("capture logger", cap_log, AR_OP_SET_CFG, ArBasicResult), NOUNDO, 0}, \
    {CMD("map the capture buffer", map_cap, AR_OP_MEM_MAP, ArMapped), UNDO(unmap, AR_OP_MEM_UNMAP, ArBasicResult), 0}, \
    {CMD("prepare mic device", prep_mic, AR_OP_GRAPH_PREPARE, ArBasicResult), NOUNDO, 0}, \
    {CMD("prepare capture", prep_cap, AR_OP_GRAPH_PREPARE, ArBasicResult), NOUNDO, 0}
#define MIC_START_STEPS \
    {CMD("start mic device", start_mic, AR_OP_GRAPH_START, ArBasicResult), UNDO(stop_mic, AR_OP_GRAPH_STOP, ArBasicResult), 0}, \
    {CMD("start capture", start_cap, AR_OP_GRAPH_START, ArBasicResult), UNDO(stop_cap, AR_OP_GRAPH_STOP, ArBasicResult), 0}
static int va_on(const ArSeq *s) {const ArMicPlan *p = mic(s); return p->va_on ? p->va_on(p->owner) : 0;}
static int va_off(const ArSeq *s) {const ArMicPlan *p = mic(s); return p->va_off ? p->va_off(p->owner) : 0;}
static const ArStep kMicLive[] = {
    VA_STEPS,
    MIC_DEVICE_STEPS,
    {"VA macro DMIC0/DMIC2 into DEC0/DEC1", 0, 0, 0, va_on, 0, 0, 0, va_off, 0},
    MIC_START_STEPS,
};

const ArStep *ar_mic_live_steps(unsigned *count)
{
    *count = sizeof kMicLive / sizeof kMicLive[0];
    return kMicLive;
}

static const ArStep kMic[] = {
    VA_STEPS,
    MIC_DEVICE_STEPS,
    MIC_START_STEPS,
};
_Static_assert(MIC_PREPARED < sizeof kMic / sizeof kMic[0], "the mic plan's prepared prefix");

const ArStep *ar_mic_steps(unsigned *count, unsigned *prepared)
{
    *count = sizeof kMic / sizeof kMic[0];
    if (prepared) *prepared = MIC_PREPARED;
    return kMic;
}

void ar_mic_defaults(ArMicPlan *p)
{
    for (unsigned i = 0; i < sizeof *p; ++i) ((uint8_t *)p)[i] = 0;
    p->device.rate = 48000; p->device.bits = 16; p->device.channels = 2;
    p->stream.rate = 48000; p->stream.bits = 16; p->stream.channels = 2;
    p->channel_mask = 0x3;
    p->stream_id = 0x1001;
}

/* --------------------------------------------------------------------------------------------------------- the period pump */
int ar_pump_init(ArPump *p, uint64_t base, uint32_t handle, uint32_t port, uint32_t slots, uint32_t period_bytes, uint32_t depth,
                 uint32_t late_ms)
{
    for (unsigned i = 0; i < sizeof *p; ++i) ((uint8_t *)p)[i] = 0;
    if (slots < 2 || slots > AR_PUMP_MAX_SLOTS || !depth || depth > slots || !period_bytes || !handle || !late_ms) return -1;
    if ((uint64_t)(uint32_t)base + (uint64_t)slots * period_bytes > 0x100000000ull) return -1;
    p->base = base; p->handle = handle; p->port = port; p->slots = slots; p->period_bytes = period_bytes; p->depth = depth;
    p->late_ms = late_ms; p->token_base = 0x4000u;
    return 0;
}

int ar_pump_init_capture(ArPump *p, uint64_t base, uint32_t handle, uint32_t port, uint32_t slots, uint32_t period_bytes, uint32_t depth,
                         uint32_t late_ms)
{
    if (ar_pump_init(p, base, handle, port, slots, period_bytes, depth, late_ms)) return -1;
    p->capture = 1;
    p->token_base = 0x5000u;
    return 0;
}

int ar_pump_free_slot(const ArPump *p)
{
    if (!p->slots || p->in_flight >= p->depth || p->busy[p->next]) return -1;
    return (int)p->next;
}

uint32_t ar_pump_write(ArPump *p, uint8_t *out, uint32_t capacity, uint32_t now_ms)
{
    const int slot = ar_pump_free_slot(p);
    if (slot < 0) return 0;
    const uint64_t at = p->base + (uint64_t)slot * p->period_bytes;
    const uint32_t token = p->token_base + (uint32_t)slot;
    const uint32_t n = p->capture ? ar_read(out, capacity, p->port, token, at, p->handle, p->period_bytes)
                                  : ar_write(out, capacity, p->port, token, at, p->handle, p->period_bytes);
    if (!n) return 0;
    p->busy[slot] = 1; p->sent_ms[slot] = now_ms; p->in_flight++; p->sent++;
    p->next = (p->next + 1) % p->slots;
    return n;
}

int ar_pump_done(ArPump *p, const ArAnswer *a)
{
    if (a->opcode != (p->capture ? AR_OP_READ_DONE : AR_OP_WRITE_DONE) || a->dst_port != p->port) return 0;
    const uint64_t at = a->address - p->base;
    const uint32_t slot = (uint32_t)(at / p->period_bytes);
    if (a->address < p->base || at % p->period_bytes || slot >= p->slots || !p->busy[slot] || a->token != p->token_base + slot ||
        a->handle != p->handle) {
        p->errors++;
        return -1;
    }
    p->busy[slot] = 0; p->in_flight--;
    if (a->status) {p->errors++; p->last_status = a->status; return -1;}
    if (p->capture && (a->offset > p->period_bytes || a->data_bytes > p->period_bytes - a->offset)) {p->errors++; return -1;}
    p->last_slot = slot;
    p->returned++;
    return 1;
}

int ar_pump_check(ArPump *p, uint32_t now_ms)
{
    for (uint32_t i = 0; i < p->slots; ++i)
        if (p->busy[i] && now_ms - p->sent_ms[i] > p->late_ms) {p->late++; return -1;}
    return 0;
}
