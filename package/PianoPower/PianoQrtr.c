/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The IPC router link to the audio DSP (GLINK channel "IPCRTR", which the DSP opens) and the service locator on it: UniXNU-sheng's
 * ShengQrtr.c (UPSTREAM) on PianoAdsp.h. The DSP's protection domains (audio, charger, sensors) ask the application processor's locator
 * which domains provide a service before they start, and the charger properties are served from the charger domain: without these
 * answers they never come up. The domain table is the one Linux's pd-mapper uses for SM8750 (its SM8550 table), the same as sheng's.
 * Everything runs in the DSP thread's callbacks: each packet is handled at once and any answer is queued with piano_adsp_send.
 * Other kexts open client ports on this router (PianoAdspContract.h): gLock covers the router and the port table; the DSP thread takes
 * it around every router event, the clients around open, send, server and close. A client callback never runs under it: the events of
 * one router step are collected under the lock and delivered after it is dropped, each only while its port is still open, the port
 * marked busy so that a close from another thread waits for the callback in progress. */
#include <stddef.h>
#include <stdint.h>
#include "QcomQrtr.h"
#include "PianoAdsp.h"
#include "QrtrPorts.h"
#include "Servreg.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>

enum {kLocalNode = 1, kLocatorPort = 1, kServregPort = 2};

static const char *const kAudio[] = {"avs/audio", NULL};
static const char *const kGps[] = {"gps/gps_service", NULL};
static const QcomPdDomain kDomains[] = {
    {"msm/adsp/audio_pd", 74, kAudio},   {"msm/adsp/root_pd", 74, NULL}, {"msm/adsp/charger_pd", 74, NULL},
    {"msm/adsp/sensor_pd", 74, NULL},    {"msm/cdsp/root_pd", 76, NULL}, {"msm/modem/root_pd", 180, kGps},
};

static QcomQrtr gQrtr;
static PianoAdspChannel *gChannel;
static uint32_t gStage, gRequests, gAnswers;

/* The charger domain's state from the DSP's service registry notifier (Servreg.h): registered once the notifier (0x42, instance 74)
 * is announced, then every indication is acknowledged. gPdState: 0 unknown, else a SERVREG_STATE_* value; gPdUps counts the times it
 * went up (a new generation each: the charger is back on its defaults). Read by PianoPower from another thread. */
static volatile uint32_t gPdState, gPdUps;
static uint32_t gNotifierNode, gNotifierPort;
static uint8_t gRegistered;
static uint16_t gTxn = 1;

static void pdState(uint32_t state, const char *how)
{
    const uint32_t was = gPdState;
    __atomic_store_n(&gPdState, state, __ATOMIC_RELEASE);
    if (state == SERVREG_STATE_UP && was != SERVREG_STATE_UP) __atomic_fetch_add(&gPdUps, 1, __ATOMIC_RELEASE);
    IOLog("PianoPower: servreg: %s is %s (%#x, %s)\n", SERVREG_CHARGER_PD, servreg_state_name(state), state, how);
}

/* The ADSP's notifier among the announced services (another domain's notifier may be announced first): its index, or -1. */
static int notifier(void)
{
    for (uint32_t i = 0; i < gQrtr.remote_count; ++i)
        if (gQrtr.remote[i].service == SERVREG_NOTIFIER_SERVICE && (gQrtr.remote[i].instance >> 8) == SERVREG_ADSP_INSTANCE) return (int)i;
    return -1;
}

static void servregRegister(void)
{
    const int i = notifier();
    if (gRegistered || i < 0) return;
    uint8_t message[96];
    const uint32_t n = servreg_register_listener(message, sizeof message, gTxn++, SERVREG_CHARGER_PD);
    gNotifierNode = gQrtr.remote[i].node;
    gNotifierPort = gQrtr.remote[i].port;
    const int rc = n ? qcom_qrtr_send_data(&gQrtr, kServregPort, gNotifierNode, gNotifierPort, message, n) : -1;
    gRegistered = rc == 0;
    IOLog("PianoPower: servreg: the notifier is at %u:%u (instance %#x); registering a listener for %s: %s\n", gNotifierNode, gNotifierPort,
          gQrtr.remote[i].instance, SERVREG_CHARGER_PD, rc ? "not sent" : "sent");
}

static void servregReceive(void *context, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context; (void)port;
    QcomQmiMessage m;
    if (qcom_qmi_parse(data, length, &m)) {IOLog("PianoPower: servreg: a message that is not QMI (%u bytes)\n", length); return;}
    uint16_t result = 0, error = 0;
    if (m.type == QCOM_QMI_RESPONSE && m.message == SERVREG_REGISTER_LISTENER) {
        if (servreg_result(&m, &result, &error)) {
            IOLog("PianoPower: servreg: the listener was refused (result %u, error %u)\n", result, error);
            gRegistered = 0;
            return;
        }
        const uint32_t state = servreg_listener_state(&m);
        if (state) pdState(state, "registration answer");
        else IOLog("PianoPower: servreg: listener registered; no state yet\n");
        return;
    }
    if (m.type == QCOM_QMI_INDICATION && m.message == SERVREG_STATE_UPDATED) {
        uint32_t state = 0;
        uint16_t indTxn = 0;
        if (servreg_state_updated(&m, SERVREG_CHARGER_PD, &state, &indTxn)) return;  /* another domain's */
        pdState(state, "indication");
        uint8_t ack[96];
        const uint32_t n = servreg_set_ack(ack, sizeof ack, gTxn++, SERVREG_CHARGER_PD, indTxn);
        const int rc = n ? qcom_qrtr_send_data(q, kServregPort, node, from, ack, n) : -1;
        IOLog("PianoPower: servreg: state change acknowledged (transaction %u): %s\n", indTxn, rc ? "not sent" : "sent");
        return;
    }
    if (m.type == QCOM_QMI_RESPONSE && m.message == SERVREG_SET_ACK) {
        if (servreg_result(&m, &result, &error)) IOLog("PianoPower: servreg: the acknowledgement was refused (result %u, error %u)\n", result, error);
        return;
    }
    IOLog("PianoPower: servreg: message %#x type %u ignored\n", m.message, m.type);
}

static IOLock *volatile gLock;
static QrtrPorts gPorts;
static uint8_t gStarted;         /* piano_qrtr_start ran: the router exists */
static uint32_t gSendKeep;       /* while a client's packet is queued: the IPCRTR queue slots kept for PianoPower's own packets */
static volatile uint32_t gMatchDue;
enum {kClientKeep = 4, kSlowMs = 20, kCloseWaitMs = 5000};

/* The data packet of the router step in progress for a client port (one per step), delivered once the lock is dropped. */
static struct {uint8_t pending, slot; uint32_t number, node, from, length; const uint8_t *data;} gRx;

static IOLock *routerLock(void)
{
    IOLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
    if (lock) return lock;
    IOLock *fresh = IOLockAlloc(), *expected = NULL;
    if (!fresh) return NULL;
    if (__atomic_compare_exchange_n(&gLock, &expected, fresh, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return fresh;
    IOLockFree(fresh);
    return expected;
}

static int send(void *context, const uint8_t *packet, uint32_t length)
{
    (void)context;
    return piano_adsp_send_keep(gChannel, packet, length, gSendKeep) ? -1 : 0;
}

/* A data packet for a client port (in qcom_qrtr_receive, under the lock): kept for delivery after the step. */
static void clientRx(void *context, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context; (void)q;
    const int slot = qrtr_ports_find(&gPorts, port);
    if (slot < 0) return;
    gRx.pending = 1; gRx.slot = (uint8_t)slot; gRx.number = port; gRx.node = node; gRx.from = from; gRx.data = data; gRx.length = length;
}

/* Under the lock: the port's callbacks for one call, the port marked busy; 0 when it is closed or closing. */
static int claim(uint8_t slot, uint32_t number, PianoQrtrClient *client, void **context)
{
    QrtrPort *p = &gPorts.port[slot];
    if (!p->used || p->closing || p->number != number) return 0;
    p->busy++;
    *client = p->client;
    *context = p->context;
    return 1;
}

static void unclaim(uint8_t slot, uint32_t number)
{
    QrtrPort *p = &gPorts.port[slot];
    if (p->used && p->number == number && p->busy) p->busy--;
}

static void timed(uint64_t since, const char *who)
{
    const uint64_t ms = piano_adsp_now_ms() - since;
    if (ms > kSlowMs) piano_adsp_slow_callback(who, (uint32_t)ms);
}

/* With the lock held, after a router step: the server changes of the client ports and the step's data packet; drops the lock and
 * delivers them (servers first). */
static void deliverAndUnlock(IOLock *lock)
{
    QrtrPortEvent events[2 * PIANO_QRTR_MAX_PORTS];
    const unsigned n = qrtr_ports_match(&gPorts, gStarted ? &gQrtr : NULL, events, sizeof events / sizeof events[0]);
    const uint8_t pending = gRx.pending, slot = gRx.slot;
    const uint32_t number = gRx.number, node = gRx.node, from = gRx.from, length = gRx.length;
    const uint8_t *data = gRx.data;
    gRx.pending = 0;
    gMatchDue = 0;
    IOLockUnlock(lock);
    for (unsigned i = 0; i < n; ++i) {
        const QrtrPortEvent *e = &events[i];
        IOLog("PianoPower: qrtr: port %u: its service %s %u:%u (instance %#x, generation %u)\n", e->number, e->up ? "is at" : "left",
              e->node, e->port, e->instance, e->generation);
        PianoQrtrClient c;
        void *context = NULL;
        IOLockLock(lock);
        const int ok = claim(e->slot, e->number, &c, &context);
        IOLockUnlock(lock);
        if (!ok) continue;
        if (c.server) {
            const uint64_t t = piano_adsp_now_ms();
            c.server(context, e->up, e->node, e->port, e->instance);
            timed(t, "a QRTR port's server callback");
        }
        IOLockLock(lock);
        unclaim(e->slot, e->number);
        IOLockUnlock(lock);
    }
    if (!pending) return;
    PianoQrtrClient c;
    void *context = NULL;
    IOLockLock(lock);
    const int ok = claim(slot, number, &c, &context);
    if (ok) gPorts.delivered++;
    IOLockUnlock(lock);
    if (!ok) return;
    if (c.receive) {
        const uint64_t t = piano_adsp_now_ms();
        c.receive(context, node, from, data, length);   /* data: the GLINK intent, valid until linkReceive returns */
        timed(t, "a QRTR port's receive callback");
    }
    IOLockLock(lock);
    unclaim(slot, number);
    IOLockUnlock(lock);
}

static void locator(void *context, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context;
    uint8_t answer[480];
    QcomQmiMessage m;
    uint32_t size = 0;
    char name[48] = "?";
    gRequests++;
    if (!qcom_qmi_parse(data, length, &m)) {
        const uint8_t *text = qcom_qmi_tlv(&m, 0x01, &size);
        for (uint32_t i = 0; text && i < size && i < sizeof name - 1; ++i) name[i] = text[i] >= 32 && text[i] < 127 ? (char)text[i] : '.';
        if (text) name[size < sizeof name - 1 ? size : sizeof name - 1] = 0;
    }
    const uint32_t n = qcom_servloc_answer(kDomains, sizeof kDomains / sizeof kDomains[0], data, length, answer, sizeof answer);
    const int sent = n && !qcom_qrtr_send_data(q, port, node, from, answer, n);
    if (sent) gAnswers++;
    IOLog("PianoPower: qrtr: the DSP asks the locator for \"%s\": %s\n", name, sent ? "answered" : n ? "answer not sent" : "not answered");
}

/* The link's state. The old link's hello and services are forgotten when it goes DOWN, not when the new one comes up: the DSP may send
 * as soon as it holds one of our intents, before the channel reports up here, and its first message is its name service's HELLO.
 * Losing that HELLO left Linux with every channel open and no service at all, 1 cold boot in 26 (docs/ubuntu-history/kernel.md §2.5,
 * K:235ab34e2a6e); here a HELLO (and the servers after it) that came first is kept, and ours goes out on "up" if it has not yet. */
static void forget(void)
{
    gRegistered = 0;
    gQrtr.hello_sent = gQrtr.hello_received = 0;
    gQrtr.remote_count = 0;
    gQrtr.remote_node = QCOM_QRTR_NODE_BROADCAST;
}

static void linkState(void *context, int up)
{
    (void)context;
    IOLock *lock = routerLock();
    if (!lock) return;
    IOLockLock(lock);
    if (!up) {
        if (gPdState) pdState(0, "link down");
        forget();
        gStage = 1;
    } else {
        gStage = 2;
        if (gQrtr.hello_received) {
            gStage = 3;
            IOLog("PianoPower: qrtr: the DSP's hello came before the channel's up (node %u, %u services): kept\n", gQrtr.remote_node,
                  gQrtr.remote_count);
        }
        if (!gQrtr.hello_sent) qcom_qrtr_start(&gQrtr);
        servregRegister();
    }
    deliverAndUnlock(lock);
}

static void linkReceive(void *context, const uint8_t *data, uint32_t length)
{
    (void)context;
    IOLock *lock = routerLock();
    if (!lock) return;
    IOLockLock(lock);
    const int had = notifier() >= 0;
    qcom_qrtr_receive(&gQrtr, data, length);
    if (gQrtr.hello_received && gStage == 2) {
        gStage = 3;
        IOLog("PianoPower: qrtr: the DSP said hello (node %u)\n", gQrtr.remote_node);
    }
    const int has = notifier() >= 0;
    if (had && !has) {   /* the notifier is gone (its domain restarts): register again when it is back */
        gRegistered = 0;
        if (gPdState) pdState(0, "notifier gone");
    }
    servregRegister();
    deliverAndUnlock(lock);
}

/* The IPCRTR receive buffers: 2048 bytes (a sensor attribute report is about 1000 bytes of protobuf plus 50 of headers). */
enum {kRouterIntent = 2048, kRouterIntents = 8};

void piano_qrtr_start(void)
{
    static const PianoAdspClient client = {linkState, linkReceive};
    IOLock *lock = routerLock();
    if (!lock) return;
    IOLockLock(lock);
    qcom_qrtr_init(&gQrtr, kLocalNode, send, NULL);
    qcom_qrtr_add_service(&gQrtr, QCOM_SERVLOC_SERVICE, QCOM_SERVLOC_INSTANCE, kLocatorPort, locator, NULL);
    qcom_qrtr_add_client(&gQrtr, kServregPort, servregReceive, NULL);
    for (unsigned i = 0; i < PIANO_QRTR_MAX_PORTS; ++i)   /* the ports clients opened before the router existed */
        if (gPorts.port[i].used && !gPorts.port[i].closing) qcom_qrtr_add_client(&gQrtr, gPorts.port[i].number, clientRx, NULL);
    gStarted = 1;
    IOLockUnlock(lock);
    gChannel = piano_adsp_channel("IPCRTR", kRouterIntent, kRouterIntents, 0, &client, NULL);
    gStage = gChannel ? 1 : 0;
}

/* On the DSP thread at every pass: the server changes a client's open asked for (PianoAdsp.cpp's service()). */
void piano_qrtr_poll(void)
{
    if (!__atomic_load_n(&gMatchDue, __ATOMIC_ACQUIRE)) return;
    IOLock *lock = routerLock();
    if (!lock) return;
    IOLockLock(lock);
    deliverAndUnlock(lock);
}

/* ---- the client ports (PianoAdspContract.h) ---- */
int piano_qrtr_open(uint32_t service, uint32_t instance, const PianoQrtrClient *client, void *context)
{
    IOLock *lock = routerLock();
    if (!lock) return -1;
    IOLockLock(lock);
    const int slot = qrtr_ports_open(&gPorts, service, instance, client, context);
    int rc = slot;
    if (slot >= 0) {
        rc = (int)gPorts.port[slot].number;
        if (gStarted && qcom_qrtr_add_client(&gQrtr, gPorts.port[slot].number, clientRx, NULL)) {qrtr_ports_free(&gPorts, slot); rc = -1;}
        else __atomic_store_n(&gMatchDue, 1u, __ATOMIC_RELEASE);
    }
    IOLockUnlock(lock);
    if (rc >= 0) {
        IOLog("PianoPower: qrtr: port %d opened, following service %u instance %#x\n", rc, service, instance);
        piano_adsp_wake();
    }
    return rc;
}

int piano_qrtr_server(int port, PianoQrtrServer *out)
{
    IOLock *lock = routerLock();
    if (!lock || !out || port <= 0) return -1;
    IOLockLock(lock);
    const int slot = qrtr_ports_find(&gPorts, (uint32_t)port);
    if (slot >= 0) *out = gPorts.port[slot].server;
    IOLockUnlock(lock);
    return slot >= 0 ? 0 : -1;
}

int piano_qrtr_send(int port, uint32_t node, uint32_t to_port, const void *data, uint32_t length)
{
    IOLock *lock = routerLock();
    if (!lock) return -3;
    if (port <= 0 || !data || !length || length > PIANO_QRTR_MAX_DATA) return -2;
    IOLockLock(lock);
    int rc;
    const int slot = qrtr_ports_find(&gPorts, (uint32_t)port);
    if (slot < 0 || gPorts.port[slot].closing) rc = -2;
    else if (!gStarted || !gQrtr.hello_received) rc = -3;
    else {
        gSendKeep = kClientKeep;
        rc = qcom_qrtr_send_data(&gQrtr, (uint32_t)port, node, to_port, (const uint8_t *)data, length) ? -1 : 0;
        gSendKeep = 0;
        if (rc) gPorts.refused++;
    }
    IOLockUnlock(lock);
    return rc;
}

void piano_qrtr_close(int port)
{
    IOLock *lock = routerLock();
    if (!lock || port <= 0) return;
    IOLockLock(lock);
    const int slot = qrtr_ports_find(&gPorts, (uint32_t)port);
    if (slot < 0 || gPorts.port[slot].closing) {IOLockUnlock(lock); return;}
    QrtrPort *p = &gPorts.port[slot];
    p->closing = 1;   /* no callback starts from here on */
    if (gStarted) {
        if (gQrtr.hello_received) qcom_qrtr_del_client(&gQrtr, (uint32_t)port);
        qcom_qrtr_remove_port(&gQrtr, (uint32_t)port);
    }
    /* A callback in progress on the DSP thread finishes first, unless this is that callback closing its own port. */
    const int self = piano_adsp_on_thread();
    unsigned waited = 0;
    while (!self && p->busy && waited < kCloseWaitMs) {IOLockUnlock(lock); IOSleep(1); waited++; IOLockLock(lock);}
    if (p->busy && !self) IOLog("PianoPower: qrtr: port %d closed with a callback still running after %u ms\n", port, waited);
    qrtr_ports_free(&gPorts, slot);
    IOLockUnlock(lock);
    IOLog("PianoPower: qrtr: port %d closed\n", port);
}

void piano_qrtr_port_counters(uint32_t *open, uint32_t *delivered, uint32_t *refused)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < PIANO_QRTR_MAX_PORTS; ++i) n += gPorts.port[i].used;
    *open = n;
    *delivered = gPorts.delivered;
    *refused = gPorts.refused;
}

int piano_qrtr_link_up(void)
{
    return gStage >= 3 && gQrtr.hello_received;
}

void piano_qrtr_counters(uint32_t *stage, uint32_t *requests, uint32_t *answers, uint32_t *services)
{
    *stage = gStage;
    *requests = gRequests;
    *answers = gAnswers;
    *services = gQrtr.remote_count;
}

uint32_t piano_qrtr_charger_pd(uint32_t *ups)
{
    if (ups) *ups = __atomic_load_n(&gPdUps, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&gPdState, __ATOMIC_ACQUIRE);
}
