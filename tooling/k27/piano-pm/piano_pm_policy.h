/*
 * piano (k27 0061, 0062): the decisions of osfmk/arm64/piano_pm.c that touch no hardware, as plain inline C so that they are
 * host-tested (UniXNU-piano tooling/k27/piano-pm: this file byte for byte, and test_piano_pm_policy.c). No kernel headers.
 */
#ifndef PIANO_PM_POLICY_H
#define PIANO_PM_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define PIANO_PM_LEVELS         4   /* 0 WFI, 1 ret (CPU), 2 cl5 (cluster), 3 ss3 (CPU subsystem) */

struct piano_pm_state {
	const char *name;
	uint32_t    param;              /* PSCI power_state, extended StateID format */
	uint32_t    entry_us, exit_us, residency_us;
};

/* piano's states (its Linux DT, idle-states and domain-idle-states): cores 0-5 (E) and cores 6-7 (P). */
static const struct piano_pm_state piano_pm_states_e[PIANO_PM_LEVELS] = {
	{ "wfi", 0, 0, 0, 0 },
	{ "ret", 0x00000004, 93, 129, 560 },
	{ "cl5", 0x01000054, 2150, 1983, 9144 },
	{ "ss3", 0x0200c354, 2800, 4400, 10150 },
};
static const struct piano_pm_state piano_pm_states_p[PIANO_PM_LEVELS] = {
	{ "wfi", 0, 0, 0, 0 },
	{ "ret", 0x00000004, 172, 130, 686 },
	{ "cl5", 0x01000054, 2150, 1983, 9144 },
	{ "ss3", 0x0200c354, 2800, 4400, 10150 },
};

#define PIANO_PM_POWER_DOWN     (1u << 30)  /* StateType in the extended format */

/* The inputs of one idle choice; times in one unit (the caller's), RESIDENCY per level in it. */
struct piano_pm_pick {
	const struct piano_pm_state *table;
	const uint64_t *residency;
	const uint8_t *disabled;
	int      max;                   /* the deepest level allowed */
	uint64_t now, until;            /* now, and this CPU's next deadline */
	uint32_t latency_us;            /* the tightest exit-latency hold (UINT32_MAX: none) */
	uint32_t idle;                  /* the CPUs in a PSCI state, this one included */
	uint32_t cluster_mask, all_mask;
	uint64_t cluster_until, all_until;   /* the earliest deadline of the cluster's CPUs, of all CPUs (when they are all idle) */
	bool     ss3_ok;                /* the LLCC island is reserved (or overridden) */
};

static inline bool
piano_pm_fits(uint64_t now, uint64_t until, uint64_t residency)
{
	return until > now && until - now >= residency;
}

/* Whether the CPU state (level 1) is worth entering: 1, else 0 (WFI). */
static inline int
piano_pm_pick_cpu(const struct piano_pm_pick *p)
{
	return p->max >= 1 && !p->disabled[1] && piano_pm_fits(p->now, p->until, p->residency[1]) &&
	       p->table[1].exit_us <= p->latency_us ? 1 : 0;
}

/* With the CPU state chosen: the deepest domain level (1..3) and its param, as Linux's OSI domain idle sends it (the domain's
 * param in place of the CPU's): a cluster's only when this CPU completes it, the system's only when all CPUs are idle. */
static inline int
piano_pm_pick_domain(const struct piano_pm_pick *p, uint32_t *param)
{
	*param = p->table[1].param;
	if (p->max < 2 || (p->idle & p->cluster_mask) != p->cluster_mask || p->disabled[2] || p->table[2].exit_us > p->latency_us ||
	    !piano_pm_fits(p->now, p->cluster_until, p->residency[2])) {
		return 1;
	}
	*param = p->table[2].param;
	if (p->max < 3 || !p->ss3_ok || (p->idle & p->all_mask) != p->all_mask || p->disabled[3] ||
	    p->table[3].exit_us > p->latency_us || !piano_pm_fits(p->now, p->all_until, p->residency[3])) {
		return 2;
	}
	*param = p->table[3].param;
	return 3;
}

/* PSCI_FEATURES(CPU_SUSPEND): bit 1 extended StateID format, bit 0 OS-initiated mode. */
static inline bool
piano_pm_osi_supported(int64_t features)
{
	return features >= 0 && (features & 1) != 0;
}

/* The LLCC ISLAND1 slice (SCID 12) as CX collapse needs it: reserved ways 0x7fffffff, no bonus ways. */
static inline bool
piano_pm_llcc_island_ok(uint32_t reserved_ways, uint32_t bonus_ways)
{
	return reserved_ways == 0x7fffffffu && bonus_ways == 0;
}

/* ---- k27 0062: the wake sources ---- */

#define PIANO_SRC_POWER         0x01
#define PIANO_SRC_LID           0x02
#define PIANO_SRC_USB           0x04
#define PIANO_SRC_VOLUME_DOWN   0x08
#define PIANO_SRC_KEYBOARD      0x10

#define PIANO_PON_KPDPWR        0x80    /* PMK8550 PON INT_RT_STS: the power key */
#define PIANO_PON_RESIN         0x40    /* volume-down */
#define PIANO_TLMM_RAW_STATUS   0x10    /* INTR_CFG: edges latched into INTR_STATUS */

/* What each source read before the suspend. */
struct piano_pm_wake {
	uint32_t src;                   /* the sources armed */
	bool     power_down, volume_down, usb_attached;
	uint32_t open_samples;
};

/* Arm the wanted sources from their first readings (an *_ok false: that source did not read). The cover wakes by opening only
 * (armed when it reads closed: the hall line low); the keyboard's status line only with the cover open and its owner latching
 * rising edges (raw status) with nothing latched yet; USB on any attach change. */
static inline void
piano_pm_wake_arm(struct piano_pm_wake *b, uint32_t wanted, bool pon_ok, uint8_t pon, bool hall_ok, uint32_t hall_io,
    bool kbd_ok, uint32_t kbd_cfg, uint32_t kbd_status, bool usb_ok, uint8_t typec_misc)
{
	b->src = wanted;
	b->open_samples = 0;
	b->power_down = pon_ok && (pon & PIANO_PON_KPDPWR) != 0;
	b->volume_down = pon_ok && (pon & PIANO_PON_RESIN) != 0;
	b->usb_attached = usb_ok && (typec_misc & 1) != 0;
	if (!pon_ok) {
		b->src &= ~(uint32_t)(PIANO_SRC_POWER | PIANO_SRC_VOLUME_DOWN);
	}
	bool closed = hall_ok && (hall_io & 1) == 0;
	if (!closed) {
		b->src &= ~(uint32_t)PIANO_SRC_LID;
	}
	if (!hall_ok || closed || !kbd_ok || (kbd_cfg & PIANO_TLMM_RAW_STATUS) == 0 || (kbd_status & 1) != 0) {
		b->src &= ~(uint32_t)PIANO_SRC_KEYBOARD;
	}
	if (!usb_ok) {
		b->src &= ~(uint32_t)PIANO_SRC_USB;
	}
}

#define PIANO_PM_WAKE_POWER_KEY    2
#define PIANO_PM_WAKE_LID          3
#define PIANO_PM_WAKE_USB          4
#define PIANO_PM_WAKE_VOLUME_DOWN  5
#define PIANO_PM_WAKE_KEYBOARD     8

/* One poll's readings of the armed sources (the others are not read and their values ignored): a wake reason, else 0. The power
 * key and volume-down wake on their press (released -> pressed), the cover after two open readings in a row. */
static inline int
piano_pm_wake_check(struct piano_pm_wake *b, bool pon_ok, uint8_t pon, uint32_t hall_io, uint32_t kbd_status, bool usb_ok,
    uint8_t typec_misc)
{
	if ((b->src & (PIANO_SRC_POWER | PIANO_SRC_VOLUME_DOWN)) && pon_ok) {
		bool power = (pon & PIANO_PON_KPDPWR) != 0, volume = (pon & PIANO_PON_RESIN) != 0;
		int r = 0;
		if ((b->src & PIANO_SRC_POWER) && power && !b->power_down) {
			r = PIANO_PM_WAKE_POWER_KEY;
		} else if ((b->src & PIANO_SRC_VOLUME_DOWN) && volume && !b->volume_down) {
			r = PIANO_PM_WAKE_VOLUME_DOWN;
		}
		b->power_down = power;
		b->volume_down = volume;
		if (r != 0) {
			return r;
		}
	}
	if (b->src & PIANO_SRC_LID) {
		if (hall_io & 1) {
			if (++b->open_samples >= 2) {
				return PIANO_PM_WAKE_LID;
			}
		} else {
			b->open_samples = 0;
		}
	}
	if ((b->src & PIANO_SRC_KEYBOARD) && (kbd_status & 1)) {
		return PIANO_PM_WAKE_KEYBOARD;
	}
	if ((b->src & PIANO_SRC_USB) && usb_ok && ((typec_misc & 1) != 0) != b->usb_attached) {
		return PIANO_PM_WAKE_USB;
	}
	return 0;
}

/* The charger PMIC (SID 7): a charger block is TYPE 0x02 with SUBTYPE 0x7f, 0xa8 or 0xba; its Type-C block is 0x600 above it,
 * 0x900 for SUBTYPE 0xba..0xc4 (UniXNU-piano package/PianoPower/ChargerPmic.h). */
static inline bool
piano_pm_charger_kind(uint8_t type, uint8_t subtype)
{
	return type == 0x02 && (subtype == 0x7f || subtype == 0xa8 || subtype == 0xba);
}

static inline uint16_t
piano_pm_typec_address(uint16_t charger, uint8_t subtype)
{
	return (uint16_t)(charger + (subtype >= 0xba && subtype <= 0xc4 ? 0x300 : 0) + 0x600);
}

/* The wall time at wake: the calendar (which did not count the suspend) plus the time suspended. */
static inline void
piano_pm_add_slept(unsigned long secs, unsigned int usecs, uint64_t slept_ns, unsigned long *out_secs, unsigned int *out_usecs)
{
	uint64_t total_us = (uint64_t)usecs + (slept_ns / 1000) % 1000000;
	*out_secs = secs + (unsigned long)(slept_ns / 1000000000ULL) + (unsigned long)(total_us / 1000000);
	*out_usecs = (unsigned int)(total_us % 1000000);
}

#endif /* PIANO_PM_POLICY_H */
