/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The FastRPC reverse file service of the ADSP's sensor protection domain. The sensor PD reads its registry and configuration from the
 * application processor: on Android /vendor/bin/sensors.qti (the daemon adsp_default_listener) attaches to the sensors PD and serves
 * apps_std file calls to it; without that the physical sensor drivers get no bus, interrupt or calibration and the SUID lookups find
 * no accel (build-userspace/a830-inputs/sensors/ssc-facts.md 6.1). This is that daemon in the kernel, portable C over a table of
 * operations (PaganiSensorsRpc.cpp gives the kernel's, Test/rpc.c a simulated DSP). Everything here is facts of the public protocol,
 * written from the descriptions in Qualcomm's BSD-3 fastrpc (its IDL files, inc/listener_buf.h) and Linux's fastrpc.c and hexagonrpc read for
 * facts only; no code was taken.
 *
 * Transport. One GLINK channel, "fastrpcglink-apps-dsp" (the DSP opens it). An invoke is a 40-byte message
 *   s32 pid, s32 tid, u64 ctx, u32 handle, u32 sc, u64 addr, u64 size
 * answered by { u64 ctx, s32 retval } (longer forms exist; the first twelve bytes are the same). ctx: our number << 8 | the PD (2: the
 * sensors PD). sc: attr << 29 | method << 24 | in buffers << 16 | out buffers << 8 | in handles << 4 | out handles. addr: the buffer of the
 * invoke's metadata: the context bank in bits 39:32 (compute-cb@2 of the 13T's devicetree: reg 4, pd-type 3 for the sensors PD, stream
 * ids 0x1004/0x1044 in the apps SMMU) over the DMA address, which is the physical address here: those streams are in pass-through
 * (the same planner as UFS, touch and speaker) and the DSP takes 32-bit addresses, so the arena lies below 4 GiB, outside XNU's memory.
 * The metadata, for n = in + out buffers (each array packed):
 *   n { u64 pv, u64 len }, n { u32 num, u32 pgidx }, n { u64 addr, u64 size }, 16 u64 fds, 64 u32 crcs
 * a buffer's pages are { addr & ~0xfff, whole 4 KiB pages }, the low twelve bits of pv its offset in the first one.
 * PianoSensors (UPSTREAM): Linux sends that metadata with every invoke, also with no buffers at all (16 zero fds and 64 zero crcs, the
 * message's addr/size its page with the bank above bit 32, size 4096: fastrpc_get_args runs for every invoke); PAG sent addr 0 size 0
 * then, and the tablet's adsp_listener answered such an init2 with AEE EBADPARM (0x8000040e, 2026-11-14; register took it). The framing
 * is a session property (SSR_FRAME_*): META (the default) is Linux's metadata page for every invoke; CTX4 numbers the ctx as Linux's
 * tree does (id << 4 | PD, PAG: id << 8 | PD); TID gives the messages a thread id of their own (Linux: the calling task's, never the
 * client id). An init2 the DSP answers with an error is tried again with the next framing (META, then + CTX4, then + TID); the one
 * that is answered 0 serves the rest of the session.
 *
 * Bring-up (each step an invoke on a fixed handle):
 *   attach    handle 1, method 0, in: u32 our process id, pid 0 in the message; from here on the messages carry that process id
 *   open      handle 0 (remotectl), method 0, in {name length, error length} + name, out {handle, error} + error text: "adsp_default_listener"
 *   register  that handle, method 0, no arguments; then remotectl close (method 1)
 *   init2     handle 3 (adsp_listener), method 3, no arguments
 *   next2     handle 3, method 4, in {previous ctx, previous result, previous out bytes, in capacity} + previous out buffers, out {ctx,
 *             handle, sc, in bytes} + the next call's in buffers. It blocks until the DSP calls us. First with ctx 0, result -1.
 *   release   handle 1, method 1 (Linux's FASTRPC_RMID_INIT_RELEASE, what Linux sends when the daemon's file closes), in: u32 our
 *             process id, pid 0 in the message (PianoSensors, UPSTREAM)
 * PianoSensors (UPSTREAM): once attached, the sensor PD asks its files through the listener and a thread of it that waits about a
 * minute unanswered is what sns_watchdog kills the whole DSP for (the tablet, 2026-11-14: attach + register, init2 refused, the fatal
 * 59 s later; with no attach at all the DSP stayed up). So a session never ends between the attach and the listening silently: every
 * stop after an answered attach (a step refused, the ladder exhausted, next2 failing, the deadline: not listening deadline_ms after the
 * attach, ssr_core_abort) sends the release first, unless the tellable op says the DSP may not be told (a fatal sign: never touch a
 * dying DSP). The status says what became of it (SSR_RELEASE_*).
 * The DSP's call, packed: each in buffer u32 length, (when not empty) aligned to 8, its bytes; then one u32 length per out buffer (unaligned
 * after the last in buffer). The answer: each out buffer u32 length (exactly what was asked), aligned to 8, its bytes; nothing at all when
 * the result is not 0. Alignment counts from the start of the blob. A method id above 30 goes as method 31 with the id as the first word
 * of in buffer 0.
 *
 * Local interfaces (the DSP's handles). 0 remotectl: open(name) gives the handles below, an unknown name -5 (logged: what is missing shows
 * in the boot log). 1 apps_std: the read-only file calls below, over the pack (SensorsPack.h). 2 apps_mem: pages of the arena's heap
 * for the DSP (map64 with ADD_PAGES, through the mmap method 4 of handle 1). The apps_std calls: fopen, fopen_with_env (a relative name
 * searched in ADSP_LIBRARY_PATH), fclose, fread, fgets, fseek, ftell, flen, rewind, feof, ferror, clearerr, fflush, fsync, opendir, readdir,
 * closedir, stat, fileExists, getenv, setenv, unsetenv, print_string, mkdir of what exists. Writes are refused (as hexagonrpcd does; the
 * pack holds the registry Android generated).
 *
 * What is proven: nothing here has met a real DSP. Test/rpc.c holds the layouts to the facts above byte by byte; the first run with the
 * phone decides the rest (see build-userspace/a830-inputs/sensors/build-report.md).
 * PianoSensors: piano's first run (2026-11-14, PAG's framing): attach, remotectl open, register and close answered 0; init2 answered
 * EBADPARM (docs/drivers/sensors.md section 8.2). */
#ifndef SENSORS_RPC_H
#define SENSORS_RPC_H
#include <stdint.h>
#include "SensorsPack.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SSR_CHANNEL "fastrpcglink-apps-dsp"
#define SSR_INTENT_SIZE 0x181u          /* the devicetree's qcom,intents: 385 bytes */
#define SSR_INTENT_COUNT 24u            /* PAGANI_ADSP_MAX_INTENTS (Android offers 64) */
#define SSR_CONTEXT_BANK 4u             /* compute-cb@2 reg, the sensors PD's (Android): bits 39:32 of every address the DSP gets */
#define SSR_PD_SENSORS 2u
#define SSR_MESSAGE 40u
#define SSR_ARENA_BYTES 0x800000u       /* the arena: 2 MiB for the calls (blobs up to about 1.9 MiB), the rest the DSP's heap (apps_mem) */
#define SSR_CALL_BYTES 0x200000u
#define SSR_BOUNCE 0x10000u             /* normal memory the pack is read through */
#define SSR_LIBRARY_PATH "/vendor/dsp/adsp;/vendor/dsp;/vendor/lib/rfsa/adsp;/odm/lib/rfsa/adsp;/vendor/dsp/sensors"
#define SSR_LIBRARY_PATH_FILE "/.pagani/library_path"   /* in the pack: overrides it (Android's own ADSP_LIBRARY_PATH) */
#define SSR_PACK_WAIT_MS 240000u        /* longest wait for the Data volume before going on with an empty pack */
/* The framing of a session's messages (PianoSensors, UPSTREAM; see above). SsrConfig.frame 0 is SSR_FRAME_DEFAULT, SSR_FRAME_PAG none. */
#define SSR_FRAME_META 1u               /* a metadata page with every invoke, also with no buffers (Linux) */
#define SSR_FRAME_CTX4 2u               /* ctx = id << 4 | PD (Linux's tree; else id << 8 | PD) */
#define SSR_FRAME_TID 4u                /* the messages' tid = our process id + SSR_TID_OFFSET (else the process id) */
#define SSR_FRAME_PAG 0x80u             /* PaganiSensors' framing: none of the above */
#define SSR_FRAME_DEFAULT SSR_FRAME_META
#define SSR_TID_OFFSET 1000u
#define SSR_DEADLINE_MS 15000u          /* the attach answered -> the first next2 sent, at most (the PD's watchdog: about a minute) */

/* Results towards the DSP (AEEStdErr.h). */
enum {SSR_OK = 0, SSR_EFAILED = 1, SSR_ENOMEMORY = 2, SSR_EBADPARM = 14, SSR_EUNSUPPORTED = 20, SSR_EBADSIZE = 0x4d, SSR_EINTERRUPTED = 46};
/* Stages; the status's stage is the last one reached, or minus the one that failed. */
enum {
    SsrStOff = 0, SsrStChannel = 1, SsrStPack = 2, SsrStSmmu = 3, SsrStAttach = 4, SsrStRegister = 5, SsrStListen = 6, SsrStServing = 7,
};
enum {SsrPackOk = 0, SsrPackNotYet = 1, SsrPackAbsent = 2, SsrPackError = -1};
/* What became of the session at a stop (SsrStatus.release): none needed (never attached, or the link went away), released and answered
 * 0, answered with an error, not answered (or not sent), withheld (the tellable op: the DSP may not be told). */
enum {SSR_RELEASE_NONE = 0, SSR_RELEASE_DONE = 1, SSR_RELEASE_REFUSED = 2, SSR_RELEASE_UNANSWERED = 3, SSR_RELEASE_WITHHELD = 4};

typedef struct {
    void *context;
    uint64_t (*now_ms)(void *context);
    /* One message on the channel: 0 queued. */
    int (*send)(void *context, const uint8_t *message, uint32_t length);
    /* Sleeps up to ms; a response or a channel change wakes it. */
    void (*wait)(void *context, uint32_t ms);
    void (*trace)(void *context, const char *line);
    /* What was written to the arena is visible to the DSP (a barrier). */
    void (*publish)(void *context);
    /* Once, before the first attach: the sensors PD's streams in pass-through. 0, or the error that stops the service. Late on purpose: the
     * SMMU planner has no lock, and by then the boot's other users (UFS, touch) have long finished theirs. */
    int (*prepare)(void *context);
    /* The pack file: its size (SsrPack*). */
    int (*pack_size)(void *context, uint64_t *size);
    /* length bytes at offset into normal memory: 0, or nonzero. */
    int (*pack_read)(void *context, uint64_t offset, void *to, uint32_t length);
    /* Memory for the pack's table, never freed. */
    void *(*alloc)(void *context, uint32_t bytes);
    /* PianoSensors (UPSTREAM): asked once for each call of the DSP before it is served; 0 declines it, answered at once with
     * SSR_EUNSUPPORTED and no out buffers (SensorsGuard.h: a call is never left unanswered). Null: every call is served. */
    int (*admit)(void *context);
    /* PianoSensors (UPSTREAM): asked before the release; 0 withholds it (the DSP shows a fatal sign, another link or charger generation:
     * a dying DSP is never touched). Null: always told. */
    int (*tellable)(void *context);
} SsrOps;

typedef struct {
    uint8_t *arena;                    /* mapped uncached: everything the DSP sees */
    uint64_t arena_pa;                 /* physical, below 4 GiB */
    uint32_t arena_size;
    uint8_t *bounce;                   /* SSR_BOUNCE bytes of normal memory */
    uint32_t process;                  /* our process id on the DSP */
    uint32_t bank;                     /* the context bank (SSR_CONTEXT_BANK; Linux takes the first free one, so 3 is worth a try too) */
    uint32_t log;                      /* 0 failures, 1 rationed, 2 more */
    /* PianoSensors (UPSTREAM), 0 the defaults: */
    uint32_t frame;                    /* the first framing: SSR_FRAME_* bits, SSR_FRAME_PAG for none; 0 SSR_FRAME_DEFAULT */
    uint32_t no_ladder;                /* 1: an init2 answered with an error is not tried again with the next framing */
    uint32_t deadline_ms;              /* 0: SSR_DEADLINE_MS */
} SsrConfig;

enum {SSR_FILES = 32, SSR_DIRS = 8};
typedef struct {
    int used;
    uint32_t entry;
    uint64_t position;
} SsrFile;
typedef struct {
    int used;
    uint32_t entry, cursor;
} SsrDir;

typedef struct {
    int32_t stage, error;
    uint32_t attaches, calls, opens, missing, reads, refused, unknown, declined;   /* declined: PianoSensors' admit said no */
    uint64_t bytes;
    uint32_t heap_used;
    char note[80];                     /* the first path that was missing */
    uint32_t frame, inits;             /* PianoSensors: the session's framing (SSR_FRAME_* bits), the init2 calls answered */
    int32_t release;                   /* SSR_RELEASE_* at the stop */
} SsrStatus;

typedef struct {
    const SsrOps *ops;
    SsrConfig cfg;
    /* set on the DSP thread, read here */
    volatile uint32_t link_up, link_changes, responses;
    volatile uint64_t response_ctx;
    volatile int32_t response_value;
    /* the thread's */
    int phase;
    uint32_t seen_changes, seen_responses;
    uint32_t next_id, stray;
    uint64_t started_ms, retry_at, pack_since;
    uint32_t tries;
    int pack_done, prepared;
    SspPack pack;
    SsrFile file[SSR_FILES];
    SsrDir dir[SSR_DIRS];
    uint32_t heap_used;
    uint32_t listener_handle;
    uint32_t rctx;
    int32_t result;
    uint32_t out_length;
    uint32_t next_failures;
    char library_path[192];
    unsigned budget, open_lines, missing_lines, summary_lines;
    int summary_due;
    SsrStatus status;
    char line[240];
    /* PianoSensors (UPSTREAM) */
    uint32_t frame;                    /* the session's framing (SSR_FRAME_* bits) */
    int attached;                      /* the attach was answered 0 and no release has been sent since */
    uint64_t attached_ms;
} SsrCore;

void ssr_core_init(SsrCore *c, const SsrOps *ops, const SsrConfig *config);
/* On the DSP thread: the channel's state, a message of the channel. They only record; the glue wakes the thread. */
void ssr_core_channel(SsrCore *c, int up);
void ssr_core_receive(SsrCore *c, const uint8_t *data, uint32_t length);
/* One step of the thread (a stage, or one call of the DSP served): 1 go on, 0 stopped for good (the status says why). */
int ssr_core_step(SsrCore *c);
/* PianoSensors (UPSTREAM). 1 between an answered attach and the listening (the window in which the sensor PD may wait for us). */
int ssr_core_bringing_up(const SsrCore *c);
/* 1 when that window has lasted the deadline: the glue calls ssr_core_abort (also while it holds the steps back). */
int ssr_core_overdue(SsrCore *c);
/* Stops the service for good (if not stopped): a session between the attach and the listening is released first (tellable). */
void ssr_core_abort(SsrCore *c, const char *why);
/* The framing's name: "pag", "meta", "meta+ctx4", ... */
const char *ssr_frame_name(uint32_t frame);

#ifdef __cplusplus
}
#endif
#endif
