// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of k27 0069's decisions (piano_halt_policy.h): which PSCI call PE_halt_restart makes for each PEHaltRestart type, and
// the restart word that keeps the boot guard from writing the restart reason once a restart has begun, single-threaded through
// every order of the steps and with two threads racing (the guard's health decision and PianoDwc3's "reset fastboot").
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "piano_halt_policy.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void
test_psci_function(void)
{
	/* IOKit/IOPlatformExpert.h: kPEHaltCPU 0, kPERestartCPU 1, kPEHangCPU 2, kPEUPSDelayHaltCPU 3, kPEPanicRestartCPU 4,
	 * kPEPanicSync 5, kPEPagingOff 6, kPEPanicBegin 7, kPEPanicEnd 8, kPEPanicRestartCPUNoCallouts 9,
	 * kPEPanicDiagnosticsDone 10, kPEPanicDiagnosticsInProgress 11 */
	CHECK(piano_halt_psci_function(1, true) == 0x84000009u);
	CHECK(piano_halt_psci_function(0, true) == 0x84000008u);
	CHECK(piano_halt_psci_function(3, true) == 0x84000008u);
	for (unsigned int t = 0; t < 16; t++) {
		CHECK(piano_halt_psci_function(t, false) == 0);              /* off: no handler behaviour at all */
		if (t != 0 && t != 1 && t != 3) {
			CHECK(piano_halt_psci_function(t, true) == 0);           /* the panic path's types and the rest: -1 as before */
		}
		CHECK(piano_halt_type_begins(t) == (t == 0 || t == 1 || t == 3));
	}
}

/* One boot's steps: G the guard's health decision (enter, write 0, leave), R a restart with a reason of its own (mark, wait while
 * the guard writes, write 2), H a restart without one (PEHaltRestart: mark only). */
struct sim {
	uint32_t word;
	int reason;
	int guard_writes;
};

static void
sim_guard(struct sim *s)
{
	if (piano_restart_guard_enter(&s->word)) {
		s->reason = 0;
		s->guard_writes++;
		piano_restart_guard_leave(&s->word);
	}
}

static void
sim_restart(struct sim *s, bool own_reason)
{
	(void)piano_restart_mark(&s->word);
	if (own_reason) {
		CHECK(!piano_restart_guard_busy(&s->word));   /* single-threaded: never mid-write */
		s->reason = 2;
	}
}

static void
test_sequences(void)
{
	/* the guard first, then the reset: the reset's reason stands */
	struct sim a = { .reason = 2 };
	sim_guard(&a);
	CHECK(a.reason == 0 && a.guard_writes == 1);
	sim_restart(&a, true);
	CHECK(a.reason == 2 && piano_restart_is_started(&a.word));
	sim_guard(&a);                                         /* a second decision (none happens, but never again) */
	CHECK(a.reason == 2 && a.guard_writes == 1);

	/* the reset first (the 20:39:50 case: reset at 170 s, the decision at 180 s during the 30 s hang): no guard write */
	struct sim b = { .reason = 2 };
	sim_restart(&b, true);
	sim_guard(&b);
	CHECK(b.reason == 2 && b.guard_writes == 0);

	/* shutdown -r / PEHaltRestart only: the reason is whatever it was, the guard leaves it */
	struct sim c = { .reason = 2 };
	sim_restart(&c, false);
	sim_guard(&c);
	CHECK(c.reason == 2 && c.guard_writes == 0);
	struct sim d = { .reason = 2 };
	sim_guard(&d);
	sim_restart(&d, false);
	CHECK(d.reason == 0 && d.guard_writes == 1);           /* healthy before the restart: normal, the installed system next */

	/* the word: marks are sticky, leave clears only WRITING */
	uint32_t w = 0;
	CHECK(piano_restart_guard_enter(&w) && w == PIANO_RESTART_WRITING);
	CHECK(!piano_restart_guard_enter(&w));                 /* one writer at a time */
	CHECK(piano_restart_mark(&w) == PIANO_RESTART_WRITING);
	CHECK(piano_restart_guard_busy(&w) && piano_restart_is_started(&w));
	piano_restart_guard_leave(&w);
	CHECK(w == PIANO_RESTART_STARTED && !piano_restart_guard_busy(&w));
	CHECK(piano_restart_mark(&w) == PIANO_RESTART_STARTED);
	CHECK(!piano_restart_guard_enter(&w));
}

/* Two threads, many rounds: the guard (enter, write 0 in two halves with a yield, leave) against the reset (mark, wait while busy,
 * write 2). Whatever the interleaving the last write must be the reset's. */
struct race {
	uint32_t word;
	volatile int reason;
	volatile int go;
};

static void *
race_guard(void *p)
{
	struct race *r = p;
	while (!__atomic_load_n(&r->go, __ATOMIC_ACQUIRE)) {
	}
	if (piano_restart_guard_enter(&r->word)) {
		int v = 0;
		sched_yield();
		r->reason = v;
		piano_restart_guard_leave(&r->word);
	}
	return NULL;
}

static void *
race_reset(void *p)
{
	struct race *r = p;
	while (!__atomic_load_n(&r->go, __ATOMIC_ACQUIRE)) {
	}
	(void)piano_restart_mark(&r->word);
	while (piano_restart_guard_busy(&r->word)) {
	}
	r->reason = 2;
	return NULL;
}

static void
test_race(int rounds)
{
	int wrong = 0;
	for (int i = 0; i < rounds; i++) {
		struct race r = { .reason = 2 };
		pthread_t g, x;
		if (pthread_create(&g, NULL, race_guard, &r) != 0 || pthread_create(&x, NULL, race_reset, &r) != 0) {
			fprintf(stderr, "FAIL: pthread_create\n");
			exit(1);
		}
		__atomic_store_n(&r.go, 1, __ATOMIC_RELEASE);
		pthread_join(g, NULL);
		pthread_join(x, NULL);
		if (r.reason != 2 || !piano_restart_is_started(&r.word) || piano_restart_guard_busy(&r.word)) {
			wrong++;
		}
	}
	CHECK(wrong == 0);
}

int
main(void)
{
	test_psci_function();
	test_sequences();
	test_race(20000);
	if (failures) {
		fprintf(stderr, "test_piano_halt_policy: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_piano_halt_policy: all checks passed (PSCI mapping, restart word sequences, 20000 guard/reset races)\n");
	return 0;
}
