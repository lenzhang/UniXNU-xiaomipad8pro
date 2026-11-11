/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* piano's audio DSP (ADSP, LPASS), which runs the charger firmware (PianoAdsp.cpp). UniXNU-sheng's ShengAdsp.h client interface,
 * renamed: a client registers a GLINK channel by name at any time (before or after the DSP is up) and gets its state and messages
 * through callbacks that run on the DSP thread; they must not block (copy what they need and wake their own thread). Sends are queued
 * and go out from the DSP thread, so a caller never waits for the DSP. */
#ifndef PIANO_ADSP_H
#define PIANO_ADSP_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PIANO_ADSP_MAX_MESSAGE 560u

typedef struct PianoAdspChannel PianoAdspChannel;
typedef struct {
    void (*state)(void *context, int up);
    void (*receive)(void *context, const uint8_t *data, uint32_t length);
} PianoAdspClient;

/* Registers the channel; intent_size/intent_count are the receive buffers offered to the DSP. initiator: we open it (otherwise we answer
 * the DSP's open). Returns null when out of slots or memory. */
PianoAdspChannel *piano_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                     const PianoAdspClient *client, void *context);
/* Queues one message (at most PIANO_ADSP_MAX_MESSAGE bytes): 0 queued, -1 queue full or too big, -2 bad arguments. */
int piano_adsp_send(PianoAdspChannel *channel, const void *data, uint32_t length);

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

/* The IPC router's link and service locator (PianoQrtr.c): registered before the DSP starts. */
void piano_qrtr_start(void);
void piano_qrtr_counters(uint32_t *stage, uint32_t *requests, uint32_t *answers, uint32_t *services);
/* The charger domain's state as the DSP's service registry reports it (Servreg.h): 0 unknown, 0x1fffffff up, ...; *ups (optional)
 * counts its transitions to up, one per charger generation. */
uint32_t piano_qrtr_charger_pd(uint32_t *ups);

/* The firmware files (PianoAdspFirmware.c). */
int piano_fw_open(const char *path, void **handle, uint64_t *size);
int piano_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void piano_fw_close(void *handle);

#ifdef __cplusplus
}
#endif
#endif
