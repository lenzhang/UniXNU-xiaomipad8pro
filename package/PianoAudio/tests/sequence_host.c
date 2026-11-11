// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the DSP exchange rule (AudioSequence.c) against a model of the DSP: piano's speaker plan comes up in the order the
// design names, stops in exactly the reverse order, and on an error at any step (a refused command, a missing answer, a failed
// amplifier start, the owner's abort, the DSP going down) undoes exactly what was done, the amplifiers first; the period pump.
//   cc -std=c11 -Wall -Wextra -Werror sequence_host.c ../AudioSequence.c ../PianoAudioReach.c ../PianoGpr.c && ./a.out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioSequence.h"
#include "../PianoGpr.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

// ---- what happened, one line per command or local action
static char lg[64][40];
static unsigned lines;
static void note(const char *s) {if (lines < 64) snprintf(lg[lines++], sizeof lg[0], "%s", s);}

struct Owner {int amps_on_fails, amps_off_calls, amps_on_calls;};
static int amps_on(void *o) {struct Owner *w = o; w->amps_on_calls++; note("AMPS ON"); return w->amps_on_fails;}
static int amps_off(void *o) {struct Owner *w = o; w->amps_off_calls++; note("AMPS OFF"); return 0;}

// A packet as one line: the opcode's name and what it names (subgraph, module, clock, handle).
static void describe(const uint8_t *b, uint32_t n)
{
    GprPacket p;
    CHECK(gpr_decode(b, n, &p) == GprOk && p.size == n && n <= GPR_MAX_PACKET);
    char s[40];
    switch (p.opcode) {
    case AR_OP_PRM_REQUEST: snprintf(s, sizeof s, "PRM REQ %#x %u", le32(p.payload + 36), le32(p.payload + 40)); break;
    case AR_OP_PRM_RELEASE: snprintf(s, sizeof s, "PRM REL %#x", le32(p.payload + 36)); break;
    case AR_OP_GRAPH_OPEN: snprintf(s, sizeof s, "OPEN %#x", le32(p.payload + 36)); break;
    case AR_OP_GRAPH_PREPARE: snprintf(s, sizeof s, "PREPARE %#x", le32(p.payload + 36)); break;
    case AR_OP_GRAPH_START: snprintf(s, sizeof s, "START %#x", le32(p.payload + 36)); break;
    case AR_OP_GRAPH_STOP: snprintf(s, sizeof s, "STOP %#x", le32(p.payload + 36)); break;
    case AR_OP_GRAPH_CLOSE: snprintf(s, sizeof s, "CLOSE %#x", le32(p.payload + 36)); break;
    case AR_OP_SET_CFG: snprintf(s, sizeof s, "CFG %#x", le32(p.payload + 16)); break;
    case AR_OP_MEM_MAP: snprintf(s, sizeof s, "MAP %#x%08x", le32(p.payload + 12), le32(p.payload + 8)); break;
    case AR_OP_MEM_UNMAP: snprintf(s, sizeof s, "UNMAP %#x", le32(p.payload)); break;
    default: snprintf(s, sizeof s, "OP %#x", p.opcode);
    }
    note(s);
}

// ---- the DSP model: answers every command with success (MEM_MAP: handle 0x77), except where told otherwise.
struct Dsp {
    int fail_at, timeout_at, abort_at, down_at;   // the command (by count) to refuse / leave unanswered / abort at; -1 none
    uint32_t fail_status;
    int no_handle;                                // MEM_MAP answered with a plain success (no handle)
    unsigned commands;
};
static uint32_t answer(struct Dsp *d, const uint8_t *q, uint32_t n, uint8_t *out, uint32_t status)
{
    GprPacket p, r;
    CHECK(gpr_decode(q, n, &p) == GprOk);
    memset(&r, 0, sizeof r);
    r.dst_domain = p.src_domain; r.src_domain = p.dst_domain; r.src_port = p.dst_port; r.dst_port = p.src_port; r.token = p.token;
    uint32_t w[2] = {p.opcode, status};
    if (p.opcode == AR_OP_MEM_MAP && !status && !d->no_handle) {r.opcode = AR_OP_MEM_MAP_DONE; w[0] = 0x77; return gpr_encode(out, GPR_MAX_PACKET, &r, w, 4);}
    if (p.opcode == AR_OP_PRM_REQUEST) r.opcode = AR_OP_PRM_REQUEST_DONE;
    else if (p.opcode == AR_OP_PRM_RELEASE) r.opcode = AR_OP_PRM_RELEASE_DONE;
    else r.opcode = GPR_OP_BASIC_RESULT;
    return gpr_encode(out, GPR_MAX_PACKET, &r, w, 8);
}

// Runs the sequence until it waits for nothing more (up, down or failed).
static void drive(ArSeq *s, struct Dsp *d)
{
    uint8_t pkt[GPR_MAX_PACKET], rsp[GPR_MAX_PACKET];
    for (unsigned guard = 0; guard < 100; ++guard) {
        const uint32_t n = ar_seq_next(s, pkt, sizeof pkt);
        if (!n) return;
        describe(pkt, n);
        const int k = (int)d->commands++;
        if (k == d->timeout_at) {ar_seq_timeout(s); continue;}
        if (k == d->abort_at) {ar_seq_abort(s, ArSeqErrAbort, 0); answer(d, pkt, n, rsp, 0); continue;}
        if (k == d->down_at) {ar_seq_abort(s, ArSeqErrDspDown, 1); continue;}
        const uint32_t m = answer(d, pkt, n, rsp, k == d->fail_at ? d->fail_status : 0);
        CHECK(ar_seq_receive(s, rsp, m) != ArSeqIgnored);
    }
    CHECK(0);   // never more than 100 packets
}

static const char *kUp[] = {"PRM REQ 0x202 6144000", "OPEN 0x4005", "CFG 0x6050", "CFG 0x6051", "CFG 0x6052", "OPEN 0x4001",
                            "CFG 0x6001", "CFG 0x6002", "CFG 0x6004", "MAP 0x19f000000", "PREPARE 0x4005", "PREPARE 0x4001",
                            "START 0x4005", "START 0x4001", "AMPS ON"};
// What undoes step i (by the step list), or null.
static const char *kUndo[] = {"PRM REL 0x202", "CLOSE 0x4005", 0, 0, 0, "CLOSE 0x4001", 0, 0, 0, "UNMAP 0x77", 0, 0, "STOP 0x4005",
                              "STOP 0x4001", "AMPS OFF"};
enum {kSteps = sizeof kUp / sizeof kUp[0]};

static ArSpeakerPlan plan;
static struct Owner owner;
static void setup(ArSeq *s)
{
    ar_speaker_defaults(&plan);
    plan.buffer_phys = 0x9f000000ull; plan.buffer_bytes = 4 * 7680; plan.owner = &owner;
    plan.amps_on = amps_on; plan.amps_off = amps_off;
    memset(&owner, 0, sizeof owner);
    unsigned count;
    const ArStep *steps = ar_speaker_steps(&count);
    CHECK(count == kSteps);
    ar_seq_init(s, steps, count, AR_PORT_APM, AR_PORT_PRM, 0x100, &plan);
    lines = 0;
}
static struct Dsp healthy(void) {struct Dsp d = {-1, -1, -1, -1, 0, 0, 0}; return d;}

// The log from line `from` must be the undo of steps [0, upto) in reverse, then nothing.
static void expect_unwind(unsigned from, unsigned upto, int with_dsp)
{
    unsigned at = from;
    for (int i = (int)upto - 1; i >= 0; --i) {
        if (!kUndo[i] || (!with_dsp && strcmp(kUndo[i], "AMPS OFF"))) continue;
        CHECK(at < lines && !strcmp(lg[at], kUndo[i]));
        ++at;
    }
    CHECK(at == lines);
}

static void testUpAndStop(void)
{
    ArSeq s;
    setup(&s);
    struct Dsp d = healthy();
    drive(&s, &d);
    CHECK(s.state == ArSeqUp && s.error == ArSeqOk && s.handle == 0x77 && s.done == kSteps && s.failed_step == -1);
    CHECK(lines == kSteps);
    for (unsigned i = 0; i < kSteps; ++i) CHECK(!strcmp(lg[i], kUp[i]));
    CHECK(owner.amps_on_calls == 1 && owner.amps_off_calls == 0 && s.answers == kSteps - 1);
    uint8_t pkt[GPR_MAX_PACKET];
    CHECK(ar_seq_next(&s, pkt, sizeof pkt) == 0);   // up: nothing to send
    ar_seq_stop(&s);
    drive(&s, &d);
    CHECK(s.state == ArSeqDown && s.error == ArSeqOk && owner.amps_off_calls == 1 && s.undo_errors == 0);
    expect_unwind(kSteps, kSteps, 1);
    CHECK(!strcmp(lg[kSteps], "AMPS OFF") && !strcmp(lg[kSteps + 1], "STOP 0x4001") && !strcmp(lg[lines - 1], "PRM REL 0x202"));
    ar_seq_stop(&s);   // down: a no-op
    CHECK(s.state == ArSeqDown && ar_seq_next(&s, pkt, sizeof pkt) == 0);
}

// A refused command at each DSP step: what came before is undone, the refused step is not.
static void testErrorAtEveryStep(void)
{
    for (unsigned k = 0; k + 1 < kSteps; ++k) {   // the last step is the amplifiers' local action
        ArSeq s;
        setup(&s);
        struct Dsp d = healthy();
        d.fail_at = (int)k; d.fail_status = 0x0b;
        drive(&s, &d);
        CHECK(s.state == ArSeqFailed && s.error == ArSeqErrStatus && s.failed_step == (int)k && s.error_status == 0x0b);
        CHECK(owner.amps_on_calls == 0 && owner.amps_off_calls == 0);
        for (unsigned i = 0; i <= k; ++i) CHECK(!strcmp(lg[i], kUp[i]));
        expect_unwind(k + 1, k, 1);
    }
}

// No answer at each DSP step: that step may have run, so it is undone too.
static void testTimeoutAtEveryStep(void)
{
    for (unsigned k = 0; k + 1 < kSteps; ++k) {
        ArSeq s;
        setup(&s);
        struct Dsp d = healthy();
        d.timeout_at = (int)k;
        drive(&s, &d);
        CHECK(s.state == ArSeqFailed && s.error == ArSeqErrTimeout && s.failed_step == (int)k && s.timeouts == 1);
        // the map that got no answer has no handle: nothing to unmap
        unsigned at = k + 1;
        for (int i = (int)k; i >= 0; --i) {
            if (!kUndo[i] || !strcmp(kUndo[i], "AMPS OFF") || (!strcmp(kUndo[i], "UNMAP 0x77") && (unsigned)i == k)) continue;
            CHECK(at < lines && !strcmp(lg[at], kUndo[i]));
            ++at;
        }
        CHECK(at == lines);
    }
}

static void testAmplifiersFail(void)
{
    // amps_on fails (two of four may be up): they are shut down first, then everything else.
    ArSeq s;
    setup(&s);
    owner.amps_on_fails = -5;
    struct Dsp d = healthy();
    drive(&s, &d);
    CHECK(s.state == ArSeqFailed && s.error == ArSeqErrLocal && s.failed_step == kSteps - 1 && owner.amps_off_calls == 1);
    CHECK(!strcmp(lg[kSteps - 1], "AMPS ON") && !strcmp(lg[kSteps], "AMPS OFF"));
    expect_unwind(kSteps, kSteps, 1);
}

static void testAbort(void)
{
    // The owner's watchdog while up: everything undone, the result an error.
    ArSeq s;
    setup(&s);
    struct Dsp d = healthy();
    drive(&s, &d);
    ar_seq_abort(&s, ArSeqErrAbort, 0);
    drive(&s, &d);
    CHECK(s.state == ArSeqFailed && s.error == ArSeqErrAbort && s.failed_step == -1 && owner.amps_off_calls == 1);
    expect_unwind(kSteps, kSteps, 1);

    // The DSP goes down while up: no command is sent any more, the amplifiers still go down.
    setup(&s);
    d = healthy();
    drive(&s, &d);
    ar_seq_abort(&s, ArSeqErrDspDown, 1);
    drive(&s, &d);
    CHECK(s.state == ArSeqFailed && s.error == ArSeqErrDspDown && owner.amps_off_calls == 1 && lines == kSteps + 1);
    CHECK(!strcmp(lg[kSteps], "AMPS OFF"));

    // The DSP goes down with a command in flight (the device start): no undo command, no amplifiers were on.
    setup(&s);
    d = healthy();
    d.down_at = 12;
    drive(&s, &d);
    CHECK(s.state == ArSeqFailed && s.error == ArSeqErrDspDown && lines == 13 && owner.amps_off_calls == 0);

    // An abort with a forward command in flight: that command is undone too (stream start in flight -> STOP 0x4001 first).
    setup(&s);
    d = healthy();
    d.abort_at = 13;
    drive(&s, &d);
    CHECK(s.state == ArSeqFailed && s.error == ArSeqErrAbort && s.failed_step == 13 && !strcmp(lg[14], "STOP 0x4001"));
    expect_unwind(14, 14, 1);

    // The DSP goes down during the unwinding: what is left of it runs without commands, the amplifiers' undo already ran.
    setup(&s);
    d = healthy();
    drive(&s, &d);
    ar_seq_stop(&s);
    uint8_t pkt[GPR_MAX_PACKET];
    CHECK(ar_seq_next(&s, pkt, sizeof pkt) && owner.amps_off_calls == 1 && s.state == ArSeqWaiting);   // STOP 0x4001 in flight
    ar_seq_abort(&s, ArSeqErrDspDown, 1);
    CHECK(ar_seq_next(&s, pkt, sizeof pkt) == 0 && s.state == ArSeqFailed && s.error == ArSeqErrDspDown);
    ar_seq_abort(&s, ArSeqErrAbort, 0);   // failed: a no-op
    CHECK(s.state == ArSeqFailed && s.error == ArSeqErrDspDown);
}

static void testUndoErrorsContinue(void)
{
    // While stopping, a refused STOP and a missing CLOSE answer are counted; the unwinding goes on to the clock's release.
    ArSeq s;
    setup(&s);
    struct Dsp d = healthy();
    drive(&s, &d);
    d.fail_at = (int)d.commands; d.fail_status = 2;          // STOP 0x4001
    d.timeout_at = (int)d.commands + 3;                       // CLOSE 0x4001 (after STOP 0x4005, UNMAP)
    ar_seq_stop(&s);
    drive(&s, &d);
    CHECK(s.state == ArSeqDown && s.undo_errors == 2 && s.error == ArSeqOk);
    expect_unwind(kSteps, kSteps, 1);
}

static void testIgnored(void)
{
    ArSeq s;
    setup(&s);
    uint8_t pkt[GPR_MAX_PACKET], rsp[GPR_MAX_PACKET];
    struct Dsp d = healthy();
    uint32_t n = ar_seq_next(&s, pkt, sizeof pkt);   // PRM request in flight
    CHECK(n && s.state == ArSeqWaiting && s.pending_port == AR_PORT_PRM);
    uint32_t m = answer(&d, pkt, n, rsp, 0);
    rsp[16] ^= 1;   // another token
    CHECK(ar_seq_receive(&s, rsp, m) == ArSeqIgnored);
    rsp[16] ^= 1;
    rsp[12] = AR_PORT_APM;   // to the APM port, not the PRM port
    CHECK(ar_seq_receive(&s, rsp, m) == ArSeqIgnored);
    rsp[12] = AR_PORT_PRM;
    rsp[5] = GPR_DOMAIN_MODEM;   // not from the ADSP
    CHECK(ar_seq_receive(&s, rsp, m) == ArSeqIgnored);
    rsp[5] = GPR_DOMAIN_ADSP;
    CHECK(ar_seq_receive(&s, rsp, 10) == ArSeqIgnored && s.ignored == 4 && s.state == ArSeqWaiting);
    CHECK(ar_seq_next(&s, pkt, sizeof pkt) == 0);   // one in flight
    CHECK(ar_seq_receive(&s, rsp, m) == ArSeqAdvanced && s.done == 1);
    CHECK(ar_seq_receive(&s, rsp, m) == ArSeqIgnored);   // a repeat
    // a basic result naming another command is not this step's answer
    n = ar_seq_next(&s, pkt, sizeof pkt);   // OPEN 0x4005
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = 1, .dst_port = AR_PORT_APM,
                   .token = s.pending_token, .opcode = GPR_OP_BASIC_RESULT};
    uint32_t w[2] = {AR_OP_GRAPH_CLOSE, 0};
    m = gpr_encode(rsp, sizeof rsp, &r, w, 8);
    CHECK(ar_seq_receive(&s, rsp, m) == ArSeqIgnored && s.done == 1);
    ar_seq_timeout(&s);
    CHECK(s.state == ArSeqUnwinding);
}

static void testMapProblems(void)
{
    // a MEM_MAP "success" without a handle is an error, and nothing is unmapped
    ArSeq s;
    setup(&s);
    struct Dsp d = healthy();
    d.no_handle = 1;
    drive(&s, &d);
    CHECK(s.state == ArSeqFailed && s.error == ArSeqErrAnswer && s.failed_step == 9 && s.handle == 0);
    expect_unwind(10, 9, 1);
    // a buffer the DSP cannot address (XNU's memory, above 4 GiB) is never mapped: the build fails at that step
    setup(&s);
    plan.buffer_phys = 0x8c0000000ull;
    d = healthy();
    drive(&s, &d);
    CHECK(s.state == ArSeqFailed && s.error == ArSeqErrBuild && s.failed_step == 9 && lines == 9 + 3);
    expect_unwind(9, 9, 1);
    setup(&s);
    plan.buffer_phys = 0x9f000800ull;   // not 4 KiB aligned
    d = healthy();
    drive(&s, &d);
    CHECK(s.error == ArSeqErrBuild && s.failed_step == 9);
}

static void testPump(void)
{
    ArPump p;
    CHECK(ar_pump_init(&p, 0x19f000000ull, 0x77, AR_PORT_DATA, 1, 7680, 1, 50) == -1);    // one slot
    CHECK(ar_pump_init(&p, 0x19f000000ull, 0x77, AR_PORT_DATA, 17, 7680, 4, 50) == -1);   // too many
    CHECK(ar_pump_init(&p, 0x19f000000ull, 0x77, AR_PORT_DATA, 8, 7680, 9, 50) == -1);    // depth > slots
    CHECK(ar_pump_init(&p, 0x1fffff000ull, 0x77, AR_PORT_DATA, 8, 7680, 4, 50) == -1);    // past 4 GiB
    CHECK(ar_pump_init(&p, 0x19f000000ull, 0, AR_PORT_DATA, 8, 7680, 4, 50) == -1);       // no handle
    CHECK(ar_pump_init(&p, 0x19f000000ull, 0x77, AR_PORT_DATA, 8, 7680, 4, 50) == 0);     // 8 x 10 ms of 4 ch S16, 4 in flight
    uint8_t b[GPR_MAX_PACKET], rsp[GPR_MAX_PACKET];
    GprPacket q;
    for (int i = 0; i < 4; ++i) {
        CHECK(ar_pump_free_slot(&p) == i);
        const uint32_t n = ar_pump_write(&p, b, sizeof b, 100 + (uint32_t)i);
        CHECK(n && gpr_decode(b, n, &q) == GprOk && q.opcode == AR_OP_WRITE && le32(b + 24) == 0x9f000000u + 7680u * (uint32_t)i);
        CHECK(le32(b + 28) == 1 && le32(b + 32) == 0x77 && le32(b + 36) == 7680 && q.src_port == AR_PORT_DATA);
    }
    CHECK(ar_pump_free_slot(&p) == -1 && ar_pump_write(&p, b, sizeof b, 105) == 0 && p.in_flight == 4);
    // WRITE_DONE for slot 0
    ArAnswer a;
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = AR_IID_PLAY_SHMEM, .dst_port = AR_PORT_DATA,
                   .token = p.token_base, .opcode = AR_OP_WRITE_DONE};
    uint32_t w[4] = {0x9f000000u, 1, 0x77, 0};
    uint32_t m = gpr_encode(rsp, sizeof rsp, &r, w, 16);
    CHECK(ar_parse(rsp, m, &a) == ArWriteDone && ar_pump_done(&p, &a) == 1 && p.in_flight == 3 && p.returned == 1);
    CHECK(ar_pump_done(&p, &a) == -1 && p.errors == 1);   // slot 0 is not out any more
    CHECK(ar_pump_free_slot(&p) == 4 && ar_pump_write(&p, b, sizeof b, 110));
    CHECK(ar_pump_check(&p, 140) == 0 && ar_pump_check(&p, 152) == -1 && p.late == 1);   // slot 1 sent at 101, out 51 ms
    // a WRITE_DONE with an error status, an address inside a period, another handle, another port
    r.token = p.token_base + 1; w[0] = 0x9f000000u + 7680u; w[3] = 5;
    m = gpr_encode(rsp, sizeof rsp, &r, w, 16);
    CHECK(ar_parse(rsp, m, &a) == ArWriteDone && ar_pump_done(&p, &a) == -1 && p.last_status == 5 && p.in_flight == 3);
    r.token = p.token_base + 2; w[0] = 0x9f000000u + 7680u * 2 + 4; w[3] = 0;
    m = gpr_encode(rsp, sizeof rsp, &r, w, 16);
    CHECK(ar_parse(rsp, m, &a) == ArWriteDone && ar_pump_done(&p, &a) == -1);
    w[0] = 0x9f000000u + 7680u * 2; w[2] = 0x78;
    m = gpr_encode(rsp, sizeof rsp, &r, w, 16);
    CHECK(ar_parse(rsp, m, &a) == ArWriteDone && ar_pump_done(&p, &a) == -1);
    w[2] = 0x77; r.dst_port = AR_PORT_APM;
    m = gpr_encode(rsp, sizeof rsp, &r, w, 16);
    CHECK(ar_parse(rsp, m, &a) == ArWriteDone && ar_pump_done(&p, &a) == 0);   // not this pump's
    r.dst_port = AR_PORT_DATA; r.token = p.token_base + 3;   // slot 2's address with slot 3's token
    m = gpr_encode(rsp, sizeof rsp, &r, w, 16);
    CHECK(ar_parse(rsp, m, &a) == ArWriteDone && ar_pump_done(&p, &a) == -1);
    r.token = p.token_base + 2;
    m = gpr_encode(rsp, sizeof rsp, &r, w, 16);
    CHECK(ar_parse(rsp, m, &a) == ArWriteDone && ar_pump_done(&p, &a) == 1 && p.in_flight == 2);
    // round the ring: slots 5, 6, 7, then 0 again
    for (int i = 5; i < 8; ++i) {
        if (ar_pump_free_slot(&p) < 0) break;
        CHECK(ar_pump_write(&p, b, sizeof b, 200));
    }
    CHECK(p.next == 7 && p.in_flight == 4);
}

int main(void)
{
    testUpAndStop();
    testErrorAtEveryStep();
    testTimeoutAtEveryStep();
    testAmplifiersFail();
    testAbort();
    testUndoErrorsContinue();
    testIgnored();
    testMapProblems();
    testPump();
    printf("PASS sequence: %u checks\n", checks);
    return 0;
}
