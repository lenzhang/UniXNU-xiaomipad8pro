/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The charger as piano's audio DSP firmware serves it on the PMIC GLINK channel "PMIC_RTR_ADSP_APPS". This firmware does not speak the
 * Qualcomm battery manager protocol (UniXNU-sheng's ShengBattmgr.c); it has a flat property space of its own (Xiaomi's "MCA"), under the
 * owner the battery manager has elsewhere (docs/drivers/charging.md section 3 is the specification this was written from):
 *   request  {owner, type 1, opcode (1 read, 2 write), property, seq, data[256]}                 276 bytes
 *   response {owner, type 1, opcode, property, retcode, seq, data[256]}                          280 bytes
 *   notification: type 2 on the same owner (something changed: read again)
 * and, under the USB-C port owner, the port notifications the firmware holds its Type-C/PD state machine for until the host
 * acknowledges them (up to 13 s each): enable once {owner, type 1, opcode 0x15, command 0x10, argument 0, 0}, then acknowledge each
 * notification (opcode 0x16, port index in byte 12) with command 0x11 and the port index.
 *
 * This is the codec, one refresh cycle at a time (one request in flight; the owner's timer gives up on one after a second), and the
 * charging policy: after every cycle it decides which writes are due. Writes are limited to the input current limit, the charge
 * current, the charger's enable bit and the fixed PDO request, each within conservative bounds (piano_mca_write_allowed); anything
 * else is refused before it is built. No I/O and no waits: the owner sends the bytes and feeds back what arrives. */
#ifndef PIANO_MCA_H
#define PIANO_MCA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PIANO_MCA_OWNER 0x800au       /* 32778 */
#define PIANO_MCA_OWNER_PAN 0x800cu   /* 32780: USB-C port notifications */
#define PIANO_MCA_TYPE_REQUEST 1u
#define PIANO_MCA_TYPE_NOTIFY 2u
#define PIANO_MCA_OP_READ 1u
#define PIANO_MCA_OP_WRITE 2u
#define PIANO_MCA_DATA 256u
#define PIANO_MCA_REQUEST_BYTES 276u
#define PIANO_MCA_RESPONSE_BYTES 280u
#define PIANO_MCA_PAN_REQUEST_BYTES 24u
#define PIANO_MCA_PAN_WRITE 0x15u
#define PIANO_MCA_PAN_NOTIFY 0x16u
#define PIANO_MCA_PAN_ENABLE 0x10u
#define PIANO_MCA_PAN_ACK 0x11u
#define PIANO_MCA_PAN_PORTS 3u

/* Properties; bits 31:16 are the class (0 charger and battery, 2 USB and PD). The PD ids are the OS3 firmware's (HyperOS 4 firmware
 * inserts one at 0x2100b and moves the later ones up by one). */
#define PIANO_MCA_STATUS 0x2001u
#define PIANO_MCA_CHARGE_ENABLE 0x2003u   /* one byte: the charger's enable bit */
#define PIANO_MCA_FCC 0x2005u             /* charge current, mA; reading it is a no-op that returns zeroes, so it is never read */
#define PIANO_MCA_ICL 0x2008u             /* input current limit, mA */
#define PIANO_MCA_PACK_VOLTAGE 0x2026u
#define PIANO_MCA_PACK_CURRENT 0x2027u
#define PIANO_MCA_TBAT 0x202au            /* battery pack temperature, degC */
#define PIANO_MCA_BUS_VOLTAGE 0x20002u    /* uV */
#define PIANO_MCA_BUS_CURRENT 0x20004u    /* uA */
#define PIANO_MCA_REAL_TYPE 0x2000cu      /* the charger type the firmware detected (PIANO_MCA_TYPE_*) */
#define PIANO_MCA_VERIFY 0x21002u         /* non-zero while a charger authentication runs */
#define PIANO_MCA_PDOS 0x21011u           /* up to 7 x {min mV, max mV, max mA} */
#define PIANO_MCA_PD_FIXED 0x21018u       /* ask the source for a fixed PDO, mV */

enum {
    PIANO_MCA_TYPE_UNKNOWN = 0, PIANO_MCA_TYPE_SDP = 1, PIANO_MCA_TYPE_CDP = 2, PIANO_MCA_TYPE_DCP = 3, PIANO_MCA_TYPE_FLOAT = 4,
    PIANO_MCA_TYPE_HVDCP = 5, PIANO_MCA_TYPE_HVDCP_3 = 6, PIANO_MCA_TYPE_HVDCP_3_B = 7, PIANO_MCA_TYPE_HVDCP_3P5 = 8,
    PIANO_MCA_TYPE_C = 9, PIANO_MCA_TYPE_PD = 10, PIANO_MCA_TYPE_PD_VERIFY = 11, PIANO_MCA_TYPE_PD_PPS = 12, PIANO_MCA_TYPE_ACA = 14,
    PIANO_MCA_TYPE_DCP_B = 15,
};

/* The policy's bounds (mA, mV). */
#define PIANO_MCA_ICL_FLOOR 100u      /* the firmware's own power-on value; put back on detach */
#define PIANO_MCA_ICL_MAX_5V 1500u
#define PIANO_MCA_ICL_MAX_9V 1600u    /* stock 9 V / 1.6 A on a plain PD source */
#define PIANO_MCA_FCC_FLOOR 500u      /* about what the firmware charges with on its own */
#define PIANO_MCA_FCC_5V 1000u
#define PIANO_MCA_FCC_9V 1500u
#define PIANO_MCA_TBAT_LOW 15         /* below: the floor */
#define PIANO_MCA_TBAT_HIGH 38        /* from: the floor (the firmware's own JEITA window, 0-55 degC, still applies) */

/* The fields one cycle reads, in order. */
enum {
    McaBusVoltage, McaBusCurrent, McaType, McaIcl, McaTbat, McaChargeEnable, McaStatus, McaPackVoltage, McaPackCurrent, McaVerify,
    McaPdos, McaFields
};

/* receive results */
enum {McaIgnored = 0, McaAnswer = 1, McaCycleDone = 2, McaNotified = 3, McaWritten = 4, McaPanNotified = 5, McaPanDone = 6};
/* events for the owner's log (piano_mca_take_events) */
enum {
    McaEvAttach = 1u << 0, McaEvDetach = 1u << 1, McaEvWrite = 1u << 2, McaEvHvOn = 1u << 3, McaEvHvBlocked = 1u << 4,
    McaEvRefused = 1u << 5, McaEvHvAsked = 1u << 6,
};
enum {McaHvOff = 0, McaHvAsked = 1, McaHvOn = 2, McaHvBlocked = 3};

typedef struct {
    int32_t value[McaFields];
    uint32_t valid;                  /* bit per field */
    uint32_t pdo[7][3];              /* min mV, max mV, max mA */
    uint8_t pdos;
    /* the cycle */
    uint8_t cursor, in_flight, refresh_wanted;
    uint32_t seq, sent_property, sent_opcode;
    /* the writes the policy queued, sent between cycles; sent_write: one of them is in flight */
    struct {uint32_t property, value; uint8_t bytes;} queue[4];
    uint8_t queued, sent_write;
    /* the policy: 0 reads only, 1 writes at 5 V; hv_allowed adds the 9 V request */
    uint8_t policy, hv_allowed, attached, hv, hv_checks, attach_cycles;
    uint8_t icl_tries, fcc_tries, enable_tries;
    uint32_t icl_want, fcc_want;     /* what the last cycle wanted (tries restart when it changes) */
    uint32_t icl_set, fcc_set;       /* what the firmware took since its start (0: nothing yet) */
    int32_t ext_tbat_c;              /* the gauges' temperature, from the owner */
    uint8_t ext_tbat_valid;
    /* USB-C port notifications */
    uint8_t pan_enable, pan_pending, pan_in_flight;
    /* counters and the last write, for the log */
    uint32_t cycles, answers, errors, timeouts, ignored, notifications, writes_ok, writes_failed, refused, pan_notifies, pan_acks;
    uint32_t links;
    int32_t last_result;
    uint32_t last_property, last_value;
    uint32_t events;
} PianoMca;

void piano_mca_init(PianoMca *m, unsigned policy, int hv_allowed);
/* The channel came up (a started firmware is on its own defaults: everything is set again) or went down. */
void piano_mca_link(PianoMca *m, int up);
/* Starts a cycle (remembered when one runs). */
void piano_mca_refresh(PianoMca *m);
/* The next charger request (a due write between cycles, else the cycle's next read); 0 when nothing is to be sent now. */
uint32_t piano_mca_next(PianoMca *m, uint8_t out[PIANO_MCA_REQUEST_BYTES]);
/* The next port request (enable, then acknowledgements); 0 when none. */
uint32_t piano_mca_pan_next(PianoMca *m, uint8_t out[PIANO_MCA_PAN_REQUEST_BYTES]);
/* A message from the channel (either owner); returns a Mca* result. */
int piano_mca_receive(PianoMca *m, const uint8_t *data, uint32_t length);
/* The owner's timer gave up on the request (or the port request) in flight. */
void piano_mca_timeout(PianoMca *m);
void piano_mca_pan_timeout(PianoMca *m);
/* The gauges' temperature, used when the firmware's own reading is missing. */
void piano_mca_battery_temperature(PianoMca *m, int valid, int32_t celsius);
uint32_t piano_mca_take_events(PianoMca *m);

static inline int piano_mca_has(const PianoMca *m, int field) {return (int)((m->valid >> field) & 1u);}

/* The policy's pieces, exposed for the host tests. */
uint32_t piano_mca_icl_for(int type, uint32_t pdo5v_ma);
uint32_t piano_mca_fcc_for(int tbat_valid, int32_t tbat_c, int hv);
int piano_mca_write_allowed(uint32_t property, uint32_t value, uint8_t bytes);
/* The max current of the source's fixed PDO at mv, 0 when it has none. */
uint32_t piano_mca_pdo_fixed(const PianoMca *m, uint32_t mv);
const char *piano_mca_type_name(int type);

#ifdef __cplusplus
}
#endif
#endif
