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
#include <IOKit/IOLib.h>

enum {kLocalNode = 1, kLocatorPort = 1};

static const char *const kAudio[] = {"avs/audio", NULL};
static const char *const kGps[] = {"gps/gps_service", NULL};
static const QcomPdDomain kDomains[] = {
    {"msm/adsp/audio_pd", 74, kAudio},   {"msm/adsp/root_pd", 74, NULL}, {"msm/adsp/charger_pd", 74, NULL},
    {"msm/adsp/sensor_pd", 74, NULL},    {"msm/cdsp/root_pd", 76, NULL}, {"msm/modem/root_pd", 180, kGps},
};

static QcomQrtr gQrtr;
static PianoAdspChannel *gChannel;
static uint32_t gStage, gRequests, gAnswers;

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
    qcom_qrtr_receive(&gQrtr, data, length);
    if (gQrtr.hello_received && gStage == 2) {
        gStage = 3;
        IOLog("PianoPower: qrtr: the DSP said hello (node %u)\n", gQrtr.remote_node);
    }
}

void piano_qrtr_start(void)
{
    static const PianoAdspClient client = {linkState, linkReceive};
    qcom_qrtr_init(&gQrtr, kLocalNode, send, NULL);
    qcom_qrtr_add_service(&gQrtr, QCOM_SERVLOC_SERVICE, QCOM_SERVLOC_INSTANCE, kLocatorPort, locator, NULL);
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
