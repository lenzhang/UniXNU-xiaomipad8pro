/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The USB-C port events' fan-out (PianoAdspContract.h, "USB-C port events"): PianoPower's side. A small read-only table of client
 * callbacks and the one call PianoPower.cpp makes after the charger codec took a port notification (PianoMca.c receive_pan, McaPanNotified):
 * nothing here sends, acknowledges or decides anything; without a registered client a notification costs one atomic load.
 * Portable C, no allocation, no I/O; registration and delivery use atomics (no lock: a client registers from its own thread while
 * PianoPower's thread may be delivering). Host-tested (tests/port_events_host.cpp). docs/drivers/displayport.md D2. */
#ifndef PIANO_PORT_EVENTS_H
#define PIANO_PORT_EVENTS_H
#include <stdint.h>
#include "PianoAdspContract.h"
#ifdef __cplusplus
extern "C" {
#endif

/* PianoPower's thread only: the notification as received (data, length) to every registered client, in slot order. Returns the
 * number of clients called. */
unsigned piano_port_publish(const uint8_t *data, uint32_t length);
/* Host tests only (built with PIANO_PORT_EVENTS_TEST): every slot free again, the count back to 0. */
void piano_port_reset_for_test(void);

#ifdef __cplusplus
}
#endif
#endif
