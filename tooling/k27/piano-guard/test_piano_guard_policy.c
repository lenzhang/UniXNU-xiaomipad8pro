// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of k27 0063's piano_guard_policy.h (the boot guard's decisions without hardware): the watchdog's holders, the
// health decision and its tick schedule, the test argument, the milestone's name as p_comm keeps it, and machdep.piano_boot_guard.
// tooling/k27/test-piano-guard.sh builds and runs it and checks that the patch carries this header byte for byte.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "piano_guard_policy.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

#define S 1000000000ull         /* ns */

static void
test_holders(void)
{
	CHECK(piano_wdt_change(PIANO_WDT_GUARD, false) == 1, "the first holder enables it");
	CHECK(piano_wdt_change(PIANO_WDT_GUARD | PIANO_WDT_SLEEP, true) == 0, "a second holder leaves it");
	CHECK(piano_wdt_change(PIANO_WDT_GUARD, true) == 0, "0062's release with the guard holding: it stays on");
	CHECK(piano_wdt_change(0, true) == -1, "the last holder out disables it");
	CHECK(piano_wdt_change(0, false) == 0, "a release with nothing enabled: nothing");
	CHECK(piano_wdt_change(PIANO_WDT_SLEEP, false) == 1, "a hold after a failed enable tries again");
	CHECK(piano_wdt_change(PIANO_WDT_RESET_AFTER, true) == 0, "0056 keeps it");
}

static void
test_decide(void)
{
	const uint64_t dl = 180 * S;
	CHECK(piano_guard_decide(PIANO_GUARD_ARMED, 179 * S, dl, 31 * S, true) == PIANO_GUARD_WAIT, "before the deadline: wait");
	CHECK(piano_guard_decide(PIANO_GUARD_ARMED, 1, dl, 0, true) == PIANO_GUARD_WAIT, "the first tick: wait");
	CHECK(piano_guard_decide(PIANO_GUARD_ARMED, dl, dl, 31 * S, true) == PIANO_GUARD_GO_HEALTHY, "at the deadline with the milestone");
	CHECK(piano_guard_decide(PIANO_GUARD_ARMED, dl + 3, dl, 179 * S, true) == PIANO_GUARD_GO_HEALTHY, "a late tick, milestone just in");
	CHECK(piano_guard_decide(PIANO_GUARD_ARMED, dl, dl, 0, true) == PIANO_GUARD_GO_FAILED, "no milestone at the deadline: failed");
	CHECK(piano_guard_decide(PIANO_GUARD_ARMED, dl, dl, 0, false) == PIANO_GUARD_GO_HEALTHY, "no milestone needed: uptime alone");
	CHECK(piano_guard_decide(PIANO_GUARD_HEALTHY, 900 * S, dl, 31 * S, true) == PIANO_GUARD_WAIT, "healthy: nothing more to decide");
	CHECK(piano_guard_decide(PIANO_GUARD_HEALTHY, 900 * S, dl, 0, true) == PIANO_GUARD_WAIT, "healthy stays healthy");
	CHECK(piano_guard_decide(PIANO_GUARD_FAILED, 900 * S, dl, 0, true) == PIANO_GUARD_WAIT, "failed: decided once");
	CHECK(piano_guard_decide(PIANO_GUARD_DEFERRED, 900 * S, dl, 0, true) == PIANO_GUARD_WAIT, "deferred: never decides");
	CHECK(piano_guard_decide(PIANO_GUARD_OFF, 900 * S, dl, 0, true) == PIANO_GUARD_WAIT, "off: never decides");
}

static void
test_next(void)
{
	const uint64_t dl = 60 * S, p = 10 * S;
	CHECK(piano_guard_next(PIANO_GUARD_ARMED, 2 * S, dl, p) == 12 * S, "10 s later");
	CHECK(piano_guard_next(PIANO_GUARD_ARMED, 52 * S, dl, p) == dl, "the deadline before the next period: at the deadline");
	CHECK(piano_guard_next(PIANO_GUARD_ARMED, 50 * S, dl, p) == dl, "exactly one period before");
	CHECK(piano_guard_next(PIANO_GUARD_ARMED, dl, dl, p) == dl + p, "at the deadline: a period on (the decision ran)");
	CHECK(piano_guard_next(PIANO_GUARD_HEALTHY, 52 * S, dl, p) == 62 * S, "healthy: every period");
	/* The ticks of a 60 s guard from a first tick at 2.5 s: 12.5 ... 52.5, then 60 exactly. */
	uint64_t t = 2500000000ull;
	int ticks = 0;
	while (t < dl && ticks < 100) {
		t = piano_guard_next(PIANO_GUARD_ARMED, t, dl, p);
		ticks++;
	}
	CHECK(t == dl && ticks == 6, "the decision lands on the deadline (%d ticks, %llu)", ticks, (unsigned long long)t);
	/* Pinging every period never leaves 32 s between two pings while the ticks run on time. */
	uint64_t gap = 0, prev = 1 * S;
	for (int i = 0; i < 50; i++) {
		uint64_t next = piano_guard_next(i < 8 ? PIANO_GUARD_ARMED : PIANO_GUARD_HEALTHY, prev, dl, p);
		if (next - prev > gap) {
			gap = next - prev;
		}
		prev = next;
	}
	CHECK(gap <= p && gap < 32 * S, "the longest planned gap is a period (%llu ns)", (unsigned long long)gap);
}

static void
test_quiet(void)
{
	uint32_t last = 0;
	uint8_t quiet = 0;
	CHECK(!piano_guard_cpu_quiet(100, &last, &quiet, false, true) && last == 100 && quiet == 0, "the first look: a baseline");
	CHECK(!piano_guard_cpu_quiet(100, &last, &quiet, true, true) && quiet == 1, "one poke unanswered: not yet");
	CHECK(piano_guard_cpu_quiet(100, &last, &quiet, true, true) && quiet == 2, "two in a row: stuck");
	CHECK(!piano_guard_cpu_quiet(101, &last, &quiet, true, true) && quiet == 0, "an interrupt again: alive");
	CHECK(!piano_guard_cpu_quiet(101, &last, &quiet, true, true), "one quiet tick");
	CHECK(!piano_guard_cpu_quiet(101, &last, &quiet, false, true) && quiet == 0, "a tick whose poke did not go out does not count");
	CHECK(!piano_guard_cpu_quiet(101, &last, &quiet, true, true) && !piano_guard_cpu_quiet(102, &last, &quiet, true, true),
	    "quiet, then an interrupt: alive");
	quiet = 1;
	last = 7;
	CHECK(!piano_guard_cpu_quiet(7, &last, &quiet, true, false) && quiet == 0, "a CPU that is not up: never stuck");
	last = 0xffffffffu;
	quiet = 1;
	CHECK(!piano_guard_cpu_quiet(0, &last, &quiet, true, true), "the 32-bit count wrapping is an interrupt");
	quiet = 255;
	last = 5;
	CHECK(piano_guard_cpu_quiet(5, &last, &quiet, true, true) && quiet == 255, "the count of quiet ticks saturates");
	/* A parked CPU of a suspend-to-idle (0062) swallows the IPI by hand: one quiet tick across the suspend, alive after it. */
	last = 50;
	quiet = 0;
	CHECK(!piano_guard_cpu_quiet(50, &last, &quiet, true, true), "the tick after a wake");
	CHECK(!piano_guard_cpu_quiet(60, &last, &quiet, true, true) && quiet == 0, "the next one");
}

static void
test_parse(void)
{
	uint32_t n = 0;
	CHECK(piano_guard_parse_test("hang-at=40", &n) == 0 && n == 40, "hang-at=40");
	CHECK(piano_guard_parse_test("hang-at=1", &n) == 0 && n == 1, "hang-at=1");
	CHECK(piano_guard_parse_test("hang-at=86400", &n) == 0 && n == 86400, "a day");
	n = 7;
	CHECK(piano_guard_parse_test("hang-at=86401", &n) == -1 && n == 7, "over a day: refused, untouched");
	CHECK(piano_guard_parse_test("hang-at=0", &n) == -1, "0 refused");
	CHECK(piano_guard_parse_test("hang-at=", &n) == -1, "no digits");
	CHECK(piano_guard_parse_test("hang-at=4x", &n) == -1, "trailing garbage");
	CHECK(piano_guard_parse_test("hang-at=99999999999999999999", &n) == -1, "no overflow");
	CHECK(piano_guard_parse_test("hang=40", &n) == -1, "another key");
	CHECK(piano_guard_parse_test("", &n) == -1, "empty");
	CHECK(piano_guard_parse_test("hang-a", &n) == -1, "a prefix of the key");
}

static void
test_comm(void)
{
	CHECK(piano_guard_comm_is("launchd", "launchd"), "launchd");
	CHECK(!piano_guard_comm_is("launchd", "launch"), "a prefix is not the name");
	CHECK(!piano_guard_comm_is("launch", "launchd"), "nor the other way");
	CHECK(!piano_guard_comm_is("loginwindow", "launchd"), "another process");
	/* p_comm holds 16 characters of a longer name: the first 16 of the argument must match them. */
	CHECK(piano_guard_comm_is("com.apple.Window", "com.apple.WindowServer"), "16 characters of a long name");
	CHECK(piano_guard_comm_is("com.apple.Window", "com.apple.Window"), "the name as p_comm keeps it");
	CHECK(!piano_guard_comm_is("com.apple.Windo", "com.apple.WindowServer"), "15 do not");
	CHECK(piano_guard_exec_none("none") && piano_guard_exec_none(""), "none and empty: no milestone");
	CHECK(!piano_guard_exec_none("nonexistent") && !piano_guard_exec_none("non"), "only exactly none");
}

static struct piano_guard_view
view(int state)
{
	struct piano_guard_view v;
	memset(&v, 0, sizeof(v));
	v.state = state;
	v.secs = 180;
	v.exec = "launchd";
	v.reason = 2;
	v.wdt_on = true;
	v.holders = PIANO_WDT_GUARD;
	v.pinging = true;
	v.now_ms = 95400;
	v.milestone_ms = 31200;
	v.pings = 10;
	v.max_gap_ms = 10012;
	v.cpu_check = true;
	v.stuck_cpu = -1;
	return v;
}

static void
test_format(void)
{
	char buf[384];
	struct piano_guard_view v = view(PIANO_GUARD_ARMED);
	size_t n = piano_guard_format(buf, sizeof(buf), &v);
	CHECK(n == strlen(buf), "the length");
	CHECK(strcmp(buf, "armed; restart reason bootloader; health at 180 s (now 95 s): exec of launchd at 31 s; watchdog on (32 s "
	    "bite), holders guard; 10 pings, longest gap 10.0 s\n") == 0, "armed: %s", buf);
	v = view(PIANO_GUARD_HEALTHY);
	v.reason = 0;
	v.decided_ms = 180000;
	v.holders = PIANO_WDT_GUARD | PIANO_WDT_SLEEP;
	v.pings = 19;
	piano_guard_format(buf, sizeof(buf), &v);
	CHECK(strcmp(buf, "healthy; restart reason normal; health at 180 s (decided at 180 s): exec of launchd at 31 s; watchdog on "
	    "(32 s bite), holders guard sleep; 19 pings, longest gap 10.0 s\n") == 0, "healthy: %s", buf);
	v = view(PIANO_GUARD_FAILED);
	v.secs = 60;
	v.decided_ms = 60000;
	v.milestone_ms = 0;
	v.exec = "piano-nosuch";
	piano_guard_format(buf, sizeof(buf), &v);
	CHECK(strstr(buf, "not healthy (reset into ABL fastboot); restart reason bootloader; health at 60 s (decided at 60 s): exec of "
	    "piano-nosuch not yet;") == buf, "failed: %s", buf);
	v = view(PIANO_GUARD_HEALTHY);
	v.keep_reason = true;
	v.exec = "";
	v.pinging = false;
	v.hang_at = 40;
	v.holders = 0;
	v.wdt_on = false;
	v.reason_err = -5;
	piano_guard_format(buf, sizeof(buf), &v);
	CHECK(strstr(buf, "restart reason bootloader (kept: piano-reset-to) (SDAM write error -5)") != NULL, "kept, error: %s", buf);
	CHECK(strstr(buf, ": no milestone; watchdog off, holders none;") != NULL, "no milestone, no holders: %s", buf);
	CHECK(strstr(buf, "; not pinging; test: every CPU stops at 40 s\n") != NULL, "test: %s", buf);
	v = view(PIANO_GUARD_HEALTHY);
	v.stuck_cpu = 3;
	v.pinging = false;
	v.cpu_check = false;
	piano_guard_format(buf, sizeof(buf), &v);
	CHECK(strstr(buf, "longest gap 10.0 s; CPU check off; CPU 3 took no interrupt; not pinging\n") != NULL, "stuck: %s", buf);
	v = view(PIANO_GUARD_ARMED);
	v.reason = -1;
	piano_guard_format(buf, sizeof(buf), &v);
	CHECK(strstr(buf, "restart reason not written;") != NULL, "never written: %s", buf);
	v = view(PIANO_GUARD_DEFERRED);
	v.reset_after = 45;
	piano_guard_format(buf, sizeof(buf), &v);
	CHECK(strcmp(buf, "deferred: piano-reset-after=45 owns the watchdog and the restart reason\n") == 0, "deferred: %s", buf);
	v = view(PIANO_GUARD_OFF);
	piano_guard_format(buf, sizeof(buf), &v);
	CHECK(strcmp(buf, "off\n") == 0, "off: %s", buf);
	v = view(PIANO_GUARD_NOT_NATIVE);
	piano_guard_format(buf, sizeof(buf), &v);
	CHECK(strncmp(buf, "off: native only", 16) == 0, "not native: %s", buf);
	/* A short buffer: truncated, terminated, the length within it. */
	for (size_t len = 1; len < 200; len += 7) {
		char small[200];
		memset(small, 'x', sizeof(small));
		v = view(PIANO_GUARD_HEALTHY);
		n = piano_guard_format(small, len, &v);
		CHECK(n < len && small[n] == '\0' && strlen(small) == n && small[len] == 'x', "len %zu: n %zu", len, n);
	}
	CHECK(piano_guard_format(buf, 0, &v) == 0, "len 0");
}

int
main(void)
{
	test_holders();
	test_decide();
	test_next();
	test_quiet();
	test_parse();
	test_comm();
	test_format();
	if (failures) {
		printf("test_piano_guard_policy: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_piano_guard_policy: all passed\n");
	return 0;
}
