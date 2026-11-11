/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* piano's audio DSP (ADSP, LPASS), which runs the charger firmware (PianoAdsp.cpp): PianoPower's own side. What other kexts may use
 * (the GLINK channels, the QRTR client ports, the health and quiet calls) is package/include/PianoAdspContract.h, included here. */
#ifndef PIANO_ADSP_H
#define PIANO_ADSP_H
#include <stddef.h>
#include <stdint.h>
#include "PianoAdspContract.h"
#ifdef __cplusplus
extern "C" {
#endif

/* PianoPower's own sends on a channel it shares with clients (the router's): refused while fewer than keep_free queue slots would stay
 * free (the clients' share; PianoPower's own packets use keep_free 0). */
int piano_adsp_send_keep(PianoAdspChannel *channel, const void *data, uint32_t length, uint32_t keep_free);
/* True on PianoPower's DSP thread (where every callback runs). */
int piano_adsp_on_thread(void);
/* PianoPower raises and drops the quiet bits (PIANO_ADSP_QUIET_*) clients read with piano_adsp_quiet(). */
void piano_adsp_quiet_set(uint32_t bits, int on);
/* A callback into a client took this long (PianoQrtr.c's dispatch reports its own): counted, the first few logged. */
void piano_adsp_slow_callback(const char *who, uint32_t ms);
/* Wakes the DSP thread (a client's open wants its server events). */
void piano_adsp_wake(void);
/* Milliseconds since boot (mach_absolute_time). */
uint64_t piano_adsp_now_ms(void);

/* What the boot stages left, read only: the windows are mapped and read, nothing is written (piano-power=1, and the first step of 2). */
typedef struct {
    int32_t mapped;      /* 0, else the window that did not map: 1 SMEM, 2 TCSR mutex, 3 IPCC, 4 AOSS, 5 RSC */
    int32_t partition;   /* the APPS/ADSP SMEM partition: 0 found, else a QcomSmem error */
    uint32_t items;      /* bit 0 the DSP's SMP2P item (443), 1 ours (429), 2 GLINK descriptor (478), 3 our FIFO (479), 4 the DSP's
                          * FIFO (480), 9 the global partition */
    int32_t slave;       /* the DSP's "slave-kernel" bits: 0 fatal, 1 ready, 2 handover, 3 stop ack; -1 no item */
    uint32_t aoss;       /* AOSS message RAM: bit 12 its magic, bits 8-11 the four link/channel words up */
    uint32_t rsc_id;     /* the apps RSC DRV id register */
    char crash[80];      /* the SMEM crash reason (item 423), empty when there is none */
} PianoAdspSurvey;
int piano_adsp_survey(PianoAdspSurvey *out);

/* piano-power=2: in a thread of its own, loads the DSP's images from modem_a and starts it (PAS), then GLINK and the IPC router's
 * service locator. Returns at once; piano_adsp_status shows the progress. */
void piano_adsp_start(void);

typedef struct {
    int32_t stage;          /* PianoAdspStage; negative: the stage that failed */
    int32_t error;
    int32_t slave;          /* as in the survey */
    uint32_t doorbells;     /* IPCC doorbells taken */
    uint32_t glink_in, glink_out, channels_up, version_agreed, broken, unknown_opens;
    uint32_t ready_ms, adsp_kib;
    int64_t scm_x0;
    uint64_t scm_x1;
    uint32_t qrtr_stage, qrtr_requests, qrtr_answers, qrtr_services;
    uint32_t fatal;         /* FATAL reports from the DSP since the start */
    char text[96];          /* the firmware path that failed, the crash reason or the channels nobody serves */
} PianoAdspStatus;
void piano_adsp_status(PianoAdspStatus *out);
const char *piano_adsp_stage_name(int stage);

/* The bring-up steps, in order; piano-power-stop=N stops before step N. Each image has five: metadata, init_image, mem_setup (only
 * a relocatable image), segments, auth_and_reset. */
enum PianoAdspStage {
    PianoAdspMapped = 1, PianoAdspSurveyed = 2, PianoAdspFirmware = 3, PianoAdspSmp2p = 4, PianoAdspDoorbells = 5,
    PianoAdspLoadState = 6, PianoAdspRails = 7,
    PianoAdspDtbMeta = 8, PianoAdspDtbInit = 9, PianoAdspDtbSetup = 10, PianoAdspDtbLoad = 11, PianoAdspDtbAuth = 12,
    PianoAdspMeta = 13, PianoAdspInit = 14, PianoAdspSetup = 15, PianoAdspLoad = 16, PianoAdspAuth = 17,
    PianoAdspReady = 18, PianoAdspGlink = 19, PianoAdspChannelUp = 20,
};

/* The IPC router's link and service locator (PianoQrtr.c): registered before the DSP starts. The charger domain's state
 * (piano_qrtr_charger_pd, in the contract) comes from the DSP's service registry (Servreg.h). */
void piano_qrtr_start(void);
void piano_qrtr_counters(uint32_t *stage, uint32_t *requests, uint32_t *answers, uint32_t *services);
/* The router's client ports (PianoQrtr.c) for the status line: open ports, packets delivered, sends refused. */
void piano_qrtr_port_counters(uint32_t *open, uint32_t *delivered, uint32_t *refused);
/* On the DSP thread at every pass: delivers the server changes an open asked for. */
void piano_qrtr_poll(void);
/* The router's link has the DSP's hello. */
int piano_qrtr_link_up(void);

/* The firmware files (PianoAdspFirmware.c). */
int piano_fw_open(const char *path, void **handle, uint64_t *size);
int piano_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void piano_fw_close(void *handle);

#ifdef __cplusplus
}
#endif
#endif
