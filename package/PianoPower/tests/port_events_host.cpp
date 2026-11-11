// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the USB-C port events' fan-out (PortEvents.c, PianoAdspContract.h "USB-C port events"): registration (bad arguments,
// the four slots, full), delivery to every client in slot order with the sequence and the bytes as received (a short notification
// zero-padded, a long one cut at 32 with its real length), nothing delivered and only the count moved without a client, and the
// charger codec's side of the hook: PianoMca.c returns McaPanNotified for a 32-byte DisplayPort notification and queues its
// acknowledgement whether or not anyone watches.
//   c++ -std=c++20 -I.. -I../../include port_events_host.cpp ../PortEvents.c ../PianoMca.c -DPIANO_PORT_EVENTS_TEST
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" {
#include "PortEvents.h"
#include "PianoMca.h"
}

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static void put32(uint8_t *p, uint32_t v) {p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);}

struct Seen {
    int who;
    PianoPortEvent event;
};
static std::vector<Seen> seen;
static void record(void *context, const PianoPortEvent *event) {seen.push_back({*static_cast<int *>(context), *event});}

// A DisplayPort notification as the DSP sends it (struct usbc_notify, 32 bytes): port, orientation, mux, pin assignment, HPD.
static std::vector<uint8_t> dpNotify(uint8_t port, uint8_t orientation, uint8_t mux, uint8_t pin, bool hpd, bool irq)
{
    std::vector<uint8_t> m(32, 0);
    put32(&m[0], PIANO_MCA_OWNER_PAN);
    put32(&m[4], 2);
    put32(&m[8], PIANO_MCA_PAN_NOTIFY | 0xff01u << 16);
    m[12] = port; m[13] = orientation; m[14] = mux;
    m[18] = 0x01; m[19] = 0xff;
    m[20] = uint8_t(pin | (hpd ? 0x40 : 0) | (irq ? 0x80 : 0));
    return m;
}

int main()
{
    piano_port_reset_for_test();
    // Registration.
    PianoPortClient none = {nullptr};
    CHECK(piano_port_events(nullptr, nullptr) == -2);
    CHECK(piano_port_events(&none, nullptr) == -2);
    // Without a client: nothing delivered, the count moves.
    const auto first = dpNotify(0, 1, 3, 4, true, false);
    CHECK(piano_port_seen() == 0);
    CHECK(piano_port_publish(first.data(), uint32_t(first.size())) == 0);
    CHECK(piano_port_seen() == 1);
    CHECK(seen.empty());
    int ids[5] = {0, 1, 2, 3, 4};
    const PianoPortClient client = {record};
    CHECK(piano_port_events(&client, &ids[0]) == 0);
    CHECK(piano_port_events(&client, &ids[1]) == 1);
    // Delivery in slot order, the sequence counting every notification since the start (the first one included), the bytes intact.
    const auto second = dpNotify(0, 0, 2, 3, true, false);
    CHECK(piano_port_publish(second.data(), uint32_t(second.size())) == 2);
    CHECK(seen.size() == 2);
    if (seen.size() == 2) {
        CHECK(seen[0].who == 0 && seen[1].who == 1);
        CHECK(seen[0].event.sequence == 2 && seen[1].event.sequence == 2);
        CHECK(seen[0].event.length == 32);
        CHECK(!std::memcmp(seen[0].event.data, second.data(), 32));
    }
    // A short message zero-padded, a long one cut at 32 with its real length.
    seen.clear();
    const uint8_t shortOne[13] = {0x0c, 0x80, 0, 0, 2, 0, 0, 0, 0x16, 0, 0, 0, 1};
    CHECK(piano_port_publish(shortOne, sizeof shortOne) == 2);
    CHECK(seen.size() == 2 && seen[0].event.length == 13 && seen[0].event.data[12] == 1 && seen[0].event.data[13] == 0 &&
          seen[0].event.data[31] == 0);
    seen.clear();
    std::vector<uint8_t> longOne(40, 0xab);
    CHECK(piano_port_publish(longOne.data(), uint32_t(longOne.size())) == 2);
    CHECK(seen.size() == 2 && seen[1].event.length == 40 && seen[1].event.data[31] == 0xab);
    // Four slots in all.
    CHECK(piano_port_events(&client, &ids[2]) == 2);
    CHECK(piano_port_events(&client, &ids[3]) == 3);
    CHECK(piano_port_events(&client, &ids[4]) == -1);
    seen.clear();
    CHECK(piano_port_publish(first.data(), uint32_t(first.size())) == 4);
    CHECK(seen.size() == 4 && seen[3].who == 3 && seen[3].event.sequence == 5);
    CHECK(piano_port_seen() == 5);
    CHECK(piano_port_publish(nullptr, 0) == 0);   // counted, not delivered

    // The codec's side of the hook: a 32-byte DisplayPort notification is McaPanNotified and its acknowledgement is queued
    // (PianoPower then publishes the same bytes), with or without clients.
    PianoMca m;
    piano_mca_init(&m, 0, 0);
    piano_mca_link(&m, 1);
    uint8_t out[PIANO_MCA_PAN_REQUEST_BYTES];
    CHECK(piano_mca_pan_next(&m, out) == PIANO_MCA_PAN_REQUEST_BYTES);   // the enable first
    uint8_t answer[24] = {};
    put32(answer, PIANO_MCA_OWNER_PAN); put32(answer + 4, 1); put32(answer + 8, PIANO_MCA_PAN_WRITE);
    CHECK(piano_mca_receive(&m, answer, sizeof answer) == McaPanDone);
    const auto dp = dpNotify(0, 1, 3, 4, true, false);
    CHECK(piano_mca_receive(&m, dp.data(), uint32_t(dp.size())) == McaPanNotified);
    CHECK(piano_mca_pan_next(&m, out) == PIANO_MCA_PAN_REQUEST_BYTES);
    CHECK(out[12] == PIANO_MCA_PAN_ACK && out[16] == 0);   // command 0x11, argument: port 0

    std::printf("port events host tests: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
