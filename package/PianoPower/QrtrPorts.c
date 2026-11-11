/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QrtrPorts.h"

static void zero(void *p, uint32_t n) {for (uint32_t i = 0; i < n; ++i) ((uint8_t *)p)[i] = 0;}

void qrtr_ports_init(QrtrPorts *t) {zero(t, sizeof *t);}

int qrtr_ports_open(QrtrPorts *t, uint32_t service, uint32_t instance, const PianoQrtrClient *client, void *context)
{
    if (!client || (!client->server && !client->receive)) return -2;
    for (unsigned i = 0; i < PIANO_QRTR_MAX_PORTS; ++i) {
        QrtrPort *p = &t->port[i];
        if (p->used) continue;
        const uint32_t uses = p->uses + 1;
        zero(p, sizeof *p);
        p->used = 1;
        p->uses = uses;
        p->number = PIANO_QRTR_FIRST_PORT + i + PIANO_QRTR_MAX_PORTS * (uses % 64);
        p->service = service;
        p->instance = instance;
        p->client = *client;
        p->context = context;
        t->opened++;
        return (int)i;
    }
    return -1;
}

int qrtr_ports_find(const QrtrPorts *t, uint32_t number)
{
    for (unsigned i = 0; i < PIANO_QRTR_MAX_PORTS; ++i)
        if (t->port[i].used && t->port[i].number == number) return (int)i;
    return -1;
}

void qrtr_ports_free(QrtrPorts *t, int slot)
{
    if (slot < 0 || slot >= (int)PIANO_QRTR_MAX_PORTS || !t->port[slot].used) return;
    QrtrPort *p = &t->port[slot];
    const uint32_t uses = p->uses;
    zero(p, sizeof *p);
    p->uses = uses;
    t->closed++;
}

int qrtr_ports_matches(const QrtrPort *p, uint32_t service, uint32_t instance)
{
    return service == p->service && (p->instance == PIANO_QRTR_ANY || instance == p->instance);
}

unsigned qrtr_ports_match(QrtrPorts *t, const QcomQrtr *q, QrtrPortEvent *events, unsigned capacity)
{
    unsigned n = 0;
    for (unsigned i = 0; i < PIANO_QRTR_MAX_PORTS; ++i) {
        QrtrPort *p = &t->port[i];
        if (!p->used || p->closing) continue;
        int keep = 0, found = -1;
        for (uint32_t r = 0; q && r < q->remote_count; ++r) {
            if (!qrtr_ports_matches(p, q->remote[r].service, q->remote[r].instance)) continue;
            if (p->server.up && q->remote[r].node == p->server.node && q->remote[r].port == p->server.port &&
                q->remote[r].instance == p->server.instance) {keep = 1; break;}
            if (found < 0) found = (int)r;
        }
        if (keep) continue;
        if (p->server.up) {
            p->server.up = 0;
            p->server.generation++;
            if (n < capacity) {
                QrtrPortEvent *e = &events[n++];
                e->slot = (uint8_t)i; e->up = 0; e->number = p->number;
                e->node = p->server.node; e->port = p->server.port; e->instance = p->server.instance; e->generation = p->server.generation;
            }
        }
        if (found >= 0) {
            p->server.up = 1;
            p->server.node = q->remote[found].node;
            p->server.port = q->remote[found].port;
            p->server.instance = q->remote[found].instance;
            p->server.generation++;
            if (n < capacity) {
                QrtrPortEvent *e = &events[n++];
                e->slot = (uint8_t)i; e->up = 1; e->number = p->number;
                e->node = p->server.node; e->port = p->server.port; e->instance = p->server.instance; e->generation = p->server.generation;
            }
        }
    }
    return n;
}
