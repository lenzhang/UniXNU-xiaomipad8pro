// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of k27 0064's piano_s2idle_policy.h: the LLCC island asked only at an ss3 decision (and what it decides there), the
// live verdict's console lines, and a driver's cancel of the suspend in progress (its window across the drivers' kOff, PreSleep
// and the park, the first caller kept, the refusals, the freeze, the next sleep untouched, the test trigger).
// tooling/k27/test-piano-pm.sh builds and runs it and checks that the 0064 patch carries this header byte for byte.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "piano_s2idle_policy.h"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static uint64_t res_e[PIANO_PM_LEVELS];
static uint8_t none_disabled[PIANO_PM_LEVELS];

/* cpu0 completes everything: its cluster (0-5) and all eight CPUs idle, deadlines IDLE_US away. */
static struct piano_pm_pick
pick_all(int max, uint64_t idle_us)
{
	struct piano_pm_pick p;
	memset(&p, 0, sizeof(p));
	p.table = piano_pm_states_e;
	p.residency = res_e;
	p.disabled = none_disabled;
	p.max = max;
	p.now = 1000000;
	p.until = p.now + idle_us;
	p.latency_us = UINT32_MAX;
	p.cluster_mask = 0x3f;
	p.all_mask = 0xff;
	p.idle = 0xff;
	p.cluster_until = p.all_until = p.until;
	p.ss3_ok = false;               /* whatever the caller had: the helper decides it */
	return p;
}

struct island {
	int calls;
	bool reserved;
};

static bool
island_read(void *ctx)
{
	struct island *i = ctx;
	i->calls++;
	return i->reserved;
}

static void
test_island(void)
{
	uint32_t param = 0;
	struct island i = { 0, true };
	/* Every condition of ss3 holds: the island is read once and decides. */
	struct piano_pm_pick p = pick_all(3, 20000);
	CHECK(piano_pm_pick_domain_island(&p, &param, island_read, &i) == 3 && param == 0x0200c354 && i.calls == 1,
	    "reserved at the decision: ss3 (read once)");
	i = (struct island){ 0, false };
	p = pick_all(3, 20000);
	CHECK(piano_pm_pick_domain_island(&p, &param, island_read, &i) == 2 && param == 0x01000054 && i.calls == 1 && !p.ss3_ok,
	    "not reserved at the decision: cl5 (read once)");
	/* Anything else keeps ss3 out: the island is not read at all. */
	struct {
		const char *what;
		int max;
		uint64_t idle_us;
		uint32_t idle;
		uint32_t latency_us;
		int want;
	} cases[] = {
		{ "level 2", 2, 20000, 0xff, UINT32_MAX, 2 },
		{ "level 1", 1, 20000, 0xff, UINT32_MAX, 1 },
		{ "a CPU outside the cluster still runs", 3, 20000, 0x3f, UINT32_MAX, 2 },
		{ "a CPU of the cluster still runs", 3, 20000, 0x1f, UINT32_MAX, 1 },
		{ "the system's deadline below ss3's 10150 us", 3, 10149, 0xff, UINT32_MAX, 2 },
		{ "the cluster's deadline below cl5's 9144 us", 3, 9000, 0xff, UINT32_MAX, 1 },
		{ "PianoTouch's 2000 us hold (ss3's exit is 4400 us; cl5's 1983 fits)", 3, 20000, 0xff, 2000, 2 },
		{ "a 1900 us hold", 3, 20000, 0xff, 1900, 1 },
		{ "a 4000 us hold", 3, 20000, 0xff, 4000, 2 },
	};
	for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		i = (struct island){ 0, true };
		p = pick_all(cases[k].max, cases[k].idle_us);
		p.idle = cases[k].idle;
		p.latency_us = cases[k].latency_us;
		int got = piano_pm_pick_domain_island(&p, &param, island_read, &i);
		CHECK(got == cases[k].want && i.calls == 0, "%s: level %d (want %d), %d island reads (want 0)", cases[k].what, got,
		    cases[k].want, i.calls);
	}
	uint8_t ss3_off[PIANO_PM_LEVELS] = { 0, 0, 0, 1 };
	i = (struct island){ 0, true };
	p = pick_all(3, 20000);
	p.disabled = ss3_off;
	CHECK(piano_pm_pick_domain_island(&p, &param, island_read, &i) == 2 && i.calls == 0, "ss3 disabled (rejected 16 times): no read");
	/* The same choices as 0061's pick with ss3_ok given, for every level and deadline. */
	for (int max = 0; max <= 3; max++) {
		for (uint64_t us = 0; us <= 30000; us += 250) {
			for (int reserved = 0; reserved < 2; reserved++) {
				struct piano_pm_pick a = pick_all(max, us), b = pick_all(max, us);
				uint32_t pa = 0, pb = 0;
				b.ss3_ok = reserved;
				i = (struct island){ 0, reserved };
				int la = piano_pm_pick_domain_island(&a, &pa, island_read, &i);
				int lb = piano_pm_pick_domain(&b, &pb);
				CHECK(la == lb && pa == pb && i.calls <= 1 && (i.calls == 1) == (la == 3 || (!reserved && lb == 2 && max == 3 &&
				    piano_pm_fits(a.now, a.all_until, a.residency[3]))), "max %d, %llu us, reserved %d: %d/%d, %d reads", max,
				    (unsigned long long)us, reserved, la, lb, i.calls);
			}
		}
	}
	/* The verdict of one reading. */
	CHECK(piano_llcc_ss3_verdict(false, true, 0x7fffffff, 0), "reserved ways, no bonus ways");
	CHECK(!piano_llcc_ss3_verdict(false, true, 0x7fffffff, 0x7fffffff), "bonus ways too (the upstream table): not");
	CHECK(!piano_llcc_ss3_verdict(false, true, 0, 0x7fffffff), "only bonus ways: not");
	CHECK(!piano_llcc_ss3_verdict(false, false, 0x7fffffff, 0), "the window not mapped: not");
	CHECK(!piano_llcc_ss3_verdict(false, true, 0xffffffff, 0xffffffff), "all ones (not accessible): not");
	CHECK(piano_llcc_ss3_verdict(true, false, 0, 0), "piano-llcc-island-ok=1: always");
	/* The console: the first reading, then each change only. */
	int said = -1;
	CHECK(!piano_llcc_live_due(-1, &said) && said == -1, "no reading yet: no line");
	CHECK(piano_llcc_live_due(0, &said) && said == 0, "the first reading: a line");
	CHECK(!piano_llcc_live_due(0, &said), "the same verdict: none");
	CHECK(piano_llcc_live_due(1, &said) && said == 1, "PianoA830 programmed it: a line");
	CHECK(!piano_llcc_live_due(1, &said), "again reserved: none");
	CHECK(piano_llcc_live_due(0, &said) && said == 0, "lost again: a line");
}

static void
test_cancel(void)
{
	struct piano_cancel c;
	memset(&c, 0, sizeof(c));
	/* Awake, no sleep: refused, counted. */
	CHECK(piano_cancel_request(&c, true, "PianoX", "busy") == PIANO_CANCEL_NONE && c.refused == 1, "no sleep in progress: none");
	CHECK(!piano_cancel_pending(&c), "nothing pending");
	/* The argument off: refused whatever the window. */
	piano_cancel_open(&c, 1, true);
	CHECK(piano_cancel_request(&c, false, "PianoX", "busy") == PIANO_CANCEL_OFF && !piano_cancel_pending(&c) && c.refused == 2,
	    "piano-sleep-cancel=0: off, the sleep goes on");
	CHECK(!piano_cancel_shut(&c) && c.total == 0, "not cancelled at the freeze");
	piano_cancel_close(&c);
	/* A cancel in a driver's kOff survives IOCPUSleepKernel's re-open and is the one kept. */
	piano_cancel_open(&c, 2, true);
	CHECK(piano_cancel_request(&c, true, "NativeUfs", "the link did not enter Hibern8") == PIANO_CANCEL_TAKEN, "kOff: taken");
	piano_cancel_open(&c, 2, false);
	CHECK(piano_cancel_pending(&c) && !strcmp(c.who, "NativeUfs") && !strcmp(c.why, "the link did not enter Hibern8"),
	    "PreSleep's re-open keeps it");
	CHECK(piano_cancel_request(&c, true, "PianoA830", "busy") == PIANO_CANCEL_TAKEN && c.callers == 2 && !strcmp(c.who, "NativeUfs"),
	    "a second caller: taken, the first one's reason kept");
	CHECK(piano_cancel_shut(&c) && c.total == 1, "the freeze: cancelled, counted once");
	CHECK(!piano_cancel_pending(&c), "shut: no longer pending");
	CHECK(piano_cancel_request(&c, true, "late", "x") == PIANO_CANCEL_LATE, "after the freeze: late");
	piano_cancel_close(&c);
	CHECK(piano_cancel_request(&c, true, "after", "x") == PIANO_CANCEL_NONE, "after the wake: none");
	/* The next sleep starts clean: nothing of the last one, no stale cancel. */
	piano_cancel_open(&c, 3, true);
	CHECK(!piano_cancel_pending(&c) && c.callers == 0 && c.who[0] == '\0' && c.attempt == 3, "the next sleep: a fresh window");
	CHECK(!piano_cancel_shut(&c) && c.total == 1, "not cancelled");
	piano_cancel_close(&c);
	/* A request between a sleep that never reached the kernel and the next one: forgotten at the next fresh open. */
	piano_cancel_open(&c, 4, true);
	CHECK(piano_cancel_request(&c, true, "stale", "x") == PIANO_CANCEL_TAKEN, "taken in an open window");
	piano_cancel_open(&c, 4, true);
	CHECK(!piano_cancel_pending(&c), "the next sleep's fresh open forgets it");
	/* PreSleep without the root domain's open (a path that skipped it): its own window. */
	piano_cancel_close(&c);
	piano_cancel_open(&c, 5, false);
	CHECK(c.window == PIANO_WINDOW_OPEN && c.attempt == 5 && piano_cancel_request(&c, true, "PianoA830", "x") == PIANO_CANCEL_TAKEN,
	    "PreSleep opens a closed window");
	CHECK(piano_cancel_shut(&c), "cancelled");
	/* The strings: bounded, printable, terminated. */
	char who[PIANO_CANCEL_WHO];
	memset(who, 'x', sizeof(who));
	piano_cancel_copy(who, sizeof(who), "a-very-long-driver-name-that-does-not-fit-in-32-bytes");
	CHECK(strlen(who) == PIANO_CANCEL_WHO - 1, "cut to %d bytes", PIANO_CANCEL_WHO - 1);
	piano_cancel_copy(who, sizeof(who), "tab\there\nnl");
	CHECK(!strcmp(who, "tab?here?nl"), "unprintable bytes as ?: %s", who);
	piano_cancel_copy(who, sizeof(who), NULL);
	CHECK(!strcmp(who, "?"), "NULL as ?");
	piano_cancel_copy(who, 1, "abc");
	CHECK(who[0] == '\0', "a 1-byte buffer: empty");
	/* Refusal lines: the first 16, then every 64th. */
	int lines = 0;
	for (uint64_t n = 1; n <= 1024; n++) {
		lines += piano_cancel_refusal_logged(n);
	}
	CHECK(lines == 16 + 1024 / 64 - 0 && piano_cancel_refusal_logged(64) && !piano_cancel_refusal_logged(65), "%d lines for 1024", lines);
	/* The test trigger. */
	CHECK(!piano_cancel_test_due(0, 1) && piano_cancel_test_due(2, 2) && !piano_cancel_test_due(2, 1) && !piano_cancel_test_due(2, 3),
	    "piano-sleep-cancel-test=2: suspend 2 only");
}

int
main(void)
{
	for (int l = 0; l < PIANO_PM_LEVELS; l++) {
		res_e[l] = piano_pm_states_e[l].residency_us;
	}
	test_island();
	test_cancel();
	if (failures) {
		printf("test_piano_s2idle_policy: %d of %d checks FAILED\n", failures, checks);
		return 1;
	}
	printf("test_piano_s2idle_policy: all %d checks passed\n", checks);
	return 0;
}
