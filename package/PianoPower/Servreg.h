/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The service registry notifier the audio DSP serves on the IPC router (QMI service 0x42, one instance per processor: 74 for the
 * ADSP, as in the service locator's domain table): a listener registers for a protection domain's state and is told when it goes
 * up or down, acknowledging every change. Linux gates every pmic_glink client on "msm/adsp/charger_pd" reporting up
 * (docs/ubuntu-history/device.md §2.5); PianoPower does the same (PianoQrtr.c). Message layout as Linux's pdr messages describe it
 * (drivers/soc/qcom/qcom_pdr_msg.c, read as a specification):
 *   REGISTER_LISTENER 0x20  request: TLV 0x01 enable (u8), TLV 0x02 service path (string)
 *                           response: TLV 0x02 result/error (u16, u16), TLV 0x10 current state (u32, optional)
 *   STATE_UPDATED     0x22  indication: TLV 0x01 state (u32), TLV 0x02 service path, TLV 0x03 transaction id (u16)
 *   SET_ACK           0x23  request: TLV 0x01 service path, TLV 0x02 transaction id (u16); response: TLV 0x02 result
 * States: 0x1fffffff up, 0x0fffffff down, 0x2fffffff early down, 0x7fffffff uninitialised. Pure: builds and reads messages. */
#ifndef PIANO_POWER_SERVREG_H
#define PIANO_POWER_SERVREG_H
#include <stdint.h>
#include "QcomQrtr.h"

#define SERVREG_NOTIFIER_SERVICE 0x42u
#define SERVREG_ADSP_INSTANCE 74u
#define SERVREG_REGISTER_LISTENER 0x20u
#define SERVREG_STATE_UPDATED 0x22u
#define SERVREG_SET_ACK 0x23u
#define SERVREG_STATE_UP 0x1fffffffu
#define SERVREG_STATE_DOWN 0x0fffffffu
#define SERVREG_STATE_EARLY_DOWN 0x2fffffffu
#define SERVREG_STATE_UNINIT 0x7fffffffu
#define SERVREG_CHARGER_PD "msm/adsp/charger_pd"

static inline uint32_t servreg_tlv(uint8_t *out, uint32_t at, uint32_t cap, uint8_t type, const void *value, uint16_t length)
{
    if (at + 3u + length > cap) return 0;
    out[at] = type;
    out[at + 1] = (uint8_t)length;
    out[at + 2] = (uint8_t)(length >> 8);
    for (uint16_t i = 0; i < length; ++i) out[at + 3 + i] = ((const uint8_t *)value)[i];
    return at + 3u + length;
}

static inline uint32_t servreg_header(uint8_t *out, uint8_t type, uint16_t txn, uint16_t msg, uint32_t end)
{
    const uint32_t length = end - QCOM_QMI_HEADER;
    out[0] = type;
    out[1] = (uint8_t)txn; out[2] = (uint8_t)(txn >> 8);
    out[3] = (uint8_t)msg; out[4] = (uint8_t)(msg >> 8);
    out[5] = (uint8_t)length; out[6] = (uint8_t)(length >> 8);
    return end;
}

static inline uint16_t servreg_strlen(const char *s)
{
    uint16_t n = 0;
    while (s[n] && n < 64) ++n;
    return n;
}

/* REGISTER_LISTENER for path (enable 1); returns the message length, 0 when it does not fit. */
static inline uint32_t servreg_register_listener(uint8_t *out, uint32_t cap, uint16_t txn, const char *path)
{
    const uint8_t enable = 1;
    uint32_t at = QCOM_QMI_HEADER;
    if (cap < at) return 0;
    if (!(at = servreg_tlv(out, at, cap, 0x01, &enable, 1))) return 0;
    if (!(at = servreg_tlv(out, at, cap, 0x02, path, servreg_strlen(path)))) return 0;
    return servreg_header(out, QCOM_QMI_REQUEST, txn, SERVREG_REGISTER_LISTENER, at);
}

/* SET_ACK of the indication with transaction id ind_txn. */
static inline uint32_t servreg_set_ack(uint8_t *out, uint32_t cap, uint16_t txn, const char *path, uint16_t ind_txn)
{
    const uint8_t id[2] = {(uint8_t)ind_txn, (uint8_t)(ind_txn >> 8)};
    uint32_t at = QCOM_QMI_HEADER;
    if (cap < at) return 0;
    if (!(at = servreg_tlv(out, at, cap, 0x01, path, servreg_strlen(path)))) return 0;
    if (!(at = servreg_tlv(out, at, cap, 0x02, id, 2))) return 0;
    return servreg_header(out, QCOM_QMI_REQUEST, txn, SERVREG_SET_ACK, at);
}

static inline uint32_t servreg_le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

/* A response's result TLV: 0 when it says success. */
static inline int servreg_result(const QcomQmiMessage *m, uint16_t *result, uint16_t *error)
{
    uint32_t size = 0;
    const uint8_t *r = qcom_qmi_tlv(m, 0x02, &size);
    if (!r || size < 4) return -1;
    *result = (uint16_t)(r[0] | r[1] << 8);
    *error = (uint16_t)(r[2] | r[3] << 8);
    return *result ? -1 : 0;
}

/* The current state a REGISTER_LISTENER response carries, 0 when it has none. */
static inline uint32_t servreg_listener_state(const QcomQmiMessage *m)
{
    uint32_t size = 0;
    const uint8_t *v = qcom_qmi_tlv(m, 0x10, &size);
    return v && size >= 4 ? servreg_le32(v) : 0;
}

/* A STATE_UPDATED indication: 0 when it names path (the state and the transaction id to acknowledge come back). */
static inline int servreg_state_updated(const QcomQmiMessage *m, const char *path, uint32_t *state, uint16_t *ind_txn)
{
    uint32_t size = 0;
    const uint8_t *s = qcom_qmi_tlv(m, 0x01, &size);
    if (!s || size < 4) return -1;
    *state = servreg_le32(s);
    const uint8_t *p = qcom_qmi_tlv(m, 0x02, &size);
    const uint16_t n = servreg_strlen(path);
    if (!p || size != n) return -1;
    for (uint16_t i = 0; i < n; ++i) if (p[i] != (uint8_t)path[i]) return -1;
    const uint8_t *t = qcom_qmi_tlv(m, 0x03, &size);
    if (!t || size < 2) return -1;
    *ind_txn = (uint16_t)(t[0] | t[1] << 8);
    return 0;
}

static inline const char *servreg_state_name(uint32_t state)
{
    return state == SERVREG_STATE_UP ? "up" : state == SERVREG_STATE_DOWN ? "down" : state == SERVREG_STATE_EARLY_DOWN ? "early down"
         : state == SERVREG_STATE_UNINIT ? "uninitialised" : state ? "other" : "unknown";
}

#endif
