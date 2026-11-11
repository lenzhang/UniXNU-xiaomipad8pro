/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbcRelay.h"

static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void zero(void *p, uint32_t n) {for (uint32_t i = 0; i < n; ++i) ((uint8_t *)p)[i] = 0;}

void usbc_relay_init(UsbcRelay *r)
{
    zero(r, sizeof *r);
    r->told = 1;   /* nothing to tell before a client is attached */
}

int usbc_relay_attach(UsbcRelay *r, const PianoUsbcClient *client, void *context)
{
    if (!client || !client->ucsi) return -2;
    if (r->attached) return -1;
    r->client = *client;
    r->context = context;
    r->attached = 1;
    r->closing = 0;
    r->head = r->count = 0;
    r->told = !r->ready;   /* ready already: state(1) is due */
    return 0;
}

void usbc_relay_detach(UsbcRelay *r)
{
    r->dropped += r->count;
    r->head = r->count = 0;
    r->attached = r->closing = 0;
    r->told = 1;
    zero(&r->client, sizeof r->client);
    r->context = 0;
}

int usbc_relay_request_ok(const uint8_t *data, uint32_t length)
{
    if (!data || length < 12 || le32(data) != PIANO_USBC_OWNER_UCSI || le32(data + 4) != 1) return 0;
    const uint32_t opcode = le32(data + 8);
    if (opcode == PIANO_USBC_UCSI_READ) return length == 12;
    if (opcode == PIANO_USBC_UCSI_WRITE)
        return length == 12 + PIANO_USBC_BUFFER_V1 + 4 || length == 12 + PIANO_USBC_BUFFER_V2 + 4;
    return 0;
}

int usbc_relay_queue(UsbcRelay *r, const uint8_t *data, uint32_t length)
{
    int result = 0;
    if (!r->attached || r->closing) result = -4;
    else if (!usbc_relay_request_ok(data, length)) result = -2;
    else if (!r->ready || !r->told) result = -3;   /* not ready, or the client has not been told the new generation yet */
    else if (r->count >= PIANO_USBC_QUEUE) result = -1;
    if (result) {
        r->refused++;
        return result;
    }
    const unsigned slot = (r->head + r->count) % PIANO_USBC_QUEUE;
    for (uint32_t i = 0; i < length; ++i) r->queue[slot].data[i] = data[i];
    r->queue[slot].length = length;
    r->count++;
    r->kick = 1;
    return 0;
}

int usbc_relay_link(UsbcRelay *r, int ready, uint32_t generation)
{
    ready = ready != 0;
    const int changed = ready != r->ready || (ready && generation != r->generation);
    if (changed) {
        /* A request queued for the old generation belongs to a PPM that is gone (or back on its defaults). */
        r->dropped += r->count;
        r->head = r->count = 0;
        r->ready = (uint8_t)ready;
        r->generation = ready ? generation : r->generation;
        if (r->attached) r->told = 0;
    }
    return r->attached && !r->closing && !r->told;
}

void usbc_relay_told(UsbcRelay *r) {r->told = 1;}

int usbc_relay_classify(const UsbcRelay *r, const uint8_t *data, uint32_t length)
{
    if (!data || length < 12 || !r->attached || r->closing) return USBC_MINE_NONE;
    return le32(data) == PIANO_USBC_OWNER_UCSI && r->ready && r->told ? USBC_MINE_UCSI : USBC_MINE_NONE;
}

uint32_t usbc_relay_peek(const UsbcRelay *r, const uint8_t **data)
{
    if (!r->count || !r->attached || r->closing || !r->ready) return 0;
    *data = r->queue[r->head].data;
    return r->queue[r->head].length;
}

void usbc_relay_pop(UsbcRelay *r)
{
    if (!r->count) return;
    r->head = (uint8_t)((r->head + 1) % PIANO_USBC_QUEUE);
    r->count--;
    r->sent++;
}

void usbc_relay_status(const UsbcRelay *r, PianoUsbcStatus *out)
{
    zero(out, sizeof *out);
    out->version = PIANO_USBC_STATUS_VERSION;
    out->attached = r->attached && !r->closing;
    out->ready = r->ready;
    out->generation = r->generation;
    out->queued = r->count;
    out->sent = r->sent;
    out->send_failures = r->send_failures;
    out->delivered = r->delivered;
    out->refused = r->refused;
    out->dropped = r->dropped;
}
