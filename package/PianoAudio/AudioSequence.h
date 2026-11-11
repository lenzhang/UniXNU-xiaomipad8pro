/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The rule of every DSP exchange of PianoAudio (docs/drivers/audio.md 10.1: the DSP also runs the charger firmware and nothing restarts
 * it natively): one command at a time, each answer checked; at the first error (an answer with a non-zero status, a missing answer, the
 * DSP going down, the owner's abort, e.g. its watchdog) the steps already done are undone in reverse order, and the amplifiers are
 * powered down whatever else fails. Steps are DSP commands (a packet builder and the answer it waits for) or local actions the owner
 * runs (the amplifiers' power). Then the speaker plan of piano built from it, and the period pump of a running stream.
 * Pure logic, new code for PianoAudio: no I/O, no clock, no waits; the owner sends what ar_seq_next returns, feeds back the packets
 * that arrive and its timer, and runs the steps' local actions through the callbacks. */
#ifndef PIANO_AUDIO_SEQUENCE_H
#define PIANO_AUDIO_SEQUENCE_H
#include <stdint.h>
#include "PianoAudioReach.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ArSeq ArSeq;
/* A DSP command: writes the packet (token given) and returns its size, 0 when it cannot be built. */
typedef uint32_t (*ArBuild)(const ArSeq *s, uint8_t *out, uint32_t capacity, uint32_t token);
/* A local action: 0 done, else an error. */
typedef int (*ArLocal)(const ArSeq *s);

typedef struct {
    const char *name;
    ArBuild build;           /* a DSP command, or null for a local action */
    uint32_t opcode;         /* the command's opcode (what a basic result names) */
    int answer;              /* the answer that completes it: ArBasicResult, ArMapped, ArPrmDone or ArFrameworkState */
    ArLocal local;           /* the local action when build is null */
    ArBuild undo;            /* how it is undone: a DSP command, or null */
    uint32_t undo_opcode;
    int undo_answer;
    ArLocal undo_local;      /* or a local action, or nothing to undo */
    int prm;                 /* a PRM request: sent from, and answered to, the PRM port */
} ArStep;

enum {ArSeqIdle, ArSeqWaiting, ArSeqUp, ArSeqUnwinding, ArSeqDown, ArSeqFailed};
enum {ArSeqOk = 0, ArSeqErrStatus = 1, ArSeqErrTimeout = 2, ArSeqErrBuild = 3, ArSeqErrLocal = 4, ArSeqErrAbort = 5,
      ArSeqErrDspDown = 6, ArSeqErrAnswer = 7};
enum {ArSeqIgnored = 0, ArSeqAdvanced = 1, ArSeqError = 2};

struct ArSeq {
    const ArStep *steps;
    unsigned count;
    uint32_t port;           /* our GPR port for APM commands (their answers come back to it) */
    uint32_t prm_port;       /* and for PRM requests (a step with prm set) */
    uint32_t token_base;     /* step i's token: base + i; its undo's: base + 0x100 + i */
    void *context;           /* the owner's (the builders and local actions read it) */
    uint32_t handle;         /* the MEM_MAP handle, from the ArMapped answer (0: none) */
    int state, error, failed_step;
    unsigned done;           /* steps completed, forward */
    unsigned undo_next;      /* while unwinding: steps [0, undo_next) are still to undo */
    int dsp_down;            /* unwinding without the DSP: only local undos run */
    int timed_out_step;      /* the forward step that may have half run (no answer, a failed local action): undone too, or -1 */
    uint32_t pending_token, pending_opcode, pending_port;
    int pending_answer, pending_undo;
    uint32_t last_status, last_opcode;   /* of the last answer */
    uint32_t error_status;               /* the status of the answer that failed the sequence */
    unsigned answers, ignored, timeouts, undo_errors, locals;
};

void ar_seq_init(ArSeq *s, const ArStep *steps, unsigned count, uint32_t port, uint32_t prm_port, uint32_t token_base, void *context);
/* Runs local steps inline and returns the next packet to send (the sequence then waits for its answer), or 0: waiting, up, down or
 * failed. Call again after every receive/timeout/abort/stop. */
uint32_t ar_seq_next(ArSeq *s, uint8_t *out, uint32_t capacity);
/* A packet from the channel: ArSeqIgnored (not the awaited answer), ArSeqAdvanced, or ArSeqError (the sequence now unwinds). */
int ar_seq_receive(ArSeq *s, const uint8_t *in, uint32_t length);
/* The owner's answer timer ran out for the packet in flight. */
void ar_seq_timeout(ArSeq *s);
/* Stop now with an error (ArSeqErrAbort, ArSeqErrDspDown...): unwind what was done; dsp_down: skip the DSP commands (the amplifiers'
 * local undo still runs). From up, waiting or unwinding; a no-op when down or failed. */
void ar_seq_abort(ArSeq *s, int error, int dsp_down);
/* The normal stop of a sequence that is up: everything undone in reverse order, then down. */
void ar_seq_stop(ArSeq *s);
const char *ar_seq_step_name(const ArSeq *s, int step);

/* ---- piano's speaker plan: the TDM bit clock (PRM), the device graph (LOG, MFC, TDM sink) and the stream graph (WR_SH, DEC, CNV,
 * LOG) opened and configured, the period buffer mapped, both prepared and started, then the amplifiers powered up with the bit clock
 * running; undone in reverse (the amplifiers first, while the clock still runs). */
typedef struct {
    ArTdm tdm;
    ArFormat stream;         /* what the kernel writes: 48000, 16, 4 (FL FR LS RS) */
    ArFormat slots;          /* the TDM slots: 48000, 32, 4 */
    uint64_t buffer_phys;    /* the period buffer: physical address (below 4 GiB, 4 KiB aligned) and size; mapped for the DSP as */
    uint32_t buffer_bytes;   /* ar_dsp_address(buffer_phys, stream_id) */
    uint32_t stream_id;      /* the DSP's SMMU stream for PCM buffers: 0x1001 */
    void *owner;
    int (*amps_on)(void *owner);    /* start the four amplifiers (the bit clock runs); 0 or an error */
    int (*amps_off)(void *owner);   /* shut them down; must not fail silently: its error is counted, never stops the unwinding */
} ArSpeakerPlan;
/* The speaker plan's steps (static, ArSpeakerPlan as the sequence's context) and their number. */
const ArStep *ar_speaker_steps(unsigned *count);
void ar_speaker_defaults(ArSpeakerPlan *p);

/* ---- the period pump: a running stream keeps up to `depth` periods queued at the DSP, each slot of the mapped buffer written by the
 * owner before its WRITE goes out, refilled as WRITE_DONE returns it. A WRITE_DONE with an error, for an unknown slot, or a period out
 * longer than late_ms is an error: the owner then zeroes, aborts the sequence and powers the amplifiers down. */
#define AR_PUMP_MAX_SLOTS 16u
typedef struct {
    uint64_t base;           /* the buffer's DSP address */
    uint32_t handle, port, token_base;
    uint32_t slots, period_bytes, depth, late_ms;
    uint32_t next;           /* the slot the next WRITE uses */
    uint32_t in_flight;
    uint32_t sent_ms[AR_PUMP_MAX_SLOTS];
    uint8_t busy[AR_PUMP_MAX_SLOTS];
    uint32_t sent, returned, errors, late, last_status;
} ArPump;
/* 0, or -1 for a geometry that does not fit (slots 2..16, depth 1..slots, period > 0). */
int ar_pump_init(ArPump *p, uint64_t base, uint32_t handle, uint32_t port, uint32_t slots, uint32_t period_bytes, uint32_t depth,
                 uint32_t late_ms);
/* The slot the owner fills next, or -1 when `depth` periods are in flight (or that slot is still the DSP's). */
int ar_pump_free_slot(const ArPump *p);
/* The WRITE for that slot (filled now), stamped now_ms; returns its size or 0. */
uint32_t ar_pump_write(ArPump *p, uint8_t *out, uint32_t capacity, uint32_t now_ms);
/* A parsed answer: 1 a period returned, 0 not this pump's, -1 an error (bad status, an unknown or idle slot). */
int ar_pump_done(ArPump *p, const ArAnswer *a);
/* -1 when a period has been out more than late_ms at now_ms, else 0. */
int ar_pump_check(ArPump *p, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
