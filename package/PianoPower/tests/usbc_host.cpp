// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the USB-C relay's core (UsbcRelay.c; the contract is package/include/PianoUsbcContract.h): which requests a client may
// send (UCSI reads and writes of owner 0x800b only: never the charger's owner, never the port owner's enable or acknowledgement), the
// queue and its refusals, the ready transitions and the state() calls they make due (a new generation is told once, requests of the
// old one are dropped), whose a received message is (the client's UCSI traffic is not PianoMca's while the client is ready; a port
// notification is never the relay's: PianoMca acknowledges it, PortEvents.c hands it on), and PianoPower's order of the two (the hook
// runs before piano_mca_receive).
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" {
#include "UsbcRelay.h"
#include "PianoMca.h"
}

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static void put32(uint8_t *p, uint32_t v) {p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);}

static std::vector<uint8_t> msg(uint32_t owner, uint32_t type, uint32_t opcode, uint32_t length)
{
    std::vector<uint8_t> m(length, 0);
    if (length >= 12) {put32(&m[0], owner); put32(&m[4], type); put32(&m[8], opcode);}
    return m;
}

static void noState(void *, int, uint32_t) {}
static void noUcsi(void *, const uint8_t *, uint32_t) {}
static const PianoUsbcClient kClient = {noState, noUcsi};

static void testRequests()
{
    CHECK(usbc_relay_request_ok(msg(0x800b, 1, 0x11, 12).data(), 12));
    CHECK(usbc_relay_request_ok(msg(0x800b, 1, 0x12, 64).data(), 64));     // 1.x write: header + 48 + reserved
    CHECK(usbc_relay_request_ok(msg(0x800b, 1, 0x12, 544).data(), 544));   // 2.x write: header + 528 + reserved
    CHECK(!usbc_relay_request_ok(msg(0x800b, 1, 0x11, 16).data(), 16));    // a read is the header alone
    CHECK(!usbc_relay_request_ok(msg(0x800b, 1, 0x12, 60).data(), 60));    // a write is a whole buffer
    CHECK(!usbc_relay_request_ok(msg(0x800b, 1, 0x13, 24).data(), 24));    // notifications come from the DSP
    CHECK(!usbc_relay_request_ok(msg(0x800b, 2, 0x11, 12).data(), 12));    // type 1 only
    CHECK(!usbc_relay_request_ok(msg(0x800a, 1, 0x12, 276).data(), 276));  // never the charger's owner
    CHECK(!usbc_relay_request_ok(msg(0x800a, 1, 0x11, 12).data(), 12));
    CHECK(!usbc_relay_request_ok(msg(0x800c, 1, 0x15, 24).data(), 24));    // never the port enable or acknowledgement
    CHECK(!usbc_relay_request_ok(msg(0x800b, 1, 0x11, 8).data(), 8));
    CHECK(!usbc_relay_request_ok(nullptr, 12));
}

static void testQueueAndLink()
{
    UsbcRelay r;
    usbc_relay_init(&r);
    const auto read = msg(0x800b, 1, 0x11, 12);
    CHECK(usbc_relay_queue(&r, read.data(), 12) == -4);                   // nobody attached
    CHECK(usbc_relay_attach(&r, nullptr, nullptr) == -2);
    const PianoUsbcClient noUcsiCallback = {noState, nullptr};
    CHECK(usbc_relay_attach(&r, &noUcsiCallback, nullptr) == -2);
    CHECK(usbc_relay_attach(&r, &kClient, nullptr) == 0);
    CHECK(usbc_relay_attach(&r, &kClient, nullptr) == -1);                // one client
    CHECK(!usbc_relay_link(&r, 0, 0));                                    // not ready, nothing to tell
    CHECK(usbc_relay_queue(&r, read.data(), 12) == -3);
    // Ready with generation 1: told once; requests flow only after the state() call.
    CHECK(usbc_relay_link(&r, 1, 1));
    CHECK(usbc_relay_queue(&r, read.data(), 12) == -3);                   // not told yet
    usbc_relay_told(&r);
    CHECK(!usbc_relay_link(&r, 1, 1));
    CHECK(usbc_relay_queue(&r, read.data(), 12) == 0);
    CHECK(r.kick == 1);
    const auto bad = msg(0x800a, 1, 0x11, 12);
    CHECK(usbc_relay_queue(&r, bad.data(), 12) == -2);
    for (int i = 0; i < 3; ++i) CHECK(usbc_relay_queue(&r, read.data(), 12) == 0);
    CHECK(usbc_relay_queue(&r, read.data(), 12) == -1);                   // PIANO_USBC_QUEUE
    CHECK(r.refused == 5);                                                // every refusal counts (-4, -3, -3, -2, -1)
    const uint8_t *data = nullptr;
    CHECK(usbc_relay_peek(&r, &data) == 12 && data && data[8] == 0x11);
    usbc_relay_pop(&r);
    CHECK(r.count == 3 && r.sent == 1);
    // A new generation while ready (charger_pd restarted): told again, the old generation's requests dropped.
    CHECK(usbc_relay_link(&r, 1, 2));
    CHECK(r.count == 0 && r.dropped == 3 && r.generation == 2);
    CHECK(usbc_relay_peek(&r, &data) == 0);
    usbc_relay_told(&r);
    CHECK(usbc_relay_queue(&r, read.data(), 12) == 0);
    // Not ready: the queue is dropped, told again, sends refused.
    CHECK(usbc_relay_link(&r, 0, 2));
    CHECK(r.count == 0 && r.dropped == 4);
    usbc_relay_told(&r);
    CHECK(usbc_relay_queue(&r, read.data(), 12) == -3);
    // Attaching to a ready owner: state(1) is due at once.
    UsbcRelay late;
    usbc_relay_init(&late);
    CHECK(!usbc_relay_link(&late, 1, 7));
    CHECK(usbc_relay_attach(&late, &kClient, nullptr) == 0);
    CHECK(usbc_relay_link(&late, 1, 7));
    usbc_relay_told(&late);
    CHECK(usbc_relay_queue(&late, read.data(), 12) == 0);
    usbc_relay_detach(&late);
    CHECK(!late.attached && late.dropped == 1);
    CHECK(usbc_relay_queue(&late, read.data(), 12) == -4);
    PianoUsbcStatus s;
    usbc_relay_status(&r, &s);
    CHECK(s.version == PIANO_USBC_STATUS_VERSION && s.attached && !s.ready && s.sent == 1 && s.dropped == 4);
}

static void testClassify()
{
    UsbcRelay r;
    usbc_relay_init(&r);
    const auto answer = msg(0x800b, 1, 0x11, 64), notify = msg(0x800b, 1, 0x13, 24), charger = msg(0x800a, 1, 1, 280);
    auto pan = msg(0x800c, 1, 0x16 | 0xff01u << 16, 32), panAck = msg(0x800c, 1, 0x15, 16);
    pan[12] = 0;
    pan[13] = 1;   // reverse
    CHECK(usbc_relay_classify(&r, answer.data(), 64) == USBC_MINE_NONE);   // no client: all PianoMca's
    CHECK(usbc_relay_classify(&r, pan.data(), 32) == USBC_MINE_NONE);
    usbc_relay_attach(&r, &kClient, nullptr);
    CHECK(usbc_relay_classify(&r, answer.data(), 64) == USBC_MINE_NONE);   // not ready: PianoMca's own UCSI read may be in flight
    CHECK(usbc_relay_classify(&r, pan.data(), 32) == USBC_MINE_NONE);      // port notifications are never the relay's
    CHECK(usbc_relay_classify(&r, panAck.data(), 16) == USBC_MINE_NONE);   // nor the answers to PianoMca's port writes
    usbc_relay_link(&r, 1, 1);
    CHECK(usbc_relay_classify(&r, answer.data(), 64) == USBC_MINE_NONE);   // not told yet
    usbc_relay_told(&r);
    CHECK(usbc_relay_classify(&r, answer.data(), 64) == USBC_MINE_UCSI);
    CHECK(usbc_relay_classify(&r, notify.data(), 24) == USBC_MINE_UCSI);
    CHECK(usbc_relay_classify(&r, charger.data(), 280) == USBC_MINE_NONE);
    CHECK(usbc_relay_classify(&r, pan.data(), 32) == USBC_MINE_NONE);
    CHECK(usbc_relay_classify(&r, answer.data(), 8) == USBC_MINE_NONE);
    r.closing = 1;
    CHECK(usbc_relay_classify(&r, answer.data(), 64) == USBC_MINE_NONE);
}

// PianoPower's receive loop: the relay first; a message it keeps never reaches PianoMca, a port notification does (acknowledged).
static void testPowerOrder()
{
    UsbcRelay r;
    usbc_relay_init(&r);
    PianoMca m;
    piano_mca_init(&m, 0, 0);
    piano_mca_allow_ucsi(&m, 1);
    piano_mca_link(&m, 1);
    uint8_t out[PIANO_MCA_REQUEST_BYTES];
    CHECK(piano_mca_ucsi_next(&m, out) == PIANO_MCA_UCSI_REQUEST_BYTES);   // PianoMca's one UCSI read, in flight (ucsi == 2)
    usbc_relay_attach(&r, &kClient, nullptr);
    const auto feed = [&](const std::vector<uint8_t> &x) {
        if (usbc_relay_classify(&r, x.data(), uint32_t(x.size())) == USBC_MINE_UCSI) return -1;
        return piano_mca_receive(&m, x.data(), uint32_t(x.size()));
    };
    // The owner's ready waits for PianoMca's read: the answer is PianoMca's.
    CHECK(!(m.ucsi != 1 && m.ucsi != 2));
    auto answer = msg(0x800b, 1, 0x11, 64);
    answer[12] = 0x10;
    answer[13] = 0x01;   // version 1.10
    CHECK(feed(answer) == McaUcsiDone && m.ucsi == 3 && m.ucsi_version == 0x110 && m.ucsi_bytes == 48);
    // Now ready and told: the client's traffic stays the client's (PianoMca's counters do not move).
    CHECK(usbc_relay_link(&r, m.ucsi != 1 && m.ucsi != 2, m.links));
    usbc_relay_told(&r);
    const uint32_t ignored = m.ignored, notes = m.ucsi_notifies;
    CHECK(feed(msg(0x800b, 1, 0x12, 16)) == -1);
    CHECK(feed(msg(0x800b, 1, 0x13, 24)) == -1);
    CHECK(feed(answer) == -1);
    CHECK(m.ignored == ignored && m.ucsi_notifies == notes);
    // A port notification: PianoMca's, acknowledged (PortEvents.c hands it to the watchers after that).
    auto pan = msg(0x800c, 1, 0x16, 32);
    CHECK(feed(pan) == McaPanNotified && (m.pan_pending & 1));
}

int main()
{
    testRequests();
    testQueueAndLink();
    testClassify();
    testPowerOrder();
    if (failures) {
        std::printf("usbc_host: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("usbc_host: all USB-C relay tests passed\n");
    return 0;
}
