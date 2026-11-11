// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the DSP client contract's decisions (PianoAdspContract.h): the QRTR client port table against the router codec fed with
// the remote name service's packets (QrtrPorts.c, QcomQrtr.c), the health pieces and the quiet flag (AdspHealth.h), the charger
// codec's "configured" (PianoMca.c) the quiet waits for, and GLINK answering a channel the DSP opened before a client registered it.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
extern "C" {
#include "QcomGlink.h"
#include "QcomQrtr.h"
#include "QrtrPorts.h"
#include "AdspHealth.h"
#include "PianoMca.h"
}

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static void put32(uint8_t *p, uint32_t v) {p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);}
static uint32_t le32(const uint8_t *p) {return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;}

static std::vector<std::vector<uint8_t>> sent;
static int capture(void *, const uint8_t *packet, uint32_t length) {sent.emplace_back(packet, packet + length); return 0;}

// A packet from the DSP (node 5): a control packet (cmd + 4 words) or data.
static std::vector<uint8_t> packet(uint32_t type, uint32_t srcPort, uint32_t dstPort, const uint8_t *data, uint32_t length)
{
    std::vector<uint8_t> p(32 + ((length + 3) & ~3u));
    put32(&p[0], 1); put32(&p[4], type); put32(&p[8], 5); put32(&p[12], srcPort); put32(&p[16], 0); put32(&p[20], length);
    put32(&p[24], 1); put32(&p[28], dstPort);
    if (length) std::memcpy(&p[32], data, length);
    return p;
}
static void control(QcomQrtr &q, uint32_t cmd, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    uint8_t c[20];
    put32(c, cmd); put32(c + 4, service); put32(c + 8, instance); put32(c + 12, node); put32(c + 16, port);
    const auto p = packet(cmd, QCOM_QRTR_PORT_CTRL, QCOM_QRTR_PORT_CTRL, c, sizeof c);
    CHECK(!qcom_qrtr_receive(&q, p.data(), uint32_t(p.size())));
}

static unsigned rxCount;
static uint32_t rxPort, rxLength;
static void rx(void *, QcomQrtr *, uint32_t port, uint32_t, uint32_t, const uint8_t *, uint32_t length) {rxCount++; rxPort = port; rxLength = length;}

static void dummyServer(void *, int, uint32_t, uint32_t, uint32_t) {}
static void dummyReceive(void *, uint32_t, uint32_t, const uint8_t *, uint32_t) {}

static void testPorts()
{
    QrtrPorts t;
    qrtr_ports_init(&t);
    const PianoQrtrClient client = {dummyServer, dummyReceive}, none = {nullptr, nullptr};
    CHECK(qrtr_ports_open(&t, 400, PIANO_QRTR_ANY, nullptr, nullptr) == -2 && qrtr_ports_open(&t, 400, 0, &none, nullptr) == -2);
    int slots[PIANO_QRTR_MAX_PORTS];
    for (unsigned i = 0; i < PIANO_QRTR_MAX_PORTS; ++i) {
        slots[i] = qrtr_ports_open(&t, 400, PIANO_QRTR_ANY, &client, nullptr);
        CHECK(slots[i] == int(i));
    }
    CHECK(qrtr_ports_open(&t, 400, PIANO_QRTR_ANY, &client, nullptr) == -1);   // eight at most
    const uint32_t n0 = t.port[0].number;
    CHECK(n0 >= PIANO_QRTR_FIRST_PORT && qrtr_ports_find(&t, n0) == 0 && qrtr_ports_find(&t, 1) == -1 && qrtr_ports_find(&t, 2) == -1);
    std::vector<uint32_t> numbers;
    for (unsigned i = 0; i < PIANO_QRTR_MAX_PORTS; ++i) numbers.push_back(t.port[i].number);
    for (unsigned i = 0; i < numbers.size(); ++i)
        for (unsigned j = i + 1; j < numbers.size(); ++j) CHECK(numbers[i] != numbers[j]);
    // A reopened slot gets another number (a late answer to the old port never reaches the new owner).
    qrtr_ports_free(&t, 0);
    CHECK(qrtr_ports_find(&t, n0) == -1);
    CHECK(qrtr_ports_open(&t, 0x42, 0x4a01, &client, nullptr) == 0 && t.port[0].number != n0 && qrtr_ports_find(&t, t.port[0].number) == 0);
    for (unsigned i = 1; i < PIANO_QRTR_MAX_PORTS; ++i) qrtr_ports_free(&t, int(i));
    qrtr_ports_free(&t, 99);   // out of range: nothing
    CHECK(t.opened == 9 && t.closed == 8);

    // Matching: the instance as announced, or any.
    QrtrPort p = {};
    p.service = 400; p.instance = PIANO_QRTR_ANY;
    CHECK(qrtr_ports_matches(&p, 400, 0x101) && !qrtr_ports_matches(&p, 401, 0x101));
    p.instance = 0x101;
    CHECK(qrtr_ports_matches(&p, 400, 0x101) && !qrtr_ports_matches(&p, 400, 0x201));
}

static void testServerEvents()
{
    QcomQrtr q;
    qcom_qrtr_init(&q, 1, capture, nullptr);
    QrtrPorts t;
    qrtr_ports_init(&t);
    const PianoQrtrClient client = {dummyServer, dummyReceive};
    const int a = qrtr_ports_open(&t, 400, PIANO_QRTR_ANY, &client, nullptr);   // the sensor client service
    const int b = qrtr_ports_open(&t, 0x42, 0x4a01, &client, nullptr);          // the ADSP's servreg notifier, exactly
    QrtrPortEvent e[2 * PIANO_QRTR_MAX_PORTS];
    CHECK(qrtr_ports_match(&t, &q, e, 16) == 0);   // nothing announced: nothing to say
    // The DSP's hello and its servers.
    uint8_t hello[20] = {};
    put32(hello, QCOM_QRTR_HELLO);
    auto h = packet(QCOM_QRTR_HELLO, QCOM_QRTR_PORT_CTRL, QCOM_QRTR_PORT_CTRL, hello, sizeof hello);
    CHECK(!qcom_qrtr_receive(&q, h.data(), uint32_t(h.size())) && q.hello_received);
    control(q, QCOM_QRTR_NEW_SERVER, 0x42, 0x4c01, 5, 30);   // the CDSP's notifier: not ours
    control(q, QCOM_QRTR_NEW_SERVER, 0x42, 0x4a01, 5, 31);
    control(q, QCOM_QRTR_NEW_SERVER, 400, 0x101, 5, 40);
    unsigned n = qrtr_ports_match(&t, &q, e, 16);
    CHECK(n == 2);
    CHECK(e[0].slot == a && e[0].up == 1 && e[0].node == 5 && e[0].port == 40 && e[0].instance == 0x101 && e[0].generation == 1);
    CHECK(e[1].slot == b && e[1].up == 1 && e[1].port == 31 && e[1].instance == 0x4a01);
    CHECK(t.port[a].server.up && t.port[a].server.port == 40 && t.port[b].server.port == 31);
    CHECK(qrtr_ports_match(&t, &q, e, 16) == 0);   // nothing changed
    // A second sensor server elsewhere: the port keeps the one it has.
    control(q, QCOM_QRTR_NEW_SERVER, 400, 0x101, 5, 41);
    CHECK(qrtr_ports_match(&t, &q, e, 16) == 0 && t.port[a].server.port == 40);
    // Its server goes: the port moves to the other one (gone first, then appeared).
    control(q, QCOM_QRTR_DEL_SERVER, 400, 0x101, 5, 40);
    n = qrtr_ports_match(&t, &q, e, 16);
    CHECK(n == 2 && e[0].up == 0 && e[0].port == 40 && e[1].up == 1 && e[1].port == 41 && e[1].generation == 3);
    // The link goes (BYE): every server gone.
    uint8_t bye[20] = {};
    put32(bye, QCOM_QRTR_BYE);
    auto by = packet(QCOM_QRTR_BYE, QCOM_QRTR_PORT_CTRL, QCOM_QRTR_PORT_CTRL, bye, sizeof bye);
    CHECK(!qcom_qrtr_receive(&q, by.data(), uint32_t(by.size())));
    n = qrtr_ports_match(&t, &q, e, 16);
    CHECK(n == 2 && !e[0].up && !e[1].up && !t.port[a].server.up && !t.port[b].server.up);
    // No router at all (q null): nothing up, nothing more to say.
    CHECK(qrtr_ports_match(&t, nullptr, e, 16) == 0);
    // A closing port says nothing.
    control(q, QCOM_QRTR_NEW_SERVER, 400, 0x101, 5, 42);
    t.port[a].closing = 1;
    n = qrtr_ports_match(&t, &q, e, 16);
    CHECK(n == 0);
    // The capacity is honoured.
    t.port[a].closing = 0;
    CHECK(qrtr_ports_match(&t, &q, e, 0) == 0 && t.port[a].server.up);   // the state moves even when no event fits
}

static void testRouterPorts()
{
    QcomQrtr q;
    qcom_qrtr_init(&q, 1, capture, nullptr);
    // PianoPower's two ports and eight client ports fit; one more does not.
    CHECK(!qcom_qrtr_add_service(&q, QCOM_SERVLOC_SERVICE, QCOM_SERVLOC_INSTANCE, 1, rx, nullptr));
    CHECK(!qcom_qrtr_add_client(&q, 2, rx, nullptr));
    for (uint32_t i = 0; i < PIANO_QRTR_MAX_PORTS; ++i) CHECK(!qcom_qrtr_add_client(&q, PIANO_QRTR_FIRST_PORT + i, rx, nullptr));
    CHECK(!qcom_qrtr_add_client(&q, 100, rx, nullptr) && !qcom_qrtr_add_client(&q, 101, rx, nullptr));
    CHECK(qcom_qrtr_add_client(&q, 102, rx, nullptr) == -1);   // QCOM_QRTR_MAX_LOCAL is 12
    CHECK(!qcom_qrtr_remove_port(&q, 100) && !qcom_qrtr_remove_port(&q, 101) && qcom_qrtr_remove_port(&q, 100) == -1);
    // Data for a client port is routed to it; after the port is removed it is not.
    const uint8_t d[1200] = {7};
    auto p = packet(QCOM_QRTR_DATA, 40, PIANO_QRTR_FIRST_PORT + 3, d, 1200);   // a large report (the 2048-byte intents)
    rxCount = 0;
    CHECK(!qcom_qrtr_receive(&q, p.data(), uint32_t(p.size())) && rxCount == 1 && rxPort == PIANO_QRTR_FIRST_PORT + 3 && rxLength == 1200);
    CHECK(!qcom_qrtr_remove_port(&q, PIANO_QRTR_FIRST_PORT + 3));
    CHECK(!qcom_qrtr_receive(&q, p.data(), uint32_t(p.size())) && rxCount == 1 && q.rx_unrouted == 1);
    // DEL_CLIENT {our node, port} to the remote's control port.
    sent.clear();
    CHECK(!qcom_qrtr_del_client(&q, PIANO_QRTR_FIRST_PORT + 3) && sent.size() == 1);
    CHECK(le32(&sent[0][4]) == QCOM_QRTR_DEL_CLIENT && le32(&sent[0][28]) == QCOM_QRTR_PORT_CTRL);
    CHECK(le32(&sent[0][32]) == QCOM_QRTR_DEL_CLIENT && le32(&sent[0][36]) == 1 && le32(&sent[0][40]) == PIANO_QRTR_FIRST_PORT + 3);
    // A client's send of PIANO_QRTR_MAX_DATA bytes fits the router's packet; one more does not.
    sent.clear();
    static uint8_t big[PIANO_QRTR_MAX_DATA + 1];
    CHECK(!qcom_qrtr_send_data(&q, PIANO_QRTR_FIRST_PORT, 5, 40, big, PIANO_QRTR_MAX_DATA) && sent.size() == 1);
    CHECK(sent[0].size() == 32 + PIANO_QRTR_MAX_DATA && sent[0].size() <= PIANO_ADSP_MAX_MESSAGE);
    CHECK(qcom_qrtr_send_data(&q, PIANO_QRTR_FIRST_PORT, 5, 40, big, PIANO_QRTR_MAX_DATA + 1) == -1);
}

static void testHealth()
{
    CHECK(adsp_health_dsp_up(20, ADSP_SLAVE_READY | 4, 1, 1));
    CHECK(!adsp_health_dsp_up(17, ADSP_SLAVE_READY, 1, 1));                     // not past the ready stage
    CHECK(!adsp_health_dsp_up(-17, ADSP_SLAVE_READY, 1, 1));                    // failed
    CHECK(!adsp_health_dsp_up(20, ADSP_SLAVE_READY | ADSP_SLAVE_FATAL, 1, 1));   // fatal
    CHECK(!adsp_health_dsp_up(20, 0, 1, 1) && !adsp_health_dsp_up(20, ADSP_SLAVE_READY, 0, 1) && !adsp_health_dsp_up(20, ADSP_SLAVE_READY, 1, 0));
    // Six channel slots: four for clients, whatever registers first; PianoPower's two always fit.
    CHECK(adsp_channel_room(0, 0, 0, PIANO_ADSP_CHANNELS) && adsp_channel_room(3, 3, 0, PIANO_ADSP_CHANNELS));
    CHECK(!adsp_channel_room(4, 4, 0, PIANO_ADSP_CHANNELS) && adsp_channel_room(4, 4, 1, PIANO_ADSP_CHANNELS));
    CHECK(adsp_channel_room(5, 4, 1, PIANO_ADSP_CHANNELS) && !adsp_channel_room(6, 4, 1, PIANO_ADSP_CHANNELS));
    CHECK(adsp_channel_room(5, 3, 0, PIANO_ADSP_CHANNELS) && !adsp_channel_room(6, 4, 0, PIANO_ADSP_CHANNELS));
    AdspLinkTrack l = {};
    adsp_link_channel(&l, 0, 1);
    adsp_link_channel(&l, 1, 1);
    CHECK(l.generation == 0 && l.up_mask == 3);   // coming up is not a new generation
    adsp_link_channel(&l, 1, 0);
    CHECK(l.generation == 1);
    adsp_link_channel(&l, 1, 0);                  // down twice: once
    adsp_link_channel(&l, 3, 0);                  // never up: nothing
    CHECK(l.generation == 1);
    adsp_link_broken(&l, 1);
    adsp_link_broken(&l, 1);
    CHECK(l.generation == 2);
    adsp_link_broken(&l, 0);
    CHECK(l.generation == 2 && !l.broken);
}

static void testQuiet()
{
    PowerQuiet q = {};
    CHECK(power_quiet_step(&q, 0, 0, 0, 1000, 60000) == PowerQuietNone && !q.raised);   // no charger yet: no quiet
    CHECK(power_quiet_step(&q, 1, 1, 0, 2000, 60000) == PowerQuietRaised && q.raised);  // the first generation links
    CHECK(power_quiet_step(&q, 1, 1, 0, 5000, 60000) == PowerQuietNone && q.raised);
    CHECK(power_quiet_step(&q, 1, 1, 1, 6000, 60000) == PowerQuietReleased && !q.raised && q.releases == 1);
    CHECK(power_quiet_step(&q, 1, 1, 0, 7000, 60000) == PowerQuietNone && !q.raised);   // later writes (an attach): not again
    // A new generation: raised again; never configured: released after the cap.
    CHECK(power_quiet_step(&q, 1, 2, 0, 10000, 60000) == PowerQuietRaised && q.raises == 2);
    CHECK(power_quiet_step(&q, 1, 2, 0, 69999, 60000) == PowerQuietNone && q.raised);
    CHECK(power_quiet_step(&q, 1, 2, 0, 70000, 60000) == PowerQuietTimedOut && !q.raised && q.timeouts == 1);
    // Unlinked while raised: dropped (nothing to configure); the next link raises it again.
    CHECK(power_quiet_step(&q, 1, 3, 0, 80000, 60000) == PowerQuietRaised);
    CHECK(power_quiet_step(&q, 0, 3, 0, 81000, 60000) == PowerQuietDropped && !q.raised);
    CHECK(power_quiet_step(&q, 1, 4, 0, 82000, 60000) == PowerQuietRaised);
}

// The charger codec: configured once a whole cycle ends with nothing to write and the port enable answered or timed out. The model
// answers reads with a DCP at 5 V and reads back what was written (as the firmware does for the input limit).
static void answerAll(PianoMca &m, std::map<uint32_t, uint32_t> &props)
{
    uint8_t out[PIANO_MCA_REQUEST_BYTES], resp[PIANO_MCA_RESPONSE_BYTES];
    for (unsigned guard = 0; guard < 400; ++guard) {
        const uint32_t len = piano_mca_next(&m, out);
        if (!len) break;
        std::memset(resp, 0, sizeof resp);
        std::memcpy(resp, out, 16);
        put32(resp + 16, 0);
        put32(resp + 20, le32(out + 16));
        const uint32_t prop = le32(out + 12);
        if (le32(out + 8) == PIANO_MCA_OP_READ) put32(resp + 24, props.count(prop) ? props[prop] : 0);
        else props[prop] = le32(out + 20);
        piano_mca_receive(&m, resp, sizeof resp);
    }
}

static void testConfigured()
{
    // Read only (piano-power-write=0): the first cycle and the enable's answer.
    std::map<uint32_t, uint32_t> props = {{PIANO_MCA_BUS_VOLTAGE, 5000000}, {PIANO_MCA_REAL_TYPE, PIANO_MCA_TYPE_DCP},
                                          {PIANO_MCA_TBAT, 25}, {PIANO_MCA_CHARGE_ENABLE, 1}, {PIANO_MCA_ICL, 100}};
    PianoMca r;
    piano_mca_init(&r, 0, 0);
    piano_mca_link(&r, 1);
    CHECK(!piano_mca_configured(&r));
    answerAll(r, props);
    CHECK(!piano_mca_configured(&r));   // the port enable not answered yet
    uint8_t pan[PIANO_MCA_PAN_REQUEST_BYTES];
    CHECK(piano_mca_pan_next(&r, pan));
    piano_mca_pan_timeout(&r);
    CHECK(piano_mca_configured(&r));
    piano_mca_link(&r, 0);
    CHECK(!piano_mca_configured(&r));
    // With writes and the board configuration: only once the board and the policy's writes are through and a cycle ended clean.
    PianoMca w;
    piano_mca_init(&w, 1, 0);
    piano_mca_allow_board(&w, 1);
    piano_mca_link(&w, 1);
    CHECK(piano_mca_pan_next(&w, pan));
    uint8_t reply[16] = {};
    put32(reply, PIANO_MCA_OWNER_PAN); put32(reply + 4, PIANO_MCA_TYPE_REQUEST); put32(reply + 8, PIANO_MCA_PAN_WRITE);
    CHECK(piano_mca_receive(&w, reply, sizeof reply) == McaPanDone);
    CHECK(!piano_mca_configured(&w));
    answerAll(w, props);   // the reads, the board configuration, the reads again, the limits, a clean cycle
    CHECK(piano_mca_configured(&w) && w.board_done == 7 && w.icl_set == 1500 && w.writes_ok == 5);   // CP, OTG, CID, ICL, FCC
    // A later cycle with new writes (a detach) does not make it unconfigured for the quiet's purpose: the quiet is per generation.
    piano_mca_link(&w, 0);
    CHECK(!piano_mca_configured(&w));
}

// ---- GLINK: the DSP opens a channel before any client registered it (a client kext that starts late) ----
struct Fifos {
    volatile uint32_t txHead = 0, txTail = 0, rxHead = 0, rxTail = 0;
    uint8_t tx[4096] = {}, rx[4096] = {};
    // The remote writes one message (8-byte header + payload, padded) into our receive FIFO.
    void remote(uint16_t cmd, uint16_t p1, uint32_t p2, const void *payload = nullptr, uint32_t n = 0)
    {
        uint8_t m[8] = {uint8_t(cmd), uint8_t(cmd >> 8), uint8_t(p1), uint8_t(p1 >> 8)};
        put32(m + 4, p2);
        uint32_t h = rxHead;
        for (uint32_t i = 0; i < 8; ++i) rx[h++ % sizeof rx] = m[i];
        for (uint32_t i = 0; i < n; ++i) rx[h++ % sizeof rx] = static_cast<const uint8_t *>(payload)[i];
        rxHead = ((h + 7) & ~7u) % sizeof rx;
    }
    // What we wrote since the last call: {cmd, param1} per 8-byte-aligned message (OPEN's name skipped).
    std::vector<std::pair<uint16_t, uint16_t>> ours()
    {
        std::vector<std::pair<uint16_t, uint16_t>> v;
        while (txTail != txHead) {
            const uint32_t t = txTail;
            const uint16_t cmd = uint16_t(tx[t] | tx[t + 1] << 8), p1 = uint16_t(tx[t + 2] | tx[t + 3] << 8);
            const uint32_t p2 = le32(&tx[t + 4]);
            v.emplace_back(cmd, p1);
            uint32_t size = 8;
            if (cmd == QCOM_GLINK_OPEN) size = (8 + p2 + 7) & ~7u;
            if (cmd == QCOM_GLINK_INTENT) size = 8 + 8 * p2;
            txTail = (t + size) % sizeof tx;
        }
        return v;
    }
};

static unsigned glinkUps;
static void glinkState(void *, QcomGlinkChannel *, int up) {if (up) glinkUps++;}
static void glinkReceive(void *, QcomGlinkChannel *, const uint8_t *, uint32_t) {}

static bool has(const std::vector<std::pair<uint16_t, uint16_t>> &v, uint16_t cmd, uint16_t p1)
{
    for (const auto &m : v) if (m.first == cmd && m.second == p1) return true;
    return false;
}

static void testLateChannel()
{
    static Fifos f;
    static QcomGlink g;
    const QcomGlinkFifo tx = {&f.txHead, &f.txTail, f.tx, sizeof f.tx}, rx = {&f.rxHead, &f.rxTail, f.rx, sizeof f.rx};
    qcom_glink_init(&g, tx, rx, nullptr, nullptr);
    f.remote(QCOM_GLINK_VERSION, 1, QCOM_GLINK_FEATURE_INTENT_REUSE);
    CHECK(qcom_glink_poll(&g) >= 1 && g.version_agreed);
    f.ours();
    // The DSP opens two channels nobody serves yet; it withdraws one of them.
    const char a[] = "fastrpcglink-apps-dsp", b[] = "adsp_apps";
    f.remote(QCOM_GLINK_OPEN, 7, sizeof a, a, sizeof a);
    f.remote(QCOM_GLINK_OPEN, 8, sizeof b, b, sizeof b);
    CHECK(qcom_glink_poll(&g) == 2 && g.unknown_opens == 2 && g.pending[0].used && g.pending[1].used);
    CHECK(f.ours().empty());   // nothing answered: nobody owns them
    f.remote(QCOM_GLINK_CLOSE, 8, 0);
    qcom_glink_poll(&g);
    CHECK(!g.pending[1].used && has(f.ours(), QCOM_GLINK_CLOSE_ACK, 8));
    // A client registers the first one later: answered (OPEN_ACK to the DSP's id, then our OPEN), up once the DSP acks ours.
    static uint8_t storage[2 * 512];
    const QcomGlinkClient client = {glinkState, glinkReceive};
    QcomGlinkChannel *c = qcom_glink_channel(&g, a, 512, 2, storage, &client, nullptr, 0);
    CHECK(c && c->remote_open && c->ack_due && c->rcid == 7 && g.late_opens == 1 && !g.pending[0].used);
    qcom_glink_poll(&g);
    const auto sent1 = f.ours();
    CHECK(has(sent1, QCOM_GLINK_OPEN_ACK, 7) && has(sent1, QCOM_GLINK_OPEN, c->lcid) && !c->ack_due);
    f.remote(QCOM_GLINK_OPEN_ACK, c->lcid, 0);
    qcom_glink_poll(&g);
    CHECK(c->up && glinkUps == 1 && has(f.ours(), QCOM_GLINK_INTENT, c->lcid));
    // A channel registered with no open pending waits for the DSP's (no OPEN_ACK of its own).
    QcomGlinkChannel *d = qcom_glink_channel(&g, "LOOPBACK_CTL_LPASS", 512, 0, nullptr, &client, nullptr, 0);
    qcom_glink_poll(&g);
    CHECK(d && !d->remote_open && !d->ack_due && f.ours().empty());
    // More pending opens than slots: the oldest goes.
    for (uint16_t i = 0; i < QCOM_GLINK_MAX_PENDING_OPENS + 1; ++i) {
        const std::string name = "ch" + std::to_string(i);
        f.remote(QCOM_GLINK_OPEN, uint16_t(20 + i), uint32_t(name.size() + 1), name.c_str(), uint32_t(name.size() + 1));
    }
    qcom_glink_poll(&g);
    bool oldest = false, newest = false;
    for (unsigned k = 0; k < QCOM_GLINK_MAX_PENDING_OPENS; ++k) {
        oldest |= g.pending[k].used && g.pending[k].rcid == 20;
        newest |= g.pending[k].used && g.pending[k].rcid == 20 + QCOM_GLINK_MAX_PENDING_OPENS;
    }
    CHECK(!oldest && newest);
}

int main()
{
    testLateChannel();
    testPorts();
    testServerEvents();
    testRouterPorts();
    testHealth();
    testQuiet();
    testConfigured();
    if (failures) {std::printf("ports_host: %d failure(s)\n", failures); return 1;}
    std::printf("ports_host: all passed\n");
    return 0;
}
