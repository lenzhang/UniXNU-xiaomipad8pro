/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The pieces of the DSP's health (piano_adsp_health) and of the quiet flag (piano_adsp_quiet) that are decisions rather than reads:
 * when the DSP counts as up, how the link generation moves, and when PianoPower raises and drops the quiet for a charger generation.
 * Portable C, header only, host-tested (tests/ports_host.cpp). */
#ifndef ADSP_HEALTH_H
#define ADSP_HEALTH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define ADSP_HEALTH_READY_STAGE 18      /* PianoAdspReady */
#define ADSP_SLAVE_FATAL 1u             /* slave-kernel bit 0 */
#define ADSP_SLAVE_READY 2u             /* slave-kernel bit 1 */

/* Up: past the ready stage (not failed), ready and not fatal on SMP2P, GLINK agreed and not broken, the router's link has its hello. */
static inline int adsp_health_dsp_up(int32_t stage, uint32_t slave, int glink_ok, int router_up)
{
    return stage >= ADSP_HEALTH_READY_STAGE && (slave & ADSP_SLAVE_READY) && !(slave & ADSP_SLAVE_FATAL) && glink_ok && router_up;
}

/* A channel registration fits: a free slot, and two of the slots always left for PianoPower's own two channels (the router's and the
 * charger's), which may register after the clients' (registered: all so far; others: the clients' among them; own: this is one of
 * PianoPower's). */
static inline int adsp_channel_room(unsigned registered, unsigned others, int own, unsigned slots)
{
    return registered < slots && (own || others + 2 < slots);
}

/* The link generation: moves each time a channel that was up goes down, and when the transport breaks (once per break). */
typedef struct {
    uint32_t up_mask, generation;
    uint8_t broken;
} AdspLinkTrack;

static inline void adsp_link_channel(AdspLinkTrack *t, unsigned index, int up)
{
    const uint32_t bit = 1u << (index & 31);
    if (up) {t->up_mask |= bit; return;}
    if (t->up_mask & bit) {t->up_mask &= ~bit; t->generation++;}
}

static inline void adsp_link_broken(AdspLinkTrack *t, int broken)
{
    if (broken && !t->broken) t->generation++;
    t->broken = broken ? 1 : 0;
}

/* The quiet for one charger generation: raised when the charger codec links (a new generation: links moved), dropped when the charger
 * is configured (piano_mca_configured), when the codec is unlinked, or after cap_ms. */
enum {PowerQuietNone = 0, PowerQuietRaised = 1, PowerQuietReleased = 2, PowerQuietTimedOut = 3, PowerQuietDropped = 4};
typedef struct {
    uint8_t raised;
    uint32_t links_seen;
    uint64_t since_ms;
    uint32_t raises, releases, timeouts;
} PowerQuiet;

/* One pass: linked = the codec talks to a charger generation, links = its link count, configured = that generation is configured.
 * Returns a PowerQuiet* event; q->raised is the flag. */
static inline int power_quiet_step(PowerQuiet *q, int linked, uint32_t links, int configured, uint64_t now_ms, uint32_t cap_ms)
{
    if (linked && links != q->links_seen) {
        q->links_seen = links;
        q->since_ms = now_ms;
        q->raised = 1;
        q->raises++;
        return PowerQuietRaised;
    }
    if (!q->raised) return PowerQuietNone;
    if (!linked) {q->raised = 0; return PowerQuietDropped;}
    if (configured) {q->raised = 0; q->releases++; return PowerQuietReleased;}
    if (now_ms - q->since_ms >= cap_ms) {q->raised = 0; q->timeouts++; return PowerQuietTimedOut;}
    return PowerQuietNone;
}

#ifdef __cplusplus
}
#endif
#endif
