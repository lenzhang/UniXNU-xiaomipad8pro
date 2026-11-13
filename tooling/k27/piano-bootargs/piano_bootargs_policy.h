/*
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * k27 0066's string handling (UniXNU-piano tooling/k27/piano-bootargs/piano_bootargs_policy.h, host-tested by
 * tooling/k27/test-piano-bootargs.sh; the patch adds this file byte for byte as pexpert/arm/piano_bootargs_policy.h).
 *
 * DarwinBoot passes at most 1023 bytes of command line (boot_args.CommandLine, BOOT_LINE_LENGTH 1024). The rest of the arguments
 * lives in the ADT: /chosen/piano-boot-args, or /chosen/piano-boot-args-NAME when the line says piano-boot-args=NAME
 * (piano-boot-args=0: none). The kernel appends that string to a copy of the line, PE_boot_args() returns the copy, and every
 * PE_parse_boot_argn() sees both. Pure functions: no kernel or libc calls, so the host test compiles this same text.
 */
#ifndef PIANO_BOOTARGS_POLICY_H
#define PIANO_BOOTARGS_POLICY_H

#include <stddef.h>

#define PIANO_BOOT_ARGS_SIZE      4096   /* the merged line's buffer: the line, a space, the tail, the NUL */
#define PIANO_BOOT_ARGS_PROP      "piano-boot-args"   /* the /chosen property and the line's selector */
#define PIANO_BOOT_ARGS_SET_MAX   15     /* "piano-boot-args-" + 15 = 31 characters, an ADT property name's limit */
#define PIANO_BOOT_ARGS_NAME_SIZE 32

enum {
	PIANO_BA_NONE = 0,   /* no selector and no /chosen/piano-boot-args: nothing appended, nothing logged (the boot of before) */
	PIANO_BA_APPENDED,   /* the tail is appended */
	PIANO_BA_OFF,        /* piano-boot-args=0 on the line */
	PIANO_BA_BAD_SET,    /* the selector is not 1-15 of A-Z a-z 0-9 . _ - */
	PIANO_BA_MISSING,    /* the selector names a set the ADT lacks */
	PIANO_BA_EMPTY,      /* the property holds no argument */
	PIANO_BA_BAD_CHAR,   /* a byte outside printable ASCII (0x20-0x7e) before the property's NUL */
	PIANO_BA_TOO_LONG,   /* the line, a space and the tail do not fit PIANO_BOOT_ARGS_SIZE */
};

static inline int
piano_bootargs_set_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

/*
 * The property a boot takes. SELECTOR: the line's piano-boot-args value, NULL when the line has none. Returns 0 with the property
 * name in NAME (PIANO_BOOT_ARGS_NAME_SIZE bytes), or PIANO_BA_OFF / PIANO_BA_BAD_SET with NAME empty.
 */
static inline int
piano_bootargs_property(const char *selector, char *name)
{
	static const char prefix[] = PIANO_BOOT_ARGS_PROP;
	size_t i, n;

	name[0] = '\0';
	if (selector != NULL && selector[0] == '0' && selector[1] == '\0') {
		return PIANO_BA_OFF;
	}
	if (selector != NULL) {
		for (n = 0; selector[n] != '\0'; n++) {
			if (n >= PIANO_BOOT_ARGS_SET_MAX || !piano_bootargs_set_char(selector[n])) {
				return PIANO_BA_BAD_SET;
			}
		}
		if (n == 0) {
			return PIANO_BA_BAD_SET;
		}
	}
	for (i = 0; prefix[i] != '\0'; i++) {
		name[i] = prefix[i];
	}
	if (selector != NULL) {
		name[i++] = '-';
		for (n = 0; selector[n] != '\0'; n++) {
			name[i++] = selector[n];
		}
	}
	name[i] = '\0';
	return 0;
}

/*
 * OUT (OUT_SIZE bytes, not LINE's storage) = LINE + ' ' + the arguments of the property VALUE (VALUE_SIZE bytes, read up to its
 * first NUL, leading and trailing spaces dropped). LINE is read up to its NUL, at most LINE_MAX - 1 bytes. Returns PIANO_BA_APPENDED
 * with *TAIL_LEN and *TOTAL_LEN (OUT's strlen); else PIANO_BA_EMPTY, PIANO_BA_BAD_CHAR or PIANO_BA_TOO_LONG with OUT untouched,
 * *TAIL_LEN the tail's length when it was measured and *TOTAL_LEN the length the merge would have had.
 */
static inline int
piano_bootargs_merge(char *out, size_t out_size, const char *line, size_t line_max, const unsigned char *value, size_t value_size,
    size_t *tail_len, size_t *total_len)
{
	size_t line_len = 0, start = 0, end = 0, i, at = 0;

	*tail_len = 0;
	*total_len = 0;
	while (line_len + 1 < line_max && line[line_len] != '\0') {
		line_len++;
	}
	while (end < value_size && value[end] != 0) {
		end++;
	}
	for (i = 0; i < end; i++) {
		if (value[i] < 0x20 || value[i] > 0x7e) {
			return PIANO_BA_BAD_CHAR;
		}
	}
	while (start < end && value[start] == ' ') {
		start++;
	}
	while (end > start && value[end - 1] == ' ') {
		end--;
	}
	*tail_len = end - start;
	if (*tail_len == 0) {
		return PIANO_BA_EMPTY;
	}
	*total_len = line_len + (line_len ? 1 : 0) + *tail_len;
	if (out_size == 0 || *total_len > out_size - 1) {
		return PIANO_BA_TOO_LONG;
	}
	for (i = 0; i < line_len; i++) {
		out[at++] = line[i];
	}
	if (line_len) {
		out[at++] = ' ';
	}
	for (i = start; i < end; i++) {
		out[at++] = (char)value[i];
	}
	out[at] = '\0';
	return PIANO_BA_APPENDED;
}

/*
 * kern.bootargs keeps its BOOT_LINE_LENGTH answer (userland reads it into fixed buffers): DST (SIZE bytes) gets SRC, a merged line
 * longer than SIZE - 1 cut after its last whole argument. Returns DST's strlen.
 */
static inline size_t
piano_bootargs_clip(char *dst, size_t size, const char *src)
{
	size_t n = 0;

	if (size == 0) {
		return 0;
	}
	while (n + 1 < size && src[n] != '\0') {
		dst[n] = src[n];
		n++;
	}
	if (src[n] != '\0' && src[n] != ' ') {
		while (n > 0 && dst[n - 1] != ' ') {
			n--;
		}
	}
	while (n > 0 && dst[n - 1] == ' ') {
		n--;
	}
	dst[n] = '\0';
	return n;
}

#endif /* PIANO_BOOTARGS_POLICY_H */
