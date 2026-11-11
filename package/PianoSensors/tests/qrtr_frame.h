// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// wire.c's vector 4 (a QRTR data packet around an SNS request) built by the router piano really uses: PianoPower's QcomQrtr.c
// (UniXNU-sheng's, compiled in from ../../PianoPower, read only), instead of PaganiAdsp's qcom_qrtr_frame. Same signature.
#ifndef SENSORS_TEST_QRTR_FRAME_H
#define SENSORS_TEST_QRTR_FRAME_H
#include <stdint.h>
#include <string.h>
#include "../../PianoPower/QcomQrtr.h"

static uint8_t frame_packet[1100];
static uint32_t frame_length;

static int frame_capture(void *context, const uint8_t *packet, uint32_t length)
{
    (void)context;
    if (length > sizeof frame_packet) return -1;
    memcpy(frame_packet, packet, length);
    frame_length = length;
    return 0;
}

static inline uint32_t qcom_qrtr_frame(uint8_t *out, uint32_t capacity, uint32_t type, uint32_t src_node, uint32_t src_port,
                                       uint32_t dst_node, uint32_t dst_port, const uint8_t *data, uint32_t length)
{
    QcomQrtr q;
    if (type != QCOM_QRTR_DATA) return 0;
    qcom_qrtr_init(&q, src_node, frame_capture, NULL);
    frame_length = 0;
    if (qcom_qrtr_send_data(&q, src_port, dst_node, dst_port, data, length) || frame_length > capacity) return 0;
    memcpy(out, frame_packet, frame_length);
    return frame_length;
}
#endif
