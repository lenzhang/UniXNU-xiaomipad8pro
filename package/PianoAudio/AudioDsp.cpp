// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio level 2: the DSP link over PianoPower's client contract (package/include/PianoAdspContract.h, docs/drivers/charging.md
// section 9). The channel "adsp_apps" is registered (the DSP opens it: we answer; 20 receive intents of 512 bytes, the DT's
// qcom,intents), PianoPower's health is watched through AudioDspLink.h's guard, and one question goes to the DSP's graph manager:
// is the framework ready (APM_CMD_GET_SPF_STATE from GPR port 1, as Ubuntu's q6apm asks it). Nothing else is sent at this level.
// PianoAudio never starts, stops or restarts the DSP and votes nothing: PianoPower owns it (piano-power>=2 in the same boot).
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <PianoAdspContract.h>
#include "AudioLog.h"
#include "AudioDspLink.h"

extern "C" {
// Post-mortem: the link's state (AudioLink AlWaiting..AlOff) | the guard's reason << 8 | queries << 16.
volatile uint32_t piano_audio_dsp_word = 0;
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
extern "C" int piano_audio_dsp_link(uint32_t wait_s, uint32_t settle_ms, char *summary, size_t size)
{
    audio_inbox_init(&gInbox);
    audio_link_init(&gLink, GPR_PORT_APM, 10, settle_ms);
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
        if (gLink.spf.queries != lastQueries) {
            piano_audio_log("dsp: APM_CMD_GET_SPF_STATE %u sent (so far: %u answers, last state %u, %u refused, %u timeouts)",
                            gLink.spf.queries, gLink.spf.answers, gLink.spf.last_status, gLink.spf.errors, gLink.spf.timeouts);
            lastQueries = gLink.spf.queries;
        }
        piano_audio_dsp_word = uint32_t(gLink.state) | uint32_t(gLink.guard.reason) << 8 | gLink.spf.queries << 16;
        IOSleep(20);
    }
    piano_audio_dsp_word = uint32_t(gLink.state) | uint32_t(gLink.guard.reason) << 8 | gLink.spf.queries << 16;
    if (gLink.state == AlReady)
        snprintf(summary, size, "APM ready (status 1) after %u queries, %u ms after the first; inbox dropped %u", gLink.spf.queries,
                 gLink.ready_at - gLink.first_sent, gInbox.dropped + gInbox.too_long);
    else if (gLink.state == AlOff)
        snprintf(summary, size, "stopped for the rest of the boot: %s (crash \"%s\")", adg_reason(gLink.guard.reason), h.crash);
    else if (gLink.state == AlGaveUp)
        snprintf(summary, size, "APM not ready after %u queries (last status %u, refused %u, timeouts %u)", gLink.spf.queries,
                 gLink.spf.last_status, gLink.spf.errors, gLink.spf.timeouts);
    else
        snprintf(summary, size, "no answer within %u s (channel %s, guard %s, %u queries)", wait_s, o.channel_up ? "up" : "not up",
                 gLink.guard.state == AdgRun ? "run" : "wait", gLink.spf.queries);
    piano_audio_log("dsp: %s", summary);
    return gLink.state == AlReady ? 0 : -1;
}
