/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The FourSemi FS19xx smart amplifier (piano: four of them, DEVID 0x2900, on the I2C hub's SE0 at 0x34-0x37, fed by the secondary
 * TDM): the register model, probe and identity, the vendor preset container (fs19xx.fsm, read at run time from the owner's tablet,
 * never committed: only its documented structure is parsed here), the power sequences, mute, the gain cap and the status reads.
 * New code for PianoAudio from facts: Linux's fs19xx driver (yzddmr6/linux-sm8750-piano, Project-SunUEFI 0005-audio.patch; GPL-2.0)
 * and Ubuntu's tools/speaker-playback-check.py were read as specifications only (docs/drivers/audio.md section 11).
 *
 * Speaker safety, enforced here whatever the caller asks (audio.md 10.2): the gain register never takes more than FS19XX_GAIN_CAP;
 * only fs19xx_start powers an amplifier up and only fs19xx_start / fs19xx_mute(.., 0) unmute it; every preset unit that would touch
 * the power, the DAC mute or the gain passes a filter that keeps the amplifier powered down, muted and capped; preset delays are capped.
 * Portable C: the bus transfers and the sleeps are the owner's callbacks; no allocation, no clock. */
#ifndef PIANO_FS19XX_H
#define PIANO_FS19XX_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Registers: 8-bit addresses, 16-bit values sent most significant byte first. */
#define FS19XX_REG_STATUS 0x00u
#define FS19XX_REG_DEVID 0x03u
#define FS19XX_REG_REVID 0x04u
#define FS19XX_REG_DIGSTAT 0x06u
#define FS19XX_REG_OTPACC 0x0bu
#define FS19XX_REG_CHIPINI 0x0eu
#define FS19XX_REG_PWRCTRL 0x10u
#define FS19XX_REG_SYSCTRL 0x11u
#define FS19XX_REG_SPKCOEF 0x14u
#define FS19XX_REG_VOLCTRL 0x16u
#define FS19XX_REG_I2SCTRL 0x17u
#define FS19XX_REG_DACCTRL 0x30u
#define FS19XX_REG_TSCTRL 0x4cu
#define FS19XX_REG_PLLCTRL1 0xa1u
#define FS19XX_REG_PLLCTRL2 0xa2u
#define FS19XX_REG_PLLCTRL3 0xa3u

#define FS19XX_PWRCTRL_PWDN 0x0001u        /* powered down */
#define FS19XX_PWRCTRL_I2CR 0x0002u        /* soft reset request (the write is not acknowledged) */
#define FS19XX_DIGSTAT_DACRUN 0x0002u
#define FS19XX_SYSCTRL_ON 0x00efu
#define FS19XX_DACCTRL_MUTE 0x0310u
#define FS19XX_DACCTRL_UNMUTE 0x0210u
#define FS19XX_DACCTRL_MUTE_BIT 0x0100u
#define FS19XX_I2SCTRL_SR_SHIFT 12u
#define FS19XX_I2SCTRL_SR_MASK 0xf000u
#define FS19XX_I2SCTRL_DOE 0x0800u
#define FS19XX_SR_48K 0x9u
#define FS19XX_TSCTRL_TSEN 0x0008u
#define FS19XX_OTPACC_KEY 0xca91u
#define FS19XX_DEVID 0x2900u               /* the identity register of every piano amplifier */
#define FS19XX_DEVID_FAMILY 0x29u          /* its high byte; others (FS1958 0x17) encode the gain differently: refused */
/* The PLL from the 6.144 MHz TDM bit clock (48 kHz x 4 slots x 32 bits), the only one piano runs. */
#define FS19XX_PLL1_6M144 0x0260u
#define FS19XX_PLL2_6M144 0x0540u
#define FS19XX_PLL3_6M144 0x0018u

/* Gain: VOLCTRL bits 15:8, 0.375 dB per step, 255 = 0 dB. No speaker protection runs, so the cap is a compile-time constant:
 * 192 = -23.6 dB, Ubuntu's UCM speaker gain; never above 215 (-15 dB, Ubuntu's first kernel cap). Unattended tests use 160 (-35.6 dB). */
#define FS19XX_GAIN_CAP 192u
#define FS19XX_GAIN_TEST 160u
#define FS19XX_GAIN_HARD_LIMIT 215u
#if FS19XX_GAIN_CAP > FS19XX_GAIN_HARD_LIMIT
#error "FS19XX_GAIN_CAP above 215: no speaker protection runs on piano"
#endif
unsigned fs19xx_gain_clamp(unsigned requested);
/* The gain in tenths of a dB (192: -236). */
int fs19xx_gain_db10(unsigned gain);

/* STATUS bit names (their polarity is not documented: compared, never judged alone). */
#define FS19XX_STATUS_BITS 0x4f7fu
const char *fs19xx_status_bit(unsigned bit);   /* "BOVDS", "PLLS", ... or null */

typedef struct {
    void *context;
    /* Writes out_len bytes to the 7-bit address, then, when in_len > 0, a repeated start and a read of in_len bytes. 0 or an error. */
    int (*transfer)(void *context, uint8_t address, const uint8_t *out, uint32_t out_len, uint8_t *in, uint32_t in_len);
    void (*sleep_us)(void *context, uint32_t us);
} Fs19xxBus;

enum {Fs19xxAbsent = 0, Fs19xxIdentified, Fs19xxReady, Fs19xxPlaying, Fs19xxFailed};
enum {Fs19xxOk = 0, Fs19xxErrBus = -1, Fs19xxErrId = -2, Fs19xxErrTimeout = -3, Fs19xxErrState = -4, Fs19xxErrArg = -5};

typedef struct {
    const Fs19xxBus *bus;
    uint8_t address;
    int state;
    uint16_t devid, revid;
    uint8_t gain;                 /* what fs19xx_start wrote (capped) */
    int muted;
    uint32_t reads, writes, bus_errors;
    uint32_t filtered;            /* preset writes the safety filter changed */
    uint32_t skipped;             /* preset units of an unknown kind */
    uint32_t waits_missed;        /* preset waits whose condition never came */
    uint32_t otp_unlocks;         /* preset writes of the OTP access key (logged) */
    int last_error;
} Fs19xx;

void fs19xx_init_amp(Fs19xx *a, const Fs19xxBus *bus, uint8_t address);
int fs19xx_read(Fs19xx *a, uint8_t reg, uint16_t *value);
int fs19xx_write(Fs19xx *a, uint8_t reg, uint16_t value);   /* raw (the filter applies to preset units and fs19xx_* sequences) */

/* The amplifier out of reset (its reset line released at least 10 ms ago, the owner's GPIO): DEVID polled every 5 ms for up to
 * 100 ms (the first power-up after a cold boot answers late), then REVID. Fs19xxOk with state Identified, Fs19xxErrTimeout (no answer),
 * Fs19xxErrId (another device). */
int fs19xx_probe(Fs19xx *a);

/* ---- the preset container (fs19xx.fsm): little endian; a 28-byte header (size at 22, CRC16 of bytes 26.. at 24, the count of
 * descriptors at 26), descriptors {offset, type} from 28; type 0 is an amplifier's block: a 40-byte head (version at 0, bit 15 marks a
 * valid block; its list count at 26; the I2C address at 30; the device type at 32, DEVID's high byte in its high byte), then {offset,
 * type} list entries (offsets from the end of the head) to lists of a 16-bit count and units: type 1 speaker info (16-bit words, word
 * 1 the temperature coefficient), 2 the common register table (unit, value), 3 the scene tables (scene mask, unit, value). A unit is
 * the register in bits 7:0, the field's first bit in 11:8 and its width - 1 in 15:12 (0, 15: the whole register); first bit 15 with a
 * width field of 1 is a delay of `value` ms, of 2 a wait for a field (expected value bits 7:0, first bit 11:8, width - 1 15:12). */
#define FSM_SCENE_MUSIC 0x0001u
enum {FsmOk = 0, FsmShort = -1, FsmBadSize = -2, FsmBadCrc = -3, FsmBadIndex = -4, FsmNoBlock = -5, FsmInvalid = -6,
      FsmWrongDevice = -7, FsmBadList = -8};
typedef struct {
    const uint8_t *common;        /* common_count units of 4 bytes */
    uint32_t common_count;
    const uint8_t *scenes;        /* scene_count units of 6 bytes */
    uint32_t scene_count;
    uint16_t tcoef;               /* speaker info word 1, 0 when absent */
    uint16_t version, type;
} Fs19xxPreset;
/* CRC-16 (polynomial 0x8005 reflected, initial value 0), the container's checksum. */
uint16_t fsm_crc16(const uint8_t *data, uint32_t length);
/* The container's header: FsmOk and the number of descriptors, or an error. */
int fsm_check(const uint8_t *data, uint32_t size, uint32_t *descriptors);
/* The block of the amplifier at address with identity devid (pointers into data, which must stay valid while used). */
int fsm_find(const uint8_t *data, uint32_t size, uint8_t address, uint16_t devid, Fs19xxPreset *out);
const char *fsm_error(int error);

/* One preset unit, through the safety filter: a PWRCTRL value keeps PWDN set, a DACCTRL value keeps the mute bit, a VOLCTRL gain is
 * capped; delays are capped at 100 ms, waits poll 35 times at 1 ms (a wait that never matches is counted, not an error). */
int fs19xx_unit(Fs19xx *a, uint16_t unit, uint16_t value);
/* The filter alone: the value that may be written to reg (*changed: 1 when it differs). */
uint16_t fs19xx_filter(uint8_t reg, uint16_t value, int *changed);

/* Configure an identified amplifier from its preset block, powered down: soft reset (until CHIPINI reads 0x0003 or 0x0300, 5 tries),
 * the speaker coefficient, the common table, the music scene, then shut down. State Ready. */
int fs19xx_configure(Fs19xx *a, const Fs19xxPreset *p);
/* Power up a Ready amplifier with the bit clock running: 48 kHz / 6.144 MHz only; the gain is capped. muted: leave the DAC muted
 * (fs19xx_mute(a, 0) later). State Playing; on an error the amplifier is shut down again and the state is Failed. */
int fs19xx_start(Fs19xx *a, uint32_t rate, uint32_t bit_clock, unsigned gain, int muted);
int fs19xx_mute(Fs19xx *a, int mute);
/* Mute, power down, 35 ms, all blocks off: every write is tried even after a failure. State Ready (Failed on a bus error). */
int fs19xx_shutdown(Fs19xx *a);

typedef struct {
    int ok;                       /* every read answered */
    uint16_t status, digstat, pwrctrl, sysctrl, volctrl, dacctrl;
} Fs19xxStatus;
int fs19xx_read_status(Fs19xx *a, Fs19xxStatus *s);
/* Playing as Ubuntu's check judges it: PWDN clear, SYSCTRL 0x00ef, DACRUN set; down: PWDN set. */
int fs19xx_status_running(const Fs19xxStatus *s);
int fs19xx_status_down(const Fs19xxStatus *s);
/* The amplifiers that look wrong while playing (bit per amplifier): a read failed, one is not running, its STATUS differs from the first
 * amplifier's, or from its own first reading of this stream (first: null on that first reading). The owner stops on any bit. */
unsigned fs19xx_group_check(const Fs19xxStatus *now, const Fs19xxStatus *first, unsigned count);
/* STATUS as names ("PLLS CLKS ..."), at most size bytes with the terminator. */
void fs19xx_status_names(uint16_t status, char *out, unsigned size);

#ifdef __cplusplus
}
#endif
#endif
