// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of levels 8-10 (AudioLive.c) in simulated time, after the real session (AudioSession.c, levels 4-6 with keep) handed
// the hardware over, against models of the DSP (PRM clocks and votes, the APM's graphs, two maps, the speaker's shared memory
// endpoint playing one 10 ms buffer after another on its own clock, the capture endpoint filling one), the four amplifiers on
// their bus and reset lines, the VA macro's DMIC path, and both macOS devices' engine threads (5 ms periods on the host clock):
//   - the commands of each plan in order, never two plan commands in flight at once (over both plans), from the port set's ports;
//   - speaker safety at every amplifier write and every buffer the DSP plays: no amplifier powered up but while the bit clock and
//     both speaker graphs run, none unmuted before four periods of the stream came back, no unmuted gain above the level's cap
//     (160 at level 8, 192 at 9), no gain above 192 ever, no sample above the level's ceiling, no slot changed while the DSP has
//     it, (in the clean runs) no sound played while an amplifier is muted (the fades come first), the amplifiers down before a
//     speaker graph stops and held in reset at the end;
//   - a device that plays only silence never brings anything up; sound brings the path up within a few hundred ms; silence, a
//     stopped device and the master mute take it down after idle_ms; a sound within the hold keeps it up;
//   - the clocks: the device 200 ppm fast or slow against the DSP: single frames slip, no underrun, the ring never overflows;
//   - the master volume ramps (no step above AUDIO_VOLUME_STEP a period); level 9's ceiling and gain;
//   - a refused period, an amplifier whose status changes, a plan step refused or unanswered: that path off for the boot; late
//     periods: down and back, the third time off; quiet: down and back; a fatal report: no command any more, everything held;
//   - the Mic (level 10): the capture plan with the VA macro's DMIC path between the prepares and the starts (and undone after the
//     stops), the two channels mixed to mono after the first 100 ms, both paths at once.
//   cc -std=c11 -Wall -Wextra -Werror live_host.c ../AudioLive.c ../AudioSession.c ../AudioAmps.c ../Fs19xx.c ../AudioSha256.c \
//      ../AudioSequence.c ../PianoAudioReach.c ../PianoGpr.c ../AudioSafety.c ../AudioDspLink.c -lm && ./a.out
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioLive.h"
#include "../PianoGpr.h"
#include "synthetic_fsm.h"

static unsigned checks;
static void dump(void);
#define CHECK(x) do {if (!(x)) {dump(); fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

#define DMA_PHYS 0x9da00000ull
#define H_SPK 0x55u
#define H_MIC 0x77u
#define MAX_PERIODS 20000

struct Answer {uint64_t due_us; uint32_t n; int read_slot, plan; uint8_t b[64];};
struct Amp {uint8_t address; int present, released; uint16_t reg[256]; uint16_t inject;};
struct World {
    uint64_t now_us;
    // ---- the DSP
    struct Answer q[1024];
    unsigned nq;
    char cmd[1024][40];
    unsigned ncmd, outstanding;
    const char *fail_match;           // the first command (after `fail_after` commands) whose line is this: refused or, with drop, lost
    unsigned fail_after;
    int fail_seen, drop;
    uint32_t fail_status;
    unsigned writes, write_dones, reads, read_dones;
    int write_error_at, drop_writes_from, drop_writes_until;   // WRITEs by count: refused with status 5; not answered in [from, until)
    double dsp_ppm;                   // the DSP's clock against the host's
    uint64_t play_until_us, cap_until_us;
    int clock_on, va_votes, va_mclk, dev_started, str_started, mic_started, cap_started, mic_prepared, cap_prepared;
    int map_spk, map_mic;
    unsigned stream_dones;
    uint8_t slot_copy[AUDIO_SPK_SLOTS][AUDIO_SPK_PERIOD_BYTES];
    int slot_busy[AUDIO_SPK_SLOTS];
    uint64_t cap_k;                   // the capture signal's sample counter
    // ---- what was played
    uint64_t played_periods, sound_periods, first_sound_played_us;
    int32_t peak, period_peak[MAX_PERIODS];
    uint32_t period_at[MAX_PERIODS];
    unsigned nperiods;
    // ---- the amplifiers, the pins, the VA macro
    struct Amp amp[4];
    unsigned power_ups, unmutes, transfers, gain_cap;
    int pins_on, va_on, va_calls;
    // ---- the devices' engines
    int out_running, in_running, sound_on, strict;
    int16_t amplitude;
    double phase, engine_ppm;
    double next_out_us, next_in_us;
    uint64_t first_sound_offered_us;
    int wake;
    unsigned wakes;
    uint64_t mic_frames, mic_ok, mic_bad;
    int16_t mic_peak;
    // ---- PianoPower's health
    AdgObservation o;
    int off;
    unsigned sends_after_off, send_calls;
    int queue_full_every;
    // ---- what went wrong
    unsigned violations;
    char violation[200];
    char lines[8000][240];
    unsigned nlines;
};
static struct World w;
static AudioLiveShared sh;
static int16_t g_out_ring[AUDIO_LIVE_RING * 2], g_in_ring[AUDIO_LIVE_MIC_RING];
static AudioSession session;
static AudioLive live;
static AudioDspGuard guard;
static uint8_t preset[4096];
static uint8_t g_dma[AUDIO_DMA_BYTES];   // the audio slice the kernel maps
static unsigned preset_bytes;
static uint32_t g_t0;                // the live paths' start (ms)
static double g_ppm;                 // the next run's device clock against the DSP's
static const char *g_fail;           // the next run's failing live command (refused, or lost with g_drop)
static int g_drop, g_queue_full;
static unsigned g_wakes;
static int g_mark;
static uint32_t now_ms(void) {return (uint32_t)(w.now_us / 1000u);}

static void violate(const char *what)
{
    if (!w.violations++) snprintf(w.violation, sizeof w.violation, "%s (at %u ms)", what, now_ms());
}
static void dump(void)
{
    const unsigned from = w.nlines > 60 ? w.nlines - 60 : 0;
    for (unsigned i = from; i < w.nlines; ++i) fprintf(stderr, "  | %s\n", w.lines[i]);
    const unsigned c0 = w.ncmd > 60 ? w.ncmd - 60 : 0;
    for (unsigned i = c0; i < w.ncmd; ++i) fprintf(stderr, "  cmd %u: %s\n", i, w.cmd[i]);
    if (w.violations) fprintf(stderr, "  first violation: %s (%u in all)\n", w.violation, w.violations);
}
static void log_line(void *ctx, const char *fmt, ...)
{
    (void)ctx;
    va_list ap;
    va_start(ap, fmt);
    if (w.nlines < 8000) vsnprintf(w.lines[w.nlines++], sizeof w.lines[0], fmt, ap);
    va_end(ap);
    if (getenv("LIVE_LOG")) fprintf(stderr, "[%6u] %s\n", now_ms(), w.nlines ? w.lines[w.nlines - 1] : "");
}
static int logged(const char *needle)
{
    for (unsigned i = 0; i < w.nlines; ++i) if (strstr(w.lines[i], needle)) return 1;
    return 0;
}
static unsigned count_cmd(const char *line, unsigned from)
{
    unsigned n = 0;
    for (unsigned i = from; i < w.ncmd; ++i) n += !strcmp(w.cmd[i], line);
    return n;
}
static void note_cmd(const char *line) {if (w.ncmd < 1024) snprintf(w.cmd[w.ncmd++], sizeof w.cmd[0], "%s", line);}

// ------------------------------------------------------------------------------------------------------- the amplifiers
static int any_powered(void)
{
    for (int i = 0; i < 4; ++i) if (w.amp[i].released && !(w.amp[i].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN)) return 1;
    return 0;
}
static int all_unmuted(void)
{
    for (int i = 0; i < 4; ++i)
        if (!w.amp[i].released || (w.amp[i].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) || (w.amp[i].reg[FS19XX_REG_DACCTRL] & FS19XX_DACCTRL_MUTE_BIT))
            return 0;
    return 1;
}
static void amp_defaults(struct Amp *a)
{
    memset(a->reg, 0, sizeof a->reg);
    a->reg[FS19XX_REG_DEVID] = FS19XX_DEVID; a->reg[FS19XX_REG_REVID] = 0x00b1; a->reg[FS19XX_REG_CHIPINI] = 0x0003;
    a->reg[FS19XX_REG_PWRCTRL] = FS19XX_PWRCTRL_PWDN; a->reg[FS19XX_REG_VOLCTRL] = 0xffc0;
    a->reg[FS19XX_REG_DACCTRL] = FS19XX_DACCTRL_MUTE; a->reg[FS19XX_REG_I2SCTRL] = 0x0003;
}
static void amp_derive(struct Amp *a)
{
    const int on = !(a->reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) && a->reg[FS19XX_REG_SYSCTRL] == FS19XX_SYSCTRL_ON;
    const int clocked = w.clock_on && w.dev_started;
    a->reg[FS19XX_REG_DIGSTAT] = on && clocked && a->reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_UNMUTE ? FS19XX_DIGSTAT_DACRUN : 0;
    a->reg[FS19XX_REG_STATUS] = (uint16_t)((clocked ? 0x0040 : 0) | (on && clocked ? 0x0002 : 0) | a->inject);
}
static struct Amp *amp_at(uint8_t address)
{
    for (int i = 0; i < 4; ++i) if (w.amp[i].address == address && w.amp[i].present && w.amp[i].released) return &w.amp[i];
    return 0;
}
static int transfer(void *ctx, uint8_t address, const uint8_t *out, uint32_t out_len, uint8_t *in, uint32_t in_len)
{
    (void)ctx;
    w.transfers++;
    w.now_us += 300;
    struct Amp *a = amp_at(address);
    if (!a) return -1;
    amp_derive(a);
    if (out_len == 3 && !in_len) {
        const uint8_t reg = out[0];
        const uint16_t v = (uint16_t)(out[1] << 8 | out[2]);
        const int unmuted = !(a->reg[FS19XX_REG_DACCTRL] & FS19XX_DACCTRL_MUTE_BIT);
        if (reg == FS19XX_REG_VOLCTRL && (v >> 8) > FS19XX_GAIN_CAP) violate("an amplifier gain above 192");
        if (reg == FS19XX_REG_VOLCTRL && unmuted && (v >> 8) > w.gain_cap) violate("an unmuted amplifier's gain above the level's cap");
        if (reg == FS19XX_REG_DACCTRL && !(v & FS19XX_DACCTRL_MUTE_BIT) && (a->reg[FS19XX_REG_VOLCTRL] >> 8) > w.gain_cap)
            violate("an amplifier unmuted at a gain above the level's cap");
        if (reg == FS19XX_REG_PWRCTRL && !(v & (FS19XX_PWRCTRL_PWDN | FS19XX_PWRCTRL_I2CR))) {
            w.power_ups++;
            if (!w.clock_on || !w.dev_started || !w.str_started) violate("an amplifier powered up without the bit clock and both graphs");
        }
        if (reg == FS19XX_REG_DACCTRL && !(v & FS19XX_DACCTRL_MUTE_BIT)) {
            w.unmutes++;
            if (w.stream_dones < AUDIO_UNMUTE_PERIODS) violate("an amplifier unmuted before four periods of the stream came back");
            if (a->reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) violate("an amplifier unmuted while powered down");
        }
        if (reg == FS19XX_REG_PWRCTRL && (v & FS19XX_PWRCTRL_I2CR)) {amp_defaults(a); return -1;}
        a->reg[reg] = v;
        amp_derive(a);
        return 0;
    }
    if (out_len == 1 && in_len == 2) {
        in[0] = (uint8_t)(a->reg[out[0]] >> 8); in[1] = (uint8_t)a->reg[out[0]];
        return 0;
    }
    return -1;
}
static void sleep_us(void *ctx, uint32_t us) {(void)ctx; w.now_us += us;}
static const Fs19xxBus bus = {0, transfer, sleep_us};
static int reset_line(void *owner, unsigned slot, int release)
{
    (void)owner;
    if (slot >= 4) return -1;
    struct Amp *a = &w.amp[slot];
    if (release && !a->released) amp_defaults(a);
    if (!release) amp_defaults(a);
    a->released = release;
    return 0;
}

// ------------------------------------------------------------------------------------------------------------- the DSP
static struct Answer *queue(uint64_t due_us, const GprPacket *r, const uint32_t *words, unsigned n)
{
    CHECK(w.nq < 1024);
    struct Answer *a = &w.q[w.nq++];
    a->due_us = due_us;
    a->read_slot = -1;
    a->plan = 0;
    a->n = gpr_encode(a->b, sizeof a->b, r, words, 4 * n);
    CHECK(a->n);
    return a;
}

static void describe(const GprPacket *p, char *s, unsigned size)
{
    const uint8_t *d = p->payload;
    switch (p->opcode) {
    case AR_OP_PRM_REQUEST:
        if (le32(d + 20) == 0x08001032u) snprintf(s, size, "PRM VOTE %u", le32(d + 32));
        else snprintf(s, size, "PRM REQ %#x %u", le32(d + 36), le32(d + 40));
        break;
    case AR_OP_PRM_RELEASE:
        if (le32(d + 20) == 0x08001032u) snprintf(s, size, "PRM UNVOTE %u", le32(d + 32));
        else snprintf(s, size, "PRM REL %#x", le32(d + 36));
        break;
    case AR_OP_GRAPH_OPEN: snprintf(s, size, "OPEN %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_PREPARE: snprintf(s, size, "PREPARE %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_START: snprintf(s, size, "START %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_STOP: snprintf(s, size, "STOP %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_CLOSE: snprintf(s, size, "CLOSE %#x", le32(d + 36)); break;
    case AR_OP_SET_CFG: snprintf(s, size, "CFG %#x", le32(d + 16)); break;
    case AR_OP_MEM_MAP: snprintf(s, size, "MAP %#x%08x", le32(d + 12), le32(d + 8)); break;
    case AR_OP_MEM_UNMAP: snprintf(s, size, "UNMAP %#x", le32(d)); break;
    default: snprintf(s, size, "OP %#x", p->opcode);
    }
}

static uint64_t dsp_period_us(void) {return (uint64_t)(10000.0 / (1.0 + w.dsp_ppm * 1e-6) + 0.5);}

static void dsp_write(const GprPacket *p)
{
    const uint8_t *d = p->payload;
    w.writes++;
    if (p->src_port != session.ports->data || p->dst_port != AR_IID_PLAY_SHMEM) violate("a WRITE from the wrong port");
    if (!w.str_started || !w.dev_started || !w.clock_on) violate("a WRITE without the running graphs");
    const uint64_t address = le32(d) | (uint64_t)le32(d + 4) << 32;
    const uint64_t offset = (address & 0xffffffffu) - DMA_PHYS;
    const unsigned slot = (unsigned)(offset / AUDIO_SPK_PERIOD_BYTES);
    if (address >> 32 != 1 || offset % AUDIO_SPK_PERIOD_BYTES || slot >= AUDIO_SPK_SLOTS || le32(d + 8) != H_SPK || le32(d + 12) != AUDIO_SPK_PERIOD_BYTES) {
        violate("a WRITE of another buffer");
        return;
    }
    if (w.slot_busy[slot]) violate("a WRITE of a slot the DSP still has");
    w.slot_busy[slot] = 1;
    memcpy(w.slot_copy[slot], g_dma + offset, AUDIO_SPK_PERIOD_BYTES);
    const int k = (int)w.writes;
    if (k >= w.drop_writes_from && k < w.drop_writes_until) {w.slot_busy[slot] = 0; return;}   // never played, never answered
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = p->dst_port, .dst_port = p->src_port,
                   .token = p->token, .opcode = AR_OP_WRITE_DONE};
    const uint32_t words[4] = {le32(d), le32(d + 4), le32(d + 8), k == w.write_error_at ? 5u : 0u};
    uint64_t at = w.play_until_us > w.now_us ? w.play_until_us : w.now_us;
    at += dsp_period_us();
    w.play_until_us = at;
    struct Answer *a = queue(at, &r, words, 4);
    a->read_slot = -2 - (int)slot;   // a speaker slot to "play" at delivery
}

// The DSP plays a speaker slot when its WRITE_DONE is due.
static void play_slot(unsigned slot)
{
    w.slot_busy[slot] = 0;
    const int16_t *pcm = (const int16_t *)(const void *)w.slot_copy[slot];
    // a slot the kernel changed while the DSP had it, but for zeros (a stop for trouble zeroes the buffer at once)
    const uint8_t *now = g_dma + slot * AUDIO_SPK_PERIOD_BYTES;
    if (memcmp(w.slot_copy[slot], now, AUDIO_SPK_PERIOD_BYTES)) {
        int zeros = 1;
        for (unsigned i = 0; i < AUDIO_SPK_PERIOD_BYTES; ++i) zeros &= !now[i];
        if (!zeros) violate("a slot changed while the DSP had it");
    }
    int32_t peak = 0;
    for (uint32_t i = 0; i < AUDIO_PERIOD_FRAMES * 4; ++i) {
        const int v = abs(pcm[i]);
        if (v > peak) peak = v;
    }
    if (peak > live.cfg.ceiling) violate("a sample above the level's ceiling");
    if (peak > w.peak) w.peak = peak;
    if (w.nperiods < MAX_PERIODS) {w.period_at[w.nperiods] = now_ms() - g_t0; w.period_peak[w.nperiods++] = peak;}
    w.played_periods++;
    if (peak) {
        w.sound_periods++;
        if (!w.first_sound_played_us) w.first_sound_played_us = w.now_us;
        if (w.strict && !all_unmuted()) violate("sound played while an amplifier is muted (a fade was cut)");
    }
}

static void dsp_read(const GprPacket *p)
{
    const uint8_t *d = p->payload;
    w.reads++;
    if (p->src_port != session.ports->data || p->dst_port != AR_IID_CAP_SHMEM) violate("a READ from the wrong port");
    if (!w.mic_started || !w.cap_started || !w.va_mclk || w.va_votes != 2) violate("a READ without the running capture graphs");
    if (!w.va_on) violate("a READ without the VA macro's DMIC path");
    const uint64_t address = le32(d) | (uint64_t)le32(d + 4) << 32;
    const uint64_t offset = (address & 0xffffffffu) - DMA_PHYS - AUDIO_DMA_MIC;
    const unsigned slot = (unsigned)(offset / AUDIO_MIC_PERIOD_BYTES);
    if (address >> 32 != 1 || offset % AUDIO_MIC_PERIOD_BYTES || slot >= AUDIO_MIC_SLOTS || le32(d + 8) != H_MIC || le32(d + 12) != AUDIO_MIC_PERIOD_BYTES) {
        violate("a READ of another buffer");
        return;
    }
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = p->dst_port, .dst_port = p->src_port,
                   .token = p->token, .opcode = AR_OP_READ_DONE};
    const uint32_t words[6] = {0, le32(d), le32(d + 4), H_MIC, AUDIO_MIC_PERIOD_BYTES, 0};
    uint64_t at = w.cap_until_us > w.now_us ? w.cap_until_us : w.now_us;
    at += dsp_period_us();
    w.cap_until_us = at;
    struct Answer *a = queue(at, &r, words, 6);
    a->read_slot = (int)slot;
}

// The DSP's capture: a square wave of +-8000 on DEC0 and +-4000 on DEC1 (96 samples a cycle): the mono mix is +-6000.
static void fill_capture(unsigned slot)
{
    int16_t *pcm = (int16_t *)(void *)(g_dma + AUDIO_DMA_MIC + slot * AUDIO_MIC_PERIOD_BYTES);
    for (uint32_t i = 0; i < AUDIO_PERIOD_FRAMES; ++i, ++w.cap_k) {
        const int hi = w.cap_k % 96 < 48;
        pcm[2 * i] = (int16_t)(hi ? 8000 : -8000);
        pcm[2 * i + 1] = (int16_t)(hi ? 4000 : -4000);
    }
}

static int dsp_send(void *ctx, const uint8_t *b, uint32_t n)
{
    (void)ctx;
    w.send_calls++;
    if (w.queue_full_every && w.send_calls % (unsigned)w.queue_full_every == 0) return -1;
    if (w.off) w.sends_after_off++;
    GprPacket p;
    CHECK(gpr_decode(b, n, &p) == GprOk && p.size == n && n <= GPR_MAX_PACKET);
    if (p.opcode == AR_OP_WRITE) {dsp_write(&p); return 0;}
    if (p.opcode == AR_OP_READ) {dsp_read(&p); return 0;}
    char line[40];
    describe(&p, line, sizeof line);
    note_cmd(line);
    if (w.outstanding) violate("two DSP commands in flight at once");
    const int prm = p.opcode == AR_OP_PRM_REQUEST || p.opcode == AR_OP_PRM_RELEASE;
    if (p.src_port != (prm ? session.ports->prm : session.ports->apm)) violate("a command from the wrong port");
    uint32_t status = 0;
    if (w.fail_match && !w.fail_seen && w.ncmd > w.fail_after && !strcmp(line, w.fail_match)) {
        w.fail_seen = 1;
        if (w.drop) return 0;   // lost: no answer will come
        status = w.fail_status;
    }
    if (!status) {
        if (!strcmp(line, "PRM REQ 0x202 6144000")) w.clock_on = 1;
        if (!strcmp(line, "PRM REL 0x202")) {if (w.dev_started || w.str_started) violate("the bit clock released under running graphs"); w.clock_on = 0;}
        if (!strncmp(line, "PRM VOTE", 8)) w.va_votes++;
        if (!strncmp(line, "PRM UNVOTE", 10)) w.va_votes--;
        if (!strcmp(line, "PRM REQ 0x30c 19200000")) w.va_mclk = 1;
        if (!strcmp(line, "PRM REL 0x30c")) {if (w.va_on) violate("the VA mclk released under the DMIC path"); w.va_mclk = 0;}
        if (!strcmp(line, "START 0x4005")) {if (!w.clock_on) violate("the device graph started without the bit clock"); w.dev_started = 1;}
        if (!strcmp(line, "START 0x4001")) {w.str_started = 1; w.stream_dones = 0;}
        if (!strcmp(line, "STOP 0x4001") || !strcmp(line, "STOP 0x4005")) {
            if (any_powered()) violate("a speaker graph stopped with an amplifier powered up");
            if (line[10] == '1') w.str_started = 0; else w.dev_started = 0;
        }
        if (!strcmp(line, "PREPARE 0x4008")) w.mic_prepared = 1;
        if (!strcmp(line, "PREPARE 0x4003")) w.cap_prepared = 1;
        if (!strcmp(line, "START 0x4008")) w.mic_started = 1;
        if (!strcmp(line, "START 0x4003")) w.cap_started = 1;
        if (!strcmp(line, "STOP 0x4008")) w.mic_started = 0;
        if (!strcmp(line, "STOP 0x4003")) w.cap_started = 0;
        if (!strcmp(line, "CLOSE 0x4008")) {if (w.va_on) violate("the mic graph closed under the DMIC path"); w.mic_prepared = 0;}
        if (!strcmp(line, "CLOSE 0x4003")) w.cap_prepared = 0;
    }
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = p.dst_port, .dst_port = p.src_port,
                   .token = p.token};
    const uint64_t due = w.now_us + 1000;
    w.outstanding = 1;
    if (p.opcode == AR_OP_MEM_MAP && !status) {
        r.opcode = AR_OP_MEM_MAP_DONE;
        const uint32_t h = le32(p.payload + 8) == (uint32_t)DMA_PHYS ? H_SPK : H_MIC;
        if (h == H_SPK) w.map_spk++; else w.map_mic++;
        queue(due, &r, &h, 1)->plan = 1;
        return 0;
    }
    if (p.opcode == AR_OP_MEM_UNMAP && !status) {if (le32(p.payload) == H_SPK) w.map_spk--; else w.map_mic--;}
    r.opcode = p.opcode == AR_OP_PRM_REQUEST ? AR_OP_PRM_REQUEST_DONE : p.opcode == AR_OP_PRM_RELEASE ? AR_OP_PRM_RELEASE_DONE : GPR_OP_BASIC_RESULT;
    const uint32_t words[2] = {p.opcode, status};
    queue(due, &r, words, 2)->plan = 1;
    return 0;
}

// ---------------------------------------------------------------------------------------------------- the owner's side
static void lpass_read(void *ctx) {(void)ctx;}
static int tdm_pins(void *ctx, int on) {(void)ctx; w.pins_on = on; return 0;}
static int smmu(void *ctx) {(void)ctx; return 0;}
static void publish(void *ctx) {(void)ctx;}
static int va_mic(void *ctx, int on)
{
    (void)ctx;
    w.va_calls++;
    note_cmd(on ? "VA ON" : "VA OFF");
    if (w.va_votes != 2 || !w.va_mclk) violate("the VA macro touched without both votes and the mclk");
    if (on && (!w.mic_prepared || !w.cap_prepared || w.mic_started || w.cap_started)) violate("the DMIC path on outside prepared, not started graphs");
    if (!on && (w.mic_started || w.cap_started)) violate("the DMIC path off under running capture graphs");
    w.va_on = on;
    return 0;
}

// ------------------------------------------------------------------------------------------------------ the engines
static void engine(void)
{
    const double out_period = 5000.0 / (1.0 + w.engine_ppm * 1e-6);
    if (w.out_running && w.now_us >= (uint64_t)w.next_out_us) {
        int16_t buf[240 * 2];
        for (unsigned i = 0; i < 240; ++i) {
            const int16_t v = w.sound_on ? (int16_t)lrint(w.amplitude * sin(w.phase)) : 0;
            w.phase += 2 * M_PI * 1000.0 / 48000.0;
            buf[2 * i] = v;
            buf[2 * i + 1] = (int16_t)-v;
        }
        if (w.sound_on && w.amplitude && !w.first_sound_offered_us) w.first_sound_offered_us = w.now_us;
        int wake = 0;
        CHECK(audio_live_play(&sh, buf, 240, now_ms(), &wake) == 240);
        if (wake) {
            w.wake = 1;
            w.wakes++;
            if (getenv("LIVE_WAKES")) fprintf(stderr, "wake at %u ms (speaker %s)\n", now_ms() - g_t0, audio_live_state(live.spk_state));
        }
        w.next_out_us += out_period;
    }
    if (w.in_running && w.now_us >= (uint64_t)w.next_in_us) {
        int16_t mono[240];
        memset(mono, 0x55, sizeof mono);
        const uint32_t n = audio_live_capture(&sh, mono, 240);
        CHECK(n <= 240);
        for (uint32_t i = 0; i < n; ++i) {
            if (mono[i] == 6000 || mono[i] == -6000) w.mic_ok++;
            else if (mono[i]) w.mic_bad++;
            if (abs(mono[i]) > w.mic_peak) w.mic_peak = (int16_t)abs(mono[i]);
        }
        w.mic_frames += n;
        w.next_in_us += 5000.0;
    }
}
static void device(int out, int in)
{
    if (out && !w.out_running) w.next_out_us = (double)w.now_us;
    if (in && !w.in_running) w.next_in_us = (double)w.now_us;
    w.out_running = out;
    w.in_running = in;
    audio_live_running(&sh, out, in);
    w.wake = 1;
}

// ---------------------------------------------------------------------------------------------------- the runs
struct Scenario {
    uint32_t level;
    unsigned gain;
    unsigned ceiling_db;              // 0: the level's default
    uint32_t idle_ms, end_ms;
    int ports, strict;
    void (*hook)(uint32_t t);         // ms since the live paths started
};

static void deliver(int to_live)
{
    for (unsigned i = 0; i < w.nq;) {
        if (w.q[i].due_us <= w.now_us) {
            struct Answer a = w.q[i];
            memmove(&w.q[i], &w.q[i + 1], (w.nq - i - 1) * sizeof w.q[0]);
            w.nq--;
            if (a.plan) w.outstanding = 0;
            if (a.n >= 24 && le32(a.b + 20) == AR_OP_WRITE_DONE) {w.write_dones++; if (w.str_started) w.stream_dones++;}
            if (a.read_slot <= -2) play_slot((unsigned)(-2 - a.read_slot));
            if (a.read_slot >= 0) {fill_capture((unsigned)a.read_slot); w.read_dones++;}
            if (to_live) audio_live_receive(&live, a.b, a.n, now_ms());
            else audio_session_receive(&session, a.b, a.n, now_ms());
            continue;
        }
        ++i;
    }
}

static void (*g_setup)(AudioLiveConfig *);

static void run(const struct Scenario *sc)
{
    memset(&w, 0, sizeof w);
    const uint8_t addresses[4] = AUDIO_AMP_ADDRESSES;
    for (int i = 0; i < 4; ++i) {w.amp[i].address = addresses[i]; w.amp[i].present = 1; amp_defaults(&w.amp[i]);}
    w.write_error_at = -1;
    w.drop_writes_from = w.drop_writes_until = -1;
    w.gain_cap = AUDIO_AMPS_TEST_GAIN;   // the session's levels 6 never power up anyway
    w.strict = sc->strict;
    w.o = (AdgObservation){1, 1, 0, 0, 3, ADG_CHARGER_UP, 1, 0, "", 1};
    adg_init(&guard, 0);
    CHECK(adg_observe(&guard, &w.o, 0) == AdgRun);
    // ---- levels 4-6 with keep: the session hands the hardware over
    AudioSessionConfig c;
    audio_session_defaults(&c);
    c.level = 6; c.dsp = 1; c.ports = sc->ports; c.preset_any = 1; c.mic = 1; c.silence_ms = 300; c.keep = 1;
    AudioSessionOps ops = {0};
    ops.log = log_line; ops.send = dsp_send; ops.lpass_read = lpass_read;
    ops.amps = (AudioAmpsIo){0, &bus, reset_line, 0};
    ops.preset = preset; ops.preset_bytes = preset_bytes;
    ops.tdm_pins = tdm_pins; ops.smmu = smmu;
    uint8_t *dma = g_dma;
    memset(g_dma, 0, sizeof g_dma);
    ops.dma = dma; ops.dma_phys = DMA_PHYS; ops.dma_bytes = sizeof g_dma; ops.publish = publish;
    ops.va_mic = va_mic;
    audio_session_init(&session, &c, &ops, &guard);
    uint64_t next = 0;
    while (!session.finished && w.now_us < 60000000ull) {
        deliver(0);
        if (w.now_us >= next) {
            const uint32_t d = audio_session_tick(&session, &w.o, now_ms());
            next = w.now_us + (d ? d : 1) * 1000u;
        }
        w.now_us += 1000;
    }
    CHECK(session.finished && session.kept && !w.violations);
    CHECK(session.result[4] == AudioLevelPassed && session.result[5] == AudioLevelPassed && session.result[6] == AudioLevelPassed);
    for (int i = 0; i < 4; ++i) CHECK(w.amp[i].released && (w.amp[i].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN));
    CHECK(w.pins_on && !w.clock_on && !w.va_votes && !w.va_mclk && !w.map_spk && !w.map_mic && w.nq == 0);
    // ---- levels 8-10
    w.gain_cap = sc->level >= 9 ? FS19XX_GAIN_CAP : AUDIO_AMPS_TEST_GAIN;
    w.engine_ppm = g_ppm;
    w.queue_full_every = g_queue_full;
    w.fail_match = g_fail; w.drop = g_drop; w.fail_status = 1; w.fail_after = w.ncmd;
    AudioLiveConfig lc;
    audio_live_defaults(&lc, sc->level);
    if (sc->gain) lc.gain = sc->gain;
    if (sc->ceiling_db) lc.ceiling = audio_ceiling_db(sc->ceiling_db);
    if (sc->idle_ms) lc.idle_ms = sc->idle_ms;
    if (g_setup) g_setup(&lc);
    audio_live_shared_init(&sh, g_out_ring, g_in_ring, AUDIO_Q15_ONE);
    CHECK(audio_live_init(&live, &lc, &session, &sh) == 0);
    w.write_dones = w.stream_dones = 0;
    w.writes = 0;
    g_t0 = now_ms();
    next = 0;
    const uint64_t end = w.now_us + (uint64_t)sc->end_ms * 1000u;
    while (!live.finished && w.now_us < end) {
        deliver(1);
        engine();
        if (sc->hook) sc->hook(now_ms() - g_t0);
        if (getenv("LIVE_FILL") && w.now_us % 2000000u == 0)
            fprintf(stderr, "t %u ms: fill %u avg %d drops %u repeats %u played %llu\n", now_ms() - g_t0, audio_ring_fill(&sh.out),
                    live.spk_drift.avg / 64, live.drops, live.repeats, (unsigned long long)w.played_periods);
        if (w.now_us >= next || w.wake) {
            w.wake = 0;
            const uint32_t d = audio_live_tick(&live, &w.o, now_ms());
            next = w.now_us + (d ? d : 1) * 1000u;
        }
        w.now_us += 500;
    }
    // ---- the owner's stop, then the end state
    if (!live.finished) {
        w.strict = 0;   // the owner's stop powers the amplifiers down at once, whatever plays
        audio_live_abort(&live, 0, now_ms());
        const uint64_t stop = w.now_us + 60000000ull;
        while (!live.finished && w.now_us < stop) {
            deliver(1);
            if (w.now_us >= next) {
                const uint32_t d = audio_live_tick(&live, &w.o, now_ms());
                next = w.now_us + (d ? d : 1) * 1000u;
            }
            w.now_us += 500;
        }
    }
    CHECK(live.finished);
    for (int i = 0; i < 4; ++i) CHECK(!w.amp[i].released);
    CHECK(!w.pins_on && !w.va_on);
    for (unsigned i = 0; i < sizeof g_dma; ++i) if (g_dma[i]) {CHECK(!"the buffer is not zeroed at the end"); break;}
    if (!w.off) CHECK(!w.clock_on && !w.va_votes && !w.va_mclk && !w.map_spk && !w.map_mic && !w.dev_started && !w.str_started);
    g_setup = 0;
}

// The live paths' own commands (after the session's).
static unsigned live_from(void)
{
    for (unsigned i = 0; i < w.ncmd; ++i) if (!strcmp(w.cmd[i], "CLOSE 0x4008")) return i + 4;   // level 6's mic plan ends: mclk, votes
    return 0;
}
static const char *kSpkUp[] = {"PRM REQ 0x202 6144000", "OPEN 0x4005", "CFG 0x6050", "CFG 0x6051", "CFG 0x6052", "OPEN 0x4001", "CFG 0x6001",
                               "CFG 0x6002", "CFG 0x6004", "MAP 0x19da00000", "PREPARE 0x4005", "PREPARE 0x4001", "START 0x4005", "START 0x4001"};
static const char *kSpkDown[] = {"STOP 0x4001", "STOP 0x4005", "UNMAP 0x55", "CLOSE 0x4001", "CLOSE 0x4005", "PRM REL 0x202"};
static const char *kMicUp[] = {"PRM VOTE 1", "PRM VOTE 2", "PRM REQ 0x30c 19200000", "OPEN 0x4008", "CFG 0x6080", "CFG 0x6081", "OPEN 0x4003",
                               "CFG 0x6021", "CFG 0x6020", "MAP 0x19da10000", "PREPARE 0x4008", "PREPARE 0x4003", "VA ON", "START 0x4008",
                               "START 0x4003"};
static const char *kMicDown[] = {"STOP 0x4003", "STOP 0x4008", "VA OFF", "UNMAP 0x77", "CLOSE 0x4003", "CLOSE 0x4008", "PRM REL 0x30c",
                                 "PRM UNVOTE 2", "PRM UNVOTE 1"};
#define N(a) (sizeof a / sizeof a[0])
static unsigned expect(unsigned at, const char *const *want, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        if (at + i >= w.ncmd || strcmp(w.cmd[at + i], want[i])) {
            fprintf(stderr, "command %u: got \"%s\", want \"%s\"\n", at + i, at + i < w.ncmd ? w.cmd[at + i] : "(none)", want[i]);
            CHECK(0);
        }
    }
    return at + n;
}
static unsigned power_ups_after_session(void) {return w.power_ups;}   // the session's levels power nothing up

// ------------------------------------------------------------------------------------------------------------- the tests
// A device that runs from 500 ms, sound (full scale) from 1000 to 3000 ms, stopped at 9000.
static void burst(uint32_t t)
{
    if (t == 500 && !w.out_running) device(1, 0);
    w.amplitude = 32767;
    w.sound_on = t >= 1000 && t < 3000;
    if (t == 9000 && w.out_running) device(0, 0);
}
static void testLevel8(int ports)
{
    const struct Scenario sc = {8, 0, 0, 0, 12000, ports, 1, burst};
    run(&sc);
    CHECK(!w.violations);
    unsigned at = expect(live_from(), kSpkUp, N(kSpkUp));
    at = expect(at, kSpkDown, N(kSpkDown));
    CHECK(at == w.ncmd && live.spk_starts == 1 && power_ups_after_session() == 4 && w.unmutes == 4);
    // up within 300 ms of the first sound; about 2 s of sound played at the level 8 ceiling (full scale lands on it)
    CHECK(w.first_sound_played_us > w.first_sound_offered_us && w.first_sound_played_us - w.first_sound_offered_us < 300000);
    CHECK(w.sound_periods > 170 && w.sound_periods < 205 && w.peak <= AUDIO_CEILING_TEST && w.peak > AUDIO_CEILING_TEST * 97 / 100);
    CHECK(live.underruns <= 1 && live.skipped <= 1 && live.drops == 0 && live.repeats == 0);   // equal clocks: nothing slips
    // down 3 s after the sound ended (no sound for idle_ms), the amplifiers powered down then, not held (the path comes back)
    CHECK(logged("live: speaker: no sound for 3000 ms") && logged("the amplifiers unmuted, the sound fades in"));
    CHECK(logged("live: level 8: the Speaker on the DSP: playing gain 160"));
}

// Two bursts 5 s apart (two starts) and two 1 s apart (one: the path stays up through the gap).
static void bursts(uint32_t t)
{
    if (t == 200 && !w.out_running) device(1, 0);
    w.amplitude = 16000;
    w.sound_on = (t >= 500 && t < 1500) || (t >= 7000 && t < 8000) || (t >= 9000 && t < 9500);
}
static void testRestart(void)
{
    const struct Scenario sc = {8, 0, 0, 0, 16000, AudioPortsUbuntu, 1, bursts};
    run(&sc);
    CHECK(!w.violations && live.spk_starts == 2 && count_cmd("START 0x4001", live_from()) == 2 && count_cmd("STOP 0x4001", live_from()) == 2);
    CHECK(w.power_ups == 8 && w.unmutes == 8);
}

// The device stops while sound plays: a fade, the amplifiers muted after it played, the hold; the device back within the hold:
// unmuted again on the same plan.
static void stop_and_back(uint32_t t)
{
    if (t == 100 && !w.out_running) device(1, 0);
    if (t == 2000 && w.out_running) device(0, 0);
    if (t == 3500 && !w.out_running) device(1, 0);
    if (t == 5000 && w.out_running) device(0, 0);
    w.amplitude = 20000;
    w.sound_on = 1;
}
static void testDeviceStopHold(void)
{
    const struct Scenario sc = {8, 0, 0, 0, 12000, AudioPortsUbuntu, 1, stop_and_back};
    run(&sc);
    CHECK(!w.violations && live.spk_starts == 1 && w.power_ups == 4 && w.unmutes == 8);
    CHECK(logged("the device stopped: faded to silence, the amplifiers muted"));
    // the fade: the periods' peaks fall over 20 ms, not at once
    unsigned falls = 0;
    for (unsigned i = 1; i < w.nperiods; ++i) if (w.period_peak[i - 1] > 1000 && w.period_peak[i] > 0 && w.period_peak[i] < w.period_peak[i - 1] / 2) falls++;
    CHECK(falls >= 1);
}

// A device that plays only silence brings nothing up.
static void silent(uint32_t t)
{
    if (t == 100 && !w.out_running) device(1, 1);
    w.sound_on = 0;
}
static void testSilentDevice(void)
{
    const struct Scenario sc = {8, 0, 0, 0, 20000, AudioPortsUbuntu, 1, silent};
    run(&sc);
    CHECK(!w.violations && live.spk_starts == 0 && w.ncmd == live_from() && w.writes == 0 && w.power_ups == 0);
}

// Level 9: gain 192, the ceiling -1 dBFS; the master volume halved and back, then muted and unmuted.
static void volume(uint32_t t)
{
    if (t == 100 && !w.out_running) device(1, 0);
    w.amplitude = 32767;
    w.sound_on = 1;
    if (t == 2000) {audio_live_control(&sh, 50, -1); w.wake = 1;}
    if (t == 3000) {audio_live_control(&sh, 100, -1); w.wake = 1;}
    if (t == 4000) {audio_live_control(&sh, -1, 1); w.wake = 1;}
    if (t == 7600 && g_mark == 0) {g_mark = 1; g_wakes = w.wakes;}            // the path is down (muted at 4 s, 3 s of hold)
    if (t == 8990 && g_mark == 1) {g_mark = 2; g_wakes = w.wakes - g_wakes;}  // (the hook runs twice a millisecond)
    if (t == 9000) {audio_live_control(&sh, -1, 0); w.wake = 1;}
}
static void testLevel9(void)
{
    g_mark = 0;
    const struct Scenario sc = {9, 192, 1, 0, 12000, AudioPortsUbuntu, 1, volume};
    run(&sc);
    CHECK(!w.violations && live.cfg.ceiling == AUDIO_CEILING_MAX && w.peak <= AUDIO_CEILING_MAX && w.peak > AUDIO_CEILING_MAX * 97 / 100);
    CHECK(logged("gain 192 (-23.6 dB)") && logged("muted: faded to silence"));
    CHECK(live.spk_starts == 2);   // muted at 4 s: down 3 s later; unmuted at 9 s: up again
    if (g_wakes > 1) fprintf(stderr, "wakes while muted and down: %u\n", g_wakes);
    CHECK(g_wakes <= 1);           // sound while muted and down wakes the live thread at most once, not every period
    // the ramp (1.9-3.9 s: the two volume changes, no fade): from one period to the next the level moves by at most the step
    unsigned big = 0;
    for (unsigned i = 1; i < w.nperiods; ++i) {
        if (w.period_at[i] < 1900 || w.period_at[i] > 3900) continue;
        const int32_t a = w.period_peak[i - 1], b = w.period_peak[i];
        if (a > 1000 && b > 1000 && abs(a - b) > (int32_t)((int64_t)AUDIO_CEILING_MAX * AUDIO_VOLUME_STEP / AUDIO_Q15_ONE) + 200) big++;
    }
    CHECK(big == 0);
    // half volume (-25 dB) reached: some period near 0.056 of the ceiling
    int seen = 0;
    for (unsigned i = 0; i < w.nperiods; ++i) if (w.period_peak[i] > AUDIO_CEILING_MAX * 5 / 100 && w.period_peak[i] < AUDIO_CEILING_MAX * 7 / 100) seen = 1;
    CHECK(seen);
}

// Level 8 never takes level 9's gain or ceiling, whatever is asked.
static void testLevel8Caps(void)
{
    const struct Scenario sc = {8, 215, 1, 0, 5000, AudioPortsUbuntu, 1, burst};
    run(&sc);
    CHECK(!w.violations && live.cfg.gain == AUDIO_AMPS_TEST_GAIN && live.cfg.ceiling == AUDIO_CEILING_TEST && w.peak <= AUDIO_CEILING_TEST);
}

// The clocks: the device 200 ppm fast, then slow, for 60 s of sound.
static void steady(uint32_t t)
{
    if (t == 100 && !w.out_running) device(1, 0);
    w.amplitude = 12000;
    w.sound_on = 1;
}
static void testDrift(double ppm)
{
    const struct Scenario sc = {8, 0, 0, 0, 62000, AudioPortsUbuntu, 1, steady};
    g_ppm = ppm;
    run(&sc);
    g_ppm = 0;
    CHECK(!w.violations && live.spk_starts == 1);
    // the drift less the band the average may wander before the first slip
    const double expect_slips = fabs(ppm) * 1e-6 * 48000.0 * 60.0 - AUDIO_LIVE_SLIP_BAND;
    const uint32_t slips = ppm > 0 ? live.drops : live.repeats;
    if (getenv("LIVE_DRIFT"))
        fprintf(stderr, "drift %+.0f ppm: offered %llu taken %llu dropped %llu, played %llu periods, drops %u repeats %u underruns %u skipped %u\n",
                ppm, (unsigned long long)sh.offered, (unsigned long long)sh.taken, (unsigned long long)sh.dropped,
                (unsigned long long)w.played_periods, live.drops, live.repeats, live.underruns, live.skipped);
    CHECK(slips > expect_slips * 0.6 && slips < expect_slips * 1.4 + 20 && (ppm > 0 ? live.repeats : live.drops) == 0);
    CHECK(live.underruns <= 1 && live.skipped <= 1);
}

// ---- faults
static int g_kind;
static uint32_t g_at;
static void fault(uint32_t t)
{
    steady(t);
    if (t == 1500 && g_kind == 1) w.write_error_at = (int)w.writes + 3;                        // a refused period
    if (t == 1500 && g_kind == 2) w.amp[1].inject = 0x0100;                                     // OTPDS on one amplifier
    if (t == 1500 && g_kind == 3) {w.drop_writes_from = (int)w.writes + 1; w.drop_writes_until = w.drop_writes_from + 4;}   // late
    if (t == 1500 && g_kind == 4) w.o.quiet = 1;
    if (t == 2500 && g_kind == 4) w.o.quiet = 0;
    if (t == 1500 && g_kind == 5) {w.o.fatal = 1; w.o.fatal_reports = 1; w.off = 1;}
    if (g_kind == 6 && (t == 1500 || t == 4000 || t == 6500)) {w.drop_writes_from = (int)w.writes + 1; w.drop_writes_until = w.drop_writes_from + 4;}
    (void)g_at;
}
static void testFaults(void)
{
    // a refused period: the amplifiers down at once, the plan undone, no restart in this boot
    g_kind = 1;
    struct Scenario sc = {8, 0, 0, 0, 8000, AudioPortsUbuntu, 0, fault};
    run(&sc);
    CHECK(!w.violations && live.spk_state == LiveOff && live.spk_starts == 1 && logged("WRITE_DONE refused") && logged("OFF for this boot"));
    CHECK(!any_powered());
    // an amplifier whose status changes while playing
    g_kind = 2;
    run(&sc);
    CHECK(!w.violations && live.spk_state == LiveOff && logged("flagged by the group check") && logged("stopped by amplifier status"));
    // late periods: down, back with the sound
    g_kind = 3;
    run(&sc);
    CHECK(!w.violations && live.spk_starts == 2 && live.late_stops == 1 && logged("STOPPED: late period"));
    // quiet: down, no new command while it lasts, back after
    g_kind = 4;
    run(&sc);
    CHECK(!w.violations && live.spk_starts == 2 && logged("STOPPED: PianoPower asked for quiet") && logged("quiet over"));
    // a fatal report: no command any more, everything held in reset, finished
    g_kind = 5;
    run(&sc);
    CHECK(!w.violations && w.sends_after_off == 0 && logged("guard OFF"));
    // three late stops: off
    g_kind = 6;
    sc.end_ms = 12000;
    run(&sc);
    CHECK(!w.violations && live.late_stops == 3 && live.spk_state == LiveOff && logged("3 late stops"));
    g_kind = 0;
}

// A plan step refused or unanswered: the speaker path off, what was done undone, nothing powered up unless the plan was up.
static void testPlanFailures(void)
{
    for (unsigned k = 0; k < N(kSpkUp); ++k)
        for (int drop = 0; drop < 2; ++drop) {
            g_kind = 0;
            const struct Scenario sc = {8, 0, 0, 0, 6000, AudioPortsUbuntu, 0, steady};
            g_fail = kSpkUp[k];
            g_drop = drop;
            run(&sc);
            CHECK(!w.violations && live.spk_state == LiveOff && w.power_ups == 0);
            CHECK(logged("the speaker path is OFF for this boot"));
        }
    g_fail = 0;
    g_drop = 0;
}

// ---- level 10: the Mic, alone and with the speaker
static void mic_only(uint32_t t)
{
    if (t == 100 && !w.in_running) device(0, 1);
    if (t == 3000 && w.in_running) device(0, 0);
}
static void both(uint32_t t)
{
    if (t == 100) device(1, 1);
    w.amplitude = 10000;
    w.sound_on = t >= 100 && t < 4000;
    if (t == 4000) device(0, 0);
}
static void testMic(void)
{
    struct Scenario sc = {10, 0, 0, 0, 10000, AudioPortsUbuntu, 1, mic_only};
    run(&sc);
    CHECK(!w.violations && live.mic_starts == 1 && live.spk_starts == 0);
    unsigned at = expect(live_from(), kMicUp, N(kMicUp));
    at = expect(at, kMicDown, N(kMicDown));
    CHECK(at == w.ncmd && w.va_calls == 2);
    // 2.9 s of capture after the first 100 ms: the mono mix of the two channels, nothing else
    CHECK(w.mic_ok > 48000u * 25 / 10 && w.mic_bad == 0 && w.mic_peak == 6000);
    // both at once: one command at a time over both plans (the model flags two), each path's commands complete
    sc = (struct Scenario){10, 0, 0, 0, 10000, AudioPortsUbuntu, 1, both};
    run(&sc);
    CHECK(!w.violations && live.mic_starts == 1 && live.spk_starts == 1 && w.sound_periods > 300 && w.mic_ok > 48000u * 3);
    CHECK(count_cmd("VA ON", 0) == 1 && count_cmd("VA OFF", 0) == 1 && count_cmd("START 0x4003", 0) == 1 && count_cmd("STOP 0x4003", 0) == 1);
    // sheng's and SunUEFI's ports
    sc.ports = AudioPortsSheng;
    run(&sc);
    CHECK(!w.violations);
    sc.ports = AudioPortsSun;
    run(&sc);
    CHECK(!w.violations);
    // level 9 leaves the Mic silent (capture() gives nothing), no capture plan
    sc = (struct Scenario){9, 0, 0, 0, 5000, AudioPortsUbuntu, 1, mic_only};
    run(&sc);
    CHECK(!w.violations && live.mic_starts == 0 && w.mic_frames == 0);
}

// A full queue every third send: everything still in order, nothing lost.
static void testQueueFull(void)
{
    g_queue_full = 3;
    const struct Scenario sc = {10, 0, 0, 0, 8000, AudioPortsUbuntu, 1, both};
    run(&sc);
    g_queue_full = 0;
    CHECK(!w.violations && live.queue_full > 0 && live.spk_starts == 1 && live.mic_starts == 1 && w.sound_periods > 300);
}

int main(void)
{
    preset_bytes = sfsm_build(preset, sizeof preset, 0);
    CHECK(preset_bytes > 100);
    CHECK(audio_volume_q15(100) == AUDIO_Q15_ONE && audio_volume_q15(0) == 0 && audio_volume_q15(98) == 29204);
    CHECK(audio_ceiling_db(1) == AUDIO_CEILING_MAX && audio_ceiling_db(20) == 3277 && audio_ceiling_db(0) == AUDIO_CEILING_TEST);
    CHECK(audio_ceiling_db(6) == 16422 && audio_ceiling_db(41) == AUDIO_CEILING_TEST);
    testLevel8(AudioPortsUbuntu);
    testLevel8(AudioPortsSheng);
    testLevel8(AudioPortsSun);
    testRestart();
    testDeviceStopHold();
    testSilentDevice();
    testLevel9();
    testLevel8Caps();
    testDrift(200);
    testDrift(-200);
    testFaults();
    testPlanFailures();
    testMic();
    testQueueFull();
    printf("PASS live: %u checks\n", checks);
    return 0;
}
