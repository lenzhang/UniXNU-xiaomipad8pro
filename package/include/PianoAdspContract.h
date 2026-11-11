/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The piano audio DSP (ADSP) client contract: what PianoPower offers the other native kexts that talk to the running DSP (PianoAudio's
 * adsp_apps, PianoSensors' fastrpcglink-apps-dsp and its sensor QMI client), and the rules they keep. Include it as
 * <PianoAdspContract.h> (tooling/k27/build-native-kext.py puts package/include on every kext's include path); name
 * "com.unixnu.driver.PianoPower" among the libraries of the client's descriptor and list the client after PianoPower in
 * configs/k27-native-kexts.txt, so the KC links these functions by name to PianoPower's single copy. docs/drivers/charging.md §9.
 *
 * The rules (the DSP also runs the charger firmware, and natively nothing restarts it: a DSP crash ends charging until the reboot):
 *   1. Only PianoPower starts the DSP (piano-power>=2). A client never starts, stops or restarts it, never votes its rails and never
 *      touches RPMh, AOSS or the PAS calls; nothing here offers that.
 *   2. Callbacks run on PianoPower's DSP thread with no lock held. They must not block, sleep or wait for another thread (the charger
 *      waits with them; PianoPower logs a callback that takes more than 20 ms); copy what is needed and wake the client's own thread.
 *      The data pointers are valid during the callback only.
 *   3. Sends are queued and go out from the DSP thread; a caller never waits. A full queue is refused (-1), never waited out. Each
 *      channel has its own buffers; the QRTR ports share the router's channel, of which PianoPower keeps a share for its own traffic.
 *   4. Hold traffic while piano_adsp_quiet() is nonzero (PianoPower configures the charger), except answers the DSP is waiting for.
 *      Stop for the rest of the boot on any sign of trouble in piano_adsp_health() (a fatal report, a crash reason, a new link or
 *      charger generation): docs/drivers/sensors.md §7 is the model (SensorsGuard.c).
 * Thread context only (not from an interrupt or with a spinlock held); every function returns at once. */
#ifndef PIANO_ADSP_CONTRACT_H
#define PIANO_ADSP_CONTRACT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- GLINK channels (UniXNU-sheng's ShengAdsp client interface, renamed) ----
 * A client registers a channel by name at any time (before or after the DSP is up) and gets its state and messages through callbacks.
 * Six channels in all: PianoPower's IPCRTR and PMIC_RTR_ADSP_APPS, then four for clients (audio's adsp_apps, the sensors'
 * fastrpcglink-apps-dsp, two spare). A name already registered is refused. */
#define PIANO_ADSP_MAX_MESSAGE 1056u   /* one message, either way: 1024 bytes of QRTR data and its 32-byte header */
#define PIANO_ADSP_CHANNELS 6u

typedef struct PianoAdspChannel PianoAdspChannel;
typedef struct {
    void (*state)(void *context, int up);
    void (*receive)(void *context, const uint8_t *data, uint32_t length);
} PianoAdspClient;

/* Registers the channel; intent_size/intent_count are the receive buffers offered to the DSP (at most 24). initiator: we open it
 * (otherwise we answer the DSP's open). Returns null when the name is taken, the slots are full or memory is short. */
PianoAdspChannel *piano_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                     const PianoAdspClient *client, void *context);
/* Queues one message (at most PIANO_ADSP_MAX_MESSAGE bytes): 0 queued, -1 queue full or too big, -2 bad arguments. */
int piano_adsp_send(PianoAdspChannel *channel, const void *data, uint32_t length);

/* ---- QRTR client ports on PianoPower's IPC router (the IPCRTR channel) ----
 * A port follows one remote service: server() reports it appearing (up 1) and going (up 0) as the DSP's name service announces it
 * (instance as announced: version | instance << 8, or PIANO_QRTR_ANY), and receive() gets every data packet sent to the port, from any
 * node and port. Up to PIANO_QRTR_MAX_PORTS ports; local port numbers from PIANO_QRTR_FIRST_PORT. A port may be opened before the DSP
 * runs (or with PianoPower below level 2, when no server ever appears). */
#define PIANO_QRTR_ANY 0xffffffffu
#define PIANO_QRTR_MAX_PORTS 8u
#define PIANO_QRTR_FIRST_PORT 16u
#define PIANO_QRTR_MAX_DATA 1024u      /* one data packet's payload (sends; the router receives up to 2016) */

typedef struct {
    void (*server)(void *context, int up, uint32_t node, uint32_t port, uint32_t instance);
    void (*receive)(void *context, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length);
} PianoQrtrClient;
typedef struct {
    int up;                            /* the followed service is announced now */
    uint32_t node, port, instance;     /* where (valid while up) */
    uint32_t generation;               /* moves on every change (appeared, gone, moved) */
} PianoQrtrServer;

/* Opens a port: its number (>= PIANO_QRTR_FIRST_PORT), or -1 all taken, -2 bad arguments. When the service is announced already, the
 * first server() call comes from the DSP thread soon after. */
int piano_qrtr_open(uint32_t service, uint32_t instance, const PianoQrtrClient *client, void *context);
/* The followed service as last seen: 0 and *out filled, -1 not an open port. */
int piano_qrtr_server(int port, PianoQrtrServer *out);
/* Queues one data packet from port to node:to_port: 0 queued, -1 queue full (or the router's share for PianoPower reached), -2 bad
 * arguments or too long, -3 no router link yet (the DSP is not up). */
int piano_qrtr_send(int port, uint32_t node, uint32_t to_port, const void *data, uint32_t length);
/* Closes the port (the DSP is told: DEL_CLIENT). No callback for it runs once this returns (called from the port's own callback, the
 * callback in progress is the last). */
void piano_qrtr_close(int port);

/* ---- health and quiet (read only) ---- */
#define PIANO_ADSP_HEALTH_VERSION 1u
typedef struct {
    uint32_t version;                  /* PIANO_ADSP_HEALTH_VERSION */
    int32_t stage;                     /* PianoPower's bring-up stage (18 DSP ready, 20 a channel up); negative: the stage that failed */
    uint32_t dsp_up;                   /* ready on SMP2P, no fatal bit, GLINK up and the router's channel up */
    uint32_t fatal;                    /* the SMP2P fatal bit now (slave-kernel bit 0) */
    uint32_t fatal_reports;            /* fatal reports since the start (it never goes back down) */
    uint32_t link_generation;          /* moves each time a channel that was up goes down, or the GLINK transport breaks */
    uint32_t charger_pd;               /* msm/adsp/charger_pd's servreg state: 0x1fffffff up, 0 unknown */
    uint32_t charger_generation;       /* charger_pd's transitions to up */
    uint32_t quiet;                    /* piano_adsp_quiet() */
    char crash[80];                    /* the SMEM crash reason (item 423) read at the last fatal report; empty when none */
} PianoAdspHealth;
void piano_adsp_health(PianoAdspHealth *out);

/* Nonzero while PianoPower asks clients to hold their traffic: PIANO_ADSP_QUIET_CHARGER while it configures a charger generation (the
 * board configuration, the port enable and the first policy writes; at most 60 s per generation). */
#define PIANO_ADSP_QUIET_CHARGER 1u
uint32_t piano_adsp_quiet(void);

/* The charger domain's state (as in the health): 0 unknown, 0x1fffffff up, ...; *ups (optional) its transitions to up. */
uint32_t piano_qrtr_charger_pd(uint32_t *ups);

#ifdef __cplusplus
}
#endif
#endif
