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
 * current, the charger's enable bit, the fixed PDO request, the opt-in input un-suspend and the stock board configuration, each within
 * conservative bounds or to its one value (piano_mca_write_allowed); anything else is refused before it is built. The UCSI owner is
 * only read (piano_mca_ucsi_next). No I/O and no waits: the owner sends the bytes and feeds back what arrives. */
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
/* The USB-C connector interface (UCSI) the same firmware serves under its own owner (docs/ubuntu-history/kernel.md §2.5): a read of
 * the PPM's buffer is the 12-byte header alone (opcode 0x11); the answer is the header, the buffer (48 bytes for UCSI 1.x, larger for
 * 2.x: VERSION at 0, CCI at 4, CONTROL at 8, MESSAGE_IN at 16) and a return code. Notifications: opcode 0x13. Read only here: it tells
 * whether the owner is served at all (Linux runs a full UCSI client on every charger_pd "up"; we send no UCSI command). */
#define PIANO_MCA_OWNER_UCSI 0x800bu
#define PIANO_MCA_UCSI_READ 0x11u
#define PIANO_MCA_UCSI_NOTIFY 0x13u
#define PIANO_MCA_UCSI_REQUEST_BYTES 12u

/* Properties; bits 31:16 are the class (0 charger and battery, 2 USB and PD). The PD ids are the OS3 firmware's (HyperOS 4 firmware
 * inserts one at 0x2100b and moves the later ones up by one). */
#define PIANO_MCA_STATUS 0x2001u
/* The input path, read only here (§5c): the charger firmware can suspend the USB input independently of the input current limit and
 * the charger's enable bit, and its automatic input-current limiting (AICL) walks the limit down when the source sags. Ids from the
 * Linux driver's property family (Project-SunUEFI piano_mca.c, spec only) and the K90 port's notes; what they read on piano is what
 * the diagnostic answers. */
#define PIANO_MCA_INPUT_SUSPEND 0x2019u   /* 1 byte: 1 = the USB input is suspended */
#define PIANO_MCA_AICL_ENABLE 0x2012u     /* 1 byte */
#define PIANO_MCA_AICL_STATUS 0x2029u     /* 4 bytes */
/* The firmware's own view of the port and the float voltage, read for the diag line (docs/ubuntu-history/device.md §3.1). */
#define PIANO_MCA_OTG_BOOST 0x200du       /* OTG boost status */
#define PIANO_MCA_OTG_GATE 0x200eu        /* OTG gate status */
#define PIANO_MCA_TYPEC_MODE 0x21015u
#define PIANO_MCA_SNK_SRC 0x21022u        /* sink/source mode */
#define PIANO_MCA_USB_TYPE 0x20001u
#define PIANO_MCA_TERM_VOLT 0x2007u       /* reads 2 x mV - 3600 (the default 4400 mV reads 5200) */
#define PIANO_MCA_OTG_PLUGIN 0x21020u
#define PIANO_MCA_CC_TOGGLE 0x21021u      /* 1 byte */
#define PIANO_MCA_DATA_ROLE 0x2100cu      /* 1 UFP, 2 DFP (a write swaps: never written here) */
#define PIANO_MCA_PD_STATE 0x2100du       /* 2 = SNK_Ready */
/* The stock board configuration (Linux sends it on every charger generation before any policy, as the stock subpmic driver does):
 * charge pump present (1 byte, 1), the OTG configuration (16 bytes: words 0, 1, 0xffffffff, 0xffffffff: the charger boost and the
 * PMIC GPIO controller, no external GPIOs) and the cable-insertion detection identifier (u32 0x708). Written only these values. */
#define PIANO_MCA_CP_STATE 0x201cu
#define PIANO_MCA_OTG_CFG 0x201au
#define PIANO_MCA_CID_CFG 0x201bu
#define PIANO_MCA_CID_VALUE 0x708u
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
    McaBusVoltage, McaBusCurrent, McaType, McaIcl, McaTbat, McaChargeEnable, McaStatus, McaPackVoltage, McaPackCurrent,
    McaInputSuspend, McaAiclEnable, McaAiclStatus, McaOtgBoost, McaOtgGate, McaTypecMode, McaSnkSrc, McaUsbType, McaTermVolt,
    McaCpState, McaOtgCfg, McaCidCfg, McaOtgPlugin, McaCcToggle, McaDataRole, McaPdState, McaVerify, McaPdos, McaFields
};

/* receive results */
enum {McaIgnored = 0, McaAnswer = 1, McaCycleDone = 2, McaNotified = 3, McaWritten = 4, McaPanNotified = 5, McaPanDone = 6,
      McaUcsiDone = 7, McaUcsiNotified = 8};
/* events for the owner's log (piano_mca_take_events) */
enum {
    McaEvAttach = 1u << 0, McaEvDetach = 1u << 1, McaEvWrite = 1u << 2, McaEvHvOn = 1u << 3, McaEvHvBlocked = 1u << 4,
    McaEvRefused = 1u << 5, McaEvHvAsked = 1u << 6, McaEvBoardDone = 1u << 7, McaEvBoardGaveUp = 1u << 8,
    McaEvPanEnabled = 1u << 9, McaEvPanSilent = 1u << 10, McaEvUcsiSilent = 1u << 11,
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
    uint8_t unsuspend_allowed, unsuspend_tries;
    uint8_t board_allowed, board_done, board_tries;   /* the board configuration: bit 0 CP state, 1 OTG, 2 CID */
    uint8_t icl_tries, fcc_tries, enable_tries;
    uint32_t icl_want, fcc_want;     /* what the last cycle wanted (tries restart when it changes) */
    uint32_t icl_set, fcc_set;       /* what the firmware took since its start (0: nothing yet) */
    int32_t ext_tbat_c;              /* the gauges' temperature, from the owner */
    uint8_t ext_tbat_valid;
    /* USB-C port notifications; pan_cmd: the command in flight; pan_enabled: 0 not answered yet, 1 answered, 2 no answer */
    uint8_t pan_enable, pan_pending, pan_in_flight, pan_cmd, pan_enabled;
    /* the UCSI probe: ucsi 0 off, 1 due, 2 in flight, 3 answered, 4 no answer; what the buffer read showed */
    uint8_t ucsi_allowed, ucsi;
    uint16_t ucsi_version;
    uint32_t ucsi_cci, ucsi_ret, ucsi_bytes, ucsi_notifies;
    /* counters and the last write, for the log */
    uint32_t cycles, answers, errors, timeouts, ignored, notifications, writes_ok, writes_failed, refused, pan_notifies, pan_acks;
    uint32_t links;
    uint8_t settled;                 /* the last cycle of this link ended with nothing left to write (piano_mca_configured) */
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
/* The UCSI buffer read (once per link, when allowed); 0 when none is due. */
uint32_t piano_mca_ucsi_next(PianoMca *m, uint8_t out[PIANO_MCA_UCSI_REQUEST_BYTES]);
void piano_mca_ucsi_timeout(PianoMca *m);
void piano_mca_allow_ucsi(PianoMca *m, int allow);
/* A message from the channel (any of the three owners); returns a Mca* result. */
int piano_mca_receive(PianoMca *m, const uint8_t *data, uint32_t length);
/* The owner's timer gave up on the request (or the port request) in flight. */
void piano_mca_timeout(PianoMca *m);
void piano_mca_pan_timeout(PianoMca *m);
/* Allows the one write of the input-suspend flag: 0x2019 = 0 when it reads non-zero, at most three times per attach, never 1
 * (piano-power-unsuspend=1; off by default, and it needs the policy on). */
void piano_mca_allow_unsuspend(PianoMca *m, int allow);
/* Allows the stock board configuration (PIANO_MCA_CP_STATE, _OTG_CFG, _CID_CFG with their one value each), sent once per charger
 * generation before any policy write and retried for five cycles; needs the policy on. */
void piano_mca_allow_board(PianoMca *m, int allow);
/* The gauges' temperature, used when the firmware's own reading is missing. */
void piano_mca_battery_temperature(PianoMca *m, int valid, int32_t celsius);
uint32_t piano_mca_take_events(PianoMca *m);
/* The link's charger generation is configured: a whole cycle ended with no write due or in flight (so the board configuration and the
 * policy's writes are through, or given up) and the port enable was answered or timed out. PianoPower's quiet flag waits for it. */
int piano_mca_configured(const PianoMca *m);
/* The name of a charger status value, "unknown" when it is not one of the five the Linux driver and the K90 notes name
 * (1 charging, 2 discharging, 3 not charging, 4/5 full; the 07:28 run read 0x7). */
const char *piano_mca_status_name(int32_t status);

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
