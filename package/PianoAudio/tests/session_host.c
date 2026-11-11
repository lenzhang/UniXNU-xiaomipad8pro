// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of levels 4-7 (AudioSession.c) against a model of the DSP (PRM clocks and votes, the APM's graphs, the shared memory
// endpoint playing one 10 ms buffer after another) and of the four amplifiers on their bus and reset lines, in simulated time:
//   - the DSP commands of each level in the designed order, each from the port of the port set (Ubuntu's, sheng's, SunUEFI's);
//   - speaker safety, checked at every amplifier write and every buffer handed to the DSP: no amplifier is powered up but while the
//     bit clock and both speaker graphs run at level 7, none unmuted before four silent periods came back, no gain above 160, no
//     sample above -20 dBFS, nothing but zeros while the amplifiers are muted, at most 3 s of tone; the amplifiers are down before
//     a speaker graph stops and held in reset at the end;
//   - without piano-audio-tone=1 nothing is ever powered up and nothing but zeros reaches the DSP;
//   - at a refused or unanswered command, a fatal report, a new link generation, quiet, a refused or late period, an amplifier whose
//     status changes, the watchdog: the amplifiers go down at once, nothing new goes to the DSP that should not, what was done is
//     undone, the levels after are skipped;
//   - level 5 without the DSP, a missing amplifier, a preset that is not the stock file.
//   cc -std=c11 -Wall -Wextra -Werror session_host.c ../AudioSession.c ../AudioAmps.c ../Fs19xx.c ../AudioSha256.c \
//      ../AudioSequence.c ../PianoAudioReach.c ../PianoGpr.c ../AudioSafety.c ../AudioDspLink.c && ./a.out
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioSession.h"
#include "../PianoGpr.h"
#include "synthetic_fsm.h"

static unsigned checks;
static void dump(void);
#define CHECK(x) do {if (!(x)) {dump(); fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

#define DMA_PHYS 0x9da00000ull
#define HANDLE 0x55u

struct Answer {uint32_t due, n; uint8_t b[64];};
struct Amp {uint8_t address; int present, released; uint16_t reg[256]; uint16_t inject;};
struct World {
    uint64_t now_us;
    // ---- the DSP
    struct Answer q[512];
    unsigned nq;
    char cmd[256][40];
    unsigned ncmd;                    // every command (not the WRITEs), one line each
    unsigned writes, write_dones;
    const char *fail_match;           // the first command whose line is this is refused (fail_status) or, with drop, not answered
    int fail_seen, drop;
    uint32_t fail_status;
    int drop_writes_after;            // WRITEs after this many are never answered (-1: all answered)
    int write_error_at;               // this WRITE (by count) is answered with status 5 (-1: none)
    uint32_t period_us, play_until_us;
    int clock_on, va_votes, va_mclk, dev_started, str_started, mapped;
    uint32_t apm, prm, data;          // the ports every packet must come from
    unsigned stream_dones;            // WRITE_DONEs since the stream graph started
    int queue_full_every;
    unsigned send_calls, sends_after_off;
    int off;                          // the health went bad: nothing may be sent any more
    // ---- the amplifiers, the pins, the SMMU, the LPASS reads
    struct Amp amp[4];
    unsigned power_ups, unmutes, transfers;
    int need_swap, swapped;           // the bus answers only once SDA and SCL are swapped
    int lpass_reads, pins_on, pins_calls, smmu_calls;
    // ---- the buffer handed to the DSP
    uint8_t dma[AUDIO_DMA_BYTES];
    uint64_t tone_frames;
    int32_t peak;
    // ---- what went wrong
    unsigned violations;
    char violation[200];
    // ---- the session's log
    char lines[3000][220];
    unsigned nlines;
    AdgObservation o;
};
static struct World w;
static uint32_t now_ms(void) {return (uint32_t)(w.now_us / 1000u);}

static void violate(const char *what)
{
    if (!w.violations++) snprintf(w.violation, sizeof w.violation, "%s (at %u ms)", what, now_ms());
}

static void dump(void)
{
    const unsigned from = w.nlines > 80 ? w.nlines - 80 : 0;
    for (unsigned i = from; i < w.nlines; ++i) fprintf(stderr, "  | %s\n", w.lines[i]);
    for (unsigned i = 0; i < w.ncmd; ++i) fprintf(stderr, "  cmd %u: %s\n", i, w.cmd[i]);
    if (w.violations) fprintf(stderr, "  first violation: %s\n", w.violation);
}

static void log_line(void *ctx, const char *fmt, ...)
{
    (void)ctx;
    va_list ap;
    va_start(ap, fmt);
    if (w.nlines < 3000) vsnprintf(w.lines[w.nlines++], sizeof w.lines[0], fmt, ap);
    va_end(ap);
    if (getenv("SESSION_LOG")) fprintf(stderr, "[%6u] %s\n", now_ms(), w.nlines ? w.lines[w.nlines - 1] : "");
}
static int logged(const char *needle)
{
    for (unsigned i = 0; i < w.nlines; ++i) if (strstr(w.lines[i], needle)) return 1;
    return 0;
}

// ------------------------------------------------------------------------------------------------------- the amplifiers
static int any_powered(void)
{
    for (int i = 0; i < 4; ++i) if (w.amp[i].released && !(w.amp[i].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN)) return 1;
    return 0;
}
static int any_unmuted(void)
{
    for (int i = 0; i < 4; ++i)
        if (w.amp[i].released && !(w.amp[i].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) && !(w.amp[i].reg[FS19XX_REG_DACCTRL] & FS19XX_DACCTRL_MUTE_BIT))
            return 1;
    return 0;
}
static void amp_defaults(struct Amp *a)
{
    memset(a->reg, 0, sizeof a->reg);
    a->reg[FS19XX_REG_DEVID] = FS19XX_DEVID; a->reg[FS19XX_REG_REVID] = 0x00b1; a->reg[FS19XX_REG_CHIPINI] = 0x0003;
    a->reg[FS19XX_REG_PWRCTRL] = FS19XX_PWRCTRL_PWDN; a->reg[FS19XX_REG_VOLCTRL] = 0xffc0;   // 0 dB after reset: the dangerous default
    a->reg[FS19XX_REG_DACCTRL] = FS19XX_DACCTRL_MUTE; a->reg[FS19XX_REG_I2SCTRL] = 0x0003;
}
static void amp_derive(struct Amp *a)
{
    const int on = !(a->reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) && a->reg[FS19XX_REG_SYSCTRL] == FS19XX_SYSCTRL_ON;
    const int clocked = w.clock_on && w.dev_started;
    a->reg[FS19XX_REG_DIGSTAT] = on && clocked && a->reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_UNMUTE ? FS19XX_DIGSTAT_DACRUN : 0;
    a->reg[FS19XX_REG_STATUS] = (uint16_t)((clocked ? 0x0040 : 0) | (on && clocked ? 0x0002 : 0) | a->inject);   // CLKS, PLLS
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
    w.now_us += 400;   // a byte-banged transfer takes its time
    struct Amp *a = amp_at(address);
    if (!a || (w.need_swap && !w.swapped)) return -1;
    amp_derive(a);
    if (out_len == 3 && !in_len) {
        const uint8_t reg = out[0];
        const uint16_t v = (uint16_t)(out[1] << 8 | out[2]);
        // the gain: never above the core's cap (192), and at most 160 whenever the DAC is unmuted (the test gain)
        const int unmuted = !(a->reg[FS19XX_REG_DACCTRL] & FS19XX_DACCTRL_MUTE_BIT);
        if (reg == FS19XX_REG_VOLCTRL && (v >> 8) > FS19XX_GAIN_CAP) violate("an amplifier gain above the cap");
        if (reg == FS19XX_REG_VOLCTRL && unmuted && (v >> 8) > AUDIO_AMPS_TEST_GAIN) violate("an unmuted amplifier's gain above 160");
        if (reg == FS19XX_REG_DACCTRL && !(v & FS19XX_DACCTRL_MUTE_BIT) && (a->reg[FS19XX_REG_VOLCTRL] >> 8) > AUDIO_AMPS_TEST_GAIN)
            violate("an amplifier unmuted at a gain above 160");
        if (reg == FS19XX_REG_PWRCTRL && !(v & (FS19XX_PWRCTRL_PWDN | FS19XX_PWRCTRL_I2CR))) {
            w.power_ups++;
            if (!w.clock_on || !w.dev_started || !w.str_started) violate("an amplifier powered up without the bit clock and both graphs");
        }
        if (reg == FS19XX_REG_DACCTRL && !(v & FS19XX_DACCTRL_MUTE_BIT)) {
            w.unmutes++;
            if (w.stream_dones < AUDIO_UNMUTE_PERIODS) violate("an amplifier unmuted before four silent periods came back");
            if (a->reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) violate("an amplifier unmuted while powered down");
        }
        if (reg == FS19XX_REG_PWRCTRL && (v & FS19XX_PWRCTRL_I2CR)) {amp_defaults(a); return -1;}   // soft reset: no acknowledge
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
    if (!release) amp_defaults(a);   // held in shutdown: the registers are lost
    a->released = release;
    return 0;
}

// ------------------------------------------------------------------------------------------------------------- the DSP
static void queue(uint32_t due_ms, const GprPacket *r, const uint32_t *words, unsigned n)
{
    CHECK(w.nq < 512);
    struct Answer *a = &w.q[w.nq++];
    a->due = due_ms;
    a->n = gpr_encode(a->b, sizeof a->b, r, words, 4 * n);
    CHECK(a->n);
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

static void dsp_write(const GprPacket *p)
{
    const uint8_t *d = p->payload;
    w.writes++;
    if (p->src_port != w.data || p->dst_port != AR_IID_PLAY_SHMEM) violate("a WRITE from the wrong port");
    if (!w.str_started || !w.dev_started || !w.clock_on) violate("a WRITE without the running graphs");
    const uint64_t address = le32(d) | (uint64_t)le32(d + 4) << 32;
    const uint64_t offset = (address & 0xffffffffu) - DMA_PHYS;
    if (address >> 32 != 1 || offset % AUDIO_SPK_PERIOD_BYTES || offset + AUDIO_SPK_PERIOD_BYTES > (AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES) || le32(d + 8) != HANDLE ||
        le32(d + 12) != AUDIO_SPK_PERIOD_BYTES)
        violate("a WRITE of another buffer");
    else {
        const int16_t *pcm = (const int16_t *)(const void *)(w.dma + offset);
        for (uint32_t f = 0; f < AUDIO_PERIOD_FRAMES; ++f) {
            int sound = 0;
            for (int c = 0; c < 4; ++c) {
                const int v = abs(pcm[4 * f + c]);
                if (v > AUDIO_TEST_PEAK) violate("a sample above -20 dBFS");
                if (v > w.peak) w.peak = v;
                sound |= v != 0;
            }
            if (sound) {
                w.tone_frames++;
                if (!any_unmuted()) violate("sound while every amplifier is muted (the stream must start with zeros)");
            }
        }
    }
    const int k = (int)w.writes;
    if (w.drop_writes_after >= 0 && k > w.drop_writes_after) return;
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = p->dst_port, .dst_port = p->src_port,
                   .token = p->token, .opcode = AR_OP_WRITE_DONE};
    const uint32_t words[4] = {le32(d), le32(d + 4), le32(d + 8), k == w.write_error_at ? 5u : 0u};
    uint64_t at = w.play_until_us > w.now_us ? w.play_until_us : w.now_us;
    at += w.period_us;
    w.play_until_us = at;
    queue((uint32_t)(at / 1000u), &r, words, 4);
}

static int dsp_send(void *ctx, const uint8_t *b, uint32_t n)
{
    (void)ctx;
    w.send_calls++;
    if (w.queue_full_every && w.send_calls % (unsigned)w.queue_full_every == 0) return -1;
    if (w.off) w.sends_after_off++;
    GprPacket p;
    CHECK(gpr_decode(b, n, &p) == GprOk && p.size == n && n <= GPR_MAX_PACKET);
    CHECK(p.dst_domain == GPR_DOMAIN_ADSP && p.src_domain == GPR_DOMAIN_APPS);
    if (p.opcode == AR_OP_WRITE) {dsp_write(&p); return 0;}
    char line[40];
    describe(&p, line, sizeof line);
    if (w.ncmd < 256) snprintf(w.cmd[w.ncmd++], sizeof w.cmd[0], "%s", line);
    const int prm = p.opcode == AR_OP_PRM_REQUEST || p.opcode == AR_OP_PRM_RELEASE;
    if (p.src_port != (prm ? w.prm : w.apm)) violate("a command from the wrong port");
    if (prm && p.dst_port != AR_PRM_IID) violate("a PRM request not to the PRM");
    uint32_t status = 0;
    if (w.fail_match && !w.fail_seen && !strcmp(line, w.fail_match)) {
        w.fail_seen = 1;
        if (w.drop) return 0;
        status = w.fail_status;
    }
    if (!status) {   // the model's state
        if (!strcmp(line, "PRM REQ 0x202 6144000")) w.clock_on = 1;
        if (!strcmp(line, "PRM REL 0x202")) {if (w.dev_started || w.str_started) violate("the bit clock released under running graphs"); w.clock_on = 0;}
        if (!strncmp(line, "PRM VOTE", 8)) w.va_votes++;
        if (!strncmp(line, "PRM UNVOTE", 10)) w.va_votes--;
        if (!strcmp(line, "PRM REQ 0x30c 19200000")) w.va_mclk = 1;
        if (!strcmp(line, "PRM REL 0x30c")) w.va_mclk = 0;
        if (!strcmp(line, "START 0x4005")) {if (!w.clock_on) violate("the device graph started without the bit clock"); w.dev_started = 1;}
        if (!strcmp(line, "START 0x4001")) {w.str_started = 1; w.stream_dones = 0;}
        if (!strcmp(line, "STOP 0x4001") || !strcmp(line, "STOP 0x4005")) {
            if (any_powered()) violate("a speaker graph stopped with an amplifier powered up");
            if (line[10] == '1') w.str_started = 0; else w.dev_started = 0;
        }
        if (!strncmp(line, "MAP", 3)) w.mapped++;
        if (!strncmp(line, "UNMAP", 5)) w.mapped--;
    }
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = p.dst_port, .dst_port = p.src_port,
                   .token = p.token};
    const uint32_t due = now_ms() + 1;
    if (p.opcode == AR_OP_MEM_MAP && !status) {
        r.opcode = AR_OP_MEM_MAP_DONE;
        const uint32_t h = HANDLE;
        queue(due, &r, &h, 1);
        return 0;
    }
    r.opcode = p.opcode == AR_OP_PRM_REQUEST ? AR_OP_PRM_REQUEST_DONE : p.opcode == AR_OP_PRM_RELEASE ? AR_OP_PRM_RELEASE_DONE : GPR_OP_BASIC_RESULT;
    const uint32_t words[2] = {p.opcode, status};
    queue(due, &r, words, 2);
    return 0;
}

// ---------------------------------------------------------------------------------------------------- the owner's side
static void lpass_read(void *ctx)
{
    (void)ctx;
    w.lpass_reads++;
    if (w.va_votes != 2 || !w.va_mclk) violate("the LPASS registers read without both votes and the mclk");
}
static int tdm_pins(void *ctx, int on) {(void)ctx; w.pins_calls++; w.pins_on = on; return 0;}
static int smmu(void *ctx) {(void)ctx; w.smmu_calls++; if (w.mapped) violate("the SMMU set after a map"); return 0;}
static void publish(void *ctx) {(void)ctx;}

static uint8_t preset[4096];
static unsigned preset_bytes;

struct Scenario {
    uint32_t level;
    int dsp, ports, tone, any, mic;
    uint32_t silence_ms, limit_ms;
    void (*hook)(AudioSession *s, uint32_t now);
};

static AudioSession session;
static AudioDspGuard guard;
static const AudioSession *run(const struct Scenario *sc, void (*setup)(AudioSessionConfig *, AudioSessionOps *))
{
    memset(&w, 0, sizeof w);
    const uint8_t addresses[4] = AUDIO_AMP_ADDRESSES;
    for (int i = 0; i < 4; ++i) {w.amp[i].address = addresses[i]; w.amp[i].present = 1; amp_defaults(&w.amp[i]);}
    w.drop_writes_after = -1;
    w.write_error_at = -1;
    w.period_us = 10000;
    const AudioPorts *ports = audio_ports(sc->ports);
    w.apm = ports->apm; w.prm = ports->prm; w.data = ports->data;
    w.o = (AdgObservation){1, 1, 0, 0, 3, ADG_CHARGER_UP, 1, 0, "", 1};
    // level 2's guard, already running
    adg_init(&guard, 0);
    CHECK(adg_observe(&guard, &w.o, 0) == AdgRun);
    AudioSessionConfig c;
    audio_session_defaults(&c);
    c.level = sc->level; c.dsp = sc->dsp; c.ports = sc->ports; c.tone = sc->tone; c.preset_any = sc->any; c.mic = sc->mic;
    if (sc->silence_ms) c.silence_ms = sc->silence_ms;
    AudioSessionOps ops = {0};
    ops.log = log_line;
    ops.send = dsp_send;
    ops.lpass_read = lpass_read;
    ops.amps = (AudioAmpsIo){0, &bus, reset_line, 0};
    ops.preset = preset; ops.preset_bytes = preset_bytes;
    ops.tdm_pins = tdm_pins;
    ops.smmu = smmu;
    ops.dma = w.dma; ops.dma_phys = DMA_PHYS; ops.dma_bytes = sizeof w.dma;
    ops.publish = publish;
    if (setup) setup(&c, &ops);
    audio_session_init(&session, &c, &ops, sc->dsp ? &guard : 0);
    uint64_t next = 0;
    while (!session.finished && w.now_us < (uint64_t)sc->limit_ms * 1000u) {
        // answers that are due, in order
        for (unsigned i = 0; i < w.nq;) {
            if (w.q[i].due <= now_ms()) {
                struct Answer a = w.q[i];
                memmove(&w.q[i], &w.q[i + 1], (w.nq - i - 1) * sizeof w.q[0]);
                w.nq--;
                if (a.n >= 24 && le32(a.b + 20) == AR_OP_WRITE_DONE) {w.write_dones++; if (w.str_started) w.stream_dones++;}
                audio_session_receive(&session, a.b, a.n, now_ms());
                continue;
            }
            ++i;
        }
        if (sc->hook) sc->hook(&session, now_ms());
        if (w.now_us >= next) {
            const uint32_t d = audio_session_tick(&session, &w.o, now_ms());
            next = w.now_us + (d ? d : 1) * 1000u;
        }
        w.now_us += 1000;
    }
    CHECK(session.finished);
    // whatever happened: every amplifier held in reset, the pins given back, the buffer zeroed
    for (int i = 0; i < 4; ++i) CHECK(!w.amp[i].released);
    CHECK(!w.pins_on);
    for (unsigned i = 0; i < sizeof w.dma; ++i) if (w.dma[i]) {CHECK(!"the buffer is not zeroed at the end"); break;}
    return &session;
}

// The commands, as the DSP saw them.
static const char *kL4[] = {"PRM REQ 0x202 6144000", "PRM REL 0x202", "PRM VOTE 1", "PRM VOTE 2", "PRM REQ 0x30c 19200000", "PRM REL 0x30c",
                            "PRM UNVOTE 2", "PRM UNVOTE 1"};
static const char *kSpeakerUp[] = {"PRM REQ 0x202 6144000", "OPEN 0x4005", "CFG 0x6050", "CFG 0x6051", "CFG 0x6052", "OPEN 0x4001",
                                   "CFG 0x6001", "CFG 0x6002", "CFG 0x6004", "MAP 0x19da00000", "PREPARE 0x4005", "PREPARE 0x4001",
                                   "START 0x4005", "START 0x4001"};
static const char *kSpeakerDown[] = {"STOP 0x4001", "STOP 0x4005", "UNMAP 0x55", "CLOSE 0x4001", "CLOSE 0x4005", "PRM REL 0x202"};
static const char *kMic[] = {"PRM VOTE 1", "PRM VOTE 2", "PRM REQ 0x30c 19200000", "OPEN 0x4008", "CFG 0x6080", "CFG 0x6081",
                             "OPEN 0x4003", "CFG 0x6021", "CFG 0x6020", "MAP 0x19da10000", "PREPARE 0x4008", "PREPARE 0x4003",
                             "UNMAP 0x55", "CLOSE 0x4003", "CLOSE 0x4008", "PRM REL 0x30c", "PRM UNVOTE 2", "PRM UNVOTE 1"};
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

// ------------------------------------------------------------------------------------------------------------- the tests
static void testFullWalk(int ports)
{
    const struct Scenario sc = {7, 1, ports, 1, 1, 1, 1000, 60000, 0};
    const AudioSession *s = run(&sc, 0);
    CHECK(!w.violations);
    unsigned at = expect(0, kL4, N(kL4));
    at = expect(at, kSpeakerUp, N(kSpeakerUp));
    at = expect(at, kSpeakerDown, N(kSpeakerDown));
    at = expect(at, kMic, N(kMic));
    at = expect(at, kSpeakerUp, N(kSpeakerUp));
    at = expect(at, kSpeakerDown, N(kSpeakerDown));
    CHECK(at == w.ncmd);
    for (unsigned l = 4; l <= 7; ++l) CHECK(s->result[l] == AudioLevelPassed);
    CHECK(w.lpass_reads == 1 && w.smmu_calls == 1 && w.pins_calls == 2 && !w.clock_on && !w.va_votes && !w.va_mclk && !w.mapped);
    // the walk: four tones of 600 ms (rounded to the 20 ms ramps' zero samples), at the test level, four power-ups and unmutes
    CHECK(w.tone_frames > 4 * 590 * 48 && w.tone_frames <= 4 * 600 * 48 && w.peak > AUDIO_TEST_PEAK * 99 / 100 && w.peak <= AUDIO_TEST_PEAK);
    CHECK(w.power_ups == 4 && w.unmutes == 4);
    CHECK(logged("stream: done") && logged("the amplifiers unmuted") && logged("level 7: passed"));
    CHECK(logged("amp Top Left: first STATUS 0x0040 (CLKS)"));   // level 6: clocked, powered down
}

static void testNoTone(void)
{
    // level 7 without piano-audio-tone=1: levels 4-6 run, nothing is powered up, nothing but zeros reaches the DSP
    const struct Scenario sc = {7, 1, AudioPortsUbuntu, 0, 1, 1, 500, 60000, 0};
    const AudioSession *s = run(&sc, 0);
    CHECK(!w.violations && w.power_ups == 0 && w.unmutes == 0 && w.tone_frames == 0 && w.writes > 0);
    CHECK(s->result[6] == AudioLevelPassed && s->result[7] == AudioLevelSkipped && strstr(s->why[7], "piano-audio-tone=1"));
    unsigned at = expect(0, kL4, N(kL4));
    at = expect(at, kSpeakerUp, N(kSpeakerUp));
    at = expect(at, kSpeakerDown, N(kSpeakerDown));
    at = expect(at, kMic, N(kMic));
    CHECK(at == w.ncmd);
    // the silent stream ran its time (500 ms of 10 ms periods) and the amplifiers stayed down all along
    CHECK(w.writes >= 50 && w.writes <= 60 && logged("first STATUS"));
}

static int bus_swap(void *ctx) {(void)ctx; if (w.swapped) return 0; w.swapped = 1; return 1;}
static void swapped_bus(AudioSessionConfig *c, AudioSessionOps *o) {(void)c; o->bus_swap = bus_swap; w.need_swap = 1;}
static void wrong_bus(AudioSessionConfig *c, AudioSessionOps *o) {(void)c; o->bus_swap = 0; w.need_swap = 1;}
static void testBusOrientation(void)
{
    // SDA and SCL the other way round: nobody answers, the owner swaps them once, the four answer
    const struct Scenario sc = {5, 0, AudioPortsUbuntu, 0, 1, 1, 0, 30000, 0};
    const AudioSession *s = run(&sc, swapped_bus);
    CHECK(!w.violations && w.swapped && s->result[5] == AudioLevelPassed && logged("SDA and SCL swapped"));
    // no swap offered (piano-audio-i2c-swap=0 or 1): level 5 fails, all held
    s = run(&sc, wrong_bus);
    CHECK(s->result[5] == AudioLevelFailed && !w.swapped && logged("no answer within 100 ms"));
}

static void testLevels(void)
{
    // level 4 alone: the clocks, nothing else; the amplifiers untouched
    struct Scenario sc = {4, 1, AudioPortsUbuntu, 0, 1, 1, 0, 30000, 0};
    const AudioSession *s = run(&sc, 0);
    CHECK(!w.violations && s->result[4] == AudioLevelPassed && expect(0, kL4, N(kL4)) == w.ncmd && w.transfers == 0 && w.lpass_reads == 1);
    // level 5 without the DSP: the amplifiers alone, held in reset at the end, no packet at all
    sc = (struct Scenario){5, 0, AudioPortsUbuntu, 0, 1, 1, 0, 30000, 0};
    s = run(&sc, 0);
    CHECK(!w.violations && w.send_calls == 0 && s->result[4] == AudioLevelSkipped && s->result[5] == AudioLevelPassed && w.power_ups == 0);
    CHECK(logged("amp Top Left 0x35 (slot 0): DEVID 0x2900") && logged("configured: ") && logged("powered down"));
    // level 6 without the DSP: 4 skipped, 5 passed, 6 skipped
    sc.level = 6;
    s = run(&sc, 0);
    CHECK(s->result[5] == AudioLevelPassed && s->result[6] == AudioLevelSkipped && w.send_calls == 0);
}

static void no_lpass(AudioSessionConfig *c, AudioSessionOps *o) {(void)o; c->lpass_read = 0;}
static void testPreset(void)
{
    // the synthetic preset is not the stock file: refused without piano-audio-preset-any=1, nothing configured
    struct Scenario sc = {5, 1, AudioPortsUbuntu, 0, 0, 1, 0, 30000, 0};
    const AudioSession *s = run(&sc, no_lpass);
    CHECK(s->result[5] == AudioLevelFailed && logged("NOT the stock file") && logged("not the stock fs19xx.fsm") && w.lpass_reads == 0);
    // a hostile preset with any=1: configured, but its power, unmute and gain units are filtered (the model flags none)
    uint8_t keep[4096];
    const unsigned keep_bytes = preset_bytes;
    memcpy(keep, preset, sizeof keep);
    preset_bytes = sfsm_build(preset, sizeof preset, 1);
    sc.any = 1;
    s = run(&sc, 0);
    CHECK(!w.violations && s->result[5] == AudioLevelPassed && w.power_ups == 0 && w.unmutes == 0 && logged("3 changed by the safety filter"));
    // a damaged container
    preset[100] ^= 0xff;
    s = run(&sc, 0);
    CHECK(s->result[5] == AudioLevelFailed && logged("CRC16 mismatch"));
    memcpy(preset, keep, sizeof keep);
    preset_bytes = keep_bytes;
}

static void missing_amp(AudioSessionConfig *c, AudioSessionOps *o) {(void)c; (void)o; w.amp[2].present = 0;}
static void testMissingAmplifier(void)
{
    // one amplifier silent: level 5 fails and holds all four; level 6 still runs the graphs (amplifiers in reset); 7 is skipped
    const struct Scenario sc = {7, 1, AudioPortsUbuntu, 1, 1, 0, 300, 60000, 0};
    const AudioSession *s = run(&sc, missing_amp);
    CHECK(!w.violations && s->result[5] == AudioLevelFailed && s->result[6] == AudioLevelPassed && s->result[7] == AudioLevelSkipped);
    CHECK(w.power_ups == 0 && logged("amp Bottom Left 0x34 (slot 2): no answer within 100 ms") && strstr(s->why[7], "level 5"));
}

static const char *g_fail;
static int g_drop;
static uint32_t g_status;
static void arm_failure(AudioSessionConfig *c, AudioSessionOps *o) {(void)c; (void)o; w.fail_match = g_fail; w.drop = g_drop; w.fail_status = g_status;}

static void testRefusedAndUnanswered(void)
{
    const struct Scenario sc = {7, 1, AudioPortsUbuntu, 1, 1, 1, 300, 60000, 0};
    // the bit clock refused: level 4 fails at once (nothing to undo), no other DSP command; level 5 runs; 6 and 7 skipped
    g_fail = "PRM REQ 0x202 6144000"; g_drop = 0; g_status = 1;
    const AudioSession *s = run(&sc, arm_failure);
    CHECK(!w.violations && w.ncmd == 1 && s->result[4] == AudioLevelFailed && s->result[5] == AudioLevelPassed);
    CHECK(s->result[6] == AudioLevelSkipped && s->result[7] == AudioLevelSkipped && w.power_ups == 0 && s->failed_status[4] == 1);
    // the VA mclk unanswered: it may have run, so it is released too, then the votes
    g_fail = "PRM REQ 0x30c 19200000"; g_drop = 1;
    s = run(&sc, arm_failure);
    static const char *va_timeout[] = {"PRM REQ 0x202 6144000", "PRM REL 0x202", "PRM VOTE 1", "PRM VOTE 2", "PRM REQ 0x30c 19200000",
                                       "PRM REL 0x30c", "PRM UNVOTE 2", "PRM UNVOTE 1"};
    CHECK(expect(0, va_timeout, N(va_timeout)) == w.ncmd && s->result[4] == AudioLevelFailed && w.lpass_reads == 0 && s->timeouts == 1);
    // the stream graph's start refused at level 6: what was done undone in reverse, the amplifiers never on, 7 skipped
    g_fail = "START 0x4001"; g_drop = 0; g_status = 0x0b;
    s = run(&sc, arm_failure);
    static const char *undo6[] = {"STOP 0x4005", "UNMAP 0x55", "CLOSE 0x4001", "CLOSE 0x4005", "PRM REL 0x202"};
    unsigned at = expect(0, kL4, N(kL4));
    at = expect(at, kSpeakerUp, N(kSpeakerUp));
    CHECK(expect(at, undo6, N(undo6)) == w.ncmd && s->result[6] == AudioLevelFailed && s->result[7] == AudioLevelSkipped);
    CHECK(!w.violations && w.writes == 0 && w.power_ups == 0 && !strcmp(s->failed_step[6], "start stream") && s->failed_status[6] == 0x0b);
    // the capture stream's open refused: the mic plan undone, level 6 failed (its speaker part passed), 7 skipped
    g_fail = "OPEN 0x4003"; g_status = 1;
    s = run(&sc, arm_failure);
    CHECK(!w.violations && s->result[6] == AudioLevelFailed && s->result[7] == AudioLevelSkipped && w.power_ups == 0);
    static const char *undo_mic[] = {"CLOSE 0x4008", "PRM REL 0x30c", "PRM UNVOTE 2", "PRM UNVOTE 1"};
    CHECK(!strcmp(w.cmd[w.ncmd - 5], "OPEN 0x4003") && expect(w.ncmd - 4, undo_mic, 4) == w.ncmd);
    g_fail = 0;
}

// Level 7's own command refused or unanswered: armed once level 7 starts (the first commands of level 6 look the same).
static const char *g_l7_fail;
static int g_l7_drop;
static void arm_at_l7(AudioSession *s, uint32_t now)
{
    (void)now;
    if (g_l7_fail && !w.fail_match && !strcmp(audio_session_phase(s->phase), "level 7 speaker plan")) {
        w.fail_match = g_l7_fail; w.drop = g_l7_drop; w.fail_status = 0x0c; w.fail_seen = 0;
    }
}
static void testLevel7Failures(void)
{
    const struct Scenario sc = {7, 1, AudioPortsUbuntu, 1, 1, 0, 300, 60000, arm_at_l7};
    // the device start unanswered: it may have run, so it is stopped; the amplifiers were never on
    g_l7_fail = "START 0x4005"; g_l7_drop = 1;
    const AudioSession *s = run(&sc, 0);
    static const char *undo[] = {"STOP 0x4005", "UNMAP 0x55", "CLOSE 0x4001", "CLOSE 0x4005", "PRM REL 0x202"};
    CHECK(!w.violations && s->result[7] == AudioLevelFailed && w.power_ups == 0 && expect(w.ncmd - N(undo), undo, N(undo)) == w.ncmd);
    // the stream start refused: the device graph stopped, the amplifiers never on
    g_l7_fail = "START 0x4001"; g_l7_drop = 0;
    s = run(&sc, 0);
    CHECK(!w.violations && s->result[7] == AudioLevelFailed && w.power_ups == 0 && !strcmp(w.cmd[w.ncmd - 6], "START 0x4001"));
    g_l7_fail = 0;
}

// ---- trouble while the tone plays: injected at a time into the walk
static uint32_t g_at;
static int g_kind;
enum {KFatal = 1, KLink, KQuiet, KAmp, KLate, KWriteError, KAbortSoft, KAbortHard, KCharger};
static uint32_t g_trip_ms, g_quiet_end;
static void trouble(AudioSession *s, uint32_t now)
{
    if (strcmp(audio_session_phase(s->phase), "level 7 tone stream") || !s->unmuted || g_trip_ms) {
        if (g_kind == KQuiet && g_trip_ms && now >= g_quiet_end) w.o.quiet = 0;
        return;
    }
    if (now < s->unmuted_at + g_at) return;
    g_trip_ms = now;
    switch (g_kind) {
    case KFatal: w.o.fatal_reports++; w.off = 1; break;
    case KLink: w.o.link_generation++; w.off = 1; break;
    case KCharger: w.o.charger_generation++; w.off = 1; break;
    case KQuiet: w.o.quiet = 1; g_quiet_end = now + 3000; break;
    case KAmp: w.amp[1].inject = 0x0100; break;   // OTWDS on the top right amplifier
    case KLate: w.drop_writes_after = (int)w.writes; break;
    case KWriteError: w.write_error_at = (int)w.writes + 1; break;
    case KAbortSoft: audio_session_abort(s, 0, now); break;
    case KAbortHard: audio_session_abort(s, 1, now); w.off = 1; break;
    }
}

static void testTrouble(void)
{
    static const struct {int kind; const char *why; int dsp_undo;} cases[] = {
        {KFatal, "the DSP link went off", 0}, {KLink, "the DSP link went off", 0}, {KCharger, "the DSP link went off", 0},
        {KQuiet, "PianoPower asked for quiet", 1}, {KAmp, "amplifier status", 1}, {KLate, "late period", 1},
        {KWriteError, "DSP error", 1}, {KAbortSoft, "stopped by the owner", 1}, {KAbortHard, "stopped by the owner", 0},
    };
    for (unsigned i = 0; i < N(cases); ++i) {
        g_kind = cases[i].kind; g_at = 700; g_trip_ms = 0; g_quiet_end = 0;
        const struct Scenario sc = {7, 1, AudioPortsUbuntu, 1, 1, 0, 300, 60000, trouble};
        const AudioSession *s = run(&sc, 0);
        CHECK(g_trip_ms && !w.violations);
        CHECK(s->result[7] == AudioLevelFailed && s->why[7] && strstr(s->why[7], cases[i].why));
        // the amplifiers went down within one status period (plus the time the shut-down sequence takes) of the trouble
        CHECK(!any_powered());
        CHECK(w.tone_frames < (uint64_t)(g_at + 400) * 48);
        if (cases[i].dsp_undo) {
            // with the DSP still there, the plan is undone with its commands, the amplifiers first
            CHECK(expect(w.ncmd - N(kSpeakerDown), kSpeakerDown, N(kSpeakerDown)) == w.ncmd);
        } else {
            // without it (or told so), nothing goes to the DSP any more
            CHECK(w.sends_after_off == 0 && !strcmp(w.cmd[w.ncmd - 1], "START 0x4001"));
        }
        if (g_kind == KQuiet) CHECK(logged("PianoPower asks for quiet") && logged("quiet over"));
    }
}

// The walk outlasting its 5 s watchdog (a DSP that plays slowly): stopped at 5 s.
static void slow(AudioSessionConfig *c, AudioSessionOps *o)
{
    (void)o;
    c->tone_ms = 700; c->gap_ms = 400;   // 2.8 s of tone, 4.0 s in all: the walk's limits
    w.period_us = 14000;                  // each 10 ms period takes 14 ms
}
static void testWatchdog(void)
{
    const struct Scenario sc = {7, 1, AudioPortsUbuntu, 1, 1, 0, 300, 60000, 0};
    const AudioSession *s = run(&sc, slow);
    CHECK(!w.violations && s->result[7] == AudioLevelFailed && !strcmp(s->why[7], "watchdog") && logged("STOPPED: watchdog after 5000 ms"));
    CHECK(expect(w.ncmd - N(kSpeakerDown), kSpeakerDown, N(kSpeakerDown)) == w.ncmd);
}

// The walk's limits refused at the start: level 7 does not play.
static void too_long(AudioSessionConfig *c, AudioSessionOps *o) {(void)o; c->tone_ms = 1000;}   // 4 s of tone
static void testRefusedWalk(void)
{
    const struct Scenario sc = {7, 1, AudioPortsUbuntu, 1, 1, 0, 300, 60000, 0};
    const AudioSession *s = run(&sc, too_long);
    CHECK(!w.violations && w.power_ups == 0 && s->result[7] == AudioLevelSkipped && logged("tone REFUSED"));
}

// The DSP's queue full now and then: every packet still goes out once, in order.
static void full_queue(AudioSessionConfig *c, AudioSessionOps *o) {(void)c; (void)o; w.queue_full_every = 3;}
static void testQueueFull(void)
{
    const struct Scenario sc = {7, 1, AudioPortsUbuntu, 1, 1, 1, 500, 60000, 0};
    const AudioSession *s = run(&sc, full_queue);
    CHECK(!w.violations && s->result[7] == AudioLevelPassed && s->queue_full > 10);
    unsigned at = expect(0, kL4, N(kL4));
    at = expect(at, kSpeakerUp, N(kSpeakerUp));
    at = expect(at, kSpeakerDown, N(kSpeakerDown));
    at = expect(at, kMic, N(kMic));
    at = expect(at, kSpeakerUp, N(kSpeakerUp));
    CHECK(expect(at, kSpeakerDown, N(kSpeakerDown)) == w.ncmd);
}

// The DSP's health goes bad during level 6's silence: no amplifier was on; nothing more to the DSP.
static void link_in_silence(AudioSession *s, uint32_t now)
{
    if (!g_trip_ms && !strcmp(audio_session_phase(s->phase), "level 6 silent stream") && now >= s->stream_at + 200) {
        g_trip_ms = now; w.o.link_generation++; w.off = 1;
    }
}
static void testLinkInSilence(void)
{
    g_trip_ms = 0;
    const struct Scenario sc = {7, 1, AudioPortsUbuntu, 1, 1, 1, 1000, 60000, link_in_silence};
    const AudioSession *s = run(&sc, 0);
    CHECK(g_trip_ms && !w.violations && w.sends_after_off == 0 && w.power_ups == 0);
    CHECK(s->result[6] == AudioLevelFailed && s->result[7] == AudioLevelSkipped && logged("guard OFF"));
}

int main(void)
{
    preset_bytes = sfsm_build(preset, sizeof preset, 0);
    CHECK(preset_bytes > 100 && fsm_check(preset, preset_bytes, 0) == FsmOk);
    testFullWalk(AudioPortsUbuntu);
    testFullWalk(AudioPortsSheng);
    testFullWalk(AudioPortsSun);
    testNoTone();
    testLevels();
    testBusOrientation();
    testPreset();
    testMissingAmplifier();
    
    testRefusedAndUnanswered();
    testLevel7Failures();
    testTrouble();
    testWatchdog();
    testRefusedWalk();
    testQueueFull();
    testLinkInSilence();
    printf("PASS session: %u checks\n", checks);
    return 0;
}
