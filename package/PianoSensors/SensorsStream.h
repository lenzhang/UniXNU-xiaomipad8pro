/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* Level 4 of PianoSensors (docs/drivers/sensors.md section 5c): the two streams macOS's consumers need, each on a QMI client of its own
 * (its own QRTR port: libssc and Android's sensors HAL keep one client per sensor) of the hub's sensor client service 400:
 *
 *   light  the default "ambient_light" sensor on change (request 514, no payload; 513 at its lowest advertised rate if its attributes
 *          say it is a streaming sensor), wanted while a consumer takes its targets, the display is on and the cover's lid is open.
 *          Each sample (event 1025, data[0] = lux) goes through AutoLight.h (SensorsPolicy's ps_als_step: curve, smoothing,
 *          hysteresis, a fresh start after every off/on) into the outputs lux and target (permille).
 *   accel  the default "accel" sensor streaming at 10 Hz (request 513 {sample_rate}), wanted while the rotation agent reads the edge,
 *          the display is on and the keyboard cover is detached. Each sample (event 1025, data[0..2] = m/s^2) goes through
 *          ps_accel_sample (SensorsFixed's axis map, SensorsOrient's state machine) into the output edge (ps_edge_of).
 *   Both are switched off with message 10 (disable, no payload). The stream's request goes to its sensor's SUID (the level-3 list,
 *   SensorsClient.h, found it); every request goes out only with the guard's token (SensorsGuard.h's sg_may_request) and every
 *   outcome is told to it (a response, a refusal, a missing response, an error event).
 *
 * The off/on rules are AutoStream.h's (Ubuntu v1.1.1's fix as a rule): one request outstanding per client, no nested close, the first
 * event kept also before the enable's answer, a re-arm when it never comes, a disable first when the hub's state is unknown. The
 * outputs change only when their value does; a sequence number moves with every change (the consumers poll it, sensors.md section 5).
 * Stream on/off and its failures are logged through a limiter per stream (a burst of 8 lines, then one per 30 s, the lines left out
 * counted in the next one); the counters in ss_status are always complete.
 *
 * Teardown: a condition that goes away (display, lid, cover, consumer) disables the stream; a system sleep (ss_sleep) disables both
 * and, for a stream that is not off within the glue's wait, ss_force_off closes its port (the hub forgets a client whose port is gone:
 * QRTR DEL_CLIENT); the guard's strike (ss_off) sends nothing more: the outputs go to rest (the edge "none", the light's consumer told
 * to leave automatic mode once), every later packet is ignored, and the ports that carried a stream are closed when the glue says the
 * DSP may still be told (ss_close_all). Portable C, single threaded (the driver's own thread), integer only, no allocation, no C
 * library; host-tested against a simulated hub (tests/streams.c). */
#ifndef SENSORS_STREAM_H
#define SENSORS_STREAM_H
#include <stdint.h>
#include "SensorsSsc.h"
#include "SensorsClient.h"
#include "SensorsPolicy.h"
#include "AutoLight.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {SS_LIGHT = 0, SS_ACCEL = 1, SS_STREAMS = 2};
enum {SS_OUT_LUX = 1, SS_OUT_TARGET = 2, SS_OUT_EDGE = 4};
#define SS_ACCEL_HZ 10u                  /* the rotation's rate (sensors.md 5a.5): the lowest that still turns within a second */
#define SS_LUX_MS 250u                   /* the lux output moves at most this often between targets */
#define SS_LOG_BURST 8u
#define SS_LOG_REFILL_MS 30000u

typedef struct {
    uint32_t lux;                        /* piano-sensors-lux: the front light sensor's last reading, integer lux */
    int32_t target;                      /* piano-sensors-light-target: permille 0..1000, -1 before the first */
    int edge;                            /* piano-sensors-edge: PS_EDGE_* (ps_edge_name), PS_EDGE_NONE while the stream is off */
    uint64_t seq;                        /* piano-sensors-seq: moves with every change of the three */
    unsigned changed;                    /* SS_OUT_*: what moved in this publication */
} SsOutputs;

typedef struct {
    void *context;
    uint64_t (*now_ms)(void *context);
    /* One QMI message on the stream's own port to the service: 0 queued, else refused. */
    int (*send)(void *context, int stream, const uint8_t *data, uint32_t length);
    /* The guard: 1 and a token taken, or 0 (not now). */
    int (*may_request)(void *context);
    /* The guard: the outcome of a request (1 answered; 0 refused, unanswered, an error event). */
    void (*result)(void *context, int ok);
    void (*trace)(void *context, const char *line);
    /* The outputs changed (SsOutputs.changed says which). */
    void (*publish)(void *context, const SsOutputs *out);
    /* The light's consumer is told to leave automatic mode (PianoBacklightAmbient -1): the stream failed for the boot or the guard
     * struck after a target was given. At most once per boot. */
    void (*leave)(void *context);
    /* Close the stream's port (the glue's piano_qrtr_close: DEL_CLIENT); a later ss_port_opened gives it a new one. */
    void (*close_port)(void *context, int stream);
} SsOps;

typedef struct {
    uint32_t accel_hz;                   /* SS_ACCEL_HZ (piano-sensors-accel-hz) */
    uint32_t accel_batch_ms;             /* 0: no batching (as libssc); else the hub may hold samples this long */
    SfxAxes axes;                        /* PS_ACCEL_AXES (piano-sensors-axes) */
    PasConfig stream;                    /* AutoStream's defaults */
} SsConfig;
void ss_defaults(SsConfig *c);

typedef struct {
    int light_consumer;                  /* the light's targets are taken (piano-sensors-als, PianoBacklight's automatic mode) */
    int accel_consumer;                  /* the edge is read (piano-sensors-rotate, piano-rotated) */
    int display;                         /* NativeGopDisplay's piano-display-power: 1 on, 0 off, -1 unknown (counts as on) */
    int lid;                             /* PianoKeyboard's piano-keyboard-hall: 1 open, 0 closed, -1 unknown (counts as open) */
    int attached;                        /* PianoKeyboard's piano-keyboard-attached: 1 attached, anything else detached */
} SsConditions;

typedef struct {
    uint32_t budget_ms;                  /* SS_LOG_REFILL_MS per line, at most SS_LOG_BURST lines' worth */
    uint64_t at;
    uint32_t left_out;                   /* lines not written since the last one */
} SsLog;

typedef struct {
    int open;                            /* the glue has a port for it (0 after ss_force_off / ss_close_all until ss_port_opened) */
    int up;                              /* the port's view of service 400 */
    uint32_t generation;
    int found;                           /* the level-3 list has its default sensor */
    SscSuid suid;
    int on_change;                       /* light: 514 (1) or 513 at rate_bits (0) */
    uint32_t rate_bits;                  /* 513's sample_rate (float bits) */
    uint16_t next_txn, txn;              /* the transaction of the request out */
    int has_client;
    uint64_t client_id;                  /* the hub's id for this client (TLV 0x10 of a response) */
    uint32_t hub_rate_milli;             /* the rate the hub runs the sensor at (physical configuration event 768), mHz; 0 not told */
    uint32_t timeouts_seen;              /* the stream's timeouts already told to the guard */
    uint32_t rearmed_seen;               /* ... and its re-arms already logged */
    uint32_t sample_session;             /* the session whose first sample was logged */
    int last_state;                      /* the stream's state as last logged */
    int ever_sent;                       /* a request went out on this port (its client exists at the hub) */
    const char *why;                     /* why it is not wanted (the off line's reason) */
    SsLog log;
    uint32_t requests, refused_port, responses, refused_hub, timeouts, reports, samples, unreliable, physical, errors;
    uint32_t malformed, unexpected, other, dropped, ignored, restarts, closes;
    char line[200];
} SsChannel;

typedef struct {
    const SsOps *ops;
    SsConfig cfg;
    PalLight light;                      /* the light's stream, policy and smoothing (AutoLight.h) */
    PasStream accel;                     /* the accelerometer's stream (AutoStream.h) */
    SnoState orient;
    SsChannel ch[SS_STREAMS];
    SsConditions cond;
    int asleep;                          /* ss_sleep: a system sleep is under way */
    int off;                             /* ss_off: the guard struck (for the rest of the boot) */
    int left;                            /* ops->leave was called */
    int target_given;                    /* a target was published (leaving automatic mode then means something) */
    SsOutputs out;
    int lux_pending;
    uint64_t lux_ms;
} SsEngine;

void ss_init(SsEngine *e, const SsOps *ops, const SsConfig *config);
/* The glue opened (or reopened) the stream's port. The stream starts afresh once the port sees the service. */
void ss_port_opened(SsEngine *e, int stream);
/* The port's view of service 400 (piano_qrtr_server): a new generation starts the stream over (the hub forgot it). */
void ss_service(SsEngine *e, int stream, int up, uint32_t generation);
/* The level-3 list's default sensor for the stream: found 0 or its SUID, its attributes' stream type (SSC_STREAM_*, -1 not read) and
 * its lowest advertised rate (float bits, 0 none). A different SUID starts the stream over. */
void ss_sensor(SsEngine *e, int stream, int found, const SscSuid *suid, int64_t stream_type, uint32_t lowest_rate_bits);
/* ss_sensor for both streams from the level-3 list: a sensor counts once its lookup is answered, it is found and its attributes are
 * read or given up (the request depends on them), and not when its attributes say it is unavailable. */
void ss_from_list(SsEngine *e, const ScClient *list);
/* The consumers' and the hardware's state (repeating it is harmless). */
void ss_conditions(SsEngine *e, const SsConditions *c);
/* A system sleep begins (1: every stream is disabled) or ended (0). */
void ss_sleep(SsEngine *e, int asleep);
/* One QMI message the stream's port received. */
void ss_receive(SsEngine *e, int stream, const uint8_t *data, uint32_t length);
/* Time and the next requests: call it after every input and at least every 250 ms while ss_deadline asks. */
void ss_tick(SsEngine *e);
/* When ss_tick has something to do next (absolute ms), 0 when only an input can change anything. */
uint64_t ss_deadline(const SsEngine *e);
/* 1 when no stream is on, being switched or in an unknown state at the hub (a sleep may proceed). */
int ss_idle(const SsEngine *e);
/* The sleep's wait is over: each stream not idle has its port closed (the hub drops its requests) and starts afresh on a new one. */
void ss_force_off(SsEngine *e);
/* The guard struck: nothing more is sent, the outputs go to rest, every later packet is ignored (for the rest of the boot). */
void ss_off(SsEngine *e);
/* After ss_off, when the DSP may still be told: the ports that carried a request are closed (the hub forgets their streams). */
void ss_close_all(SsEngine *e);
/* One line about the stream for the status property. */
void ss_status(const SsEngine *e, int stream, char *out, uint32_t size);
const char *ss_stream_name(int stream);

#ifdef __cplusplus
}
#endif
#endif
