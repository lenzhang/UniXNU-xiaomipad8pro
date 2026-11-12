/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The VA macro's DMIC path: see AudioVa.h. */
#include "AudioVa.h"

static uint32_t rd(const AudioVaIo *io, uint32_t off) {return io->read(io->ctx, off);}
static void wr(const AudioVaIo *io, uint32_t off, uint32_t v) {io->write(io->ctx, off, v);}
static void field(const AudioVaIo *io, uint32_t off, uint32_t mask, uint32_t value) {wr(io, off, (rd(io, off) & ~mask) | (value & mask));}
static void sleep_ms(const AudioVaIo *io, uint32_t ms) {if (io->sleep_us) io->sleep_us(io->ctx, ms * 1000u);}

uint8_t audio_va_gain_register(unsigned gain)
{
    if (gain > AUDIO_VA_GAIN_MAX) gain = AUDIO_VA_GAIN_MAX;
    return (uint8_t)((int)gain - 84);
}

/* A decimator powered up: its clock, the high-pass zero gate on, the 150 Hz cut-off asked for (a change request around it), the
 * gate off again, then its gain written once more (the gain takes effect only on a running decimator). */
static void decimator_up(const AudioVaIo *io, unsigned d, uint8_t gain)
{
    field(io, VA_TX_CTL(d), 1u << 5, 1u << 5);
    field(io, VA_TX_GATE(d), 1u << 0, 0);
    sleep_ms(io, 1);
    if ((rd(io, VA_TX_CFG0(d)) >> 5 & 3u) != VA_HPF_150HZ) {
        field(io, VA_TX_CFG0(d), 3u << 5, VA_HPF_150HZ << 5);
        field(io, VA_TX_GATE(d), 1u << 1, 1u << 1);
        sleep_ms(io, 1);
        field(io, VA_TX_GATE(d), 1u << 1, 0);
    }
    sleep_ms(io, 1);
    field(io, VA_TX_GATE(d), 1u << 0, 1u << 0);
    sleep_ms(io, 6);
    wr(io, VA_TX_VOL(d), gain);
}

static void clock_off(const AudioVaIo *io)
{
    field(io, VA_MCLK_CONTROL, 1u, 0);
    field(io, VA_FS_CNT_CONTROL, 1u, 0);
    field(io, VA_TOP_CFG0, 1u << 1, 0);
}

int audio_va_on(const AudioVaIo *io, unsigned gain)
{
    const uint8_t g = audio_va_gain_register(gain);
    /* the macro's clock: MCLK, the FS counter (enabled and cleared, then enabled), the FS broadcast */
    field(io, VA_MCLK_CONTROL, 1u, 1u);
    if (!(rd(io, VA_MCLK_CONTROL) & 1u)) {clock_off(io); return -1;}
    field(io, VA_FS_CNT_CONTROL, 3u, 3u);
    field(io, VA_FS_CNT_CONTROL, 3u, 1u);
    field(io, VA_TOP_CFG0, 1u << 1, 1u << 1);
    /* both decimators at 48 kHz, fed by the DMICs: DEC0 <- DMIC0, DEC1 <- DMIC2, each path's DMIC select; their gain; ADC mode 0 */
    for (unsigned d = 0; d < 2; ++d) field(io, VA_TX_CTL(d), 0xfu, VA_RATE_48K);
    field(io, VA_ADC_MUX0_CFG1, 1u, 0);
    field(io, VA_ADC_MUX1_CFG1, 1u, 0);
    field(io, VA_ADC_MUX0_CFG0, 0xf0u, (0u + 1u) << 4);
    field(io, VA_ADC_MUX1_CFG0, 0xf0u, (2u + 1u) << 4);
    for (unsigned d = 0; d < 2; ++d) {
        field(io, VA_TX_CFG0(d), 1u << 7, 1u << 7);
        wr(io, VA_TX_VOL(d), g);
        field(io, VA_TX_CFG0(d), 3u << 1, 0);
    }
    for (unsigned d = 0; d < 2; ++d) decimator_up(io, d, g);
    /* the DMIC clocks of both pairs, after the decimators: the DMICs' reset released, 2.4 MHz, on */
    field(io, VA_DMIC_CFG, 1u << 7, 0);
    field(io, VA_DMIC01_CTL, 0xfu, VA_DMIC_DIV4 << 1 | 1u);
    field(io, VA_DMIC23_CTL, 0xfu, VA_DMIC_DIV4 << 1 | 1u);
    /* last: unmuted */
    for (unsigned d = 0; d < 2; ++d) field(io, VA_TX_CTL(d), 1u << 4, 0);
    return 0;
}

void audio_va_off(const AudioVaIo *io)
{
    for (unsigned d = 0; d < 2; ++d) field(io, VA_TX_CTL(d), 1u << 4, 1u << 4);
    field(io, VA_DMIC01_CTL, 1u, 0);
    field(io, VA_DMIC01_CTL, 0xeu, 0);
    field(io, VA_DMIC23_CTL, 1u, 0);
    field(io, VA_DMIC23_CTL, 0xeu, 0);
    for (unsigned d = 0; d < 2; ++d) field(io, VA_TX_CTL(d), 1u << 5, 0);
    field(io, VA_ADC_MUX0_CFG0, 0xf0u, 0);
    field(io, VA_ADC_MUX1_CFG0, 0xf0u, 0);
    clock_off(io);
}

/* CFG: function 1 (bits 5:2), 4 mA (bits 8:6 = 1), output enable (bit 9) for the clocks; no pull (the DT gives none). */
static const AudioVaPin kPins[AUDIO_VA_PINS] = {
    {6, "dmic1 clock (DMIC0/1)", 1u << 9 | 1u << 6 | 1u << 2, 1u << 1, 1},
    {7, "dmic1 data", 1u << 6 | 1u << 2, 0, 0},
    {8, "dmic2 clock (DMIC2/3)", 1u << 9 | 1u << 6 | 1u << 2, 1u << 1, 1},
    {9, "dmic2 data", 1u << 6 | 1u << 2, 0, 0},
};

const AudioVaPin *audio_va_pins(void) {return kPins;}
