// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano's PMIC RTC (docs/drivers/rtc.md): the counter, its offset and the date, shared by PianoPower's readout and the k27 kernel's
// calendar (patches/k27/0060 adds this file, unchanged, as osfmk/arm64/piano_rtc.h). Plain C, no allocation, no I/O of its own.
//
// The RTC of the PMK8550 PMIC (SID 0, DT pmic@0 rtc@6100, "qcom,pmk8350-rtc") is a seconds counter that runs on the battery. It is
// read only here: the boot DT has no allow-set-time, the counter is never written, and the calendar is the counter plus an offset
// kept elsewhere (Ubuntu's DT: the 4-byte "offset" cell at 0x72 of the SDAM at 0x7100; Android: persist/time/ats_2).
//   RTC_RW (PPID 0x061)  +0x46 EN_CTL, bit 7 = the counter runs; +0x48..+0x4b RDATA, the count, little endian
//   SDAM   (PPID 0x071)  +0x72..+0x75 the offset (UTC - count), little endian (+0x48 is the restart reason of k27 0057)
// The count is read as Linux's rtc-pm8xxx does (specification only): the four bytes, then the low byte again; a low byte smaller
// than the first one means a carry ran through the bytes during the read, and the four are read once more.
#ifndef PIANO_PMIC_RTC_H
#define PIANO_PMIC_RTC_H
#include <stdint.h>

#define PIANO_RTC_PPID 0x061u         /* SID 0, peripheral 0x61: RTC_RW */
#define PIANO_RTC_SDAM_PPID 0x071u    /* SID 0, peripheral 0x71: the SDAM with the offset cell */
#define PIANO_RTC_TYPE 0x04u          /* peripheral TYPE and SUBTYPE at +4, +5 (logged, not checked) */
#define PIANO_RTC_EN_CTL 0x46u
#define PIANO_RTC_ENABLED 0x80u
#define PIANO_RTC_RDATA 0x48u
#define PIANO_RTC_SDAM_OFFSET 0x72u
/* A sum outside [2026-01-01, 2100-01-01) is not taken as the date: a zero, erased or stale offset. */
#define PIANO_RTC_UTC_MIN 1767225600u
#define PIANO_RTC_UTC_MAX 4102444800u

/* Reads n (1..8) registers from reg of the peripheral ctx stands for; 0 when the read completed. */
typedef int (*piano_rtc_read_fn)(void *ctx, uint8_t reg, uint8_t *value, unsigned n);

static inline uint32_t piano_rtc_le32(const uint8_t b[4])
{
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* The count, consistent across a carry during the read. 0 on success. */
static inline int piano_rtc_read_count(piano_rtc_read_fn rd, void *ctx, uint32_t *count)
{
	uint8_t v[4], low = 0;
	if (rd(ctx, PIANO_RTC_RDATA, v, 4) != 0 || rd(ctx, PIANO_RTC_RDATA, &low, 1) != 0) {
		return -1;
	}
	if (low < v[0] && rd(ctx, PIANO_RTC_RDATA, v, 4) != 0) {
		return -1;
	}
	*count = piano_rtc_le32(v);
	return 0;
}

/* UTC = count + offset, modulo 2^32 as the stock reader adds them (so an offset "below zero" works too). */
static inline uint32_t piano_rtc_utc(uint32_t count, uint32_t offset)
{
	return count + offset;
}

static inline int piano_rtc_plausible(uint32_t utc)
{
	return utc >= PIANO_RTC_UTC_MIN && utc < PIANO_RTC_UTC_MAX;
}

struct piano_rtc_date {
	uint32_t year, month, day, hour, minute, second;
};

/* Seconds since 1970-01-01 00:00:00 UTC as a proleptic Gregorian date. Days are counted from 0000-03-01, so that the leap day
 * is the last day of a year: 400-year eras of 146097 days, years of 365 days plus the leap days before them, months of
 * 153 days per 5. */
static inline void piano_rtc_civil(uint32_t utc, struct piano_rtc_date *d)
{
	const uint32_t days = utc / 86400u, rem = utc % 86400u;
	d->hour = rem / 3600u;
	d->minute = rem / 60u % 60u;
	d->second = rem % 60u;
	const uint32_t z = days + 719468u;   /* 1970-01-01 is day 719468 after 0000-03-01 */
	const uint32_t era = z / 146097u, doe = z - era * 146097u;
	const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
	const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
	const uint32_t mp = (5u * doy + 2u) / 153u;
	d->day = doy - (153u * mp + 2u) / 5u + 1u;
	d->month = mp < 10u ? mp + 3u : mp - 9u;
	d->year = era * 400u + yoe + (d->month <= 2u ? 1u : 0u);
}

#endif
