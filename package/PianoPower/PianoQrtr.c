/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The IPC router link to the audio DSP (GLINK channel "IPCRTR", which the DSP opens) and the service locator on it: UniXNU-sheng's
 * ShengQrtr.c (UPSTREAM) on PianoAdsp.h. The DSP's protection domains (audio, charger, sensors) ask the application processor's locator
 * which domains provide a service before they start, and the charger properties are served from the charger domain: without these
 * answers they never come up. The domain table is the one Linux's pd-mapper uses for SM8750 (its SM8550 table), the same as sheng's.
 * Everything runs in the DSP thread's callbacks: each packet is handled at once and any answer is queued with piano_adsp_send. */
#include <stddef.h>
#include <stdint.h>
#include "QcomQrtr.h"
#include "PianoAdsp.h"
#include "Servreg.h"
#include <IOKit/IOLib.h>

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

static void servregRegister(void)
{
    const int i = qcom_qrtr_find_remote(&gQrtr, SERVREG_NOTIFIER_SERVICE);
    if (gRegistered || i < 0) return;
    if ((gQrtr.remote[i].instance >> 8) != SERVREG_ADSP_INSTANCE) return;
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

static int send(void *context, const uint8_t *packet, uint32_t length)
{
    (void)context;
    return piano_adsp_send(gChannel, packet, length) ? -1 : 0;
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

static void linkState(void *context, int up)
{
    (void)context;
    gRegistered = 0;
    if (gPdState) pdState(0, up ? "link restarted" : "link down");
    if (up) {
        /* a fresh link: forget the old one's hello and services, keep ours */
        gQrtr.hello_sent = gQrtr.hello_received = 0;
        gQrtr.remote_count = 0;
        gQrtr.remote_node = QCOM_QRTR_NODE_BROADCAST;
        gStage = 2;
        qcom_qrtr_start(&gQrtr);
    } else {
        gStage = 1;
    }
}

static void linkReceive(void *context, const uint8_t *data, uint32_t length)
{
    (void)context;
    const int had = qcom_qrtr_find_remote(&gQrtr, SERVREG_NOTIFIER_SERVICE) >= 0;
    qcom_qrtr_receive(&gQrtr, data, length);
    if (gQrtr.hello_received && gStage == 2) {
        gStage = 3;
        IOLog("PianoPower: qrtr: the DSP said hello (node %u)\n", gQrtr.remote_node);
    }
    const int has = qcom_qrtr_find_remote(&gQrtr, SERVREG_NOTIFIER_SERVICE) >= 0;
    if (had && !has) {   /* the notifier is gone (its domain restarts): register again when it is back */
        gRegistered = 0;
        if (gPdState) pdState(0, "notifier gone");
    }
    servregRegister();
}

void piano_qrtr_start(void)
{
    static const PianoAdspClient client = {linkState, linkReceive};
    qcom_qrtr_init(&gQrtr, kLocalNode, send, NULL);
    qcom_qrtr_add_service(&gQrtr, QCOM_SERVLOC_SERVICE, QCOM_SERVLOC_INSTANCE, kLocatorPort, locator, NULL);
    qcom_qrtr_add_client(&gQrtr, kServregPort, servregReceive, NULL);
    gChannel = piano_adsp_channel("IPCRTR", 1024, 8, 0, &client, NULL);
    gStage = gChannel ? 1 : 0;
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
