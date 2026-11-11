/*
 * piano (k27 0063): the boot guard's decisions that touch no hardware (osfmk/arm64/machine_routines.c), as plain inline C so
 * that they are host-tested (UniXNU-piano tooling/k27/piano-guard: this file byte for byte, and test_piano_guard_policy.c).
 * No kernel headers; the includer declares snprintf (the kernel's <kern/misc_protos.h>, the host's <stdio.h>).
 */
#ifndef PIANO_GUARD_POLICY_H
#define PIANO_GUARD_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The Gunyah watchdog's holders. machine_routines.c owns the watchdog: it is enabled while anyone holds it. */
#define PIANO_WDT_RESET_AFTER   0x1u    /* k27 0056, piano-reset-after: the whole boot */
#define PIANO_WDT_GUARD         0x2u    /* k27 0063, piano-boot-guard: the whole boot */
#define PIANO_WDT_SLEEP         0x4u    /* k27 0062, piano-sleep: from a suspend's entry to 240 s after its wake */

/* After a hold or a release (HOLDERS now, ENABLED the watchdog's state): 1 enable it (also a retry after a failed enable),
 * -1 disable it (the last holder left), 0 leave it. */
static inline int
piano_wdt_change(uint32_t holders, bool enabled)
{
	if (holders != 0 && !enabled) {
		return 1;
	}
	if (holders == 0 && enabled) {
		return -1;
	}
	return 0;
}

#define PIANO_GUARD_OFF         0   /* no piano-boot-guard */
#define PIANO_GUARD_DEFERRED    1   /* piano-reset-after (k27 0056) owns the watchdog and the restart reason */
#define PIANO_GUARD_NOT_NATIVE  2   /* the PSCI conduit is not SMC (a VM): nothing to guard */
#define PIANO_GUARD_ARMED       3   /* restart reason bootloader, the watchdog pinged, the health decision still due */
#define PIANO_GUARD_HEALTHY     4   /* restart reason normal again (unless kept), still pinged */
#define PIANO_GUARD_FAILED      5   /* not healthy at the decision: reset into ABL fastboot */

#define PIANO_GUARD_WAIT        0   /* nothing to decide: ping (when pinging) and tick again */
#define PIANO_GUARD_GO_HEALTHY  1
#define PIANO_GUARD_GO_FAILED   2

/* The health decision at uptime NOW: due at DEADLINE (piano-boot-guard's seconds), healthy when the milestone (the exec of
 * piano-boot-guard-exec, at uptime MILESTONE, 0 not seen) was reached or NEED says none is needed. One unit for all times. */
static inline int
piano_guard_decide(int state, uint64_t now, uint64_t deadline, uint64_t milestone, bool need)
{
	if (state != PIANO_GUARD_ARMED || now < deadline) {
		return PIANO_GUARD_WAIT;
	}
	return !need || milestone != 0 ? PIANO_GUARD_GO_HEALTHY : PIANO_GUARD_GO_FAILED;
}

/* The tick after one at NOW: PERIOD later, or at the deadline when the decision is still due and comes first. */
static inline uint64_t
piano_guard_next(int state, uint64_t now, uint64_t deadline, uint64_t period)
{
	uint64_t next = now + period;
	if (state == PIANO_GUARD_ARMED && deadline > now && deadline < next) {
		next = deadline;
	}
	return next;
}

#define PIANO_GUARD_QUIET_TICKS 2   /* a CPU that took no interrupt across this many pokes in a row is stuck */

/* One CPU at a tick: IRQS its interrupt count now (cpu_stat.irq_ex_cnt, the low 32 bits), *LAST the count at the last tick,
 * *QUIET the ticks in a row it stayed there although POKED (the last tick's nop IPI reached it); ELIGIBLE false for a CPU that
 * is not up (or the ticking CPU itself). Updates both; true when the CPU is stuck (an interrupt-masked hang, a bus stall). */
static inline bool
piano_guard_cpu_quiet(uint32_t irqs, uint32_t *last, uint8_t *quiet, bool poked, bool eligible)
{
	if (!eligible || !poked || irqs != *last) {
		*quiet = 0;
	} else if (*quiet < 255) {
		(*quiet)++;
	}
	*last = irqs;
	return *quiet >= PIANO_GUARD_QUIET_TICKS;
}

/* piano-boot-guard-test: "hang-at=N", every CPU stops at N s of uptime (1..86400): *HANG_AT = N and 0; anything else -1. */
static inline int
piano_guard_parse_test(const char *s, uint32_t *hang_at)
{
	static const char key[] = "hang-at=";
	for (size_t i = 0; i < sizeof(key) - 1; i++) {
		if (s[i] != key[i]) {
			return -1;
		}
	}
	s += sizeof(key) - 1;
	uint32_t n = 0;
	int digits = 0;
	for (; *s >= '0' && *s <= '9'; s++, digits++) {
		if (n > 86400) {
			return -1;
		}
		n = n * 10 + (uint32_t)(*s - '0');
	}
	if (*s != '\0' || digits == 0 || n == 0 || n > 86400) {
		return -1;
	}
	*hang_at = n;
	return 0;
}

#define PIANO_GUARD_COMLEN      16  /* MAXCOMLEN: an exec's p_comm keeps the first 16 characters of the file name */

/* Whether an exec's p_comm COMM is the milestone NAME (compared as p_comm keeps it: at most 16 characters). */
static inline bool
piano_guard_comm_is(const char *comm, const char *name)
{
	for (int i = 0; i < PIANO_GUARD_COMLEN; i++) {
		if (comm[i] != name[i]) {
			return false;
		}
		if (comm[i] == '\0') {
			return true;
		}
	}
	return true;
}

/* "none" (or an empty value) as piano-boot-guard-exec: no milestone, the decision is the uptime and a running pinger alone. */
static inline bool
piano_guard_exec_none(const char *name)
{
	return name[0] == '\0' || (name[0] == 'n' && name[1] == 'o' && name[2] == 'n' && name[3] == 'e' && name[4] == '\0');
}

static inline const char *
piano_guard_reason_name(int reason)
{
	return reason == 0 ? "normal" : reason == 2 ? "bootloader" : "not written";
}

/* What machdep.piano_boot_guard shows. Times are uptimes in ms (decided 0: not yet; milestone 0: not seen). */
struct piano_guard_view {
	int         state;              /* PIANO_GUARD_* */
	uint32_t    secs;               /* piano-boot-guard */
	uint32_t    reset_after;        /* piano-reset-after, when deferred */
	uint64_t    now_ms, decided_ms, milestone_ms;
	const char *exec;               /* the milestone process ("" none) */
	int         reason;             /* the restart reason as last written (2, 0), -1 never written */
	int         reason_err;         /* the last SDAM write's error, 0 none */
	bool        keep_reason;        /* piano-reset-to=bootloader: it stays bootloader */
	bool        wdt_on;             /* the watchdog is enabled */
	uint32_t    holders;            /* PIANO_WDT_* */
	bool        pinging;            /* the guard pings (not piano-boot-guard-ping=0, no test hang) */
	uint64_t    pings, max_gap_ms;  /* the guard's pings, the longest time between two of its ticks */
	bool        cpu_check;          /* piano-boot-guard-cpus (default 1): every CPU must keep taking interrupts */
	int         stuck_cpu;          /* the CPU found stuck, -1 none */
	uint32_t    hang_at;            /* piano-boot-guard-test=hang-at, 0 none */
};

/* One line, NUL-terminated; the length without the NUL (at most LEN - 1). */
static inline size_t
piano_guard_format(char *buf, size_t len, const struct piano_guard_view *v)
{
	size_t n = 0;
	if (len == 0) {
		return 0;
	}
	buf[0] = '\0';
#define PIANO_GUARD_PUT(...) do { if (n < len) { int k_ = snprintf(buf + n, len - n, __VA_ARGS__); if (k_ > 0) { n += (size_t)k_; } } } while (0)
	switch (v->state) {
	case PIANO_GUARD_OFF:
		PIANO_GUARD_PUT("off\n");
		break;
	case PIANO_GUARD_NOT_NATIVE:
		PIANO_GUARD_PUT("off: native only (the PSCI conduit is not SMC)\n");
		break;
	case PIANO_GUARD_DEFERRED:
		PIANO_GUARD_PUT("deferred: piano-reset-after=%u owns the watchdog and the restart reason\n", v->reset_after);
		break;
	default:
		PIANO_GUARD_PUT("%s; restart reason %s", v->state == PIANO_GUARD_HEALTHY ? "healthy" : v->state == PIANO_GUARD_ARMED ?
		    "armed" : "not healthy (reset into ABL fastboot)", piano_guard_reason_name(v->reason));
		if (v->keep_reason) {
			PIANO_GUARD_PUT(" (kept: piano-reset-to)");
		}
		if (v->reason_err != 0) {
			PIANO_GUARD_PUT(" (SDAM write error %d)", v->reason_err);
		}
		PIANO_GUARD_PUT("; health at %u s", v->secs);
		if (v->decided_ms != 0) {
			PIANO_GUARD_PUT(" (decided at %llu s)", (unsigned long long)(v->decided_ms / 1000));
		} else {
			PIANO_GUARD_PUT(" (now %llu s)", (unsigned long long)(v->now_ms / 1000));
		}
		if (v->exec[0] == '\0') {
			PIANO_GUARD_PUT(": no milestone");
		} else if (v->milestone_ms != 0) {
			PIANO_GUARD_PUT(": exec of %s at %llu s", v->exec, (unsigned long long)(v->milestone_ms / 1000));
		} else {
			PIANO_GUARD_PUT(": exec of %s not yet", v->exec);
		}
		PIANO_GUARD_PUT("; watchdog %s, holders", v->wdt_on ? "on (32 s bite)" : "off");
		if (v->holders == 0) {
			PIANO_GUARD_PUT(" none");
		}
		if (v->holders & PIANO_WDT_RESET_AFTER) {
			PIANO_GUARD_PUT(" reset-after");
		}
		if (v->holders & PIANO_WDT_GUARD) {
			PIANO_GUARD_PUT(" guard");
		}
		if (v->holders & PIANO_WDT_SLEEP) {
			PIANO_GUARD_PUT(" sleep");
		}
		PIANO_GUARD_PUT("; %llu pings, longest gap %llu.%llu s", (unsigned long long)v->pings,
		    (unsigned long long)(v->max_gap_ms / 1000), (unsigned long long)(v->max_gap_ms % 1000 / 100));
		if (!v->cpu_check) {
			PIANO_GUARD_PUT("; CPU check off");
		}
		if (v->stuck_cpu >= 0) {
			PIANO_GUARD_PUT("; CPU %d took no interrupt", v->stuck_cpu);
		}
		if (!v->pinging) {
			PIANO_GUARD_PUT("; not pinging");
		}
		if (v->hang_at != 0) {
			PIANO_GUARD_PUT("; test: every CPU stops at %u s", v->hang_at);
		}
		PIANO_GUARD_PUT("\n");
		break;
	}
#undef PIANO_GUARD_PUT
	return n < len ? n : len - 1;
}

#endif /* PIANO_GUARD_POLICY_H */
