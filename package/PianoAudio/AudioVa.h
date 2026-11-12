/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Level 10: the LPASS VA macro's DMIC path for piano's two microphones, as register values and their order; the owner supplies the
 * loads, stores and sleeps (AudioBoard.cpp: the macro's first page at 0x07660000 and the LPI pins at 0x07760000 + n * 0x1000, mapped
 * uncached). Called only while the DSP's PRM holds the macro and dcodec votes and the VA mclk (TX core mclk, 19.2 MHz): both the
 * macro and the LPI TLMM are clocked by them, and an access to an unclocked block may stall the bus.
 *   on   the LPI DMIC pins (gpio6 clock / gpio7 data: the VA DMIC0/1 pair, "dmic1" in the LPI pin table; gpio8 / gpio9: DMIC2/3,
 *        "dmic2"): function 1, 4 mA, the clocks driven; the macro's clock (MCLK enable, the FS counter enabled and cleared, the FS
 *        broadcast); both decimators at 48 kHz; DEC0 <- DMIC0, DEC1 <- DMIC2 (the input muxes and each path's DMIC select); the
 *        decimators' gain; each decimator powered up as Linux's VA macro does (its clock, the high-pass zero gate, the 150 Hz cut-off,
 *        about 9 ms); then the DMIC clocks of both pairs at 9.6 MHz / 4 = 2.4 MHz (the DT's qcom,dmic-sample-rate), the DMIC reset
 *        released; last the decimators unmuted
 *   off  the reverse: both decimators muted, the DMIC clocks stopped, the decimators' clocks off, the input muxes to zero, the
 *        macro's clock off; the LPI pins as found
 * Facts (offsets, fields, values, the order) from Linux's lpass-va-macro.c, pinctrl-lpass-lpi.{c,h} and pinctrl-sm8650-lpass-lpi.c
 * (the DT's LPI compatible falls back to sm8650's) and the ASoC power sequence, read as a specification only (GPL-2.0); the piano
 * DT (the VA macro and LPI nodes, their pin states); Ubuntu's UCM (DEC0 <- DMIC0 side top, DEC1 <- DMIC2 side bottom, gain 124).
 * Portable C, new code for PianoAudio; host-tested against a register model in tests/va_host.c. */
#ifndef PIANO_AUDIO_VA_H
#define PIANO_AUDIO_VA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The VA macro's registers (offsets from 0x07660000, all in its first page). */
#define VA_MCLK_CONTROL 0x000u            /* bit 0: MCLK enable */
#define VA_FS_CNT_CONTROL 0x004u          /* bit 0: FS counter enable, bit 1: clear */
#define VA_TOP_CFG0 0x080u                /* bit 1: FS broadcast enable */
#define VA_DMIC01_CTL 0x084u              /* the DMIC0/1 pair: bit 0 clock enable, bits 3:1 the divider code (2: / 4) */
#define VA_DMIC23_CTL 0x088u              /* the DMIC2/3 pair */
#define VA_DMIC_CFG 0x094u                /* bit 7: all DMICs held in reset (its reset value 0x80) */
#define VA_ADC_MUX0_CFG0 0x100u           /* DEC0's DMIC: bits 7:4 = n + 1 for DMIC n, 0 none */
#define VA_ADC_MUX0_CFG1 0x104u           /* DEC0's source: bit 0 = 0 the DMICs (1: SoundWire) */
#define VA_ADC_MUX1_CFG0 0x108u
#define VA_ADC_MUX1_CFG1 0x10cu
#define VA_TX_CTL(d) (0x400u + 0x80u * (d))      /* bits 3:0 the rate (4: 48 kHz), bit 4 mute, bit 5 the decimator's clock */
#define VA_TX_CFG0(d) (0x404u + 0x80u * (d))     /* bit 7 DMIC select, bits 6:5 the high-pass cut-off (2: 150 Hz), bits 2:1 ADC mode */
#define VA_TX_VOL(d) (0x40cu + 0x80u * (d))      /* the decimator's gain: signed dB (0 = 0 dB) */
#define VA_TX_GATE(d) (0x418u + 0x80u * (d))     /* bit 0 the high-pass zero gate off, bit 1 a cut-off change request */
#define VA_DMIC_DIV4 2u                   /* 9.6 MHz / 4 = 2.4 MHz */
#define VA_RATE_48K 4u
#define VA_HPF_150HZ 2u

/* The decimators' gain as Ubuntu's UCM gives it: a control of 0..124, 84 = 0 dB, 1 dB a step (124: +40 dB). */
#define AUDIO_VA_GAIN_DEFAULT 124u
#define AUDIO_VA_GAIN_MAX 124u
uint8_t audio_va_gain_register(unsigned gain);

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t offset);
    void (*write)(void *ctx, uint32_t offset, uint32_t value);
    void (*sleep_us)(void *ctx, uint32_t us);
} AudioVaIo;
/* The macro's DMIC path on, with the decimators' gain (capped at AUDIO_VA_GAIN_MAX). 0, or -1 when the macro does not take its
 * clock (MCLK enable does not read back): then nothing after it was written and the clock is off again. */
int audio_va_on(const AudioVaIo *io, unsigned gain);
void audio_va_off(const AudioVaIo *io);

/* The LPI DMIC pins: CFG (pull bits 1:0, function 5:2, drive (mA / 2 - 1) 8:6, output enable 9) and VALUE (bit 1 the output). */
#define AUDIO_VA_PINS 4u
typedef struct {
    unsigned gpio;                        /* the LPI TLMM's pin */
    const char *name;
    uint32_t cfg;
    uint32_t value;                       /* written before cfg when `drive` */
    int drive;
} AudioVaPin;
/* The four pins' values while the path is on: gpio6 dmic1 clock, gpio7 dmic1 data, gpio8 dmic2 clock, gpio9 dmic2 data. */
const AudioVaPin *audio_va_pins(void);

#ifdef __cplusplus
}
#endif
#endif
