/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* Level 3's client of the hub's sensor client service (QMI service 400, SensorsSsc.h) on one QRTR port of PianoPower's router: the
 * SUID lookup of each data type piano has (default sensor only, with updates: the hub reports late sensors), then the attributes of
 * every sensor found (name, vendor, rates, ...). No stream: the list is what level 3 publishes. Portable C, single threaded (the
 * driver's own thread), no allocation, integer only; host-tested against a simulated hub (tests/piano.c).
 *
 * One connection serves all of it (a client may ask about any SUID; the events name the sensor they are about): one request at a time,
 * the next after its response (a response only says "received"; the data come in reports). The guard (SensorsGuard.h) is asked for
 * each request and told each outcome: a refused request, a missing response (2 s) or an error event counts as a failure, so the
 * guard's backoff paces the retries and its strikes end them. A SUID or attribute event that does not come within 5 s is asked again,
 * three times at most (a lookup given up counts as none found). A service that goes and comes back (a new generation) starts everything over. */
#ifndef SENSORS_CLIENT_H
#define SENSORS_CLIENT_H
#include <stdint.h>
#include "SensorsSsc.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {SC_REGISTRY = 0, SC_ACCEL = 1, SC_GYRO = 2, SC_MAG = 3, SC_LIGHT = 4, SC_PROX = 5, SC_TYPES = 6};
enum {SC_LOOKUP_TODO = 0, SC_LOOKUP_SENT = 1, SC_LOOKUP_ANSWERED = 2};
enum {SC_ATTR_NONE = 0, SC_ATTR_TODO = 1, SC_ATTR_SENT = 2, SC_ATTR_READ = 3, SC_ATTR_GAVE_UP = 4};
#define SC_RESPONSE_MS 2000u
#define SC_ATTR_MS 5000u
#define SC_ATTR_TRIES 3u
extern const char *const sc_type_name[SC_TYPES];   /* the data types: registry, accel, gyro, mag, ambient_light, proximity */

typedef struct {
    void *context;
    uint64_t (*now_ms)(void *context);
    /* One QMI message to the service (the glue knows its node and port): 0 queued, else refused. */
    int (*send)(void *context, const uint8_t *data, uint32_t length);
    /* The guard: 1 and a token taken, or 0 (not now). */
    int (*may_request)(void *context);
    /* The guard: the outcome of a request (1 answered; 0 refused, unanswered or an error event). */
    void (*result)(void *context, int ok);
    void (*trace)(void *context, const char *line);
    /* The sensor list changed (a lookup answered, attributes read or given up): publish it again. */
    void (*changed)(void *context);
} ScOps;

typedef struct {
    int lookup;                          /* SC_LOOKUP_* */
    unsigned lookup_tries;
    uint64_t lookup_deadline;            /* the SUID event is due by then (SC_ATTR_MS after the request) */
    int lookup_gave_up;                  /* no SUID event after SC_ATTR_TRIES requests: counted as none found */
    unsigned found, more;                /* SUIDs the hub listed (more: beyond SSC_MAX_SUIDS) */
    SscSuid suid;                        /* the first, the default sensor */
    int attr;                            /* SC_ATTR_* */
    unsigned attr_tries;
    uint64_t attr_deadline;
    SscAttrs attrs;
} ScSensor;

typedef struct {
    const ScOps *ops;
    int up;
    uint32_t generation;
    uint16_t next_txn, pending_txn;
    int pending;                         /* a request is outstanding */
    int pending_type;                    /* its data type, -1 for none */
    int pending_lookup;                  /* 1 lookup, 0 attributes */
    uint64_t sent_ms;
    int has_client;
    uint64_t client_id;                  /* the hub's id for this connection (TLV 0x10 of a response) */
    int discovery_done;
    ScSensor sensor[SC_TYPES];
    uint32_t requests, responses, refused, timeouts, reports, events, errors, malformed, unexpected;
    char line[200];
} ScClient;

void sc_init(ScClient *c, const ScOps *ops);
/* Service 400 appeared (up 1) or went (0); a new generation while up starts everything over. */
void sc_service(ScClient *c, int up, uint32_t generation);
/* One QMI message the port received. */
void sc_receive(ScClient *c, const uint8_t *data, uint32_t length);
/* Timeouts and the next request; call it often (every 250 ms) and after each sc_receive. */
void sc_tick(ScClient *c);
/* 1 when every lookup is answered and every found sensor's attributes are read or given up. */
int sc_settled(const ScClient *c);

#ifdef __cplusplus
}
#endif
#endif
