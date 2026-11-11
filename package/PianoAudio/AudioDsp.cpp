// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio level 2: the DSP link over PianoPower's client contract (package/include/PianoAdspContract.h, docs/drivers/charging.md
// section 9). The channel "adsp_apps" is registered (the DSP opens it: we answer; 20 receive intents of 512 bytes, the DT's
// qcom,intents), PianoPower's health is watched through AudioDspLink.h's guard, and one question goes to the DSP's graph manager:
// is the framework ready (APM_CMD_GET_SPF_STATE from the port set's APM port: Ubuntu's port 1 by default, piano-audio-ports=N for
// another, piano-audio-ports-try=1 to walk the others while nobody answers). Nothing else is sent at this level.
// Levels 4-7 (piano_audio_levels, AudioSession.h) then run on the same channel, inbox and guard, with the hardware of AudioBoard.cpp.
// PianoAudio never starts, stops or restarts the DSP and votes nothing: PianoPower owns it (piano-power>=2 in the same boot).
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoAdspContract.h>
#include "AudioLog.h"
#include "AudioBoard.h"
#include "AudioDspLink.h"
#include "AudioSession.h"

extern "C" {
// Post-mortem: the link's state (AudioLink AlWaiting..AlOff) | the guard's reason << 8 | queries << 16.
volatile uint32_t piano_audio_dsp_word = 0;
// Post-mortem: levels 4-7: the session's phase | the results of levels 4..7 << 8 (two bits each: AudioLevel*).
volatile uint32_t piano_audio_levels_word = 0;
}

namespace {

AudioInbox gInbox;
AudioLink gLink;
PianoAdspChannel *gChannel;
int gChannelUp;

void onState(void *, int up) {__atomic_store_n(&gChannelUp, up ? 1 : 0, __ATOMIC_RELEASE);}
// On PianoPower's DSP thread: copy and return (the contract's rule 2).
void onReceive(void *, const uint8_t *data, uint32_t length) {audio_inbox_put(&gInbox, data, length);}
const PianoAdspClient kClient = {onState, onReceive};

uint32_t nowMs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return uint32_t(ns / 1000000);
}

void observe(AdgObservation &o, const PianoAdspHealth &h)
{
    o.contract_ok = h.version == PIANO_ADSP_HEALTH_VERSION;
    o.dsp_up = h.dsp_up != 0;
    o.fatal = h.fatal != 0;
    o.fatal_reports = h.fatal_reports;
    o.link_generation = h.link_generation;
    o.charger_pd = h.charger_pd;
    o.charger_generation = h.charger_generation;
    o.quiet = h.quiet;
    o.crash = h.crash;
    o.channel_up = __atomic_load_n(&gChannelUp, __ATOMIC_ACQUIRE);
}

}  // namespace

// Runs on PianoAudio's thread until the exchange ends (ready, gave up, off) or wait_s pass; the summary for the log and ioreg.
extern "C" int piano_audio_dsp_link(uint32_t wait_s, uint32_t settle_ms, int ports, int try_ports, char *summary, size_t size)
{
    audio_inbox_init(&gInbox);
    audio_link_init_ports(&gLink, ports, try_ports ? (1u << AUDIO_PORT_SETS) - 1u : 0u, 10, settle_ms);
    const AudioPorts *set = audio_ports(gLink.set);
    piano_audio_log("dsp: GPR ports: set %d (%s: %s): APM from %#x, PRM from %#x, data from %#x%s", gLink.set, set->name, set->source,
                    set->apm, set->prm, set->data, try_ports ? "; the others are tried while nobody answers" : "");
    int lastSet = gLink.set;
    gChannel = piano_adsp_channel("adsp_apps", AUDIO_INBOX_BYTES, 20, 0, &kClient, nullptr);
    if (!gChannel) {
        snprintf(summary, size, "adsp_apps refused by PianoPower (name taken or slots full)");
        piano_audio_log("dsp: %s", summary);
        return -1;
    }
    piano_audio_log("dsp: channel adsp_apps registered (20 x %u-byte intents; the DSP opens it, we answer)", AUDIO_INBOX_BYTES);
    const uint32_t start = nowMs();
    int lastGuard = -1, lastChannel = -1;
    unsigned lastQueries = 0;
    uint8_t q[GPR_HEADER_BYTES], in[AUDIO_INBOX_BYTES];
    PianoAdspHealth h;
    AdgObservation o;
    while (gLink.state != AlReady && gLink.state != AlGaveUp && gLink.state != AlOff) {
        const uint32_t now = nowMs();
        if (now - start >= wait_s * 1000) break;
        bzero(&h, sizeof h);
        piano_adsp_health(&h);
        observe(o, h);
        for (uint32_t n; (n = audio_inbox_get(&gInbox, in, sizeof in)) != 0;) audio_link_receive(&gLink, in, n, now);
        const uint32_t n = audio_link_tick(&gLink, &o, now, q);
        if (n && piano_adsp_send(gChannel, q, n)) audio_link_unsent(&gLink);   // the queue is full: asked again at the next tick
        if (o.channel_up != lastChannel) {
            piano_audio_log("dsp: channel adsp_apps %s (PianoPower stage %d, dsp_up %u, charger_pd %#x gen %u, link gen %u, quiet %#x)",
                            o.channel_up ? "up" : "not up", h.stage, h.dsp_up, h.charger_pd, h.charger_generation, h.link_generation, h.quiet);
            lastChannel = o.channel_up;
        }
        if (gLink.guard.state != lastGuard) {
            piano_audio_log("dsp: guard %s%s%s", gLink.guard.state == AdgRun ? "run" : gLink.guard.state == AdgWait ? "wait" : "OFF",
                            gLink.guard.state == AdgOff ? ": " : "", gLink.guard.state == AdgOff ? adg_reason(gLink.guard.reason) : "");
            lastGuard = gLink.guard.state;
        }
        if (gLink.set != lastSet) {
            const AudioPorts *now = audio_ports(gLink.set);
            piano_audio_log("dsp: nobody answered from port %#x: GPR ports set %d (%s: %s) now: APM %#x, PRM %#x, data %#x",
                            audio_ports(lastSet)->apm, gLink.set, now->name, now->source, now->apm, now->prm, now->data);
            lastSet = gLink.set;
            lastQueries = 0;
        }
        if (gLink.spf.queries != lastQueries) {
            piano_audio_log("dsp: APM_CMD_GET_SPF_STATE %u sent from port %#x (so far: %u answers, last state %u, %u refused, %u timeouts)",
                            gLink.spf.queries, gLink.spf.port, gLink.spf.answers, gLink.spf.last_status, gLink.spf.errors, gLink.spf.timeouts);
            lastQueries = gLink.spf.queries;
        }
        piano_audio_dsp_word = uint32_t(gLink.state) | uint32_t(gLink.guard.reason) << 8 | gLink.spf.queries << 16;
        IOSleep(20);
    }
    piano_audio_dsp_word = uint32_t(gLink.state) | uint32_t(gLink.guard.reason) << 8 | gLink.spf.queries << 16;
    if (gLink.state == AlReady)
        snprintf(summary, size, "APM ready (status 1) after %u queries from port %#x (set %d, %s), %u ms after the first; inbox dropped %u",
                 gLink.spf.queries, gLink.spf.port, gLink.set, audio_ports(gLink.set)->name, gLink.ready_at - gLink.first_sent,
                 gInbox.dropped + gInbox.too_long);
    else if (gLink.state == AlOff)
        snprintf(summary, size, "stopped for the rest of the boot: %s (crash \"%s\")", adg_reason(gLink.guard.reason), h.crash);
    else if (gLink.state == AlGaveUp)
        snprintf(summary, size, "APM not ready after %u queries from port %#x (last status %u, refused %u, timeouts %u; sets tried %#x)",
                 gLink.spf.queries, gLink.spf.port, gLink.spf.last_status, gLink.spf.errors, gLink.spf.timeouts, gLink.tried);
    else
        snprintf(summary, size, "no answer within %u s (channel %s, guard %s, %u queries)", wait_s, o.channel_up ? "up" : "not up",
                 gLink.guard.state == AdgRun ? "run" : "wait", gLink.spf.queries);
    piano_audio_log("dsp: %s", summary);
    return gLink.state == AlReady ? 0 : -1;
}

// ------------------------------------------------------------------------------------------------------- levels 4-7
namespace {

AudioSession gSession;

void sessionLog(void *, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sessionLog(void *, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    IOLog("PianoAudio: %s\n", line);
}

int sessionSend(void *, const uint8_t *packet, uint32_t length)
{
    return gChannel && piano_adsp_send(gChannel, packet, length) == 0 ? 0 : -1;
}

uint32_t levelsWord(const AudioSession &s)
{
    uint32_t w = uint32_t(s.phase) & 0xffu;
    for (unsigned l = 4; l <= 7; ++l) w |= (uint32_t(s.result[l]) & 3u) << (8 + 2 * (l - 4));
    return w;
}

}  // namespace

int piano_audio_levels(const PianoAudioLevelArgs *a, char *summary, size_t size)
{
    AudioSessionConfig c;
    audio_session_defaults(&c);
    c.level = a->level;
    c.dsp = a->dsp && gChannel && gLink.state == AlReady;
    c.ports = gLink.set >= 0 ? gLink.set : AudioPortsUbuntu;
    c.lpass_read = int(a->lpassRead);
    c.preset_any = int(a->presetAny);
    c.mic = int(a->mic);
    c.silence_ms = a->silenceMs;
    c.tone = a->tone == 1 ? 1 : 0;
    c.tone_mask = a->toneMask;
    c.tone_ms = a->toneMs;
    c.gap_ms = a->gapMs;
    c.tone_gain = a->toneGain;
    AudioSessionOps ops = {};
    ops.log = sessionLog;
    ops.send = sessionSend;
    const pianoaudio::BoardArgs b = {a->level, a->i2cSwap, a->i2cHalfUs, a->smmu};
    pianoaudio::boardSetup(ops, b);
    audio_session_init(&gSession, &c, &ops, c.dsp ? &gLink.guard : nullptr);
    // A budget well beyond what the levels need (level 6's silence and level 7's walk are seconds; every command is answered or
    // times out in 2 s): past it the session is stopped, with its undo commands first, then without.
    const uint32_t start = nowMs(), budget = 180000u + (c.silence_ms < AUDIO_SILENCE_MAX_MS ? c.silence_ms : AUDIO_SILENCE_MAX_MS);
    uint32_t aborted = 0;
    uint8_t in[AUDIO_INBOX_BYTES];
    PianoAdspHealth h;
    AdgObservation o;
    while (!gSession.finished) {
        const uint32_t now = nowMs();
        if (c.dsp) {
            bzero(&h, sizeof h);
            piano_adsp_health(&h);
            observe(o, h);
            for (uint32_t n; (n = audio_inbox_get(&gInbox, in, sizeof in)) != 0;) audio_session_receive(&gSession, in, n, now);
        }
        const uint32_t d = audio_session_tick(&gSession, c.dsp ? &o : nullptr, now);
        piano_audio_levels_word = levelsWord(gSession);
        if (!d) break;
        if (!aborted && now - start > budget) {
            aborted = now;
            audio_session_abort(&gSession, 0, now);
        } else if (aborted && now - aborted > 30000u) {
            audio_session_abort(&gSession, 1, now);
        }
        IOSleep(d);
    }
    piano_audio_levels_word = levelsWord(gSession);
    char board[400];
    pianoaudio::boardSummary(board, sizeof board);
    pianoaudio::boardTeardown();
    size_t n = 0;
    int failed = 0;
    for (unsigned l = 4; l <= c.level && l <= 7 && n < size; ++l) {
        n += size_t(snprintf(summary + n, size - n, "%s%u %s", l > 4 ? ", " : "", l, audio_level_result(gSession.result[l])));
        failed |= gSession.result[l] == AudioLevelFailed;
    }
    piano_audio_log("levels: %s; %s", summary, board);
    return failed ? -1 : 0;
}
