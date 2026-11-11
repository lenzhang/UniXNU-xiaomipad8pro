/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The table of the QRTR client ports other kexts open on PianoPower's router (PianoAdspContract.h): which port follows which remote
 * service, what it last reported, and the server changes to report after a router event. Portable C, no locks, no allocation, no I/O:
 * PianoQrtr.c holds its router lock around every call and runs the callbacks after dropping it. Host-tested (tests/ports_host.cpp). */
#ifndef QRTR_PORTS_H
#define QRTR_PORTS_H
#include <stdint.h>
#include "QcomQrtr.h"
#include "PianoAdspContract.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t used, closing;             /* closing: no new callback starts; the slot frees once the one in progress returns */
    uint32_t number;                   /* the local port: PIANO_QRTR_FIRST_PORT + slot + PIANO_QRTR_MAX_PORTS * (uses % 64) */
    uint32_t uses;                     /* opens of this slot so far (a reopened slot gets another number) */
    uint32_t service, instance;        /* the followed service (instance as announced, or PIANO_QRTR_ANY) */
    PianoQrtrClient client;
    void *context;
    uint32_t busy;                     /* callbacks in progress (the glue's count) */
    PianoQrtrServer server;            /* what was last reported */
} QrtrPort;

typedef struct {
    QrtrPort port[PIANO_QRTR_MAX_PORTS];
    uint32_t opened, closed, delivered, refused;
} QrtrPorts;

/* One server change to report: up 0 (gone: the old place) or 1 (appeared: the new one). */
typedef struct {
    uint8_t slot, up;
    uint32_t number, node, port, instance, generation;
} QrtrPortEvent;

void qrtr_ports_init(QrtrPorts *t);
/* Fills a free slot: its index, -1 none free, -2 bad arguments (no client, or neither callback). */
int qrtr_ports_open(QrtrPorts *t, uint32_t service, uint32_t instance, const PianoQrtrClient *client, void *context);
/* The slot of an open port number, -1 when none. */
int qrtr_ports_find(const QrtrPorts *t, uint32_t number);
/* Frees the slot (its number is not reused by the next open). */
void qrtr_ports_free(QrtrPorts *t, int slot);
/* True when an announced (service, instance) is the port's service. */
int qrtr_ports_matches(const QrtrPort *p, uint32_t service, uint32_t instance);
/* Compares every open port (not closing) with the services the router knows (q null: no link, every server gone) and records the
 * changes: a server that is gone or moved first (up 0), then the one that appeared (up 1); a port keeps its server while announced.
 * Returns the number of events written (at most 2 per port; capacity 2 * PIANO_QRTR_MAX_PORTS is always enough). */
unsigned qrtr_ports_match(QrtrPorts *t, const QcomQrtr *q, QrtrPortEvent *events, unsigned capacity);

#ifdef __cplusplus
}
#endif
#endif
