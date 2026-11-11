/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The DSP's UCSI owner for one client kext (PianoUsbHost's UCSI client, docs/drivers/usb-host.md H4): an extension of PianoAdspContract.h.
 * The DSP's charger firmware serves three owners on PianoPower's GLINK channel PMIC_RTR_ADSP_APPS (not on the IPC router, so a QRTR port
 * cannot reach them): the charger 0x800a (PianoPower's own), the USB Type-C connector interface (UCSI) 0x800b, and the USB-C port
 * notifications 0x800c (PianoPower enables and acknowledges them; a client watches them through PianoAdspContract.h's port events,
 * piano_port_events). This relay gives one client the UCSI owner to drive; the charger owner and the port owner stay PianoPower's. Include it as
 * <PianoUsbcContract.h>, name "com.unixnu.driver.PianoPower" among the client descriptor's libraries and list the client after
 * PianoPower in configs/k27-native-kexts.txt (as for PianoAdspContract.h). Implemented by package/PianoPower/PianoUsbcRelay.cpp.
 *
 * The framing (Linux drivers/usb/typec/ucsi/ucsi_glink.c read as the specification): every message starts with {owner, type 1, opcode}
 * (three little-endian words). UCSI: a read request is that header alone (opcode 0x11), answered with the header, the PPM's buffer
 * (48 bytes for UCSI 1.x: VERSION 0, CCI 4, CONTROL 8, MESSAGE_IN 16, MESSAGE_OUT 32; 528 for 2.x) and a return code; a write request
 * (opcode 0x12) is the header, a whole buffer (the command in CONTROL) and a reserved word, answered with the header and a return code;
 * a notification (opcode 0x13: read the CCI now) is the header and three words.
 *
 * The rules (PianoAdspContract.h's, and):
 *   1. One client. It never starts, stops or restarts the DSP and sends only UCSI requests: piano_usbc_send refuses anything that is
 *      not a well-formed owner-0x800b read or write request (never the charger's owner, never the port owner's enable or acknowledge).
 *   2. Ready: the charger generation is linked (msm/adsp/charger_pd up, or PianoPower's 60 s fallback), PianoPower's quiet flag is down
 *      (the board configuration and the port enable are through) and PianoPower's own one UCSI buffer read of that generation is
 *      answered or given up. state() reports every change: ready 0, then ready 1 with a new generation after a DSP or charger_pd
 *      restart (the PPM is back on its defaults: the client starts over, PPM_RESET first). Sends while not ready are refused (-3);
 *      requests queued when the generation ends are dropped.
 *   3. Callbacks run on PianoPower's thread with no lock held. They must not block, sleep or wait for another thread; copy what is
 *      needed and wake the client's own thread. The data pointers are valid during the callback only. ucsi() gets every owner-0x800b
 *      message while ready (read and write answers, notifications); PianoMca does not see them meanwhile.
 *   4. Sends are queued (PIANO_USBC_QUEUE) and go out from PianoPower's thread in order; a caller never waits. One request in flight
 *      at a time is the client's own discipline (the PPM answers in order; UCSI is a mailbox).
 * Thread context only; every function returns at once (piano_usbc_detach waits for a callback in progress to return, unless it is
 * called from one). */
#ifndef PIANO_USBC_CONTRACT_H
#define PIANO_USBC_CONTRACT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PIANO_USBC_OWNER_UCSI 0x800bu
#define PIANO_USBC_UCSI_READ 0x11u
#define PIANO_USBC_UCSI_WRITE 0x12u
#define PIANO_USBC_UCSI_NOTIFY 0x13u
#define PIANO_USBC_BUFFER_V1 48u
#define PIANO_USBC_BUFFER_V2 528u
#define PIANO_USBC_MAX_MESSAGE (12u + PIANO_USBC_BUFFER_V2 + 4u)   /* a UCSI 2.x write request: 544 bytes */
#define PIANO_USBC_QUEUE 4u

typedef struct {
    void (*state)(void *context, int ready, uint32_t generation);
    void (*ucsi)(void *context, const uint8_t *data, uint32_t length);
} PianoUsbcClient;

/* Becomes the client: 0, -1 another client is attached, -2 bad arguments (no client or no ucsi callback). When the owner is ready
 * already, state(1, generation) comes from PianoPower's thread soon after. */
int piano_usbc_attach(const PianoUsbcClient *client, void *context);
/* Stops the client's callbacks and drops its queued requests. */
void piano_usbc_detach(void);
/* Queues one UCSI request: 0 queued, -1 queue full, -2 not a UCSI read (12 bytes) or write (12 + 48 + 4 or 12 + 528 + 4 bytes)
 * request of owner 0x800b, -3 not ready, -4 not attached. */
int piano_usbc_send(const void *data, uint32_t length);

#define PIANO_USBC_STATUS_VERSION 1u
typedef struct {
    uint32_t version;                  /* PIANO_USBC_STATUS_VERSION */
    uint32_t attached, ready, generation;
    uint32_t queued;                   /* requests waiting for PianoPower's thread */
    uint32_t sent, send_failures;      /* requests handed to the channel; refused by it (retried at the next pass) */
    uint32_t delivered;                /* UCSI messages given to the client */
    uint32_t refused, dropped;         /* sends refused (bad, not ready, full); queued requests dropped at a generation's end */
} PianoUsbcStatus;
void piano_usbc_status(PianoUsbcStatus *out);

#ifdef __cplusplus
}
#endif
#endif
