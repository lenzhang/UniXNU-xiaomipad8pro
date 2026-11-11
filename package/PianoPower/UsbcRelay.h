/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The USB-C relay (package/include/PianoUsbcContract.h): one client kext drives the DSP's UCSI owner 0x800b over PianoPower's
 * PMIC_RTR_ADSP_APPS channel (the port notifications 0x800c reach clients through PortEvents.c, not here). Two parts:
 *   - the core below (portable C, no locks, no allocation, no I/O; UsbcRelay.c, host-tested by tests/usbc_host.cpp): the request
 *     check, the queue, the ready transitions, which received message is whose;
 *   - the kernel glue (PianoUsbcRelay.cpp): the contract's piano_usbc_* functions under one spin lock, and the three hooks
 *     PianoPower.cpp calls from its own thread (piano_usbc_relay_*), which run the callbacks with no lock held.
 * PianoPower's own traffic is unchanged: the charger owner, the port enable and acknowledgements and its one UCSI buffer read per
 * generation stay in PianoMca.c; a UCSI message goes to the client instead of PianoMca only while the client is ready. */
#ifndef USBC_RELAY_H
#define USBC_RELAY_H
#include <stdint.h>
#include "PianoUsbcContract.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t attached, closing, ready;
    uint32_t generation;               /* the generation the client was last told (valid while ready) */
    uint8_t told;                      /* the client has been told the current state (a state() call is due when 0) */
    PianoUsbcClient client;
    void *context;
    uint32_t busy;                     /* callbacks in progress (the glue's count) */
    struct {uint32_t length; uint8_t data[PIANO_USBC_MAX_MESSAGE];} queue[PIANO_USBC_QUEUE];
    uint8_t head, count;
    uint8_t kick;                      /* something was queued since the owner's last pump */
    uint32_t sent, send_failures, delivered, refused, dropped;
} UsbcRelay;

void usbc_relay_init(UsbcRelay *r);
/* 0, -1 taken, -2 bad arguments. */
int usbc_relay_attach(UsbcRelay *r, const PianoUsbcClient *client, void *context);
void usbc_relay_detach(UsbcRelay *r);
/* True when data is a well-formed UCSI read or write request of owner 0x800b (the only requests a client may send). */
int usbc_relay_request_ok(const uint8_t *data, uint32_t length);
/* piano_usbc_send's decision: 0 queued, -1 full, -2 bad, -3 not ready, -4 not attached. */
int usbc_relay_queue(UsbcRelay *r, const uint8_t *data, uint32_t length);
/* The owner's view (ready, generation). Returns 1 when the client must be told (state() due with r->ready, r->generation); a change
 * of generation while ready is told as one call with the new generation. Leaving ready drops the queue. */
int usbc_relay_link(UsbcRelay *r, int ready, uint32_t generation);
/* The state() call was made: the client's requests and the UCSI messages flow from now on. */
void usbc_relay_told(UsbcRelay *r);
/* Whose a received message is: USBC_MINE_NONE (PianoMca's: everything but UCSI traffic while the client is ready), USBC_MINE_UCSI
 * (the client's alone: PianoMca does not see it). */
enum {USBC_MINE_NONE = 0, USBC_MINE_UCSI = 1};
int usbc_relay_classify(const UsbcRelay *r, const uint8_t *data, uint32_t length);
/* The oldest queued request (its length; 0 none), left queued until usbc_relay_pop. */
uint32_t usbc_relay_peek(const UsbcRelay *r, const uint8_t **data);
void usbc_relay_pop(UsbcRelay *r);
void usbc_relay_status(const UsbcRelay *r, PianoUsbcStatus *out);

/* ---- the hooks PianoPower.cpp calls from its thread (PianoUsbcRelay.cpp) ----
 * step: after each pass of its loop: ready as rule 2 of the contract says (computed by PianoPower), the generation (the charger
 * codec's link count), its wait event (a client's send wakes the thread with it); the state callback if due, then the queue through
 * the channel. receive: every message of the channel before PianoMca sees it; nonzero: the client's, not PianoMca's. pending: a
 * send arrived since the last step (the loop must not block). attached: a client is attached (for PianoPower's log). */
struct PianoAdspChannel;
void piano_usbc_relay_step(struct PianoAdspChannel *channel, int ready, uint32_t generation, const void *wake);
int piano_usbc_relay_receive(const uint8_t *data, uint32_t length);
int piano_usbc_relay_pending(void);
int piano_usbc_relay_attached(void);

#ifdef __cplusplus
}
#endif
#endif
