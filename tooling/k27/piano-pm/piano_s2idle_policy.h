/*
 * piano (k27 0064): the decisions 0064 adds to osfmk/arm64/piano_pm.c that touch no hardware, as plain inline C so that they are
 * host-tested (UniXNU-piano tooling/k27/piano-pm: this file byte for byte, and test_piano_s2idle_policy.c). No kernel headers.
 *
 * 1. The LLCC ISLAND1 slice at each decision (piano-llcc-island-live=1). 0061 read its two words (TRP_ATTR0 0x26841300, ATTR1
 *    0x26841308, through the broadcast base) once at boot, before any kext, so a slice PianoA830 programs later stayed "NOT
 *    reserved" to the kernel. Live, the system state (ss3) is decided on a reading taken at that decision: the idle loop reads
 *    the two words only when every other condition of ss3 already holds (the last CPU of all, its deadline, the latency holds),
 *    and a suspend reads them at its entry for its level. A slice not reserved at the decision keeps ss3 out (cl5 instead).
 * 2. A driver's cancel of the system sleep in progress (piano-sleep-cancel=1). The window opens when IOPMrootDomain starts to
 *    take the drivers down (willNotifyPowerChildren, the sleep state) and is opened again, keeping what was asked, when
 *    IOCPUSleepKernel starts (before the platform sleep actions, the drivers' PreSleep); it shuts at the freeze, the last point
 *    where another thread can run. A cancel inside the window makes 0062's suspend return at once (before the park), or abort
 *    its park, exactly as a CPU that does not park does: nothing is suspended, the platform wake actions run, a normal wake.
 *    The first caller's name and reason are kept and logged; later callers are counted. Outside the window, or with the
 *    argument off, the request is refused and the sleep goes on as before.
 */
#ifndef PIANO_S2IDLE_POLICY_H
#define PIANO_S2IDLE_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "piano_pm_policy.h"

/* ---- 1. the LLCC island at each decision ---- */

/*
 * The idle choice of piano_pm_pick_domain with the island asked only at an ss3 decision: P's ss3_ok is set here (true, then
 * false when ISLAND(CTX) says the slice is not reserved now); ISLAND is called at most once, and only when the system state is
 * what would be chosen with the slice reserved.
 */
static inline int
piano_pm_pick_domain_island(struct piano_pm_pick *p, uint32_t *param, bool (*island)(void *ctx), void *ctx)
{
	p->ss3_ok = true;
	int level = piano_pm_pick_domain(p, param);
	if (level == 3 && !island(ctx)) {
		p->ss3_ok = false;
		level = piano_pm_pick_domain(p, param);
	}
	return level;
}

/* One reading's verdict for ss3: OVERRIDE (piano-llcc-island-ok=1) always; else the slice reserved (MAPPED: the window was). */
static inline bool
piano_llcc_ss3_verdict(bool override, bool mapped, uint32_t reserved_ways, uint32_t bonus_ways)
{
	return override || (mapped && piano_pm_llcc_island_ok(reserved_ways, bonus_ways));
}

/* What the console has said about the live verdict: SEEN the verdict of the last reading (-1 none), SAID the one last logged.
 * True (and SAID updated) when a line is due: the first reading, then each change. */
static inline bool
piano_llcc_live_due(int seen, int *said)
{
	if (seen < 0 || seen == *said) {
		return false;
	}
	*said = seen;
	return true;
}

/* ---- 2. a driver's cancel of the suspend in progress ---- */

#define PIANO_CANCEL_WHO        32
#define PIANO_CANCEL_WHY        96

#define PIANO_WINDOW_CLOSED     0   /* no system sleep in progress */
#define PIANO_WINDOW_OPEN       1   /* the drivers' kOff, the platform sleep actions, the park */
#define PIANO_WINDOW_SHUT       2   /* the freeze: nothing else runs any more */

/* A request's answer (piano_s2idle_cancel; IOPMrootDomain's "piano-sleep-cancel" maps them to IOReturns). */
#define PIANO_CANCEL_TAKEN      0   /* this suspend will not happen (asked now or by an earlier caller) */
#define PIANO_CANCEL_OFF        1   /* piano-sleep-cancel is not 1: the sleep goes on */
#define PIANO_CANCEL_NONE       2   /* no system sleep in progress */
#define PIANO_CANCEL_LATE       3   /* the CPUs are frozen */

struct piano_cancel {
	uint32_t window;                /* PIANO_WINDOW_* */
	uint32_t attempt;               /* the suspend the window is for (its number in machdep.piano_sleep) */
	bool     cancelled;
	uint32_t callers;               /* the requests taken in this window */
	char     who[PIANO_CANCEL_WHO]; /* the first caller's, printable, always terminated */
	char     why[PIANO_CANCEL_WHY];
	uint64_t total;                 /* suspends cancelled this boot */
	uint64_t refused;               /* requests refused this boot (off, none, late) */
};

/* A caller's string into DST (N > 0): at most N - 1 bytes, unprintable bytes as '?', NULL as "?". */
static inline void
piano_cancel_copy(char *dst, size_t n, const char *src)
{
	size_t i = 0;
	if (src == NULL) {
		src = "?";
	}
	for (; i + 1 < n && src[i] != '\0'; i++) {
		char ch = src[i];
		dst[i] = ch >= 0x20 && ch < 0x7f ? ch : '?';
	}
	dst[i] = '\0';
}

/* The window for suspend ATTEMPT. FRESH (IOPMrootDomain, the drivers about to go down): a new window, any earlier request
 * forgotten. Not FRESH (IOCPUSleepKernel, before PreSleep): opened only when not open yet, so a cancel asked in a driver's kOff
 * stays. */
static inline void
piano_cancel_open(struct piano_cancel *c, uint32_t attempt, bool fresh)
{
	if (!fresh && c->window == PIANO_WINDOW_OPEN) {
		return;
	}
	c->window = PIANO_WINDOW_OPEN;
	c->attempt = attempt;
	c->cancelled = false;
	c->callers = 0;
	c->who[0] = '\0';
	c->why[0] = '\0';
}

/* A driver's request. ENABLED: piano-sleep-cancel=1. */
static inline int
piano_cancel_request(struct piano_cancel *c, bool enabled, const char *who, const char *why)
{
	if (!enabled) {
		c->refused++;
		return PIANO_CANCEL_OFF;
	}
	if (c->window != PIANO_WINDOW_OPEN) {
		c->refused++;
		return c->window == PIANO_WINDOW_SHUT ? PIANO_CANCEL_LATE : PIANO_CANCEL_NONE;
	}
	c->callers++;
	if (!c->cancelled) {
		c->cancelled = true;
		piano_cancel_copy(c->who, sizeof(c->who), who);
		piano_cancel_copy(c->why, sizeof(c->why), why);
	}
	return PIANO_CANCEL_TAKEN;
}

/* Whether the suspend in progress is cancelled (the entry, the park's wait). */
static inline bool
piano_cancel_pending(const struct piano_cancel *c)
{
	return c->window == PIANO_WINDOW_OPEN && c->cancelled;
}

/* The freeze, or the suspend's return before it: the window shuts; true when the suspend is cancelled (counted once). */
static inline bool
piano_cancel_shut(struct piano_cancel *c)
{
	bool cancelled = piano_cancel_pending(c);
	c->window = PIANO_WINDOW_SHUT;
	if (cancelled) {
		c->total++;
	}
	return cancelled;
}

/* After the suspend (woken, aborted or cancelled) and at every system-up: closed until the next sleep. */
static inline void
piano_cancel_close(struct piano_cancel *c)
{
	c->window = PIANO_WINDOW_CLOSED;
}

/* A refusal's console line is due: the first 16 of the boot, then every 64th (a driver asking in a loop cannot flood it). */
static inline bool
piano_cancel_refusal_logged(uint64_t refused)
{
	return refused <= 16 || refused % 64 == 0;
}

/* piano-sleep-cancel-test=N: the kernel's own test caller cancels suspend N (1-based) of the boot, 0 never. */
static inline bool
piano_cancel_test_due(uint32_t test, uint32_t attempt)
{
	return test != 0 && attempt == test;
}

#endif /* PIANO_S2IDLE_POLICY_H */
