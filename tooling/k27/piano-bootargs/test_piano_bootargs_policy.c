// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of k27 0066's string handling (piano_bootargs_policy.h): the property a selector names, the merge of the line and
// the ADT's tail, and kern.bootargs' clip. tooling/k27/test-piano-bootargs.sh builds and runs it.
#include <stdio.h>
#include <string.h>

#include "piano_bootargs_policy.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int
merge(char *out, size_t out_size, const char *line, const char *value, size_t value_size, size_t *tail, size_t *total)
{
	return piano_bootargs_merge(out, out_size, line, 1024, (const unsigned char *)value, value_size, tail, total);
}

static void
test_property(void)
{
	char name[PIANO_BOOT_ARGS_NAME_SIZE];

	CHECK(piano_bootargs_property(NULL, name) == 0 && strcmp(name, "piano-boot-args") == 0);
	CHECK(piano_bootargs_property("ram", name) == 0 && strcmp(name, "piano-boot-args-ram") == 0);
	CHECK(piano_bootargs_property("macos-27.3", name) == 0 && strcmp(name, "piano-boot-args-macos-27.3") == 0);
	CHECK(piano_bootargs_property("A_b.9-z", name) == 0 && strcmp(name, "piano-boot-args-A_b.9-z") == 0);
	// 15 characters is the longest: "piano-boot-args-" + 15 = 31, an ADT property name's limit
	CHECK(piano_bootargs_property("abcdefghijklmno", name) == 0 && strlen(name) == 31);
	CHECK(piano_bootargs_property("abcdefghijklmnop", name) == PIANO_BA_BAD_SET && name[0] == '\0');
	CHECK(piano_bootargs_property("0", name) == PIANO_BA_OFF && name[0] == '\0');
	CHECK(piano_bootargs_property("00", name) == 0 && strcmp(name, "piano-boot-args-00") == 0);
	CHECK(piano_bootargs_property("", name) == PIANO_BA_BAD_SET);
	CHECK(piano_bootargs_property("a b", name) == PIANO_BA_BAD_SET);
	CHECK(piano_bootargs_property("a/b", name) == PIANO_BA_BAD_SET);
	CHECK(piano_bootargs_property("a=b", name) == PIANO_BA_BAD_SET);
}

static void
test_merge(void)
{
	char out[PIANO_BOOT_ARGS_SIZE];
	size_t tail, total;
	static const char value[] = "piano-audio=1 piano-camera=5";

	// the plain case: line, one space, the property's string; its NUL ends it
	memset(out, 'x', sizeof(out));
	CHECK(merge(out, sizeof(out), "debug=0x14e -v", value, sizeof(value), &tail, &total) == PIANO_BA_APPENDED);
	CHECK(strcmp(out, "debug=0x14e -v piano-audio=1 piano-camera=5") == 0);
	CHECK(tail == strlen(value) && total == strlen(out));
	// outer spaces of the property dropped, inner ones kept as they are
	CHECK(merge(out, sizeof(out), "a=1", "  b=2  c=3   ", 14, &tail, &total) == PIANO_BA_APPENDED);
	CHECK(strcmp(out, "a=1 b=2  c=3") == 0 && tail == 8);
	// a property without its NUL is read to its size; bytes after the first NUL are not read
	CHECK(merge(out, sizeof(out), "a=1", "b=2c=3", 3, &tail, &total) == PIANO_BA_APPENDED && strcmp(out, "a=1 b=2") == 0);
	CHECK(merge(out, sizeof(out), "a=1", "b=2\0\x01\x02", 6, &tail, &total) == PIANO_BA_APPENDED && strcmp(out, "a=1 b=2") == 0);
	// an empty line: no leading space
	CHECK(merge(out, sizeof(out), "", value, sizeof(value), &tail, &total) == PIANO_BA_APPENDED && strcmp(out, value) == 0);
	// nothing to append: out untouched
	strcpy(out, "untouched");
	CHECK(merge(out, sizeof(out), "a=1", "", 1, &tail, &total) == PIANO_BA_EMPTY && strcmp(out, "untouched") == 0);
	CHECK(merge(out, sizeof(out), "a=1", "    ", 5, &tail, &total) == PIANO_BA_EMPTY);
	CHECK(merge(out, sizeof(out), "a=1", "x", 0, &tail, &total) == PIANO_BA_EMPTY);
	// a byte outside printable ASCII before the NUL: refused whole
	CHECK(merge(out, sizeof(out), "a=1", "b=2\tc=3", 8, &tail, &total) == PIANO_BA_BAD_CHAR && strcmp(out, "untouched") == 0);
	CHECK(merge(out, sizeof(out), "a=1", "b=2\nc=3", 8, &tail, &total) == PIANO_BA_BAD_CHAR);
	CHECK(merge(out, sizeof(out), "a=1", "b=\x80", 4, &tail, &total) == PIANO_BA_BAD_CHAR);
	CHECK(merge(out, sizeof(out), "a=1", "b=\x7f", 4, &tail, &total) == PIANO_BA_BAD_CHAR);
	CHECK(merge(out, sizeof(out), "a=1", "~ !", 4, &tail, &total) == PIANO_BA_APPENDED && strcmp(out, "a=1 ~ !") == 0);
}

static void
test_limits(void)
{
	static char out[PIANO_BOOT_ARGS_SIZE], line[1024], value[PIANO_BOOT_ARGS_SIZE + 8];
	size_t tail, total;

	// DarwinBoot's longest line (1023 bytes) and the longest tail that still fits: 4095 - 1023 - 1 = 3071
	memset(line, 'l', 1023);
	line[1023] = '\0';
	memset(value, 'v', 3071);
	value[3071] = '\0';
	CHECK(merge(out, sizeof(out), line, value, 3072, &tail, &total) == PIANO_BA_APPENDED);
	CHECK(total == 4095 && strlen(out) == 4095 && out[1023] == ' ' && out[4094] == 'v' && out[4095] == '\0');
	// one byte more: refused whole, the would-be length reported
	value[3071] = 'v';
	value[3072] = '\0';
	memcpy(out, "untouched", 10);
	CHECK(merge(out, sizeof(out), line, value, 3073, &tail, &total) == PIANO_BA_TOO_LONG);
	CHECK(tail == 3072 && total == 4096 && strcmp(out, "untouched") == 0);
	// a line without its NUL in BOOT_LINE_LENGTH is read as its first 1023 bytes
	memset(line, 'l', sizeof(line));
	CHECK(merge(out, sizeof(out), line, "t", 2, &tail, &total) == PIANO_BA_APPENDED && total == 1025 && out[1023] == ' ');
	// an output too small for anything
	CHECK(merge(out, 0, "", "t", 2, &tail, &total) == PIANO_BA_TOO_LONG);
	CHECK(merge(out, 1, "", "t", 2, &tail, &total) == PIANO_BA_TOO_LONG);
	CHECK(merge(out, 2, "", "t", 2, &tail, &total) == PIANO_BA_APPENDED && strcmp(out, "t") == 0);
}

static void
test_clip(void)
{
	char buf[16];

	// a line that fits is copied whole
	CHECK(piano_bootargs_clip(buf, sizeof(buf), "a=1 b=2") == 7 && strcmp(buf, "a=1 b=2") == 0);
	// 15 bytes exactly fit
	CHECK(piano_bootargs_clip(buf, sizeof(buf), "a=1 b=2 c=33333") == 15 && strcmp(buf, "a=1 b=2 c=33333") == 0);
	// longer: cut after the last whole argument
	CHECK(piano_bootargs_clip(buf, sizeof(buf), "a=1 b=2 c=333333") == 7 && strcmp(buf, "a=1 b=2") == 0);
	// the cut falls on a space: the argument before it is whole
	CHECK(piano_bootargs_clip(buf, sizeof(buf), "a=1 b=2 c=33333 d=4") == 15 && strcmp(buf, "a=1 b=2 c=33333") == 0);
	CHECK(piano_bootargs_clip(buf, sizeof(buf), "a=1 b=2 c=3333  d=4") == 14 && strcmp(buf, "a=1 b=2 c=3333") == 0);
	// one argument longer than the answer: nothing
	CHECK(piano_bootargs_clip(buf, sizeof(buf), "aaaaaaaaaaaaaaaaaaaa") == 0 && buf[0] == '\0');
	CHECK(piano_bootargs_clip(buf, sizeof(buf), "") == 0 && buf[0] == '\0');
	CHECK(piano_bootargs_clip(buf, 1, "a=1") == 0 && buf[0] == '\0');
	CHECK(piano_bootargs_clip(buf, 0, "a=1") == 0);
}

int
main(void)
{
	test_property();
	test_merge();
	test_limits();
	test_clip();
	if (failures) {
		printf("test_piano_bootargs_policy: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_piano_bootargs_policy: all passed\n");
	return 0;
}
