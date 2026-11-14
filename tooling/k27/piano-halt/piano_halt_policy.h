/*
 * piano (k27 0069): the restart/power-off decisions that touch no hardware (osfmk/arm64/machine_routines.c), as plain inline C so
 * that they are host-tested (UniXNU-piano tooling/k27/piano-halt: this file byte for byte, and test_piano_halt_policy.c).
 * No kernel headers: the compiler's __atomic builtins, which the kernel's os_atomic is made of as well.
 */
#ifndef PIANO_HALT_POLICY_H
#define PIANO_HALT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* PEHaltRestart's types (IOKit/IOPlatformExpert.h's enum, in its order) that PE_halt_restart sees. IOPlatformExpert::haltRestart
 * answers kPEPanicSync and kPEHangCPU itself and turns kPEUPSDelayHaltCPU into kPEHaltCPU before it calls PE_halt_restart. */
#define PIANO_PE_HALT_CPU               0u
#define PIANO_PE_RESTART_CPU            1u
#define PIANO_PE_UPS_DELAY_HALT_CPU     3u

#define PIANO_PSCI_SYSTEM_OFF           0x84000008u
#define PIANO_PSCI_SYSTEM_RESET         0x84000009u

/* piano-psci-halt's PE_halt_restart(TYPE) with the switch ON: the PSCI function to call, 0 for none (then it answers -1, what no
 * PE_halt_restart answered: kPEPanicRestartCPU, kPEPagingOff and the rest keep their course; a panic restart then calls
 * halt_all_cpus(TRUE), whose PEHaltRestart(kPERestartCPU) gets SYSTEM_RESET here, the call halt_all_cpus would make next). */
static inline uint32_t
piano_halt_psci_function(unsigned int type, bool on)
{
	if (!on) {
		return 0;
	}
	if (type == PIANO_PE_RESTART_CPU) {
		return PIANO_PSCI_SYSTEM_RESET;
	}
	if (type == PIANO_PE_HALT_CPU || type == PIANO_PE_UPS_DELAY_HALT_CPU) {
		return PIANO_PSCI_SYSTEM_OFF;
	}
	return 0;
}

/* PEHaltRestart's types that begin an explicit restart or power-off (the ones its 30 s timer covers). */
static inline bool
piano_halt_type_begins(unsigned int type)
{
	return type == PIANO_PE_HALT_CPU || type == PIANO_PE_RESTART_CPU || type == PIANO_PE_UPS_DELAY_HALT_CPU;
}

/*
 * The restart word: STARTED once an explicit restart or power-off has begun (PEHaltRestart, PianoDwc3's console reset through
 * machdep.piano_restart_started), never cleared in a boot; WRITING while the boot guard writes the restart reason. The guard writes
 * only from 0 (piano_restart_guard_enter), so after STARTED it never writes again; a restarter that writes a reason of its own marks
 * first and waits while WRITING is set (piano_restart_guard_busy), so its write comes after any guard write already in flight and
 * no guard write comes after it.
 */
#define PIANO_RESTART_STARTED           0x1u
#define PIANO_RESTART_WRITING           0x2u

/* The guard, about to write the restart reason: true (WRITING now set) when no restart has begun, false: leave the reason alone. */
static inline bool
piano_restart_guard_enter(uint32_t *word)
{
	uint32_t expected = 0;
	return __atomic_compare_exchange_n(word, &expected, PIANO_RESTART_WRITING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* The guard's write is done. */
static inline void
piano_restart_guard_leave(uint32_t *word)
{
	(void)__atomic_fetch_and(word, ~PIANO_RESTART_WRITING, __ATOMIC_RELEASE);
}

/* A restart or power-off begins: the word before (STARTED in it: not the first; WRITING: the guard's write is in flight). */
static inline uint32_t
piano_restart_mark(uint32_t *word)
{
	return __atomic_fetch_or(word, PIANO_RESTART_STARTED, __ATOMIC_ACQ_REL);
}

static inline bool
piano_restart_guard_busy(const uint32_t *word)
{
	return (__atomic_load_n(word, __ATOMIC_ACQUIRE) & PIANO_RESTART_WRITING) != 0;
}

static inline bool
piano_restart_is_started(const uint32_t *word)
{
	return (__atomic_load_n(word, __ATOMIC_ACQUIRE) & PIANO_RESTART_STARTED) != 0;
}

#endif /* PIANO_HALT_POLICY_H */
