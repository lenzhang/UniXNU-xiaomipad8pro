/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The safety governor of piano's sensors (docs/drivers/sensors.md section 7). The audio DSP that hosts the sensor protection domain also
 * runs the charger firmware (charging.md); natively nothing restarts that DSP (no subsystem restart), so one DSP crash ends charging until
 * the next boot, and Ubuntu's issue #3 showed the sensor PD's watchdog taking the whole DSP down (device.md section 2.14). The sensors are
 * a client of the running DSP: they never start, stop or restart it and never vote its rails (PianoPower does, piano-power>=2). This file
 * decides when the client may act at all. Portable C, integer only, no allocation, no C library; host-tested (tests/piano.c).
 *
 * The glue feeds it one observation of the DSP per tick (read only, from PianoPower's status) and asks it before each request it starts;
 * the DSP's own file calls are never refused silently: in SG_OFF the file service still answers each call, with an error, at once (a call
 * left unanswered blocks a sensor PD thread, which is what its watchdog kills the DSP for).
 *
 *   SG_WAIT     the DSP is not up, its charger domain is not up, PianoPower asks for quiet, or the charger's newest generation has not
 *               been up for settle_ms: nothing is sent (the charger's own configuration goes first)
 *   SG_RUN      requests may go, at most bucket at once, refilled at refill_per_s
 *   SG_BACKOFF  a request failed: nothing new until the backoff ends (first_ms, doubling to max_ms; a success resets it)
 *   SG_OFF      for the rest of the boot: the DSP's fatal bit or a crash reason, the charger domain restarted (a new generation) or the
 *               link went down after we started, strikes failures within window_ms, or the DSP asking for files in a loop */
#ifndef SENSORS_GUARD_H
#define SENSORS_GUARD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {SG_WAIT = 0, SG_RUN = 1, SG_BACKOFF = 2, SG_OFF = 3};
enum {
    SG_REASON_NONE = 0, SG_REASON_FATAL = 1, SG_REASON_CRASH = 2, SG_REASON_CHARGER = 3, SG_REASON_LINK = 4, SG_REASON_ERRORS = 5,
    SG_REASON_FILE_STORM = 6, SG_REASON_DISABLED = 7,
};
#define SG_CHARGER_UP 0x1fffffffu        /* SERVREG_STATE_UP, as piano_qrtr_charger_pd() reports it */

typedef struct {
    uint32_t settle_ms;                  /* the charger domain up this long before the first action (default 20 s) */
    uint32_t backoff_first_ms, backoff_max_ms;   /* 1 s, 64 s */
    uint32_t strikes, window_ms;         /* this many failures within the window: off (6 in 10 min) */
    uint32_t bucket, refill_per_s;       /* our requests: 8 at once, 4 per second */
    uint32_t file_calls_per_min;         /* more DSP file calls than this in one minute: a loop (default 3000) */
} SgConfig;
void sg_defaults(SgConfig *c);

/* One read-only observation of the DSP (PianoPower's piano_adsp_status / piano_qrtr_charger_pd / the requested health call). */
typedef struct {
    int dsp_up;                          /* the DSP said ready on SMP2P and its GLINK link is up */
    int fatal;                           /* the SMP2P fatal bit (slave-kernel bit 0) */
    int quiet;                           /* PianoPower asks clients to hold still (charger configuration in progress) */
    uint32_t charger_state;              /* SG_CHARGER_UP when the charger domain is up */
    uint32_t charger_ups;                /* the charger domain's generation (its transitions to up) */
    uint32_t link_generation;            /* the GLINK link's generation (moves on every down) */
    const char *crash;                   /* the SMEM crash reason, null or empty when there is none */
} SgDsp;

typedef struct {
    SgConfig cfg;
    int state, reason;
    int started;                         /* SG_RUN reached once: the baselines below hold */
    int up_seen;                         /* the DSP and its charger domain are up (not yet started) */
    uint64_t up_since;                   /* since when, in the charger's current generation */
    uint32_t charger_ups0, link0;
    uint64_t backoff_until;
    uint32_t backoff_ms;
    uint64_t strike_at[16];              /* the last failures (a ring of cfg.strikes <= 16) */
    uint32_t strike_next, strike_count;
    uint32_t tokens_milli;               /* the bucket, in thousandths of a token */
    uint64_t refill_at;
    uint64_t minute_start;
    uint32_t minute_calls;
    uint32_t requests, refused, failures, file_calls;
} SgGuard;

void sg_init(SgGuard *g, const SgConfig *config);
/* One observation; returns the state. */
int sg_observe(SgGuard *g, const SgDsp *d, uint64_t now_ms);
/* Before each request of ours (a QMI request, a FastRPC invoke we start): 1 and a token taken, or 0 (wait, backoff, off, no token). */
int sg_may_request(SgGuard *g, uint64_t now_ms);
/* The outcome of a request (or an error event of the hub): failures back off and count toward SG_OFF; a success ends the backoff. */
void sg_result(SgGuard *g, int ok, uint64_t now_ms);
/* Each file call of the DSP served or declined: counts toward the loop detector. */
void sg_file_call(SgGuard *g, uint64_t now_ms);
/* 1 when the file service serves the DSP's calls; 0 when it declines them (answers with an error, at once). */
int sg_serve_files(const SgGuard *g);
/* The owner turns the sensors off (a boot argument, a sysctl): SG_OFF with SG_REASON_DISABLED. */
void sg_disable(SgGuard *g);
const char *sg_state_name(int state);
const char *sg_reason_name(int reason);
/* 1 when a crash reason names the sensor framework (qsh_process, sns_, the sensor watchdog: device.md 2.14, Ubuntu's sensors-prepare). */
int sg_crash_is_sensors(const char *crash);

#ifdef __cplusplus
}
#endif
#endif
