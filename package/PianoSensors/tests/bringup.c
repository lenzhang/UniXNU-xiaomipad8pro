// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// Host tests of the file service's bring-up after the tablet's first S2 (batch-2 adsp run, 2026-11-14): attach, remotectl open, register
// and close were answered 0, adsp_listener init2 (handle 3, method 3, no buffers) was answered AEE EBADPARM 0x8000040e, the service
// stopped, and 59 s after the attach the sensor PD's watchdog took the whole ADSP (sns_watchdog.c:84, qsh_process).
//
// Against a simulated DSP written from Linux's fastrpc.c as facts (no code shared with SensorsRpc.c): every invoke carries the metadata
// page (fastrpc_internal_invoke calls fastrpc_get_args for every invoke: n {pv, len}, n {num, pgidx}, n {addr, size}, 16 u64 fds, 64 u32
// crcs; the message's addr its page with the bank in bits 32 and up, size whole pages), ctx = id << 4 | PD in Linux's tree, the
// message's tid the calling task's, the release INIT_RELEASE = handle 1, method 1, one in buffer {client id}, client id 0 in the message.
// The DSP's init2 is scripted by what the message carries, which is how the three framings are told apart; the tablet decides which.
//
// Covered: the default framing byte for byte (register and init2 with a page of 384 zero bytes over junk in the arena, ctx id << 8,
// the tid the client id), the tablet's failure reproduced with PaganiSensors' framing and the session released, the ladder (pag ->
// meta, meta -> meta+ctx4 -> meta+ctx4+tid, the framing kept for next2), the ladder exhausted (released, no next2), the release withheld
// from a DSP the glue says may not be told, the deadline (an open never answered: released well inside the PD's minute), an abort by
// the glue, no release after a link loss, and the guard's file-service strike.
#include "check.h"
#include "../SensorsGuard.h"
#include "../SensorsRpc.h"

#define ARENA_PA 0x9d200000ull
#define CLIENT 1u                     /* Linux's first client id: the kext's */
#define BANK 3u
#define EBADPARM ((int32_t)0x8000040e)   /* DSP_AEE_EOFFSET 0x80000400 + AEE_EBADPARM 14: the tablet's answer */

static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t rd64(const uint8_t *p) {return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;}
static void wr32(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));}
static void wr64(uint8_t *p, uint64_t v) {wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));}

static uint8_t *arena;
static uint8_t bounce[SSR_BOUNCE];
static SsrCore core;
static uint64_t clock_ms;
static char logbuf[1 << 16];
static size_t logused;

/* The DSP's script and record. */
enum {INIT_NEEDS_PAGE, INIT_NEEDS_TID, INIT_NEVER, INIT_ALWAYS};
static struct {
    int init_rule, silent_open, tell;
    int pending;
    uint8_t m[SSR_MESSAGE];
    unsigned messages, attaches, opens, registers, closes, inits, next2s, releases;
    uint64_t attach_at, release_at;
    uint32_t init_tid[4], init_ctx_low[4], init_page[4];
    uint64_t next2_ctx;
    uint32_t next2_tid;
    uint32_t order[16];
} dsp;

static uint64_t op_now(void *c) {(void)c; return clock_ms;}
static void op_trace(void *c, const char *line)
{
    (void)c;
    const size_t n = strlen(line);
    if (logused + n + 2 < sizeof logbuf) {
        memcpy(logbuf + logused, line, n);
        logused += n;
        logbuf[logused++] = '\n';
        logbuf[logused] = 0;
    }
}
static int logged(const char *text) {return strstr(logbuf, text) != NULL;}
static int op_send(void *c, const uint8_t *m, uint32_t n) {(void)c; CHECK(n == SSR_MESSAGE && !dsp.pending); memcpy(dsp.m, m, n); dsp.pending = 1; return 0;}
static void op_publish(void *c) {(void)c;}
static int op_prepare(void *c) {(void)c; return 0;}
static int op_pack_size(void *c, uint64_t *size) {(void)c; *size = 0; return SsrPackAbsent;}
static int op_pack_read(void *c, uint64_t o, void *to, uint32_t n) {(void)c; (void)o; (void)to; (void)n; return 1;}
static void *op_alloc(void *c, uint32_t n) {(void)c; return calloc(1, n);}
static int op_admit(void *c) {(void)c; return 1;}
static int op_tellable(void *c) {(void)c; return dsp.tell;}

/* The metadata the way Linux lays it out; the bytes of buffer i (bank bits dropped), NULL when empty. */
static const uint8_t *metadata_of(const uint8_t *m, uint32_t *n)
{
    const uint32_t sc = rd32(m + 20);
    const uint64_t addr = rd64(m + 24), size = rd64(m + 32);
    *n = (sc >> 16 & 0xff) + (sc >> 8 & 0xff);
    if (!addr && !size) return NULL;                  /* PaganiSensors' framing of an invoke without buffers */
    CHECK(addr >> 32 == BANK && !(addr & 0xfff) && size == 0x1000);
    const uint64_t pa = addr & 0xffffffffull;
    CHECK(pa >= ARENA_PA && pa + size <= ARENA_PA + SSR_ARENA_BYTES);
    const uint8_t *meta = arena + (pa - ARENA_PA);
    for (unsigned i = 0; i < 16 * 8 + 64 * 4; ++i) CHECK(meta[*n * 40 + i] == 0);   /* fds and crcs zero (the arena was junk) */
    return meta;
}
static uint8_t *buffer(const uint8_t *meta, uint32_t n, uint32_t i, uint64_t *len)
{
    CHECK(meta && i < n);
    const uint8_t *list = meta + n * 16, *pages = list + n * 8;
    const uint64_t pv = rd64(meta + i * 16);
    *len = rd64(meta + i * 16 + 8);
    if (!*len) return NULL;
    const uint64_t page = rd64(pages + i * 16);
    CHECK(page >> 32 == BANK && rd32(list + i * 8) == 1);
    return arena + ((page & 0xffffffffull) - ARENA_PA) + (pv & 0xfff);
}

static void respond(int32_t value)
{
    uint8_t r[16] = {0};
    wr64(r, rd64(dsp.m + 8));
    wr32(r + 8, (uint32_t)value);
    dsp.pending = 0;
    ssr_core_receive(&core, r, sizeof r);
}

/* One message of the service, answered (or not) as the script says. */
static void dsp_turn(void)
{
    const uint8_t *m = dsp.m;
    const uint32_t pid = rd32(m), tid = rd32(m + 4), handle = rd32(m + 16), sc = rd32(m + 20), method = sc >> 24 & 0x1f;
    const uint64_t ctx = rd64(m + 8);
    uint32_t n = 0;
    uint64_t len = 0;
    const uint8_t *meta = metadata_of(m, &n);
    CHECK((ctx & 0xf) == SSR_PD_SENSORS && !(sc & 0xff) && !(sc >> 29));
    if (n) CHECK(meta);                               /* buffers always come with their metadata */
    if (dsp.messages < 16) dsp.order[dsp.messages] = handle << 8 | method;
    dsp.messages++;
    if (handle == 1 && method == 0) {                 /* attach */
        CHECK(pid == 0 && sc == 0x00010000u && rd32(buffer(meta, n, 0, &len)) == CLIENT && len == 4);
        dsp.attaches++;
        dsp.attach_at = clock_ms;
        respond(0);
        return;
    }
    if (handle == 1 && method == 1) {                 /* release: Linux's INIT_RELEASE */
        CHECK(pid == 0 && sc == 0x01010000u && meta && rd32(buffer(meta, n, 0, &len)) == CLIENT && len == 4);
        dsp.releases++;
        dsp.release_at = clock_ms;
        respond(0);
        return;
    }
    CHECK(pid == CLIENT);
    if (handle == 0 && method == 0) {                 /* remotectl open */
        CHECK(sc == 0x00020200u);
        dsp.opens++;
        if (dsp.silent_open) {dsp.pending = 0; return;}    /* never answered */
        uint8_t *out = buffer(meta, n, 2, &len);
        wr32(out, 0x77);
        wr32(out + 4, 0);
        respond(0);
        return;
    }
    if (handle == 0x77) {                             /* register: no buffers, so a page of 384 zero bytes under the default */
        CHECK(sc == 0 && n == 0);
        dsp.registers++;
        respond(0);
        return;
    }
    if (handle == 0 && method == 1) {dsp.closes++; respond(0); return;}
    CHECK(handle == 3);
    if (method == 3) {                                /* init2: no buffers */
        CHECK(sc == 0x03000000u && n == 0);
        const unsigned i = dsp.inits++;
        if (i < 4) {
            dsp.init_tid[i] = tid;
            dsp.init_ctx_low[i] = (uint32_t)(ctx & 0xff);
            dsp.init_page[i] = meta != NULL;
        }
        int ok = 0;
        switch (dsp.init_rule) {
        case INIT_NEEDS_PAGE: ok = meta != NULL; break;
        case INIT_NEEDS_TID: ok = meta != NULL && tid != pid; break;
        case INIT_ALWAYS: ok = 1; break;
        default: ok = 0; break;
        }
        respond(ok ? 0 : EBADPARM);
        return;
    }
    CHECK(method == 4 && sc == 0x04020200u);          /* next2: the session serves; the test ends it by taking the link away */
    dsp.next2s++;
    dsp.next2_ctx = ctx;
    dsp.next2_tid = tid;
    dsp.pending = 0;
    ssr_core_channel(&core, 0);
}

/* The DSP answers within a millisecond; with nothing to answer the wait runs its whole time. */
static void op_wait(void *c, uint32_t ms)
{
    (void)c;
    if (dsp.pending) {
        clock_ms += 1;
        dsp_turn();
        return;
    }
    clock_ms += ms ? ms : 1;
}

static const SsrOps ops = {NULL, op_now, op_send, op_wait, op_trace, op_publish, op_prepare, op_pack_size, op_pack_read, op_alloc,
                           op_admit, op_tellable};

static void start(uint32_t frame, uint32_t no_ladder, int rule)
{
    memset(&dsp, 0, sizeof dsp);
    dsp.init_rule = rule;
    dsp.tell = 1;
    memset(arena, 0x5a, SSR_ARENA_BYTES);             /* junk: the page's fds and crcs must be cleared by the service */
    logused = 0;
    logbuf[0] = 0;
    clock_ms = 1000;
    const SsrConfig cfg = {arena, ARENA_PA, SSR_ARENA_BYTES, bounce, CLIENT, BANK, 2, frame, no_ladder, 0};
    ssr_core_init(&core, &ops, &cfg);
    ssr_core_channel(&core, 1);
}

/* Steps until the session serves (a next2 is out), stops, or the step budget ends; the service's thread in miniature. */
static int run(unsigned budget)
{
    for (unsigned i = 0; i < budget; ++i) {
        if (!ssr_core_step(&core)) return 0;
        if (dsp.next2s) return 1;
    }
    return 1;
}

static void default_framing(void)
{
    start(0, 0, INIT_NEEDS_PAGE);
    CHECK(run(100) && dsp.next2s == 1);
    CHECK(dsp.attaches == 1 && dsp.opens == 1 && dsp.registers == 1 && dsp.closes == 1 && dsp.inits == 1 && dsp.releases == 0);
    CHECK(dsp.order[0] == (1u << 8 | 0) && dsp.order[1] == 0 && dsp.order[2] == (0x77u << 8) && dsp.order[3] == 1 && dsp.order[4] == (3u << 8 | 3));
    CHECK(dsp.init_page[0] && dsp.init_tid[0] == CLIENT && dsp.init_ctx_low[0] == SSR_PD_SENSORS);   /* id << 8: bits 4..7 clear */
    CHECK(core.status.frame == SSR_FRAME_META && core.status.inits == 1 && core.status.stage == SsrStListen);
    CHECK((dsp.next2_ctx & 0xff) == SSR_PD_SENSORS && dsp.next2_ctx >> 8 && dsp.next2_tid == CLIENT);
    CHECK(logged("attached to the sensors PD") && logged("frame=meta deadline=15000") && logged("listening frame=meta inits=1"));
    CHECK(!strcmp(ssr_frame_name(SSR_FRAME_DEFAULT), "meta") && !strcmp(ssr_frame_name(0), "pag") &&
          !strcmp(ssr_frame_name(7), "meta+ctx4+tid"));
    printf("PASS default framing: register and init2 carry Linux's metadata page (384 zero bytes over junk, bank 3), ctx id << 8, tid "
           "the client id; init2 answered at once, next2 follows\n");
}

static void tablet_failure(void)
{
    /* PaganiSensors' framing, no ladder: the tablet's run, and now the session is released at once */
    start(SSR_FRAME_PAG, 1, INIT_NEEDS_PAGE);
    CHECK(!run(100));
    CHECK(dsp.inits == 1 && !dsp.init_page[0] && dsp.next2s == 0);
    CHECK(logged("FAIL st=-6 err=-2147482610 adsp_listener init2 failed"));             /* the tablet's line, byte for byte */
    CHECK(dsp.releases == 1 && dsp.release_at - dsp.attach_at < 1000 && core.status.release == SSR_RELEASE_DONE);
    CHECK(logged("released the sensors PD's session rc=0 value=0") && core.status.stage == -SsrStListen);
    CHECK(dsp.order[dsp.messages - 1] == (1u << 8 | 1));                                 /* the release is the last message */
    CHECK(!ssr_core_step(&core) && dsp.messages == 6);                                   /* stopped: nothing more */

    /* PaganiSensors' framing with the ladder (piano-sensors-rpc-frame=0): the control, then Linux's page in the same session */
    start(SSR_FRAME_PAG, 0, INIT_NEEDS_PAGE);
    CHECK(run(100) && dsp.next2s == 1 && dsp.inits == 2 && !dsp.init_page[0] && dsp.init_page[1] && dsp.releases == 0);
    CHECK(logged("init2 frame=pag answered rc=0 value=-2147482610: next frame=meta") && logged("listening frame=meta inits=2"));
    CHECK(core.status.frame == SSR_FRAME_META);
    printf("PASS the tablet's failure: PaganiSensors' page-less init2 answered EBADPARM, the session released at once (the last "
           "message); with the ladder the same session listens with Linux's page\n");
}

static void ladder(void)
{
    /* a DSP that also wants a thread id of its own: meta, meta+ctx4, meta+ctx4+tid; the framing stays for next2 */
    start(0, 0, INIT_NEEDS_TID);
    CHECK(run(100) && dsp.next2s == 1 && dsp.inits == 3 && dsp.releases == 0);
    CHECK(dsp.init_page[0] && dsp.init_page[1] && dsp.init_page[2]);
    CHECK(dsp.init_tid[0] == CLIENT && dsp.init_tid[1] == CLIENT && dsp.init_tid[2] == CLIENT + SSR_TID_OFFSET);
    CHECK(core.status.frame == (SSR_FRAME_META | SSR_FRAME_CTX4 | SSR_FRAME_TID));
    /* the context numbers 1..8 in order (attach, open, register, close, three init2, next2): id << 8 first, id << 4 from ctx4 on */
    CHECK(dsp.init_ctx_low[0] == 0x02 && dsp.init_ctx_low[1] == 0x62 && dsp.init_ctx_low[2] == 0x72);
    CHECK(dsp.next2_tid == CLIENT + SSR_TID_OFFSET && dsp.next2_ctx == 0x82);
    CHECK(logged("next frame=meta+ctx4") && logged("next frame=meta+ctx4+tid") && logged("listening frame=meta+ctx4+tid inits=3"));

    /* the ladder exhausted: released, no next2, stopped */
    start(0, 0, INIT_NEVER);
    CHECK(!run(100) && dsp.inits == 3 && dsp.next2s == 0 && dsp.releases == 1 && core.status.release == SSR_RELEASE_DONE);
    CHECK(dsp.release_at - dsp.attach_at < 1000 && logged("FAIL st=-6 err=-2147482610 adsp_listener init2 failed"));

    /* no ladder: one init2 only */
    start(0, 1, INIT_NEVER);
    CHECK(!run(100) && dsp.inits == 1 && dsp.releases == 1);
    printf("PASS ladder: an init2 answered with an error goes again with + ctx4, then + tid; the framing that is answered 0 serves "
           "next2; exhausted: released at once, nothing more\n");
}

static void withheld_and_deadline(void)
{
    /* the glue says the DSP may not be told (a fatal sign): no release, said */
    start(0, 0, INIT_NEVER);
    dsp.tell = 0;
    CHECK(!run(100) && dsp.releases == 0 && core.status.release == SSR_RELEASE_WITHHELD && logged("release withheld"));

    /* an open never answered: 5 s per try, 1 s apart; the deadline (15 s after the attach) releases the session */
    start(0, 0, INIT_ALWAYS);
    dsp.silent_open = 1;
    CHECK(!run(1000));
    CHECK(dsp.opens == 3 && dsp.releases == 1 && dsp.inits == 0 && core.status.release == SSR_RELEASE_DONE);
    CHECK(dsp.release_at - dsp.attach_at >= SSR_DEADLINE_MS && dsp.release_at - dsp.attach_at < SSR_DEADLINE_MS + 7000);   /* < 59 s */
    CHECK(logged("the listener was not up within the deadline") && core.status.stage == -SsrStRegister);

    /* the glue's abort while the bring-up is held (the guard struck, or quiet ran past the deadline) */
    start(0, 0, INIT_ALWAYS);
    dsp.silent_open = 1;
    for (unsigned i = 0; i < 50 && !dsp.opens; ++i) CHECK(ssr_core_step(&core));
    CHECK(ssr_core_bringing_up(&core) && !ssr_core_overdue(&core));
    clock_ms += SSR_DEADLINE_MS;
    CHECK(ssr_core_overdue(&core));
    ssr_core_abort(&core, "the guard is off before the listening");
    CHECK(dsp.releases == 1 && !ssr_core_bringing_up(&core) && !ssr_core_step(&core));
    ssr_core_abort(&core, "twice");                                                       /* stopped: nothing */
    CHECK(dsp.releases == 1 && logged("the guard is off before the listening"));

    /* before the attach nothing is released; after a link loss the DSP's side is gone: nothing either */
    start(0, 0, INIT_ALWAYS);
    ssr_core_abort(&core, "before the attach");
    CHECK(dsp.messages == 0 && core.status.release == SSR_RELEASE_NONE);
    start(0, 0, INIT_ALWAYS);
    dsp.silent_open = 1;
    for (unsigned i = 0; i < 50 && !dsp.opens; ++i) CHECK(ssr_core_step(&core));
    ssr_core_channel(&core, 0);
    for (unsigned i = 0; i < 20; ++i) CHECK(ssr_core_step(&core));
    CHECK(!ssr_core_bringing_up(&core) && dsp.releases == 0 && logged("channel went away"));
    printf("PASS release: withheld from a DSP that may not be told; the deadline (an open never answered) releases inside 22 s of the "
           "attach; the glue's abort; nothing before an attach or after a link loss\n");
}

static void guard_strike(void)
{
    SgGuard g;
    sg_init(&g, 0);
    sg_strike(&g, SG_REASON_FILES);
    CHECK(g.state == SG_OFF && g.reason == SG_REASON_FILES && !sg_serve_files(&g) && !sg_may_request(&g, 1));
    CHECK(!strcmp(sg_reason_name(SG_REASON_FILES), "the file service failed after the attach"));
    sg_init(&g, 0);
    const SgDsp fatal = {1, 1, 0, SG_CHARGER_UP, 1, 0, 0};
    CHECK(sg_observe(&g, &fatal, 1) == SG_OFF && g.reason == SG_REASON_FATAL);
    sg_strike(&g, SG_REASON_FILES);
    CHECK(g.reason == SG_REASON_FATAL);                                                  /* the first reason stays */
    printf("PASS guard: the file service's strike, off for the boot; an earlier reason kept\n");
}

int main(void)
{
    arena = aligned_alloc(0x1000, SSR_ARENA_BYTES);
    CHECK(arena);
    default_framing();
    tablet_failure();
    ladder();
    withheld_and_deadline();
    guard_strike();
    free(arena);
    printf("PASS PianoSensors bring-up tests: %u checks\n", checks);
    return 0;
}
