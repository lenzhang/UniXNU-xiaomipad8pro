/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The USB-C port events' fan-out (PortEvents.h). A slot is claimed with a compare-and-swap on the claimed word, filled, and only then
 * published in the ready word (release); delivery reads the ready word (acquire) and calls each ready slot's client. Slots are never
 * freed: a client registers for the rest of the boot (kexts do not unload natively). */
#include "PortEvents.h"

typedef struct {
    PianoPortClient client;
    void *context;
} PortSlot;

static PortSlot gSlots[PIANO_PORT_CLIENTS];
static uint32_t gClaimed, gReady, gSeen;

int piano_port_events(const PianoPortClient *client, void *context)
{
    if (!client || !client->event) return -2;
    for (unsigned i = 0; i < PIANO_PORT_CLIENTS; i++) {
        const uint32_t bit = 1u << i;
        uint32_t claimed = __atomic_load_n(&gClaimed, __ATOMIC_ACQUIRE);
        while (!(claimed & bit)) {
            if (__atomic_compare_exchange_n(&gClaimed, &claimed, claimed | bit, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                gSlots[i].client = *client;
                gSlots[i].context = context;
                __atomic_fetch_or(&gReady, bit, __ATOMIC_RELEASE);
                return (int)i;
            }
        }
    }
    return -1;
}

uint32_t piano_port_seen(void)
{
    return __atomic_load_n(&gSeen, __ATOMIC_ACQUIRE);
}

unsigned piano_port_publish(const uint8_t *data, uint32_t length)
{
    const uint32_t sequence = __atomic_add_fetch(&gSeen, 1u, __ATOMIC_ACQ_REL);
    const uint32_t ready = __atomic_load_n(&gReady, __ATOMIC_ACQUIRE);
    if (!ready || !data) return 0;
    PianoPortEvent event;
    event.sequence = sequence;
    event.length = length;
    const uint32_t keep = length < PIANO_PORT_EVENT_MAX ? length : PIANO_PORT_EVENT_MAX;
    for (uint32_t i = 0; i < PIANO_PORT_EVENT_MAX; i++) event.data[i] = i < keep ? data[i] : 0;
    unsigned called = 0;
    for (unsigned i = 0; i < PIANO_PORT_CLIENTS; i++) {
        if (!(ready & (1u << i))) continue;
        gSlots[i].client.event(gSlots[i].context, &event);
        called++;
    }
    return called;
}

#ifdef PIANO_PORT_EVENTS_TEST
void piano_port_reset_for_test(void)
{
    for (unsigned i = 0; i < PIANO_PORT_CLIENTS; i++) {
        gSlots[i].client.event = 0;
        gSlots[i].context = 0;
    }
    __atomic_store_n(&gReady, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gClaimed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gSeen, 0u, __ATOMIC_RELEASE);
}
#endif
