/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The IPC router (QRTR, version 1 headers) on one link to one remote node, with the name service duties of this side: say hello, announce
 * our services to the remote once it has said hello, answer flow control, and keep the list of the services the remote announces. Our
 * services are ports with a receive callback. No allocation and no waiting: the owner feeds every received packet in and gives a send
 * callback that queues one packet (it may refuse; the counters show it). */
#ifndef QCOM_QRTR_H
#define QCOM_QRTR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_QRTR_HEADER 32u
#define QCOM_QRTR_PORT_CTRL 0xfffffffeu
#define QCOM_QRTR_NODE_BROADCAST 0xffffffffu
#define QCOM_QRTR_MAX_LOCAL 12u    /* UniXNU-piano: PianoPower's two ports and the eight client ports (PianoAdspContract.h) */
#define QCOM_QRTR_MAX_REMOTE 64u
#define QCOM_QRTR_MAX_DATA 1024u   /* one data packet's payload we send */

enum {
    QCOM_QRTR_DATA = 1, QCOM_QRTR_HELLO = 2, QCOM_QRTR_BYE = 3, QCOM_QRTR_NEW_SERVER = 4, QCOM_QRTR_DEL_SERVER = 5,
    QCOM_QRTR_DEL_CLIENT = 6, QCOM_QRTR_RESUME_TX = 7, QCOM_QRTR_EXIT = 8, QCOM_QRTR_PING = 9, QCOM_QRTR_NEW_LOOKUP = 10,
    QCOM_QRTR_DEL_LOOKUP = 11,
};

typedef struct QcomQrtr QcomQrtr;
/* One packet of length bytes (header included); returns 0 when queued. */
typedef int (*QcomQrtrSend)(void *context, const uint8_t *packet, uint32_t length);
/* A data packet for one of our ports from node:port. */
typedef void (*QcomQrtrReceive)(void *context, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from_port, const uint8_t *data,
                                uint32_t length);

struct QcomQrtr {
    uint32_t local_node, remote_node;  /* remote_node: QCOM_QRTR_NODE_BROADCAST until the remote's hello */
    uint8_t hello_sent, hello_received;
    QcomQrtrSend send;
    void *send_context;
    struct {uint32_t service, instance, port; QcomQrtrReceive receive; void *context;} local[QCOM_QRTR_MAX_LOCAL];
    uint32_t local_count;
    struct {uint32_t service, instance, node, port;} remote[QCOM_QRTR_MAX_REMOTE];
    uint32_t remote_count;
    /* counters for the log */
    uint32_t rx_packets, rx_data, rx_bad, rx_unrouted, tx_packets, tx_refused, resumes_sent;
    /* UniXNU-piano (2026-11-12, PianoWifi's P21 change ported): data packets that asked for a flow-control answer, and an answer the
     * link refused, kept for qcom_qrtr_poll (the sender's node and port, and our port the answer names) */
    uint32_t rx_confirms, resume_owed, resume_node, resume_port, resume_from;
};

void qcom_qrtr_init(QcomQrtr *q, uint32_t local_node, QcomQrtrSend send, void *send_context);
/* Adds one of our services (instance as announced: version | instance << 8) on port; announced to the remote after its hello, or at
 * once when that has been. Returns 0, or -1 when full. */
int qcom_qrtr_add_service(QcomQrtr *q, uint32_t service, uint32_t instance, uint32_t port, QcomQrtrReceive receive, void *context);
/* A client port: data packets to it go to receive, and it is never announced (UniXNU-piano addition, for the servreg listener of
 * PianoQrtr.c). Returns 0, or -1 when full. */
int qcom_qrtr_add_client(QcomQrtr *q, uint32_t port, QcomQrtrReceive receive, void *context);
/* Removes one of our ports (a closed client port: UniXNU-piano addition); 0, or -1 when it is not ours. */
int qcom_qrtr_remove_port(QcomQrtr *q, uint32_t port);
/* Tells the remote that our port is gone (DEL_CLIENT {our node, port}): its servers forget the client. 0 when queued. */
int qcom_qrtr_del_client(QcomQrtr *q, uint32_t port);
/* The link is up: says hello. */
void qcom_qrtr_start(QcomQrtr *q);
/* One received packet. Returns 0, or -1 when it was not a valid packet. */
int qcom_qrtr_receive(QcomQrtr *q, const uint8_t *packet, uint32_t length);
/* A data packet from our port to the remote's node:port. capacity bounds the packet built on the stack. Returns 0 when queued. */
int qcom_qrtr_send_data(QcomQrtr *q, uint32_t from_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length);
/* Index of a remote service in the list, -1 when not announced. */
int qcom_qrtr_find_remote(const QcomQrtr *q, uint32_t service);
/* UniXNU-piano (PianoWifi's P21 change ported): a flow-control answer (RESUME_TX) the link refused when it was due, sent again (the
 * owner calls this on every turn). Returns 1 when one went out, 0 when none was owed, -1 when the link refused it again (it stays
 * owed). qcom_qrtr_receive sends the answer for every data packet whose header asks for it: it names our port (the packet's
 * destination) as the destination the sender resumes, and goes from that port to the sender's node:port. */
int qcom_qrtr_poll(QcomQrtr *q);

/* ---- QMI messages carried in data packets: a 7-byte header (type, transaction, message id, length) and TLVs ---- */
#define QCOM_QMI_HEADER 7u
enum {QCOM_QMI_REQUEST = 0, QCOM_QMI_RESPONSE = 2, QCOM_QMI_INDICATION = 4};
typedef struct {uint8_t type; uint16_t transaction, message; const uint8_t *tlvs; uint32_t length;} QcomQmiMessage;
int qcom_qmi_parse(const uint8_t *data, uint32_t length, QcomQmiMessage *m);
/* The value of TLV type in m (length in *size), or null. */
const uint8_t *qcom_qmi_tlv(const QcomQmiMessage *m, uint8_t type, uint32_t *size);

/* ---- the service locator (service 64): which protection domains provide a service ---- */
#define QCOM_SERVLOC_SERVICE 0x40u
#define QCOM_SERVLOC_INSTANCE 0x101u  /* as announced: version 0x101, instance 0 */
#define QCOM_SERVLOC_GET_DOMAIN_LIST 0x21u
typedef struct {const char *domain; uint32_t instance; const char *const *services;} QcomPdDomain;
/* Answers a locator request (a whole QMI message) into out; returns the answer's length, 0 when the request is not one to answer. Every
 * domain provides "tms/servreg"; services lists the others. */
uint32_t qcom_servloc_answer(const QcomPdDomain *domains, unsigned count, const uint8_t *request, uint32_t length, uint8_t *out,
                             uint32_t capacity);

#ifdef __cplusplus
}
#endif
#endif
